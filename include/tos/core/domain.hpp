// Execution domain model.
//
// An execution domain is a concrete place where transport-path work can run:
// host CPU, accelerator, NIC, SmartNIC, DPU or another explicitly registered
// offload engine. The generic model carries no vendor assumption; vendor facts
// travel in capability and compatibility evidence.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#ifndef TOS_CORE_DOMAIN_HPP
#define TOS_CORE_DOMAIN_HPP

#include <array>
#include <cstdint>
#include <string>

#include "tos/core/capability.hpp"
#include "tos/core/enums.hpp"
#include "tos/core/identities.hpp"
#include "tos/core/operation.hpp"
#include "tos/util/status.hpp"

namespace tos {

using CapacityVector = std::array<std::uint64_t, kResourceKindCount>;

/// Current, volatile operating evidence. Never authoritative after a restart.
struct DomainLoadEvidence {
  LoadGeneration load_generation;
  QueueGeneration queue_generation;
  HealthGeneration health_generation;
  EvidenceGeneration evidence;
  Provenance provenance{Provenance::kUnsupported};
  std::uint32_t utilization_percent{0};   ///< 0..100
  std::uint32_t congestion_percent{0};    ///< 0..100
  std::uint32_t queue_depth{0};
  std::uint32_t in_flight{0};
  std::uint64_t observed_latency_ns{0};
  std::uint64_t throughput_bytes_per_second{0};
  bool healthy{false};
  bool ready{false};
  bool accepting{true};

  [[nodiscard]] bool published() const noexcept {
    return health_generation.published() && load_generation.published() && queue_generation.published();
  }
};

/// Locality evidence. Consumed from external adapters; never invented.
struct DomainLocality {
  LocalityGeneration generation;
  LocalityClass class_to_payload{LocalityClass::kUnknown};
  std::uint32_t numa_node{0};
  std::uint32_t host_index{0};
  std::uint32_t pcie_root_complex{0};
  std::uint32_t pcie_switch{0};
  ExecutionDomainId local_nic;  ///< NIC on the same root complex, when known
  bool has_local_nic{false};
};

/// Topology evidence: which host/fabric the domain belongs to.
struct DomainTopology {
  TopologyGeneration generation;
  std::string host_node;   ///< bounded stable host identity
  std::string fabric;      ///< bounded fabric identity, may be empty
  std::uint32_t rack{0};
  std::uint32_t pod{0};
};

/// Compatibility-relevant version facts about a domain.
struct DomainCompatibility {
  CompatibilityGeneration generation;
  std::uint32_t driver_backend_version{0};
  std::uint32_t firmware_generation{0};
  std::uint32_t protocol_version{0};
  std::uint32_t accelerator_arch{0};
  std::string backend_family;
};

/// Highest publication sequence consumed from one publisher incarnation.
///
/// Authoritative generations are owned by the coordinator and always advance.
/// These watermarks record how far the publisher's own stream has been consumed, so
/// a duplicate or reordered publication is rejected while a replacement process —
/// whose counters restart — begins a fresh stream.
struct PublisherSequence {
  WorkerBootId boot;
  std::uint64_t capability{0};
  std::uint64_t load{0};
  std::uint64_t queue{0};
  std::uint64_t health{0};
  std::uint64_t locality{0};
  std::uint64_t topology{0};
  std::uint64_t compatibility{0};
  std::uint64_t domain{0};
};

/// Complete record of one execution domain.
struct ExecutionDomainRecord {
  ExecutionDomainId id;
  ExecutionDomainGeneration generation;
  ExecutionDomainType type{ExecutionDomainType::kCpu};
  std::string name;           ///< stable, bounded, ASCII
  std::string parent_device;  ///< bounded stable device identity
  std::string parent_host;    ///< bounded stable host identity
  WorkerId worker;
  WorkerBootId worker_boot;
  Provenance provenance{Provenance::kUnsupported};
  std::uint64_t registration_sequence{0};  ///< deterministic tie-break input
  CapabilityRecord capability;
  DomainLoadEvidence load;
  DomainLocality locality;
  DomainTopology topology;
  DomainCompatibility compatibility;
  IsolationClass isolation{IsolationClass::kUnknown};
  CapacityVector capacity{};  ///< total governable capacity per resource kind
  BackendGeneration backend_generation;
  PublisherSequence publisher_sequence;
  bool fenced{false};
  std::string fence_reason;

  [[nodiscard]] bool is_offload_engine() const noexcept {
    return type != ExecutionDomainType::kCpu;
  }
  /// Volatile evidence is current only if it was published by the live worker boot.
  [[nodiscard]] bool dynamic_evidence_current(WorkerBootId live_boot) const noexcept {
    return load.published() && worker_boot.valid() && worker_boot == live_boot && !fenced;
  }
};

/// Validate structural invariants of a domain record before it is admitted.
[[nodiscard]] Status validate_domain_record(const ExecutionDomainRecord& record);

}  // namespace tos

#endif  // TOS_CORE_DOMAIN_HPP
