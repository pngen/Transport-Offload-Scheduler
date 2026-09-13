// Multiprocess proof: real worker processes, real process death, real fencing.
//
// The coordinator runs in this process (real sockets, real threads) and the workers
// are separate OS processes. Killing a worker is TerminateProcess, not a flag.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "tos/dist/coordinator.hpp"
#include "tos_test_support.hpp"

using namespace tos;

namespace {

/// Read a worker's stdout until it reports readiness or the process is gone.
std::string wait_for_ready_line(const std::shared_ptr<ProcessHandle>& handle) {
  std::string collected;
  tos_test::wait_until([&] {
    collected += read_process_stdout(handle);
    return collected.find("WORKER_READY") != std::string::npos || !process_alive(handle);
  });
  const std::size_t position = collected.find("WORKER_READY");
  if (position == std::string::npos) return std::string();
  const std::size_t end = collected.find('\n', position);
  return collected.substr(position,
                          end == std::string::npos ? std::string::npos : end - position);
}

}  // namespace

TOS_TEST(worker_death_fencing_with_real_processes) {
  dist::CoordinatorConfig config;
  config.bind_host = "127.0.0.1";
  config.port = 0;
  config.scheduler.policy = make_default_policy();
  config.scheduler.host_node = "proof.host";
  dist::OffloadCoordinator coordinator(std::move(config));
  TOS_REQUIRE(coordinator.start().ok);
  const std::uint16_t port = coordinator.port();
  TOS_CHECK(port != 0);

  const std::string stderr_a = "worker_a.stderr.log";
  const std::string stderr_b = "worker_b.stderr.log";
  auto worker_a = tos_test::spawn_tool(
      "tos_worker",
      {"--host", "127.0.0.1", "--port", std::to_string(port), "--name", "dpu-a", "--type", "dpu",
       "--node", "proof.host", "--domain-base", "4096", "--provenance", "synthetic", "--log-level",
       "warn"},
      stderr_a);
  TOS_REQUIRE(worker_a.ok());
  auto worker_b = tos_test::spawn_tool(
      "tos_worker",
      {"--host", "127.0.0.1", "--port", std::to_string(port), "--name", "smartnic-b", "--type",
       "smartnic", "--node", "proof.host", "--domain-base", "8192", "--provenance", "synthetic",
       "--log-level", "warn"},
      stderr_b);
  TOS_REQUIRE(worker_b.ok());

  const std::string ready_a = wait_for_ready_line(worker_a.value);
  const std::string ready_b = wait_for_ready_line(worker_b.value);
  TOS_CHECK_MSG(ready_a.rfind("WORKER_READY", 0) == 0, ready_a);
  TOS_CHECK_MSG(ready_b.rfind("WORKER_READY", 0) == 0, ready_b);
  TOS_CHECK_EQ(coordinator.registered_worker_count(), 2U);

  // Both worker domains must be visible WITH current evidence before anything runs.
  // Registration and evidence are separate publications, so the wait covers both
  // instead of assuming they arrive in the same scheduling window.
  tos_test::wait_until([&] {
    const std::vector<ExecutionDomainRecord> registered = coordinator.scheduler().domains().all();
    if (registered.size() < 2) return false;
    for (const ExecutionDomainRecord& domain : registered) {
      if (!domain.load.published()) return false;
    }
    return true;
  });
  for (const ExecutionDomainRecord& domain : coordinator.scheduler().domains().all()) {
    TOS_CHECK_MSG(domain.load.published(), "worker domain must publish current evidence");
    TOS_CHECK_EQ(domain.provenance, Provenance::kSynthetic);
  }

  const std::vector<std::uint8_t> payload = tos_test::make_payload(2048, 21);
  OperationRequest request = tos_test::make_request(opclass::checksum_crc32c(), payload.size());
  DispatchOutcome first = coordinator.scheduler().plan_reserve_dispatch(
      request, std::span<const std::uint8_t>(payload.data(), payload.size()));
  TOS_REQUIRE(first.plan_result.planned);
  TOS_CHECK(first.dispatch_result.dispatched);
  const ExecutionDomainId first_domain = first.plan_result.plan.domain;
  const WorkerBootId first_boot = first.plan_result.plan.binding.worker_boot;
  const ExecutionAttemptId first_attempt = first.plan_result.plan.attempt;

  tos_test::wait_until([&] {
    ExecutionAttempt stored;
    return coordinator.scheduler().attempts().get(first_attempt, stored) &&
           stored.state == AttemptState::kCompleted;
  });
  ExecutionAttempt completed;
  TOS_REQUIRE(coordinator.scheduler().attempts().get(first_attempt, completed));
  TOS_CHECK(completed.completion_committed);
  TOS_CHECK_EQ(completed.result.result_digest,
               static_cast<std::uint64_t>(crc32c(payload.data(), payload.size())));
  TOS_CHECK_EQ(completed.provenance, Provenance::kSynthetic);

  // Determine which process owned the domain that just ran, and kill that process.
  const bool owner_is_a = first_domain.value() >= 4096 && first_domain.value() < 8192;
  const std::shared_ptr<ProcessHandle>& victim = owner_is_a ? worker_a.value : worker_b.value;
  const std::shared_ptr<ProcessHandle>& survivor = owner_is_a ? worker_b.value : worker_a.value;
  const WorkerBootId victim_boot = first_boot;
  TOS_CHECK(process_alive(victim));
  TOS_REQUIRE(terminate_process(victim).ok);

  tos_test::wait_until([&] { return !coordinator.scheduler().workers().is_current(
                                     completed.worker, victim_boot); });
  TOS_CHECK(!coordinator.scheduler().workers().is_current(completed.worker, victim_boot));
  TOS_CHECK(coordinator.scheduler().workers().fenced_boot_id(victim_boot));
  TOS_CHECK(process_alive(survivor));

  // The fenced domain must be ineligible while the peer stays usable.
  ExecutionDomainRecord fenced_domain;
  TOS_REQUIRE(coordinator.scheduler().domains().get(first_domain, fenced_domain));
  TOS_CHECK(fenced_domain.fenced);

  DispatchOutcome second = coordinator.scheduler().plan_reserve_dispatch(
      request, std::span<const std::uint8_t>(payload.data(), payload.size()));
  TOS_REQUIRE(second.plan_result.planned);
  TOS_CHECK(second.plan_result.plan.domain != first_domain);
  TOS_CHECK(second.dispatch_result.dispatched);
  const ExecutionAttemptId second_attempt = second.plan_result.plan.attempt;
  tos_test::wait_until([&] {
    ExecutionAttempt stored;
    return coordinator.scheduler().attempts().get(second_attempt, stored) &&
           stored.state == AttemptState::kCompleted;
  });
  ExecutionAttempt peer_attempt;
  TOS_REQUIRE(coordinator.scheduler().attempts().get(second_attempt, peer_attempt));
  TOS_CHECK_EQ(peer_attempt.state, AttemptState::kCompleted);

  // A completion arriving from the dead incarnation must be rejected without
  // mutating anything: this is the stale-worker replay attempt.
  CompletionSubmission replay;
  replay.attempt = first_attempt;
  replay.generation = completed.generation;
  replay.dispatch = completed.dispatch;
  replay.completion = CompletionId(first_attempt.value());
  replay.worker = completed.worker;
  replay.worker_boot = victim_boot;
  replay.coordinator_epoch = coordinator.scheduler().coordinator_epoch();
  replay.domain_generation = completed.domain_generation;
  replay.capability_generation = completed.capability_generation;
  replay.operation = completed.operation;
  replay.provenance = completed.provenance;
  replay.result.success = true;
  replay.result.result_digest = 0xABCDEFULL;
  const CompletionOutcome replay_outcome = coordinator.scheduler().complete(replay);
  TOS_CHECK(!replay_outcome.committed);
  TOS_CHECK_EQ(replay_outcome.rejection, CompletionRejection::kConflictingDuplicate);

  // An identical replay of the committed result is accepted idempotently and still
  // does not create a second authoritative completion.
  CompletionSubmission identical = replay;
  identical.result.result_digest = completed.result.result_digest;
  identical.result.bytes_processed = completed.result.bytes_processed;
  const CompletionOutcome identical_outcome = coordinator.scheduler().complete(identical);
  TOS_CHECK(identical_outcome.committed);
  TOS_CHECK(identical_outcome.idempotent);
  TOS_CHECK_EQ(identical_outcome.rejection, CompletionRejection::kDuplicateIdentical);
  ExecutionAttempt after_replay;
  TOS_REQUIRE(coordinator.scheduler().attempts().get(first_attempt, after_replay));
  TOS_CHECK_EQ(after_replay.result.result_digest, completed.result.result_digest);

  // A replacement worker with a fresh incarnation must be accepted and become
  // eligible only through fresh evidence.
  auto replacement = tos_test::spawn_tool(
      "tos_worker",
      {"--host", "127.0.0.1", "--port", std::to_string(port), "--name", "dpu-a-replacement",
       "--type", "dpu", "--node", "proof.host", "--domain-base",
       std::to_string(owner_is_a ? 4096 : 8192), "--provenance", "synthetic", "--log-level", "warn"},
      "worker_replacement.stderr.log");
  TOS_REQUIRE(replacement.ok());
  const std::string ready_replacement = wait_for_ready_line(replacement.value);
  TOS_CHECK_MSG(ready_replacement.rfind("WORKER_READY", 0) == 0, ready_replacement);
  TOS_CHECK(ready_replacement != ready_a && ready_replacement != ready_b);

  tos_test::wait_until([&] {
    ExecutionDomainRecord restored_probe;
    return coordinator.scheduler().domains().get(first_domain, restored_probe) &&
           !restored_probe.fenced && restored_probe.load.published();
  });
  ExecutionDomainRecord restored;
  TOS_REQUIRE(coordinator.scheduler().domains().get(first_domain, restored));
  TOS_CHECK(!restored.fenced);
  TOS_CHECK(restored.worker_boot != victim_boot);
  TOS_CHECK(restored.generation.value() > fenced_domain.generation.value());

  DispatchOutcome third = coordinator.scheduler().plan_reserve_dispatch(
      request, std::span<const std::uint8_t>(payload.data(), payload.size()));
  TOS_REQUIRE(third.plan_result.planned);
  TOS_CHECK(third.dispatch_result.dispatched);
  const ExecutionAttemptId third_attempt = third.plan_result.plan.attempt;
  tos_test::wait_until([&] {
    ExecutionAttempt stored;
    return coordinator.scheduler().attempts().get(third_attempt, stored) &&
           stored.state == AttemptState::kCompleted;
  });
  ExecutionAttempt restored_attempt;
  TOS_REQUIRE(coordinator.scheduler().attempts().get(third_attempt, restored_attempt));
  TOS_CHECK_EQ(restored_attempt.state, AttemptState::kCompleted);

  // Attempt generations for one operation never regress.
  TOS_CHECK(third_attempt.value() > 0);
  TOS_CHECK(restored_attempt.generation.value() >= completed.generation.value());

  TOS_REQUIRE(coordinator.stop().ok);
  // stop() asks every worker to shut down, so the survivor exits on its own: no
  // process is left orphaned and no forced termination is needed here.
  const auto survivor_exit = wait_process(survivor);
  TOS_CHECK(survivor_exit.ok());
  const auto replacement_exit = wait_process(replacement.value);
  TOS_CHECK(replacement_exit.ok());
  close_process(worker_a.value);
  close_process(worker_b.value);
  close_process(replacement.value);
  std::remove(stderr_a.c_str());
  std::remove(stderr_b.c_str());
  std::remove("worker_replacement.stderr.log");
}

TOS_TEST(worker_death_before_acknowledgement_is_ambiguous) {
  dist::CoordinatorConfig config;
  config.bind_host = "127.0.0.1";
  config.port = 0;
  config.scheduler.policy = make_default_policy();
  config.scheduler.host_node = "proof.host";
  dist::OffloadCoordinator coordinator(std::move(config));
  TOS_REQUIRE(coordinator.start().ok);

  // This worker applies the operation and then terminates before acknowledging it.
  auto dying = tos_test::spawn_tool(
      "tos_worker",
      {"--host", "127.0.0.1", "--port", std::to_string(coordinator.port()), "--name", "dying-dpu",
       "--type", "dpu", "--node", "proof.host", "--domain-base", "65536", "--provenance",
       "synthetic", "--fault", "die-after-apply", "--log-level", "warn"},
      "worker_dying.stderr.log");
  TOS_REQUIRE(dying.ok());
  const std::string ready = wait_for_ready_line(dying.value);
  TOS_CHECK_MSG(ready.rfind("WORKER_READY", 0) == 0, ready);
  tos_test::wait_until([&] { return coordinator.scheduler().domains().size() >= 1; });

  // A non-repeatable operation: replaying it would append a second record.
  const std::vector<std::uint8_t> payload = tos_test::make_payload(256, 5);
  OperationRequest request = tos_test::make_request(opclass::side_effect_emit(), payload.size());
  DispatchOutcome outcome = coordinator.scheduler().plan_reserve_dispatch(
      request, std::span<const std::uint8_t>(payload.data(), payload.size()));
  TOS_CHECK_MSG(outcome.plan_result.planned, std::string(to_string(outcome.plan_result.explanation.outcome)));
  TOS_REQUIRE(outcome.plan_result.planned);
  TOS_CHECK(outcome.dispatch_result.dispatched);

  const ExecutionAttemptId attempt = outcome.plan_result.plan.attempt;
  tos_test::wait_until([&] {
    ExecutionAttempt stored;
    return coordinator.scheduler().attempts().get(attempt, stored) &&
           (stored.state == AttemptState::kOutcomeUnknown || stored.state == AttemptState::kFailed ||
            stored.state == AttemptState::kFenced ||
            stored.state == AttemptState::kRejectedStale);
  });
  ExecutionAttempt stored;
  TOS_REQUIRE(coordinator.scheduler().attempts().get(attempt, stored));
  TOS_CHECK_MSG(stored.state == AttemptState::kOutcomeUnknown,
                std::string("unexpected state ") + std::string(to_string(stored.state)));
  TOS_CHECK_EQ(stored.resolution, AttemptResolution::kAmbiguous);
  TOS_CHECK(!stored.completion_committed);
  TOS_CHECK(!coordinator.scheduler().workers().is_current(stored.worker, stored.worker_boot));
  TOS_CHECK(coordinator.scheduler().workers().fenced_boot_id(stored.worker_boot));

  // The runtime must not replay a non-repeatable operation automatically.
  const RetryResult retry = coordinator.scheduler().retry(attempt);
  TOS_CHECK(!retry.retry_permitted);
  TOS_CHECK(retry.rejection == RetryRejection::kAmbiguousOutcome ||
            retry.rejection == RetryRejection::kNonRepeatableOperation);

  // No completion may ever be committed for that attempt generation.
  CompletionSubmission late;
  late.attempt = attempt;
  late.generation = stored.generation;
  late.dispatch = stored.dispatch;
  late.completion = CompletionId(attempt.value());
  late.worker = stored.worker;
  late.worker_boot = stored.worker_boot;
  late.coordinator_epoch = coordinator.scheduler().coordinator_epoch();
  late.domain_generation = stored.domain_generation;
  late.capability_generation = stored.capability_generation;
  late.operation = stored.operation;
  late.provenance = stored.provenance;
  late.result.success = true;
  late.result.result_digest = 12345;
  const CompletionOutcome late_outcome = coordinator.scheduler().complete(late);
  TOS_CHECK(!late_outcome.committed);
  ExecutionAttempt unchanged;
  TOS_REQUIRE(coordinator.scheduler().attempts().get(attempt, unchanged));
  TOS_CHECK_EQ(unchanged.state, AttemptState::kOutcomeUnknown);
  TOS_CHECK(!unchanged.completion_committed);

  TOS_REQUIRE(coordinator.stop().ok);
  const auto exit_code = wait_process(dying.value);
  TOS_CHECK(exit_code.ok());
  if (exit_code.ok()) TOS_CHECK_EQ(exit_code.value, 21);
  close_process(dying.value);
  std::remove("worker_dying.stderr.log");
}