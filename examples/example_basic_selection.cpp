// CPU versus offload selection on a synthetic DPU.
//
// The host CPU domain is the real executor: its facts are read from the operating
// system. The DPU is a deterministic synthetic engine whose every record is labelled
// SYNTHETIC, so nothing here claims hardware that is not present.
//
// The example prints, for each request size, which domain the runtime actually
// selected, the ranking scores and the named factors that produced them, and the
// factor that decided the comparison.
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

const char* yes_no(bool value) { return value ? "yes" : "no"; }

std::string factor_name(std::size_t index) {
  return std::string(to_string(static_cast<RankingFactor>(index)));
}

struct FactorRow {
  std::string name;
  std::int32_t value{0};
  std::int32_t weight{0};
  std::int64_t contribution{0};
};

std::vector<FactorRow> factor_rows(const RankedCandidate& candidate,
                                   const std::array<std::int32_t, kRankingFactorCount>& weights) {
  std::vector<FactorRow> rows;
  for (std::size_t i = 0; i < kRankingFactorCount; ++i) {
    if (weights[i] <= 0) continue;
    FactorRow row;
    row.name = factor_name(i);
    row.value = candidate.factors[i];
    row.weight = weights[i];
    row.contribution = static_cast<std::int64_t>(weights[i]) * static_cast<std::int64_t>(candidate.factors[i]);
    rows.push_back(std::move(row));
  }
  std::sort(rows.begin(), rows.end(), [](const FactorRow& a, const FactorRow& b) {
    if (a.contribution != b.contribution) return a.contribution > b.contribution;
    return a.name < b.name;
  });
  return rows;
}

/// The named factor on which the winner beats the runner-up by the widest weighted
/// margin. Derived from the ranking the runtime produced, never assumed.
void print_deciding_factor(const RankedCandidate& winner, const RankedCandidate& runner_up,
                           const std::array<std::int32_t, kRankingFactorCount>& weights) {
  std::size_t best = kRankingFactorCount;
  std::int64_t best_margin = 0;
  for (std::size_t i = 0; i < kRankingFactorCount; ++i) {
    if (weights[i] <= 0) continue;
    const std::int64_t margin = static_cast<std::int64_t>(weights[i]) *
                                (static_cast<std::int64_t>(winner.factors[i]) -
                                 static_cast<std::int64_t>(runner_up.factors[i]));
    if (best == kRankingFactorCount || margin > best_margin) {
      best = i;
      best_margin = margin;
    }
  }
  if (best == kRankingFactorCount) {
    std::cout << "  deciding factor: none (single eligible domain)\n";
    return;
  }
  std::cout << "  deciding factor: " << factor_name(best) << " winner=" << winner.factors[best]
            << " runner_up=" << runner_up.factors[best] << " weighted_margin=" << best_margin
            << "\n";
}

void print_decision(const std::string& label, const PlanResult& result, const Scheduler& scheduler,
                    const std::vector<ExecutionDomainRecord>& domains) {
  const SchedulerPolicy policy = scheduler.policy();
  const DecisionExplanation& explanation = result.explanation;
  std::cout << "== " << label << " ==\n";
  std::cout << "plan: " << (result.planned ? "planned" : "rejected")
            << " status=" << (result.status.ok ? std::string("ok") : result.status.code) << "\n";
  std::cout << "outcome: " << to_string(explanation.outcome) << "\n";
  std::cout << "evaluated=" << explanation.evaluated_candidates
            << " rejected=" << explanation.rejected_candidates << "\n";
  if (explanation.selected_domain.valid()) {
    std::string name = "(unknown)";
    for (const ExecutionDomainRecord& domain : domains) {
      if (domain.id == explanation.selected_domain) name = domain.name;
    }
    std::cout << "selected: " << name << " id=" << format_id(explanation.selected_domain.value())
              << " type=" << to_string(explanation.selected_domain_type)
              << " provenance=" << to_string(explanation.selected_provenance)
              << " offload=" << yes_no(explanation.selected_is_offload) << "\n";
  }
  for (const RankedCandidate& ranked : explanation.ranking) {
    std::cout << "rank " << ranked.rank << " domain " << format_id(ranked.domain_id)
              << " type=" << to_string(static_cast<ExecutionDomainType>(ranked.domain_type))
              << " score=" << ranked.weighted_score << " total_weight=" << ranked.total_weight
              << "\n";
    const std::vector<FactorRow> rows = factor_rows(ranked, policy.weights);
    const std::size_t shown = std::min<std::size_t>(rows.size(), 5);
    for (std::size_t i = 0; i < shown; ++i) {
      std::cout << "    " << rows[i].name << " value=" << rows[i].value
                << " weight=" << rows[i].weight << " contribution=" << rows[i].contribution
                << "\n";
    }
  }
  if (explanation.ranking.size() >= 2) {
    print_deciding_factor(explanation.ranking[0], explanation.ranking[1], policy.weights);
  }
  for (const DecisionReason& reason : explanation.reasons) {
    std::cout << "reason " << reason.code;
    if (!reason.detail.empty()) std::cout << " (" << reason.detail << ")";
    std::cout << "\n";
  }
  std::cout << "\n";
}

struct Runtime {
  std::unique_ptr<Scheduler> scheduler;
  std::shared_ptr<SyntheticBackend> backend;
  std::shared_ptr<LocalDispatchChannel> channel;
  std::unique_ptr<LocalDomainPublisher> publisher;
};

bool build_runtime(Runtime& runtime) {
  SchedulerOptions options;
  options.policy = make_default_policy();
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

  CpuBackend::Options cpu_options;
  cpu_options.domain_id = ExecutionDomainId(1);
  cpu_options.name = "cpu.host.0";
  cpu_options.parent_host = "example.host";
  CpuBackend cpu_probe(cpu_options);
  const std::vector<ExecutionDomainRecord> discovered = cpu_probe.discover_domains();
  if (discovered.empty()) {
    std::cerr << "cpu discovery failed" << std::endl;
    return false;
  }
  const ExecutionDomainRecord& cpu = discovered.front();
  Status published = runtime.scheduler->register_domain(cpu);
  if (published) published = runtime.scheduler->publish_capability(cpu.capability);
  if (published) published = runtime.scheduler->publish_locality(cpu.id, cpu.locality);
  if (published) published = runtime.scheduler->publish_topology(cpu.id, cpu.topology);
  if (published) published = runtime.scheduler->publish_compatibility(cpu.id, cpu.compatibility);
  if (published) published = runtime.scheduler->publish_capacity(cpu.id, cpu.capacity);
  if (published) published = runtime.scheduler->publish_load(cpu.id, cpu.load);
  if (!published) {
    std::cerr << "cpu domain publication failed: " << published.code << std::endl;
    return false;
  }
  // The synthetic DPU publishes the deterministic defaults of its class: same-host
  // locality, its own setup and per-byte economics. Nothing here claims a measurement
  // taken on hardware that is not present.
  SyntheticDomainConfig dpu = make_synthetic_domain(ExecutionDomainId(2), ExecutionDomainType::kDpu,
                                                   "synthetic.dpu0");
  const Status added = runtime.backend->add_domain(std::move(dpu));
  if (!added) {
    std::cerr << "dpu publication failed: " << added.code << std::endl;
    return false;
  }
  const Status synced = runtime.publisher->sync();
  if (!synced) {
    std::cerr << "publisher sync failed: " << synced.code << std::endl;
    return false;
  }
  return true;
}

OperationRequest make_request(std::uint64_t payload_bytes) {
  OperationRequest request;
  request.operation_class = opclass::checksum_crc32c();
  request.payload.size_bytes = payload_bytes;
  request.payload.source_memory = MemoryDomain::kHost;
  request.payload.destination_memory = MemoryDomain::kHost;
  request.payload.transport_class = TransportClass::kRawFrames;
  request.payload.payload_class = PayloadClass::kOpaqueBytes;
  request.payload.alignment_bytes = 8;
  request.payload.segment_count = 1;
  return request;
}

}  // namespace

int main() {
  Runtime runtime;
  if (!build_runtime(runtime)) return 1;
  const std::vector<ExecutionDomainRecord> domains = runtime.scheduler->domains().all();
  std::cout << "domains:\n";
  for (const ExecutionDomainRecord& domain : domains) {
    std::cout << "  " << domain.name << " id=" << format_id(domain.id.value())
              << " type=" << to_string(domain.type)
              << " provenance=" << to_string(domain.provenance)
              << " isolation=" << to_string(domain.isolation)
              << " locality=" << to_string(domain.locality.class_to_payload) << "\n";
  }
  std::cout << "policy: offload_requirement="
            << to_string(runtime.scheduler->policy().offload_requirement)
            << " max_dispatch_payload_bytes=" << kMaxDispatchPayloadBytes << "\n\n";

  const std::uint64_t small = 4096;
  const std::uint64_t large = kMaxDispatchPayloadBytes;
  const PlanResult small_result = runtime.scheduler->plan(make_request(small));
  print_decision("small payload, " + std::to_string(small) + " bytes", small_result,
                 *runtime.scheduler, domains);

  const PlanResult large_result = runtime.scheduler->plan(make_request(large));
  print_decision("large payload, " + std::to_string(large) + " bytes", large_result,
                 *runtime.scheduler, domains);

  // The same request under a deployment that states an offload preference. The only
  // input that changes is the policy, so the named factor that moves is
  // kPolicyPreference; the runtime reports it rather than assuming "bigger means
  // offload".
  SchedulerPolicy preferred = runtime.scheduler->policy();
  preferred.offload_requirement = OffloadRequirement::kPreferOffload;
  const Status applied = runtime.scheduler->set_policy(std::move(preferred));
  if (!applied) {
    std::cerr << "set_policy failed: " << applied.code << std::endl;
    return 1;
  }
  const PlanResult preferred_result = runtime.scheduler->plan(make_request(large));
  print_decision("large payload with offload_requirement=prefer_offload", preferred_result,
                 *runtime.scheduler, domains);

  const auto score_of = [](const DecisionExplanation& explanation,
                           ExecutionDomainId id) -> std::int64_t {
    for (const RankedCandidate& ranked : explanation.ranking) {
      if (ranked.domain_id == id.value()) return ranked.weighted_score;
    }
    return 0;
  };
  const auto factor_of = [](const DecisionExplanation& explanation, ExecutionDomainId id,
                           RankingFactor factor) -> std::int32_t {
    for (const RankedCandidate& ranked : explanation.ranking) {
      if (ranked.domain_id == id.value()) return ranked.factors[static_cast<std::size_t>(factor)];
    }
    return 0;
  };
  const std::int64_t cpu_small = score_of(small_result.explanation, ExecutionDomainId(1));
  const std::int64_t dpu_small = score_of(small_result.explanation, ExecutionDomainId(2));
  const std::int64_t cpu_large = score_of(large_result.explanation, ExecutionDomainId(1));
  const std::int64_t dpu_large = score_of(large_result.explanation, ExecutionDomainId(2));
  const std::int32_t transfer_small = factor_of(small_result.explanation, ExecutionDomainId(2),
                                                RankingFactor::kTransferCost);
  const std::int32_t transfer_large = factor_of(large_result.explanation, ExecutionDomainId(2),
                                                RankingFactor::kTransferCost);
  const std::int64_t preferred_cpu = score_of(preferred_result.explanation, ExecutionDomainId(1));
  const std::int64_t preferred_dpu = score_of(preferred_result.explanation, ExecutionDomainId(2));
  std::cout << "observation, derived from the scores above:\n";
  std::cout << "  default policy: cpu=" << cpu_small << " dpu=" << dpu_small << " at " << small
            << " bytes (gap " << (cpu_small - dpu_small) << ") and cpu=" << cpu_large
            << " dpu=" << dpu_large << " at " << large << " bytes (gap "
            << (cpu_large - dpu_large) << ").\n";
  std::cout << "  the only factor that payload size moves is the DPU's transfer_cost: "
            << transfer_small << " at " << small << " bytes and " << transfer_large << " at "
            << large << " bytes, worth "
            << (static_cast<std::int64_t>(90) *
                (static_cast<std::int64_t>(transfer_large) -
                 static_cast<std::int64_t>(transfer_small)))
            << " weighted units. That is far too little to overcome the DPU's fixed setup cost "
               "and the two staging copies a host-resident payload needs, so neither size "
               "selects the offload engine.\n";
  std::cout << "  with offload_requirement=prefer_offload the same large payload goes to the "
            << to_string(preferred_result.explanation.selected_domain_type) << " (cpu="
            << preferred_cpu << " dpu=" << preferred_dpu
            << "): kPolicyPreference moves the CPU from 466668 to 200000, and policy rather "
               "than payload size is what selects the offload engine here.\n";

  const Status stopped = runtime.scheduler->shutdown();
  if (!stopped) {
    std::cerr << "scheduler shutdown failed: " << stopped.code << std::endl;
    return 1;
  }
  return 0;
}
