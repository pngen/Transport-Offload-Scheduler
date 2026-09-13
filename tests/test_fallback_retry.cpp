// Fallback, OFFLOAD_REQUIRED, retry and ambiguous-outcome semantics.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "tos_test_support.hpp"

using namespace tos;

namespace {

/// Fences the selected domain between ranking and dispatch.
class FenceAfterRankingHook : public IInterleavingHook {
 public:
  FenceAfterRankingHook(Scheduler& scheduler, ExecutionDomainId domain)
      : scheduler_(&scheduler), domain_(domain) {}
  void at(const HookContext& context) override {
    if (context.point != HookPoint::kAfterRanking || fired_) return;
    fired_ = true;
    const Status fenced = scheduler_->fence_domain(domain_, "hook: domain withdrawn after ranking");
    (void)fenced;
  }
  [[nodiscard]] bool fired() const noexcept { return fired_; }

 private:
  Scheduler* scheduler_;
  ExecutionDomainId domain_;
  bool fired_{false};
};

/// Completes an attempt the moment it is registered, before the channel returns.
class FastCompletionHook : public IInterleavingHook {
 public:
  explicit FastCompletionHook(Scheduler& scheduler) : scheduler_(&scheduler) {}
  void at(const HookContext& context) override {
    if (context.point != HookPoint::kAfterAttemptRegistration || completed_) return;
    ExecutionAttempt attempt;
    if (!scheduler_->attempts().get(context.attempt, attempt)) return;
    completed_ = true;
    CompletionSubmission submission;
    submission.attempt = attempt.id;
    submission.generation = attempt.generation;
    submission.dispatch = attempt.dispatch;
    submission.completion = CompletionId(attempt.id.value());
    submission.worker = attempt.worker;
    submission.worker_boot = attempt.worker_boot;
    submission.coordinator_epoch = attempt.coordinator_epoch;
    submission.domain_generation = attempt.domain_generation;
    submission.capability_generation = attempt.capability_generation;
    submission.operation = attempt.operation;
    submission.provenance = attempt.provenance;
    submission.result.success = true;
    submission.result.result_digest = 0xFACADEULL;
    submission.result.bytes_processed = 1;
    const CompletionOutcome outcome = scheduler_->complete(submission);
    rejected_because_not_dispatched_ = !outcome.committed;
  }
  [[nodiscard]] bool completed() const noexcept { return completed_; }
  [[nodiscard]] bool rejected_because_not_dispatched() const noexcept {
    return rejected_because_not_dispatched_;
  }

 private:
  Scheduler* scheduler_;
  bool completed_{false};
  bool rejected_because_not_dispatched_{false};
};

}  // namespace

TOS_TEST(fallback_after_pre_dispatch_staleness) {
  tos_test::TestRuntime::Config config;
  config.policy = make_default_policy();
  config.policy.offload_requirement = OffloadRequirement::kPreferOffload;
  config.policy.fallback.enabled = true;
  config.policy.fallback.max_depth = 3;
  tos_test::TestRuntime runtime(config);

  SyntheticDomainConfig dpu = make_synthetic_domain(ExecutionDomainId(10), ExecutionDomainType::kDpu,
                                                    "dpu.0");
  dpu.locality.class_to_payload = LocalityClass::kDomainLocal;
  dpu.observed_latency_ns = 200;
  SyntheticDomainConfig nic = make_synthetic_domain(ExecutionDomainId(11), ExecutionDomainType::kNic,
                                                    "nic.0");
  SyntheticDomainConfig cpu = make_synthetic_domain(ExecutionDomainId(12), ExecutionDomainType::kCpu,
                                                    "cpu.0");
  runtime.add_domain(std::move(dpu));
  runtime.add_domain(std::move(nic));
  runtime.add_domain(std::move(cpu));

  const std::vector<std::uint8_t> payload = tos_test::make_payload(4096, 3);
  OperationRequest request = tos_test::make_request(opclass::checksum_crc32c(), payload.size());
  PlanResult planned = runtime.scheduler().plan(request, payload);
  TOS_REQUIRE(planned.planned);
  TOS_CHECK_MSG(planned.explanation.selected_domain_type == ExecutionDomainType::kDpu,
                std::string("selected ") +
                    std::string(to_string(planned.explanation.selected_domain_type)));
  const WorkerBootId dpu_boot = planned.plan.binding.worker_boot;
  const ExecutionAttemptGeneration original_generation = planned.plan.attempt_generation;

  // The domain disappears after planning but before dispatch.
  TOS_REQUIRE(runtime.scheduler().fence_domain(planned.plan.domain, "domain lost").ok);
  ExecutionPlan stale = planned.plan;
  const DispatchResult rejected = runtime.scheduler().dispatch(stale);
  TOS_CHECK(!rejected.dispatched);
  TOS_CHECK_EQ(rejected.rejection, DispatchRejection::kAuthorityStale);
  TOS_CHECK_EQ(stale.state, PlanState::kRejected);
  ExecutionAttempt never_registered;
  TOS_CHECK(!runtime.scheduler().attempts().get(planned.plan.attempt, never_registered));

  // Fallback re-runs hard eligibility for the next legal class.
  const FallbackResult fallback = runtime.scheduler().fallback(planned.plan, FailureKind::kWorkerDeath);
  TOS_CHECK_MSG(fallback.fallback_selected, std::string(to_string(fallback.rejection)));
  TOS_REQUIRE(fallback.fallback_selected);
  TOS_CHECK_EQ(fallback.plan.explanation.outcome, SelectionOutcome::kFallbackSelected);
  TOS_CHECK(fallback.plan.domain != planned.plan.domain);
  TOS_CHECK_EQ(fallback.plan.domain_type, ExecutionDomainType::kNic);
  TOS_CHECK_EQ(fallback.plan.fallback_depth, 1U);
  TOS_CHECK_EQ(fallback.plan.fallback_from, planned.plan.domain);
  TOS_CHECK(fallback.plan.attempt != planned.plan.attempt);
  TOS_CHECK(fallback.plan.attempt_generation.value() > original_generation.value());
  TOS_CHECK(fallback.explanation.has_reason("fallback.reason.worker_death"));
  TOS_CHECK(fallback.explanation.has_reason("fallback.from.dpu"));
  TOS_CHECK(fallback.explanation.selected_is_offload);

  // The fallback target does not inherit authority: it carries its own binding for a
  // different domain and generation, so the original attempt can never publish.
  TOS_CHECK(fallback.plan.binding.domain != planned.plan.binding.domain);
  TOS_CHECK(fallback.plan.binding.domain_generation.published());
  TOS_CHECK(fallback.plan.binding.attempt != planned.plan.binding.attempt);
  // The chain records every target that was attempted, in order: the SmartNIC class
  // has no registered domain here, so it is recorded before the NIC that won.
  TOS_REQUIRE(fallback.plan.explanation.fallback_chain_tried.size() == 2U);
  TOS_CHECK_EQ(fallback.plan.explanation.fallback_chain_tried[0], ExecutionDomainType::kSmartNic);
  TOS_CHECK_EQ(fallback.plan.explanation.fallback_chain_tried[1], ExecutionDomainType::kNic);
  TOS_CHECK_EQ(fallback.plan.explanation.outcome, SelectionOutcome::kFallbackSelected);
  DispatchOutcome dispatched = runtime.scheduler().plan_reserve_dispatch(request, payload);
  (void)dispatched;
  ExecutionPlan fallback_plan = fallback.plan;
  const DispatchResult result = runtime.scheduler().dispatch(fallback_plan);
  TOS_CHECK(result.dispatched);
  runtime.wait_for(fallback_plan.attempt);
  ExecutionAttempt stored;
  TOS_REQUIRE(runtime.scheduler().attempts().get(fallback_plan.attempt, stored));
  TOS_CHECK_EQ(stored.state, AttemptState::kCompleted);
  TOS_CHECK(stored.fallback);
  TOS_CHECK_EQ(stored.fallback_from, planned.plan.domain);
}

TOS_TEST(offload_required_rejects_instead_of_using_host) {
  tos_test::TestRuntime::Config config;
  config.policy = make_default_policy();
  config.policy.offload_requirement = OffloadRequirement::kOffloadRequired;
  config.policy.fallback.enabled = true;
  config.policy.fallback.max_depth = 2;
  tos_test::TestRuntime runtime(config);

  runtime.add_domain(make_synthetic_domain(ExecutionDomainId(20), ExecutionDomainType::kCpu, "cpu.0"));
  const std::vector<std::uint8_t> payload = tos_test::make_payload(1024, 4);
  OperationRequest request = tos_test::make_request(opclass::checksum_crc32c(), payload.size());

  PlanResult planned = runtime.scheduler().plan(request, payload);
  TOS_CHECK(!planned.planned);
  TOS_CHECK_EQ(planned.explanation.outcome, SelectionOutcome::kPolicyRejected);
  TOS_CHECK(planned.explanation.has_reason("offload_required.no_offload_domain"));
  for (const CandidateEvaluation& candidate : planned.explanation.candidates) {
    TOS_CHECK(!candidate.eligible);
    TOS_CHECK_EQ(candidate.reason, IneligibilityReason::kPolicyForbidden);
  }
}

TOS_TEST(retry_requires_a_fresh_generation_and_respects_side_effects) {
  tos_test::TestRuntime::Config config;
  config.policy = make_default_policy();
  config.policy.retry.enabled = true;
  config.policy.retry.max_attempts = 3;
  tos_test::TestRuntime runtime(config);

  runtime.add_domain(make_synthetic_domain(ExecutionDomainId(30), ExecutionDomainType::kNic, "nic.0"));
  const std::vector<std::uint8_t> payload = tos_test::make_payload(512, 6);
  OperationRequest request = tos_test::make_request(opclass::checksum_crc32c(), payload.size());

  // A transport failure before the executor accepted the work is retryable.
  runtime.channel().inject_fault(ChannelFault::kSendFailure);
  DispatchOutcome outcome = runtime.scheduler().plan_reserve_dispatch(request, payload);
  TOS_REQUIRE(outcome.plan_result.planned);
  TOS_CHECK(!outcome.dispatch_result.dispatched);
  TOS_CHECK_EQ(outcome.dispatch_result.rejection, DispatchRejection::kTransportFailure);
  const ExecutionAttemptId first_attempt = outcome.plan_result.plan.attempt;
  ExecutionAttempt failed;
  TOS_REQUIRE(runtime.scheduler().attempts().get(first_attempt, failed));
  TOS_CHECK_EQ(failed.state, AttemptState::kFailed);

  const RetryResult retry = runtime.scheduler().retry(first_attempt);
  TOS_CHECK_MSG(retry.retry_permitted, std::string(to_string(retry.rejection)));
  TOS_REQUIRE(retry.retry_permitted);
  TOS_CHECK(retry.attempt != first_attempt);
  TOS_CHECK(retry.generation.value() > failed.generation.value());
  TOS_CHECK(retry.plan.retry_index == 1U);
  TOS_CHECK(retry.plan.domain == failed.domain);

  ExecutionPlan retry_plan = retry.plan;
  TOS_CHECK(runtime.scheduler().dispatch(retry_plan).dispatched);
  runtime.wait_for(retry_plan.attempt);
  ExecutionAttempt retried;
  TOS_REQUIRE(runtime.scheduler().attempts().get(retry_plan.attempt, retried));
  TOS_CHECK_EQ(retried.state, AttemptState::kCompleted);

  // A non-repeatable operation is never replayed, and the ambiguous outcome of an
  // operation that may already have run is never retried either.
  OperationRequest emit = tos_test::make_request(opclass::side_effect_emit(), payload.size());
  const SchedulerPolicy active = runtime.scheduler().policy();
  SchedulerPolicy with_retry = active;
  with_retry.retry.enabled = true;
  with_retry.retry.max_attempts = 3;
  TOS_REQUIRE(runtime.scheduler().set_policy(with_retry).ok);
  runtime.channel().inject_fault(ChannelFault::kAmbiguousCompletion);
  DispatchOutcome ambiguous = runtime.scheduler().plan_reserve_dispatch(emit, payload);
  TOS_REQUIRE(ambiguous.plan_result.planned);
  TOS_CHECK(ambiguous.dispatch_result.dispatched);
  // The executor applied the operation but never acknowledged it. Nothing is
  // delivered, so the attempt stays in flight until the runtime classifies it.
  ExecutionAttempt in_flight;
  TOS_REQUIRE(runtime.scheduler().attempts().get(ambiguous.plan_result.plan.attempt, in_flight));
  TOS_CHECK_EQ(in_flight.state, AttemptState::kDispatched);
  TOS_REQUIRE(runtime.scheduler()
                  .report_failure(in_flight.id, FailureKind::kAmbiguousOutcome,
                                  "executor did not acknowledge")
                  .ok);
  ExecutionAttempt ambiguous_attempt;
  TOS_REQUIRE(runtime.scheduler().attempts().get(ambiguous.plan_result.plan.attempt, ambiguous_attempt));
  TOS_CHECK_EQ(ambiguous_attempt.state, AttemptState::kOutcomeUnknown);
  const RetryResult refused = runtime.scheduler().retry(ambiguous_attempt.id);
  TOS_CHECK(!refused.retry_permitted);
  TOS_CHECK(refused.rejection == RetryRejection::kAmbiguousOutcome ||
            refused.rejection == RetryRejection::kNonRepeatableOperation);

  // A completed attempt has no failure to retry: that is not a limit rejection.
  const RetryResult completed_retry = runtime.scheduler().retry(retried.id);
  TOS_CHECK(!completed_retry.retry_permitted);
  TOS_CHECK_EQ(completed_retry.rejection, RetryRejection::kFailureNotRetryable);

  // Bounded: retries stop at the configured limit and never loop forever. Each
  // retry starts from a freshly failed attempt so that its plan of record is live.
  runtime.channel().inject_fault(ChannelFault::kSendFailure);
  DispatchOutcome fresh = runtime.scheduler().plan_reserve_dispatch(request, payload);
  TOS_REQUIRE(fresh.plan_result.planned);
  TOS_CHECK(!fresh.dispatch_result.dispatched);
  ExecutionAttempt current;
  TOS_REQUIRE(runtime.scheduler().attempts().get(fresh.plan_result.plan.attempt, current));
  TOS_CHECK_EQ(current.state, AttemptState::kFailed);
  std::uint32_t attempts = 1;
  for (int i = 0; i < 8; ++i) {
    const RetryResult next = runtime.scheduler().retry(current.id);
    if (!next.retry_permitted) {
      TOS_CHECK_EQ(next.rejection, RetryRejection::kAttemptLimitReached);
      break;
    }
    ++attempts;
    ExecutionPlan plan = next.plan;
    runtime.channel().inject_fault(ChannelFault::kSendFailure);
    const DispatchResult dispatched = runtime.scheduler().dispatch(plan);
    TOS_CHECK(!dispatched.dispatched);
    ExecutionAttempt stored;
    TOS_REQUIRE(runtime.scheduler().attempts().get(next.attempt, stored));
    current = stored;
  }
  TOS_CHECK_EQ(attempts, runtime.scheduler().policy().retry.max_attempts);
}

TOS_TEST(ambiguous_completion_is_never_promoted_to_success) {
  tos_test::TestRuntime::Config config;
  config.policy = make_default_policy();
  tos_test::TestRuntime runtime(config);
  runtime.add_domain(make_synthetic_domain(ExecutionDomainId(40), ExecutionDomainType::kDpu, "dpu.0"));

  const std::vector<std::uint8_t> payload = tos_test::make_payload(2048, 9);
  OperationRequest request = tos_test::make_request(opclass::side_effect_emit(), payload.size());
  runtime.channel().inject_fault(ChannelFault::kDropCompletion);
  DispatchOutcome outcome = runtime.scheduler().plan_reserve_dispatch(request, payload);
  TOS_REQUIRE(outcome.plan_result.planned);
  TOS_CHECK(outcome.dispatch_result.dispatched);
  const ExecutionAttemptId attempt_id = outcome.plan_result.plan.attempt;

  ExecutionAttempt stored;
  TOS_REQUIRE(runtime.scheduler().attempts().get(attempt_id, stored));
  TOS_CHECK_EQ(stored.state, AttemptState::kDispatched);
  TOS_CHECK(!stored.completion_committed);

  // The worker dies without acknowledging: the coordinator classifies the outcome.
  TOS_REQUIRE(runtime.scheduler().fence_worker_boot(stored.worker_boot, "worker died").ok);
  ExecutionAttempt fenced;
  TOS_REQUIRE(runtime.scheduler().attempts().get(attempt_id, fenced));
  TOS_CHECK_EQ(fenced.state, AttemptState::kOutcomeUnknown);
  TOS_CHECK_EQ(fenced.resolution, AttemptResolution::kAmbiguous);
  TOS_CHECK(!fenced.completion_committed);

  // A late completion for the fenced incarnation must not promote the attempt.
  CompletionSubmission late;
  late.attempt = attempt_id;
  late.generation = fenced.generation;
  late.dispatch = fenced.dispatch;
  late.completion = CompletionId(attempt_id.value());
  late.worker = fenced.worker;
  late.worker_boot = fenced.worker_boot;
  late.coordinator_epoch = runtime.scheduler().coordinator_epoch();
  late.domain_generation = fenced.domain_generation;
  late.capability_generation = fenced.capability_generation;
  late.operation = fenced.operation;
  late.provenance = fenced.provenance;
  late.result.success = true;
  late.result.result_digest = 999;
  const CompletionOutcome late_outcome = runtime.scheduler().complete(late);
  TOS_CHECK(!late_outcome.committed);
  ExecutionAttempt unchanged;
  TOS_REQUIRE(runtime.scheduler().attempts().get(attempt_id, unchanged));
  TOS_CHECK_EQ(unchanged.state, AttemptState::kOutcomeUnknown);
  TOS_CHECK(!unchanged.completion_committed);
  const AttemptAudit audit = runtime.scheduler().attempts().audit();
  TOS_CHECK_EQ(audit.completed, 0U);
  TOS_CHECK_EQ(audit.ambiguous, 1U);
  TOS_CHECK(audit.consistent);
}

TOS_TEST(cancellation_before_dispatch_prevents_execution) {
  tos_test::TestRuntime::Config config;
  config.policy = make_default_policy();
  tos_test::TestRuntime runtime(config);
  runtime.add_domain(make_synthetic_domain(ExecutionDomainId(50), ExecutionDomainType::kNic, "nic.0"));

  const std::vector<std::uint8_t> payload = tos_test::make_payload(256, 2);
  OperationRequest request = tos_test::make_request(opclass::checksum_crc32c(), payload.size());
  PlanResult planned = runtime.scheduler().plan(request, payload);
  TOS_REQUIRE(planned.planned);

  const CancellationResult cancelled = runtime.scheduler().cancel(planned.plan.attempt);
  TOS_CHECK(cancelled.cancelled);
  TOS_CHECK_EQ(cancelled.resolution, AttemptResolution::kCancelledBeforeDispatch);

  ExecutionPlan plan = planned.plan;
  const DispatchResult dispatched = runtime.scheduler().dispatch(plan);
  TOS_CHECK(!dispatched.dispatched);
  TOS_CHECK_EQ(dispatched.rejection, DispatchRejection::kPlanConsumed);
  TOS_CHECK_EQ(runtime.channel().dispatched_count(), 0U);
  ExecutionAttempt never;
  TOS_CHECK(!runtime.scheduler().attempts().get(planned.plan.attempt, never));
}
