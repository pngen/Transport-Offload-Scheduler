// Hardware probe honesty: the runtime reports the adapters and CPU facts this
// machine really has, proves nothing it cannot observe, and never relabels a
// synthetic domain class as hardware.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "tos_test_support.hpp"

using namespace tos;

namespace {

/// Publish a fully discovered domain record through the normal evidence path.
void publish_domain(Scheduler& scheduler, const ExecutionDomainRecord& record) {
  // A record with no incarnation is adopted by the scheduler's local boot, which is
  // already registered; an explicit incarnation must be announced first.
  if (record.worker.valid() && record.worker_boot.valid()) {
    TOS_CHECK_MSG(
        scheduler.register_worker_boot(record.worker, record.worker_boot, "probe.worker").ok,
        "the owning incarnation must be announced");
  }
  TOS_CHECK_MSG(scheduler.register_domain(record).ok, "the domain record must register");
  TOS_CHECK_MSG(scheduler.publish_capability(record.capability).ok,
                "the capability record must publish");
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

/// True when the decision explanation rejected this exact domain with this reason.
bool rejected_with(const DecisionExplanation& explanation, ExecutionDomainId domain,
                   IneligibilityReason reason) {
  for (const CandidateEvaluation& candidate : explanation.candidates) {
    if (candidate.domain == domain && !candidate.eligible && candidate.reason == reason) return true;
  }
  return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// Real NIC discovery.
// ---------------------------------------------------------------------------

TOS_TEST(hardware_nic_inventory_reports_real_adapters) {
  const std::vector<NicInfo> nics = enumerate_nics();
  std::cout << "NIC inventory: " << nics.size() << " adapter(s)\n"
            << describe_nic_inventory(nics) << std::flush;

  TOS_CHECK_MSG(!nics.empty(),
                "the host must expose at least one network adapter; an empty inventory means the "
                "probe failed to see real hardware");
  TOS_REQUIRE(!nics.empty());

  // An adapter whose link speed the operating system does not report must publish
  // 0 bps (include/tos/backends/nic_probe.hpp), never the raw all-ones marker that
  // IP_ADAPTER_ADDRESSES::TransmitLinkSpeed carries for an adapter with no speed.
  // Regression guard: the sentinel used to be copied verbatim, which made an
  // unplugged adapter look like an 18 exabit/s engine to any ranking factor.
  constexpr std::uint64_t kUnknownLinkSpeed = 0xFFFFFFFFFFFFFFFFULL;
  for (const NicInfo& nic : nics) {
    TOS_CHECK_MSG(!nic.name.empty(), "every discovered adapter must carry a non-empty OS name");
    TOS_CHECK_MSG(!nic.loopback, "loopback interfaces must be excluded from discovery: " + nic.name);
    TOS_CHECK_MSG(nic.index != 0, "every discovered adapter must carry a non-zero interface index");
    TOS_CHECK_MSG(nic.link_speed_bps != kUnknownLinkSpeed,
                  "an adapter with no reported link speed must publish 0 bps, not the raw "
                  "unknown-value sentinel: " + nic.name + " reported " +
                      std::to_string(nic.link_speed_bps));

    // The same adapter must be describable as an honest NIC domain.
    const ExecutionDomainRecord record = make_nic_domain(nic, ExecutionDomainId(2000 + nic.index),
                                                         "probe.host", WorkerId(0x71),
                                                         WorkerBootId(0x72));
    TOS_CHECK_EQ(record.type, ExecutionDomainType::kNic);
    TOS_CHECK_EQ(record.name, "nic." + nic.name);
    TOS_CHECK_EQ(record.provenance, Provenance::kReal);
    TOS_CHECK_MSG(validate_domain_record(record).ok,
                  "a discovered adapter must produce a valid domain record");
  }
}

// ---------------------------------------------------------------------------
// The NIC domain is honest: real adapter, unproven offload capability.
// ---------------------------------------------------------------------------

TOS_TEST(hardware_nic_domain_is_honest_and_ineligible_without_proof) {
  const std::vector<NicInfo> nics = enumerate_nics();
  TOS_REQUIRE(!nics.empty());
  const NicInfo& nic = nics.front();

  const WorkerId worker(0x51C);
  const WorkerBootId boot(0x51D);
  const ExecutionDomainRecord record = make_nic_domain(nic, ExecutionDomainId(3100), "probe.host",
                                                       worker, boot);
  TOS_CHECK(validate_domain_record(record).ok);
  TOS_CHECK_EQ(record.provenance, Provenance::kReal);
  TOS_CHECK_MSG(!record.capability.authoritative,
                "hardware discovery proves the adapter exists, not that it offloads anything");
  TOS_CHECK(record.capability.capability.operations.empty());
  TOS_CHECK(!record.capability.capability.supports_operation(opclass::checksum_crc32c()));
  TOS_CHECK(record.load.published());

  const OperationRequest request = tos_test::make_request(opclass::checksum_crc32c(), 4096);

  // Policy A: positive evidence required. The adapter is real, but its capability
  // record is not authoritative, so the domain must fail closed as kCapabilityUnproven.
  {
    SchedulerPolicy policy = make_default_policy();
    policy.require_positive_evidence = true;
    SchedulerOptions options;
    options.policy = policy;
    options.host_node = "probe.host";
    Scheduler scheduler(std::move(options));
    TOS_REQUIRE(scheduler.start().ok);
    publish_domain(scheduler, record);

    const PlanResult result = scheduler.plan(request);
    TOS_CHECK_MSG(!result.planned, "an unproven NIC capability must not be selected");
    TOS_CHECK_EQ(result.explanation.outcome, SelectionOutcome::kCapabilityUnsupported);
    TOS_CHECK_MSG(rejected_with(result.explanation, record.id, IneligibilityReason::kCapabilityUnproven),
                  "a real adapter with a non-authoritative capability record must fail as "
                  "kCapabilityUnproven");
    TOS_CHECK(!result.explanation.selected_domain.valid());
    TOS_REQUIRE(scheduler.shutdown().ok);
  }

  // Policy B: unsupported provenance allowed and positive evidence not required. The
  // capability gate opens, and the domain is still refused because it proves no
  // operation class at all.
  {
    SchedulerPolicy policy = make_default_policy();
    policy.allow_unsupported_provenance = true;
    policy.require_positive_evidence = false;
    SchedulerOptions options;
    options.policy = policy;
    options.host_node = "probe.host";
    Scheduler scheduler(std::move(options));
    TOS_REQUIRE(scheduler.start().ok);
    publish_domain(scheduler, record);

    const PlanResult result = scheduler.plan(request);
    TOS_CHECK_MSG(!result.planned, "a NIC domain that proves no operation class must not be selected");
    TOS_CHECK_EQ(result.explanation.outcome, SelectionOutcome::kCapabilityUnsupported);
    TOS_CHECK_MSG(rejected_with(result.explanation, record.id, IneligibilityReason::kOperationNotSupported),
                  "a domain with an empty proven operation list must fail as kOperationNotSupported");
    TOS_CHECK(!result.explanation.selected_domain.valid());
    TOS_REQUIRE(scheduler.shutdown().ok);
  }
}

// ---------------------------------------------------------------------------
// Real CPU facts and a real CPU execution.
// ---------------------------------------------------------------------------

TOS_TEST(hardware_cpu_facts_and_execution_are_real) {
  const std::uint32_t threads = CpuBackend::hardware_threads();
  const unsigned int reported = std::thread::hardware_concurrency();
  TOS_CHECK_MSG(threads > 0, "the CPU backend must report at least one hardware thread");
  TOS_CHECK_EQ(threads, reported == 0 ? 1U : reported);

  const std::string brand = CpuBackend::cpu_brand();
  TOS_CHECK_MSG(!brand.empty(), "the CPU brand string must be readable and non-empty");

  CpuBackend::Options options;
  options.domain_id = ExecutionDomainId(7001);
  options.name = "cpu.host.hardware.probe";
  options.parent_host = "probe.host";
  CpuBackend backend(options);

  const std::vector<ExecutionDomainRecord> discovered = backend.discover_domains();
  TOS_REQUIRE(discovered.size() == std::size_t{1});
  const ExecutionDomainRecord& record = discovered.front();
  TOS_CHECK_MSG(validate_domain_record(record).ok, "the CPU domain record must validate");
  TOS_CHECK_EQ(record.type, ExecutionDomainType::kCpu);
  TOS_CHECK_EQ(record.provenance, Provenance::kReal);
  TOS_CHECK(record.capability.authoritative);
  TOS_CHECK(record.capability.capability.supports_operation(opclass::checksum_crc32c()));
  TOS_CHECK_EQ(record.capability.capability.max_concurrency, threads);
  TOS_CHECK_EQ(record.capability.capability.backend_family, std::string("cpu.host"));

  const std::vector<std::uint8_t> payload = tos_test::make_payload(4096, 17);
  ExecutionInvocation invocation;
  invocation.domain = record.id;
  invocation.domain_generation = record.generation;
  invocation.capability_generation = record.capability.generation;
  invocation.operation_class = opclass::checksum_crc32c();
  invocation.payload.size_bytes = payload.size();
  invocation.payload.alignment_bytes = 8;
  invocation.input = payload;
  const ExecutionOutcome outcome = backend.execute(invocation);
  TOS_CHECK_MSG(outcome.executed, "the CPU backend must really execute the operation");
  TOS_CHECK_MSG(outcome.success, "the CHECKSUM_CRC32C execution must succeed");
  TOS_CHECK_EQ(outcome.bytes_processed, static_cast<std::uint64_t>(payload.size()));
  TOS_CHECK_EQ(outcome.result_digest,
               static_cast<std::uint64_t>(crc32c(payload.data(), payload.size())));
  TOS_CHECK_EQ(backend.executed_count(), std::uint64_t{1});

  // The runtime itself never upgrades the provenance of the published record.
  SchedulerOptions scheduler_options;
  scheduler_options.policy = make_default_policy();
  scheduler_options.host_node = "probe.host";
  Scheduler scheduler(std::move(scheduler_options));
  TOS_REQUIRE(scheduler.start().ok);
  publish_domain(scheduler, record);
  ExecutionDomainRecord stored;
  TOS_REQUIRE(scheduler.domains().get(record.id, stored));
  TOS_CHECK_EQ(stored.provenance, Provenance::kReal);
  TOS_CHECK(stored.capability.authoritative);
  TOS_REQUIRE(scheduler.shutdown().ok);
}

// ---------------------------------------------------------------------------
// No fabricated accelerator, SmartNIC or DPU claim.
// ---------------------------------------------------------------------------

TOS_TEST(hardware_synthetic_offload_classes_never_claim_real_hardware) {
  const ExecutionDomainType types[] = {ExecutionDomainType::kSmartNic, ExecutionDomainType::kDpu,
                                       ExecutionDomainType::kAccelerator};
  std::uint64_t domain_id = 8100;
  for (ExecutionDomainType type : types) {
    const std::string name = "synthetic." + std::string(to_string(type)) + ".honesty";
    const SyntheticDomainConfig config =
        make_synthetic_domain(ExecutionDomainId(domain_id), type, name);
    TOS_CHECK_EQ(config.provenance, Provenance::kSynthetic);
    TOS_CHECK_MSG(config.provenance != Provenance::kReal,
                  "a synthetic domain class must never be built with REAL provenance");

    SyntheticBackend backend("hardware.honesty.probe");
    TOS_REQUIRE(backend.add_domain(config).ok);
    const std::vector<ExecutionDomainRecord> discovered = backend.discover_domains();
    TOS_REQUIRE(discovered.size() == std::size_t{1});
    const ExecutionDomainRecord& record = discovered.front();
    TOS_CHECK_EQ(record.provenance, Provenance::kSynthetic);
    TOS_CHECK(record.provenance != Provenance::kReal);
    TOS_CHECK_EQ(record.capability.provenance, Provenance::kSynthetic);
    TOS_CHECK(record.capability.provenance != Provenance::kReal);
    TOS_CHECK(validate_domain_record(record).ok);

    // Registering the record does not relabel it either.
    SchedulerOptions options;
    options.policy = make_default_policy();
    options.host_node = "probe.host";
    Scheduler scheduler(std::move(options));
    TOS_REQUIRE(scheduler.start().ok);
    TOS_REQUIRE(scheduler.register_domain(record).ok);
    ExecutionDomainRecord stored;
    TOS_REQUIRE(scheduler.domains().get(record.id, stored));
    TOS_CHECK_MSG(stored.provenance == Provenance::kSynthetic,
                  "the runtime must never report a synthetic offload class as REAL");
    TOS_CHECK(stored.provenance != Provenance::kReal);
    TOS_REQUIRE(scheduler.shutdown().ok);
    ++domain_id;
  }
}
