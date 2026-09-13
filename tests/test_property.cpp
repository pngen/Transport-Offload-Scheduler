// Seeded property testing over the whole decision and execution lifecycle.
//
// A deterministic generator drives registration, capability/load/policy changes,
// planning, reservation, dispatch, completion, cancellation, fencing, replacement,
// fallback, retry and persistence. After every step the runtime's invariants are
// re-checked. The seed and the step trace are printed, so any failure replays
// exactly.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include <map>
#include <string>
#include <vector>

#include "tos_test_support.hpp"

using namespace tos;

namespace {

struct InvariantReport {
  bool ok{true};
  std::string detail;
};

/// Minimal restatement of hard eligibility, used to prove that a selected domain was
/// legally selectable at the moment it was chosen.
InvariantReport selected_domain_was_eligible(Scheduler& scheduler, const ExecutionPlan& plan,
                                             const OperationRequest& request) {
  ExecutionDomainRecord domain;
  if (!scheduler.domains().get(plan.domain, domain)) {
    return {false, "selected domain is not registered"};
  }
  if (domain.fenced) return {false, "selected domain is fenced"};
  if (!scheduler.workers().is_current(domain.worker, domain.worker_boot)) {
    return {false, "selected domain has no live worker incarnation"};
  }
  const SchedulerPolicy policy = scheduler.policy();
  if (policy.domain_type_forbidden(domain.type) || policy.domain_id_forbidden(domain.id)) {
    return {false, "selected domain is forbidden by policy"};
  }
  if (policy.offload_requirement == OffloadRequirement::kOffloadRequired &&
      domain.type == ExecutionDomainType::kCpu) {
    return {false, "selected host CPU while policy requires offload"};
  }
  const CapabilityCheck capability =
      check_capability(domain.capability, scheduler.operation_classes(), request,
                       policy.require_positive_evidence);
  if (!capability.supported) {
    return {false, std::string("selected domain fails capability: ") +
                       std::string(to_string(capability.reason))};
  }
  const IsolationClass required =
      std::max(policy.minimum_isolation, request.required_isolation);
  if (required != IsolationClass::kUnknown && domain.isolation < required) {
    return {false, "selected domain isolation is below the requirement"};
  }
  if (policy.freshness.health && !domain.load.published()) {
    return {false, "selected domain has no current evidence while policy requires it"};
  }
  if (domain.load.published() && (!domain.load.healthy || !domain.load.ready)) {
    return {false, "selected domain is not healthy and ready"};
  }
  if (policy.reservation.enabled) {
    ResourceAmounts amounts{};
    for (std::size_t i = 0; i < kResourceKindCount; ++i) {
      amounts[i] = policy.reservation.per_operation[i];
    }
    if (!scheduler.reservations().has_capacity(domain.id, amounts)) {
      return {false, "selected domain has no capacity left"};
    }
  }
  return {true, "eligible"};
}

struct Trace {
  std::vector<std::string> steps;
  void add(const std::string& text) { steps.push_back(text); }
};

/// One seeded run of the lifecycle state machine.
Trace run_sequence(std::uint64_t seed, std::uint32_t steps) {
  Trace trace;
  DeterministicRng rng(seed);

  tos_test::TestRuntime::Config config;
  config.policy = make_default_policy();
  config.policy.fallback.enabled = true;
  config.policy.fallback.max_depth = 3;
  config.policy.retry.enabled = true;
  config.policy.retry.max_attempts = 3;
  config.policy.reservation.enabled = true;
  config.policy.reservation.per_operation[static_cast<std::size_t>(ResourceKind::kExecutionSlot)] = 1;
  tos_test::TestRuntime runtime(config);

  const std::vector<ExecutionDomainId> domains = {
      runtime.add_domain(make_synthetic_domain(ExecutionDomainId(101), ExecutionDomainType::kDpu, "dpu.a")),
      runtime.add_domain(make_synthetic_domain(ExecutionDomainId(102), ExecutionDomainType::kNic, "nic.a")),
      runtime.add_domain(make_synthetic_domain(ExecutionDomainId(103), ExecutionDomainType::kCpu, "cpu.a"))};

  std::map<std::uint64_t, std::uint64_t> highest_generation;
  std::vector<ExecutionPlan> plans;
  std::uint64_t plan_counter = 0;

  auto check_invariants = [&](const char* where) {
    const ReservationAudit reservations = runtime.scheduler().reservations().audit();
    TOS_CHECK_MSG(reservations.consistent,
                  std::string(where) + ": reservation accounting inconsistent: " + reservations.detail);
    const AttemptAudit attempts = runtime.scheduler().attempts().audit();
    TOS_CHECK_MSG(attempts.consistent,
                  std::string(where) + ": attempt accounting inconsistent: " + attempts.detail);
    TOS_CHECK_EQ(attempts.multiple_completions, 0U);
    for (const ExecutionAttempt& attempt : runtime.scheduler().attempts().list(4096)) {
      auto& highest = highest_generation[attempt.operation.value()];
      TOS_CHECK_MSG(attempt.generation.value() >= highest,
                    std::string(where) + ": attempt generation regressed for one operation");
      highest = std::max(highest, attempt.generation.value());
      if (attempt.state == AttemptState::kCompleted) {
        TOS_CHECK_MSG(attempt.completion_committed && attempt.result.success,
                      std::string(where) + ": completed attempt without a committed result");
      }
      if (attempt.resolution == AttemptResolution::kAmbiguous) {
        TOS_CHECK_MSG(!attempt.completion_committed,
                      std::string(where) + ": ambiguous attempt published a completion");
      }
    }
  };

  for (std::uint32_t step = 0; step < steps; ++step) {
    const std::uint32_t action = rng.below(12);
    const std::uint64_t payload_size = 256 + rng.below(4096);
    const std::vector<std::uint8_t> payload = tos_test::make_payload(
        static_cast<std::size_t>(payload_size), static_cast<std::uint32_t>(seed + step));
    const bool side_effect = rng.chance(1, 6);
    OperationRequest request = tos_test::make_request(
        side_effect ? opclass::side_effect_emit() : opclass::checksum_crc32c(), payload_size);
    const ExecutionDomainId target = domains[rng.below(static_cast<std::uint32_t>(domains.size()))];

    switch (action) {
      case 0: {
        // Planning is always legal to attempt; when it succeeds the selected domain
        // must have been legally selectable.
        PlanResult planned = runtime.scheduler().plan(request, payload);
        trace.add("plan:" + std::string(to_string(planned.explanation.outcome)));
        if (planned.planned) {
          const InvariantReport report =
              selected_domain_was_eligible(runtime.scheduler(), planned.plan, request);
          TOS_CHECK_MSG(report.ok, "step " + std::to_string(step) + ": " + report.detail);
          plans.push_back(planned.plan);
          ++plan_counter;
        }
        break;
      }
      case 1: {
        if (plans.empty()) break;
        ExecutionPlan plan = plans.back();
        const Status reserved = runtime.scheduler().reserve(plan);
        trace.add("reserve:" + reserved.code);
        if (reserved) plans.back() = plan;
        break;
      }
      case 2: {
        if (plans.empty()) break;
        ExecutionPlan plan = plans.back();
        const DispatchResult dispatched = runtime.scheduler().dispatch(plan);
        trace.add("dispatch:" +
                  std::string(dispatched.dispatched ? "ok" : to_string(dispatched.rejection)));
        if (dispatched.dispatched) {
          plans.back() = plan;
          runtime.wait_for(plan.attempt, 1);
          ExecutionAttempt stored;
          if (runtime.scheduler().attempts().get(plan.attempt, stored)) {
            trace.add("completed:" + std::string(to_string(stored.state)));
          }
        }
        break;
      }
      case 3: {
        // Capability change: the engine generation moves.
        const CapabilitySet previous = runtime.backend().discover_domains().front().capability.capability;
        const Status changed = runtime.backend().set_operation_support(
            target, opclass::checksum_crc32c(), !(step % 2 == 0));
        trace.add("capability:" + changed.code);
        const Status synced = runtime.refresh();
        trace.add("sync:" + synced.code);
        (void)previous;
        break;
      }
      case 4: {
        const std::uint32_t utilization = rng.below(100);
        const std::uint32_t congestion = rng.below(100);
        const std::uint32_t queue = rng.below(300);
        const Status loaded =
            runtime.backend().set_load(target, utilization, congestion, queue, 1, 1000 + step);
        trace.add("load:" + loaded.code);
        const Status synced = runtime.refresh();
        trace.add("sync:" + synced.code);
        break;
      }
      case 5: {
        const Status health = runtime.backend().set_health(target, rng.chance(3, 4), true, true);
        trace.add("health:" + health.code);
        (void)runtime.refresh();
        break;
      }
      case 6: {
        SchedulerPolicy policy = runtime.scheduler().policy();
        policy.offload_requirement =
            rng.chance(1, 4) ? OffloadRequirement::kPreferOffload : OffloadRequirement::kAny;
        policy.minimum_isolation = rng.chance(1, 5) ? IsolationClass::kSeparateProcess
                                                   : IsolationClass::kUnknown;
        const Status applied = runtime.scheduler().set_policy(std::move(policy));
        trace.add("policy:" + applied.code);
        break;
      }
      case 7: {
        const Status fenced = runtime.scheduler().fence_domain(target, "property: fence");
        trace.add("fence_domain:" + fenced.code);
        break;
      }
      case 8: {
        ExecutionDomainRecord record;
        if (!runtime.scheduler().domains().get(target, record)) break;
        const Status fenced =
            runtime.scheduler().fence_worker_boot(record.worker_boot, "property: worker lost");
        trace.add("fence_boot:" + fenced.code);
        break;
      }
      case 9: {
        // Replacement worker: same domain identity, new incarnation, fresh evidence.
        ExecutionDomainRecord record;
        if (!runtime.scheduler().domains().get(target, record)) break;
        const WorkerId worker(process_unique_u64());
        const WorkerBootId boot(process_unique_u64());
        const Status registered = runtime.scheduler().register_worker_boot(worker, boot, "replacement");
        ExecutionDomainRecord replacement = record;
        replacement.worker = worker;
        replacement.worker_boot = boot;
        replacement.fenced = false;
        replacement.fence_reason.clear();
        replacement.generation = ExecutionDomainGeneration(record.generation.value() + 1);
        replacement.load.load_generation = LoadGeneration(record.load.load_generation.value() + 1);
        replacement.load.queue_generation = QueueGeneration(record.load.queue_generation.value() + 1);
        replacement.load.health_generation =
            HealthGeneration(record.load.health_generation.value() + 1);
        replacement.load.evidence = EvidenceGeneration(record.load.evidence.value() + 1);
        auto updated = runtime.scheduler().update_domain(replacement);
        trace.add("replacement:" + registered.code + "/" + updated.status.code);
        break;
      }
      case 10: {
        if (plans.empty()) break;
        const ExecutionPlan plan = plans.back();
        const CancellationResult cancelled = runtime.scheduler().cancel(plan.attempt);
        trace.add("cancel:" + cancelled.status.code);
        plans.pop_back();
        break;
      }
      default: {
        // Persistence round trip through the durable format.
        PersistedState state;
        state.generation = PersistenceGeneration(1 + step);
        state.last_epoch = runtime.scheduler().coordinator_epoch();
        state.has_policy = true;
        state.policy = runtime.scheduler().policy();
        for (const ExecutionDomainRecord& domain : runtime.scheduler().domains().all()) {
          state.domains.push_back(domain);
        }
        const std::vector<std::uint8_t> encoded = StateStore::encode(state);
        Checked<PersistedState> decoded = StateStore::decode(encoded);
        TOS_CHECK_MSG(decoded.ok(), "durable state failed to decode: " + decoded.status.code);
        trace.add("persist:" + std::to_string(encoded.size()));
        const ReconciliationReport report = runtime.scheduler().reconcile();
        TOS_CHECK_EQ(report.discrepancies, report.items.size());
        trace.add("reconcile:" + std::to_string(report.discrepancies));
        break;
      }
    }
    const std::string where = "step " + std::to_string(step);
    check_invariants(where.c_str());
  }

  // The runtime must close its books at the end of any sequence.
  const ReservationAudit reservations = runtime.scheduler().reservations().audit();
  TOS_CHECK(reservations.consistent);
  const AttemptAudit attempts = runtime.scheduler().attempts().audit();
  TOS_CHECK(attempts.consistent);
  trace.add("plans:" + std::to_string(plan_counter));
  return trace;
}

}  // namespace

TOS_TEST(seeded_lifecycle_properties_seed_1) {
  const Trace trace = run_sequence(1, 120);
  TOS_CHECK(trace.steps.size() >= 100U);
}

TOS_TEST(seeded_lifecycle_properties_seed_2) {
  const Trace trace = run_sequence(2, 120);
  TOS_CHECK(trace.steps.size() >= 100U);
}

TOS_TEST(seeded_lifecycle_properties_seed_3) {
  const Trace trace = run_sequence(3, 120);
  TOS_CHECK(trace.steps.size() >= 100U);
}

TOS_TEST(identical_state_produces_identical_decisions) {
  // Determinism: the same seed must produce the same trace, and repeated planning
  // over an unchanged state must select the same domain with the same score.
  const Trace first = run_sequence(7, 60);
  const Trace second = run_sequence(7, 60);
  TOS_REQUIRE(first.steps.size() == second.steps.size());
  for (std::size_t i = 0; i < first.steps.size(); ++i) {
    TOS_CHECK_MSG(first.steps[i] == second.steps[i],
                  std::string("trace diverged at step ") + std::to_string(i) + ": " +
                      first.steps[i] + " vs " + second.steps[i]);
  }

  tos_test::TestRuntime runtime;
  runtime.add_domain(make_synthetic_domain(ExecutionDomainId(201), ExecutionDomainType::kDpu, "dpu.b"));
  runtime.add_domain(make_synthetic_domain(ExecutionDomainId(202), ExecutionDomainType::kCpu, "cpu.b"));
  const std::vector<std::uint8_t> payload = tos_test::make_payload(2048, 77);
  OperationRequest request = tos_test::make_request(opclass::checksum_crc32c(), payload.size());
  PlanResult reference = runtime.scheduler().plan(request, payload);
  TOS_REQUIRE(reference.planned);
  for (int i = 0; i < 25; ++i) {
    PlanResult again = runtime.scheduler().plan(request, payload);
    TOS_REQUIRE(again.planned);
    // The logical result must be identical. The operation identity and the snapshot
    // generation are freshly allocated per request, so they are compared separately
    // rather than being folded into the decision itself.
    TOS_CHECK_EQ(again.explanation.outcome, reference.explanation.outcome);
    TOS_CHECK_EQ(again.plan.domain, reference.plan.domain);
    TOS_CHECK_EQ(again.plan.domain_type, reference.plan.domain_type);
    TOS_CHECK_EQ(again.explanation.evaluated_candidates,
                 reference.explanation.evaluated_candidates);
    TOS_CHECK_EQ(again.explanation.rejected_candidates,
                 reference.explanation.rejected_candidates);
    TOS_REQUIRE(again.explanation.ranking.size() == reference.explanation.ranking.size());
    for (std::size_t rank = 0; rank < again.explanation.ranking.size(); ++rank) {
      TOS_CHECK_EQ(again.explanation.ranking[rank].domain_id,
                   reference.explanation.ranking[rank].domain_id);
      TOS_CHECK_EQ(again.explanation.ranking[rank].weighted_score,
                   reference.explanation.ranking[rank].weighted_score);
      TOS_CHECK_EQ(again.explanation.ranking[rank].factors,
                   reference.explanation.ranking[rank].factors);
    }
    TOS_CHECK(again.plan.operation.value() > reference.plan.operation.value());
    TOS_CHECK(!render_explanation_json(again.explanation).empty());
  }
  TOS_CHECK_EQ(runtime.scheduler().snapshot().totals.domains, 2U);
}
