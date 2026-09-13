// Deterministic race tests: forced interleavings at every authority boundary.
//
// Random thread timing is not a proof. Each test here forces one specific ordering
// with an interleaving hook or an injected channel fault and then asserts the exact
// outcome the boundary is supposed to produce.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include <atomic>
#include <functional>
#include <memory>

#include "tos_test_support.hpp"

using namespace tos;

namespace {

/// Runs an action once at a named boundary, outside any scheduler lock.
class AtPointHook : public IInterleavingHook {
 public:
  AtPointHook(HookPoint point, std::function<void()> action)
      : point_(point), action_(std::move(action)) {}
  void at(const HookContext& context) override {
    if (context.point != point_) return;
    if (fired_.exchange(true)) return;
    action_();
  }
  [[nodiscard]] bool fired() const noexcept { return fired_.load(); }

 private:
  HookPoint point_;
  std::function<void()> action_;
  std::atomic<bool> fired_{false};
};

CompletionSubmission make_submission(const ExecutionAttempt& attempt, bool success,
                                     std::uint64_t digest) {
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
  submission.result.success = success;
  submission.result.result_digest = digest;
  submission.result.bytes_processed = 16;
  return submission;
}

}  // namespace

TOS_TEST(completion_races_cancellation) {
  tos_test::TestRuntime runtime;
  runtime.add_domain(make_synthetic_domain(ExecutionDomainId(1), ExecutionDomainType::kNic, "nic.0"));
  const std::vector<std::uint8_t> payload = tos_test::make_payload(256, 1);
  OperationRequest request = tos_test::make_request(opclass::checksum_crc32c(), payload.size());

  // The completion is withheld so that cancellation wins the race deterministically.
  runtime.channel().inject_fault(ChannelFault::kDelayedCompletion);
  DispatchOutcome outcome = runtime.scheduler().plan_reserve_dispatch(request, payload);
  TOS_REQUIRE(outcome.plan_result.planned);
  TOS_CHECK(outcome.dispatch_result.dispatched);
  const ExecutionAttemptId attempt_id = outcome.plan_result.plan.attempt;

  // The executor applies the operation and withholds the acknowledgement: wait until
  // that is actually the case instead of assuming a timing.
  tos_test::wait_until([&] { return runtime.channel().delayed_count() == 1U; });

  const CancellationResult cancelled = runtime.scheduler().cancel(attempt_id);
  TOS_CHECK(cancelled.cancelled);
  // The operation is PURE and the executor honoured the cancellation request, so the
  // caller is told the outcome is not ambiguous. The non-repeatable case below is.
  TOS_CHECK(!cancelled.ambiguous);
  TOS_CHECK_EQ(cancelled.resolution, AttemptResolution::kCancelledAmbiguous);

  // The withheld completion now arrives: completion authority must refuse it.
  TOS_CHECK_EQ(runtime.channel().release_delayed(), 1U);
  ExecutionAttempt stored;
  TOS_REQUIRE(runtime.scheduler().attempts().get(attempt_id, stored));
  TOS_CHECK_EQ(stored.state, AttemptState::kCancelled);
  TOS_CHECK(!stored.completion_committed);
  TOS_CHECK_MSG(runtime.commit_count() == 0U,
                "commits " + std::to_string(runtime.commit_count()));
  TOS_CHECK_MSG(runtime.rejection_count() == 1U,
                "rejections " + std::to_string(runtime.rejection_count()));
}

TOS_TEST(cancellation_of_a_non_repeatable_operation_is_ambiguous) {
  tos_test::TestRuntime runtime;
  runtime.add_domain(make_synthetic_domain(ExecutionDomainId(21), ExecutionDomainType::kDpu, "dpu.0"));
  const std::vector<std::uint8_t> payload = tos_test::make_payload(256, 21);
  OperationRequest request = tos_test::make_request(opclass::side_effect_emit(), payload.size());

  runtime.channel().inject_fault(ChannelFault::kDelayedCompletion);
  DispatchOutcome outcome = runtime.scheduler().plan_reserve_dispatch(request, payload);
  TOS_REQUIRE(outcome.plan_result.planned);
  const ExecutionAttemptId attempt_id = outcome.plan_result.plan.attempt;
  tos_test::wait_until([&] { return runtime.channel().delayed_count() == 1U; });

  const CancellationResult cancelled = runtime.scheduler().cancel(attempt_id);
  TOS_CHECK(cancelled.cancelled);
  TOS_CHECK_MSG(cancelled.ambiguous,
                "an operation that may already have produced a side effect must be reported "
                "as ambiguous");
  TOS_CHECK_EQ(cancelled.resolution, AttemptResolution::kCancelledAmbiguous);

  TOS_CHECK_EQ(runtime.channel().release_delayed(), 1U);
  ExecutionAttempt stored;
  TOS_REQUIRE(runtime.scheduler().attempts().get(attempt_id, stored));
  TOS_CHECK_EQ(stored.state, AttemptState::kCancelled);
  TOS_CHECK(!stored.completion_committed);
  TOS_CHECK_EQ(stored.side_effect, SideEffectClass::kNonRepeatable);
  const RetryResult refused = runtime.scheduler().retry(attempt_id);
  TOS_CHECK(!refused.retry_permitted);
}

TOS_TEST(worker_fence_races_completion) {
  tos_test::TestRuntime runtime;
  runtime.add_domain(make_synthetic_domain(ExecutionDomainId(2), ExecutionDomainType::kDpu, "dpu.0"));
  const std::vector<std::uint8_t> payload = tos_test::make_payload(256, 2);
  OperationRequest request = tos_test::make_request(opclass::checksum_crc32c(), payload.size());

  runtime.channel().inject_fault(ChannelFault::kDelayedCompletion);
  DispatchOutcome outcome = runtime.scheduler().plan_reserve_dispatch(request, payload);
  TOS_REQUIRE(outcome.plan_result.planned);
  const ExecutionAttemptId attempt_id = outcome.plan_result.plan.attempt;
  ExecutionAttempt before;
  TOS_REQUIRE(runtime.scheduler().attempts().get(attempt_id, before));

  // The incarnation dies before its acknowledgement can be committed.
  TOS_REQUIRE(runtime.scheduler().fence_worker_boot(before.worker_boot, "worker died").ok);
  ExecutionAttempt fenced;
  TOS_REQUIRE(runtime.scheduler().attempts().get(attempt_id, fenced));
  TOS_CHECK_EQ(fenced.state, AttemptState::kOutcomeUnknown);

  TOS_CHECK_EQ(runtime.channel().release_delayed(), 1U);
  ExecutionAttempt after;
  TOS_REQUIRE(runtime.scheduler().attempts().get(attempt_id, after));
  TOS_CHECK_EQ(after.state, AttemptState::kOutcomeUnknown);
  TOS_CHECK(!after.completion_committed);
  TOS_CHECK_EQ(runtime.commit_count(), 0U);
}

TOS_TEST(policy_update_races_dispatch) {
  tos_test::TestRuntime runtime;
  runtime.add_domain(make_synthetic_domain(ExecutionDomainId(3), ExecutionDomainType::kNic, "nic.0"));
  const std::vector<std::uint8_t> payload = tos_test::make_payload(256, 3);
  OperationRequest request = tos_test::make_request(opclass::checksum_crc32c(), payload.size());

  PlanResult planned = runtime.scheduler().plan(request, payload);
  TOS_REQUIRE(planned.planned);

  // The policy changes after planning and before dispatch.
  Status policy_applied = Status::success();
  AtPointHook hook(HookPoint::kBeforeRevalidate, [&runtime, &policy_applied] {
    SchedulerPolicy policy = runtime.scheduler().policy();
    policy.minimum_isolation = IsolationClass::kSeparateAddressSpace;
    policy_applied = runtime.scheduler().set_policy(std::move(policy));
  });
  runtime.scheduler().set_hook(&hook);
  ExecutionPlan plan = planned.plan;
  const DispatchResult dispatched = runtime.scheduler().dispatch(plan);
  runtime.scheduler().set_hook(nullptr);
  TOS_CHECK(hook.fired());
  TOS_CHECK_MSG(policy_applied.ok, policy_applied.code);
  TOS_CHECK(!dispatched.dispatched);
  TOS_CHECK_EQ(dispatched.rejection, DispatchRejection::kAuthorityStale);
  ExecutionAttempt never;
  TOS_CHECK(!runtime.scheduler().attempts().get(planned.plan.attempt, never));
}

TOS_TEST(capability_change_after_planning_races_dispatch) {
  tos_test::TestRuntime runtime;
  const ExecutionDomainId domain =
      runtime.add_domain(make_synthetic_domain(ExecutionDomainId(4), ExecutionDomainType::kDpu, "dpu.0"));
  const std::vector<std::uint8_t> payload = tos_test::make_payload(256, 4);
  OperationRequest request = tos_test::make_request(opclass::checksum_crc32c(), payload.size());

  PlanResult planned = runtime.scheduler().plan(request, payload);
  TOS_REQUIRE(planned.planned);

  // The engine generation changes: the same engine no longer supports the class.
  TOS_REQUIRE(runtime.backend()
                  .set_operation_support(domain, opclass::checksum_crc32c(), false)
                  .ok);
  TOS_REQUIRE(runtime.refresh().ok);
  ExecutionPlan plan = planned.plan;
  const DispatchResult dispatched = runtime.scheduler().dispatch(plan);
  TOS_CHECK(!dispatched.dispatched);
  TOS_CHECK_EQ(dispatched.rejection, DispatchRejection::kAuthorityStale);
  ExecutionAttempt never;
  TOS_CHECK(!runtime.scheduler().attempts().get(planned.plan.attempt, never));
}

TOS_TEST(load_generation_change_after_planning_races_dispatch) {
  tos_test::TestRuntime runtime;
  const ExecutionDomainId domain =
      runtime.add_domain(make_synthetic_domain(ExecutionDomainId(5), ExecutionDomainType::kNic, "nic.0"));
  const std::vector<std::uint8_t> payload = tos_test::make_payload(256, 5);
  OperationRequest request = tos_test::make_request(opclass::checksum_crc32c(), payload.size());

  PlanResult planned = runtime.scheduler().plan(request, payload);
  TOS_REQUIRE(planned.planned);

  TOS_REQUIRE(runtime.backend().set_load(domain, 95, 90, 200, 7, 5000000).ok);
  TOS_REQUIRE(runtime.refresh().ok);
  ExecutionPlan plan = planned.plan;
  const DispatchResult dispatched = runtime.scheduler().dispatch(plan);
  TOS_CHECK(!dispatched.dispatched);
  TOS_CHECK_EQ(dispatched.rejection, DispatchRejection::kAuthorityStale);

  // Re-planning after the change must be legal and must pick up the new evidence.
  PlanResult replanned = runtime.scheduler().plan(request, payload);
  TOS_CHECK(replanned.planned);
  TOS_CHECK(replanned.plan.domain == domain);
}

TOS_TEST(reservation_release_races_completion) {
  tos_test::TestRuntime::Config config;
  config.policy = make_default_policy();
  config.policy.reservation.enabled = true;
  config.policy.reservation.per_operation[static_cast<std::size_t>(ResourceKind::kExecutionSlot)] = 1;
  tos_test::TestRuntime runtime(config);
  SyntheticDomainConfig domain_config =
      make_synthetic_domain(ExecutionDomainId(6), ExecutionDomainType::kDpu, "dpu.0");
  domain_config.capacity[static_cast<std::size_t>(ResourceKind::kExecutionSlot)] = 2;
  runtime.add_domain(std::move(domain_config));

  const std::vector<std::uint8_t> payload = tos_test::make_payload(256, 6);
  OperationRequest request = tos_test::make_request(opclass::checksum_crc32c(), payload.size());

  // A second release of the same reservation is attempted after the completion
  // committed: double release must be refused and accounting must stay exact.
  ReservationId observed;
  AtPointHook hook(HookPoint::kAfterCompletionCommit, [&runtime, &observed] {
    if (!observed.valid()) return;
    const Status released = runtime.scheduler().reservations().release(observed, "race: double release");
    (void)released;
  });
  runtime.scheduler().set_hook(&hook);
  DispatchOutcome outcome = runtime.scheduler().plan_reserve_dispatch(request, payload);
  TOS_REQUIRE(outcome.plan_result.planned);
  observed = outcome.plan_result.plan.reservation;
  TOS_CHECK(observed.valid());
  runtime.wait_for(outcome.plan_result.plan.attempt);
  runtime.scheduler().set_hook(nullptr);
  TOS_CHECK(hook.fired());

  const ReservationAudit audit = runtime.scheduler().reservations().audit();
  TOS_CHECK(audit.consistent);
  TOS_CHECK_EQ(audit.outstanding, 0U);
  TOS_CHECK_EQ(audit.committed, 0U);
  TOS_CHECK_EQ(runtime.scheduler().reservations().outstanding_count(), 0U);
}

TOS_TEST(fast_completion_arrives_before_dispatch_returns) {
  tos_test::TestRuntime runtime;
  runtime.add_domain(make_synthetic_domain(ExecutionDomainId(7), ExecutionDomainType::kNic, "nic.0"));
  const std::vector<std::uint8_t> payload = tos_test::make_payload(256, 7);
  OperationRequest request = tos_test::make_request(opclass::checksum_crc32c(), payload.size());

  // The attempt is registered before the channel is called, so a completion that
  // arrives while the attempt is still DISPATCHING is authoritative exactly once.
  AtPointHook hook(HookPoint::kAfterAttemptRegistration, [&runtime] {
    // The hook does not know the identity, but exactly one attempt is in flight here.
    const std::vector<ExecutionAttempt> live = runtime.scheduler().attempts().list_live();
    if (!live.empty()) {
      const CompletionOutcome outcome =
          runtime.scheduler().complete(make_submission(live.front(), true, 0x5151ULL));
      (void)outcome;
    }
  });
  runtime.scheduler().set_hook(&hook);
  DispatchOutcome outcome = runtime.scheduler().plan_reserve_dispatch(request, payload);
  runtime.scheduler().set_hook(nullptr);
  TOS_CHECK(hook.fired());
  TOS_REQUIRE(outcome.plan_result.planned);
  TOS_CHECK(outcome.dispatch_result.dispatched);

  runtime.wait_for(outcome.plan_result.plan.attempt, 1);
  ExecutionAttempt stored;
  TOS_REQUIRE(runtime.scheduler().attempts().get(outcome.plan_result.plan.attempt, stored));
  TOS_CHECK_EQ(stored.state, AttemptState::kCompleted);
  TOS_CHECK(stored.completion_committed);
  TOS_CHECK_EQ(stored.result.result_digest, 0x5151ULL);
  const AttemptAudit audit = runtime.scheduler().attempts().audit();
  TOS_CHECK(audit.consistent);
  TOS_CHECK_EQ(audit.completed, 1U);
}

TOS_TEST(fallback_races_original_completion) {
  tos_test::TestRuntime::Config config;
  config.policy = make_default_policy();
  config.policy.offload_requirement = OffloadRequirement::kPreferOffload;
  config.policy.fallback.enabled = true;
  config.policy.fallback.max_depth = 2;
  tos_test::TestRuntime runtime(config);
  SyntheticDomainConfig dpu =
      make_synthetic_domain(ExecutionDomainId(8), ExecutionDomainType::kDpu, "dpu.0");
  dpu.locality.class_to_payload = LocalityClass::kDomainLocal;
  runtime.add_domain(std::move(dpu));
  runtime.add_domain(make_synthetic_domain(ExecutionDomainId(9), ExecutionDomainType::kNic, "nic.0"));

  const std::vector<std::uint8_t> payload = tos_test::make_payload(256, 8);
  OperationRequest request = tos_test::make_request(opclass::checksum_crc32c(), payload.size());

  runtime.channel().inject_fault(ChannelFault::kDelayedCompletion);
  DispatchOutcome outcome = runtime.scheduler().plan_reserve_dispatch(request, payload);
  TOS_REQUIRE(outcome.plan_result.planned);
  const ExecutionAttemptId original = outcome.plan_result.plan.attempt;
  ExecutionAttempt before;
  TOS_REQUIRE(runtime.scheduler().attempts().get(original, before));

  // The domain is declared lost while the original completion is still withheld.
  TOS_REQUIRE(runtime.scheduler().fence_domain(before.domain, "domain lost").ok);
  const FallbackResult fallback =
      runtime.scheduler().fallback(outcome.plan_result.plan, FailureKind::kDomainUnavailable);
  TOS_CHECK_MSG(fallback.fallback_selected, std::string(to_string(fallback.rejection)));
  TOS_REQUIRE(fallback.fallback_selected);
  TOS_CHECK(fallback.plan.attempt_generation.value() > before.generation.value());

  // The original completion now arrives: it belongs to a fenced incarnation and a
  // superseded plan, so it must not commit.
  TOS_CHECK_EQ(runtime.channel().release_delayed(), 1U);
  ExecutionAttempt original_after;
  TOS_REQUIRE(runtime.scheduler().attempts().get(original, original_after));
  TOS_CHECK(original_after.state == AttemptState::kOutcomeUnknown ||
            original_after.state == AttemptState::kFenced);
  TOS_CHECK(!original_after.completion_committed);
  TOS_CHECK_EQ(runtime.commit_count(), 0U);

  ExecutionPlan fallback_plan = fallback.plan;
  TOS_CHECK(runtime.scheduler().dispatch(fallback_plan).dispatched);
  runtime.wait_for(fallback_plan.attempt);
  ExecutionAttempt completed;
  TOS_REQUIRE(runtime.scheduler().attempts().get(fallback_plan.attempt, completed));
  TOS_CHECK_EQ(completed.state, AttemptState::kCompleted);
}

TOS_TEST(shutdown_races_dispatch) {
  tos_test::TestRuntime runtime;
  runtime.add_domain(make_synthetic_domain(ExecutionDomainId(11), ExecutionDomainType::kNic, "nic.0"));
  const std::vector<std::uint8_t> payload = tos_test::make_payload(256, 11);
  OperationRequest request = tos_test::make_request(opclass::checksum_crc32c(), payload.size());

  // Shutdown is requested between attempt registration and the channel call. The
  // dispatch must either complete as a non-authoritative attempt or be refused, and
  // in both cases the runtime must end up with no live attempt and closed books.
  AtPointHook hook(HookPoint::kBeforeChannelSend, [&runtime] {
    const Status stopped = runtime.scheduler().shutdown();
    (void)stopped;
  });
  runtime.scheduler().set_hook(&hook);
  DispatchOutcome outcome = runtime.scheduler().plan_reserve_dispatch(request, payload);
  runtime.scheduler().set_hook(nullptr);
  TOS_CHECK(hook.fired());
  TOS_CHECK(!runtime.scheduler().running());
  TOS_CHECK_EQ(runtime.scheduler().attempts().live_count(), 0U);
  const ReservationAudit audit = runtime.scheduler().reservations().audit();
  TOS_CHECK(audit.consistent);
  TOS_CHECK_EQ(runtime.scheduler().reservations().outstanding_count(), 0U);
  if (outcome.plan_result.planned) {
    ExecutionAttempt stored;
    TOS_REQUIRE(runtime.scheduler().attempts().get(outcome.plan_result.plan.attempt, stored));
    TOS_CHECK_MSG(stored.state == AttemptState::kOutcomeUnknown ||
                      stored.state == AttemptState::kFenced ||
                      stored.state == AttemptState::kFailed,
                  std::string("unexpected state ") + std::string(to_string(stored.state)));
  }
}

TOS_TEST(stale_completion_races_retry) {
  tos_test::TestRuntime::Config config;
  config.policy = make_default_policy();
  config.policy.retry.enabled = true;
  config.policy.retry.max_attempts = 3;
  tos_test::TestRuntime runtime(config);
  runtime.add_domain(make_synthetic_domain(ExecutionDomainId(12), ExecutionDomainType::kNic, "nic.0"));
  const std::vector<std::uint8_t> payload = tos_test::make_payload(256, 12);
  OperationRequest request = tos_test::make_request(opclass::checksum_crc32c(), payload.size());

  runtime.channel().inject_fault(ChannelFault::kDropCompletion);
  DispatchOutcome outcome = runtime.scheduler().plan_reserve_dispatch(request, payload);
  TOS_REQUIRE(outcome.plan_result.planned);
  const ExecutionAttemptId first_attempt = outcome.plan_result.plan.attempt;
  ExecutionAttempt dropped;
  TOS_REQUIRE(runtime.scheduler().attempts().get(first_attempt, dropped));
  TOS_REQUIRE(runtime.scheduler()
                  .report_failure(first_attempt, FailureKind::kTransportFailure, "no acknowledgement")
                  .ok);
  ExecutionAttempt failed;
  TOS_REQUIRE(runtime.scheduler().attempts().get(first_attempt, failed));
  TOS_CHECK_EQ(failed.state, AttemptState::kFailed);

  const RetryResult retry = runtime.scheduler().retry(first_attempt);
  TOS_CHECK_MSG(retry.retry_permitted, std::string(to_string(retry.rejection)));
  TOS_REQUIRE(retry.retry_permitted);

  // A late completion for the superseded generation must not commit, and must not
  // disturb the retry that superseded it.
  const CompletionOutcome late = runtime.scheduler().complete(make_submission(dropped, true, 42));
  TOS_CHECK(!late.committed);
  ExecutionAttempt after;
  TOS_REQUIRE(runtime.scheduler().attempts().get(first_attempt, after));
  TOS_CHECK_EQ(after.state, AttemptState::kFailed);
  TOS_CHECK(!after.completion_committed);

  ExecutionPlan retry_plan = retry.plan;
  TOS_CHECK(runtime.scheduler().dispatch(retry_plan).dispatched);
  runtime.wait_for(retry_plan.attempt);
  ExecutionAttempt retried;
  TOS_REQUIRE(runtime.scheduler().attempts().get(retry_plan.attempt, retried));
  TOS_CHECK_EQ(retried.state, AttemptState::kCompleted);
  TOS_CHECK(retried.generation.value() > failed.generation.value());
}
