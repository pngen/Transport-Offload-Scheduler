// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "tos/core/domain.hpp"

#include <algorithm>

#include "tos/util/bytes.hpp"

namespace tos {
namespace {

constexpr std::uint64_t kMaxCountableCapacity = 1ULL << 32;
constexpr std::uint64_t kMaxByteCapacity = 1ULL << 48;

bool is_power_of_two(std::uint32_t value) noexcept { return value != 0 && (value & (value - 1)) == 0; }

bool capacity_plausible(ResourceKind kind, std::uint64_t value) noexcept {
  switch (kind) {
    case ResourceKind::kDeviceMemoryBytes:
    case ResourceKind::kScratchMemoryBytes:
      return value <= kMaxByteCapacity;
    default:
      return value <= kMaxCountableCapacity;
  }
}

}  // namespace

Status validate_domain_record(const ExecutionDomainRecord& record) {
  if (!record.id.valid()) return Status::failure("domain.missing_identity");
  if (!is_valid_name(record.name)) return Status::failure("domain.invalid_name", record.name);
  if (!is_printable_ascii(record.name, kMaxNameLength)) {
    return Status::failure("domain.non_ascii_name", record.name);
  }
  if (record.name.size() > kMaxNameLength) return Status::failure("domain.name_too_long");
  if (!record.parent_device.empty() &&
      !is_printable_ascii(record.parent_device, kMaxLabelLength)) {
    return Status::failure("domain.invalid_parent_device");
  }
  if (!record.parent_host.empty() && !is_printable_ascii(record.parent_host, kMaxLabelLength)) {
    return Status::failure("domain.invalid_parent_host");
  }
  if (static_cast<int>(record.type) < 0 ||
      static_cast<int>(record.type) >= kExecutionDomainTypeCount) {
    return Status::failure("domain.invalid_type");
  }
  if (record.capability.domain != record.id) {
    return Status::failure("domain.capability_identity_mismatch");
  }
  if (record.capability.capability.alignment_bytes == 0 ||
      !is_power_of_two(record.capability.capability.alignment_bytes)) {
    return Status::failure("domain.invalid_alignment");
  }
  if (record.capability.capability.max_payload_bytes > kMaxPayloadBytes) {
    return Status::failure("domain.payload_limit_exceeded");
  }
  if (record.capability.capability.queue_capacity > kMaxCountableCapacity) {
    return Status::failure("domain.queue_capacity_exceeded");
  }
  if (record.capability.capability.max_concurrency == 0) {
    return Status::failure("domain.zero_concurrency");
  }
  if (!is_printable_ascii(record.capability.capability.backend_family, kMaxNameLength)) {
    return Status::failure("domain.invalid_backend_family");
  }
  if (!is_printable_ascii(record.fence_reason, kMaxTextLength)) {
    return Status::failure("domain.invalid_fence_reason");
  }
  for (int i = 0; i < kResourceKindCount; ++i) {
    if (!capacity_plausible(static_cast<ResourceKind>(i), record.capacity[static_cast<std::size_t>(i)])) {
      return Status::failure("domain.capacity_out_of_range");
    }
  }
  if (record.load.utilization_percent > 100 || record.load.congestion_percent > 100) {
    return Status::failure("domain.utilization_out_of_range");
  }
  if (record.load.queue_depth > kMaxCountableCapacity || record.load.in_flight > kMaxCountableCapacity) {
    return Status::failure("domain.queue_out_of_range");
  }
  if (record.provenance == Provenance::kUnsupported &&
      record.capability.provenance != Provenance::kUnsupported &&
      record.capability.provenance != Provenance::kSynthetic) {
    return Status::failure("domain.provenance_mismatch");
  }
  if (record.capability.provenance == Provenance::kReal && record.provenance == Provenance::kSynthetic) {
    return Status::failure("domain.provenance_overclaim");
  }
  return Status::success();
}

}  // namespace tos
