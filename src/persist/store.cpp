// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "tos/persist/store.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#include "codec.hpp"
#include "tos/util/bytes.hpp"
#include "tos/util/crc32c.hpp"
#include "tos/version.hpp"

namespace tos {
namespace {

constexpr std::array<std::uint8_t, 4> kMagic = {'T', 'O', 'S', 'P'};
constexpr std::size_t kHeaderBytes = 16;

/// Bounded, traversal-free persistence path check. Absolute paths are legitimate:
/// the caller chose the location. Traversal components and control characters are not.
bool is_safe_state_path(std::string_view path) noexcept {
  if (path.empty() || path.size() > 512) return false;
  for (char c : path) {
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) return false;
  }
  return !path_has_traversal(path);
}

}  // namespace

std::vector<std::uint8_t> StateStore::encode(const PersistedState& state) {
  std::vector<ExecutionDomainRecord> domains = state.domains;
  std::sort(domains.begin(), domains.end(),
            [](const ExecutionDomainRecord& a, const ExecutionDomainRecord& b) { return a.id < b.id; });
  std::vector<ExecutionAttempt> attempts = state.attempts;
  std::sort(attempts.begin(), attempts.end(),
            [](const ExecutionAttempt& a, const ExecutionAttempt& b) { return a.id < b.id; });
  std::vector<WorkerBootId> boots = state.fenced_boots;
  std::sort(boots.begin(), boots.end());
  boots.erase(std::unique(boots.begin(), boots.end()), boots.end());

  ByteWriter payload(kMaxPersistedBytes);
  payload.u64(state.generation.value());
  payload.u64(state.last_epoch.value());
  payload.u64(state.policy_generation.value());
  payload.u8(state.has_policy ? 1 : 0);
  if (state.has_policy) codec::write_policy(payload, state.policy);
  for (std::uint64_t counter : state.counters) payload.u64(counter);
  payload.u32(static_cast<std::uint32_t>(domains.size()));
  for (const ExecutionDomainRecord& domain : domains) codec::write_domain(payload, domain);
  payload.u32(static_cast<std::uint32_t>(attempts.size()));
  for (const ExecutionAttempt& attempt : attempts) codec::write_attempt(payload, attempt);
  payload.u32(static_cast<std::uint32_t>(boots.size()));
  for (WorkerBootId boot : boots) payload.u64(boot.value());

  const std::vector<std::uint8_t>& body = payload.data();
  ByteWriter file(kMaxPersistedBytes);
  file.raw(kMagic.data(), kMagic.size());
  file.u16(kPersistenceFormatVersion);
  file.u16(0);  // flags
  file.u32(static_cast<std::uint32_t>(body.size()));
  file.u32(crc32c(body.data(), body.size()));
  file.raw(body.data(), body.size());
  return file.take();
}

std::uint32_t StateStore::payload_crc(const PersistedState& state) {
  const std::vector<std::uint8_t> bytes = encode(state);
  if (bytes.size() < kHeaderBytes) return 0;
  std::uint32_t crc = 0;
  std::memcpy(&crc, bytes.data() + 12, sizeof(crc));
  return crc;
}

Checked<PersistedState> StateStore::decode(const std::uint8_t* data, std::size_t size) {
  if (data == nullptr) return Checked<PersistedState>::bad("state.empty");
  if (size < kHeaderBytes) return Checked<PersistedState>::bad("state.truncated_header");
  if (size > kMaxPersistedBytes) return Checked<PersistedState>::bad("state.too_large");
  if (std::memcmp(data, kMagic.data(), kMagic.size()) != 0) {
    return Checked<PersistedState>::bad("state.bad_magic");
  }
  ByteReader header(data + 4, size - 4);
  std::uint16_t version = 0;
  std::uint16_t flags = 0;
  std::uint32_t length = 0;
  std::uint32_t declared_crc = 0;
  if (!header.u16(version) || !header.u16(flags) || !header.u32(length) || !header.u32(declared_crc)) {
    return Checked<PersistedState>::bad("state.truncated_header");
  }
  if (version != kPersistenceFormatVersion) {
    return Checked<PersistedState>::bad("state.unsupported_version", std::to_string(version));
  }
  if (flags != 0) return Checked<PersistedState>::bad("state.unsupported_flags");
  if (length != size - kHeaderBytes) return Checked<PersistedState>::bad("state.length_mismatch");
  const std::uint8_t* body = data + kHeaderBytes;
  if (crc32c(body, length) != declared_crc) {
    return Checked<PersistedState>::bad("state.integrity_failure");
  }

  ByteReader reader(body, length);
  PersistedState state;
  std::uint64_t value = 0;
  if (!reader.u64(value)) return Checked<PersistedState>::bad("state.truncated");
  state.generation = PersistenceGeneration(value);
  if (!reader.u64(value)) return Checked<PersistedState>::bad("state.truncated");
  state.last_epoch = CoordinatorEpoch(value);
  if (!reader.u64(value)) return Checked<PersistedState>::bad("state.truncated");
  state.policy_generation = PolicyGeneration(value);
  std::uint8_t has_policy = 0;
  if (!reader.u8(has_policy)) return Checked<PersistedState>::bad("state.truncated");
  state.has_policy = has_policy != 0;
  if (state.has_policy) {
    if (!codec::read_policy(reader, state.policy)) {
      return Checked<PersistedState>::bad("state.malformed_policy",
                                          reader.ok() ? std::string("policy bounds") : reader.error());
    }
    const Status policy_status = validate_policy(state.policy);
    if (!policy_status) {
      return Checked<PersistedState>::bad("state.invalid_policy", policy_status.code);
    }
  }
  for (std::uint64_t& counter : state.counters) {
    if (!reader.u64(counter)) return Checked<PersistedState>::bad("state.truncated");
  }
  std::uint32_t domain_count = 0;
  if (!reader.u32(domain_count)) return Checked<PersistedState>::bad("state.truncated");
  if (domain_count > kMaxPersistedDomains) {
    return Checked<PersistedState>::bad("state.too_many_domains");
  }
  state.domains.reserve(domain_count);
  std::uint64_t previous_id = 0;
  for (std::uint32_t i = 0; i < domain_count; ++i) {
    ExecutionDomainRecord domain;
    if (!codec::read_domain(reader, domain)) {
      return Checked<PersistedState>::bad("state.malformed_domain", reader.error());
    }
    if (!domain.id.valid()) return Checked<PersistedState>::bad("state.invalid_domain_identity");
    const Status domain_status = validate_domain_record(domain);
    if (!domain_status) {
      return Checked<PersistedState>::bad("state.invalid_domain", domain_status.code);
    }
    if (i > 0 && domain.id.value() <= previous_id) {
      return Checked<PersistedState>::bad("state.duplicate_or_unsorted_domain");
    }
    previous_id = domain.id.value();
    state.domains.push_back(std::move(domain));
  }
  std::uint32_t attempt_count = 0;
  if (!reader.u32(attempt_count)) return Checked<PersistedState>::bad("state.truncated");
  if (attempt_count > kMaxPersistedAttempts) {
    return Checked<PersistedState>::bad("state.too_many_attempts");
  }
  state.attempts.reserve(attempt_count);
  previous_id = 0;
  for (std::uint32_t i = 0; i < attempt_count; ++i) {
    ExecutionAttempt attempt;
    if (!codec::read_attempt(reader, attempt)) {
      return Checked<PersistedState>::bad("state.malformed_attempt", reader.error());
    }
    if (!attempt.id.valid() || !attempt.operation.valid()) {
      return Checked<PersistedState>::bad("state.invalid_attempt");
    }
    if (i > 0 && attempt.id.value() <= previous_id) {
      return Checked<PersistedState>::bad("state.duplicate_or_unsorted_attempt");
    }
    previous_id = attempt.id.value();
    if (attempt.state == AttemptState::kCompleted || attempt.state == AttemptState::kFailed) {
      if (!attempt.completion_committed) {
        return Checked<PersistedState>::bad("state.impossible_attempt_lifecycle",
                                            "terminal result without a committed completion");
      }
      if ((attempt.state == AttemptState::kCompleted) != attempt.result.success) {
        return Checked<PersistedState>::bad("state.impossible_attempt_lifecycle",
                                            "terminal state contradicts the recorded result");
      }
    }
    if (attempt.state == AttemptState::kDispatching || attempt.state == AttemptState::kDispatched ||
        attempt.state == AttemptState::kPlanned || attempt.state == AttemptState::kReserved) {
      return Checked<PersistedState>::bad("state.in_flight_attempt_persisted",
                                          "in-flight attempts must be classified before persistence");
    }
    state.attempts.push_back(std::move(attempt));
  }
  std::uint32_t boot_count = 0;
  if (!reader.u32(boot_count)) return Checked<PersistedState>::bad("state.truncated");
  if (boot_count > 65536) return Checked<PersistedState>::bad("state.too_many_boots");
  state.fenced_boots.reserve(boot_count);
  previous_id = 0;
  for (std::uint32_t i = 0; i < boot_count; ++i) {
    if (!reader.u64(value)) return Checked<PersistedState>::bad("state.truncated");
    const WorkerBootId boot(value);
    if (!boot.valid()) return Checked<PersistedState>::bad("state.invalid_boot_id");
    if (i > 0 && boot.value() <= previous_id) {
      return Checked<PersistedState>::bad("state.duplicate_or_unsorted_boot");
    }
    previous_id = boot.value();
    state.fenced_boots.push_back(boot);
  }
  if (!reader.at_end()) return Checked<PersistedState>::bad("state.trailing_bytes");
  if (!state.generation.published()) return Checked<PersistedState>::bad("state.missing_generation");
  return Checked<PersistedState>::good(std::move(state));
}

Checked<PersistedState> StateStore::decode(const std::vector<std::uint8_t>& bytes) {
  return decode(bytes.data(), bytes.size());
}

StateStore::StateStore(std::string path) : path_(std::move(path)) {}

bool StateStore::exists() const noexcept { return file_exists(path_); }

Status StateStore::save(const PersistedState& state) const {
  if (path_.empty()) return Status::failure("state.no_path");
  if (!is_safe_state_path(path_)) return Status::failure("state.unsafe_path", path_);
  const std::vector<std::uint8_t> bytes = encode(state);
  if (bytes.size() > kMaxPersistedBytes) return Status::failure("state.too_large");
  return write_file_atomic(path_, bytes);
}

Checked<PersistedState> StateStore::load() const {
  if (path_.empty()) return Checked<PersistedState>::bad("state.no_path");
  if (!is_safe_state_path(path_)) return Checked<PersistedState>::bad("state.unsafe_path");
  auto bytes = read_file_bytes(path_, kMaxPersistedBytes);
  if (!bytes.ok()) return Checked<PersistedState>::bad(bytes.status.code, bytes.status.message);
  return decode(bytes.value);
}

Status StateStore::remove() const { return remove_file(path_); }

}  // namespace tos
