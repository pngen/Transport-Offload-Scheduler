// Authority binding and staleness: a plan is bound to the exact evidence that made
// it legal, any later change on the legal publication path invalidates it at the
// dispatch boundary, and attempt generations never regress for one operation.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "tos_test_support.hpp"

using namespace tos;

namespace {

tos_test::TestRuntime::Config config_with(SchedulerPolicy policy) {
  tos_test::TestRuntime::Config config;
  config.policy = std::move(policy);
  return config;
}

/// A dispatchable runtime: one synthetic DPU domain plus a fixed request and
/// payload whose transport operation identity stays stable across the test.
class AuthorityFixture {
 public:
  explicit AuthorityFixture(SchedulerPolicy policy = make_default_policy())
      : policy_(std::move(policy)), runtime_(config_with(policy_)) {
    domain_ = runtime_.add_domain(make_synthetic_domain(ExecutionDomainId(1),
                                                        ExecutionDomainType::kDpu,
                                                        "synthetic.authority"));
    request_.operation_class = opclass::checksum_crc32c();
    request_.operation_id = TransportOperationId(4242);
    request_.payload.size_bytes = 4096;
    request_.payload.source_memory = MemoryDomain::kHost;
    request_.payload.destination_memory = MemoryDomain::kHost;
    request_.payload.alignment_bytes = 8;
    request_.payload.transport_class = TransportClass::kRawFrames;
    request_.payload.payload_class = PayloadClass::kOpaqueBytes;
    request_.retry.allowed = true;
    request_.retry.max_attempts = 2;
    payload_ = tos_test::make_payload(4096, 21);
  }

  Scheduler& scheduler() { return runtime_.scheduler(); }
  tos_test::TestRuntime& runtime() { return runtime_; }
  SyntheticBackend& backend() { return runtime_.backend(); }
  ExecutionDomainId domain() const { return domain_; }
  const OperationRequest& request() const { return request_; }
  std::span<const std::uint8_t> payload() const {
    return std::span<const std::uint8_t>(payload_.data(), payload_.size());
  }

  PlanResult plan() { return runtime_.scheduler().plan(request_, payload()); }
  Status refresh() { return runtime_.refresh(); }

 private:
  SchedulerPolicy policy_;
  tos_test::TestRuntime runtime_;
  ExecutionDomainId domain_;
  OperationRequest request_;
  std::vector<std::uint8_t> payload_;
};

/// Recompute the authority comparison exactly as dispatch does.
AuthorityValidation validate_now(AuthorityFixture& fixture, const ExecutionPlan& plan) {
  ExecutionDomainRecord record;
  if (!fixture.scheduler().domains().get(plan.domain, record)) return AuthorityValidation{};
  const SchedulerPolicy policy = fixture.scheduler().policy();
  return validate_authority(plan.binding,
                            live_authority_of(record, fixture.scheduler().coordinator_epoch(),
                                              policy.generation, policy.isolation_generation),
                            policy.freshness);
}

/// What a stale-authority rejection must report.
struct StaleExpectation {
  const char* field;                    ///< mismatch field that must be recorded
  const char* code{"authority.stale"};  ///< stable status code
  const char* exact_detail{nullptr};    ///< exact detail, when it is unambiguous
};

/// Dispatch must be refused as stale, the plan must be marked rejected, no attempt
/// may reach the ledger, and the named mismatch field must be reported.
void expect_authority_stale(AuthorityFixture& fixture, ExecutionPlan& plan,
                            const StaleExpectation& expectation) {
  const DispatchResult result = fixture.scheduler().dispatch(plan);

  TOS_CHECK_MSG(!result.dispatched, "a plan with stale authority must not dispatch");
  TOS_CHECK_EQ(result.rejection, DispatchRejection::kAuthorityStale);
  TOS_CHECK_EQ(result.status.code, std::string(expectation.code));
  TOS_CHECK_EQ(plan.state, PlanState::kRejected);
  TOS_CHECK_EQ(plan.explanation.outcome, SelectionOutcome::kRevalidationRequired);
  TOS_CHECK(plan.explanation.has_reason(expectation.code));

  ExecutionAttempt stored;
  TOS_CHECK_MSG(!fixture.scheduler().attempts().get(plan.attempt, stored),
                "a rejected plan must never register an attempt");

  const AuthorityValidation validation = validate_now(fixture, plan);
  TOS_CHECK_MSG(!validation.valid, "live authority must no longer match the binding");
  TOS_CHECK_MSG(validation.has_field(expectation.field),
                std::string("expected authority mismatch field ") + expectation.field);
  TOS_CHECK_MSG(validation.has_field(result.status.message),
                std::string("the rejection detail must name a recorded mismatch field, got: ") +
                    result.status.message);
  if (expectation.exact_detail != nullptr) {
    TOS_CHECK_EQ(result.status.message, std::string(expectation.exact_detail));
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. A plan binds every participating generation, identity and operation.
// ---------------------------------------------------------------------------

TOS_TEST(authority_plan_binds_every_generation) {
  // Capacity must be reserved for the reservation identity to be bound at all.
  SchedulerPolicy policy = make_default_policy();
  policy.reservation.enabled = true;
  policy.reservation.per_operation[static_cast<std::size_t>(ResourceKind::kExecutionSlot)] = 1;
  AuthorityFixture fixture(policy);
  const PlanResult planned = fixture.plan();
  TOS_REQUIRE(planned.planned);
  const AuthorityBinding& binding = planned.plan.binding;

  TOS_CHECK(binding.coordinator_epoch.published());
  TOS_CHECK_EQ(binding.coordinator_epoch, fixture.scheduler().coordinator_epoch());
  TOS_CHECK(binding.worker.valid());
  TOS_CHECK(binding.worker_boot.valid());
  TOS_CHECK(binding.domain.valid());
  TOS_CHECK_EQ(binding.domain, fixture.domain());
  TOS_CHECK(binding.domain_generation.published());
  TOS_CHECK(binding.capability_generation.published());
  TOS_CHECK(binding.backend_generation.published());
  TOS_CHECK(binding.topology_generation.published());
  TOS_CHECK(binding.locality_generation.published());
  TOS_CHECK(binding.health_generation.published());
  TOS_CHECK(binding.queue_generation.published());
  TOS_CHECK(binding.load_generation.published());
  TOS_CHECK(binding.policy_generation.published());
  TOS_CHECK_EQ(binding.policy_generation, fixture.scheduler().policy_generation());
  TOS_CHECK(binding.compatibility_generation.published());
  TOS_CHECK(binding.isolation_generation.published());
  TOS_CHECK(binding.evidence_generation.published());
  TOS_CHECK(binding.operation.valid());
  TOS_CHECK_EQ(binding.operation, fixture.request().operation_id);
  TOS_CHECK(binding.attempt.valid());
  TOS_CHECK_EQ(binding.attempt, planned.plan.attempt);
  TOS_CHECK(binding.attempt_generation.published());
  TOS_CHECK_EQ(binding.attempt_generation, planned.plan.attempt_generation);
  TOS_CHECK(!binding.reservation.valid());

  // Reservation binds its own identity into the same record.
  ExecutionPlan plan = planned.plan;
  TOS_REQUIRE(fixture.scheduler().reserve(plan).ok);
  TOS_CHECK_EQ(plan.state, PlanState::kReserved);
  TOS_CHECK(plan.binding.reservation.valid());
  TOS_CHECK_EQ(plan.binding.reservation, plan.reservation);
}

// ---------------------------------------------------------------------------
// 2. Every legal evidence change makes the plan stale at dispatch.
// ---------------------------------------------------------------------------

TOS_TEST(authority_capability_change_rejects_dispatch) {
  AuthorityFixture fixture;
  const PlanResult planned = fixture.plan();
  TOS_REQUIRE(planned.planned);
  ExecutionPlan plan = planned.plan;

  ExecutionDomainRecord record;
  TOS_REQUIRE(fixture.scheduler().domains().get(fixture.domain(), record));
  CapabilitySet changed = record.capability.capability;
  changed.max_payload_bytes = 64 * 1024;
  TOS_REQUIRE(fixture.backend().set_capability(fixture.domain(), changed, true).ok);
  TOS_REQUIRE(fixture.refresh().ok);

  // Publishing changed capability content also advances the domain generation by
  // design, so the capability generation is checked explicitly here.
  expect_authority_stale(fixture, plan, {"capability_generation"});
}

TOS_TEST(authority_load_change_rejects_dispatch) {
  AuthorityFixture fixture;
  const PlanResult planned = fixture.plan();
  TOS_REQUIRE(planned.planned);
  ExecutionPlan plan = planned.plan;

  TOS_REQUIRE(fixture.backend().set_load(fixture.domain(), 90, 80, 128, 8, 250000).ok);
  TOS_REQUIRE(fixture.refresh().ok);

  // Health freshness is required by the default policy, and a load publication
  // advances the health generation because health rides the same evidence stream.
  expect_authority_stale(fixture, plan, {"health_generation", "authority.stale", "health_generation"});
}

TOS_TEST(authority_health_change_rejects_dispatch) {
  AuthorityFixture fixture;
  const PlanResult planned = fixture.plan();
  TOS_REQUIRE(planned.planned);
  ExecutionPlan plan = planned.plan;

  TOS_REQUIRE(fixture.backend().set_health(fixture.domain(), false, false, false).ok);
  TOS_REQUIRE(fixture.refresh().ok);

  expect_authority_stale(fixture, plan, {"health_generation", "authority.stale", "health_generation"});
}

TOS_TEST(authority_locality_change_rejects_dispatch) {
  SchedulerPolicy policy = make_default_policy();
  policy.freshness.locality = true;  // locality freshness is opt-in
  AuthorityFixture fixture(policy);
  const PlanResult planned = fixture.plan();
  TOS_REQUIRE(planned.planned);
  ExecutionPlan plan = planned.plan;

  ExecutionDomainRecord record;
  TOS_REQUIRE(fixture.scheduler().domains().get(fixture.domain(), record));
  DomainLocality locality = record.locality;
  locality.class_to_payload = LocalityClass::kSameNumaNode;
  locality.numa_node = 1;
  locality.generation = LocalityGeneration(record.locality.generation.value() + 1);
  TOS_REQUIRE(fixture.backend().set_locality(fixture.domain(), locality).ok);
  TOS_REQUIRE(fixture.refresh().ok);

  expect_authority_stale(fixture, plan, {"locality_generation"});
}

TOS_TEST(authority_fence_rejects_dispatch) {
  AuthorityFixture fixture;
  const PlanResult planned = fixture.plan();
  TOS_REQUIRE(planned.planned);
  ExecutionPlan plan = planned.plan;

  TOS_REQUIRE(fixture.scheduler().fence_domain(fixture.domain(), "test").ok);

  expect_authority_stale(fixture, plan,
                         {"domain_fenced", "authority.domain_fenced", "domain_fenced"});
}

// An advanced domain generation reported by the backend must reach the scheduler
// through the ordinary evidence path: LocalDomainPublisher::sync() re-publishes the
// whole record when the generation moves, the registry advances the stored
// generation, and any plan bound to the previous generation is rejected as stale.
TOS_TEST(authority_domain_generation_advance_rejects_dispatch) {
  AuthorityFixture fixture;
  const PlanResult planned = fixture.plan();
  TOS_REQUIRE(planned.planned);
  ExecutionPlan plan = planned.plan;

  ExecutionDomainRecord record;
  TOS_REQUIRE(fixture.scheduler().domains().get(fixture.domain(), record));
  TOS_REQUIRE(fixture.backend()
                  .set_domain_generation(
                      fixture.domain(), ExecutionDomainGeneration(record.generation.value() + 1))
                  .ok);
  const Status refreshed = fixture.refresh();
  TOS_CHECK_MSG(refreshed.ok, "refresh failed with " + refreshed.code + ": " + refreshed.message);
  TOS_REQUIRE(refreshed.ok);

  // The registry learned the new generation through the evidence path.
  ExecutionDomainRecord live;
  TOS_REQUIRE(fixture.scheduler().domains().get(fixture.domain(), live));
  const std::vector<ExecutionDomainRecord> reported = fixture.backend().discover_domains();
  TOS_REQUIRE(reported.size() == std::size_t{1});
  TOS_CHECK_MSG(live.generation.value() >= reported.front().generation.value(),
                "the registry must adopt the generation the backend reports");
  TOS_CHECK_MSG(live.generation > plan.binding.domain_generation,
                "the plan is bound to a generation the domain has left");

  expect_authority_stale(fixture, plan, {"domain_generation"});
}

TOS_TEST(authority_policy_change_rejects_dispatch) {
  AuthorityFixture fixture;
  const PlanResult planned = fixture.plan();
  TOS_REQUIRE(planned.planned);
  ExecutionPlan plan = planned.plan;

  SchedulerPolicy changed = fixture.scheduler().policy();
  changed.weights[static_cast<std::size_t>(RankingFactor::kPolicyPreference)] += 1;
  TOS_REQUIRE(fixture.scheduler().set_policy(changed).ok);
  TOS_CHECK(fixture.scheduler().policy_generation() > plan.binding.policy_generation);

  expect_authority_stale(fixture, plan, {"policy_generation", "authority.stale", "policy_generation"});
}

// ---------------------------------------------------------------------------
// 3. Republishing identical evidence is a no-op, and re-planning a transport
//    operation always moves its attempt generation forward.
// ---------------------------------------------------------------------------

TOS_TEST(authority_identical_republish_keeps_plan_dispatchable) {
  AuthorityFixture fixture;
  const PlanResult planned = fixture.plan();
  TOS_REQUIRE(planned.planned);
  ExecutionPlan plan = planned.plan;

  ExecutionDomainRecord record;
  TOS_REQUIRE(fixture.scheduler().domains().get(fixture.domain(), record));

  // Identical content, republished through the normal evidence path.
  TOS_REQUIRE(fixture.refresh().ok);
  TOS_REQUIRE(fixture.scheduler().publish_capability(record.capability).ok);
  TOS_REQUIRE(fixture.scheduler().publish_load(fixture.domain(), record.load).ok);
  TOS_REQUIRE(fixture.refresh().ok);

  TOS_CHECK_MSG(validate_now(fixture, plan).valid,
                "republishing identical evidence must not invalidate a plan");

  const DispatchResult dispatched = fixture.scheduler().dispatch(plan);
  TOS_CHECK_MSG(dispatched.dispatched, dispatched.status.message);
  TOS_CHECK_EQ(dispatched.rejection, DispatchRejection::kNone);
  TOS_CHECK_EQ(plan.state, PlanState::kDispatched);

  const ExecutionAttempt attempt = fixture.runtime().wait_for_terminal(plan.attempt);
  TOS_CHECK_EQ(attempt.state, AttemptState::kCompleted);
  TOS_CHECK(attempt.completion_committed);
  TOS_CHECK_EQ(fixture.runtime().commit_count(), std::size_t{1});
}

TOS_TEST(authority_replanning_advances_the_attempt_generation) {
  AuthorityFixture fixture;
  const PlanResult first = fixture.plan();
  TOS_REQUIRE(first.planned);

  const PlanResult second = fixture.plan();
  TOS_REQUIRE(second.planned);
  TOS_CHECK_EQ(second.plan.operation, first.plan.operation);
  TOS_CHECK_MSG(second.plan.attempt_generation > first.plan.attempt_generation,
                "re-planning one transport operation must advance its attempt generation");
  TOS_CHECK(second.plan.attempt != first.plan.attempt);
}

// ---------------------------------------------------------------------------
// 4. Attempt generations never regress for one operation, across a full
//    plan/refresh/dispatch/completion cycle.
// ---------------------------------------------------------------------------

TOS_TEST(authority_replan_after_stale_refresh_advances_the_generation) {
  AuthorityFixture fixture;
  const PlanResult first = fixture.plan();
  TOS_REQUIRE(first.planned);
  ExecutionPlan stale_plan = first.plan;

  // Evidence moves; the old plan is refused, and re-planning the same transport
  // operation must produce a strictly newer attempt generation.
  TOS_REQUIRE(fixture.backend().set_load(fixture.domain(), 40, 20, 4, 1, 5000).ok);
  TOS_REQUIRE(fixture.refresh().ok);
  const DispatchResult refused = fixture.scheduler().dispatch(stale_plan);
  TOS_CHECK(!refused.dispatched);
  TOS_CHECK_EQ(refused.rejection, DispatchRejection::kAuthorityStale);

  const PlanResult second = fixture.plan();
  TOS_REQUIRE(second.planned);
  TOS_CHECK_EQ(second.plan.operation, first.plan.operation);
  TOS_CHECK_MSG(second.plan.attempt_generation > first.plan.attempt_generation,
                "the replacement attempt must carry a newer generation");
}

// The registry and the validator both handle an advanced domain generation
// correctly: publishing the record through Scheduler::update_domain() invalidates
// the plan. This isolates the defect below to the local publisher.
TOS_TEST(authority_full_record_republication_rejects_dispatch) {
  AuthorityFixture fixture;
  const PlanResult planned = fixture.plan();
  TOS_REQUIRE(planned.planned);
  ExecutionPlan plan = planned.plan;

  ExecutionDomainRecord record;
  TOS_REQUIRE(fixture.scheduler().domains().get(fixture.domain(), record));
  record.generation = ExecutionDomainGeneration(record.generation.value() + 1);
  TOS_REQUIRE(fixture.scheduler().update_domain(record).ok());

  expect_authority_stale(fixture, plan,
                         {"domain_generation", "authority.stale", "domain_generation"});
}

TOS_TEST(authority_attempt_generations_never_regress) {
  AuthorityFixture fixture;
  std::vector<std::uint64_t> generations;
  ExecutionAttemptGeneration previous;

  for (int iteration = 0; iteration < 5; ++iteration) {
    const PlanResult planned = fixture.plan();
    TOS_REQUIRE(planned.planned);
    ExecutionPlan plan = planned.plan;

    TOS_CHECK_MSG(plan.attempt_generation > previous,
                  "attempt generation must strictly increase for one operation");
    TOS_CHECK_EQ(plan.operation, fixture.request().operation_id);
    generations.push_back(plan.attempt_generation.value());
    previous = plan.attempt_generation;

    TOS_REQUIRE(fixture.refresh().ok);

    const DispatchResult dispatched = fixture.scheduler().dispatch(plan);
    TOS_REQUIRE(dispatched.dispatched);
    const ExecutionAttempt attempt = fixture.runtime().wait_for_terminal(plan.attempt);
    TOS_CHECK_EQ(attempt.state, AttemptState::kCompleted);
    TOS_CHECK_EQ(attempt.generation, plan.attempt_generation);
  }

  TOS_REQUIRE(generations.size() == std::size_t{5});
  for (std::size_t i = 1; i < generations.size(); ++i) {
    TOS_CHECK_MSG(generations[i] > generations[i - 1],
                  "recorded attempt generations must be monotonic");
  }
  TOS_CHECK_EQ(fixture.scheduler().attempts().highest_generation(fixture.request().operation_id),
               previous);
  TOS_CHECK_EQ(fixture.runtime().commit_count(), std::size_t{5});
}

// ---------------------------------------------------------------------------
// 5. Completion authority is bound to the coordinator epoch.
// ---------------------------------------------------------------------------

TOS_TEST(authority_stale_coordinator_epoch_is_rejected_by_completion) {
  AuthorityFixture fixture;
  const CoordinatorEpoch epoch = fixture.scheduler().coordinator_epoch();
  TOS_CHECK(epoch.published());

  const PlanResult planned = fixture.plan();
  TOS_REQUIRE(planned.planned);
  ExecutionPlan plan = planned.plan;
  TOS_CHECK_EQ(plan.binding.coordinator_epoch, epoch);

  // Hold the completion back so the attempt stays non-terminal and the effect of a
  // rejected submission on the ledger is observable.
  fixture.runtime().channel().inject_fault(ChannelFault::kDropCompletion, 1);
  const DispatchResult dispatched = fixture.scheduler().dispatch(plan);
  TOS_REQUIRE(dispatched.dispatched);

  ExecutionAttempt before;
  TOS_REQUIRE(fixture.scheduler().attempts().get(plan.attempt, before));
  TOS_CHECK_EQ(before.state, AttemptState::kDispatched);
  TOS_CHECK(!before.completion_committed);

  CompletionSubmission submission;
  submission.attempt = plan.attempt;
  submission.generation = plan.attempt_generation;
  submission.dispatch = plan.dispatch;
  submission.completion = CompletionId(1);
  submission.worker = plan.binding.worker;
  submission.worker_boot = plan.binding.worker_boot;
  submission.coordinator_epoch = CoordinatorEpoch(epoch.value() + 1);  // foreign epoch
  submission.domain_generation = plan.binding.domain_generation;
  submission.capability_generation = plan.binding.capability_generation;
  submission.operation = plan.operation;
  submission.provenance = plan.provenance;
  submission.result.success = true;
  submission.result.bytes_processed = fixture.payload().size();

  const CompletionOutcome stale = fixture.scheduler().complete(submission);
  TOS_CHECK_EQ(stale.rejection, CompletionRejection::kStaleCoordinatorEpoch);
  TOS_CHECK(!stale.committed);
  TOS_CHECK(!stale.idempotent);
  TOS_CHECK_EQ(stale.attempt.id, plan.attempt);

  ExecutionAttempt after;
  TOS_REQUIRE(fixture.scheduler().attempts().get(plan.attempt, after));
  TOS_CHECK_MSG(after.state == before.state, "a rejected completion must not move the attempt");
  TOS_CHECK_EQ(after.state, AttemptState::kDispatched);
  TOS_CHECK(!after.completion_committed);
  TOS_CHECK_EQ(fixture.runtime().commit_count(), std::size_t{0});

  // Control: the same submission carrying the real epoch is authoritative, so the
  // rejection above is about the epoch binding and nothing else.
  submission.coordinator_epoch = epoch;
  const CompletionOutcome accepted = fixture.scheduler().complete(submission);
  TOS_CHECK_EQ(accepted.rejection, CompletionRejection::kAccepted);
  TOS_CHECK(accepted.committed);
  ExecutionAttempt committed;
  TOS_REQUIRE(fixture.scheduler().attempts().get(plan.attempt, committed));
  TOS_CHECK_EQ(committed.state, AttemptState::kCompleted);
  TOS_CHECK(committed.completion_committed);
}
