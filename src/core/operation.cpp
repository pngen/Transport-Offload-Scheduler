// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "tos/core/operation.hpp"

#include <algorithm>
#include <array>

#include "tos/core/policy.hpp"
#include "tos/util/bytes.hpp"

namespace tos {
namespace {

bool is_canonical_token(std::string_view name) noexcept {
  if (name.empty() || name.size() > kMaxNameLength) return false;
  for (char c : name) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    if (!ok) return false;
  }
  return name.front() != '_' && name.back() != '_';
}

bool is_power_of_two(std::uint32_t value) noexcept { return value != 0 && (value & (value - 1)) == 0; }

SideEffectClass more_conservative(SideEffectClass a, SideEffectClass b) noexcept {
  auto severity = [](SideEffectClass value) -> int {
    switch (value) {
      case SideEffectClass::kPure: return 0;
      case SideEffectClass::kIdempotent: return 1;
      case SideEffectClass::kAtMostOnceRequired: return 2;
      case SideEffectClass::kUnknown: return 2;  // unknown is never treated as replay-safe
      case SideEffectClass::kNonRepeatable: return 3;
    }
    return 2;
  };
  return severity(a) >= severity(b) ? a : b;
}

std::vector<OperationClassDescriptor> builtin_descriptors() {
  std::vector<OperationClassDescriptor> classes;
  auto add = [&classes](std::uint64_t id, std::string name, std::string_view category,
                        SideEffectClass side_effect, CapabilityFlag flag,
                        TransportClass transport, PayloadClass payload) {
    OperationClassDescriptor descriptor;
    descriptor.id = OperationClassId(id);
    descriptor.name = std::move(name);
    descriptor.category = category;
    descriptor.side_effect_class = side_effect;
    descriptor.required_flag = flag;
    descriptor.transport_class = transport;
    descriptor.payload_class = payload;
    classes.push_back(std::move(descriptor));
  };
  add(1, "CHECKSUM_CRC32C", "integrity", SideEffectClass::kPure, CapabilityFlag::kChecksum,
      TransportClass::kRawFrames, PayloadClass::kOpaqueBytes);
  add(2, "COMPRESS_RLE", "transform", SideEffectClass::kPure, CapabilityFlag::kCompression,
      TransportClass::kStreamBytes, PayloadClass::kOpaqueBytes);
  add(3, "DECOMPRESS_RLE", "transform", SideEffectClass::kPure, CapabilityFlag::kDecompression,
      TransportClass::kStreamBytes, PayloadClass::kOpaqueBytes);
  add(4, "COPY_STAGE", "staging", SideEffectClass::kIdempotent, CapabilityFlag::kStagingCopy,
      TransportClass::kRawFrames, PayloadClass::kOpaqueBytes);
  add(5, "SEGMENT_SPLIT", "framing", SideEffectClass::kPure, CapabilityFlag::kSegmentation,
      TransportClass::kStreamBytes, PayloadClass::kOpaqueBytes);
  add(6, "REASSEMBLE_JOIN", "framing", SideEffectClass::kPure, CapabilityFlag::kReassembly,
      TransportClass::kStreamBytes, PayloadClass::kSegmentedFrames);
  add(7, "VALIDATE_INTEGRITY", "integrity", SideEffectClass::kPure, CapabilityFlag::kIntegrityVerify,
      TransportClass::kRawFrames, PayloadClass::kOpaqueBytes);
  add(8, "SIDE_EFFECT_EMIT", "telemetry", SideEffectClass::kNonRepeatable,
      CapabilityFlag::kSideEffectSink, TransportClass::kControlPlane,
      PayloadClass::kOpaqueBytes);
  add(9, "NOOP_PROBE", "probe", SideEffectClass::kPure, CapabilityFlag::kNone,
      TransportClass::kControlPlane, PayloadClass::kOpaqueBytes);
  return classes;
}

}  // namespace

OperationClassRegistry::OperationClassRegistry() : classes_(builtin_descriptors()) {}

const OperationClassRegistry& OperationClassRegistry::builtins() {
  static const OperationClassRegistry registry;
  return registry;
}

const OperationClassDescriptor* OperationClassRegistry::find(OperationClassId id) const noexcept {
  for (const OperationClassDescriptor& descriptor : classes_) {
    if (descriptor.id == id) return &descriptor;
  }
  return nullptr;
}

const OperationClassDescriptor* OperationClassRegistry::find(std::string_view name) const noexcept {
  for (const OperationClassDescriptor& descriptor : classes_) {
    if (descriptor.name == name) return &descriptor;
  }
  return nullptr;
}

Checked<OperationClassId> OperationClassRegistry::register_class(OperationClassDescriptor descriptor) {
  if (classes_.size() >= kMaxOperationClasses) {
    return Checked<OperationClassId>::bad("operation.class_limit");
  }
  if (!is_canonical_token(descriptor.name)) {
    return Checked<OperationClassId>::bad("operation.invalid_name", descriptor.name);
  }
  if (find(descriptor.name) != nullptr) {
    return Checked<OperationClassId>::bad("operation.duplicate_name", descriptor.name);
  }
  std::uint64_t next = 1;
  for (const OperationClassDescriptor& existing : classes_) {
    next = std::max(next, existing.id.value() + 1);
  }
  if (descriptor.id.valid()) {
    if (find(descriptor.id) != nullptr) {
      return Checked<OperationClassId>::bad("operation.duplicate_id");
    }
    next = descriptor.id.value();
  }
  descriptor.id = OperationClassId(next);
  if (descriptor.category.size() > kMaxNameLength) {
    descriptor.category = descriptor.category.substr(0, kMaxNameLength);
  }
  classes_.push_back(std::move(descriptor));
  return Checked<OperationClassId>::good(classes_.back().id);
}

namespace opclass {
OperationClassId checksum_crc32c() noexcept { return OperationClassId(1); }
OperationClassId compress_rle() noexcept { return OperationClassId(2); }
OperationClassId decompress_rle() noexcept { return OperationClassId(3); }
OperationClassId copy_stage() noexcept { return OperationClassId(4); }
OperationClassId segment_split() noexcept { return OperationClassId(5); }
OperationClassId reassemble_join() noexcept { return OperationClassId(6); }
OperationClassId validate_integrity() noexcept { return OperationClassId(7); }
OperationClassId side_effect_emit() noexcept { return OperationClassId(8); }
OperationClassId noop_probe() noexcept { return OperationClassId(9); }
}  // namespace opclass

std::vector<CapabilityFlag> all_capability_flags() {
  static constexpr std::array<CapabilityFlag, kCapabilityFlagCount> kFlags = {
      CapabilityFlag::kChecksum,       CapabilityFlag::kCompression,
      CapabilityFlag::kDecompression,  CapabilityFlag::kEncryption,
      CapabilityFlag::kDecryption,     CapabilityFlag::kDma,
      CapabilityFlag::kScatterGather,  CapabilityFlag::kRdma,
      CapabilityFlag::kGpudirectAddressable, CapabilityFlag::kPacketProcessing,
      CapabilityFlag::kHeaderProcessing,     CapabilityFlag::kProtocolTransform,
      CapabilityFlag::kIntegrityVerify,      CapabilityFlag::kSegmentation,
      CapabilityFlag::kReassembly,           CapabilityFlag::kStagingCopy,
      CapabilityFlag::kSideEffectSink};
  return std::vector<CapabilityFlag>(kFlags.begin(), kFlags.end());
}

std::vector<std::string_view> describe_flags(std::uint32_t bits) {
  std::vector<std::string_view> names;
  for (CapabilityFlag flag : all_capability_flags()) {
    if (has_flag(bits, flag)) names.push_back(to_string(flag));
  }
  return names;
}

SideEffectClass effective_side_effect(SideEffectClass registered,
                                      SideEffectClass override_value) noexcept {
  if (override_value == SideEffectClass::kUnknown) return registered;
  return more_conservative(registered, override_value);
}

bool is_replay_safe(SideEffectClass value) noexcept {
  return value == SideEffectClass::kPure || value == SideEffectClass::kIdempotent;
}

Status validate_request(const OperationRequest& request, const OperationClassRegistry& registry) {
  const OperationClassDescriptor* descriptor = registry.find(request.operation_class);
  if (descriptor == nullptr) {
    return Status::failure("request.unknown_operation_class");
  }
  if (request.payload.size_bytes > kMaxPayloadBytes) {
    return Status::failure("request.payload_too_large");
  }
  if (request.payload.alignment_bytes == 0 || !is_power_of_two(request.payload.alignment_bytes)) {
    return Status::failure("request.invalid_alignment");
  }
  if (request.payload.segment_count == 0 || request.payload.segment_count > (1U << 20)) {
    return Status::failure("request.invalid_segment_count");
  }
  if (request.payload.scatter_gather && request.payload.segment_count < 2) {
    return Status::failure("request.scatter_gather_needs_segments");
  }
  for (ExecutionDomainType type : request.forbidden_domains) {
    if (std::find(request.allowed_domains.begin(), request.allowed_domains.end(), type) !=
        request.allowed_domains.end()) {
      return Status::failure("request.contradictory_domain_filter");
    }
  }
  if (request.retry.max_attempts == 0 || request.retry.max_attempts > kMaxRetryAttempts) {
    return Status::failure("request.invalid_retry_bound");
  }
  if (!request.correlation_label.empty() &&
      !is_printable_ascii(request.correlation_label, kMaxLabelLength)) {
    return Status::failure("request.invalid_label");
  }
  if (request.allowed_domains.size() > static_cast<std::size_t>(kExecutionDomainTypeCount) ||
      request.forbidden_domains.size() > static_cast<std::size_t>(kExecutionDomainTypeCount)) {
    return Status::failure("request.domain_filter_too_large");
  }
  if (request.allowed_domain_ids.size() > kMaxExecutionDomains) {
    return Status::failure("request.domain_id_filter_too_large");
  }
  if (request.economics.has_value() &&
      request.economics->maximum_total_cost_units > 0 &&
      request.economics->setup_cost_units > request.economics->maximum_total_cost_units) {
    return Status::failure("request.cost_ceiling_below_setup");
  }
  return Status::success();
}

}  // namespace tos
