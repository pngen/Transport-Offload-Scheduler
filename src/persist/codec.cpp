// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "codec.hpp"

#include <algorithm>

namespace tos {
namespace codec {
namespace {

template <class IdType>
bool read_id(ByteReader& reader, IdType& out) {
  std::uint64_t value = 0;
  if (!reader.u64(value)) return false;
  out = IdType(value);
  return true;
}

template <class GenerationType>
bool read_generation(ByteReader& reader, GenerationType& out) {
  std::uint64_t value = 0;
  if (!reader.u64(value)) return false;
  out = GenerationType(value);
  return true;
}

bool read_bool(ByteReader& reader, bool& target) {
  std::uint8_t value = 0;
  if (!reader.u8(value)) return false;
  target = value != 0;
  return true;
}

template <class EnumType>
bool read_enum(ByteReader& reader, EnumType& target, std::uint8_t max_value) {
  std::uint8_t value = 0;
  if (!reader.u8(value)) return false;
  if (value > max_value) return reader.reject("codec.enum_out_of_range");
  target = static_cast<EnumType>(value);
  return true;
}

}  // namespace

void write_capability(ByteWriter& writer, const CapabilitySet& capability) {
  writer.u32(static_cast<std::uint32_t>(capability.operations.size()));
  for (OperationClassId id : capability.operations) writer.u64(id.value());
  writer.u32(capability.flags);
  writer.u32(capability.unproven_flags);
  writer.u32(capability.memory_domains);
  writer.u64(capability.min_payload_bytes);
  writer.u64(capability.max_payload_bytes);
  writer.u32(capability.alignment_bytes);
  writer.u32(capability.max_concurrency);
  writer.u32(capability.queue_capacity);
  writer.u32(capability.transport_classes);
  writer.u32(capability.payload_classes);
  writer.u32(capability.protocol_version);
  writer.u32(capability.driver_backend_version);
  writer.u32(capability.firmware_generation);
  writer.u32(capability.accelerator_arch);
  writer.text(capability.backend_family);
}

bool read_capability(ByteReader& reader, CapabilitySet& capability) {
  std::uint32_t count = 0;
  if (!reader.u32(count)) return false;
  if (count > kMaxOperationClasses) return reader.reject("codec.too_many_operations");
  capability.operations.clear();
  capability.operations.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    OperationClassId id;
    if (!read_id(reader, id)) return false;
    if (!id.valid()) return reader.reject("codec.invalid_operation_class");
    if (!capability.operations.empty() && id <= capability.operations.back()) {
      return reader.reject("codec.duplicate_or_unsorted_operation");
    }
    capability.operations.push_back(id);
  }
  if (!reader.u32(capability.flags)) return false;
  if (!reader.u32(capability.unproven_flags)) return false;
  if (!reader.u32(capability.memory_domains)) return false;
  if (!reader.u64(capability.min_payload_bytes)) return false;
  if (!reader.u64(capability.max_payload_bytes)) return false;
  if (!reader.u32(capability.alignment_bytes)) return false;
  if (!reader.u32(capability.max_concurrency)) return false;
  if (!reader.u32(capability.queue_capacity)) return false;
  if (!reader.u32(capability.transport_classes)) return false;
  if (!reader.u32(capability.payload_classes)) return false;
  if (!reader.u32(capability.protocol_version)) return false;
  if (!reader.u32(capability.driver_backend_version)) return false;
  if (!reader.u32(capability.firmware_generation)) return false;
  if (!reader.u32(capability.accelerator_arch)) return false;
  if (!reader.text(capability.backend_family, kMaxNameLength)) return false;
  return true;
}

void write_domain(ByteWriter& writer, const ExecutionDomainRecord& domain) {
  writer.u64(domain.id.value());
  writer.u64(domain.generation.value());
  writer.u8(static_cast<std::uint8_t>(domain.type));
  writer.text(domain.name);
  writer.text(domain.parent_device);
  writer.text(domain.parent_host);
  writer.u64(domain.worker.value());
  writer.u64(domain.worker_boot.value());
  writer.u8(static_cast<std::uint8_t>(domain.provenance));
  writer.u64(domain.registration_sequence);
  write_capability(writer, domain.capability.capability);
  writer.u64(domain.capability.domain.value());
  writer.u64(domain.capability.generation.value());
  writer.u64(domain.capability.evidence.value());
  writer.u8(static_cast<std::uint8_t>(domain.capability.provenance));
  writer.u8(domain.capability.authoritative ? 1 : 0);
  writer.u64(domain.capability.publisher.value());
  writer.u8(static_cast<std::uint8_t>(domain.isolation));
  writer.u8(domain.fenced ? 1 : 0);
  writer.text(domain.fence_reason);
  writer.u64(domain.backend_generation.value());
  writer.u64(domain.publisher_sequence.boot.value());
  writer.u64(domain.publisher_sequence.capability);
  writer.u64(domain.publisher_sequence.load);
  writer.u64(domain.publisher_sequence.queue);
  writer.u64(domain.publisher_sequence.health);
  writer.u64(domain.publisher_sequence.locality);
  writer.u64(domain.publisher_sequence.topology);
  writer.u64(domain.publisher_sequence.compatibility);
  writer.u64(domain.publisher_sequence.domain);
  writer.u64(domain.locality.generation.value());
  writer.u8(static_cast<std::uint8_t>(domain.locality.class_to_payload));
  writer.u32(domain.locality.numa_node);
  writer.u32(domain.locality.host_index);
  writer.u32(domain.locality.pcie_root_complex);
  writer.u32(domain.locality.pcie_switch);
  writer.u64(domain.locality.local_nic.value());
  writer.u8(domain.locality.has_local_nic ? 1 : 0);
  writer.u64(domain.topology.generation.value());
  writer.text(domain.topology.host_node);
  writer.text(domain.topology.fabric);
  writer.u32(domain.topology.rack);
  writer.u32(domain.topology.pod);
  writer.u64(domain.compatibility.generation.value());
  writer.u32(domain.compatibility.driver_backend_version);
  writer.u32(domain.compatibility.firmware_generation);
  writer.u32(domain.compatibility.protocol_version);
  writer.u32(domain.compatibility.accelerator_arch);
  writer.text(domain.compatibility.backend_family);
  for (std::uint64_t value : domain.capacity) writer.u64(value);
}

bool read_domain(ByteReader& reader, ExecutionDomainRecord& domain) {
  if (!read_id(reader, domain.id)) return false;
  if (!read_generation(reader, domain.generation)) return false;
  if (!read_enum(reader, domain.type, kExecutionDomainTypeCount - 1)) return false;
  if (!reader.text(domain.name, kMaxNameLength)) return false;
  if (!reader.text(domain.parent_device, kMaxLabelLength)) return false;
  if (!reader.text(domain.parent_host, kMaxLabelLength)) return false;
  if (!read_id(reader, domain.worker)) return false;
  if (!read_id(reader, domain.worker_boot)) return false;
  if (!read_enum(reader, domain.provenance, static_cast<std::uint8_t>(Provenance::kUnsupported))) {
    return false;
  }
  if (!reader.u64(domain.registration_sequence)) return false;
  if (!read_capability(reader, domain.capability.capability)) return false;
  if (!read_id(reader, domain.capability.domain)) return false;
  if (!read_generation(reader, domain.capability.generation)) return false;
  if (!read_generation(reader, domain.capability.evidence)) return false;
  if (!read_enum(reader, domain.capability.provenance,
                 static_cast<std::uint8_t>(Provenance::kUnsupported))) {
    return false;
  }
  if (!read_bool(reader, domain.capability.authoritative)) return false;
  if (!read_id(reader, domain.capability.publisher)) return false;
  if (!read_enum(reader, domain.isolation,
                 static_cast<std::uint8_t>(IsolationClass::kDedicatedDeviceService))) {
    return false;
  }
  if (!read_bool(reader, domain.fenced)) return false;
  if (!reader.text(domain.fence_reason, kMaxTextLength)) return false;
  if (!read_generation(reader, domain.backend_generation)) return false;
  if (!read_id(reader, domain.publisher_sequence.boot)) return false;
  if (!reader.u64(domain.publisher_sequence.capability)) return false;
  if (!reader.u64(domain.publisher_sequence.load)) return false;
  if (!reader.u64(domain.publisher_sequence.queue)) return false;
  if (!reader.u64(domain.publisher_sequence.health)) return false;
  if (!reader.u64(domain.publisher_sequence.locality)) return false;
  if (!reader.u64(domain.publisher_sequence.topology)) return false;
  if (!reader.u64(domain.publisher_sequence.compatibility)) return false;
  if (!reader.u64(domain.publisher_sequence.domain)) return false;
  if (!read_generation(reader, domain.locality.generation)) return false;
  if (!read_enum(reader, domain.locality.class_to_payload,
                 static_cast<std::uint8_t>(LocalityClass::kDomainLocal))) {
    return false;
  }
  if (!reader.u32(domain.locality.numa_node)) return false;
  if (!reader.u32(domain.locality.host_index)) return false;
  if (!reader.u32(domain.locality.pcie_root_complex)) return false;
  if (!reader.u32(domain.locality.pcie_switch)) return false;
  if (!read_id(reader, domain.locality.local_nic)) return false;
  if (!read_bool(reader, domain.locality.has_local_nic)) return false;
  if (!read_generation(reader, domain.topology.generation)) return false;
  if (!reader.text(domain.topology.host_node, kMaxLabelLength)) return false;
  if (!reader.text(domain.topology.fabric, kMaxLabelLength)) return false;
  if (!reader.u32(domain.topology.rack)) return false;
  if (!reader.u32(domain.topology.pod)) return false;
  if (!read_generation(reader, domain.compatibility.generation)) return false;
  if (!reader.u32(domain.compatibility.driver_backend_version)) return false;
  if (!reader.u32(domain.compatibility.firmware_generation)) return false;
  if (!reader.u32(domain.compatibility.protocol_version)) return false;
  if (!reader.u32(domain.compatibility.accelerator_arch)) return false;
  if (!reader.text(domain.compatibility.backend_family, kMaxNameLength)) return false;
  for (std::uint64_t& value : domain.capacity) {
    if (!reader.u64(value)) return false;
  }
  return true;
}

void write_attempt(ByteWriter& writer, const ExecutionAttempt& attempt) {
  writer.u64(attempt.id.value());
  writer.u64(attempt.generation.value());
  writer.u64(attempt.operation.value());
  writer.u64(attempt.operation_class.value());
  writer.u8(static_cast<std::uint8_t>(attempt.side_effect));
  writer.u64(attempt.domain.value());
  writer.u8(static_cast<std::uint8_t>(attempt.domain_type));
  writer.u64(attempt.domain_generation.value());
  writer.u64(attempt.capability_generation.value());
  writer.u64(attempt.worker.value());
  writer.u64(attempt.worker_boot.value());
  writer.u64(attempt.coordinator_epoch.value());
  writer.u64(attempt.reservation.value());
  writer.u64(attempt.dispatch.value());
  writer.u8(static_cast<std::uint8_t>(attempt.state));
  writer.u8(static_cast<std::uint8_t>(attempt.resolution));
  writer.u8(static_cast<std::uint8_t>(attempt.failure));
  writer.text(bounded_text(attempt.failure_detail));
  writer.text(bounded_text(attempt.close_reason));
  writer.u8(static_cast<std::uint8_t>(attempt.provenance));
  writer.u8(attempt.fallback ? 1 : 0);
  writer.u64(attempt.fallback_from.value());
  writer.u32(attempt.retry_index);
  writer.u8(attempt.cancellation_requested ? 1 : 0);
  writer.u8(attempt.completion_committed ? 1 : 0);
  writer.u8(attempt.result.success ? 1 : 0);
  writer.u64(attempt.result.result_digest);
  writer.u64(attempt.result.bytes_processed);
  writer.u64(attempt.result.duration_ns);
  writer.text(bounded_text(attempt.result.backend_detail));
  writer.u64(attempt.sequence);
  writer.u64(attempt.dispatch_sequence);
  writer.u64(attempt.snapshot.value());
}

bool read_attempt(ByteReader& reader, ExecutionAttempt& attempt) {
  if (!read_id(reader, attempt.id)) return false;
  if (!read_generation(reader, attempt.generation)) return false;
  if (!read_id(reader, attempt.operation)) return false;
  if (!read_id(reader, attempt.operation_class)) return false;
  if (!read_enum(reader, attempt.side_effect,
                 static_cast<std::uint8_t>(SideEffectClass::kUnknown))) {
    return false;
  }
  if (!read_id(reader, attempt.domain)) return false;
  if (!read_enum(reader, attempt.domain_type, kExecutionDomainTypeCount - 1)) return false;
  if (!read_generation(reader, attempt.domain_generation)) return false;
  if (!read_generation(reader, attempt.capability_generation)) return false;
  if (!read_id(reader, attempt.worker)) return false;
  if (!read_id(reader, attempt.worker_boot)) return false;
  if (!read_generation(reader, attempt.coordinator_epoch)) return false;
  if (!read_id(reader, attempt.reservation)) return false;
  if (!read_id(reader, attempt.dispatch)) return false;
  if (!read_enum(reader, attempt.state, static_cast<std::uint8_t>(AttemptState::kFenced))) {
    return false;
  }
  if (!read_enum(reader, attempt.resolution,
                 static_cast<std::uint8_t>(AttemptResolution::kFencedMayHaveEffect))) {
    return false;
  }
  if (!read_enum(reader, attempt.failure, static_cast<std::uint8_t>(kFailureKindCount - 1))) {
    return false;
  }
  if (!reader.text(attempt.failure_detail, kMaxTextLength)) return false;
  if (!reader.text(attempt.close_reason, kMaxTextLength)) return false;
  if (!read_enum(reader, attempt.provenance, static_cast<std::uint8_t>(Provenance::kUnsupported))) {
    return false;
  }
  if (!read_bool(reader, attempt.fallback)) return false;
  if (!read_id(reader, attempt.fallback_from)) return false;
  if (!reader.u32(attempt.retry_index)) return false;
  if (!read_bool(reader, attempt.cancellation_requested)) return false;
  if (!read_bool(reader, attempt.completion_committed)) return false;
  if (!read_bool(reader, attempt.result.success)) return false;
  if (!reader.u64(attempt.result.result_digest)) return false;
  if (!reader.u64(attempt.result.bytes_processed)) return false;
  if (!reader.u64(attempt.result.duration_ns)) return false;
  if (!reader.text(attempt.result.backend_detail, kMaxTextLength)) return false;
  if (!reader.u64(attempt.sequence)) return false;
  if (!reader.u64(attempt.dispatch_sequence)) return false;
  if (!read_generation(reader, attempt.snapshot)) return false;
  return true;
}

void write_policy(ByteWriter& writer, const SchedulerPolicy& policy) {
  writer.u64(policy.generation.value());
  writer.u64(policy.isolation_generation.value());
  writer.u64(policy.compatibility_generation.value());
  writer.u8(static_cast<std::uint8_t>(policy.offload_requirement));
  writer.u8(static_cast<std::uint8_t>(policy.minimum_isolation));
  writer.u8(policy.require_positive_evidence ? 1 : 0);
  writer.u8(policy.allow_unsupported_provenance ? 1 : 0);
  writer.u32(static_cast<std::uint32_t>(policy.forbidden_domains.size()));
  for (ExecutionDomainType type : policy.forbidden_domains) {
    writer.u8(static_cast<std::uint8_t>(type));
  }
  writer.u32(static_cast<std::uint32_t>(policy.forbidden_domain_ids.size()));
  for (ExecutionDomainId id : policy.forbidden_domain_ids) writer.u64(id.value());
  writer.u32(static_cast<std::uint32_t>(policy.forbidden_provenance.size()));
  for (Provenance value : policy.forbidden_provenance) {
    writer.u8(static_cast<std::uint8_t>(value));
  }
  for (std::int32_t weight : policy.weights) writer.u32(static_cast<std::uint32_t>(weight));
  writer.u32(static_cast<std::uint32_t>(policy.preference_order.size()));
  for (ExecutionDomainType type : policy.preference_order) {
    writer.u8(static_cast<std::uint8_t>(type));
  }
  writer.u8(policy.reservation.enabled ? 1 : 0);
  writer.u8(policy.reservation.require_reservation_for_offload_only ? 1 : 0);
  for (std::uint64_t amount : policy.reservation.per_operation) writer.u64(amount);
  writer.u8(policy.retry.enabled ? 1 : 0);
  writer.u32(policy.retry.max_attempts);
  writer.u32(policy.retry.max_ambiguous_retries);
  for (std::size_t i = 0; i < static_cast<std::size_t>(kFailureKindCount); ++i) {
    writer.u8(policy.retry.rules.retryable[i] ? 1 : 0);
  }
  writer.u8(policy.fallback.enabled ? 1 : 0);
  writer.u32(policy.fallback.max_depth);
  writer.u8(policy.fallback.allow_host_fallback ? 1 : 0);
  for (const std::vector<ExecutionDomainType>& chain : policy.fallback.chains) {
    writer.u32(static_cast<std::uint32_t>(chain.size()));
    for (ExecutionDomainType type : chain) writer.u8(static_cast<std::uint8_t>(type));
  }
  writer.u8(policy.freshness.capability ? 1 : 0);
  writer.u8(policy.freshness.health ? 1 : 0);
  writer.u8(policy.freshness.queue ? 1 : 0);
  writer.u8(policy.freshness.load ? 1 : 0);
  writer.u8(policy.freshness.topology ? 1 : 0);
  writer.u8(policy.freshness.locality ? 1 : 0);
  writer.u8(policy.freshness.compatibility ? 1 : 0);
  writer.u8(policy.freshness.isolation ? 1 : 0);
  writer.u8(policy.freshness.evidence ? 1 : 0);
  for (std::size_t i = 0; i < kExecutionDomainTypeCount; ++i) {
    writer.u64(policy.type_setup_cost_units[i]);
    writer.u64(policy.type_per_byte_cost_micro_units[i]);
  }
  writer.u64(policy.maximum_cost_units);
  writer.u64(static_cast<std::uint64_t>(policy.max_reported_candidates));
}

bool read_policy(ByteReader& reader, SchedulerPolicy& policy) {
  if (!read_generation(reader, policy.generation)) return false;
  if (!read_generation(reader, policy.isolation_generation)) return false;
  if (!read_generation(reader, policy.compatibility_generation)) return false;
  if (!read_enum(reader, policy.offload_requirement,
                 static_cast<std::uint8_t>(OffloadRequirement::kHostRequired))) {
    return false;
  }
  if (!read_enum(reader, policy.minimum_isolation,
                 static_cast<std::uint8_t>(IsolationClass::kDedicatedDeviceService))) {
    return false;
  }
  if (!read_bool(reader, policy.require_positive_evidence)) return false;
  if (!read_bool(reader, policy.allow_unsupported_provenance)) return false;
  std::uint32_t count = 0;
  if (!reader.u32(count)) return false;
  if (count > kExecutionDomainTypeCount) return reader.reject("codec.too_many_domains");
  policy.forbidden_domains.clear();
  for (std::uint32_t i = 0; i < count; ++i) {
    ExecutionDomainType type{};
    if (!read_enum(reader, type, kExecutionDomainTypeCount - 1)) return false;
    policy.forbidden_domains.push_back(type);
  }
  if (!reader.u32(count)) return false;
  if (count > kMaxExecutionDomains) return reader.reject("codec.too_many_domains");
  policy.forbidden_domain_ids.clear();
  for (std::uint32_t i = 0; i < count; ++i) {
    ExecutionDomainId id;
    if (!read_id(reader, id)) return false;
    policy.forbidden_domain_ids.push_back(id);
  }
  if (!reader.u32(count)) return false;
  if (count > 8) return reader.reject("codec.too_many_provenance_entries");
  policy.forbidden_provenance.clear();
  for (std::uint32_t i = 0; i < count; ++i) {
    Provenance value{};
    if (!read_enum(reader, value, static_cast<std::uint8_t>(Provenance::kUnsupported))) return false;
    policy.forbidden_provenance.push_back(value);
  }
  for (std::size_t i = 0; i < kRankingFactorCount; ++i) {
    std::uint32_t weight = 0;
    if (!reader.u32(weight)) return false;
    policy.weights[i] = static_cast<std::int32_t>(weight);
  }
  if (!reader.u32(count)) return false;
  if (count > kExecutionDomainTypeCount) return reader.reject("codec.too_many_preferences");
  policy.preference_order.clear();
  for (std::uint32_t i = 0; i < count; ++i) {
    ExecutionDomainType type{};
    if (!read_enum(reader, type, kExecutionDomainTypeCount - 1)) return false;
    policy.preference_order.push_back(type);
  }
  if (!read_bool(reader, policy.reservation.enabled)) return false;
  if (!read_bool(reader, policy.reservation.require_reservation_for_offload_only)) return false;
  for (std::uint64_t& amount : policy.reservation.per_operation) {
    if (!reader.u64(amount)) return false;
  }
  if (!read_bool(reader, policy.retry.enabled)) return false;
  if (!reader.u32(policy.retry.max_attempts)) return false;
  if (!reader.u32(policy.retry.max_ambiguous_retries)) return false;
  for (std::size_t i = 0; i < static_cast<std::size_t>(kFailureKindCount); ++i) {
    if (!read_bool(reader, policy.retry.rules.retryable[i])) return false;
  }
  if (!read_bool(reader, policy.fallback.enabled)) return false;
  if (!reader.u32(policy.fallback.max_depth)) return false;
  if (!read_bool(reader, policy.fallback.allow_host_fallback)) return false;
  for (std::vector<ExecutionDomainType>& chain : policy.fallback.chains) {
    chain.clear();
    if (!reader.u32(count)) return false;
    if (count > kExecutionDomainTypeCount) return reader.reject("codec.fallback_chain_too_long");
    for (std::uint32_t i = 0; i < count; ++i) {
      ExecutionDomainType type{};
      if (!read_enum(reader, type, kExecutionDomainTypeCount - 1)) return false;
      chain.push_back(type);
    }
  }
  if (!read_bool(reader, policy.freshness.capability)) return false;
  if (!read_bool(reader, policy.freshness.health)) return false;
  if (!read_bool(reader, policy.freshness.queue)) return false;
  if (!read_bool(reader, policy.freshness.load)) return false;
  if (!read_bool(reader, policy.freshness.topology)) return false;
  if (!read_bool(reader, policy.freshness.locality)) return false;
  if (!read_bool(reader, policy.freshness.compatibility)) return false;
  if (!read_bool(reader, policy.freshness.isolation)) return false;
  if (!read_bool(reader, policy.freshness.evidence)) return false;
  for (std::size_t i = 0; i < kExecutionDomainTypeCount; ++i) {
    if (!reader.u64(policy.type_setup_cost_units[i])) return false;
    if (!reader.u64(policy.type_per_byte_cost_micro_units[i])) return false;
  }
  if (!reader.u64(policy.maximum_cost_units)) return false;
  std::uint64_t reported = 0;
  if (!reader.u64(reported)) return false;
  if (reported == 0 || reported > kMaxReportedCandidates) {
    return reader.reject("codec.candidate_report_bound_invalid");
  }
  policy.max_reported_candidates = static_cast<std::size_t>(reported);
  return true;
}

}  // namespace codec
}  // namespace tos