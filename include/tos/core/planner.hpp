// Deterministic factor computation used by ranking.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#ifndef TOS_CORE_PLANNER_HPP
#define TOS_CORE_PLANNER_HPP

#include <cstdint>
#include <string>

#include "tos/core/domain.hpp"
#include "tos/core/operation.hpp"
#include "tos/core/policy.hpp"
#include "tos/core/ranking.hpp"

namespace tos {

/// Inputs needed to score one eligible candidate.
struct FactorInputs {
  const ExecutionDomainRecord* domain{nullptr};
  const CapabilitySet* capability{nullptr};
  const OperationRequest* request{nullptr};
  const SchedulerPolicy* policy{nullptr};
  /// Domain that failed and triggered a fallback, when ranking fallback targets.
  const ExecutionDomainRecord* previous{nullptr};
  std::uint64_t estimated_transfer_bytes{0};
  std::uint64_t estimated_setup_cost_units{0};
  std::uint64_t total_setup_cost_units{0};
  std::uint32_t fallback_depth{0};
};

/// Compute the named factor vector for one candidate. Pure function of its inputs:
/// identical inputs always produce identical output.
[[nodiscard]] FactorVector compute_factors(const FactorInputs& inputs);

/// Normalize a value to the factor scale where smaller is better.
[[nodiscard]] std::int32_t normalize_lower_is_better(std::uint64_t value, std::uint64_t worst) noexcept;
/// Normalize a value to the factor scale where larger is better.
[[nodiscard]] std::int32_t normalize_higher_is_better(std::uint64_t value, std::uint64_t best) noexcept;

/// Human-readable summary of the factor vector for explanations and the CLI.
[[nodiscard]] std::string describe_factors(const FactorVector& factors);

}  // namespace tos

#endif  // TOS_CORE_PLANNER_HPP
