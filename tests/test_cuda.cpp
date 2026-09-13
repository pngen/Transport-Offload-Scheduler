// Real CUDA accelerator backend: end-to-end device proof.
//
// This suite proves, on a machine that really has a CUDA device, that the accelerator
// domain executes CHECKSUM_CRC32C on the device and returns the same digest the CPU
// produces, that the runtime plans and commits the result through its normal authority
// path exactly once, and that every byte the call allocated is released again.
//
// On a machine without a CUDA device the suite prints a single SKIP line and returns:
// it does not fail, and it does not pretend the test ran.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "tos/backends/cuda_backend.hpp"
#include "tos/backends/local_channel.hpp"
#include "tos/util/crc32c.hpp"
#include "tos_test_support.hpp"

using namespace tos;

namespace {

constexpr std::uint64_t kPayloadBytes = 1U << 20;        // the required 1 MiB payload
constexpr std::uint64_t kPayloadBound = 4U << 20;        // configured dispatch bound
constexpr std::uint64_t kRaggedBytes = kPayloadBytes + 3;  // exercises the tail chunk

bool terminal(AttemptState state) {
  switch (state) {
    case AttemptState::kCompleted:
    case AttemptState::kFailed:
    case AttemptState::kOutcomeUnknown:
    case AttemptState::kCancelled:
    case AttemptState::kRejectedStale:
    case AttemptState::kFenced: return true;
    default: return false;
  }
}

/// Publish a fully discovered domain record through the normal evidence path, exactly
/// as a deployment does.
void publish_domain(Scheduler& scheduler, const ExecutionDomainRecord& record) {
  if (record.worker.valid() && record.worker_boot.valid()) {
    TOS_CHECK_MSG(scheduler.register_worker_boot(record.worker, record.worker_boot, "cuda.test").ok,
                  "the owning incarnation must be announced before its domain");
  }
  TOS_CHECK_MSG(scheduler.register_domain(record).ok, "the CUDA domain record must register");
  TOS_CHECK_MSG(scheduler.publish_capability(record.capability).ok,
                "the CUDA capability record must publish");
  TOS_CHECK_MSG(scheduler.publish_load(record.id, record.load).ok, "the load evidence must publish");
  TOS_CHECK_MSG(scheduler.publish_locality(record.id, record.locality).ok,
                "the locality evidence must publish");
  TOS_CHECK_MSG(scheduler.publish_topology(record.id, record.topology).ok,
                "the topology evidence must publish");
  TOS_CHECK_MSG(scheduler.publish_compatibility(record.id, record.compatibility).ok,
                "the compatibility evidence must publish");
  TOS_CHECK_MSG(scheduler.publish_capacity(record.id, record.capacity).ok,
                "the capacity vector must publish");
}

}  // namespace

TOS_TEST(cuda_backend_executes_crc32c_on_a_real_device_and_frees_every_allocation) {
  if (!CudaBackend::available()) {
    std::cout << "SKIP cuda: no device" << std::endl;
    return;
  }

  const std::string device_name = CudaBackend::device_name();
  const std::uint32_t compute_capability = CudaBackend::device_compute_capability();
  const std::uint64_t device_memory = CudaBackend::device_memory_bytes();
  std::cout << "cuda device: " << device_name << " compute_capability=" << compute_capability
            << " memory_bytes=" << device_memory << std::endl;
  TOS_CHECK_MSG(!device_name.empty(), "a present device must report its real name");
  TOS_CHECK_MSG(compute_capability > 0, "a present device must report a compute capability");
  TOS_CHECK_MSG(device_memory > 0, "a present device must report its real memory size");

  const ExecutionDomainId domain_id(9101);
  CudaBackend::Options options;
  options.domain_id = domain_id;
  options.name = "cuda.0.test";
  options.max_payload_bytes = kPayloadBound;
  options.parent_host = "cuda.test.host";
  auto backend = std::make_shared<CudaBackend>(options);

  TOS_CHECK_MSG(backend->provenance() == Provenance::kReal,
                "an initialised device must be reported as REAL provenance");
  TOS_REQUIRE(backend->provenance() == Provenance::kReal);

  const std::vector<ExecutionDomainRecord> discovered = backend->discover_domains();
  TOS_REQUIRE(discovered.size() == std::size_t{1});
  const ExecutionDomainRecord& record = discovered.front();
  TOS_CHECK_MSG(validate_domain_record(record).ok, "the accelerator domain record must validate");
  TOS_CHECK_EQ(record.type, ExecutionDomainType::kAccelerator);
  TOS_CHECK_EQ(record.provenance, Provenance::kReal);
  TOS_CHECK_MSG(record.capability.authoritative, "the CUDA capability record is authoritative");
  TOS_CHECK_MSG(record.capability.capability.supports_operation(opclass::checksum_crc32c()),
                "CHECKSUM_CRC32C must be published as a proven device operation");
  TOS_CHECK_MSG(!record.capability.capability.supports_operation(opclass::compress_rle()),
                "the device path must not claim COMPRESS_RLE: it has no host fallback");
  TOS_CHECK(record.capability.capability.memory_domain_supported(MemoryDomain::kHost));
  TOS_CHECK(record.capability.capability.memory_domain_supported(MemoryDomain::kPinnedHost));
  TOS_CHECK(record.capability.capability.memory_domain_supported(MemoryDomain::kDeviceLocal));
  TOS_CHECK(record.capability.capability.memory_domain_supported(MemoryDomain::kPeerDevice));
  TOS_CHECK_EQ(record.capability.capability.accelerator_arch, compute_capability);
  TOS_CHECK_MSG(record.capacity[static_cast<std::size_t>(ResourceKind::kDeviceMemoryBytes)] ==
                    device_memory,
                "the published device capacity must be the real device memory");

  // The capacity handshake reads free device memory from the device itself.
  ResourceAmounts claim{};
  claim[static_cast<std::size_t>(ResourceKind::kDeviceMemoryBytes)] = std::uint64_t{1} << 20;
  TOS_CHECK_MSG(backend->reserve(record.id, ReservationId(11), claim).ok,
                "a 1 MiB device-memory claim must fit in a real device");
  ResourceAmounts impossible{};
  impossible[static_cast<std::size_t>(ResourceKind::kDeviceMemoryBytes)] = std::uint64_t{1} << 60;
  TOS_CHECK_MSG(!backend->reserve(record.id, ReservationId(12), impossible).ok,
                "a claim larger than the whole device must be refused");
  TOS_CHECK_MSG(backend->release(record.id, ReservationId(11)).ok,
                "an accepted claim must be releasable");
  TOS_CHECK_MSG(!backend->release(record.id, ReservationId(11)).ok,
                "a second release of the same claim must be refused");
  TOS_CHECK_MSG(!backend->reserve(ExecutionDomainId(9999), ReservationId(13), claim).ok,
                "a claim on an unknown domain must be refused");
  TOS_CHECK_EQ(CudaBackend::outstanding_device_bytes(), std::uint64_t{0});

  // -------------------------------------------------------------------------
  // Direct device execution: exact CPU parity across the shapes the device path can
  // take (empty, sub-block, exact chunk multiples, ragged tail, multi-megabyte).
  // -------------------------------------------------------------------------
  const std::uint64_t sizes[] = {0,        1,        7,        255,      256,
                                 257,      4095,     4096,     4097,     32768,
                                 65536,    262144,   1048576,  1048579,  3000000};
  std::uint64_t executed_before = backend->executed_count();
  for (const std::uint64_t size : sizes) {
    const std::vector<std::uint8_t> payload_case = tos_test::make_payload(
        static_cast<std::size_t>(size), static_cast<std::uint32_t>(size % 97 + 1));
    ExecutionInvocation case_invocation;
    case_invocation.domain = record.id;
    case_invocation.domain_generation = record.generation;
    case_invocation.capability_generation = record.capability.generation;
    case_invocation.operation_class = opclass::checksum_crc32c();
    case_invocation.payload.size_bytes = size;
    case_invocation.payload.alignment_bytes = 8;
    case_invocation.input = payload_case;
    const ExecutionOutcome case_outcome = backend->execute(case_invocation);
    TOS_CHECK_MSG(case_outcome.success,
                  "the device CRC32C must succeed for " + std::to_string(size) + " bytes: " +
                      case_outcome.detail);
    TOS_CHECK_EQ(case_outcome.bytes_processed, size);
    TOS_CHECK_MSG(case_outcome.result_digest ==
                      static_cast<std::uint64_t>(crc32c(payload_case.data(), payload_case.size())),
                  "device digest must equal the CPU digest for " + std::to_string(size) +
                      " bytes: device=" + format_id(case_outcome.result_digest) + " host=" +
                      format_id(static_cast<std::uint64_t>(
                          crc32c(payload_case.data(), payload_case.size()))));
  }
  TOS_CHECK_EQ(backend->executed_count() - executed_before,
               static_cast<std::uint64_t>(sizeof(sizes) / sizeof(sizes[0])));
  TOS_CHECK_MSG(CudaBackend::outstanding_device_bytes() == 0,
                "every direct device execution must release its allocations");

  const std::vector<std::uint8_t> ragged = tos_test::make_payload(kRaggedBytes, 11);
  ExecutionInvocation invocation;
  invocation.domain = record.id;
  invocation.domain_generation = record.generation;
  invocation.capability_generation = record.capability.generation;
  invocation.operation_class = opclass::checksum_crc32c();
  invocation.payload.size_bytes = ragged.size();
  invocation.payload.alignment_bytes = 8;
  invocation.input = ragged;
  const ExecutionOutcome direct = backend->execute(invocation);
  TOS_CHECK_MSG(direct.executed, "the device path must really execute");
  TOS_CHECK_MSG(direct.success, "the device CRC32C must succeed");
  TOS_CHECK_EQ(direct.bytes_processed, static_cast<std::uint64_t>(ragged.size()));
  const std::uint32_t ragged_reference = crc32c(ragged.data(), ragged.size());
  TOS_CHECK_EQ(direct.result_digest, static_cast<std::uint64_t>(ragged_reference));
  std::cout << "cuda direct: bytes=" << ragged.size() << " device_digest=0x" << std::hex
            << direct.result_digest << std::dec << " host_digest=0x" << std::hex
            << static_cast<std::uint64_t>(ragged_reference) << std::dec
            << " detail=" << direct.detail << std::endl;

  // A non-checksum class must be refused by the device path, never silently run on the
  // host under an accelerator's REAL provenance.
  ExecutionInvocation unsupported = invocation;
  unsupported.operation_class = opclass::compress_rle();
  const ExecutionOutcome refused = backend->execute(unsupported);
  TOS_CHECK_MSG(!refused.success, "COMPRESS_RLE must not be reported as executed on the device");
  TOS_CHECK_EQ(refused.failure, FailureKind::kBackendRejection);
  TOS_CHECK(!refused.executed);

  const std::uint64_t launches_after_direct = CudaBackend::kernel_launches();
  TOS_CHECK_MSG(launches_after_direct >= 2,
                "the device path must really launch its kernels (chunk + fold)");
  TOS_CHECK_MSG(CudaBackend::device_bytes_copied() >= kRaggedBytes,
                "the device path must really copy the payload to the device");
  TOS_CHECK_EQ(CudaBackend::outstanding_device_bytes(), std::uint64_t{0});

  // -------------------------------------------------------------------------
  // End-to-end dispatch through a real Scheduler and LocalDispatchChannel.
  // -------------------------------------------------------------------------
  std::mutex mutex;
  std::condition_variable condition;
  std::vector<CompletionOutcome> outcomes;

  SchedulerPolicy policy = make_default_policy();
  SchedulerLimits limits;
  limits.max_payload_dispatch_bytes = kPayloadBound;
  SchedulerOptions scheduler_options;
  scheduler_options.policy = policy;
  scheduler_options.limits = limits;
  scheduler_options.host_node = "cuda.test.host";
  Scheduler scheduler(std::move(scheduler_options));
  TOS_REQUIRE(scheduler.start().ok);

  auto channel = std::make_shared<LocalDispatchChannel>(
      backend,
      [&scheduler, &mutex, &condition, &outcomes](const CompletionSubmission& submission) {
        const CompletionOutcome outcome = scheduler.complete(submission);
        {
          std::lock_guard<std::mutex> lock(mutex);
          outcomes.push_back(outcome);
        }
        condition.notify_all();
      });
  scheduler.set_dispatch_channel(channel);

  publish_domain(scheduler, record);

  const std::vector<std::uint8_t> payload = tos_test::make_payload(kPayloadBytes, 7);
  const OperationRequest request = tos_test::make_request(opclass::checksum_crc32c(), kPayloadBytes);
  const DispatchOutcome dispatched = scheduler.plan_reserve_dispatch(request, payload);
  TOS_CHECK_MSG(dispatched.plan_result.planned, "the accelerator plan must be created");
  TOS_CHECK_MSG(dispatched.dispatch_result.dispatched, "the plan must reach the channel");
  TOS_REQUIRE(dispatched.plan_result.planned);
  TOS_REQUIRE(dispatched.dispatch_result.dispatched);

  const ExecutionPlan& plan = dispatched.plan_result.plan;
  TOS_CHECK_EQ(plan.domain, domain_id);
  TOS_CHECK_EQ(plan.domain_type, ExecutionDomainType::kAccelerator);
  TOS_CHECK_MSG(plan.provenance == Provenance::kReal,
                "the plan of record must carry REAL provenance");
  TOS_CHECK_EQ(plan.explanation.outcome, SelectionOutcome::kAcceleratorSelected);
  TOS_CHECK_EQ(plan.explanation.selected_domain_type, ExecutionDomainType::kAccelerator);
  TOS_CHECK_EQ(plan.explanation.selected_provenance, Provenance::kReal);
  TOS_CHECK_MSG(plan.explanation.selected_is_offload, "the selected domain is an offload engine");

  // Wait for the completion through the ledger. There is deliberately no deadline: a
  // completion that never arrives is a defect, and this suite must hang rather than
  // hide it.
  const ExecutionAttemptId attempt = plan.attempt;
  {
    std::unique_lock<std::mutex> lock(mutex);
    condition.wait(lock, [&scheduler, attempt] {
      ExecutionAttempt stored;
      if (!scheduler.attempts().get(attempt, stored)) return false;
      return terminal(stored.state);
    });
  }

  ExecutionAttempt stored;
  TOS_REQUIRE(scheduler.attempts().get(attempt, stored));
  TOS_CHECK_EQ(stored.state, AttemptState::kCompleted);
  TOS_CHECK_EQ(stored.domain_type, ExecutionDomainType::kAccelerator);
  TOS_CHECK_MSG(stored.provenance == Provenance::kReal,
                "the committed attempt must carry REAL provenance");
  const std::uint32_t expected = crc32c(payload.data(), payload.size());
  TOS_CHECK_EQ(stored.result.result_digest, static_cast<std::uint64_t>(expected));
  TOS_CHECK_EQ(stored.result.bytes_processed, kPayloadBytes);
  TOS_CHECK_MSG(stored.completion_committed, "the attempt must record its committed completion");

  // Exact parity with the project's own host executor, not just with crc32c().
  ExecutionInvocation host_invocation = invocation;
  host_invocation.input = payload;
  host_invocation.payload.size_bytes = payload.size();
  const ExecutionOutcome host_outcome =
      execute_operation_on_host(host_invocation, OperationClassRegistry::builtins());
  TOS_CHECK_MSG(host_outcome.success, "the host executor must produce the same operation");
  TOS_CHECK_EQ(stored.result.result_digest, host_outcome.result_digest);

  {
    std::lock_guard<std::mutex> lock(mutex);
    std::size_t commits = 0;
    std::size_t duplicates = 0;
    std::size_t rejections = 0;
    for (const CompletionOutcome& outcome : outcomes) {
      if (outcome.committed && !outcome.idempotent) ++commits;
      if (outcome.committed && outcome.idempotent) ++duplicates;
      if (!outcome.committed) ++rejections;
    }
    TOS_CHECK_EQ(outcomes.size(), std::size_t{1});
    TOS_CHECK_EQ(commits, std::size_t{1});
    TOS_CHECK_EQ(duplicates, std::size_t{0});
    TOS_CHECK_EQ(rejections, std::size_t{0});
  }
  TOS_CHECK_EQ(scheduler.attempts().audit().multiple_completions, std::uint64_t{0});
  TOS_CHECK_EQ(backend->executed_count() - executed_before,
               static_cast<std::uint64_t>(sizeof(sizes) / sizeof(sizes[0])) + 2);

  std::cout << "cuda dispatch: attempt=" << format_id(attempt.value())
            << " scope=" << to_string(stored.domain_type) << " state=" << to_string(stored.state)
            << " provenance=" << to_string(stored.provenance)
            << " digest=0x" << std::hex << stored.result.result_digest << std::dec
            << " host_digest=0x" << std::hex << static_cast<std::uint64_t>(expected) << std::dec
            << " committed_once" << std::endl;

  // -------------------------------------------------------------------------
  // Cleanup proof: every byte the device path allocated is released.
  // -------------------------------------------------------------------------
  TOS_CHECK_MSG(CudaBackend::outstanding_device_bytes() == 0,
                "the device path must free every allocation before it returns");

  const Status stopped = scheduler.shutdown();
  TOS_CHECK_MSG(stopped.ok, "the scheduler must shut down cleanly");
  TOS_CHECK_EQ(CudaBackend::outstanding_device_bytes(), std::uint64_t{0});
  std::cout << "cuda cleanup: outstanding_device_bytes=" << CudaBackend::outstanding_device_bytes()
            << " kernel_launches=" << CudaBackend::kernel_launches()
            << " device_bytes_copied=" << CudaBackend::device_bytes_copied() << std::endl;
}
