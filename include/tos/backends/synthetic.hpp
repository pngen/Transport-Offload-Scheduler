// Deterministic synthetic backend.
//
// Synthetic execution traverses exactly the same planner, authority, reservation,
// dispatch and completion paths as real execution. It exists so that domain
// classes that are not present on this host (SmartNIC, DPU, offload engines) can be
// exercised honestly: every record it publishes is labelled SYNTHETIC and no
// hardware claim is made.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#ifndef TOS_BACKENDS_SYNTHETIC_HPP
#define TOS_BACKENDS_SYNTHETIC_HPP

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "tos/backends/backend.hpp"
#include "tos/core/domain.hpp"

namespace tos {

/// Faults the synthetic backend can apply, so that boundary behaviour is provable
/// without hardware. Faults are applied deterministically to the next N executions.
enum class SyntheticFault : std::uint8_t {
  kNone = 0,
  kExecutionFailure = 1,   ///< reports a definitive execution failure
  kAmbiguousOutcome = 2,   ///< reports that the effect may have happened
  kIntegrityFailure = 3,   ///< reports an integrity failure
  kRefuseExecution = 4,    ///< refuses to execute at all
  kReserveRefused = 5,     ///< refuses the capacity handshake
};

[[nodiscard]] std::string_view to_string(SyntheticFault value) noexcept;

struct SyntheticDomainConfig {
  ExecutionDomainId id;
  ExecutionDomainType type{ExecutionDomainType::kDpu};
  std::string name;
  std::string parent_device;
  std::string parent_host;
  Provenance provenance{Provenance::kSynthetic};
  ExecutionDomainGeneration domain_generation;
  CapabilityGeneration capability_generation;
  EvidenceGeneration evidence_generation;
  BackendGeneration backend_generation;
  WorkerId worker;
  WorkerBootId worker_boot;
  IsolationClass isolation{IsolationClass::kSeparateExecutionContext};
  CapacityVector capacity{};
  DomainLocality locality{};
  DomainTopology topology{};
  DomainCompatibility compatibility{};
  CapabilitySet capability{};
  std::uint32_t utilization_percent{0};
  std::uint32_t congestion_percent{0};
  std::uint32_t queue_depth{0};
  std::uint32_t in_flight{0};
  std::uint64_t observed_latency_ns{0};
  std::uint64_t throughput_bytes_per_second{0};
  bool healthy{true};
  bool ready{true};
  bool accepting{true};
  bool authoritative{true};
  bool dynamic_evidence_published{true};
};

class SyntheticBackend : public IExecutionBackend {
 public:
  explicit SyntheticBackend(std::string name = "synthetic");
  ~SyntheticBackend() override;

  [[nodiscard]] std::string_view name() const noexcept override { return name_; }
  [[nodiscard]] Provenance provenance() const noexcept override { return Provenance::kSynthetic; }
  [[nodiscard]] BackendGeneration generation() const override;

  Status add_domain(SyntheticDomainConfig config);
  Status remove_domain(ExecutionDomainId domain);
  [[nodiscard]] std::size_t domain_count() const;

  /// Capability publication always advances the capability and evidence generations:
  /// a plan bound to the previous generation is invalidated.
  Status set_capability(ExecutionDomainId domain, CapabilitySet capability, bool authoritative);
  Status set_operation_support(ExecutionDomainId domain, OperationClassId operation, bool supported);
  /// Load publication advances load, queue and health generations.
  Status set_load(ExecutionDomainId domain, std::uint32_t utilization_percent,
                  std::uint32_t congestion_percent, std::uint32_t queue_depth,
                  std::uint32_t in_flight, std::uint64_t observed_latency_ns);
  Status set_health(ExecutionDomainId domain, bool healthy, bool ready, bool accepting);
  Status set_locality(ExecutionDomainId domain, const DomainLocality& locality);
  Status set_topology(ExecutionDomainId domain, const DomainTopology& topology);
  Status set_compatibility(ExecutionDomainId domain, const DomainCompatibility& compatibility);
  Status set_capacity(ExecutionDomainId domain, const CapacityVector& capacity);
  Status set_worker(ExecutionDomainId domain, WorkerId worker, WorkerBootId boot);
  Status set_domain_generation(ExecutionDomainId domain, ExecutionDomainGeneration generation);
  Status set_backend_generation(ExecutionDomainId domain, BackendGeneration generation);
  /// Withhold dynamic evidence, as a restart would.
  Status withdraw_dynamic_evidence(ExecutionDomainId domain);

  void set_fault(SyntheticFault fault, std::uint32_t count = 1);
  void clear_faults();
  [[nodiscard]] std::uint64_t executed_count() const noexcept { return executed_; }

  [[nodiscard]] std::vector<ExecutionDomainRecord> discover_domains() override;
  [[nodiscard]] Checked<CapabilityRecord> query_capability(ExecutionDomainId domain) override;
  [[nodiscard]] Checked<DomainLoadEvidence> query_load(ExecutionDomainId domain) override;
  [[nodiscard]] Status reserve(ExecutionDomainId domain, ReservationId id,
                               const ResourceAmounts& amounts) override;
  [[nodiscard]] Status release(ExecutionDomainId domain, ReservationId id) override;
  [[nodiscard]] ExecutionOutcome execute(const ExecutionInvocation& invocation) override;
  [[nodiscard]] bool supports_cancel() const noexcept override { return true; }
  [[nodiscard]] Status cancel(ExecutionAttemptId id) override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::string name_;
  std::uint64_t executed_{0};
};

/// Build a synthetic domain configuration with sane, explicit defaults for a domain
/// class that is not physically present. Nothing here claims measured hardware data.
[[nodiscard]] SyntheticDomainConfig make_synthetic_domain(ExecutionDomainId id,
                                                          ExecutionDomainType type,
                                                          std::string name,
                                                          bool authoritative = true);

}  // namespace tos

#endif  // TOS_BACKENDS_SYNTHETIC_HPP
