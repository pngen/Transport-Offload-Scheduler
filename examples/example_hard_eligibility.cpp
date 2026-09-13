// Hard eligibility: a fast domain that cannot prove a hard requirement never wins.
//
// Hard eligibility is decided before ranking is consulted. This scenario publishes four
// domains. Three of them would rank well - the synthetic SmartNIC is first in the
// deployment's preference order, the accelerator class has the best throughput, and the
// host CPU is cheapest to start - but none of them can prove the isolation the policy
// requires, and one DPU cannot prove any operation support at all. Only the proven DPU
// remains eligible, and the example verifies that no rejected candidate appears in the
// ranking and prints the structured rejection reason for each candidate.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "tos/tos.hpp"

using namespace tos;

namespace {

struct Runtime {
  std::unique_ptr<Scheduler> scheduler;
  std::shared_ptr<SyntheticBackend> backend;
  std::shared_ptr<LocalDispatchChannel> channel;
  std::unique_ptr<LocalDomainPublisher> publisher;
};

bool build_runtime(Runtime& runtime, const SchedulerPolicy& policy) {
  SchedulerOptions options;
  options.policy = policy;
  options.host_node = "example.host";
  runtime.scheduler = std::make_unique<Scheduler>(std::move(options));
  const Status started = runtime.scheduler->start();
  if (!started) {
    std::cerr << "scheduler start failed: " << started.code << std::endl;
    return false;
  }
  runtime.backend = std::make_shared<SyntheticBackend>("example.synthetic");
  runtime.channel = std::make_shared<LocalDispatchChannel>(
      runtime.backend, [](const CompletionSubmission&) {}, LocalChannelOptions{});
  runtime.scheduler->set_dispatch_channel(runtime.channel);
  runtime.publisher = std::make_unique<LocalDomainPublisher>(*runtime.scheduler, runtime.backend);
  return true;
}

Status publish_record(Scheduler& scheduler, const ExecutionDomainRecord& record) {
  const Status registered = scheduler.register_domain(record);
  if (!registered) return registered;
  const Status capability = scheduler.publish_capability(record.capability);
  if (!capability) return capability;
  const Status locality = scheduler.publish_locality(record.id, record.locality);
  if (!locality) return locality;
  const Status topology = scheduler.publish_topology(record.id, record.topology);
  if (!topology) return topology;
  const Status compatibility = scheduler.publish_compatibility(record.id, record.compatibility);
  if (!compatibility) return compatibility;
  const Status capacity = scheduler.publish_capacity(record.id, record.capacity);
  if (!capacity) return capacity;
  return scheduler.publish_load(record.id, record.load);
}

bool add_synthetic(Runtime& runtime, SyntheticDomainConfig config) {
  const ExecutionDomainId id = config.id;
  const Status added = runtime.backend->add_domain(std::move(config));
  if (!added) {
    std::cerr << "domain " << id.value() << " publication failed: " << added.code << std::endl;
    return false;
  }
  const Status synced = runtime.publisher->sync();
  if (!synced) {
    std::cerr << "publisher sync failed: " << synced.code << std::endl;
    return false;
  }
  return true;
}

std::string domain_name(const std::vector<ExecutionDomainRecord>& domains, ExecutionDomainId id) {
  for (const ExecutionDomainRecord& domain : domains) {
    if (domain.id == id) return domain.name;
  }
  return "(unknown)";
}

}  // namespace

int main() {
  // The deployment requires an engine that proves address-space isolation and it prefers
  // SmartNICs above everything else. Preference can never outrank a hard requirement.
  SchedulerPolicy policy = make_default_policy();
  policy.minimum_isolation = IsolationClass::kSeparateAddressSpace;
  policy.preference_order = {ExecutionDomainType::kSmartNic, ExecutionDomainType::kDpu,
                             ExecutionDomainType::kAccelerator, ExecutionDomainType::kCpu};

  Runtime runtime;
  if (!build_runtime(runtime, policy)) return 1;

  CpuBackend::Options cpu_options;
  cpu_options.domain_id = ExecutionDomainId(1);
  cpu_options.name = "cpu.host.0";
  cpu_options.parent_host = "example.host";
  CpuBackend cpu_probe(cpu_options);
  const std::vector<ExecutionDomainRecord> discovered = cpu_probe.discover_domains();
  if (discovered.empty()) {
    std::cerr << "cpu discovery failed" << std::endl;
    return 1;
  }
  const Status cpu_published = publish_record(*runtime.scheduler, discovered.front());
  if (!cpu_published) {
    std::cerr << "cpu publication failed: " << cpu_published.code << " " << cpu_published.message
              << std::endl;
    return 1;
  }

  // The fast domain: first in the preference order and the best throughput class, but it
  // can only prove shared-process isolation, which is below the policy minimum.
  SyntheticDomainConfig fast = make_synthetic_domain(ExecutionDomainId(2),
                                                     ExecutionDomainType::kSmartNic,
                                                     "synthetic.smartnic.fast");
  fast.isolation = IsolationClass::kSharedProcess;
  fast.locality.class_to_payload = LocalityClass::kSameNumaNode;
  if (!add_synthetic(runtime, std::move(fast))) return 1;

  // The unproven domain: a synthetic engine whose publisher never proved any operation
  // support, so its capability record is not authoritative.
  SyntheticDomainConfig unproven = make_synthetic_domain(
      ExecutionDomainId(3), ExecutionDomainType::kDpu, "synthetic.dpu.unproven", false);
  unproven.capability.operations.clear();
  unproven.capability.flags = 0;
  if (!add_synthetic(runtime, std::move(unproven))) return 1;

  // The only domain that proves the required isolation. Its locality is deliberately poor,
  // so the winner is selected because it is eligible, not because it is the best.
  SyntheticDomainConfig proven = make_synthetic_domain(
      ExecutionDomainId(4), ExecutionDomainType::kDpu, "synthetic.dpu.proven");
  proven.locality.class_to_payload = LocalityClass::kRemoteHost;
  if (!add_synthetic(runtime, std::move(proven))) return 1;

  const std::vector<ExecutionDomainRecord> domains = runtime.scheduler->domains().all();
  std::cout << "policy: minimum_isolation=" << to_string(policy.minimum_isolation)
            << " require_positive_evidence=" << (policy.require_positive_evidence ? "yes" : "no")
            << "\npreference_order:";
  for (ExecutionDomainType type : policy.preference_order) {
    std::cout << " " << to_string(type);
  }
  std::cout << "\ndomains:\n";
  for (const ExecutionDomainRecord& domain : domains) {
    std::cout << "  " << domain.name << " id=" << format_id(domain.id.value())
              << " type=" << to_string(domain.type)
              << " provenance=" << to_string(domain.provenance)
              << " isolation=" << to_string(domain.isolation)
              << " locality=" << to_string(domain.locality.class_to_payload)
              << " capability_authoritative=" << (domain.capability.authoritative ? "yes" : "no")
              << " operations=" << domain.capability.capability.operations.size() << "\n";
  }
  std::cout << "\n";

  OperationRequest request;
  request.operation_class = opclass::checksum_crc32c();
  request.payload.size_bytes = 16384;
  request.payload.source_memory = MemoryDomain::kHost;
  request.payload.destination_memory = MemoryDomain::kHost;
  request.payload.transport_class = TransportClass::kRawFrames;
  request.payload.payload_class = PayloadClass::kOpaqueBytes;
  request.payload.alignment_bytes = 8;
  request.payload.segment_count = 1;

  const PlanResult result = runtime.scheduler->plan(request);
  const DecisionExplanation& explanation = result.explanation;
  std::cout << "plan: " << (result.planned ? "planned" : "rejected")
            << " status=" << (result.status.ok ? std::string("ok") : result.status.code) << "\n";
  std::cout << "outcome: " << to_string(explanation.outcome)
            << " evaluated=" << explanation.evaluated_candidates
            << " rejected=" << explanation.rejected_candidates << "\n\n";

  std::cout << "candidate rejections:\n";
  for (const CandidateEvaluation& candidate : explanation.candidates) {
    std::cout << "  " << domain_name(domains, candidate.domain)
              << " id=" << format_id(candidate.domain.value())
              << " type=" << to_string(candidate.domain_type)
              << " provenance=" << to_string(candidate.provenance) << "\n";
    std::cout << "    eligible=" << (candidate.eligible ? "yes" : "no")
              << " reason=" << to_string(candidate.reason) << "\n";
    std::cout << "    detail: " << candidate.detail << "\n";
  }

  std::cout << "\nranking (eligible candidates only):\n";
  for (const RankedCandidate& ranked : explanation.ranking) {
    std::cout << "  rank " << ranked.rank << " "
              << domain_name(domains, ExecutionDomainId(ranked.domain_id))
              << " id=" << format_id(ranked.domain_id)
              << " type=" << to_string(static_cast<ExecutionDomainType>(ranked.domain_type))
              << " score=" << ranked.weighted_score << "\n";
  }

  bool rejected_ranked = false;
  for (const CandidateEvaluation& candidate : explanation.candidates) {
    if (candidate.eligible) continue;
    for (const RankedCandidate& ranked : explanation.ranking) {
      if (ranked.domain_id == candidate.domain.value()) rejected_ranked = true;
    }
  }
  if (!result.planned || rejected_ranked) {
    std::cerr << "an ineligible domain was selected or ranked" << std::endl;
    return 1;
  }
  std::cout << "\ndecision: the fast SmartNIC (first in the preference order) and the unproven "
               "DPU are absent from the ranking; the runtime selected "
            << domain_name(domains, explanation.selected_domain)
            << ", the only domain that proves the required isolation, and every rejected "
               "candidate carries a structured reason.\n";

  const Status stopped = runtime.scheduler->shutdown();
  if (!stopped) {
    std::cerr << "scheduler shutdown failed: " << stopped.code << std::endl;
    return 1;
  }
  return 0;
}
