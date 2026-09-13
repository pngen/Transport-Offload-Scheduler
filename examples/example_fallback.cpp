// Fallback after the preferred offload domain is fenced between planning and dispatch.
//
// The plan of record is bound to the exact authority that made it legal. Fencing the
// DPU after planning invalidates that binding, so dispatch is refused with
// DispatchRejection::kAuthorityStale rather than silently executing somewhere else.
// The legal next step is an explicit fallback: a new plan of record with a fresh
// attempt generation. Finally the deployment states that host execution is not legal
// at all, and the runtime rejects the request instead of quietly using the CPU.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "tos/tos.hpp"

using namespace tos;

namespace {

/// Delivers completions through the normal authority path and lets the example wait
/// for a specific attempt without sleeping and without a timeout.
class CompletionTracker {
 public:
  void bind(Scheduler* scheduler) { scheduler_ = scheduler; }

  void operator()(const CompletionSubmission& submission) {
    const CompletionOutcome outcome = scheduler_->complete(submission);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      outcomes_.push_back(outcome);
      if (outcome.committed && !outcome.idempotent) ++commits_;
      if (!outcome.committed) ++rejections_;
    }
    condition_.notify_all();
  }

  CompletionOutcome wait_for(ExecutionAttemptId attempt) {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [this, attempt] {
      for (const CompletionOutcome& outcome : outcomes_) {
        if (outcome.attempt.id == attempt) return true;
      }
      return false;
    });
    for (auto entry = outcomes_.rbegin(); entry != outcomes_.rend(); ++entry) {
      if (entry->attempt.id == attempt) return *entry;
    }
    return CompletionOutcome{};
  }

  std::size_t commits() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return commits_;
  }

  std::size_t rejections() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return rejections_;
  }

 private:
  Scheduler* scheduler_{nullptr};
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::vector<CompletionOutcome> outcomes_;
  std::size_t commits_{0};
  std::size_t rejections_{0};
};

/// A deployment with two engines routes each dispatch to the backend that owns the
/// selected domain. The host CPU is executed by the real CPU backend; the synthetic DPU
/// by the deterministic synthetic backend.
class RoutingChannel : public IDispatchChannel {
 public:
  void route(ExecutionDomainId domain, std::shared_ptr<IDispatchChannel> channel) {
    routes_.emplace_back(domain, std::move(channel));
  }

  [[nodiscard]] std::string_view name() const noexcept override { return "example.routing"; }
  [[nodiscard]] Status send(const DispatchEnvelope& envelope) override {
    for (const auto& route : routes_) {
      if (route.first == envelope.domain) return route.second->send(envelope);
    }
    return Status::failure("example.no_route", "no dispatch channel owns this domain");
  }
  bool request_cancel(const CancelEnvelope& envelope) override {
    for (const auto& route : routes_) {
      if (route.first == envelope.domain) return route.second->request_cancel(envelope);
    }
    return false;
  }
  void shutdown() override {
    for (const auto& route : routes_) route.second->shutdown();
  }

 private:
  std::vector<std::pair<ExecutionDomainId, std::shared_ptr<IDispatchChannel>>> routes_;
};

struct Runtime {
  std::unique_ptr<Scheduler> scheduler;
  std::shared_ptr<SyntheticBackend> backend;
  std::shared_ptr<CpuBackend> cpu_backend;
  std::shared_ptr<RoutingChannel> channel;
  std::unique_ptr<LocalDomainPublisher> publisher;
  CompletionTracker tracker;
};

bool build_runtime(Runtime& runtime) {
  SchedulerPolicy policy = make_default_policy();
  // The deployment prefers offload execution; the host CPU stays legal as a fallback.
  policy.offload_requirement = OffloadRequirement::kPreferOffload;
  SchedulerOptions options;
  options.policy = std::move(policy);
  options.host_node = "example.host";
  runtime.scheduler = std::make_unique<Scheduler>(std::move(options));
  const Status started = runtime.scheduler->start();
  if (!started) {
    std::cerr << "scheduler start failed: " << started.code << std::endl;
    return false;
  }
  runtime.backend = std::make_shared<SyntheticBackend>("example.synthetic");
  runtime.publisher = std::make_unique<LocalDomainPublisher>(*runtime.scheduler, runtime.backend);
  runtime.tracker.bind(runtime.scheduler.get());
  CompletionTracker* tracker = &runtime.tracker;
  const LocalDispatchChannel::CompletionSink sink =
      [tracker](const CompletionSubmission& submission) { (*tracker)(submission); };

  CpuBackend::Options cpu_options;
  cpu_options.domain_id = ExecutionDomainId(1);
  cpu_options.name = "cpu.host.0";
  cpu_options.parent_host = "example.host";
  runtime.cpu_backend = std::make_shared<CpuBackend>(cpu_options);
  const std::vector<ExecutionDomainRecord> discovered = runtime.cpu_backend->discover_domains();
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

  SyntheticDomainConfig dpu = make_synthetic_domain(ExecutionDomainId(2), ExecutionDomainType::kDpu,
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

  runtime.channel = std::make_shared<RoutingChannel>();
  runtime.channel->route(cpu.id, std::make_shared<LocalDispatchChannel>(runtime.cpu_backend, sink,
                                                                       LocalChannelOptions{}));
  runtime.channel->route(ExecutionDomainId(2), std::make_shared<LocalDispatchChannel>(
                                                   runtime.backend, sink, LocalChannelOptions{}));
  runtime.scheduler->set_dispatch_channel(runtime.channel);
  return true;
}

void print_ranking(const DecisionExplanation& explanation) {
  std::cout << "ranking:";
  for (const RankedCandidate& ranked : explanation.ranking) {
    std::cout << "\n  rank " << ranked.rank << " domain " << format_id(ranked.domain_id)
              << " type=" << to_string(static_cast<ExecutionDomainType>(ranked.domain_type))
              << " score=" << ranked.weighted_score;
  }
  std::cout << "\n";
}

void print_reasons(const DecisionExplanation& explanation) {
  std::cout << "reasons:";
  for (const DecisionReason& reason : explanation.reasons) {
    std::cout << "\n  " << reason.code;
    if (!reason.detail.empty()) std::cout << " (" << reason.detail << ")";
  }
  std::cout << "\n";
}

std::vector<std::uint8_t> make_payload(std::size_t size, std::uint32_t seed) {
  std::vector<std::uint8_t> payload(size);
  std::uint32_t state = seed * 2654435761U + 1U;
  for (std::size_t i = 0; i < size; ++i) {
    state = state * 1664525U + 1013904223U;
    payload[i] = static_cast<std::uint8_t>((state >> 16) & 0x3FU);
  }
  return payload;
}

}  // namespace

int main() {
  Runtime runtime;
  if (!build_runtime(runtime)) return 1;
  Scheduler& scheduler = *runtime.scheduler;

  const std::vector<std::uint8_t> payload = make_payload(8192, 7);
  OperationRequest request;
  request.operation_class = opclass::checksum_crc32c();
  request.payload.size_bytes = payload.size();
  request.payload.source_memory = MemoryDomain::kHost;
  request.payload.destination_memory = MemoryDomain::kHost;
  request.payload.transport_class = TransportClass::kRawFrames;
  request.payload.payload_class = PayloadClass::kOpaqueBytes;
  request.payload.alignment_bytes = 8;
  request.payload.segment_count = 1;

  std::cout << "step 1: plan with offload_requirement="
            << to_string(scheduler.policy().offload_requirement) << "\n";
  PlanResult planned = scheduler.plan(request, payload);
  if (!planned.planned) {
    std::cerr << "planning failed: " << planned.status.code << std::endl;
    return 1;
  }
  const ExecutionPlan original = planned.plan;
  std::cout << "  planned domain=" << format_id(original.domain.value())
            << " type=" << to_string(original.domain_type)
            << " attempt=" << format_id(original.attempt.value())
            << " attempt_generation=" << original.attempt_generation.value() << "\n";
  print_ranking(planned.explanation);

  std::cout << "\nstep 2: fence the planned domain between planning and dispatch\n";
  const Status fenced = scheduler.fence_domain(
      original.domain, "operator fence between planning and dispatch");
  if (!fenced) {
    std::cerr << "fence failed: " << fenced.code << std::endl;
    return 1;
  }
  ExecutionDomainRecord fenced_record;
  if (scheduler.domains().get(original.domain, fenced_record)) {
    std::cout << "  domain fenced=" << (fenced_record.fenced ? "yes" : "no")
              << " generation=" << fenced_record.generation.value()
              << " reason=" << fenced_record.fence_reason << "\n";
  }

  std::cout << "\nstep 3: dispatch the stale plan\n";
  ExecutionPlan dispatched_plan = original;
  const DispatchResult dispatched = scheduler.dispatch(dispatched_plan);
  std::cout << "  dispatched=" << (dispatched.dispatched ? "yes" : "no")
            << " rejection=" << to_string(dispatched.rejection)
            << " code=" << dispatched.status.code
            << " detail=" << dispatched.status.message << "\n";
  if (dispatched.dispatched || dispatched.rejection != DispatchRejection::kAuthorityStale) {
    std::cerr << "expected DispatchRejection::kAuthorityStale" << std::endl;
    return 1;
  }
  std::cout << "  plan state=" << to_string(dispatched_plan.state) << "\n";

  std::cout << "\nstep 4: request the legal fallback\n";
  const FallbackResult fallback = scheduler.fallback(original, FailureKind::kDomainUnavailable);
  if (!fallback.fallback_selected) {
    std::cerr << "fallback refused: " << fallback.status.code << " "
              << to_string(fallback.rejection) << std::endl;
    return 1;
  }
  std::cout << "  fallback_selected=yes rejection=" << to_string(fallback.rejection)
            << " depth=" << fallback.plan.fallback_depth
            << " from=" << format_id(fallback.plan.fallback_from.value()) << "\n";
  std::cout << "  fallback domain=" << format_id(fallback.plan.domain.value())
            << " type=" << to_string(fallback.plan.domain_type)
            << " attempt=" << format_id(fallback.plan.attempt.value())
            << " attempt_generation=" << fallback.plan.attempt_generation.value()
            << " (original generation " << original.attempt_generation.value() << ")\n";
  std::cout << "  chain tried:";
  for (ExecutionDomainType type : fallback.chain_tried) std::cout << " " << to_string(type);
  std::cout << "\n";
  print_reasons(fallback.explanation);

  std::cout << "\nstep 5: reserve and dispatch the fallback plan\n";
  ExecutionPlan fallback_plan = fallback.plan;
  const Status reserved = scheduler.reserve(fallback_plan);
  if (!reserved) {
    std::cerr << "reserve failed: " << reserved.code << std::endl;
    return 1;
  }
  const DispatchResult fallback_dispatch = scheduler.dispatch(fallback_plan);
  std::cout << "  dispatched=" << (fallback_dispatch.dispatched ? "yes" : "no")
            << " rejection=" << to_string(fallback_dispatch.rejection)
            << " code=" << fallback_dispatch.status.code << "\n";
  if (!fallback_dispatch.dispatched) return 1;
  const CompletionOutcome completion = runtime.tracker.wait_for(fallback_plan.attempt);
  std::cout << "  completion committed=" << (completion.committed ? "yes" : "no")
            << " rejection=" << to_string(completion.rejection)
            << " state=" << to_string(completion.attempt.state)
            << " digest=" << completion.attempt.result.result_digest
            << " expected_digest="
            << static_cast<std::uint64_t>(crc32c(payload.data(), payload.size())) << "\n";

  std::cout << "\nstep 6: the deployment forbids host execution entirely\n";
  SchedulerPolicy required = scheduler.policy();
  required.offload_requirement = OffloadRequirement::kOffloadRequired;
  const Status applied = scheduler.set_policy(std::move(required));
  if (!applied) {
    std::cerr << "set_policy failed: " << applied.code << std::endl;
    return 1;
  }
  const PlanResult rejected = scheduler.plan(request, payload);
  std::cout << "  plan=" << (rejected.planned ? "planned" : "rejected")
            << " outcome=" << to_string(rejected.explanation.outcome)
            << " status=" << rejected.status.code << "\n";
  for (const CandidateEvaluation& candidate : rejected.explanation.candidates) {
    std::cout << "  candidate domain " << format_id(candidate.domain.value())
              << " type=" << to_string(candidate.domain_type)
              << " eligible=" << (candidate.eligible ? "yes" : "no")
              << " reason=" << to_string(candidate.reason) << "\n";
    std::cout << "    detail: " << candidate.detail << "\n";
  }
  print_reasons(rejected.explanation);
  std::cout << "  the policy refused the request instead of silently using the CPU\n";

  const Status stopped = scheduler.shutdown();
  if (!stopped) {
    std::cerr << "scheduler shutdown failed: " << stopped.code << std::endl;
    return 1;
  }
  return 0;
}
