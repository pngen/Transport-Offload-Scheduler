// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "tos/core/policy.hpp"

#include <algorithm>

namespace tos {
namespace {

bool reaches(std::array<int, kExecutionDomainTypeCount>& state, const FallbackPolicy& policy,
             int node) noexcept {
  // 0 = unvisited, 1 = on stack, 2 = done
  if (state[static_cast<std::size_t>(node)] == 1) return true;
  if (state[static_cast<std::size_t>(node)] == 2) return false;
  state[static_cast<std::size_t>(node)] = 1;
  for (ExecutionDomainType next :
       policy.chains[static_cast<std::size_t>(node)]) {
    if (reaches(state, policy, static_cast<int>(next))) return true;
  }
  state[static_cast<std::size_t>(node)] = 2;
  return false;
}

}  // namespace

bool SchedulerPolicy::domain_type_forbidden(ExecutionDomainType type) const noexcept {
  return std::find(forbidden_domains.begin(), forbidden_domains.end(), type) !=
         forbidden_domains.end();
}

bool SchedulerPolicy::domain_id_forbidden(ExecutionDomainId id) const noexcept {
  return std::find(forbidden_domain_ids.begin(), forbidden_domain_ids.end(), id) !=
         forbidden_domain_ids.end();
}

bool SchedulerPolicy::provenance_forbidden(Provenance value) const noexcept {
  return std::find(forbidden_provenance.begin(), forbidden_provenance.end(), value) !=
         forbidden_provenance.end();
}

const std::vector<ExecutionDomainType>& SchedulerPolicy::fallback_chain(
    ExecutionDomainType from) const noexcept {
  static const std::vector<ExecutionDomainType> kEmpty;
  const auto index = static_cast<std::size_t>(from);
  if (index >= kExecutionDomainTypeCount) return kEmpty;
  return fallback.chains[index];
}

bool fallback_graph_has_cycle(const FallbackPolicy& policy) noexcept {
  for (int node = 0; node < kExecutionDomainTypeCount; ++node) {
    std::array<int, kExecutionDomainTypeCount> state{};
    if (reaches(state, policy, node)) return true;
  }
  return false;
}

SchedulerPolicy make_default_policy() {
  SchedulerPolicy policy;
  policy.generation = PolicyGeneration::first();
  policy.isolation_generation = IsolationPolicyGeneration::first();
  policy.compatibility_generation = CompatibilityGeneration::first();
  policy.offload_requirement = OffloadRequirement::kAny;
  policy.require_positive_evidence = true;
  policy.minimum_isolation = IsolationClass::kUnknown;
  policy.weights = default_ranking_weights();
  policy.preference_order = {ExecutionDomainType::kDpu, ExecutionDomainType::kSmartNic,
                             ExecutionDomainType::kNic, ExecutionDomainType::kAccelerator,
                             ExecutionDomainType::kCpu,
                             ExecutionDomainType::kOtherRegisteredOffloadEngine};
  policy.fallback.enabled = true;
  policy.fallback.max_depth = 3;
  policy.fallback.allow_host_fallback = true;
  policy.fallback.chains[static_cast<std::size_t>(ExecutionDomainType::kDpu)] = {
      ExecutionDomainType::kSmartNic, ExecutionDomainType::kNic,
      ExecutionDomainType::kAccelerator, ExecutionDomainType::kCpu};
  policy.fallback.chains[static_cast<std::size_t>(ExecutionDomainType::kSmartNic)] = {
      ExecutionDomainType::kNic, ExecutionDomainType::kAccelerator, ExecutionDomainType::kCpu};
  policy.fallback.chains[static_cast<std::size_t>(ExecutionDomainType::kNic)] = {
      ExecutionDomainType::kAccelerator, ExecutionDomainType::kCpu};
  policy.fallback.chains[static_cast<std::size_t>(ExecutionDomainType::kAccelerator)] = {
      ExecutionDomainType::kCpu};
  policy.fallback.chains[static_cast<std::size_t>(
      ExecutionDomainType::kOtherRegisteredOffloadEngine)] = {ExecutionDomainType::kCpu};
  policy.retry.enabled = true;
  policy.retry.max_attempts = 2;
  policy.retry.max_ambiguous_retries = 0;
  policy.retry.rules.retryable[static_cast<std::size_t>(FailureKind::kTransportFailure)] = true;
  policy.retry.rules.retryable[static_cast<std::size_t>(FailureKind::kPreDispatchRejection)] = true;
  policy.retry.rules.retryable[static_cast<std::size_t>(FailureKind::kWorkerDeath)] = true;
  policy.retry.rules.retryable[static_cast<std::size_t>(FailureKind::kDomainUnavailable)] = true;
  policy.retry.rules.retryable[static_cast<std::size_t>(FailureKind::kCapacityExhausted)] = true;
  policy.retry.rules.retryable[static_cast<std::size_t>(FailureKind::kCoordinatorRestart)] = true;
  policy.retry.rules.retryable[static_cast<std::size_t>(FailureKind::kPostDispatchRejection)] = false;
  policy.retry.rules.retryable[static_cast<std::size_t>(FailureKind::kAmbiguousOutcome)] = false;
  policy.retry.rules.retryable[static_cast<std::size_t>(FailureKind::kIntegrityFailure)] = false;
  policy.retry.rules.retryable[static_cast<std::size_t>(FailureKind::kExecutionFailure)] = false;
  policy.retry.rules.retryable[static_cast<std::size_t>(FailureKind::kCancelled)] = false;
  policy.retry.rules.retryable[static_cast<std::size_t>(FailureKind::kBackendRejection)] = true;
  policy.freshness = FreshnessRequirements{};
  policy.reservation.enabled = false;
  policy.reservation.require_reservation_for_offload_only = true;
  // Default economics: host CPU is the cheapest to start and the most expensive to
  // occupy; offload engines cost more to configure and less per byte. Deployments
  // replace these with measured numbers.
  policy.type_setup_cost_units[static_cast<std::size_t>(ExecutionDomainType::kCpu)] = 1;
  policy.type_setup_cost_units[static_cast<std::size_t>(ExecutionDomainType::kAccelerator)] = 120;
  policy.type_setup_cost_units[static_cast<std::size_t>(ExecutionDomainType::kNic)] = 60;
  policy.type_setup_cost_units[static_cast<std::size_t>(ExecutionDomainType::kSmartNic)] = 80;
  policy.type_setup_cost_units[static_cast<std::size_t>(ExecutionDomainType::kDpu)] = 100;
  policy.type_setup_cost_units[static_cast<std::size_t>(
      ExecutionDomainType::kOtherRegisteredOffloadEngine)] = 70;
  policy.type_per_byte_cost_micro_units[static_cast<std::size_t>(ExecutionDomainType::kCpu)] = 40;
  policy.type_per_byte_cost_micro_units[static_cast<std::size_t>(ExecutionDomainType::kAccelerator)] = 8;
  policy.type_per_byte_cost_micro_units[static_cast<std::size_t>(ExecutionDomainType::kNic)] = 12;
  policy.type_per_byte_cost_micro_units[static_cast<std::size_t>(ExecutionDomainType::kSmartNic)] = 10;
  policy.type_per_byte_cost_micro_units[static_cast<std::size_t>(ExecutionDomainType::kDpu)] = 6;
  policy.type_per_byte_cost_micro_units[static_cast<std::size_t>(
      ExecutionDomainType::kOtherRegisteredOffloadEngine)] = 18;
  return policy;
}

Status validate_policy(const SchedulerPolicy& policy) {
  if (!policy.generation.published()) return Status::failure("policy.missing_generation");
  if (policy.forbidden_domains.size() > static_cast<std::size_t>(kExecutionDomainTypeCount)) {
    return Status::failure("policy.too_many_forbidden_domains");
  }
  if (policy.preference_order.size() > static_cast<std::size_t>(kExecutionDomainTypeCount)) {
    return Status::failure("policy.preference_order_too_long");
  }
  for (std::size_t i = 0; i < kRankingFactorCount; ++i) {
    const std::int32_t weight = policy.weights[i];
    if (weight < 0 || weight > 100000) return Status::failure("policy.weight_out_of_range");
  }
  if (policy.retry.max_attempts == 0 || policy.retry.max_attempts > kMaxRetryAttempts) {
    return Status::failure("policy.retry_bound_invalid");
  }
  if (policy.fallback.max_depth > kMaxFallbackDepth) {
    return Status::failure("policy.fallback_depth_invalid");
  }
  if (policy.fallback.enabled != (policy.fallback.max_depth > 0)) {
    // enabled with zero depth or depth without enable is a configuration error
    if (policy.fallback.enabled) return Status::failure("policy.fallback_depth_invalid");
  }
  if (fallback_graph_has_cycle(policy.fallback)) {
    return Status::failure("policy.fallback_cycle");
  }
  for (const std::vector<ExecutionDomainType>& chain : policy.fallback.chains) {
    if (chain.size() > static_cast<std::size_t>(kExecutionDomainTypeCount)) {
      return Status::failure("policy.fallback_chain_too_long");
    }
  }
  for (std::size_t i = 0; i < kExecutionDomainTypeCount; ++i) {
    if (policy.type_setup_cost_units[i] > (1ULL << 32)) {
      return Status::failure("policy.setup_cost_out_of_range");
    }
    if (policy.type_per_byte_cost_micro_units[i] > 1000000ULL) {
      return Status::failure("policy.per_byte_cost_out_of_range");
    }
  }
  if (policy.max_reported_candidates == 0 ||
      policy.max_reported_candidates > kMaxReportedCandidates) {
    return Status::failure("policy.candidate_report_bound_invalid");
  }
  if (policy.reservation.enabled) {
    for (std::size_t i = 0; i < kResourceKindCount; ++i) {
      if (policy.reservation.per_operation[i] > (1ULL << 32)) {
        return Status::failure("policy.reservation_amount_out_of_range");
      }
    }
  }
  if (policy.offload_requirement == OffloadRequirement::kHostRequired &&
      policy.domain_type_forbidden(ExecutionDomainType::kCpu)) {
    return Status::failure("policy.host_required_but_cpu_forbidden");
  }
  return Status::success();
}

}  // namespace tos
