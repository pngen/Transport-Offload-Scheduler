// Authority binding and worker-boot fencing.
//
// Capability, availability, locality and cost are evidence. Authority is the
// separate question of whether this exact engine generation, worker incarnation
// and coordinator epoch may perform this exact attempt right now.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#ifndef TOS_CORE_AUTHORITY_HPP
#define TOS_CORE_AUTHORITY_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "tos/core/domain.hpp"
#include "tos/core/identities.hpp"
#include "tos/core/policy.hpp"
#include "tos/core/operation.hpp"

namespace tos {

/// The exact state that made a plan legal. Every field that participated in the
/// decision is bound so that a later change invalidates the plan instead of
/// silently reinterpreting it.
struct AuthorityBinding {
  CoordinatorEpoch coordinator_epoch;
  WorkerId worker;
  WorkerBootId worker_boot;
  ExecutionDomainId domain;
  ExecutionDomainGeneration domain_generation;
  CapabilityGeneration capability_generation;
  BackendGeneration backend_generation;
  TopologyGeneration topology_generation;
  LocalityGeneration locality_generation;
  HealthGeneration health_generation;
  QueueGeneration queue_generation;
  LoadGeneration load_generation;
  PolicyGeneration policy_generation;
  CompatibilityGeneration compatibility_generation;
  IsolationPolicyGeneration isolation_generation;
  EvidenceGeneration evidence_generation;
  TransportOperationId operation;
  ExecutionAttemptId attempt;
  ExecutionAttemptGeneration attempt_generation;
  ReservationId reservation;
};

/// One field of a binding that no longer matches live authority.
struct AuthorityMismatch {
  std::string field;
  std::uint64_t bound{0};
  std::uint64_t observed{0};
  std::string detail;
};

struct AuthorityValidation {
  bool valid{false};
  std::vector<AuthorityMismatch> mismatches;
  std::string code;  ///< "ok" or a stable rejection code

  [[nodiscard]] bool has_field(std::string_view name) const;
};

/// Live authority of one execution domain as currently known to the runtime.
struct LiveAuthority {
  CoordinatorEpoch coordinator_epoch;
  WorkerId worker;
  WorkerBootId worker_boot;
  ExecutionDomainGeneration domain_generation;
  CapabilityGeneration capability_generation;
  BackendGeneration backend_generation;
  TopologyGeneration topology_generation;
  LocalityGeneration locality_generation;
  HealthGeneration health_generation;
  QueueGeneration queue_generation;
  LoadGeneration load_generation;
  PolicyGeneration policy_generation;
  CompatibilityGeneration compatibility_generation;
  IsolationPolicyGeneration isolation_generation;
  EvidenceGeneration evidence_generation;
  bool domain_fenced{false};
  bool domain_present{false};
};

/// Compare a binding with live authority under a freshness policy.
[[nodiscard]] AuthorityValidation validate_authority(const AuthorityBinding& binding,
                                                     const LiveAuthority& live,
                                                     const FreshnessRequirements& freshness);

/// Extract live authority from a domain record.
[[nodiscard]] LiveAuthority live_authority_of(const ExecutionDomainRecord& record,
                                              CoordinatorEpoch epoch,
                                              PolicyGeneration policy_generation,
                                              IsolationPolicyGeneration isolation_generation);

/// Fenced worker-boot registry. A dead worker's boot identity is never reused.
class WorkerAuthority {
 public:
  WorkerAuthority();
  ~WorkerAuthority();
  WorkerAuthority(const WorkerAuthority&) = delete;
  WorkerAuthority& operator=(const WorkerAuthority&) = delete;

  /// Register a fresh worker incarnation. Rejects fenced or already-current boots.
  [[nodiscard]] Status register_boot(WorkerId worker, WorkerBootId boot, std::string_view label);

  [[nodiscard]] bool is_current(WorkerId worker, WorkerBootId boot) const;
  [[nodiscard]] bool is_fenced(WorkerId worker, WorkerBootId boot) const;
  [[nodiscard]] bool fenced_boot_id(WorkerBootId boot) const;

  /// Fence a worker incarnation. Returns the number of domains that must be invalidated.
  [[nodiscard]] Status fence(WorkerId worker, WorkerBootId boot, std::string_view reason);
  [[nodiscard]] Status fence_boot(WorkerBootId boot, std::string_view reason);

  [[nodiscard]] WorkerBootId current_boot(WorkerId worker) const;
  [[nodiscard]] std::vector<WorkerBootId> fenced_boots() const;
  [[nodiscard]] std::vector<std::pair<WorkerId, WorkerBootId>> live_boots() const;
  [[nodiscard]] std::size_t tracked_workers() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tos

#endif  // TOS_CORE_AUTHORITY_HPP
