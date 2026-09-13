// Dispatch boundary types.
//
// Planning is not permission to execute. A plan becomes dispatchable only after
// revalidation against live authority, and dispatch registers the attempt before
// any external call is made.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#ifndef TOS_CORE_DISPATCH_HPP
#define TOS_CORE_DISPATCH_HPP

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "tos/core/authority.hpp"
#include "tos/core/attempt.hpp"
#include "tos/core/decision.hpp"
#include "tos/core/domain.hpp"
#include "tos/core/identities.hpp"
#include "tos/core/policy.hpp"
#include "tos/core/reservation.hpp"
#include "tos/util/status.hpp"

namespace tos {

enum class PlanState : std::uint8_t {
  kCreated = 0,    ///< eligibility, ranking and binding complete; not reserved
  kReserved = 1,   ///< capacity held
  kDispatched = 2, ///< attempt registered and handed to a channel
  kConsumed = 3,   ///< a new generation superseded this plan (retry/fallback)
  kCancelled = 4,
  kRejected = 5,   ///< revalidation at dispatch rejected the plan
};

[[nodiscard]] std::string_view to_string(PlanState value) noexcept;
[[nodiscard]] bool parse_enum(std::string_view text, PlanState& out) noexcept;

/// A concrete plan of record. Holding a plan grants no execution authority.
struct ExecutionPlan {
  TransportOperationId operation;
  OperationClassId operation_class;
  std::string operation_class_name;
  ExecutionDomainId domain;
  ExecutionDomainType domain_type{ExecutionDomainType::kCpu};
  Provenance provenance{Provenance::kUnsupported};
  SideEffectClass side_effect{SideEffectClass::kUnknown};
  AuthorityBinding binding;
  ResourceAmounts reserved_amounts{};
  ReservationId reservation;
  ExecutionAttemptId attempt;
  ExecutionAttemptGeneration attempt_generation;
  DispatchId dispatch;
  bool fallback{false};
  std::uint32_t fallback_depth{0};
  ExecutionDomainId fallback_from;
  std::uint32_t retry_index{0};
  PlanState state{PlanState::kCreated};
  std::uint64_t plan_sequence{0};
  /// The operation request as planned. Retry and fallback re-run eligibility from
  /// this record, so a plan is a complete statement of what was decided.
  OperationRequest request;
  /// Payload bytes carried with the plan, bounded by the scheduler limits. The
  /// public API never dereferences opaque payload handles.
  std::vector<std::uint8_t> payload;
  DecisionExplanation explanation;
};

struct PlanResult {
  bool planned{false};
  Status status;
  ExecutionPlan plan;
  DecisionExplanation explanation;
};

struct DispatchResult {
  bool dispatched{false};
  DispatchRejection rejection{DispatchRejection::kNone};
  Status status;
  ExecutionAttemptId attempt;
  DispatchId dispatch;
  AttemptState state{AttemptState::kPlanned};
};

struct DispatchOutcome {
  PlanResult plan_result;
  DispatchResult dispatch_result;
};

struct RetryResult {
  bool retry_permitted{false};
  RetryRejection rejection{RetryRejection::kNone};
  Status status;
  ExecutionAttemptId attempt;
  ExecutionAttemptGeneration generation;
  ExecutionPlan plan;
};

struct FallbackResult {
  bool fallback_selected{false};
  FallbackRejection rejection{FallbackRejection::kNone};
  Status status;
  ExecutionPlan plan;
  DecisionExplanation explanation;
  std::vector<ExecutionDomainType> chain_tried;
};

struct CancellationResult {
  bool cancelled{false};
  bool ambiguous{false};
  AttemptState state{AttemptState::kPlanned};
  AttemptResolution resolution{AttemptResolution::kNone};
  Status status;
};

/// Envelope handed to a dispatch channel. A channel reports transport acceptance,
/// never completion.
struct DispatchEnvelope {
  ExecutionAttemptId attempt;
  ExecutionAttemptGeneration attempt_generation;
  DispatchId dispatch;
  CoordinatorEpoch coordinator_epoch;
  WorkerId worker;
  WorkerBootId worker_boot;
  ExecutionDomainId domain;
  ExecutionDomainGeneration domain_generation;
  CapabilityGeneration capability_generation;
  PolicyGeneration policy_generation;
  TransportOperationId operation;
  OperationClassId operation_class;
  SideEffectClass side_effect{SideEffectClass::kUnknown};
  Provenance provenance{Provenance::kUnsupported};
  PayloadDescriptor payload;
  std::vector<std::uint8_t> bytes;
  bool fallback{false};
  std::uint32_t fallback_depth{0};
  std::uint32_t retry_index{0};
};

struct CancelEnvelope {
  ExecutionAttemptId attempt;
  ExecutionAttemptGeneration attempt_generation;
  DispatchId dispatch;
  CoordinatorEpoch coordinator_epoch;
  WorkerBootId worker_boot;
  ExecutionDomainId domain;
  bool may_have_executed{false};
};

/// A dispatch channel moves envelopes to executors. Implementations must not block
/// while any scheduler lock is held, and must not call back reentrantly from
/// inside send().
class IDispatchChannel {
 public:
  virtual ~IDispatchChannel() = default;
  [[nodiscard]] virtual std::string_view name() const noexcept = 0;
  [[nodiscard]] virtual Status send(const DispatchEnvelope& envelope) = 0;
  virtual bool request_cancel(const CancelEnvelope& envelope) = 0;
  virtual void shutdown() = 0;
};

}  // namespace tos

#endif  // TOS_CORE_DISPATCH_HPP
