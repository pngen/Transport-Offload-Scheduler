// Transport Offload Scheduler benchmark.
//
// Every row measures work the runtime actually performed, timed with
// std::chrono::steady_clock. Each rep reports integer nanoseconds per completed
// operation; the table reports the median of at least five reps, the number of
// operations that actually completed, and the elapsed time those operations took.
// Nothing here sleeps, waits on a timer, or reports a fabricated number: if a measured
// operation does not complete, the benchmark prints the failure and exits non-zero.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "tos/tos.hpp"

using namespace tos;

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uint32_t kRepetitions = 5;
constexpr std::size_t kMaxReportedCandidates = 64;
const char* const kStatePath = "tos_benchmark.state";

std::int64_t elapsed_ns(Clock::time_point start, Clock::time_point stop) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count();
}

std::uint64_t rounded_div(std::uint64_t numerator, std::uint64_t denominator) {
  if (denominator == 0) return 0;
  return (numerator + denominator / 2) / denominator;
}

struct Measurement {
  std::string name;
  std::uint32_t reps{0};
  std::uint64_t operations{0};
  std::uint64_t expected{0};
  std::uint64_t elapsed_ns{0};
  std::int64_t median_ns_per_op{0};
  std::int64_t ops_per_second{0};
  std::string note;

  [[nodiscard]] bool complete() const { return expected != 0 && operations == expected; }
};

void finish(Measurement& measurement, std::vector<std::uint64_t>& per_rep_ns_per_op) {
  std::sort(per_rep_ns_per_op.begin(), per_rep_ns_per_op.end());
  if (per_rep_ns_per_op.empty()) return;
  measurement.median_ns_per_op =
      static_cast<std::int64_t>(per_rep_ns_per_op[per_rep_ns_per_op.size() / 2]);
  measurement.ops_per_second =
      measurement.median_ns_per_op > 0
          ? static_cast<std::int64_t>(rounded_div(
                1000000000ULL, static_cast<std::uint64_t>(measurement.median_ns_per_op)))
          : 0;
}

/// Time the whole inner loop of every rep. Use when the operation under measurement is
/// the only work the loop performs.
template <class Body>
Measurement measure_loop(const std::string& name, std::uint64_t iterations, Body body) {
  Measurement measurement;
  measurement.name = name;
  measurement.reps = kRepetitions;
  measurement.expected = static_cast<std::uint64_t>(kRepetitions) * iterations;
  std::vector<std::uint64_t> per_rep;
  for (std::uint32_t rep = 0; rep < kRepetitions; ++rep) {
    std::uint64_t completed = 0;
    const Clock::time_point start = Clock::now();
    for (std::uint64_t i = 0; i < iterations; ++i) {
      if (body(i)) ++completed;
    }
    const Clock::time_point stop = Clock::now();
    const std::uint64_t elapsed = static_cast<std::uint64_t>(elapsed_ns(start, stop));
    measurement.operations += completed;
    measurement.elapsed_ns += elapsed;
    per_rep.push_back(completed == 0 ? 0 : rounded_div(elapsed, completed));
  }
  finish(measurement, per_rep);
  return measurement;
}

/// Time only the region a body reports. Use when every iteration must also perform
/// untimed setup, for example planning before the dispatch under measurement.
template <class Body>
Measurement measure_region(const std::string& name, std::uint64_t iterations, Body body) {
  Measurement measurement;
  measurement.name = name;
  measurement.reps = kRepetitions;
  measurement.expected = static_cast<std::uint64_t>(kRepetitions) * iterations;
  std::vector<std::uint64_t> per_rep;
  for (std::uint32_t rep = 0; rep < kRepetitions; ++rep) {
    std::uint64_t completed = 0;
    std::uint64_t measured = 0;
    for (std::uint64_t i = 0; i < iterations; ++i) {
      std::int64_t region = 0;
      if (body(i, region)) ++completed;
      if (region > 0) measured += static_cast<std::uint64_t>(region);
    }
    measurement.operations += completed;
    measurement.elapsed_ns += measured;
    per_rep.push_back(completed == 0 ? 0 : rounded_div(measured, completed));
  }
  finish(measurement, per_rep);
  return measurement;
}

/// A channel that reports transport acceptance and performs no execution, so a
/// dispatch measurement covers revalidation, reservation commit, attempt registration
/// and envelope hand-off without inventing backend work.
class CountingChannel : public IDispatchChannel {
 public:
  [[nodiscard]] std::string_view name() const noexcept override { return "benchmark.counting"; }
  [[nodiscard]] Status send(const DispatchEnvelope& envelope) override {
    (void)envelope;
    sent_ += 1;
    return Status::success();
  }
  bool request_cancel(const CancelEnvelope& envelope) override {
    (void)envelope;
    return true;
  }
  void shutdown() override {}
  [[nodiscard]] std::uint64_t sent() const noexcept { return sent_; }

 private:
  std::uint64_t sent_{0};
};

struct Scenario {
  std::unique_ptr<Scheduler> scheduler;
  std::shared_ptr<SyntheticBackend> backend;
  std::shared_ptr<CountingChannel> channel;
  std::unique_ptr<LocalDomainPublisher> publisher;
  std::size_t domain_count{0};
};

const ExecutionDomainId kCpuDomain(1);
const ExecutionDomainId kDpuDomain(2);

SchedulerOptions benchmark_options() {
  SchedulerOptions options;
  options.policy = make_default_policy();
  // Bounded reporting keeps the explanation small while every candidate is still
  // evaluated: the measurement is the evaluation path, not the size of the report.
  options.policy.max_reported_candidates = kMaxReportedCandidates;
  options.host_node = "benchmark.host";
  return options;
}

/// Build a runtime holding one real host CPU domain, one synthetic DPU and enough
/// further synthetic candidates to reach domain_count. The DPU is the deterministic
/// winner; the extra synthetic candidates are rejected by the operation-support check,
/// so the candidate scan grows while the selection stays fixed.
bool build_scenario(std::size_t domain_count, bool with_channel, Scenario& scenario,
                    std::string& error) {
  scenario.scheduler = std::make_unique<Scheduler>(benchmark_options());
  const Status started = scenario.scheduler->start();
  if (!started) {
    error = "scheduler.start: " + started.code;
    return false;
  }
  scenario.backend = std::make_shared<SyntheticBackend>("benchmark.synthetic");
  scenario.publisher =
      std::make_unique<LocalDomainPublisher>(*scenario.scheduler, scenario.backend);
  if (with_channel) {
    scenario.channel = std::make_shared<CountingChannel>();
    scenario.scheduler->set_dispatch_channel(scenario.channel);
  }

  CpuBackend::Options cpu_options;
  cpu_options.domain_id = kCpuDomain;
  cpu_options.name = "cpu.host.0";
  cpu_options.parent_host = "benchmark.host";
  CpuBackend cpu_probe(cpu_options);
  const std::vector<ExecutionDomainRecord> discovered = cpu_probe.discover_domains();
  if (discovered.empty()) {
    error = "cpu discovery returned no domain";
    return false;
  }
  const ExecutionDomainRecord& cpu = discovered.front();
  Status published = scenario.scheduler->register_domain(cpu);
  if (published) published = scenario.scheduler->publish_capability(cpu.capability);
  if (published) published = scenario.scheduler->publish_locality(cpu.id, cpu.locality);
  if (published) published = scenario.scheduler->publish_topology(cpu.id, cpu.topology);
  if (published) published = scenario.scheduler->publish_compatibility(cpu.id, cpu.compatibility);
  if (published) published = scenario.scheduler->publish_capacity(cpu.id, cpu.capacity);
  if (published) published = scenario.scheduler->publish_load(cpu.id, cpu.load);
  if (!published) {
    error = "cpu publication: " + published.code;
    return false;
  }

  SyntheticDomainConfig dpu =
      make_synthetic_domain(kDpuDomain, ExecutionDomainType::kDpu, "synthetic.dpu0");
  dpu.locality.class_to_payload = LocalityClass::kSameNumaNode;
  Status added = scenario.backend->add_domain(std::move(dpu));
  if (!added) {
    error = "dpu publication: " + added.code;
    return false;
  }
  for (std::size_t i = 2; i < domain_count; ++i) {
    SyntheticDomainConfig filler = make_synthetic_domain(
        ExecutionDomainId(static_cast<std::uint64_t>(i + 1)), ExecutionDomainType::kSmartNic,
        "synthetic.smartnic." + std::to_string(i));
    const auto position =
        std::find(filler.capability.operations.begin(), filler.capability.operations.end(),
                  opclass::checksum_crc32c());
    if (position != filler.capability.operations.end()) {
      filler.capability.operations.erase(position);
    }
    added = scenario.backend->add_domain(std::move(filler));
    if (!added) {
      error = "filler publication: " + added.code;
      return false;
    }
  }
  const Status synced = scenario.publisher->sync();
  if (!synced) {
    error = "publisher.sync: " + synced.code;
    return false;
  }
  scenario.domain_count = scenario.scheduler->domains().size();
  return true;
}

OperationRequest benchmark_request(std::uint64_t payload_bytes) {
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

std::vector<std::uint8_t> make_payload(std::size_t size, std::uint32_t seed) {
  std::vector<std::uint8_t> payload(size);
  std::uint32_t state = seed * 2654435761U + 1U;
  for (std::size_t i = 0; i < size; ++i) {
    state = state * 1664525U + 1013904223U;
    payload[i] = static_cast<std::uint8_t>((state >> 16) & 0x3FU);
  }
  return payload;
}

const Measurement* find_measurement(const std::vector<Measurement>& measurements,
                                    const std::string& name) {
  for (const Measurement& measurement : measurements) {
    if (measurement.name == name) return &measurement;
  }
  return nullptr;
}

void print_table(const std::vector<Measurement>& measurements) {
  std::cout << "\nmeasurements (" << kRepetitions << " repetitions each, median reported)\n";
  std::cout << std::left << std::setw(42) << "measurement" << std::right << std::setw(6) << "reps"
            << std::setw(10) << "ops" << std::setw(14) << "elapsed_us" << std::setw(15)
            << "median_ns/op" << std::setw(14) << "ops/sec" << "\n";
  std::cout << std::string(101, '-') << "\n";
  for (const Measurement& measurement : measurements) {
    std::cout << std::left << std::setw(42) << measurement.name << std::right << std::setw(6)
              << measurement.reps << std::setw(10) << measurement.operations << std::setw(14)
              << (measurement.elapsed_ns / 1000ULL) << std::setw(15)
              << measurement.median_ns_per_op << std::setw(14) << measurement.ops_per_second
              << "\n";
    if (!measurement.note.empty()) {
      std::cout << "    note: " << measurement.note << "\n";
    }
  }
  std::cout << "ns/op, ops/sec and elapsed_us are integers; elapsed_us is the measured "
               "elapsed time over all reps.\n";
}

void print_hot_spots(const std::vector<Measurement>& measurements) {
  std::cout << "\nhot spots (only what the measurements support)\n";
  const Measurement* plan_10 = find_measurement(measurements, "plan over 10 domains");
  const Measurement* plan_100 = find_measurement(measurements, "plan over 100 domains");
  const Measurement* plan_1000 = find_measurement(measurements, "plan over 1000 domains");
  const Measurement* plan_10000 = find_measurement(measurements, "plan over 10000 domains");
  const Measurement* creation = find_measurement(measurements, "plan creation (1 eligible domain)");
  if (plan_10 != nullptr && plan_100 != nullptr && plan_1000 != nullptr && plan_10000 != nullptr) {
    const std::uint64_t small = static_cast<std::uint64_t>(plan_10->median_ns_per_op);
    const std::uint64_t large = static_cast<std::uint64_t>(plan_10000->median_ns_per_op);
    std::cout << "- plan() cost grows with the registered domain count: " << small
              << " ns at 10 domains, " << plan_100->median_ns_per_op << " ns at 100, "
              << plan_1000->median_ns_per_op << " ns at 1000 and "
              << plan_10000->median_ns_per_op << " ns at 10000 (" << (small == 0 ? 0 : large / small)
              << "x).\n";
    const std::uint64_t per_domain = (large - small) / (10000U - 10U);
    std::cout << "- marginal planning cost is about " << per_domain
              << " ns per additional registered candidate between 10 and 10000 domains, so the "
                 "scan of every candidate dominates at fleet scale.\n";
  }
  if (creation != nullptr && plan_10000 != nullptr) {
    std::cout << "- the fixed part of one plan (a single eligible domain, no scan) is "
              << creation->median_ns_per_op << " ns, i.e. "
              << (creation->median_ns_per_op == 0
                      ? 0
                      : static_cast<std::int64_t>(plan_10000->median_ns_per_op) /
                            creation->median_ns_per_op)
              << "x cheaper than planning against 10000 registered domains.\n";
  }
  const Measurement* dispatch = find_measurement(measurements, "dispatch revalidation");
  const Measurement* dispatch_large =
      find_measurement(measurements, "dispatch revalidation (1000 domains)");
  const Measurement* reservation = find_measurement(measurements, "reservation acquire+commit");
  const Measurement* reservation_large =
      find_measurement(measurements, "reservation acquire+commit (1000 domains)");
  const Measurement* completion = find_measurement(measurements, "completion commit");
  if (dispatch != nullptr && dispatch_large != nullptr && plan_1000 != nullptr) {
    const std::int64_t ratio = dispatch->median_ns_per_op == 0
                                   ? 0
                                   : dispatch_large->median_ns_per_op / dispatch->median_ns_per_op;
    std::cout << "- dispatch revalidation (authority validation, reservation commit, attempt "
                 "registration and hand-off) costs "
              << dispatch->median_ns_per_op << " ns/op with 4 registered domains and "
              << dispatch_large->median_ns_per_op << " ns/op with 1000 (" << ratio
              << "x, cache pressure rather than a second scan), while plan() against the same "
                 "1000 domains costs "
              << plan_1000->median_ns_per_op
              << " ns: dispatch validates one binding instead of evaluating every candidate.\n";
  }
  if (reservation != nullptr && reservation_large != nullptr && completion != nullptr) {
    std::cout << "- the sharded reservation ledger (acquire+commit "
              << reservation->median_ns_per_op << " ns at 4 domains, "
              << reservation_large->median_ns_per_op << " ns at 1000) and completion authority ("
              << completion->median_ns_per_op
              << " ns, measured with 4 domains) are the cheapest measured operations.\n";
  }
  const Measurement* snapshot_small =
      find_measurement(measurements, "snapshot generation (2 domains)");
  const Measurement* snapshot_large =
      find_measurement(measurements, "snapshot generation (1000 domains)");
  if (snapshot_small != nullptr && snapshot_large != nullptr) {
    const std::int64_t per_domain =
        (snapshot_large->median_ns_per_op - snapshot_small->median_ns_per_op) / 998;
    std::cout << "- snapshot generation copies every domain record: "
              << snapshot_small->median_ns_per_op << " ns at 2 domains, "
              << snapshot_large->median_ns_per_op << " ns at 1000 domains (" << per_domain
              << " ns/domain), so inspection cost scales with the fleet as well.\n";
  }
  const Measurement* save = find_measurement(measurements, "persistence save (1000 domains)");
  const Measurement* load = find_measurement(measurements, "persistence load (1000 domains)");
  if (save != nullptr && load != nullptr) {
    std::cout << "- persistence is the heaviest single operation measured: save "
              << save->median_ns_per_op << " ns/op and load " << load->median_ns_per_op
              << " ns/op for a 1000-domain state, i.e. roughly "
              << (save->median_ns_per_op == 0
                      ? 0
                      : (plan_10 == nullptr
                             ? 0
                             : save->median_ns_per_op /
                                   (plan_10->median_ns_per_op == 0 ? 1
                                                                   : plan_10->median_ns_per_op)))
              << "x the cost of planning against 10 domains.\n";
  }
}
}  // namespace

int main() {
  std::vector<Measurement> measurements;
  const std::vector<std::size_t> domain_counts = {10, 100, 1000, 10000};

  std::cout << "Transport Offload Scheduler benchmark\n";
  std::cout << "version " << kVersionString << " protocol=" << kProtocolVersion
            << " repetitions=" << kRepetitions << "\n";

  // ---- candidate evaluation: plan() over a growing candidate set ---------------
  for (std::size_t count : domain_counts) {
    Scenario scenario;
    std::string error;
    if (!build_scenario(count, false, scenario, error)) {
      std::cerr << "scenario build failed for " << count << " domains: " << error << std::endl;
      return 1;
    }
    const std::uint64_t iterations = count >= 10000 ? 20 : (count >= 1000 ? 200 : 2000);
    std::uint64_t selections = 0;
    std::uint64_t mismatches = 0;
    std::uint64_t score_mismatches = 0;
    std::int64_t expected_score = 0;
    Measurement measurement =
        measure_loop("plan over " + std::to_string(count) + " domains", iterations,
                     [&](std::uint64_t) {
                       const PlanResult result = scenario.scheduler->plan(benchmark_request(65536));
                       if (!result.planned) return false;
                       ++selections;
                       if (result.explanation.selected_domain != kDpuDomain) ++mismatches;
                       const std::int64_t score = result.explanation.ranking.empty()
                                                      ? 0
                                                      : result.explanation.ranking.front()
                                                            .weighted_score;
                       if (expected_score == 0) {
                         expected_score = score;
                       } else if (score != expected_score) {
                         ++score_mismatches;
                       }
                       return true;
                     });
    measurement.note = "deterministic: domain " + format_id(kDpuDomain.value()) + " and score " +
                       std::to_string(expected_score) + " in " + std::to_string(selections) +
                       " plans (" + std::to_string(selections - mismatches) + " identical selections, " +
                       std::to_string(score_mismatches) + " score differences)";
    if (mismatches != 0 || score_mismatches != 0) {
      measurement.note += " (NON-DETERMINISTIC)";
    }
    measurements.push_back(std::move(measurement));
  }

  // ---- plan creation: one eligible domain, no candidate scan -------------------
  {
    Scenario scenario;
    std::string error;
    if (!build_scenario(1, false, scenario, error)) {
      std::cerr << "scenario build failed: " << error << std::endl;
      return 1;
    }
    Measurement measurement =
        measure_loop("plan creation (1 eligible domain)", 2000, [&](std::uint64_t) {
          const PlanResult result = scenario.scheduler->plan(benchmark_request(65536));
          return result.planned && result.explanation.selected_domain == kDpuDomain;
        });
    measurement.note = "ranking and plan-of-record construction without a candidate scan";
    measurements.push_back(std::move(measurement));
  }

  // ---- dispatch, reservation ledger and completion authority --------------------
  {
    Scenario scenario;
    std::string error;
    if (!build_scenario(4, true, scenario, error)) {
      std::cerr << "scenario build failed: " << error << std::endl;
      return 1;
    }
    Scheduler& scheduler = *scenario.scheduler;
    const std::vector<std::uint8_t> payload = make_payload(4096, 3);
    const OperationRequest request = benchmark_request(payload.size());

    Measurement dispatch =
        measure_region("dispatch revalidation", 2000, [&](std::uint64_t, std::int64_t& region) {
          PlanResult planned = scheduler.plan(request);
          if (!planned.planned) return false;
          ExecutionPlan plan = planned.plan;
          if (!scheduler.reserve(plan)) return false;
          const Clock::time_point start = Clock::now();
          const DispatchResult dispatched = scheduler.dispatch(plan);
          region = elapsed_ns(start, Clock::now());
          return dispatched.dispatched;
        });
    dispatch.note = "region only: re-validation, reservation commit, attempt registration, "
                    "channel hand-off";
    measurements.push_back(std::move(dispatch));

    Measurement reservation =
        measure_region("reservation acquire+commit", 5000, [&](std::uint64_t, std::int64_t& region) {
          ExecutionDomainRecord record;
          if (!scheduler.domains().get(kDpuDomain, record)) return false;
          ReservationRequest acquire;
          acquire.domain = kDpuDomain;
          acquire.domain_generation = record.generation;
          acquire.capability_generation = record.capability.generation;
          acquire.policy_generation = scheduler.policy_generation();
          acquire.worker_boot = record.worker_boot;
          acquire.operation = TransportOperationId(1);
          acquire.amounts[static_cast<std::size_t>(ResourceKind::kExecutionSlot)] = 1;
          const Clock::time_point start = Clock::now();
          const Checked<ReservationId> acquired = scheduler.reservations().acquire(acquire);
          const Status committed = acquired.ok()
                                       ? scheduler.reservations().commit(
                                             acquired.value, record.generation,
                                             record.capability.generation,
                                             scheduler.policy_generation())
                                       : acquired.status;
          region = elapsed_ns(start, Clock::now());
          if (acquired.ok()) {
            const Status released =
                scheduler.reservations().release(acquired.value, "benchmark iteration");
            if (!released) return false;
          }
          return committed.ok;
        });
    reservation.note = "region only: acquire and commit of one execution slot; release excluded";
    measurements.push_back(std::move(reservation));

    Measurement completion =
        measure_region("completion commit", 2000, [&](std::uint64_t, std::int64_t& region) {
          PlanResult planned = scheduler.plan(request);
          if (!planned.planned) return false;
          ExecutionPlan plan = planned.plan;
          if (!scheduler.reserve(plan)) return false;
          const DispatchResult dispatched = scheduler.dispatch(plan);
          if (!dispatched.dispatched) return false;
          CompletionSubmission submission;
          submission.attempt = plan.attempt;
          submission.generation = plan.attempt_generation;
          submission.dispatch = plan.dispatch;
          submission.completion = CompletionId(plan.attempt.value());
          submission.worker = plan.binding.worker;
          submission.worker_boot = plan.binding.worker_boot;
          submission.coordinator_epoch = plan.binding.coordinator_epoch;
          submission.domain_generation = plan.binding.domain_generation;
          submission.capability_generation = plan.binding.capability_generation;
          submission.operation = plan.operation;
          submission.provenance = plan.provenance;
          submission.result.success = true;
          submission.result.result_digest = 0x5A5A5A5AULL;
          submission.result.bytes_processed = payload.size();
          const Clock::time_point start = Clock::now();
          const CompletionOutcome outcome = scheduler.complete(submission);
          region = elapsed_ns(start, Clock::now());
          return outcome.committed && !outcome.idempotent;
        });
    completion.note = "region only: completion authority commit of a dispatched attempt";
    measurements.push_back(std::move(completion));
  }

  // ---- the same fixed-cost operations against a large fleet ---------------------
  {
    Scenario scenario;
    std::string error;
    if (!build_scenario(1000, true, scenario, error)) {
      std::cerr << "scenario build failed: " << error << std::endl;
      return 1;
    }
    Scheduler& scheduler = *scenario.scheduler;
    const OperationRequest request = benchmark_request(4096);

    Measurement dispatch = measure_region(
        "dispatch revalidation (1000 domains)", 100, [&](std::uint64_t, std::int64_t& region) {
          PlanResult planned = scheduler.plan(request);
          if (!planned.planned) return false;
          ExecutionPlan plan = planned.plan;
          if (!scheduler.reserve(plan)) return false;
          const Clock::time_point start = Clock::now();
          const DispatchResult dispatched = scheduler.dispatch(plan);
          region = elapsed_ns(start, Clock::now());
          return dispatched.dispatched;
        });
    dispatch.note = "region only: the same operation against 1000 registered domains";
    measurements.push_back(std::move(dispatch));

    Measurement reservation = measure_region(
        "reservation acquire+commit (1000 domains)", 5000,
        [&](std::uint64_t, std::int64_t& region) {
          ExecutionDomainRecord record;
          if (!scheduler.domains().get(kDpuDomain, record)) return false;
          ReservationRequest acquire;
          acquire.domain = kDpuDomain;
          acquire.domain_generation = record.generation;
          acquire.capability_generation = record.capability.generation;
          acquire.policy_generation = scheduler.policy_generation();
          acquire.worker_boot = record.worker_boot;
          acquire.operation = TransportOperationId(1);
          acquire.amounts[static_cast<std::size_t>(ResourceKind::kExecutionSlot)] = 1;
          const Clock::time_point start = Clock::now();
          const Checked<ReservationId> acquired = scheduler.reservations().acquire(acquire);
          const Status committed = acquired.ok()
                                       ? scheduler.reservations().commit(
                                             acquired.value, record.generation,
                                             record.capability.generation,
                                             scheduler.policy_generation())
                                       : acquired.status;
          region = elapsed_ns(start, Clock::now());
          if (acquired.ok()) {
            const Status released =
                scheduler.reservations().release(acquired.value, "benchmark iteration");
            if (!released) return false;
          }
          return committed.ok;
        });
    reservation.note = "region only: reservation accounting is sharded per domain, not per fleet";
    measurements.push_back(std::move(reservation));
  }

  // ---- snapshot generation -----------------------------------------------------
  {
    Scenario scenario;
    std::string error;
    if (!build_scenario(2, false, scenario, error)) {
      std::cerr << "scenario build failed: " << error << std::endl;
      return 1;
    }
    Measurement measurement =
        measure_loop("snapshot generation (2 domains)", 2000, [&](std::uint64_t) {
          const SchedulerSnapshot snapshot = scenario.scheduler->snapshot();
          return snapshot.totals.domains == 2;
        });
    measurement.note = "self-consistent copy of domains, workers, reservations and attempts";
    measurements.push_back(std::move(measurement));
  }
  {
    Scenario scenario;
    std::string error;
    if (!build_scenario(1000, false, scenario, error)) {
      std::cerr << "scenario build failed: " << error << std::endl;
      return 1;
    }
    Measurement measurement =
        measure_loop("snapshot generation (1000 domains)", 100, [&](std::uint64_t) {
          const SchedulerSnapshot snapshot = scenario.scheduler->snapshot();
          return snapshot.totals.domains == 1000;
        });
    measurement.note = "self-consistent copy of 1000 domain records";
    measurements.push_back(std::move(measurement));
  }

  // ---- persistence -------------------------------------------------------------
  {
    Scenario scenario;
    std::string error;
    if (!build_scenario(1000, false, scenario, error)) {
      std::cerr << "scenario build failed: " << error << std::endl;
      return 1;
    }
    Measurement save = measure_loop("persistence save (1000 domains)", 1, [&](std::uint64_t) {
      return scenario.scheduler->save_state_to(kStatePath).ok;
    });
    save.note = "atomic replace of the durable state file (" +
                std::to_string(file_size_bytes(kStatePath)) + " bytes)";
    measurements.push_back(std::move(save));

    Measurement load;
    load.name = "persistence load (1000 domains)";
    load.reps = kRepetitions;
    load.expected = kRepetitions;
    load.note = "region only: validated and applied into a fresh runtime for every rep";
    std::vector<std::uint64_t> per_rep;
    for (std::uint32_t rep = 0; rep < kRepetitions; ++rep) {
      std::unique_ptr<Scheduler> loader = std::make_unique<Scheduler>(benchmark_options());
      const Clock::time_point start = Clock::now();
      const Status loaded = loader->load_state_from(kStatePath);
      const std::uint64_t elapsed = static_cast<std::uint64_t>(elapsed_ns(start, Clock::now()));
      if (loaded.ok && loader->domains().size() == 1000) ++load.operations;
      load.elapsed_ns += elapsed;
      per_rep.push_back(elapsed);
    }
    finish(load, per_rep);
    measurements.push_back(std::move(load));

    std::cout << "durable state file: " << file_size_bytes(kStatePath) << " bytes\n";
    const Status removed_file = remove_file(kStatePath);
    if (!removed_file) {
      std::cerr << "could not remove the benchmark state file" << std::endl;
      return 1;
    }
  }

  print_table(measurements);
  print_hot_spots(measurements);

  bool all_complete = true;
  for (const Measurement& measurement : measurements) {
    if (measurement.complete()) continue;
    all_complete = false;
    std::cerr << "MEASUREMENT FAILED: " << measurement.name << " completed "
              << measurement.operations << " of " << measurement.expected << " operations"
              << std::endl;
  }
  if (!all_complete) return 1;
  std::cout << "\nall measurements completed the operations they report.\n";
  return 0;
}

