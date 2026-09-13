// Read-only runtime snapshot used by inspection tooling and tests.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#ifndef TOS_CORE_SNAPSHOT_HPP
#define TOS_CORE_SNAPSHOT_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "tos/core/attempt.hpp"
#include "tos/core/domain.hpp"
#include "tos/core/identities.hpp"
#include "tos/core/policy.hpp"
#include "tos/core/reconcile.hpp"
#include "tos/core/reservation.hpp"

namespace tos {

struct WorkerBootView {
  WorkerId worker;
  WorkerBootId boot;
  bool current{false};
  bool fenced{false};
};

struct SnapshotTotals {
  std::uint64_t domains{0};
  std::uint64_t domains_fenced{0};
  std::uint64_t domains_with_current_evidence{0};
  std::uint64_t live_attempts{0};
  std::uint64_t completed_attempts{0};
  std::uint64_t ambiguous_attempts{0};
  std::uint64_t fenced_attempts{0};
  std::uint64_t outstanding_reservations{0};
  std::uint64_t plans_created{0};
  std::uint64_t plans_dispatched{0};
  std::uint64_t dispatch_rejections{0};
  std::uint64_t completion_rejections{0};
  std::uint64_t fallbacks{0};
  std::uint64_t retries{0};
};

/// A point-in-time, self-consistent copy of runtime state. Rendering never
/// re-enters runtime state, so a snapshot can be produced while the runtime is
/// mutating without deadlock.
struct SchedulerSnapshot {
  SnapshotGeneration generation;
  CoordinatorEpoch coordinator_epoch;
  PolicyGeneration policy_generation;
  bool running{false};
  std::uint64_t mutation_sequence{0};
  std::vector<ExecutionDomainRecord> domains;
  std::vector<WorkerBootView> workers;
  std::vector<Reservation> reservations;
  std::vector<ExecutionAttempt> attempts;
  std::vector<ReconciliationReport> reconciliations;
  SnapshotTotals totals;
  SchedulerPolicy policy_summary;

  [[nodiscard]] std::string render_text() const;
  [[nodiscard]] std::string render_json() const;
  [[nodiscard]] const ExecutionDomainRecord* find_domain(ExecutionDomainId id) const;
};

}  // namespace tos

#endif  // TOS_CORE_SNAPSHOT_HPP
