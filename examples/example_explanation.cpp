// Deterministic decision explanation.
//
// Real decisions must be reproducible: the same state and the same request produce the
// same structured explanation, byte for byte. This example evaluates one state twice,
// compares the rendered JSON, and then changes locality evidence and load evidence and
// reports the named factor whose weighted contribution moved the decision.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "tos/tos.hpp"

using namespace tos;

namespace {

const ExecutionDomainId kCpuDomain(1);
const ExecutionDomainId kDpuDomain(2);

struct Runtime {
  std::unique_ptr<Scheduler> scheduler;
  std::shared_ptr<SyntheticBackend> backend;
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
  runtime.publisher = std::make_unique<LocalDomainPublisher>(*runtime.scheduler, runtime.backend);

  CpuBackend::Options cpu_options;
  cpu_options.domain_id = kCpuDomain;
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
    std::cerr << "cpu publication failed: " << published.code << std::endl;
    return false;
  }

  SyntheticDomainConfig dpu = make_synthetic_domain(kDpuDomain, ExecutionDomainType::kDpu,
                                                   "synthetic.dpu0");
  dpu.locality.class_to_payload = LocalityClass::kSameNumaNode;
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

OperationRequest make_request(std::uint64_t payload_bytes, std::uint64_t operation_id) {
  OperationRequest request;
  request.operation_class = opclass::checksum_crc32c();
  request.payload.size_bytes = payload_bytes;
  request.payload.source_memory = MemoryDomain::kHost;
  request.payload.destination_memory = MemoryDomain::kHost;
  request.payload.transport_class = TransportClass::kRawFrames;
  request.payload.payload_class = PayloadClass::kOpaqueBytes;
  request.payload.alignment_bytes = 8;
  request.payload.segment_count = 1;
  // A fixed operation identity keeps the rendered explanation free of incidental
  // identity allocation, so two evaluations of one state are comparable byte for byte.
  request.operation_id = TransportOperationId(operation_id);
  return request;
}

const RankedCandidate* find_ranked(const DecisionExplanation& explanation, ExecutionDomainId id) {
  for (const RankedCandidate& ranked : explanation.ranking) {
    if (ranked.domain_id == id.value()) return &ranked;
  }
  return nullptr;
}

std::string winner_text(const DecisionExplanation& explanation) {
  return std::string(to_string(explanation.selected_domain_type)) + " " +
         format_id(explanation.selected_domain.value());
}

struct FactorChange {
  std::string name;
  std::int32_t before{0};
  std::int32_t after{0};
  std::int64_t weighted_change{0};
};

std::vector<FactorChange> factor_changes(const RankedCandidate& before,
                                         const RankedCandidate& after,
                                         const std::array<std::int32_t, kRankingFactorCount>& weights) {
  std::vector<FactorChange> changes;
  for (std::size_t i = 0; i < kRankingFactorCount; ++i) {
    if (weights[i] <= 0) continue;
    if (before.factors[i] == after.factors[i]) continue;
    FactorChange change;
    change.name = std::string(to_string(static_cast<RankingFactor>(i)));
    change.before = before.factors[i];
    change.after = after.factors[i];
    change.weighted_change = static_cast<std::int64_t>(weights[i]) *
                             (static_cast<std::int64_t>(after.factors[i]) -
                              static_cast<std::int64_t>(before.factors[i]));
    changes.push_back(std::move(change));
  }
  std::sort(changes.begin(), changes.end(), [](const FactorChange& a, const FactorChange& b) {
    const std::int64_t left = a.weighted_change < 0 ? -a.weighted_change : a.weighted_change;
    const std::int64_t right = b.weighted_change < 0 ? -b.weighted_change : b.weighted_change;
    if (left != right) return left > right;
    return a.name < b.name;
  });
  return changes;
}

void report_change(const std::string& label, const DecisionExplanation& before,
                   const DecisionExplanation& after,
                   const std::array<std::int32_t, kRankingFactorCount>& weights) {
  std::cout << "-- " << label << "\n";
  std::cout << "selection before: " << winner_text(before) << " score="
            << (find_ranked(before, before.selected_domain) == nullptr
                    ? 0
                    : find_ranked(before, before.selected_domain)->weighted_score)
            << "\n";
  std::cout << "selection after:  " << winner_text(after) << " score="
            << (find_ranked(after, after.selected_domain) == nullptr
                    ? 0
                    : find_ranked(after, after.selected_domain)->weighted_score)
            << "\n";
  const RankedCandidate* dpu_before = find_ranked(before, kDpuDomain);
  const RankedCandidate* dpu_after = find_ranked(after, kDpuDomain);
  if (dpu_before == nullptr || dpu_after == nullptr) {
    std::cout << "  the DPU is not ranked in both states\n";
    return;
  }
  const std::vector<FactorChange> changes = factor_changes(*dpu_before, *dpu_after, weights);
  if (changes.empty()) {
    std::cout << "  no DPU factor changed\n";
    return;
  }
  std::cout << "  named factor that moved the decision: " << changes.front().name
            << " before=" << changes.front().before << " after=" << changes.front().after
            << " weighted_change=" << changes.front().weighted_change << "\n";
  const std::size_t shown = std::min<std::size_t>(changes.size(), 4);
  for (std::size_t i = 1; i < shown; ++i) {
    std::cout << "  also changed: " << changes[i].name << " before=" << changes[i].before
              << " after=" << changes[i].after
              << " weighted_change=" << changes[i].weighted_change << "\n";
  }
  if (before.selected_domain == after.selected_domain) {
    std::cout << "  selection did not change\n";
  } else {
    std::cout << "  selection changed from " << to_string(before.selected_domain_type) << " to "
              << to_string(after.selected_domain_type) << "\n";
  }
}

}  // namespace

int main() {
  Runtime runtime;
  if (!build_runtime(runtime)) return 1;
  Scheduler& scheduler = *runtime.scheduler;
  const std::array<std::int32_t, kRankingFactorCount> weights = scheduler.policy().weights;

  const OperationRequest request = make_request(65536, 0x1000);
  const PlanResult first = scheduler.plan(request);
  const PlanResult second = scheduler.plan(request);
  if (!first.planned || !second.planned) {
    std::cerr << "planning failed: " << first.status.code << std::endl;
    return 1;
  }
  const std::string first_json = render_explanation_json(first.explanation);
  const std::string second_json = render_explanation_json(second.explanation);
  std::cout << "evaluation 1 json:\n" << first_json << "\n\n";
  std::cout << "evaluation 2 json:\n" << second_json << "\n\n";
  std::cout << "byte_identical=" << (first_json == second_json ? "yes" : "no")
            << " bytes=" << first_json.size() << "\n";
  if (first_json != second_json) {
    std::cerr << "identical state produced different explanations" << std::endl;
    return 1;
  }
  std::cout << "baseline selection: " << winner_text(first.explanation) << " outcome="
            << to_string(first.explanation.outcome) << "\n\n";

  // Locality change: the same engine is now reported on a remote host, so the payload
  // must cross the host boundary to reach it.
  DomainLocality remote;
  remote.generation = LocalityGeneration(2);
  remote.class_to_payload = LocalityClass::kRemoteHost;
  remote.has_local_nic = false;
  const Status locality_set = runtime.backend->set_locality(kDpuDomain, remote);
  const Status locality_sync =
      locality_set ? runtime.publisher->sync() : Status::failure("skipped");
  if (!locality_set || !locality_sync) {
    std::cerr << "locality publication failed: " << locality_set.code << " "
              << locality_sync.code << std::endl;
    return 1;
  }
  const PlanResult after_locality = scheduler.plan(make_request(65536, 0x2000));
  if (!after_locality.planned) {
    std::cerr << "planning after the locality change failed" << std::endl;
    return 1;
  }
  report_change("locality evidence changed to remote_host", first.explanation,
                after_locality.explanation, weights);

  // Load change: locality is restored, and the engine now reports high utilization,
  // congestion and queue depth.
  DomainLocality local;
  local.generation = LocalityGeneration(3);
  local.class_to_payload = LocalityClass::kSameNumaNode;
  local.has_local_nic = true;
  const Status restored = runtime.backend->set_locality(kDpuDomain, local);
  const Status loaded = runtime.backend->set_load(kDpuDomain, 90, 80, 200, 8, 5000);
  const Status synced = (restored && loaded) ? runtime.publisher->sync()
                                             : Status::failure("skipped");
  if (!restored || !loaded || !synced) {
    std::cerr << "load publication failed: " << restored.code << " " << loaded.code << " "
              << synced.code << std::endl;
    return 1;
  }
  const PlanResult after_load = scheduler.plan(make_request(65536, 0x3000));
  if (!after_load.planned) {
    std::cerr << "planning after the load change failed" << std::endl;
    return 1;
  }
  report_change("load evidence changed to utilization 90 congestion 80 queue 200/256",
                first.explanation, after_load.explanation, weights);
  std::cout << "\nchanged-state json:\n" << render_explanation_json(after_load.explanation)
            << "\n";
  if (after_locality.explanation.selected_domain == first.explanation.selected_domain &&
      after_load.explanation.selected_domain == first.explanation.selected_domain) {
    std::cerr << "evidence changes never moved the selection" << std::endl;
    return 1;
  }

  const Status stopped = scheduler.shutdown();
  if (!stopped) {
    std::cerr << "scheduler shutdown failed: " << stopped.code << std::endl;
    return 1;
  }
  return 0;
}
