// Independent downstream consumer of the installed Transport Offload Scheduler package.
//
// It uses only the installed public headers and the exported CMake target, and it
// performs a real plan/reserve/dispatch/complete cycle against a real host CPU domain.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include <cstdio>
#include <memory>
#include <vector>

#include <tos/tos.hpp>

int main() {
  using namespace tos;

  std::printf("Transport Offload Scheduler %s consumer\n", std::string(kVersionString).c_str());

  SchedulerOptions options;
  options.policy = make_default_policy();
  options.host_node = "consumer.host";
  options.limits.max_domains = 8;
  Scheduler scheduler(std::move(options));
  if (!scheduler.start()) {
    std::printf("scheduler start failed\n");
    return 1;
  }

  auto backend = std::make_shared<CpuBackend>(CpuBackend::Options{});
  auto channel = std::make_shared<LocalDispatchChannel>(
      backend, [&scheduler](const CompletionSubmission& submission) {
        const CompletionOutcome outcome = scheduler.complete(submission);
        (void)outcome;
      });
  scheduler.set_dispatch_channel(channel);

  const std::vector<ExecutionDomainRecord> discovered = backend->discover_domains();
  if (discovered.empty()) {
    std::printf("no CPU domain discovered\n");
    return 1;
  }
  if (!scheduler.register_domain(discovered.front())) return 1;
  if (!scheduler.publish_capability(discovered.front().capability)) return 1;
  if (!scheduler.publish_load(discovered.front().id, discovered.front().load)) return 1;
  if (!scheduler.publish_capacity(discovered.front().id, discovered.front().capacity)) return 1;

  std::vector<std::uint8_t> payload(4096);
  for (std::size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<std::uint8_t>((i * 31U) & 0xFFU);
  }
  OperationRequest request;
  request.operation_class = opclass::checksum_crc32c();
  request.payload.size_bytes = payload.size();
  request.payload.source_memory = MemoryDomain::kHost;
  request.payload.destination_memory = MemoryDomain::kHost;
  request.payload.alignment_bytes = 8;
  request.payload.transport_class = TransportClass::kRawFrames;
  request.payload.payload_class = PayloadClass::kOpaqueBytes;
  request.payload.segment_count = 1;

  DispatchOutcome outcome = scheduler.plan_reserve_dispatch(request, payload);
  if (!outcome.plan_result.planned || !outcome.dispatch_result.dispatched) {
    std::printf("plan/dispatch failed: %s\n", outcome.dispatch_result.status.code.c_str());
    return 1;
  }
  std::printf("decision: outcome=%s domain=%s provenance=%s\n",
              std::string(to_string(outcome.plan_result.explanation.outcome)).c_str(),
              format_id(outcome.plan_result.plan.domain.value()).c_str(),
              std::string(to_string(outcome.plan_result.plan.provenance)).c_str());

  // Wait for the completion through the ledger without polling on time.
  ExecutionAttempt attempt;
  for (;;) {
    if (!scheduler.attempts().get(outcome.plan_result.plan.attempt, attempt)) {
      std::printf("attempt disappeared\n");
      return 1;
    }
    if (attempt.state == AttemptState::kCompleted || attempt.state == AttemptState::kFailed) break;
  }
  const std::uint32_t expected = crc32c(payload.data(), payload.size());
  std::printf("execution: state=%s digest=%llu expected=%u committed=%s\n",
              std::string(to_string(attempt.state)).c_str(),
              static_cast<unsigned long long>(attempt.result.result_digest), expected,
              attempt.completion_committed ? "yes" : "no");
  const bool ok = attempt.state == AttemptState::kCompleted &&
                  attempt.result.result_digest == static_cast<std::uint64_t>(expected) &&
                  attempt.completion_committed;
  const Status stopped = scheduler.shutdown();
  (void)stopped;
  std::printf("%s\n", ok ? "CONSUMER_OK" : "CONSUMER_FAILED");
  return ok ? 0 : 1;
}
