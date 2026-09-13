// Completion authority: at most one authoritative completion per attempt generation,
// and every binding field is checked before an attempt record may change.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "tos_test_support.hpp"

using namespace tos;

namespace {

constexpr std::uint64_t kDigest = 0x1122334455667788ULL;
constexpr std::uint64_t kBytesProcessed = 4096;

/// Canonical encoding of one attempt record. Two records are identical exactly when
/// their encodings are: this is a full-field snapshot, not a spot check.
std::vector<std::uint8_t> encoded_attempt(const ExecutionAttempt& attempt) {
  PersistedState state;
  state.generation = PersistenceGeneration::first();
  state.attempts.push_back(attempt);
  return StateStore::encode(state);
}

void expect_unchanged(const std::vector<std::uint8_t>& before, const ExecutionAttempt& after,
                      const std::string& what) {
  const std::vector<std::uint8_t> now = encoded_attempt(after);
  TOS_CHECK_MSG(before == now, what + ": the attempt record changed");
}

void expect_rejection(const CompletionOutcome& outcome, CompletionRejection expected,
                      const std::string& what) {
  TOS_CHECK_MSG(outcome.rejection == expected,
                what + ": expected '" + std::string(to_string(expected)) + "', got '" +
                    std::string(to_string(outcome.rejection)) + "'");
}

/// One attempt registered (and optionally dispatched) directly through the ledger,
/// plus a submission whose binding fields match it exactly.
struct BoundAttempt {
  ExecutionAttemptId id;
  ExecutionAttemptGeneration generation;
  DispatchId dispatch;
  AttemptRegistration registration;
  CompletionSubmission submission;
};

BoundAttempt bind_attempt(AttemptLedger& ledger, CoordinatorEpoch epoch) {
  BoundAttempt bound;
  bound.id = ledger.allocate_id();

  bound.registration.id = bound.id;
  bound.registration.operation = TransportOperationId(4242);
  bound.registration.operation_class = opclass::checksum_crc32c();
  bound.registration.side_effect = SideEffectClass::kPure;
  bound.registration.domain = ExecutionDomainId(21);
  bound.registration.domain_type = ExecutionDomainType::kDpu;
  bound.registration.domain_generation = ExecutionDomainGeneration(3);
  bound.registration.capability_generation = CapabilityGeneration(5);
  bound.registration.worker = WorkerId(31);
  bound.registration.worker_boot = WorkerBootId(37);
  bound.registration.coordinator_epoch = epoch;
  bound.registration.dispatch = DispatchId(47);
  bound.registration.provenance = Provenance::kSynthetic;
  bound.generation = ledger.allocate_generation(bound.registration.operation);
  bound.registration.generation = bound.generation;
  bound.dispatch = bound.registration.dispatch;

  bound.submission.attempt = bound.id;
  bound.submission.generation = bound.generation;
  bound.submission.dispatch = bound.dispatch;
  bound.submission.completion = CompletionId(53);
  bound.submission.worker = bound.registration.worker;
  bound.submission.worker_boot = bound.registration.worker_boot;
  bound.submission.coordinator_epoch = bound.registration.coordinator_epoch;
  bound.submission.domain_generation = bound.registration.domain_generation;
  bound.submission.capability_generation = bound.registration.capability_generation;
  bound.submission.operation = bound.registration.operation;
  bound.submission.provenance = bound.registration.provenance;
  bound.submission.result.success = true;
  bound.submission.result.result_digest = kDigest;
  bound.submission.result.bytes_processed = kBytesProcessed;
  bound.submission.result.duration_ns = 1000;
  bound.submission.result.backend_detail = "manual completion";
  return bound;
}

/// Register the attempt and mark it dispatched, so a completion is legal.
bool register_dispatched(AttemptLedger& ledger, const BoundAttempt& bound) {
  if (!ledger.register_attempt(bound.registration).ok) return false;
  return ledger.mark_dispatched(bound.id, bound.dispatch).ok;
}

std::size_t count_committed_results(const AttemptLedger& ledger) {
  std::size_t committed = 0;
  for (const ExecutionAttempt& attempt : ledger.list(1024)) {
    if (attempt.completion_committed) ++committed;
  }
  return committed;
}

}  // namespace

// 1. A successful completion is authoritative, and the ledger stays consistent.
TOS_TEST(completion_commits_at_most_once_per_generation) {
  tos_test::TestRuntime runtime;
  runtime.add_domain(make_synthetic_domain(ExecutionDomainId(700), ExecutionDomainType::kDpu,
                                           "dpu.completion"));

  const std::vector<std::uint8_t> payload = tos_test::make_payload(2048, 3);
  OperationRequest request = tos_test::make_request(opclass::checksum_crc32c(), payload.size());
  const DispatchOutcome dispatched = runtime.scheduler().plan_reserve_dispatch(
      request, std::span<const std::uint8_t>(payload.data(), payload.size()));
  TOS_REQUIRE(dispatched.plan_result.planned);
  TOS_REQUIRE(dispatched.dispatch_result.dispatched);
  const ExecutionAttemptId attempt = dispatched.plan_result.plan.attempt;

  runtime.wait_for(attempt);
  const ExecutionAttempt settled = runtime.wait_for_terminal(attempt);
  TOS_CHECK_EQ(settled.state, AttemptState::kCompleted);
  TOS_CHECK(settled.completion_committed);
  TOS_CHECK_EQ(settled.resolution, AttemptResolution::kAuthoritativeSuccess);
  TOS_CHECK_EQ(settled.result.result_digest,
               static_cast<std::uint64_t>(crc32c(payload.data(), payload.size())));
  TOS_CHECK_EQ(settled.result.bytes_processed, static_cast<std::uint64_t>(payload.size()));
  TOS_CHECK_EQ(runtime.commit_count(), std::size_t{1});
  TOS_CHECK_EQ(runtime.deliveries(attempt), std::size_t{1});

  const AttemptLedger& ledger = runtime.scheduler().attempts();
  const AttemptAudit audit = ledger.audit();
  TOS_CHECK_MSG(audit.consistent, audit.detail);
  TOS_CHECK_EQ(audit.completed, std::uint64_t{1});
  TOS_CHECK_EQ(audit.multiple_completions, std::uint64_t{0});
  TOS_CHECK_EQ(count_committed_results(ledger), std::size_t{1});
  TOS_CHECK_EQ(ledger.highest_generation(settled.operation), settled.generation);
}

// 2. A byte-identical duplicate is an idempotent replay, not a second result.
TOS_TEST(completion_duplicate_identical_is_idempotent) {
  tos_test::TestRuntime runtime;
  AttemptLedger& ledger = runtime.scheduler().attempts();
  const BoundAttempt bound = bind_attempt(ledger, runtime.scheduler().coordinator_epoch());
  TOS_REQUIRE(register_dispatched(ledger, bound));

  const CompletionOutcome first = runtime.scheduler().complete(bound.submission);
  TOS_CHECK_EQ(first.rejection, CompletionRejection::kAccepted);
  TOS_REQUIRE(first.committed);
  TOS_CHECK(!first.idempotent);
  const std::vector<std::uint8_t> committed_bytes = encoded_attempt(first.attempt);

  const CompletionOutcome replay = runtime.scheduler().complete(bound.submission);
  expect_rejection(replay, CompletionRejection::kDuplicateIdentical, "identical duplicate");
  TOS_CHECK_MSG(replay.committed, "an identical duplicate is still authoritative");
  TOS_CHECK_MSG(replay.idempotent, "an identical duplicate must be idempotent");

  ExecutionAttempt stored;
  TOS_REQUIRE(ledger.get(bound.id, stored));
  expect_unchanged(committed_bytes, stored, "identical duplicate");
  TOS_CHECK_EQ(stored.state, AttemptState::kCompleted);
  TOS_CHECK_EQ(stored.result.result_digest, kDigest);

  const AttemptAudit audit = ledger.audit();
  TOS_CHECK_MSG(audit.consistent, audit.detail);
  TOS_CHECK_EQ(audit.completed, std::uint64_t{1});
  TOS_CHECK_EQ(audit.multiple_completions, std::uint64_t{0});
  TOS_CHECK_EQ(count_committed_results(ledger), std::size_t{1});
}

// 3. A conflicting duplicate is rejected and never overwrites the committed result.
TOS_TEST(completion_conflicting_duplicate_is_rejected) {
  tos_test::TestRuntime runtime;
  AttemptLedger& ledger = runtime.scheduler().attempts();
  const BoundAttempt bound = bind_attempt(ledger, runtime.scheduler().coordinator_epoch());
  TOS_REQUIRE(register_dispatched(ledger, bound));

  const CompletionOutcome first = runtime.scheduler().complete(bound.submission);
  TOS_REQUIRE(first.committed);
  const std::vector<std::uint8_t> committed_bytes = encoded_attempt(first.attempt);

  CompletionSubmission different_digest = bound.submission;
  different_digest.result.result_digest = kDigest ^ 0xFFFFFFFFFFFFFFFFULL;
  const CompletionOutcome digest_outcome = runtime.scheduler().complete(different_digest);
  expect_rejection(digest_outcome, CompletionRejection::kConflictingDuplicate, "different digest");
  TOS_CHECK_MSG(!digest_outcome.committed, "a conflicting duplicate must not commit");
  TOS_CHECK_MSG(!digest_outcome.idempotent, "a conflicting duplicate is not an idempotent replay");

  CompletionSubmission different_success = bound.submission;
  different_success.result.success = false;
  const CompletionOutcome success_outcome = runtime.scheduler().complete(different_success);
  expect_rejection(success_outcome, CompletionRejection::kConflictingDuplicate,
                   "different success flag");
  TOS_CHECK_MSG(!success_outcome.committed, "a conflicting duplicate must not commit");

  ExecutionAttempt stored;
  TOS_REQUIRE(ledger.get(bound.id, stored));
  expect_unchanged(committed_bytes, stored, "conflicting duplicate");
  TOS_CHECK_EQ(stored.state, AttemptState::kCompleted);
  TOS_CHECK_EQ(stored.result.result_digest, kDigest);
  TOS_CHECK_EQ(stored.result.success, true);

  const AttemptAudit audit = ledger.audit();
  TOS_CHECK_MSG(audit.consistent, audit.detail);
  TOS_CHECK_EQ(audit.completed, std::uint64_t{1});
  TOS_CHECK_EQ(audit.multiple_completions, std::uint64_t{2});
  TOS_CHECK_EQ(count_committed_results(ledger), std::size_t{1});

  // The original completion is still the authoritative one and still replays idempotently.
  const CompletionOutcome replay = runtime.scheduler().complete(bound.submission);
  expect_rejection(replay, CompletionRejection::kDuplicateIdentical, "replay after conflicts");
  TOS_CHECK(replay.committed && replay.idempotent);
}

// 4. A completion for an attempt the ledger has never seen changes nothing.
TOS_TEST(completion_unknown_attempt_mutates_nothing) {
  tos_test::TestRuntime runtime;
  AttemptLedger& ledger = runtime.scheduler().attempts();
  const ExecutionAttemptId unknown = ledger.allocate_id();
  TOS_CHECK(unknown.valid());

  CompletionSubmission submission;
  submission.attempt = unknown;
  submission.generation = ExecutionAttemptGeneration(1);
  submission.dispatch = DispatchId(77);
  submission.completion = CompletionId(78);
  submission.worker = WorkerId(31);
  submission.worker_boot = WorkerBootId(37);
  submission.coordinator_epoch = runtime.scheduler().coordinator_epoch();
  submission.domain_generation = ExecutionDomainGeneration(3);
  submission.capability_generation = CapabilityGeneration(5);
  submission.operation = TransportOperationId(4242);
  submission.provenance = Provenance::kSynthetic;
  submission.result.success = true;
  submission.result.result_digest = kDigest;

  const CompletionOutcome outcome = runtime.scheduler().complete(submission);
  expect_rejection(outcome, CompletionRejection::kUnknownAttempt, "unknown attempt");
  TOS_CHECK_MSG(!outcome.committed, "an unknown attempt cannot commit");

  ExecutionAttempt stored;
  TOS_CHECK_MSG(!ledger.get(unknown, stored), "an unknown attempt must not be created");
  TOS_CHECK_EQ(ledger.live_count(), std::uint64_t{0});
  TOS_CHECK(ledger.list(16).empty());
  const AttemptAudit audit = ledger.audit();
  TOS_CHECK_MSG(audit.consistent, audit.detail);
  TOS_CHECK_EQ(audit.total, std::uint64_t{0});
  TOS_CHECK_EQ(runtime.scheduler().reservations().audit().outstanding, std::uint64_t{0});
}

// 5. Every binding field is checked before the record may change.
TOS_TEST(completion_binding_mismatches_are_rejected_without_mutation) {
  tos_test::TestRuntime runtime;
  AttemptLedger& ledger = runtime.scheduler().attempts();
  const CoordinatorEpoch epoch = runtime.scheduler().coordinator_epoch();

  struct Case {
    const char* label;
    CompletionRejection expected;
    std::function<void(CompletionSubmission&)> mutate;
  };

  const std::vector<Case> cases = {
      {"attempt generation",
       CompletionRejection::kAttemptGenerationMismatch,
       [](CompletionSubmission& s) { s.generation = ExecutionAttemptGeneration(s.generation.value() + 1); }},
      {"dispatch id",
       CompletionRejection::kDispatchIdMismatch,
       [](CompletionSubmission& s) { s.dispatch = DispatchId(s.dispatch.value() + 1); }},
      {"worker identity",
       CompletionRejection::kStaleWorkerBoot,
       [](CompletionSubmission& s) { s.worker = WorkerId(s.worker.value() + 1); }},
      {"worker boot",
       CompletionRejection::kStaleWorkerBoot,
       [](CompletionSubmission& s) { s.worker_boot = WorkerBootId(s.worker_boot.value() + 1); }},
      {"coordinator epoch",
       CompletionRejection::kStaleCoordinatorEpoch,
       [](CompletionSubmission& s) { s.coordinator_epoch = CoordinatorEpoch(s.coordinator_epoch.value() + 1); }},
      {"domain generation",
       CompletionRejection::kStaleDomainGeneration,
       [](CompletionSubmission& s) { s.domain_generation = ExecutionDomainGeneration(s.domain_generation.value() + 1); }},
      {"operation",
       CompletionRejection::kOperationMismatch,
       [](CompletionSubmission& s) { s.operation = TransportOperationId(s.operation.value() + 1); }},
      {"provenance",
       CompletionRejection::kProvenanceMismatch,
       [](CompletionSubmission& s) { s.provenance = Provenance::kReal; }},
      {"result larger than the payload bound",
       CompletionRejection::kResultTooLarge,
       [](CompletionSubmission& s) { s.result.bytes_processed = kMaxPayloadBytes + 1; }},
  };

  for (const Case& entry : cases) {
    BoundAttempt bound = bind_attempt(ledger, epoch);
    TOS_REQUIRE(register_dispatched(ledger, bound));
    ExecutionAttempt before;
    TOS_REQUIRE(ledger.get(bound.id, before));
    const std::vector<std::uint8_t> snapshot = encoded_attempt(before);

    CompletionSubmission submission = bound.submission;
    entry.mutate(submission);

    const CompletionOutcome outcome = ledger.commit_completion(submission);
    expect_rejection(outcome, entry.expected, entry.label);
    TOS_CHECK_MSG(!outcome.committed, std::string(entry.label) + ": must not commit");

    ExecutionAttempt after;
    TOS_REQUIRE(ledger.get(bound.id, after));
    expect_unchanged(snapshot, after, entry.label);
    TOS_CHECK_EQ(after.state, AttemptState::kDispatched);
    TOS_CHECK(!after.completion_committed);
  }

  const AttemptAudit audit = ledger.audit();
  TOS_CHECK_MSG(audit.consistent, audit.detail);
  TOS_CHECK_EQ(audit.completed, std::uint64_t{0});
  TOS_CHECK_EQ(count_committed_results(ledger), std::size_t{0});
}

// The scheduler facade rejects a stale coordinator epoch before consulting the ledger.
TOS_TEST(scheduler_facade_rejects_stale_coordinator_epoch) {
  tos_test::TestRuntime runtime;
  AttemptLedger& ledger = runtime.scheduler().attempts();
  BoundAttempt bound = bind_attempt(ledger, runtime.scheduler().coordinator_epoch());
  TOS_REQUIRE(register_dispatched(ledger, bound));

  ExecutionAttempt before;
  TOS_REQUIRE(ledger.get(bound.id, before));
  const std::vector<std::uint8_t> snapshot = encoded_attempt(before);

  CompletionSubmission stale = bound.submission;
  stale.coordinator_epoch = CoordinatorEpoch(stale.coordinator_epoch.value() + 1);
  const CompletionOutcome outcome = runtime.scheduler().complete(stale);
  expect_rejection(outcome, CompletionRejection::kStaleCoordinatorEpoch, "stale epoch facade");
  TOS_CHECK(!outcome.committed);

  ExecutionAttempt after;
  TOS_REQUIRE(ledger.get(bound.id, after));
  expect_unchanged(snapshot, after, "stale epoch facade");
}

// 6. Terminal attempts reject later completions; a fenced attempt reports its reason.
TOS_TEST(completion_terminal_and_fenced_attempts_reject_late_completions) {
  tos_test::TestRuntime runtime;
  AttemptLedger& ledger = runtime.scheduler().attempts();
  const CoordinatorEpoch epoch = runtime.scheduler().coordinator_epoch();

  // A failed attempt is terminal: no completion may resurrect it.
  {
    BoundAttempt bound = bind_attempt(ledger, epoch);
    TOS_REQUIRE(register_dispatched(ledger, bound));
    TOS_REQUIRE(ledger.mark_failed(bound.id, FailureKind::kExecutionFailure, "backend failed").ok);
    ExecutionAttempt before;
    TOS_REQUIRE(ledger.get(bound.id, before));
    TOS_CHECK_EQ(before.state, AttemptState::kFailed);
    const std::vector<std::uint8_t> snapshot = encoded_attempt(before);

    const CompletionOutcome outcome = ledger.commit_completion(bound.submission);
    expect_rejection(outcome, CompletionRejection::kAlreadyTerminal, "failed attempt");
    TOS_CHECK(!outcome.committed);
    ExecutionAttempt after;
    TOS_REQUIRE(ledger.get(bound.id, after));
    expect_unchanged(snapshot, after, "failed attempt");
    TOS_CHECK_EQ(after.state, AttemptState::kFailed);
  }

  // A cancelled attempt is terminal as well.
  {
    BoundAttempt bound = bind_attempt(ledger, epoch);
    TOS_REQUIRE(register_dispatched(ledger, bound));
    TOS_REQUIRE(ledger.mark_cancelled(bound.id, true, "cancelled after dispatch").ok);
    const CompletionOutcome outcome = ledger.commit_completion(bound.submission);
    expect_rejection(outcome, CompletionRejection::kAlreadyTerminal, "cancelled attempt");
  }

  // An attempt that was registered but never dispatched can still be classified terminal.
  {
    BoundAttempt bound = bind_attempt(ledger, epoch);
    TOS_REQUIRE(ledger.register_attempt(bound.registration).ok);
    TOS_REQUIRE(ledger.mark_failed(bound.id, FailureKind::kPreDispatchRejection, "refused").ok);
    const CompletionOutcome outcome = ledger.commit_completion(bound.submission);
    expect_rejection(outcome, CompletionRejection::kAlreadyTerminal, "never dispatched, failed");
  }

  // A fenced attempt is rejected as fenced, not as a generic terminal state.
  {
    BoundAttempt bound = bind_attempt(ledger, epoch);
    TOS_REQUIRE(register_dispatched(ledger, bound));
    TOS_REQUIRE(ledger.mark_fenced(bound.id, true, "domain authority withdrawn").ok);
    ExecutionAttempt before;
    TOS_REQUIRE(ledger.get(bound.id, before));
    TOS_CHECK_EQ(before.state, AttemptState::kFenced);
    const std::vector<std::uint8_t> snapshot = encoded_attempt(before);

    const CompletionOutcome outcome = ledger.commit_completion(bound.submission);
    expect_rejection(outcome, CompletionRejection::kDomainFenced, "fenced attempt");
    TOS_CHECK(!outcome.committed);
    ExecutionAttempt after;
    TOS_REQUIRE(ledger.get(bound.id, after));
    expect_unchanged(snapshot, after, "fenced attempt");
  }

  const AttemptAudit audit = ledger.audit();
  TOS_CHECK_MSG(audit.consistent, audit.detail);
  TOS_CHECK_EQ(audit.completed, std::uint64_t{0});
}

// 7a. A completion that arrives before the dispatch acknowledgement is still
// authoritative, and the late acknowledgement cannot regress the record.
TOS_TEST(completion_before_dispatch_ack_is_authoritative_once) {
  tos_test::TestRuntime runtime;
  AttemptLedger& ledger = runtime.scheduler().attempts();
  const BoundAttempt bound = bind_attempt(ledger, runtime.scheduler().coordinator_epoch());
  TOS_REQUIRE(ledger.register_attempt(bound.registration).ok);

  ExecutionAttempt registered;
  TOS_REQUIRE(ledger.get(bound.id, registered));
  TOS_CHECK_EQ(registered.state, AttemptState::kDispatching);

  const CompletionOutcome fast = ledger.commit_completion(bound.submission);
  TOS_CHECK_EQ(fast.rejection, CompletionRejection::kAccepted);
  TOS_REQUIRE(fast.committed);
  TOS_CHECK(fast.attempt.completion_committed);
  TOS_CHECK_EQ(fast.attempt.state, AttemptState::kCompleted);
  TOS_CHECK_EQ(fast.attempt.result.result_digest, kDigest);

  // The dispatch acknowledgement arrives after the completion: it must not win.
  const Status late = ledger.mark_dispatched(bound.id, bound.dispatch);
  TOS_CHECK_MSG(!late.ok, "a late dispatch acknowledgement must not regress a committed attempt");
  ExecutionAttempt after;
  TOS_REQUIRE(ledger.get(bound.id, after));
  TOS_CHECK_EQ(after.state, AttemptState::kCompleted);
  TOS_CHECK(after.completion_committed);

  // Exactly once: the replay is idempotent and adds no second committed result.
  const CompletionOutcome replay = ledger.commit_completion(bound.submission);
  expect_rejection(replay, CompletionRejection::kDuplicateIdentical, "fast completion replay");
  TOS_CHECK(replay.committed && replay.idempotent);
  const AttemptAudit audit = ledger.audit();
  TOS_CHECK_MSG(audit.consistent, audit.detail);
  TOS_CHECK_EQ(audit.completed, std::uint64_t{1});
  TOS_CHECK_EQ(count_committed_results(ledger), std::size_t{1});
}

// 7b. The same property across the real channel, with delivery ordering made explicit
// by withholding the completion until the test releases it.
TOS_TEST(delayed_completion_is_authoritative_once_end_to_end) {
  tos_test::TestRuntime runtime;
  runtime.add_domain(make_synthetic_domain(ExecutionDomainId(701), ExecutionDomainType::kDpu,
                                           "dpu.delayed"));
  runtime.channel().inject_fault(ChannelFault::kDelayedCompletion);

  const std::vector<std::uint8_t> payload = tos_test::make_payload(1024, 9);
  OperationRequest request = tos_test::make_request(opclass::checksum_crc32c(), payload.size());
  const DispatchOutcome dispatched = runtime.scheduler().plan_reserve_dispatch(
      request, std::span<const std::uint8_t>(payload.data(), payload.size()));
  TOS_REQUIRE(dispatched.plan_result.planned);
  TOS_REQUIRE(dispatched.dispatch_result.dispatched);
  const ExecutionAttemptId attempt = dispatched.plan_result.plan.attempt;

  // Nothing may be delivered while the completion is withheld.
  TOS_CHECK_EQ(runtime.total_deliveries(), std::size_t{0});
  TOS_CHECK_EQ(runtime.commit_count(), std::size_t{0});

  // Release the withheld completion as soon as the executor produces it: the wait ends
  // only when the completion exists, so a hang here is a defect rather than a missing
  // watchdog.
  std::size_t released = 0;
  while (released == 0) {
    released = runtime.channel().release_delayed();
    if (released == 0) std::this_thread::yield();
  }
  TOS_CHECK_EQ(released, std::size_t{1});

  runtime.wait_for(attempt);
  const ExecutionAttempt settled = runtime.wait_for_terminal(attempt);
  TOS_CHECK_EQ(settled.state, AttemptState::kCompleted);
  TOS_CHECK(settled.completion_committed);
  TOS_CHECK_EQ(settled.result.result_digest,
               static_cast<std::uint64_t>(crc32c(payload.data(), payload.size())));
  TOS_CHECK_EQ(runtime.deliveries(attempt), std::size_t{1});
  TOS_CHECK_EQ(runtime.commit_count(), std::size_t{1});
  TOS_CHECK_EQ(runtime.rejection_count(), std::size_t{0});

  const AttemptLedger& ledger = runtime.scheduler().attempts();
  TOS_CHECK_EQ(count_committed_results(ledger), std::size_t{1});
  const AttemptAudit audit = ledger.audit();
  TOS_CHECK_MSG(audit.consistent, audit.detail);
  TOS_CHECK_EQ(audit.completed, std::uint64_t{1});
}
