// Reconciliation between recovered durable state and live execution domains.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#ifndef TOS_CORE_RECONCILE_HPP
#define TOS_CORE_RECONCILE_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "tos/core/enums.hpp"
#include "tos/core/identities.hpp"

namespace tos {

struct ReconciliationItem {
  DiscrepancyKind kind{DiscrepancyKind::kDomainMissing};
  ExecutionDomainId domain;
  std::string domain_name;
  ExecutionDomainGeneration expected_generation;
  ExecutionDomainGeneration observed_generation;
  WorkerBootId expected_boot;
  WorkerBootId observed_boot;
  ExecutionAttemptId attempt;
  std::string detail;
};

struct ReconciliationReport {
  CoordinatorEpoch coordinator_epoch;
  SnapshotGeneration snapshot;
  std::uint64_t domains_compared{0};
  std::uint64_t domains_matched{0};
  std::uint64_t discrepancies{0};
  std::uint64_t attempts_classified{0};
  bool conservative{true};
  std::vector<ReconciliationItem> items;

  [[nodiscard]] std::uint64_t count_of(DiscrepancyKind kind) const noexcept;
  [[nodiscard]] std::string render_text() const;
};

}  // namespace tos

#endif  // TOS_CORE_RECONCILE_HPP
