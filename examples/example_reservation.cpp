// Reservation lifecycle: plan -> reserve -> revalidate -> dispatch -> complete -> release.
//
// Planning grants no execution authority and consumes no capacity. A reservation holds
// scarce engine capacity and must close its accounting exactly once; the ledger audit
// is printed before, during and after the lifecycle, together with a refused
// over-request that the ledger and the planner both reject.
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

class CompletionTracker {
 public:
  void bind(Scheduler* scheduler) { scheduler_ = scheduler; }

  void operator()(const CompletionSubmission& submission) {
    const CompletionOutcome outcome = scheduler_->complete(submission);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      outcomes_.push_back(outcome);
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

 private:
  Scheduler* scheduler_{nullptr};
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::vector<CompletionOutcome> outcomes_;
};

struct Runtime {
  std::unique_ptr<Scheduler> scheduler;
  std::shared_ptr<SyntheticBackend> backend;
  std::shared_ptr<LocalDispatchChannel> channel;
  std::unique_ptr<LocalDomainPublisher> publisher;
  CompletionTracker tracker;
};

bool build_runtime(Runtime& runtime) {
  SchedulerPolicy policy = make_default_policy();
  policy.reservation.enabled = true;
  policy.reservation.per_operation[static_cast<std::size_t>(ResourceKind::kExecutionSlot)] = 3;
  policy.reservation.require_reservation_for_offload_only = true;
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
  runtime.tracker.bind(runtime.scheduler.get());
  CompletionTracker* tracker = &runtime.tracker;
  runtime.channel = std::make_shared<LocalDispatchChannel>(
      runtime.backend, [tracker](const CompletionSubmission& submission) { (*tracker)(submission); },
      LocalChannelOptions{});
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
  return true;
}

void print_audit(const std::string& label, const ReservationLedger& ledger) {
  const ReservationAudit audit = ledger.audit();
  std::cout << label << ": consistent=" << (audit.consistent ? "yes" : "no")
            << " outstanding=" << audit.outstanding << " committed=" << audit.committed
            << " held_not_dispatched=" << audit.leaked << " domains=" << audit.domains.size()
            << "\n";
  for (const DomainAccounting& row : audit.domains) {
    std::cout << "  domain " << format_id(row.domain.value())
              << " active=" << row.active_count << " committed=" << row.committed_count
              << " released=" << row.released_count << " rolled_back=" << row.rolled_back_count
              << " invalidated=" << row.invalidated_count << "\n";
    std::cout << "    capacity:";
    for (std::size_t i = 0; i < row.capacity.size(); ++i) {
      if (row.capacity[i] == 0) continue;
      std::cout << " " << to_string(static_cast<ResourceKind>(i)) << "=" << row.capacity[i];
    }
    std::cout << "\n    held:";
    for (std::size_t i = 0; i < row.reserved.size(); ++i) {
      if (row.reserved[i] == 0 && row.committed[i] == 0) continue;
      std::cout << " " << to_string(static_cast<ResourceKind>(i))
                << " reserved=" << row.reserved[i] << " committed=" << row.committed[i];
    }
    std::cout << "\n";
  }
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
  Scheduler& scheduler = *runtime.scheduler;
  const std::vector<std::uint8_t> payload = make_payload(4096, 11);

  std::cout << "reservation policy: per_operation execution_slot="
            << scheduler.policy()
                   .reservation.per_operation[static_cast<std::size_t>(ResourceKind::kExecutionSlot)]
            << " offload_only="
            << (scheduler.policy().reservation.require_reservation_for_offload_only ? "yes" : "no")
            << "\n\n";
  print_audit("ledger before", scheduler.reservations());
  std::cout << "\n";

  std::cout << "step 1: plan\n";
  PlanResult planned = scheduler.plan(make_request(payload.size()), payload);
  if (!planned.planned) {
    std::cerr << "planning failed: " << planned.status.code << std::endl;
    return 1;
  }
  std::cout << "  domain=" << format_id(planned.plan.domain.value())
            << " type=" << to_string(planned.plan.domain_type)
            << " outcome=" << to_string(planned.explanation.outcome)
            << " reservation=" << format_id(planned.plan.reservation.value()) << "\n";
  print_audit("  ledger after planning (no capacity consumed)", scheduler.reservations());

  std::cout << "\nstep 2: reserve\n";
  const Status reserved = scheduler.reserve(planned.plan);
  if (!reserved) {
    std::cerr << "reserve failed: " << reserved.code << " " << reserved.message << std::endl;
    return 1;
  }
  std::cout << "  reservation=" << format_id(planned.plan.reservation.value())
            << " amounts:";
  for (std::size_t i = 0; i < planned.plan.reserved_amounts.size(); ++i) {
    if (planned.plan.reserved_amounts[i] == 0) continue;
    std::cout << " " << to_string(static_cast<ResourceKind>(i)) << "="
              << planned.plan.reserved_amounts[i];
  }
  std::cout << "\n";
  print_audit("  ledger while reserved", scheduler.reservations());

  std::cout << "\nstep 3: revalidate the binding against live authority\n";
  ExecutionDomainRecord record;
  const bool present = scheduler.domains().get(planned.plan.domain, record);
  const AuthorityValidation validation =
      present ? validate_authority(planned.plan.binding,
                                   live_authority_of(record, scheduler.coordinator_epoch(),
                                                     scheduler.policy_generation(),
                                                     scheduler.policy().isolation_generation),
                                   scheduler.policy().freshness)
              : AuthorityValidation{};
  std::cout << "  domain_present=" << (present ? "yes" : "no")
            << " valid=" << (validation.valid ? "yes" : "no") << " code=" << validation.code
            << "\n";
  for (const AuthorityMismatch& mismatch : validation.mismatches) {
    std::cout << "  mismatch " << mismatch.field << " bound=" << mismatch.bound
              << " observed=" << mismatch.observed << " (" << mismatch.detail << ")\n";
  }

  std::cout << "\nstep 4: dispatch\n";
  const DispatchResult dispatched = scheduler.dispatch(planned.plan);
  if (!dispatched.dispatched) {
    std::cerr << "dispatch failed: " << dispatched.status.code << " "
              << to_string(dispatched.rejection) << std::endl;
    return 1;
  }
  std::cout << "  dispatched attempt=" << format_id(planned.plan.attempt.value())
            << " dispatch=" << format_id(planned.plan.dispatch.value()) << "\n";
  Reservation after_commit;
  if (scheduler.reservations().get(planned.plan.reservation, after_commit)) {
    std::cout << "  reservation state after dispatch=" << to_string(after_commit.state) << "\n";
  }
  print_audit("  ledger after dispatch", scheduler.reservations());

  std::cout << "\nstep 5: complete\n";
  const CompletionOutcome completion = runtime.tracker.wait_for(planned.plan.attempt);
  std::cout << "  committed=" << (completion.committed ? "yes" : "no")
            << " rejection=" << to_string(completion.rejection)
            << " attempt_state=" << to_string(completion.attempt.state)
            << " digest=" << completion.attempt.result.result_digest
            << " expected=" << static_cast<std::uint64_t>(crc32c(payload.data(), payload.size()))
            << "\n";
  Reservation after_release;
  if (scheduler.reservations().get(planned.plan.reservation, after_release)) {
    std::cout << "  reservation state after completion=" << to_string(after_release.state)
              << " close_reason=" << after_release.close_reason << "\n";
  }
  print_audit("  ledger after completion (capacity returned)", scheduler.reservations());

  std::cout << "\nstep 6: a refused over-request\n";
  ReservationRequest over;
  over.domain = planned.plan.domain;
  over.domain_generation = record.generation;
  over.capability_generation = record.capability.generation;
  over.policy_generation = scheduler.policy_generation();
  over.worker_boot = record.worker_boot;
  over.operation = planned.plan.operation;
  over.amounts[static_cast<std::size_t>(ResourceKind::kExecutionSlot)] = 9;
  const Checked<ReservationId> refused = scheduler.reservations().acquire(over);
  std::cout << "  requested execution_slot=9 against capacity="
            << record.capacity[static_cast<std::size_t>(ResourceKind::kExecutionSlot)] << "\n";
  std::cout << "  acquired=" << (refused.ok() ? "yes" : "no")
            << " code=" << refused.status.code << " message=" << refused.status.message << "\n";
  if (refused.ok()) {
    std::cerr << "an over-request must be refused" << std::endl;
    return 1;
  }

  std::cout << "\nstep 7: the planner refuses a request it cannot serve\n";
  SchedulerPolicy tightened = scheduler.policy();
  tightened.reservation.per_operation[static_cast<std::size_t>(ResourceKind::kExecutionSlot)] = 4;
  const Status applied = scheduler.set_policy(std::move(tightened));
  if (!applied) {
    std::cerr << "set_policy failed: " << applied.code << std::endl;
    return 1;
  }
  // These operations may only run on the offload engine, so the host CPU is not a
  // legal candidate and the engine's capacity is the only capacity that can serve them.
  OperationRequest restricted = make_request(2048);
  restricted.allowed_domains = {ExecutionDomainType::kDpu};
  PlanResult first = scheduler.plan(restricted);
  if (!first.planned || !scheduler.reserve(first.plan)) {
    std::cerr << "first reservation failed" << std::endl;
    return 1;
  }
  PlanResult second = scheduler.plan(restricted);
  if (!second.planned || !scheduler.reserve(second.plan)) {
    std::cerr << "second reservation failed" << std::endl;
    return 1;
  }
  print_audit("  ledger with two reservations held (4 + 4 of 8)", scheduler.reservations());
  const PlanResult third = scheduler.plan(restricted);
  std::cout << "  third plan: planned=" << (third.planned ? "yes" : "no")
            << " outcome=" << to_string(third.explanation.outcome)
            << " status=" << third.status.code << "\n";
  for (const CandidateEvaluation& candidate : third.explanation.candidates) {
    std::cout << "    candidate " << format_id(candidate.domain.value())
              << " type=" << to_string(candidate.domain_type)
              << " eligible=" << (candidate.eligible ? "yes" : "no")
              << " reason=" << to_string(candidate.reason) << " (" << candidate.detail << ")\n";
  }
  if (third.planned) {
    std::cerr << "a plan that cannot be served must not be created" << std::endl;
    return 1;
  }
  std::cout << "  the planner refused to create a plan of record: every execution slot of the "
               "offload engine is held, and no plan is created that could not be served\n";

  const Status released_first = scheduler.reservations().release(first.plan.reservation,
                                                                "example finished");
  const Status released_second = scheduler.reservations().release(second.plan.reservation,
                                                                 "example finished");
  if (!released_first || !released_second) {
    std::cerr << "releasing the held reservations failed" << std::endl;
    return 1;
  }
  print_audit("\nledger after releasing everything", scheduler.reservations());
  const ReservationAudit final_audit = scheduler.reservations().audit();
  if (final_audit.outstanding != 0 || final_audit.committed != 0 || final_audit.leaked != 0) {
    std::cerr << "reservation accounting did not return to zero" << std::endl;
    return 1;
  }

  const Status stopped = scheduler.shutdown();
  if (!stopped) {
    std::cerr << "scheduler shutdown failed: " << stopped.code << std::endl;
    return 1;
  }
  return 0;
}
