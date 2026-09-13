// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "tos/core/capability.hpp"

#include <algorithm>

namespace tos {

void CapabilitySet::canonicalize() {
  std::sort(operations.begin(), operations.end());
  operations.erase(std::unique(operations.begin(), operations.end()), operations.end());
  flags &= ~unproven_flags;  // an unproven property is never claimed as proven
  if (alignment_bytes == 0) alignment_bytes = 1;
  if (max_payload_bytes > kMaxPayloadBytes) max_payload_bytes = kMaxPayloadBytes;
  if (min_payload_bytes > max_payload_bytes) min_payload_bytes = max_payload_bytes;
  if (max_concurrency == 0) max_concurrency = 1;
  memory_domains &= (1U << kMemoryDomainCount) - 1U;
  transport_classes &= (1U << kTransportClassCount) - 1U;
  payload_classes &= 0x0FU;
  if (backend_family.size() > kMaxNameLength) backend_family.resize(kMaxNameLength);
}

bool CapabilitySet::supports_operation(OperationClassId id) const noexcept {
  return std::binary_search(operations.begin(), operations.end(), id);
}

bool CapabilitySet::memory_domain_supported(MemoryDomain value) const noexcept {
  return (memory_domains & memory_bit(value)) != 0U;
}

CapabilityCheck check_capability(const CapabilityRecord& record,
                                 const OperationClassRegistry& registry,
                                 const OperationRequest& request,
                                 bool require_positive_evidence) {
  CapabilityCheck check;
  if (!record.domain.valid()) {
    check.reason = IneligibilityReason::kDomainNotRegistered;
    check.detail = "capability record has no domain identity";
    return check;
  }
  if (record.provenance == Provenance::kUnsupported) {
    check.reason = IneligibilityReason::kBackendUnsupported;
    check.detail = "domain provenance is UNSUPPORTED";
    return check;
  }
  if (!record.authoritative || !record.published()) {
    if (require_positive_evidence) {
      check.reason = IneligibilityReason::kCapabilityUnproven;
      check.detail = "capability record is not authoritative and policy requires positive evidence";
      return check;
    }
  }
  const OperationClassDescriptor* descriptor = registry.find(request.operation_class);
  if (descriptor == nullptr) {
    check.reason = IneligibilityReason::kUnknownOperationClass;
    check.detail = "operation class is not registered";
    return check;
  }
  const CapabilitySet& caps = record.capability;
  if (!caps.supports_operation(descriptor->id)) {
    check.reason = IneligibilityReason::kOperationNotSupported;
    check.detail = std::string("domain does not publish operation class ") + descriptor->name;
    return check;
  }
  auto require_flag = [&](CapabilityFlag flag, IneligibilityReason missing_reason,
                          std::string_view what) -> bool {
    if (has_flag(caps.flags, flag)) return true;
    check.reason = has_flag(caps.unproven_flags, flag) || require_positive_evidence
                       ? IneligibilityReason::kCapabilityUnproven
                       : missing_reason;
    check.detail = std::string("capability ") + std::string(what) + " is not proven by this domain";
    return false;
  };
  if (descriptor->required_flag != CapabilityFlag::kNone) {
    if (!require_flag(descriptor->required_flag, IneligibilityReason::kOperationNotSupported,
                      to_string(descriptor->required_flag))) {
      return check;
    }
  }
  for (CapabilityFlag flag : all_capability_flags()) {
    if (!has_flag(request.required_capability_flags, flag)) continue;
    if (!require_flag(flag, IneligibilityReason::kOperationNotSupported, to_string(flag))) {
      return check;
    }
  }
  if (request.require_dma &&
      !require_flag(CapabilityFlag::kDma, IneligibilityReason::kDmaRequired, "dma")) {
    return check;
  }
  if (request.require_scatter_gather &&
      !require_flag(CapabilityFlag::kScatterGather, IneligibilityReason::kScatterGatherRequired,
                    "scatter_gather")) {
    return check;
  }
  if (!caps.memory_domain_supported(request.payload.source_memory)) {
    check.reason = IneligibilityReason::kMemoryDomainUnsupported;
    check.detail = std::string("source memory domain ") +
                   std::string(to_string(request.payload.source_memory)) + " is not addressable";
    return check;
  }
  if (!caps.memory_domain_supported(request.payload.destination_memory)) {
    check.reason = IneligibilityReason::kMemoryDomainUnsupported;
    check.detail = std::string("destination memory domain ") +
                   std::string(to_string(request.payload.destination_memory)) +
                   " is not addressable";
    return check;
  }
  if (caps.memory_domains == 0) {
    check.reason = IneligibilityReason::kAddressabilityUnsupported;
    check.detail = "domain publishes no addressable memory domains";
    return check;
  }
  if (request.payload.size_bytes > caps.max_payload_bytes) {
    check.reason = IneligibilityReason::kPayloadTooLarge;
    check.detail = "payload exceeds declared maximum";
    return check;
  }
  if (request.payload.size_bytes < caps.min_payload_bytes) {
    check.reason = IneligibilityReason::kPayloadTooSmall;
    check.detail = "payload below declared minimum";
    return check;
  }
  if (caps.alignment_bytes < request.payload.alignment_bytes) {
    check.reason = IneligibilityReason::kAlignmentUnsatisfied;
    check.detail = "engine alignment guarantee is weaker than the payload requirement";
    return check;
  }
  if ((caps.transport_classes & transport_bit(request.payload.transport_class)) == 0U) {
    check.reason = IneligibilityReason::kTransportClassUnsupported;
    check.detail = std::string("transport class ") +
                   std::string(to_string(request.payload.transport_class)) + " unsupported";
    return check;
  }
  if ((caps.payload_classes & payload_bit(request.payload.payload_class)) == 0U) {
    check.reason = IneligibilityReason::kPayloadClassUnsupported;
    check.detail = std::string("payload class ") +
                   std::string(to_string(request.payload.payload_class)) + " unsupported";
    return check;
  }
  check.supported = true;
  check.reason = IneligibilityReason::kNone;
  check.detail = "capability satisfied";
  return check;
}

}  // namespace tos
