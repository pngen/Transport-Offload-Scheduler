// Hard eligibility: every hard constraint individually makes a candidate
// ineligible, the structured reason is visible in the decision explanation, and no
// ineligible domain is ever selected or ranked.
//
// Hard eligibility is decided before ranking is consulted: a domain that is
// strictly better on every ranking factor still loses to a hard constraint.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include <cstdint>
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

/// One isolated eligibility case: a fresh runtime holding exactly one synthetic
/// domain published through the normal evidence path.
class DomainCase {
 public:
  explicit DomainCase(SchedulerPolicy policy = make_default_policy())
      : policy_(std::move(policy)), runtime_(config_with(policy_)) {}

  ExecutionDomainId add(const SyntheticDomainConfig& config) {
    domain_ = runtime_.add_domain(config);
    return domain_;
  }

  ExecutionDomainId add_synthetic(ExecutionDomainType type, std::uint64_t id = 1,
                                  const std::string& name = "synthetic.eligibility",
                                  bool authoritative = true) {
    return add(make_synthetic_domain(ExecutionDomainId(id), type, name, authoritative));
  }

  Scheduler& scheduler() { return runtime_.scheduler(); }
  SyntheticBackend& backend() { return runtime_.backend(); }
  tos_test::TestRuntime& runtime() { return runtime_; }
  ExecutionDomainId domain() const { return domain_; }
  const SchedulerPolicy& policy() const { return policy_; }

  PlanResult plan(const OperationRequest& request) { return runtime_.scheduler().plan(request); }
  Status refresh() { return runtime_.refresh(); }

 private:
  SchedulerPolicy policy_;
  tos_test::TestRuntime runtime_;
  ExecutionDomainId domain_;
};

bool rejected_with(const DecisionExplanation& explanation, ExecutionDomainId domain,
                   IneligibilityReason reason) {
  for (const CandidateEvaluation& candidate : explanation.candidates) {
    if (candidate.domain == domain && !candidate.eligible && candidate.reason == reason) {
      return true;
    }
  }
  return false;
}

/// The candidate must be rejected with exactly this reason, must never be selected
/// and must never appear in the ranking.
void expect_rejected(const PlanResult& result, ExecutionDomainId domain,
                     IneligibilityReason reason, SelectionOutcome outcome) {
  TOS_CHECK_MSG(!result.planned,
                std::string("plan must be rejected, outcome=") +
                    std::string(to_string(result.explanation.outcome)));
  TOS_CHECK_EQ(result.explanation.outcome, outcome);
  TOS_CHECK_MSG(rejected_with(result.explanation, domain, reason),
                std::string("expected candidate rejection reason ") +
                    std::string(to_string(reason)));
  TOS_CHECK_MSG(result.explanation.selected_domain != domain,
                "an ineligible domain must never be selected");
  TOS_CHECK(!result.explanation.selected_domain.valid());
  for (const RankedCandidate& ranked : result.explanation.ranking) {
    TOS_CHECK_MSG(ranked.domain_id != domain.value(),
                  "an ineligible domain must never be ranked as a candidate");
  }
  TOS_CHECK_EQ(result.explanation.rejected_candidates, std::uint64_t{1});
}

void expect_eligible(const PlanResult& result, ExecutionDomainId domain,
                     SelectionOutcome outcome) {
  TOS_CHECK_MSG(result.planned,
                std::string("plan must succeed, outcome=") +
                    std::string(to_string(result.explanation.outcome)));
  TOS_REQUIRE(result.planned);
  TOS_CHECK_EQ(result.explanation.outcome, outcome);
  TOS_CHECK_EQ(result.explanation.selected_domain, domain);
  TOS_CHECK_EQ(result.explanation.rejected_candidates, std::uint64_t{0});
}

}  // namespace

// ---------------------------------------------------------------------------
// Baseline: the fixture itself produces a legal, selected candidate.
// ---------------------------------------------------------------------------

TOS_TEST(eligibility_baseline_eligible_domain_is_selected) {
  DomainCase fixture;
  const ExecutionDomainId domain = fixture.add_synthetic(ExecutionDomainType::kDpu);
  const OperationRequest request = tos_test::make_request(opclass::checksum_crc32c(), 4096);

  const PlanResult result = fixture.plan(request);
  expect_eligible(result, domain, SelectionOutcome::kDpuSelected);
  TOS_CHECK_EQ(result.explanation.candidates.size(), std::size_t{1});
  TOS_REQUIRE(result.explanation.candidates.size() == 1);
  TOS_CHECK(result.explanation.candidates.front().eligible);
  TOS_CHECK_EQ(result.explanation.candidates.front().reason, IneligibilityReason::kNone);
  TOS_CHECK_EQ(result.explanation.ranking.size(), std::size_t{1});
  TOS_CHECK_EQ(result.explanation.evaluated_candidates, std::uint64_t{1});

  // The aggregate outcome codes asserted throughout this file are the canonical
  // machine-readable tokens, not merely enum ordinals.
  TOS_CHECK_EQ(std::string(to_string(SelectionOutcome::kPolicyRejected)),
               std::string("POLICY_REJECTED"));
  TOS_CHECK_EQ(std::string(to_string(SelectionOutcome::kCapabilityUnsupported)),
               std::string("CAPABILITY_UNSUPPORTED"));
  TOS_CHECK_EQ(std::string(to_string(SelectionOutcome::kIsolationRejected)),
               std::string("ISOLATION_REJECTED"));
  TOS_CHECK_EQ(std::string(to_string(SelectionOutcome::kCompatibilityRejected)),
               std::string("COMPATIBILITY_REJECTED"));
  TOS_CHECK_EQ(std::string(to_string(SelectionOutcome::kNoEligibleDomain)),
               std::string("NO_ELIGIBLE_DOMAIN"));
  TOS_CHECK_EQ(std::string(to_string(SelectionOutcome::kStaleEvidence)),
               std::string("STALE_EVIDENCE"));
  TOS_CHECK_EQ(std::string(to_string(SelectionOutcome::kCapacityUnavailable)),
               std::string("CAPACITY_UNAVAILABLE"));
}

// ---------------------------------------------------------------------------
// 1. Operation not supported by the domain.
// ---------------------------------------------------------------------------

TOS_TEST(eligibility_operation_not_supported) {
  DomainCase fixture;
  const ExecutionDomainId domain = fixture.add_synthetic(ExecutionDomainType::kDpu);
  const OperationRequest request = tos_test::make_request(opclass::checksum_crc32c(), 4096);
  TOS_REQUIRE(fixture.plan(request).planned);

  TOS_REQUIRE(fixture.backend()
                  .set_operation_support(domain, opclass::checksum_crc32c(), false)
                  .ok);
  TOS_REQUIRE(fixture.refresh().ok);

  const PlanResult result = fixture.plan(request);
  expect_rejected(result, domain, IneligibilityReason::kOperationNotSupported,
                  SelectionOutcome::kCapabilityUnsupported);
}

// ---------------------------------------------------------------------------
// 2. Memory domain not addressable.
// ---------------------------------------------------------------------------

TOS_TEST(eligibility_memory_domain_not_addressable) {
  DomainCase fixture;
  SyntheticDomainConfig config = make_synthetic_domain(ExecutionDomainId(1),
                                                       ExecutionDomainType::kDpu,
                                                       "synthetic.dpu.hostonly");
  config.capability.memory_domains = memory_bit(MemoryDomain::kHost);
  const ExecutionDomainId domain = fixture.add(config);

  // Host memory is addressable, so the same request is otherwise legal.
  const OperationRequest host_request =
      tos_test::make_request(opclass::checksum_crc32c(), 4096, MemoryDomain::kHost,
                             MemoryDomain::kHost);
  TOS_REQUIRE(fixture.plan(host_request).planned);

  const OperationRequest device_request =
      tos_test::make_request(opclass::checksum_crc32c(), 4096, MemoryDomain::kDeviceLocal,
                             MemoryDomain::kHost);
  const PlanResult result = fixture.plan(device_request);
  expect_rejected(result, domain, IneligibilityReason::kMemoryDomainUnsupported,
                  SelectionOutcome::kCapabilityUnsupported);

  // The destination side is checked independently.
  const OperationRequest destination_request =
      tos_test::make_request(opclass::checksum_crc32c(), 4096, MemoryDomain::kHost,
                             MemoryDomain::kDeviceLocal);
  expect_rejected(fixture.plan(destination_request), domain,
                  IneligibilityReason::kMemoryDomainUnsupported,
                  SelectionOutcome::kCapabilityUnsupported);
}

// ---------------------------------------------------------------------------
// 3. Payload above the declared maximum / below the declared minimum.
// ---------------------------------------------------------------------------

TOS_TEST(eligibility_payload_too_large) {
  DomainCase fixture;
  SyntheticDomainConfig config = make_synthetic_domain(ExecutionDomainId(1),
                                                       ExecutionDomainType::kDpu,
                                                       "synthetic.dpu.small");
  config.capability.max_payload_bytes = 512;
  const ExecutionDomainId domain = fixture.add(config);

  TOS_REQUIRE(fixture.plan(tos_test::make_request(opclass::checksum_crc32c(), 512)).planned);

  const PlanResult result =
      fixture.plan(tos_test::make_request(opclass::checksum_crc32c(), 4096));
  expect_rejected(result, domain, IneligibilityReason::kPayloadTooLarge,
                  SelectionOutcome::kCapabilityUnsupported);
}

TOS_TEST(eligibility_payload_too_small) {
  DomainCase fixture;
  SyntheticDomainConfig config = make_synthetic_domain(ExecutionDomainId(1),
                                                       ExecutionDomainType::kDpu,
                                                       "synthetic.dpu.minimum");
  config.capability.min_payload_bytes = 1024;
  const ExecutionDomainId domain = fixture.add(config);

  TOS_REQUIRE(fixture.plan(tos_test::make_request(opclass::checksum_crc32c(), 1024)).planned);

  const PlanResult result =
      fixture.plan(tos_test::make_request(opclass::checksum_crc32c(), 256));
  expect_rejected(result, domain, IneligibilityReason::kPayloadTooSmall,
                  SelectionOutcome::kCapabilityUnsupported);
}

// ---------------------------------------------------------------------------
// 5. Alignment stronger than the engine guarantee.
// ---------------------------------------------------------------------------

TOS_TEST(eligibility_alignment_unsatisfied) {
  DomainCase fixture;
  SyntheticDomainConfig config = make_synthetic_domain(ExecutionDomainId(1),
                                                       ExecutionDomainType::kDpu,
                                                       "synthetic.dpu.alignment");
  config.capability.alignment_bytes = 8;
  const ExecutionDomainId domain = fixture.add(config);

  OperationRequest request = tos_test::make_request(opclass::checksum_crc32c(), 4096);
  request.payload.alignment_bytes = 64;
  expect_rejected(fixture.plan(request), domain, IneligibilityReason::kAlignmentUnsatisfied,
                  SelectionOutcome::kCapabilityUnsupported);
}

// ---------------------------------------------------------------------------
// 6. Isolation below the minimum required by policy, and by the request.
// ---------------------------------------------------------------------------

TOS_TEST(eligibility_isolation_policy_minimum) {
  SchedulerPolicy policy = make_default_policy();
  policy.minimum_isolation = IsolationClass::kSeparateAddressSpace;
  DomainCase fixture(policy);

  SyntheticDomainConfig config = make_synthetic_domain(ExecutionDomainId(1),
                                                       ExecutionDomainType::kDpu,
                                                       "synthetic.dpu.queue");
  config.isolation = IsolationClass::kSeparateQueue;
  const ExecutionDomainId domain = fixture.add(config);

  const PlanResult result =
      fixture.plan(tos_test::make_request(opclass::checksum_crc32c(), 4096));
  expect_rejected(result, domain, IneligibilityReason::kIsolationInsufficient,
                  SelectionOutcome::kIsolationRejected);

  // A domain proving the required class is legal under the same policy.
  DomainCase strong(policy);
  SyntheticDomainConfig strong_config = make_synthetic_domain(
      ExecutionDomainId(1), ExecutionDomainType::kDpu, "synthetic.dpu.addressspace");
  strong_config.isolation = IsolationClass::kSeparateAddressSpace;
  const ExecutionDomainId strong_domain = strong.add(strong_config);
  expect_eligible(strong.plan(tos_test::make_request(opclass::checksum_crc32c(), 4096)),
                  strong_domain, SelectionOutcome::kDpuSelected);
}

TOS_TEST(eligibility_isolation_request_requirement) {
  DomainCase fixture;
  const ExecutionDomainId domain = fixture.add_synthetic(ExecutionDomainType::kDpu);
  OperationRequest request = tos_test::make_request(opclass::checksum_crc32c(), 4096);
  request.required_isolation = IsolationClass::kDedicatedDeviceService;

  const PlanResult result = fixture.plan(request);
  expect_rejected(result, domain, IneligibilityReason::kIsolationInsufficient,
                  SelectionOutcome::kIsolationRejected);
}

// ---------------------------------------------------------------------------
// 7. Compatibility mismatch.
// ---------------------------------------------------------------------------

TOS_TEST(eligibility_compatibility_driver_version) {
  DomainCase fixture;
  const ExecutionDomainId domain = fixture.add_synthetic(ExecutionDomainType::kDpu);
  OperationRequest request = tos_test::make_request(opclass::checksum_crc32c(), 4096);
  request.compatibility.minimum_driver_backend_version = 2;  // published value is 1

  const PlanResult result = fixture.plan(request);
  expect_rejected(result, domain, IneligibilityReason::kCompatibilityMismatch,
                  SelectionOutcome::kCompatibilityRejected);
}

TOS_TEST(eligibility_compatibility_backend_family) {
  DomainCase fixture;
  const ExecutionDomainId domain = fixture.add_synthetic(ExecutionDomainType::kDpu);
  OperationRequest request = tos_test::make_request(opclass::checksum_crc32c(), 4096);
  request.compatibility.required_backend_family = "vendor.other.backend";

  const PlanResult result = fixture.plan(request);
  expect_rejected(result, domain, IneligibilityReason::kCompatibilityMismatch,
                  SelectionOutcome::kCompatibilityRejected);
}

TOS_TEST(eligibility_compatibility_accelerator_arch) {
  DomainCase fixture;
  SyntheticDomainConfig config = make_synthetic_domain(ExecutionDomainId(1),
                                                       ExecutionDomainType::kAccelerator,
                                                       "synthetic.accelerator.arch");
  config.compatibility.accelerator_arch = 7;
  const ExecutionDomainId domain = fixture.add(config);

  OperationRequest request = tos_test::make_request(opclass::checksum_crc32c(), 4096);
  request.compatibility.required_accelerator_arch = 9;

  const PlanResult result = fixture.plan(request);
  expect_rejected(result, domain, IneligibilityReason::kCompatibilityMismatch,
                  SelectionOutcome::kCompatibilityRejected);
}

// ---------------------------------------------------------------------------
// 8. Policy forbidden domain class and identity.
// ---------------------------------------------------------------------------

TOS_TEST(eligibility_policy_forbidden_domain_class) {
  SchedulerPolicy policy = make_default_policy();
  policy.forbidden_domains = {ExecutionDomainType::kDpu};
  DomainCase fixture(policy);
  const ExecutionDomainId domain = fixture.add_synthetic(ExecutionDomainType::kDpu);

  const PlanResult result =
      fixture.plan(tos_test::make_request(opclass::checksum_crc32c(), 4096));
  expect_rejected(result, domain, IneligibilityReason::kPolicyForbidden,
                  SelectionOutcome::kPolicyRejected);
}

TOS_TEST(eligibility_policy_forbidden_domain_identity) {
  const ExecutionDomainId forbidden(7);
  SchedulerPolicy policy = make_default_policy();
  policy.forbidden_domain_ids = {forbidden};
  DomainCase fixture(policy);
  const ExecutionDomainId domain = fixture.add_synthetic(ExecutionDomainType::kDpu, 7);
  TOS_CHECK_EQ(domain, forbidden);

  const PlanResult result =
      fixture.plan(tos_test::make_request(opclass::checksum_crc32c(), 4096));
  expect_rejected(result, domain, IneligibilityReason::kPolicyForbidden,
                  SelectionOutcome::kPolicyRejected);
}

TOS_TEST(eligibility_offload_required_rejects_host_cpu) {
  SchedulerPolicy policy = make_default_policy();
  policy.offload_requirement = OffloadRequirement::kOffloadRequired;
  DomainCase fixture(policy);
  const ExecutionDomainId cpu = tos_test::add_cpu_domain(fixture.runtime(), 3);
  TOS_REQUIRE(cpu.valid());

  const PlanResult result =
      fixture.plan(tos_test::make_request(opclass::checksum_crc32c(), 4096));
  expect_rejected(result, cpu, IneligibilityReason::kPolicyForbidden,
                  SelectionOutcome::kPolicyRejected);
  TOS_CHECK_MSG(result.explanation.has_reason("offload_required.no_offload_domain"),
                "the offload-required rejection must carry its named reason");
}

// ---------------------------------------------------------------------------
// 9./10. Provenance: forbidden, and UNSUPPORTED which is never eligible.
// ---------------------------------------------------------------------------

TOS_TEST(eligibility_provenance_disallowed) {
  SchedulerPolicy policy = make_default_policy();
  policy.forbidden_provenance = {Provenance::kSynthetic};
  DomainCase fixture(policy);
  const ExecutionDomainId domain = fixture.add_synthetic(ExecutionDomainType::kDpu);

  const PlanResult result =
      fixture.plan(tos_test::make_request(opclass::checksum_crc32c(), 4096));
  expect_rejected(result, domain, IneligibilityReason::kProvenanceDisallowed,
                  SelectionOutcome::kPolicyRejected);
}

TOS_TEST(eligibility_unsupported_provenance_is_never_eligible) {
  const OperationRequest request = tos_test::make_request(opclass::checksum_crc32c(), 4096);

  // Default policy: allow_unsupported_provenance is false.
  {
    DomainCase fixture;
    SyntheticDomainConfig config = make_synthetic_domain(
        ExecutionDomainId(1), ExecutionDomainType::kDpu, "synthetic.dpu.unsupported", false);
    config.provenance = Provenance::kUnsupported;
    const ExecutionDomainId domain = fixture.add(config);
    TOS_CHECK(!fixture.policy().allow_unsupported_provenance);

    const PlanResult result = fixture.plan(request);
    expect_rejected(result, domain, IneligibilityReason::kBackendUnsupported,
                    SelectionOutcome::kCapabilityUnsupported);
  }

  // Even when policy explicitly allows UNSUPPORTED domains the capability check
  // refuses to treat a claim with no supporting evidence as authority.
  {
    SchedulerPolicy policy = make_default_policy();
    policy.allow_unsupported_provenance = true;
    DomainCase fixture(policy);
    SyntheticDomainConfig config = make_synthetic_domain(
        ExecutionDomainId(1), ExecutionDomainType::kDpu, "synthetic.dpu.unsupported.allowed", false);
    config.provenance = Provenance::kUnsupported;
    const ExecutionDomainId domain = fixture.add(config);

    const PlanResult result = fixture.plan(request);
    expect_rejected(result, domain, IneligibilityReason::kBackendUnsupported,
                    SelectionOutcome::kCapabilityUnsupported);
  }
}

// ---------------------------------------------------------------------------
// 11. Unproven capability fails closed while positive evidence is required.
// ---------------------------------------------------------------------------

TOS_TEST(eligibility_unproven_capability_fails_closed) {
  const OperationRequest request = tos_test::make_request(opclass::checksum_crc32c(), 4096);

  // (a) non-authoritative capability record.
  {
    DomainCase fixture;
    const ExecutionDomainId domain =
        fixture.add_synthetic(ExecutionDomainType::kDpu, 1, "synthetic.dpu.nonauthoritative", false);
    TOS_CHECK(fixture.policy().require_positive_evidence);
    expect_rejected(fixture.plan(request), domain, IneligibilityReason::kCapabilityUnproven,
                    SelectionOutcome::kCapabilityUnsupported);
  }

  // (b) authoritative record that proves no flags at all.
  {
    DomainCase fixture;
    SyntheticDomainConfig config = make_synthetic_domain(ExecutionDomainId(1),
                                                         ExecutionDomainType::kDpu,
                                                         "synthetic.dpu.noflags");
    config.capability.flags = 0;
    const ExecutionDomainId domain = fixture.add(config);
    expect_rejected(fixture.plan(request), domain, IneligibilityReason::kCapabilityUnproven,
                    SelectionOutcome::kCapabilityUnsupported);
  }
}

TOS_TEST(eligibility_unproven_capability_allowed_when_policy_permits) {
  // Control: the rejection above is caused by require_positive_evidence, not by the
  // capability content. With the policy switched off the same record is eligible.
  SchedulerPolicy policy = make_default_policy();
  policy.require_positive_evidence = false;
  DomainCase fixture(policy);
  const ExecutionDomainId domain =
      fixture.add_synthetic(ExecutionDomainType::kDpu, 1, "synthetic.dpu.unproven.allowed", false);

  expect_eligible(fixture.plan(tos_test::make_request(opclass::checksum_crc32c(), 4096)), domain,
                  SelectionOutcome::kDpuSelected);
}

// ---------------------------------------------------------------------------
// 12. Health, readiness and withdrawn dynamic evidence.
// ---------------------------------------------------------------------------

TOS_TEST(eligibility_health_not_ready) {
  DomainCase fixture;
  const ExecutionDomainId domain = fixture.add_synthetic(ExecutionDomainType::kDpu);
  const OperationRequest request = tos_test::make_request(opclass::checksum_crc32c(), 4096);
  TOS_REQUIRE(fixture.plan(request).planned);

  TOS_REQUIRE(fixture.backend().set_health(domain, false, false, false).ok);
  TOS_REQUIRE(fixture.refresh().ok);

  const PlanResult result = fixture.plan(request);
  expect_rejected(result, domain, IneligibilityReason::kHealthNotReady,
                  SelectionOutcome::kStaleEvidence);
}

TOS_TEST(eligibility_unpublished_dynamic_evidence_is_ineligible) {
  // Registered directly, because the local publisher cannot express "this domain
  // has no current dynamic evidence yet" (see the two tests below).
  DomainCase fixture;
  SyntheticDomainConfig config = make_synthetic_domain(ExecutionDomainId(1),
                                                       ExecutionDomainType::kDpu,
                                                       "synthetic.dpu.noload");
  config.dynamic_evidence_published = false;
  TOS_REQUIRE(fixture.backend().add_domain(config).ok);

  const std::vector<ExecutionDomainRecord> discovered = fixture.backend().discover_domains();
  TOS_REQUIRE(discovered.size() == std::size_t{1});
  const ExecutionDomainRecord& record = discovered.front();
  TOS_CHECK(!record.load.published());
  TOS_REQUIRE(fixture.scheduler().register_domain(record).ok);
  TOS_REQUIRE(fixture.scheduler().publish_capability(record.capability).ok);
  TOS_REQUIRE(fixture.scheduler().publish_locality(record.id, record.locality).ok);
  TOS_REQUIRE(fixture.scheduler().publish_topology(record.id, record.topology).ok);
  TOS_REQUIRE(fixture.scheduler().publish_compatibility(record.id, record.compatibility).ok);
  TOS_REQUIRE(fixture.scheduler().publish_capacity(record.id, record.capacity).ok);

  const PlanResult result =
      fixture.plan(tos_test::make_request(opclass::checksum_crc32c(), 4096));
  expect_rejected(result, record.id, IneligibilityReason::kHealthNotReady,
                  SelectionOutcome::kStaleEvidence);
}

// DEFECT: LocalDomainPublisher::sync() cannot publish a domain that has no current
// dynamic evidence. The registration branch calls publish_load() unconditionally
// (unlike the update branch and publish_load(), which both skip unpublished load),
// the registry answers "domain.load_not_published", and sync() returns that failure.
// Because sync() returns at the first failure, every later domain in the same
// discovery batch is left unpublished: one silent backend instance hides all
// healthy domains behind it.
TOS_TEST(eligibility_domain_without_dynamic_evidence_does_not_abort_publication) {
  DomainCase fixture;
  SyntheticDomainConfig silent = make_synthetic_domain(ExecutionDomainId(1),
                                                       ExecutionDomainType::kDpu,
                                                       "synthetic.dpu.silent");
  silent.dynamic_evidence_published = false;
  TOS_REQUIRE(fixture.backend().add_domain(silent).ok);
  SyntheticDomainConfig healthy = make_synthetic_domain(ExecutionDomainId(2),
                                                        ExecutionDomainType::kDpu,
                                                        "synthetic.dpu.healthy");
  TOS_REQUIRE(fixture.backend().add_domain(healthy).ok);

  const Status synced = fixture.runtime().publisher().sync();
  TOS_CHECK_MSG(synced.ok,
                std::string("a domain without current dynamic evidence is a legal state, "
                            "not a publication failure: ") +
                    synced.code);
  TOS_CHECK_MSG(fixture.scheduler().domains().find(ExecutionDomainId(2)).has_value(),
                "a healthy domain in the same discovery batch must still be published");
}

// DEFECT: withdrawing a backend's dynamic evidence does not reach the scheduler.
// The synthetic backend withholds health/load (SyntheticBackend::
// withdraw_dynamic_evidence, "as a restart would"), LocalDomainPublisher::sync
// silently skips a record whose load is unpublished, and the registry therefore
// keeps the last published health/load evidence, so the domain stays eligible and
// even stays dispatchable. ExecutionDomainRecord::dynamic_evidence_current() and
// DomainRegistry::mark_dynamic_evidence_stale() both model the intended behaviour,
// so this test asserts the specified behaviour: withdrawal makes the domain
// ineligible with kHealthNotReady.
TOS_TEST(eligibility_dynamic_evidence_withdrawn_is_ineligible) {
  DomainCase fixture;
  const ExecutionDomainId domain = fixture.add_synthetic(ExecutionDomainType::kDpu);
  const OperationRequest request = tos_test::make_request(opclass::checksum_crc32c(), 4096);
  TOS_REQUIRE(fixture.plan(request).planned);

  TOS_REQUIRE(fixture.backend().withdraw_dynamic_evidence(domain).ok);
  TOS_REQUIRE(fixture.refresh().ok);

  // The backend withholds health/load, yet the registry still holds the previous
  // published evidence, so nothing marks the domain stale.
  ExecutionDomainRecord kept;
  TOS_REQUIRE(fixture.scheduler().domains().get(domain, kept));
  TOS_CHECK_MSG(!kept.load.published(),
                "withdrawn dynamic evidence must not stay published in the registry");

  const PlanResult result = fixture.plan(request);
  expect_rejected(result, domain, IneligibilityReason::kHealthNotReady,
                  SelectionOutcome::kStaleEvidence);
}

// ---------------------------------------------------------------------------
// 13. Worker-boot authority.
// ---------------------------------------------------------------------------

TOS_TEST(eligibility_worker_boot_authority) {
  DomainCase fixture;
  const WorkerId worker(0xA11CE);
  const WorkerBootId boot(0xB007);

  SyntheticDomainConfig config = make_synthetic_domain(ExecutionDomainId(1),
                                                       ExecutionDomainType::kDpu,
                                                       "synthetic.dpu.worker");
  config.worker = worker;
  config.worker_boot = boot;
  TOS_REQUIRE(fixture.backend().add_domain(config).ok);

  // Register the record directly: no worker incarnation has been announced yet.
  const std::vector<ExecutionDomainRecord> discovered = fixture.backend().discover_domains();
  TOS_REQUIRE(discovered.size() == std::size_t{1});
  const ExecutionDomainRecord& record = discovered.front();
  TOS_CHECK_EQ(record.worker, worker);
  TOS_CHECK_EQ(record.worker_boot, boot);
  TOS_CHECK(!fixture.scheduler().workers().is_current(worker, boot));
  TOS_REQUIRE(fixture.scheduler().register_domain(record).ok);
  TOS_REQUIRE(fixture.scheduler().publish_capability(record.capability).ok);
  TOS_REQUIRE(fixture.scheduler().publish_locality(record.id, record.locality).ok);
  TOS_REQUIRE(fixture.scheduler().publish_topology(record.id, record.topology).ok);
  TOS_REQUIRE(fixture.scheduler().publish_compatibility(record.id, record.compatibility).ok);
  TOS_REQUIRE(fixture.scheduler().publish_capacity(record.id, record.capacity).ok);
  TOS_REQUIRE(fixture.scheduler().publish_load(record.id, record.load).ok);

  const OperationRequest request = tos_test::make_request(opclass::checksum_crc32c(), 4096);
  const PlanResult result = fixture.plan(request);
  expect_rejected(result, record.id, IneligibilityReason::kWorkerBootStale,
                  SelectionOutcome::kStaleEvidence);

  // Announcing the very same incarnation makes the identical record eligible.
  TOS_REQUIRE(fixture.scheduler().register_worker_boot(worker, boot, "test").ok);
  expect_eligible(fixture.plan(request), record.id, SelectionOutcome::kDpuSelected);
}

// ---------------------------------------------------------------------------
// 14. Capacity.
// ---------------------------------------------------------------------------

TOS_TEST(eligibility_capacity_unavailable) {
  SchedulerPolicy policy = make_default_policy();
  policy.reservation.enabled = true;
  policy.reservation.per_operation[static_cast<std::size_t>(ResourceKind::kExecutionSlot)] = 4;
  DomainCase fixture(policy);

  SyntheticDomainConfig config = make_synthetic_domain(ExecutionDomainId(1),
                                                       ExecutionDomainType::kDpu,
                                                       "synthetic.dpu.nocapacity");
  config.capacity = CapacityVector{};
  const ExecutionDomainId domain = fixture.add(config);

  const PlanResult result =
      fixture.plan(tos_test::make_request(opclass::checksum_crc32c(), 4096));
  expect_rejected(result, domain, IneligibilityReason::kCapacityUnavailable,
                  SelectionOutcome::kCapacityUnavailable);

  // Control: the same policy with capacity declared is eligible.
  DomainCase stocked(policy);
  SyntheticDomainConfig stocked_config = make_synthetic_domain(ExecutionDomainId(1),
                                                               ExecutionDomainType::kDpu,
                                                               "synthetic.dpu.capacity");
  stocked_config.capacity[static_cast<std::size_t>(ResourceKind::kExecutionSlot)] = 4;
  const ExecutionDomainId stocked_domain = stocked.add(stocked_config);
  expect_eligible(stocked.plan(tos_test::make_request(opclass::checksum_crc32c(), 4096)),
                  stocked_domain, SelectionOutcome::kDpuSelected);
}

// ---------------------------------------------------------------------------
// 15. Locality hard constraints.
// ---------------------------------------------------------------------------

TOS_TEST(eligibility_locality_minimum_class) {
  DomainCase fixture;
  SyntheticDomainConfig config = make_synthetic_domain(ExecutionDomainId(1),
                                                       ExecutionDomainType::kDpu,
                                                       "synthetic.dpu.remote");
  config.locality.class_to_payload = LocalityClass::kRemoteHost;
  const ExecutionDomainId domain = fixture.add(config);

  OperationRequest request = tos_test::make_request(opclass::checksum_crc32c(), 4096);
  request.locality.minimum_class = LocalityClass::kSameNumaNode;
  expect_rejected(fixture.plan(request), domain, IneligibilityReason::kLocalityViolation,
                  SelectionOutcome::kNoEligibleDomain);
}

TOS_TEST(eligibility_locality_required_numa_node) {
  DomainCase fixture;
  SyntheticDomainConfig config = make_synthetic_domain(ExecutionDomainId(1),
                                                       ExecutionDomainType::kDpu,
                                                       "synthetic.dpu.numa");
  config.locality.numa_node = 0;
  const ExecutionDomainId domain = fixture.add(config);

  OperationRequest request = tos_test::make_request(opclass::checksum_crc32c(), 4096);
  request.locality.required_numa_node = 5;
  expect_rejected(fixture.plan(request), domain, IneligibilityReason::kLocalityViolation,
                  SelectionOutcome::kNoEligibleDomain);
}

TOS_TEST(eligibility_locality_requires_local_nic) {
  DomainCase fixture;
  SyntheticDomainConfig config = make_synthetic_domain(ExecutionDomainId(1),
                                                       ExecutionDomainType::kAccelerator,
                                                       "synthetic.accelerator.nonic");
  config.locality.class_to_payload = LocalityClass::kDomainLocal;
  config.locality.has_local_nic = false;
  const ExecutionDomainId domain = fixture.add(config);

  OperationRequest request = tos_test::make_request(opclass::checksum_crc32c(), 4096);
  request.locality.require_local_nic = true;
  expect_rejected(fixture.plan(request), domain, IneligibilityReason::kLocalityViolation,
                  SelectionOutcome::kNoEligibleDomain);
}

TOS_TEST(eligibility_locality_requires_same_host) {
  DomainCase fixture;
  SyntheticDomainConfig config = make_synthetic_domain(ExecutionDomainId(1),
                                                       ExecutionDomainType::kDpu,
                                                       "synthetic.dpu.remotehost");
  config.locality.class_to_payload = LocalityClass::kUnknown;
  const ExecutionDomainId domain = fixture.add(config);

  OperationRequest request = tos_test::make_request(opclass::checksum_crc32c(), 4096);
  request.locality.require_same_host = true;
  expect_rejected(fixture.plan(request), domain, IneligibilityReason::kLocalityViolation,
                  SelectionOutcome::kNoEligibleDomain);
}

// ---------------------------------------------------------------------------
// 16. Fencing.
// ---------------------------------------------------------------------------

TOS_TEST(eligibility_domain_fenced) {
  DomainCase fixture;
  const ExecutionDomainId domain = fixture.add_synthetic(ExecutionDomainType::kDpu);
  const OperationRequest request = tos_test::make_request(opclass::checksum_crc32c(), 4096);
  TOS_REQUIRE(fixture.plan(request).planned);

  TOS_REQUIRE(fixture.scheduler().fence_domain(domain, "test").ok);

  const PlanResult result = fixture.plan(request);
  expect_rejected(result, domain, IneligibilityReason::kDomainFenced,
                  SelectionOutcome::kStaleEvidence);
}

// ---------------------------------------------------------------------------
// Aggregate outcome with no domain registered at all.
// ---------------------------------------------------------------------------

TOS_TEST(eligibility_no_domain_registered) {
  DomainCase fixture;
  const PlanResult result =
      fixture.plan(tos_test::make_request(opclass::checksum_crc32c(), 4096));

  TOS_CHECK(!result.planned);
  TOS_CHECK_EQ(result.explanation.outcome, SelectionOutcome::kNoEligibleDomain);
  TOS_CHECK_EQ(result.status.code, std::string("plan.no_eligible_domain"));
  TOS_CHECK_EQ(result.explanation.evaluated_candidates, std::uint64_t{0});
  TOS_CHECK_EQ(result.explanation.rejected_candidates, std::uint64_t{0});
  TOS_CHECK(result.explanation.candidates.empty());
  TOS_CHECK(result.explanation.has_reason("no_domains_registered"));
}

// ---------------------------------------------------------------------------
// Hard eligibility outranks scoring: a strictly better domain that violates one
// hard constraint must lose to a strictly worse legal one.
// ---------------------------------------------------------------------------

TOS_TEST(eligibility_hard_constraint_beats_ranking) {
  const ExecutionDomainId better_id(11);
  const ExecutionDomainId worse_id(12);

  SchedulerPolicy policy = make_default_policy();
  policy.forbidden_domain_ids = {better_id};  // the hard constraint
  DomainCase fixture(policy);

  SyntheticDomainConfig better = make_synthetic_domain(better_id, ExecutionDomainType::kNic,
                                                       "synthetic.nic.better");
  better.locality.class_to_payload = LocalityClass::kDomainLocal;
  better.isolation = IsolationClass::kDedicatedDeviceService;
  better.utilization_percent = 0;
  better.congestion_percent = 0;
  better.queue_depth = 0;
  better.observed_latency_ns = 1;
  better.throughput_bytes_per_second = 1ULL << 32;
  TOS_CHECK_EQ(fixture.add(better), better_id);

  SyntheticDomainConfig worse = make_synthetic_domain(worse_id, ExecutionDomainType::kNic,
                                                      "synthetic.nic.worse");
  worse.locality.class_to_payload = LocalityClass::kRemoteHost;
  worse.isolation = IsolationClass::kSharedProcess;
  worse.utilization_percent = 95;
  worse.congestion_percent = 95;
  worse.queue_depth = 256;
  worse.observed_latency_ns = 10ULL * 1000ULL * 1000ULL;
  worse.throughput_bytes_per_second = 1ULL << 20;
  TOS_CHECK_EQ(fixture.add(worse), worse_id);

  const OperationRequest request = tos_test::make_request(opclass::checksum_crc32c(), 65536);

  ExecutionDomainRecord better_record;
  ExecutionDomainRecord worse_record;
  TOS_REQUIRE(fixture.scheduler().domains().get(better_id, better_record));
  TOS_REQUIRE(fixture.scheduler().domains().get(worse_id, worse_record));

  FactorInputs inputs;
  inputs.request = &request;
  inputs.policy = &fixture.policy();
  inputs.total_setup_cost_units =
      policy.type_setup_cost_units[static_cast<std::size_t>(ExecutionDomainType::kNic)];
  inputs.fallback_depth = 0;
  inputs.domain = &better_record;
  inputs.capability = &better_record.capability.capability;
  const FactorVector better_factors = compute_factors(inputs);
  inputs.domain = &worse_record;
  inputs.capability = &worse_record.capability.capability;
  const FactorVector worse_factors = compute_factors(inputs);

  std::size_t strictly_better = 0;
  for (std::size_t i = 0; i < kRankingFactorCount; ++i) {
    TOS_CHECK_MSG(better_factors[i] >= worse_factors[i],
                  std::string("the rejected domain must not score below the legal one on ") +
                      std::string(to_string(static_cast<RankingFactor>(i))));
    if (better_factors[i] > worse_factors[i]) ++strictly_better;
  }
  TOS_CHECK_MSG(strictly_better >= std::size_t{9},
                "the rejected domain must dominate the legal one on the ranking factors");
  std::int64_t better_total_weight = 0;
  std::int64_t worse_total_weight = 0;
  const std::int64_t better_score =
      weighted_score(better_factors, policy.weights, better_total_weight);
  const std::int64_t worse_score = weighted_score(worse_factors, policy.weights, worse_total_weight);
  TOS_CHECK(better_score > worse_score);

  // The hard constraint still decides.
  const PlanResult result = fixture.plan(request);
  TOS_REQUIRE(result.planned);
  TOS_CHECK_EQ(result.explanation.selected_domain, worse_id);
  TOS_CHECK_EQ(result.explanation.outcome, SelectionOutcome::kNicSelected);
  TOS_CHECK(rejected_with(result.explanation, better_id, IneligibilityReason::kPolicyForbidden));
  TOS_CHECK_EQ(result.explanation.ranking.size(), std::size_t{1});
  TOS_REQUIRE(result.explanation.ranking.size() == 1);
  TOS_CHECK_EQ(result.explanation.ranking.front().domain_id, worse_id.value());
}
