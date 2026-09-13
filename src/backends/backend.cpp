// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "tos/backends/backend.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>

#include "domain_factory.hpp"
#include "tos/util/crc32c.hpp"

namespace tos {

std::vector<OperationClassId> host_executor_operations() {
  return detail::host_executor_operations();
}

CapabilitySet make_host_executor_capability(std::uint64_t max_payload_bytes,
                                            std::uint32_t queue_capacity,
                                            std::uint32_t max_concurrency,
                                            std::string backend_family,
                                            std::uint32_t memory_domain_mask,
                                            std::uint32_t driver_backend_version) {
  return detail::make_host_executor_capability(max_payload_bytes, queue_capacity, max_concurrency,
                                               std::move(backend_family), memory_domain_mask,
                                               driver_backend_version);
}

std::uint32_t memory_domain_mask_for(ExecutionDomainType type) {
  const std::uint32_t host = memory_bit(MemoryDomain::kHost) | memory_bit(MemoryDomain::kPinnedHost);
  switch (type) {
    case ExecutionDomainType::kCpu: return host;
    case ExecutionDomainType::kAccelerator:
      return host | memory_bit(MemoryDomain::kDeviceLocal) | memory_bit(MemoryDomain::kPeerDevice);
    case ExecutionDomainType::kNic: return host | memory_bit(MemoryDomain::kNicOnboard);
    case ExecutionDomainType::kSmartNic:
      return host | memory_bit(MemoryDomain::kSmartNicOnboard);
    case ExecutionDomainType::kDpu: return host | memory_bit(MemoryDomain::kDpuOnboard);
    case ExecutionDomainType::kOtherRegisteredOffloadEngine: return host;
  }
  return host;
}

namespace {

std::uint64_t digest_of(const std::vector<std::uint8_t>& bytes) {
  return fnv1a64(bytes.data(), bytes.size());
}

ExecutionOutcome unsupported(std::string_view name) {
  return ExecutionOutcome::failed(FailureKind::kBackendRejection,
                                  std::string("host executor does not implement ") +
                                      std::string(name));
}

/// Run-length encode: [u8 flag][u32 count][bytes]. Deterministic and exactly
/// invertible by DECOMPRESS_RLE. flag 1 = repeat run, flag 0 = literal run.
std::vector<std::uint8_t> rle_encode(const std::vector<std::uint8_t>& input) {
  std::vector<std::uint8_t> out;
  std::size_t index = 0;
  while (index < input.size()) {
    std::size_t run = 1;
    while (index + run < input.size() && input[index + run] == input[index]) ++run;
    if (run >= 4) {
      out.push_back(1);
      const std::uint32_t count = static_cast<std::uint32_t>(run);
      for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>((count >> (8 * i)) & 0xFFU));
      }
      out.push_back(input[index]);
      index += run;
      continue;
    }
    const std::size_t literal_start = index;
    std::size_t literal_length = run;
    while (literal_start + literal_length < input.size()) {
      const std::size_t probe = literal_start + literal_length;
      std::size_t next_run = 1;
      while (probe + next_run < input.size() && input[probe + next_run] == input[probe]) ++next_run;
      if (next_run >= 4) break;
      literal_length += next_run;
    }
    out.push_back(0);
    const std::uint32_t count = static_cast<std::uint32_t>(literal_length);
    for (int i = 0; i < 4; ++i) {
      out.push_back(static_cast<std::uint8_t>((count >> (8 * i)) & 0xFFU));
    }
    out.insert(out.end(), input.begin() + static_cast<std::ptrdiff_t>(literal_start),
               input.begin() + static_cast<std::ptrdiff_t>(literal_start + literal_length));
    index = literal_start + literal_length;
  }
  return out;
}

bool rle_decode(const std::vector<std::uint8_t>& input, std::vector<std::uint8_t>& out) {
  out.clear();
  std::size_t index = 0;
  while (index < input.size()) {
    if (index + 5 > input.size()) return false;
    const std::uint8_t flag = input[index];
    std::uint32_t count = 0;
    for (int i = 0; i < 4; ++i) {
      count |= static_cast<std::uint32_t>(input[index + 1 + static_cast<std::size_t>(i)]) << (8 * i);
    }
    index += 5;
    if (flag == 1) {
      if (index >= input.size()) return false;
      out.insert(out.end(), count, input[index]);
      ++index;
    } else if (flag == 0) {
      if (index + count > input.size()) return false;
      out.insert(out.end(), input.begin() + static_cast<std::ptrdiff_t>(index),
                 input.begin() + static_cast<std::ptrdiff_t>(index + count));
      index += count;
    } else {
      return false;
    }
    if (out.size() > kMaxResultPayloadBytes) return false;
  }
  return true;
}

}  // namespace

Status IExecutionBackend::cancel(ExecutionAttemptId id) {
  (void)id;
  return Status::failure("cancel.unsupported", "backend does not support cancellation");
}

Status IExecutionBackend::shutdown() { return Status::success(); }

ExecutionOutcome execute_operation_on_host(const ExecutionInvocation& invocation,
                                           const OperationClassRegistry& registry) {
  const OperationClassDescriptor* descriptor = registry.find(invocation.operation_class);
  const OperationClassId operation = invocation.operation_class;
  const std::vector<std::uint8_t>& input = invocation.input;

  ExecutionOutcome outcome;
  if (descriptor == nullptr) {
    return unsupported("an unregistered operation class");
  }

  if (operation == opclass::checksum_crc32c()) {
    const std::uint32_t crc = crc32c(input.data(), input.size());
    outcome = ExecutionOutcome::ok(static_cast<std::uint64_t>(crc), input.size(), 0);
    outcome.output.resize(4);
    for (int i = 0; i < 4; ++i) {
      outcome.output[static_cast<std::size_t>(i)] =
          static_cast<std::uint8_t>((crc >> (8 * i)) & 0xFFU);
    }
    return outcome;
  }
  if (operation == opclass::compress_rle()) {
    std::vector<std::uint8_t> encoded = rle_encode(input);
    if (encoded.size() > kMaxResultPayloadBytes) {
      return ExecutionOutcome::failed(FailureKind::kBackendRejection, "compressed output too large");
    }
    outcome = ExecutionOutcome::ok(digest_of(encoded), input.size(), 0);
    outcome.output = std::move(encoded);
    return outcome;
  }
  if (operation == opclass::decompress_rle()) {
    std::vector<std::uint8_t> decoded;
    if (!rle_decode(input, decoded)) {
      return ExecutionOutcome::failed(FailureKind::kBackendRejection, "malformed compressed input");
    }
    outcome = ExecutionOutcome::ok(digest_of(decoded), decoded.size(), 0);
    outcome.output = std::move(decoded);
    return outcome;
  }
  if (operation == opclass::copy_stage()) {
    std::vector<std::uint8_t> staged = input;
    outcome = ExecutionOutcome::ok(digest_of(staged), staged.size(), 0);
    outcome.output = std::move(staged);
    return outcome;
  }
  if (operation == opclass::segment_split()) {
    std::size_t segment_size =
        invocation.payload.segment_count > 1 ? input.size() / invocation.payload.segment_count : 0;
    if (segment_size == 0) segment_size = 64;
    std::vector<std::uint8_t> framed;
    std::size_t index = 0;
    while (index < input.size()) {
      const std::size_t length = std::min(segment_size, input.size() - index);
      const std::uint32_t count = static_cast<std::uint32_t>(length);
      for (int i = 0; i < 4; ++i) {
        framed.push_back(static_cast<std::uint8_t>((count >> (8 * i)) & 0xFFU));
      }
      framed.insert(framed.end(), input.begin() + static_cast<std::ptrdiff_t>(index),
                    input.begin() + static_cast<std::ptrdiff_t>(index + length));
      index += length;
      if (framed.size() > kMaxResultPayloadBytes) {
        return ExecutionOutcome::failed(FailureKind::kBackendRejection, "framed output too large");
      }
    }
    outcome = ExecutionOutcome::ok(digest_of(framed), input.size(), 0);
    outcome.output = std::move(framed);
    return outcome;
  }
  if (operation == opclass::reassemble_join()) {
    std::vector<std::uint8_t> joined;
    std::size_t index = 0;
    while (index < input.size()) {
      if (index + 4 > input.size()) {
        return ExecutionOutcome::failed(FailureKind::kBackendRejection, "truncated frame header");
      }
      std::uint32_t length = 0;
      for (int i = 0; i < 4; ++i) {
        length |= static_cast<std::uint32_t>(input[index + static_cast<std::size_t>(i)]) << (8 * i);
      }
      index += 4;
      if (index + length > input.size()) {
        return ExecutionOutcome::failed(FailureKind::kBackendRejection, "truncated frame body");
      }
      joined.insert(joined.end(), input.begin() + static_cast<std::ptrdiff_t>(index),
                    input.begin() + static_cast<std::ptrdiff_t>(index + length));
      index += length;
      if (joined.size() > kMaxResultPayloadBytes) {
        return ExecutionOutcome::failed(FailureKind::kBackendRejection, "joined output too large");
      }
    }
    outcome = ExecutionOutcome::ok(digest_of(joined), joined.size(), 0);
    outcome.output = std::move(joined);
    return outcome;
  }
  if (operation == opclass::validate_integrity()) {
    if (input.size() < 8) {
      return ExecutionOutcome::failed(FailureKind::kBackendRejection, "payload shorter than digest");
    }
    std::uint64_t expected = 0;
    for (int i = 0; i < 8; ++i) {
      expected |= static_cast<std::uint64_t>(input[static_cast<std::size_t>(i)]) << (8 * i);
    }
    const std::uint64_t actual = fnv1a64(input.data() + 8, input.size() - 8);
    if (expected != actual) {
      ExecutionOutcome failure =
          ExecutionOutcome::failed(FailureKind::kIntegrityFailure, "integrity digest mismatch");
      failure.executed = true;
      failure.result_digest = actual;
      failure.bytes_processed = input.size() - 8;
      return failure;
    }
    return ExecutionOutcome::ok(actual, input.size() - 8, 0);
  }
  if (operation == opclass::side_effect_emit()) {
    // An append-only sink: repeating the operation appends a second record, which
    // is exactly why its side-effect class is NON_REPEATABLE.
    static std::atomic<std::uint64_t> sink_counter{0};
    const std::uint64_t record = sink_counter.fetch_add(1, std::memory_order_relaxed) + 1;
    outcome = ExecutionOutcome::ok(record, input.size(), 0);
    outcome.output.resize(8);
    for (int i = 0; i < 8; ++i) {
      outcome.output[static_cast<std::size_t>(i)] =
          static_cast<std::uint8_t>((record >> (8 * i)) & 0xFFU);
    }
    outcome.detail = "appended record " + std::to_string(record);
    return outcome;
  }
  if (operation == opclass::noop_probe()) {
    return ExecutionOutcome::ok(input.size(), input.size(), 0);
  }
  return unsupported(descriptor->name);
}

}  // namespace tos
