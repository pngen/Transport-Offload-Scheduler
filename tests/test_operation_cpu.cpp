// Real host execution: every implemented operation class runs on the CPU and
// produces a verifiable result, both directly and through the full scheduler path.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include <thread>

#include "tos_test_support.hpp"

using namespace tos;

namespace {

std::vector<std::uint8_t> checksum_bytes(const std::vector<std::uint8_t>& input) {
  ExecutionInvocation invocation;
  invocation.operation_class = opclass::checksum_crc32c();
  invocation.input = input;
  const ExecutionOutcome outcome =
      execute_operation_on_host(invocation, OperationClassRegistry::builtins());
  TOS_CHECK(outcome.success);
  return outcome.output;
}

}  // namespace

TOS_TEST(crc32c_known_vector) {
  const char* text = "123456789";
  TOS_CHECK_EQ(crc32c(text, 9), 0xE3069283U);
  TOS_CHECK_EQ(crc32c_extend(crc32c("1234", 4), "56789", 5), 0xE3069283U);
}

TOS_TEST(host_executor_every_operation_class) {
  const OperationClassRegistry& registry = OperationClassRegistry::builtins();
  const std::vector<std::uint8_t> payload = tos_test::make_payload(4096, 7);

  ExecutionInvocation invocation;
  invocation.payload.size_bytes = payload.size();
  invocation.payload.alignment_bytes = 8;

  // CHECKSUM
  invocation.operation_class = opclass::checksum_crc32c();
  invocation.input = payload;
  ExecutionOutcome outcome = execute_operation_on_host(invocation, registry);
  TOS_REQUIRE(outcome.success);
  TOS_CHECK_EQ(outcome.bytes_processed, payload.size());
  TOS_CHECK_EQ(outcome.result_digest, static_cast<std::uint64_t>(crc32c(payload.data(), payload.size())));

  // COMPRESS then DECOMPRESS round trip
  invocation.operation_class = opclass::compress_rle();
  const ExecutionOutcome compressed = execute_operation_on_host(invocation, registry);
  TOS_REQUIRE(compressed.success);
  invocation.operation_class = opclass::decompress_rle();
  invocation.input = compressed.output;
  const ExecutionOutcome decompressed = execute_operation_on_host(invocation, registry);
  TOS_REQUIRE(decompressed.success);
  TOS_CHECK(decompressed.output == payload);

  // SEGMENT then REASSEMBLE round trip
  invocation.operation_class = opclass::segment_split();
  invocation.input = payload;
  invocation.payload.segment_count = 8;
  const ExecutionOutcome segmented = execute_operation_on_host(invocation, registry);
  TOS_REQUIRE(segmented.success);
  invocation.operation_class = opclass::reassemble_join();
  invocation.input = segmented.output;
  const ExecutionOutcome joined = execute_operation_on_host(invocation, registry);
  TOS_REQUIRE(joined.success);
  TOS_CHECK(joined.output == payload);

  // COPY_STAGE
  invocation.operation_class = opclass::copy_stage();
  invocation.input = payload;
  const ExecutionOutcome staged = execute_operation_on_host(invocation, registry);
  TOS_REQUIRE(staged.success);
  TOS_CHECK(staged.output == payload);

  // VALIDATE_INTEGRITY: success and failure
  std::vector<std::uint8_t> framed;
  const std::uint64_t digest = fnv1a64(payload.data(), payload.size());
  for (int i = 0; i < 8; ++i) framed.push_back(static_cast<std::uint8_t>((digest >> (8 * i)) & 0xFFU));
  framed.insert(framed.end(), payload.begin(), payload.end());
  invocation.operation_class = opclass::validate_integrity();
  invocation.input = framed;
  const ExecutionOutcome verified = execute_operation_on_host(invocation, registry);
  TOS_CHECK(verified.success);
  framed[0] = static_cast<std::uint8_t>(framed[0] ^ 0xFFU);
  invocation.input = framed;
  const ExecutionOutcome tampered = execute_operation_on_host(invocation, registry);
  TOS_CHECK(!tampered.success);
  TOS_CHECK_EQ(tampered.failure, FailureKind::kIntegrityFailure);

  // NOOP_PROBE
  invocation.operation_class = opclass::noop_probe();
  invocation.input = payload;
  TOS_CHECK(execute_operation_on_host(invocation, registry).success);
}

TOS_TEST(side_effect_emit_is_not_repeatable) {
  ExecutionInvocation invocation;
  invocation.operation_class = opclass::side_effect_emit();
  invocation.input = tos_test::make_payload(16, 3);
  const ExecutionOutcome first =
      execute_operation_on_host(invocation, OperationClassRegistry::builtins());
  const ExecutionOutcome second =
      execute_operation_on_host(invocation, OperationClassRegistry::builtins());
  TOS_CHECK(first.success && second.success);
  TOS_CHECK_MSG(first.result_digest != second.result_digest ||
                    first.output != second.output,
                "repeating a non-repeatable operation must be observable");
  const OperationClassDescriptor* descriptor =
      OperationClassRegistry::builtins().find(opclass::side_effect_emit());
  TOS_REQUIRE(descriptor != nullptr);
  TOS_CHECK_EQ(descriptor->side_effect_class, SideEffectClass::kNonRepeatable);
  TOS_CHECK(!is_replay_safe(descriptor->side_effect_class));
}

TOS_TEST(unknown_operation_class_is_rejected_by_host_executor) {
  ExecutionInvocation invocation;
  invocation.operation_class = OperationClassId(9999);
  invocation.input = tos_test::make_payload(16, 1);
  const ExecutionOutcome outcome =
      execute_operation_on_host(invocation, OperationClassRegistry::builtins());
  TOS_CHECK(!outcome.success);
  TOS_CHECK_EQ(outcome.failure, FailureKind::kBackendRejection);
}

TOS_TEST(cpu_backend_reports_real_host_facts) {
  CpuBackend::Options options;
  options.domain_id = ExecutionDomainId(42);
  CpuBackend backend(options);
  const std::vector<ExecutionDomainRecord> discovered = backend.discover_domains();
  TOS_REQUIRE(discovered.size() == 1);
  const ExecutionDomainRecord& record = discovered.front();
  TOS_CHECK_EQ(record.type, ExecutionDomainType::kCpu);
  TOS_CHECK_EQ(record.provenance, Provenance::kReal);
  TOS_CHECK(record.capability.authoritative);
  TOS_CHECK(!record.capability.capability.backend_family.empty());
  TOS_CHECK(record.capability.capability.max_concurrency >= 1);
  TOS_CHECK(record.capability.capability.max_concurrency == CpuBackend::hardware_threads());
  TOS_CHECK(!CpuBackend::cpu_brand().empty());
  TOS_CHECK(validate_domain_record(record).ok);
  TOS_CHECK(record.capability.capability.supports_operation(opclass::checksum_crc32c()));

  // The CPU domain performs real work.
  const std::vector<std::uint8_t> payload = tos_test::make_payload(2048, 11);
  ExecutionInvocation invocation;
  invocation.domain = record.id;
  invocation.operation_class = opclass::checksum_crc32c();
  invocation.input = payload;
  const ExecutionOutcome outcome = backend.execute(invocation);
  TOS_CHECK(outcome.success);
  TOS_CHECK_EQ(outcome.result_digest, static_cast<std::uint64_t>(crc32c(payload.data(), payload.size())));
  TOS_CHECK_EQ(backend.executed_count(), 1U);
}

TOS_TEST(cpu_domain_executes_through_the_scheduler_boundary) {
  SchedulerOptions options;
  options.policy = make_default_policy();
  options.host_node = "test.host";
  Scheduler scheduler(std::move(options));
  TOS_REQUIRE(scheduler.start().ok);

  auto backend = std::make_shared<CpuBackend>(CpuBackend::Options{});
  auto channel = std::make_shared<LocalDispatchChannel>(
      backend, [&scheduler](const CompletionSubmission& submission) { (void)scheduler.complete(submission); });
  scheduler.set_dispatch_channel(channel);

  const std::vector<ExecutionDomainRecord> discovered = backend->discover_domains();
  TOS_REQUIRE(!discovered.empty());
  TOS_REQUIRE(scheduler.register_domain(discovered.front()).ok);
  TOS_REQUIRE(scheduler.publish_capability(discovered.front().capability).ok);
  TOS_REQUIRE(scheduler.publish_load(discovered.front().id, discovered.front().load).ok);
  TOS_REQUIRE(scheduler.publish_capacity(discovered.front().id, discovered.front().capacity).ok);

  const std::vector<std::uint8_t> payload = tos_test::make_payload(1024, 5);
  OperationRequest request = tos_test::make_request(opclass::checksum_crc32c(), payload.size());
  DispatchOutcome outcome = scheduler.plan_reserve_dispatch(
      request, std::span<const std::uint8_t>(payload.data(), payload.size()));
  TOS_CHECK_MSG(outcome.plan_result.planned, std::string(to_string(outcome.plan_result.explanation.outcome)));
  TOS_REQUIRE(outcome.plan_result.planned);
  TOS_CHECK_EQ(outcome.plan_result.explanation.selected_domain_type, ExecutionDomainType::kCpu);
  TOS_CHECK(outcome.dispatch_result.dispatched);

  // Wait for the completion through the ledger without polling on time.
  const ExecutionAttemptId attempt = outcome.plan_result.plan.attempt;
  for (;;) {
    ExecutionAttempt stored;
    TOS_REQUIRE(scheduler.attempts().get(attempt, stored));
    if (stored.state == AttemptState::kCompleted || stored.state == AttemptState::kFailed) break;
    std::this_thread::yield();
  }
  ExecutionAttempt stored;
  TOS_REQUIRE(scheduler.attempts().get(attempt, stored));
  TOS_CHECK_EQ(stored.state, AttemptState::kCompleted);
  TOS_CHECK(stored.completion_committed);
  TOS_CHECK_EQ(stored.result.result_digest,
               static_cast<std::uint64_t>(crc32c(payload.data(), payload.size())));
  TOS_REQUIRE(scheduler.shutdown().ok);
}
