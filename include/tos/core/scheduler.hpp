// Transport Offload Scheduler: the public runtime facade.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#ifndef TOS_CORE_SCHEDULER_HPP
#define TOS_CORE_SCHEDULER_HPP

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "tos/backends/backend.hpp"
#include "tos/core/attempt.hpp"
#include "tos/core/authority.hpp"
#include "tos/core/dispatch.hpp"
#include "tos/core/domain.hpp"
#include "tos/core/hooks.hpp"
#include "tos/core/operation.hpp"
#include "tos/core/policy.hpp"
#include "tos/core/reconcile.hpp"
#include "tos/core/reservation.hpp"
#include "tos/core/snapshot.hpp"
#include "tos/core/state.hpp"
#include "tos/util/status.hpp"

namespace tos {

/// Hard resource bounds. Nothing in the runtime grows without one of these.
struct SchedulerLimits {
  std::size_t max_domains{kMaxExecutionDomains};
  std::size_t max_operation_classes{kMaxOperationClasses};
  std::size_t max_pending_plans{65536};
  std::size_t max_outstanding_reservations{kMaxOutstandingReservations};
  std::size_t max_live_attempts{kMaxLiveAttempts};
  std::size_t max_historical_attempts{kMaxHistoricalAttempts};
  std::size_t max_reported_candidates{kMaxReportedCandidates};
  std::size_t max_snapshot_history{16};
  std::size_t worker_threads{4};
  std::size_t max_payload_dispatch_bytes{kMaxDispatchPayloadBytes};
  std::size_t max_frame_bytes{1024 * 1024};
  std::uint32_t max_fallback_depth{kMaxFallbackDepth};
  std::uint32_t max_retry_attempts{kMaxRetryAttempts};
  std::size_t max_fenced_boots{4096};
};

struct SchedulerOptions {
  SchedulerPolicy policy;
  SchedulerLimits limits;
  CoordinatorEpoch initial_epoch;
  std::string host_node;   ///< stable identity of this host, bounded
  std::string state_path;  ///< empty disables persistence
  bool persist_on_mutation{false};
  bool enable_persistence{false};
  bool recover_on_start{true};
  Provenance local_provenance{Provenance::kReal};
};

/// The scheduler facade. All public methods are safe to call concurrently except
/// where documented, and none of them hold internal locks across I/O.
class Scheduler {
 public:
  explicit Scheduler(SchedulerOptions options);
  ~Scheduler();
  Scheduler(const Scheduler&) = delete;
  Scheduler& operator=(const Scheduler&) = delete;

  // ---- lifecycle -----------------------------------------------------------
  [[nodiscard]] Status start();
  [[nodiscard]] Status shutdown();
  [[nodiscard]] bool running() const noexcept;
  [[nodiscard]] CoordinatorEpoch coordinator_epoch() const noexcept;

  // ---- configuration -------------------------------------------------------
  [[nodiscard]] const OperationClassRegistry& operation_classes() const noexcept;
  [[nodiscard]] Checked<OperationClassId> register_operation_class(OperationClassDescriptor descriptor);

  [[nodiscard]] SchedulerPolicy policy() const;
  [[nodiscard]] Status set_policy(SchedulerPolicy policy);
  [[nodiscard]] PolicyGeneration policy_generation() const noexcept;

  [[nodiscard]] const SchedulerLimits& limits() const noexcept;
  void set_hook(IInterleavingHook* hook) noexcept;
  void set_dispatch_channel(std::shared_ptr<IDispatchChannel> channel);

  // ---- execution domain publication ---------------------------------------
  [[nodiscard]] Status register_domain(const ExecutionDomainRecord& record);
  [[nodiscard]] Checked<DomainUpdateResult> update_domain(const ExecutionDomainRecord& record);
  [[nodiscard]] Status publish_capability(const CapabilityRecord& capability);
  [[nodiscard]] Status publish_load(ExecutionDomainId domain, const DomainLoadEvidence& load);
  [[nodiscard]] Status publish_locality(ExecutionDomainId domain, const DomainLocality& locality);
  [[nodiscard]] Status publish_topology(ExecutionDomainId domain, const DomainTopology& topology);
  [[nodiscard]] Status publish_compatibility(ExecutionDomainId domain,
                                             const DomainCompatibility& compatibility);
  [[nodiscard]] Status publish_capacity(ExecutionDomainId domain, const CapacityVector& capacity);
  /// Withdraw a domain's volatile evidence. The domain becomes ineligible for new
  /// plans until fresh evidence is published, and existing plans are invalidated.
  [[nodiscard]] Status withdraw_evidence(ExecutionDomainId domain, std::string_view reason);
  [[nodiscard]] Status fence_domain(ExecutionDomainId domain, std::string_view reason);
  [[nodiscard]] Status fence_worker_boot(WorkerBootId boot, std::string_view reason);
  [[nodiscard]] Status remove_domain(ExecutionDomainId domain);

  [[nodiscard]] Status register_worker_boot(WorkerId worker, WorkerBootId boot,
                                            std::string_view label = {});

  // ---- planning ------------------------------------------------------------
  /// Evaluate eligibility and ranking, then create a plan of record. Planning
  /// grants no execution authority. The optional payload is copied into the plan
  /// under the configured bound.
  [[nodiscard]] PlanResult plan(const OperationRequest& request,
                                std::span<const std::uint8_t> payload = {});
  [[nodiscard]] Status reserve(ExecutionPlan& plan);
  [[nodiscard]] DispatchResult dispatch(ExecutionPlan& plan);
  [[nodiscard]] DispatchOutcome plan_reserve_dispatch(const OperationRequest& request,
                                                      std::span<const std::uint8_t> payload = {});

  // ---- execution boundary --------------------------------------------------
  [[nodiscard]] CompletionOutcome complete(const CompletionSubmission& submission);
  [[nodiscard]] Status report_failure(ExecutionAttemptId attempt, FailureKind kind,
                                      std::string_view detail);
  [[nodiscard]] RetryResult retry(ExecutionAttemptId attempt);
  [[nodiscard]] FallbackResult fallback(const ExecutionPlan& plan, FailureKind trigger);
  [[nodiscard]] CancellationResult cancel(ExecutionAttemptId attempt);

  // ---- inspection ----------------------------------------------------------
  [[nodiscard]] SchedulerSnapshot snapshot() const;
  [[nodiscard]] ReconciliationReport reconcile() const;

  // ---- persistence ---------------------------------------------------------
  [[nodiscard]] Status save_state();
  [[nodiscard]] Status save_state_to(std::string_view path);
  [[nodiscard]] Status load_state();
  [[nodiscard]] Status load_state_from(std::string_view path);

  // ---- component access (inspection and integration) -----------------------
  [[nodiscard]] DomainRegistry& domains() noexcept;
  [[nodiscard]] ReservationLedger& reservations() noexcept;
  [[nodiscard]] AttemptLedger& attempts() noexcept;
  [[nodiscard]] WorkerAuthority& workers() noexcept;

  /// Channel used for dispatch, if any. Empty means dispatch returns
  /// DispatchRejection::kTransportFailure with code "channel.absent".
  [[nodiscard]] std::shared_ptr<IDispatchChannel> dispatch_channel() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tos

#endif  // TOS_CORE_SCHEDULER_HPP
