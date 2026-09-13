// Scheduler policy: weights, hard restrictions, retry, fallback and freshness.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#ifndef TOS_CORE_POLICY_HPP
#define TOS_CORE_POLICY_HPP

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "tos/core/enums.hpp"
#include "tos/core/identities.hpp"
#include "tos/core/operation.hpp"
#include "tos/core/ranking.hpp"
#include "tos/util/status.hpp"

namespace tos {

/// Bounds on policy-driven behaviour.
inline constexpr std::uint32_t kMaxRetryAttempts = 8;
inline constexpr std::uint32_t kMaxFallbackDepth = 8;

/// Structured failure classes at the scheduler boundary, used by retry policy.
enum class FailureKind : std::uint8_t {
  kNone = 0,
  kTransportFailure = 1,       ///< dispatch channel failed before the worker accepted
  kPreDispatchRejection = 2,   ///< worker refused before executing
  kPostDispatchRejection = 3,  ///< worker refused after accepting
  kWorkerDeath = 4,            ///< worker process died
  kDomainUnavailable = 5,
  kCapacityExhausted = 6,
  kBackendRejection = 7,       ///< backend refused to execute
  kIntegrityFailure = 8,       ///< result failed integrity verification
  kExecutionFailure = 9,       ///< backend reported a failure with a definitive outcome
  kAmbiguousOutcome = 10,      ///< effect may or may not have happened
  kCancelled = 11,
  kCoordinatorRestart = 12,
};
inline constexpr int kFailureKindCount = 13;

[[nodiscard]] std::string_view to_string(FailureKind value) noexcept;
[[nodiscard]] bool parse_enum(std::string_view text, FailureKind& out) noexcept;

/// Which generation families must be current for a plan to remain dispatchable.
struct FreshnessRequirements {
  bool capability{true};
  bool health{true};
  bool queue{false};
  bool load{false};
  bool topology{false};
  bool locality{false};
  bool compatibility{true};
  bool isolation{false};
  bool evidence{true};
};

/// Which failure kinds are retryable when the side-effect class permits it.
struct FailureRetryRule {
  std::array<bool, kFailureKindCount> retryable{};
  [[nodiscard]] bool allows(FailureKind kind) const noexcept {
    return retryable[static_cast<std::size_t>(kind)];
  }
};

struct RetryPolicy {
  bool enabled{false};
  std::uint32_t max_attempts{1};  ///< total attempts including the first, bounded
  FailureRetryRule rules{};
  std::uint32_t max_ambiguous_retries{0};  ///< normally 0: never replay ambiguity blindly
};

struct FallbackPolicy {
  bool enabled{false};
  std::uint32_t max_depth{0};
  /// Per-source ordered fallback chain. The graph must be acyclic; cycles are rejected.
  std::array<std::vector<ExecutionDomainType>, kExecutionDomainTypeCount> chains{};
  bool allow_host_fallback{true};
};

/// Reservation requirement: which resource kinds must be reserved before dispatch.
struct ReservationPolicy {
  bool enabled{false};
  std::array<std::uint64_t, kResourceKindCount> per_operation{};
  bool require_reservation_for_offload_only{true};
};

/// Complete, generation-bound scheduling policy.
struct SchedulerPolicy {
  PolicyGeneration generation;
  IsolationPolicyGeneration isolation_generation;
  CompatibilityGeneration compatibility_generation;

  OffloadRequirement offload_requirement{OffloadRequirement::kAny};
  IsolationClass minimum_isolation{IsolationClass::kUnknown};
  bool require_positive_evidence{true};
  bool allow_unsupported_provenance{false};  ///< UNSUPPORTED domains are never eligible when false

  std::vector<ExecutionDomainType> forbidden_domains;
  std::vector<ExecutionDomainId> forbidden_domain_ids;
  std::vector<Provenance> forbidden_provenance;

  std::array<std::int32_t, kRankingFactorCount> weights{default_ranking_weights()};
  std::vector<ExecutionDomainType> preference_order;  ///< strongest-first, feeds kPolicyPreference

  ReservationPolicy reservation;
  RetryPolicy retry;
  FallbackPolicy fallback;
  FreshnessRequirements freshness;

  /// Policy-supplied execution economics per domain class. These are policy inputs,
  /// not invented hardware facts: a deployment that has measured its own setup and
  /// per-byte costs publishes them here.
  std::array<std::uint64_t, kExecutionDomainTypeCount> type_setup_cost_units{};
  std::array<std::uint64_t, kExecutionDomainTypeCount> type_per_byte_cost_micro_units{};

  std::uint64_t maximum_cost_units{0};  ///< 0 = unlimited
  std::size_t max_reported_candidates{kMaxReportedCandidates};

  [[nodiscard]] bool domain_type_forbidden(ExecutionDomainType type) const noexcept;
  [[nodiscard]] bool domain_id_forbidden(ExecutionDomainId id) const noexcept;
  [[nodiscard]] bool provenance_forbidden(Provenance value) const noexcept;
  [[nodiscard]] const std::vector<ExecutionDomainType>& fallback_chain(ExecutionDomainType from) const noexcept;
};

/// A policy with bounded, documented defaults: any operation/domain mix, positive
/// evidence required, offload chains registered, replay restricted to replay-safe
/// side-effect classes.
[[nodiscard]] SchedulerPolicy make_default_policy();

/// Validate policy structure: bounds, no fallback cycles, consistent retry settings.
[[nodiscard]] Status validate_policy(const SchedulerPolicy& policy);

/// Detect a cycle in the fallback graph. Exposed so tests can attack the validator.
[[nodiscard]] bool fallback_graph_has_cycle(const FallbackPolicy& policy) noexcept;

}  // namespace tos

#endif  // TOS_CORE_POLICY_HPP
