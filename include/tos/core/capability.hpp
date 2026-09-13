// Generation-bound capability publication.
//
// Capability is not authority. A capability record is proof that an engine can
// technically perform an operation class; authorization is decided separately by
// policy, health, load, generation freshness and reservation state.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#ifndef TOS_CORE_CAPABILITY_HPP
#define TOS_CORE_CAPABILITY_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "tos/core/enums.hpp"
#include "tos/core/identities.hpp"
#include "tos/core/operation.hpp"

namespace tos {

/// Bitmask over MemoryDomain values.
[[nodiscard]] constexpr std::uint32_t memory_bit(MemoryDomain value) noexcept {
  return 1U << static_cast<std::uint32_t>(value);
}
/// Bitmask over TransportClass values.
[[nodiscard]] constexpr std::uint32_t transport_bit(TransportClass value) noexcept {
  return 1U << static_cast<std::uint32_t>(value);
}
/// Bitmask over PayloadClass values.
[[nodiscard]] constexpr std::uint32_t payload_bit(PayloadClass value) noexcept {
  return 1U << static_cast<std::uint32_t>(value);
}

/// Proven properties of one execution domain. Absent is not the same as unknown:
/// unproven_flags records properties the publisher could not prove.
struct CapabilitySet {
  std::vector<OperationClassId> operations;  ///< canonical: sorted, unique
  std::uint32_t flags{0};                    ///< CapabilityFlag bits proven present
  std::uint32_t unproven_flags{0};           ///< CapabilityFlag bits explicitly unproven
  std::uint32_t memory_domains{0};           ///< MemoryDomain bits addressable by the engine
  std::uint64_t min_payload_bytes{0};
  std::uint64_t max_payload_bytes{0};
  std::uint32_t alignment_bytes{1};
  std::uint32_t max_concurrency{1};
  std::uint32_t queue_capacity{0};
  std::uint32_t transport_classes{0};
  std::uint32_t payload_classes{0};
  std::uint32_t protocol_version{0};
  std::uint32_t driver_backend_version{0};
  std::uint32_t firmware_generation{0};
  std::uint32_t accelerator_arch{0};
  std::string backend_family;  ///< bounded, e.g. "cpu.crc32c", "cuda.sm120"

  void canonicalize();
  [[nodiscard]] bool supports_operation(OperationClassId id) const noexcept;
  [[nodiscard]] bool memory_domain_supported(MemoryDomain value) const noexcept;
};

/// Capability record as published by an execution domain, bound to a generation.
struct CapabilityRecord {
  ExecutionDomainId domain;
  CapabilityGeneration generation;
  EvidenceGeneration evidence;
  /// Worker incarnation that published this record. A publication from a different
  /// incarnation than the one the domain currently belongs to re-bases the
  /// generation instead of being rejected as a regression, because a replacement
  /// process has its own counter space.
  WorkerBootId publisher;
  Provenance provenance{Provenance::kUnsupported};
  /// True when the publisher proved the record by querying a real backend or by an
  /// explicitly labelled deterministic synthetic backend. False means UNKNOWN.
  bool authoritative{false};
  CapabilitySet capability;

  [[nodiscard]] bool published() const noexcept {
    return domain.valid() && generation.published() && evidence.published() && authoritative;
  }
};

/// Result of a capability check, with the exact machine-readable reason on failure.
struct CapabilityCheck {
  bool supported{false};
  IneligibilityReason reason{IneligibilityReason::kNone};
  std::string detail;
};

[[nodiscard]] CapabilityCheck check_capability(const CapabilityRecord& record,
                                               const OperationClassRegistry& registry,
                                               const OperationRequest& request,
                                               bool require_positive_evidence);

}  // namespace tos

#endif  // TOS_CORE_CAPABILITY_HPP
