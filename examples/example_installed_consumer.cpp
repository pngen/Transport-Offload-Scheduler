// A downstream consumer of the INSTALLED Transport Offload Scheduler package.
//
// This translation unit includes exactly one project header, <tos/tos.hpp>, and uses
// only the installed public interface: no internal header, no relative include into the
// source tree, no build-tree path. It is built here from the source tree for
// convenience, but it compiles unchanged against an installed package:
//
//   find_package(TransportOffloadScheduler CONFIG REQUIRED)
//   target_link_libraries(consumer PRIVATE SummonSoftwareLabs::transport_offload_scheduler)
//
// The installed package exports the target under its real name with the project
// namespace. The build tree additionally offers the aliases
// SummonSoftwareLabs::TransportOffloadScheduler and tos::tos, which are not exported.
//
// It performs a real plan -> reserve -> dispatch -> complete cycle against the host CPU
// execution domain and prints the decision the runtime made.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include <tos/tos.hpp>

#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {

/// A minimal downstream completion sink: hand every completion to completion authority
/// and let the caller wait for a specific attempt without sleeping.
class ConsumerSink {
 public:
  void bind(tos::Scheduler* scheduler) { scheduler_ = scheduler; }

  void operator()(const tos::CompletionSubmission& submission) {
    const tos::CompletionOutcome outcome = scheduler_->complete(submission);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      outcomes_.push_back(outcome);
    }
    condition_.notify_all();
  }

  tos::CompletionOutcome wait_for(tos::ExecutionAttemptId attempt) {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [this, attempt] {
      for (const tos::CompletionOutcome& outcome : outcomes_) {
        if (outcome.attempt.id == attempt) return true;
      }
      return false;
    });
    for (auto entry = outcomes_.rbegin(); entry != outcomes_.rend(); ++entry) {
      if (entry->attempt.id == attempt) return *entry;
    }
    return tos::CompletionOutcome{};
  }

 private:
  tos::Scheduler* scheduler_{nullptr};
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::vector<tos::CompletionOutcome> outcomes_;
};

std::vector<std::uint8_t> make_payload(std::size_t size) {
  std::vector<std::uint8_t> payload(size);
  std::uint32_t state = 0x9E3779B9U;
  for (std::size_t i = 0; i < size; ++i) {
    state = state * 1664525U + 1013904223U;
    payload[i] = static_cast<std::uint8_t>((state >> 16) & 0xFFU);
  }
  return payload;
}

}  // namespace

int main() {
  tos::SchedulerOptions options;
  options.policy = tos::make_default_policy();
  options.host_node = "consumer.host";
  tos::Scheduler scheduler(std::move(options));
  const tos::Status started = scheduler.start();
  if (!started) {
    std::cerr << "scheduler start failed: " << started.code << std::endl;
    return 1;
  }

  tos::CpuBackend::Options cpu_options;
  cpu_options.domain_id = tos::ExecutionDomainId(1);
  cpu_options.name = "cpu.host.0";
  cpu_options.parent_host = "consumer.host";
  auto backend = std::make_shared<tos::CpuBackend>(cpu_options);

  ConsumerSink sink;
  sink.bind(&scheduler);
  ConsumerSink* sink_pointer = &sink;
  auto channel = std::make_shared<tos::LocalDispatchChannel>(
      backend, [sink_pointer](const tos::CompletionSubmission& submission) {
        (*sink_pointer)(submission);
      },
      tos::LocalChannelOptions{});
  scheduler.set_dispatch_channel(channel);
  tos::LocalDomainPublisher publisher(scheduler, backend);
  const tos::Status synced = publisher.sync();
  if (!synced) {
    std::cerr << "domain publication failed: " << synced.code << " " << synced.message
              << std::endl;
    return 1;
  }

  const std::vector<std::uint8_t> payload = make_payload(32768);
  tos::OperationRequest request;
  request.operation_class = tos::opclass::checksum_crc32c();
  request.payload.size_bytes = payload.size();
  request.payload.source_memory = tos::MemoryDomain::kHost;
  request.payload.destination_memory = tos::MemoryDomain::kHost;
  request.payload.transport_class = tos::TransportClass::kRawFrames;
  request.payload.payload_class = tos::PayloadClass::kOpaqueBytes;
  request.payload.alignment_bytes = 8;
  request.payload.segment_count = 1;

  tos::PlanResult planned = scheduler.plan(
      request, std::span<const std::uint8_t>(payload.data(), payload.size()));
  if (!planned.planned) {
    std::cerr << "planning failed: " << planned.status.code << " "
              << tos::to_string(planned.explanation.outcome) << std::endl;
    return 1;
  }
  std::cout << "decision: outcome=" << tos::to_string(planned.explanation.outcome)
            << " domain=" << tos::format_id(planned.plan.domain.value())
            << " type=" << tos::to_string(planned.plan.domain_type)
            << " provenance=" << tos::to_string(planned.plan.provenance)
            << " class=" << planned.plan.operation_class_name << "\n";
  std::cout << "ranking:\n";
  for (const tos::RankedCandidate& ranked : planned.explanation.ranking) {
    std::cout << "  rank " << ranked.rank << " domain " << tos::format_id(ranked.domain_id)
              << " type=" << tos::to_string(static_cast<tos::ExecutionDomainType>(ranked.domain_type))
              << " score=" << ranked.weighted_score << "\n";
  }
  std::cout << "explanation: " << tos::render_explanation_json(planned.explanation) << "\n";

  const tos::Status reserved = scheduler.reserve(planned.plan);
  if (!reserved) {
    std::cerr << "reserve failed: " << reserved.code << std::endl;
    return 1;
  }
  const tos::DispatchResult dispatched = scheduler.dispatch(planned.plan);
  if (!dispatched.dispatched) {
    std::cerr << "dispatch failed: " << dispatched.status.code << " "
              << tos::to_string(dispatched.rejection) << std::endl;
    return 1;
  }
  const tos::CompletionOutcome completion = sink.wait_for(planned.plan.attempt);
  const std::uint64_t expected =
      static_cast<std::uint64_t>(tos::crc32c(payload.data(), payload.size()));
  std::cout << "execution: state=" << tos::to_string(completion.attempt.state)
            << " committed=" << (completion.committed ? "yes" : "no")
            << " rejection=" << tos::to_string(completion.rejection)
            << " digest=" << completion.attempt.result.result_digest
            << " expected=" << expected
            << " bytes=" << completion.attempt.result.bytes_processed
            << " duration_ns=" << completion.attempt.result.duration_ns << "\n";
  if (!completion.committed || completion.attempt.result.result_digest != expected) {
    std::cerr << "the completed operation does not match the payload" << std::endl;
    return 1;
  }

  const tos::SchedulerSnapshot snapshot = scheduler.snapshot();
  std::cout << "snapshot: generation=" << snapshot.generation.value()
            << " domains=" << snapshot.totals.domains
            << " plans_created=" << snapshot.totals.plans_created
            << " plans_dispatched=" << snapshot.totals.plans_dispatched
            << " completed_attempts=" << snapshot.totals.completed_attempts
            << " live_attempts=" << snapshot.totals.live_attempts << "\n";

  const tos::Status stopped = scheduler.shutdown();
  if (!stopped) {
    std::cerr << "scheduler shutdown failed: " << stopped.code << std::endl;
    return 1;
  }
  return 0;
}
