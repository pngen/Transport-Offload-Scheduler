// Conservative restart recovery through the Scheduler facade.
//
// Durable identities survive a restart; operational truth does not. A recovered
// domain has no current evidence, a recovered worker boot is never current again,
// and an attempt that was in flight at the checkpoint is never promoted to SUCCESS.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "tos_test_support.hpp"

using namespace tos;

namespace {

tos_test::TestRuntime::Config persisted_config(const std::string& path) {
  tos_test::TestRuntime::Config config;
  config.enable_persistence = true;
  config.state_path = path;
  return config;
}

/// Plan, reserve, dispatch and wait for one operation to commit.
ExecutionAttempt commit_one(tos_test::TestRuntime& runtime, std::size_t payload_bytes,
                            std::uint32_t seed) {
  const std::vector<std::uint8_t> payload = tos_test::make_payload(payload_bytes, seed);
  OperationRequest request = tos_test::make_request(opclass::checksum_crc32c(), payload.size());
  const DispatchOutcome outcome = runtime.scheduler().plan_reserve_dispatch(request, payload);
  if (!outcome.plan_result.planned || !outcome.dispatch_result.dispatched) {
    TOS_CHECK_MSG(false, "plan_reserve_dispatch failed: " + outcome.plan_result.status.code + "/" +
                             outcome.dispatch_result.status.code);
    return ExecutionAttempt{};
  }
  const ExecutionAttemptId attempt = outcome.plan_result.plan.attempt;
  runtime.wait_for(attempt);
  const ExecutionAttempt terminal = runtime.wait_for_terminal(attempt);
  TOS_CHECK_EQ(terminal.state, AttemptState::kCompleted);
  TOS_CHECK(terminal.completion_committed);
  return terminal;
}

PersistedState durable_state_from(tos_test::TestRuntime& runtime, std::uint64_t generation) {
  PersistedState state;
  state.generation = PersistenceGeneration(generation);
  state.last_epoch = runtime.scheduler().coordinator_epoch();
  state.policy = runtime.scheduler().policy();
  state.has_policy = true;
  state.policy_generation = runtime.scheduler().policy_generation();
  // Exactly what save_state() persists: the live registry records, not the
  // backend's configuration copies (whose generation is deliberately lower).
  state.domains = runtime.scheduler().domains().all();
  state.attempts = runtime.scheduler().attempts().list(64);
  return state;
}

const ExecutionDomainRecord* find_domain(const std::vector<ExecutionDomainRecord>& records,
                                         ExecutionDomainId id) {
  for (const ExecutionDomainRecord& record : records) {
    if (record.id == id) return &record;
  }
  return nullptr;
}

/// Structured eligibility reason recorded for one candidate domain.
IneligibilityReason candidate_reason(const PlanResult& result, ExecutionDomainId id) {
  for (const CandidateEvaluation& candidate : result.explanation.candidates) {
    if (candidate.domain == id) return candidate.reason;
  }
  TOS_CHECK_MSG(false, "candidate domain was not evaluated at all");
  return IneligibilityReason::kNone;
}

/// True when the report carries a revalidation discrepancy for the domain.
bool report_flags_domain(const ReconciliationReport& report, ExecutionDomainId id) {
  for (const ReconciliationItem& item : report.items) {
    if (item.domain != id) continue;
    if (item.kind == DiscrepancyKind::kCapabilityChanged ||
        item.kind == DiscrepancyKind::kDomainGenerationChanged ||
        item.kind == DiscrepancyKind::kGenerationRegressed) {
      return true;
    }
  }
  return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// A clean restart: identity survives, evidence does not.
// ---------------------------------------------------------------------------

TOS_TEST(recovery_is_conservative_and_revalidated) {
  const std::string path = "tos_recovery_main.state";
  std::remove(path.c_str());

  ExecutionDomainId dpu;
  ExecutionDomainId smartnic;
  WorkerId worker;
  WorkerBootId boot;
  ExecutionAttemptId attempt;
  CoordinatorEpoch epoch_before;
  ExecutionResultPayload committed_result;
  bool saved = false;

  {
    tos_test::TestRuntime runtime(persisted_config(path));
    dpu = runtime.add_domain(
        make_synthetic_domain(ExecutionDomainId(101), ExecutionDomainType::kDpu, "dpu.recovery"));
    smartnic = runtime.add_domain(make_synthetic_domain(ExecutionDomainId(102),
                                                       ExecutionDomainType::kSmartNic,
                                                       "smartnic.recovery"));
    TOS_REQUIRE(dpu.valid() && smartnic.valid());

    ExecutionDomainRecord registered;
    TOS_REQUIRE(runtime.scheduler().domains().get(dpu, registered));
    worker = registered.worker;
    boot = registered.worker_boot;
    TOS_REQUIRE(worker.valid() && boot.valid());

    const ExecutionAttempt terminal = commit_one(runtime, 4096, 17);
    TOS_REQUIRE(terminal.id.valid());
    attempt = terminal.id;
    committed_result = terminal.result;
    epoch_before = runtime.scheduler().coordinator_epoch();
    TOS_REQUIRE(epoch_before.published());
    saved = runtime.scheduler().save_state().ok;
  }
  TOS_REQUIRE(saved);

  tos_test::TestRuntime restored(persisted_config(path));
  Scheduler& scheduler = restored.scheduler();

  // (a) The coordinator epoch strictly advanced: recovery is a new incarnation.
  TOS_CHECK_MSG(scheduler.coordinator_epoch() > epoch_before,
                "recovery must advance the coordinator epoch");

  // (b) Durable domain identities were restored.
  TOS_CHECK(scheduler.domains().size() >= static_cast<std::size_t>(2));
  ExecutionDomainRecord recovered_dpu;
  ExecutionDomainRecord recovered_smartnic;
  TOS_REQUIRE(scheduler.domains().get(dpu, recovered_dpu));
  TOS_REQUIRE(scheduler.domains().get(smartnic, recovered_smartnic));
  TOS_CHECK_EQ(recovered_dpu.type, ExecutionDomainType::kDpu);
  TOS_CHECK_EQ(recovered_dpu.name, std::string("dpu.recovery"));
  TOS_CHECK_EQ(recovered_smartnic.name, std::string("smartnic.recovery"));
  TOS_CHECK_EQ(recovered_smartnic.type, ExecutionDomainType::kSmartNic);

  // (c) No recovered domain has current dynamic evidence.
  TOS_CHECK_MSG(!recovered_dpu.load.published(),
                "recovered load/health evidence must not be current authority");
  TOS_CHECK_MSG(!recovered_smartnic.load.published(),
                "recovered load/health evidence must not be current authority");
  TOS_CHECK_EQ(scheduler.snapshot().totals.domains_with_current_evidence,
               static_cast<std::uint64_t>(0));

  const std::vector<std::uint8_t> payload = tos_test::make_payload(2048, 23);
  const OperationRequest request = tos_test::make_request(opclass::checksum_crc32c(), payload.size());
  const PlanResult before_evidence = scheduler.plan(request);
  TOS_CHECK_MSG(!before_evidence.planned,
                "a recovered domain without fresh evidence must not be selectable");
  if (before_evidence.planned) {
    TOS_CHECK(before_evidence.plan.domain != dpu);
    TOS_CHECK(before_evidence.plan.domain != smartnic);
  }
  TOS_CHECK_EQ(before_evidence.explanation.evaluated_candidates, static_cast<std::uint64_t>(2));
  TOS_CHECK_EQ(before_evidence.explanation.rejected_candidates, static_cast<std::uint64_t>(2));
  TOS_CHECK_EQ(candidate_reason(before_evidence, dpu), IneligibilityReason::kWorkerBootStale);
  TOS_CHECK_EQ(candidate_reason(before_evidence, smartnic), IneligibilityReason::kWorkerBootStale);

  // 3. Reconciliation before any new evidence is published.
  const ReconciliationReport report = scheduler.reconcile();
  TOS_CHECK_MSG(report.conservative, "recovery reconciliation must be conservative");
  TOS_CHECK_EQ(report.discrepancies, static_cast<std::uint64_t>(report.items.size()));
  TOS_CHECK_EQ(report.domains_compared, static_cast<std::uint64_t>(2));
  TOS_CHECK_EQ(report.domains_matched, static_cast<std::uint64_t>(0));
  TOS_CHECK(report.count_of(DiscrepancyKind::kCapabilityChanged) >= 2ULL);
  TOS_CHECK_MSG(report_flags_domain(report, dpu),
                "the restored DPU domain must be reported as changed");
  TOS_CHECK_MSG(report_flags_domain(report, smartnic),
                "the restored SmartNIC domain must be reported as changed");

  // (d) Recovered worker boots are fenced, not current.
  TOS_CHECK_MSG(!scheduler.workers().is_current(worker, boot),
                "a recovered worker incarnation must not be current");
  TOS_CHECK(scheduler.workers().is_fenced(worker, boot));
  const Status same_boot = scheduler.register_worker_boot(worker, boot, "recovery replay");
  TOS_CHECK(!same_boot.ok);
  TOS_CHECK_EQ(same_boot.code, std::string("worker.boot_fenced"));

  // (f) The recovered completed attempt keeps its committed result.
  ExecutionAttempt recovered_attempt;
  TOS_REQUIRE(scheduler.attempts().get(attempt, recovered_attempt));
  TOS_CHECK_EQ(recovered_attempt.state, AttemptState::kCompleted);
  TOS_CHECK(recovered_attempt.completion_committed);
  TOS_CHECK_EQ(recovered_attempt.resolution, AttemptResolution::kAuthoritativeSuccess);
  TOS_CHECK(recovered_attempt.result.success);
  TOS_CHECK_EQ(recovered_attempt.result.result_digest, committed_result.result_digest);
  TOS_CHECK_EQ(recovered_attempt.result.bytes_processed, committed_result.bytes_processed);
  TOS_CHECK_EQ(recovered_attempt.coordinator_epoch.value(), epoch_before.value());
  TOS_CHECK(scheduler.attempts().list_live().empty());

  // 4. The epoch guard: an old-epoch completion is refused by the new incarnation.
  CompletionSubmission replay;
  replay.attempt = recovered_attempt.id;
  replay.generation = recovered_attempt.generation;
  replay.dispatch = recovered_attempt.dispatch;
  replay.completion = CompletionId(recovered_attempt.id.value());
  replay.worker = recovered_attempt.worker;
  replay.worker_boot = recovered_attempt.worker_boot;
  replay.coordinator_epoch = recovered_attempt.coordinator_epoch;  // the previous epoch
  replay.domain_generation = recovered_attempt.domain_generation;
  replay.capability_generation = recovered_attempt.capability_generation;
  replay.operation = recovered_attempt.operation;
  replay.provenance = recovered_attempt.provenance;
  replay.result = recovered_attempt.result;
  const CompletionOutcome replay_outcome = scheduler.complete(replay);
  TOS_CHECK(!replay_outcome.committed);
  TOS_CHECK_EQ(replay_outcome.rejection, CompletionRejection::kStaleCoordinatorEpoch);
  ExecutionAttempt after_replay;
  TOS_REQUIRE(scheduler.attempts().get(attempt, after_replay));
  TOS_CHECK_EQ(after_replay.state, AttemptState::kCompleted);
  TOS_CHECK(after_replay.completion_committed);

  // (e) A new worker incarnation is necessary but not sufficient: planning stays
  // blocked until the domain publishes fresh capability/load/capacity evidence.
  const WorkerBootId new_boot(0x5EEDB007ULL);
  TOS_REQUIRE(scheduler.register_worker_boot(worker, new_boot, "restarted worker").ok);
  TOS_CHECK(scheduler.workers().is_current(worker, new_boot));

  ExecutionDomainRecord revived = recovered_dpu;
  revived.worker = worker;
  revived.worker_boot = new_boot;
  TOS_REQUIRE(scheduler.update_domain(revived).ok());

  const PlanResult before_republish = scheduler.plan(request);
  TOS_CHECK_MSG(!before_republish.planned,
                "a live worker incarnation alone must not make recovered evidence current");
  TOS_CHECK_EQ(candidate_reason(before_republish, dpu), IneligibilityReason::kHealthNotReady);
  TOS_CHECK_EQ(candidate_reason(before_republish, smartnic), IneligibilityReason::kWorkerBootStale);

  SyntheticDomainConfig republished =
      make_synthetic_domain(dpu, ExecutionDomainType::kDpu, "dpu.recovery");
  republished.worker = worker;
  republished.worker_boot = new_boot;
  republished.domain_generation = ExecutionDomainGeneration(2);
  republished.capability_generation = CapabilityGeneration(2);
  republished.evidence_generation = EvidenceGeneration(2);
  TOS_REQUIRE(restored.backend().add_domain(republished).ok);
  const std::vector<ExecutionDomainRecord> fresh_records = restored.backend().discover_domains();
  const ExecutionDomainRecord* fresh = find_domain(fresh_records, dpu);
  TOS_REQUIRE(fresh != nullptr);
  TOS_REQUIRE(scheduler.publish_capability(fresh->capability).ok);
  TOS_REQUIRE(scheduler.publish_load(dpu, fresh->load).ok);
  TOS_REQUIRE(scheduler.publish_capacity(dpu, fresh->capacity).ok);

  const PlanResult after_evidence = scheduler.plan(request);
  TOS_REQUIRE(after_evidence.planned);
  TOS_CHECK_EQ(after_evidence.plan.domain, dpu);
  TOS_CHECK_EQ(after_evidence.plan.domain_type, ExecutionDomainType::kDpu);

  // The revalidated domain really executes again.
  const DispatchOutcome outcome = scheduler.plan_reserve_dispatch(request, payload);
  TOS_REQUIRE(outcome.plan_result.planned);
  TOS_REQUIRE(outcome.dispatch_result.dispatched);
  TOS_CHECK_EQ(outcome.plan_result.plan.domain, dpu);
  const ExecutionAttemptId second = outcome.plan_result.plan.attempt;
  restored.wait_for(second);
  const ExecutionAttempt second_terminal = restored.wait_for_terminal(second);
  TOS_CHECK_EQ(second_terminal.state, AttemptState::kCompleted);
  TOS_CHECK(second_terminal.coordinator_epoch > epoch_before);
  TOS_CHECK(scheduler.attempts().get(attempt, after_replay));
  TOS_CHECK_EQ(after_replay.state, AttemptState::kCompleted);

  TOS_REQUIRE(scheduler.save_state().ok);
  TOS_REQUIRE(scheduler.shutdown().ok);
  std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// An in-flight attempt at the checkpoint is never promoted to SUCCESS.
// ---------------------------------------------------------------------------

TOS_TEST(recovery_never_promotes_an_in_flight_attempt) {
  const std::string path = "tos_recovery_inflight.state";
  std::remove(path.c_str());

  ExecutionAttemptId attempt;
  ExecutionDomainId domain;
  bool had_outstanding_reservation = false;

  {
    tos_test::TestRuntime::Config config = persisted_config(path);
    config.policy = make_default_policy();
    config.policy.reservation.enabled = true;
    config.policy.reservation
        .per_operation[static_cast<std::size_t>(ResourceKind::kExecutionSlot)] = 1;
    tos_test::TestRuntime runtime(config);
    domain = runtime.add_domain(
        make_synthetic_domain(ExecutionDomainId(201), ExecutionDomainType::kDpu, "dpu.inflight"));
    TOS_REQUIRE(domain.valid());

    // The execution happens, but no completion is ever reported.
    runtime.channel().inject_fault(ChannelFault::kAmbiguousCompletion, 1);
    const std::vector<std::uint8_t> payload = tos_test::make_payload(1024, 29);
    const OperationRequest request =
        tos_test::make_request(opclass::checksum_crc32c(), payload.size());
    const DispatchOutcome outcome = runtime.scheduler().plan_reserve_dispatch(request, payload);
    TOS_REQUIRE(outcome.plan_result.planned);
    TOS_REQUIRE(outcome.dispatch_result.dispatched);
    attempt = outcome.plan_result.plan.attempt;
    TOS_CHECK(outcome.plan_result.plan.reservation.valid());

    ExecutionAttempt live;
    TOS_REQUIRE(runtime.scheduler().attempts().get(attempt, live));
    TOS_CHECK_EQ(live.state, AttemptState::kDispatched);
    TOS_CHECK(!live.completion_committed);
    TOS_CHECK_MSG(runtime.scheduler().reservations().outstanding_count() >= 1ULL,
                  "a dispatched attempt holds its committed reservation");
    had_outstanding_reservation = runtime.scheduler().reservations().outstanding_count() >= 1ULL;

    TOS_REQUIRE(runtime.scheduler().save_state().ok);

    // The checkpointed record is classified conservatively.
    const StateStore store(path);
    const Checked<PersistedState> saved = store.load();
    TOS_REQUIRE(saved.ok());
    const ExecutionAttempt* durable = nullptr;
    for (const ExecutionAttempt& record : saved.value.attempts) {
      if (record.id == attempt) durable = &record;
    }
    TOS_REQUIRE(durable != nullptr);
    TOS_CHECK_MSG(durable->state == AttemptState::kOutcomeUnknown ||
                      durable->state == AttemptState::kFenced,
                  "an in-flight attempt must be persisted as OUTCOME_UNKNOWN or FENCED, got " +
                      std::string(to_string(durable->state)));
    TOS_CHECK(durable->state != AttemptState::kCompleted);
    TOS_CHECK(!durable->completion_committed);
    TOS_CHECK(!durable->result.success);
  }
  TOS_REQUIRE(had_outstanding_reservation);

  {
    tos_test::TestRuntime runtime(persisted_config(path));
    ExecutionAttempt recovered;
    TOS_REQUIRE(runtime.scheduler().attempts().get(attempt, recovered));
    TOS_CHECK_MSG(recovered.state != AttemptState::kCompleted,
                  "recovery must never turn an in-flight attempt into SUCCESS");
    TOS_CHECK(!recovered.completion_committed);
    TOS_CHECK(!recovered.result.success);
    TOS_CHECK(recovered.state == AttemptState::kOutcomeUnknown ||
              recovered.state == AttemptState::kFenced ||
              recovered.state == AttemptState::kCancelled);
    TOS_CHECK(runtime.scheduler().attempts().list_live().empty());

    // Reservations are not durable authority: nothing may remain held.
    TOS_CHECK_EQ(runtime.scheduler().reservations().outstanding_count(),
                 static_cast<std::uint64_t>(0));
    TOS_CHECK_EQ(runtime.scheduler().snapshot().totals.outstanding_reservations,
                 static_cast<std::uint64_t>(0));
    const ReservationAudit audit = runtime.scheduler().reservations().audit();
    TOS_CHECK(audit.consistent);
    TOS_CHECK_EQ(audit.outstanding, static_cast<std::uint64_t>(0));
    TOS_CHECK_EQ(audit.committed, static_cast<std::uint64_t>(0));
    TOS_CHECK_EQ(audit.leaked, static_cast<std::uint64_t>(0));

    // The recovered domain is durable identity only, with no live evidence.
    ExecutionDomainRecord record;
    TOS_REQUIRE(runtime.scheduler().domains().get(domain, record));
    TOS_CHECK(!record.load.published());
    TOS_CHECK(!runtime.scheduler().plan(tos_test::make_request(opclass::checksum_crc32c(), 1024)).planned);
  }

  std::remove(path.c_str());
  std::cout << "recovery in-flight classification verified" << std::endl;
}

// ---------------------------------------------------------------------------
// A refused load never applies part of the durable state.
//
// KNOWN DEFECT (this test is expected to fail until the runtime is fixed):
//   Symptom   Scheduler::load_state_from() leaves attempt records it already
//             restored in the ledger when it refuses the load later in the same
//             call, so a failed load is partially applied.
//   Repro     a ledger holding attempt A, plus a structurally valid durable file
//             whose attempts vector is [B, A] with B an identity the ledger does
//             not own and B < A. The load restores B, refuses A with
//             "attempt.duplicate_identity", and B stays visible in the ledger.
//   Cause     src/core/scheduler.cpp: only the domain generation rollback is
//             validated up front; the loop over state.attempts calls
//             AttemptLedger::restore_attempt() one record at a time and returns on
//             the first failure, with no pre-flight identity check and no rollback
//             of what was already inserted.
//   Contract  The function's own comment states "Complete validation happens
//             before any mutation"; a refused load must leave the runtime exactly
//             as it was.
// ---------------------------------------------------------------------------

TOS_TEST(recovery_never_partially_applies_a_refused_state) {
  const std::string path = "tos_recovery_partial.state";
  std::remove(path.c_str());

  tos_test::TestRuntime runtime;
  TOS_REQUIRE(runtime
                  .add_domain(make_synthetic_domain(ExecutionDomainId(301), ExecutionDomainType::kDpu,
                                                    "dpu.partial"))
                  .valid());
  TOS_REQUIRE(commit_one(runtime, 1024, 31).id.valid());
  TOS_REQUIRE(commit_one(runtime, 1024, 32).id.valid());
  const std::vector<ExecutionAttempt> live = runtime.scheduler().attempts().list(16);
  TOS_REQUIRE(live.size() == static_cast<std::size_t>(2));

  // A durable file that is structurally valid but carries one attempt identity the
  // live ledger has never seen, ordered before two that it already owns.
  PersistedState state = durable_state_from(runtime, 2);
  ExecutionAttempt unknown = live.front();
  unknown.id = ExecutionAttemptId(1ULL << 56);
  TOS_REQUIRE(unknown.id.valid());
  TOS_REQUIRE(unknown.id < live.front().id);
  state.attempts = {unknown, live[0], live[1]};
  TOS_REQUIRE(StateStore::decode(StateStore::encode(state)).ok());

  const StateStore store(path);
  TOS_REQUIRE(store.save(state).ok);

  const std::uint64_t attempts_before = runtime.scheduler().attempts().audit().total;
  const std::uint64_t live_before = runtime.scheduler().attempts().live_count();
  const CoordinatorEpoch epoch_before = runtime.scheduler().coordinator_epoch();
  const Status refused = runtime.scheduler().load_state_from(path);
  TOS_CHECK_MSG(!refused.ok, "a state colliding with live attempt identities must be refused");
  TOS_CHECK_MSG(refused.code == "attempt.duplicate_identity",
                "refused load code=" + refused.code + " message=" + refused.message);

  // The refusal must be total: no record from the file may have been applied.
  ExecutionAttempt leaked;
  TOS_CHECK_MSG(!runtime.scheduler().attempts().get(unknown.id, leaked),
                "a refused load restored an attempt record: the state was partially applied");
  TOS_CHECK_EQ(runtime.scheduler().attempts().audit().total, attempts_before);
  TOS_CHECK_EQ(runtime.scheduler().attempts().live_count(), live_before);
  TOS_CHECK_EQ(runtime.scheduler().coordinator_epoch().value(), epoch_before.value());
  TOS_CHECK_EQ(runtime.scheduler().reconcile().domains_compared, static_cast<std::uint64_t>(0));

  TOS_REQUIRE(store.remove().ok);
}
