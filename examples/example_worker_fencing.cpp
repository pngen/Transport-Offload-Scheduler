// Worker fencing with a real process: a real coordinator, a real worker process, a
// real kill and a real replacement incarnation.
//
// The coordinator runs in this process (real sockets, real threads). The worker is a
// separate OS process located next to this example; killing it is a forceful process
// termination, not a simulated flag. Losing the session is what fences the worker
// incarnation, which in turn makes every domain it owned hard-ineligible. A
// replacement worker is accepted only with a fresh boot identity.
//
// There are no sleeps and no timeouts: every wait is a bounded spin that yields.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "tos/dist/coordinator.hpp"
#include "tos/tos.hpp"

using namespace tos;

namespace {

/// Directory of the running example, so sibling tools are located without hardcoded
/// absolute paths.
std::string executable_directory() {
  const std::string path = current_executable_path();
  const std::size_t separator = path.find_last_of("\\/");
  if (separator == std::string::npos) return ".";
  return path.substr(0, separator);
}

Checked<std::string> locate_tool(const std::string& tool) {
  const std::string directory = executable_directory();
  const std::string candidates[] = {directory + "\\" + tool + ".exe",
                                    directory + "\\..\\..\\Release\\" + tool + ".exe"};
  for (const std::string& candidate : candidates) {
    if (file_exists(candidate)) return Checked<std::string>::good(candidate);
  }
  return Checked<std::string>::bad("example.tool_not_found",
                                   tool + " was not found next to the example binary");
}

Checked<std::shared_ptr<ProcessHandle>> spawn_tool(const std::string& tool,
                                                   const std::vector<std::string>& arguments,
                                                   const std::string& stderr_path) {
  const Checked<std::string> resolved = locate_tool(tool);
  if (!resolved.ok()) {
    return Checked<std::shared_ptr<ProcessHandle>>::bad(resolved.status.code,
                                                        resolved.status.message);
  }
  ProcessOptions options;
  options.argv.push_back(resolved.value);
  for (const std::string& argument : arguments) options.argv.push_back(argument);
  options.working_directory = executable_directory();
  options.capture_stdout = true;
  options.capture_stderr = false;
  options.stderr_path = stderr_path;
  return spawn_process(options);
}

/// Bounded spin that yields the processor. No sleep, no timeout: the bound exists so a
/// stuck runtime fails the example instead of hanging it.
template <class Predicate>
bool spin_until(Predicate predicate, std::uint64_t attempts = 200000000ULL) {
  for (std::uint64_t i = 0; i < attempts; ++i) {
    if (predicate()) return true;
    std::this_thread::yield();
  }
  return false;
}

/// Read a worker's stdout until it reports a line starting with the token.
std::string wait_for_line(const std::shared_ptr<ProcessHandle>& handle, const std::string& token) {
  std::string collected;
  const bool seen = spin_until([&] {
    collected += read_process_stdout(handle);
    return collected.find(token) != std::string::npos || !process_alive(handle);
  });
  if (!seen) return std::string();
  const std::size_t position = collected.find(token);
  if (position == std::string::npos) return std::string();
  const std::size_t end = collected.find('\n', position);
  return collected.substr(position,
                          end == std::string::npos ? std::string::npos : end - position);
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
  const std::string stderr_first = "example_worker_fencing.first.stderr.log";
  const std::string stderr_replacement = "example_worker_fencing.replacement.stderr.log";
  dist::CoordinatorConfig config;
  config.bind_host = "127.0.0.1";
  config.port = 0;
  config.scheduler.policy = make_default_policy();
  config.scheduler.host_node = "example.host";
  dist::OffloadCoordinator coordinator(std::move(config));
  const Status started = coordinator.start();
  if (!started) {
    std::cerr << "coordinator start failed: " << started.code << std::endl;
    return 1;
  }
  Scheduler& scheduler = coordinator.scheduler();
  const std::uint16_t port = coordinator.port();
  std::cout << "step 1: coordinator serving on 127.0.0.1:" << port
            << " epoch=" << scheduler.coordinator_epoch().value() << "\n";

  const std::string port_text = std::to_string(port);
  const std::vector<std::string> worker_arguments = {
      "--host", "127.0.0.1", "--port", port_text, "--name", "example-dpu",
      "--type", "dpu",       "--node", "example.host", "--domain-base", "4096",
      "--provenance", "synthetic", "--log-level", "warn"};
  auto worker = spawn_tool("tos_worker", worker_arguments, stderr_first);
  if (!worker.ok()) {
    std::cerr << "worker spawn failed: " << worker.status.code << " " << worker.status.message
              << std::endl;
    return 1;
  }
  const std::string first_ready = wait_for_line(worker.value, "WORKER_READY");
  if (first_ready.rfind("WORKER_READY", 0) != 0) {
    std::cerr << "worker did not report readiness" << std::endl;
    return 1;
  }
  std::cout << "step 2: worker process " << process_id(worker.value) << " ready: " << first_ready
            << "\n";

  if (!spin_until([&] { return scheduler.domains().size() >= 1; })) {
    std::cerr << "worker domain was never published" << std::endl;
    return 1;
  }
  const ExecutionDomainId domain_id(4096);
  ExecutionDomainRecord domain;
  if (!scheduler.domains().get(domain_id, domain)) {
    std::cerr << "expected domain 4096 was not registered" << std::endl;
    return 1;
  }
  const WorkerBootId first_boot = domain.worker_boot;
  std::cout << "step 3: domain " << format_id(domain.id.value()) << " type=" << to_string(domain.type)
            << " provenance=" << to_string(domain.provenance)
            << " boot=" << format_id(first_boot.value())
            << " load_published=" << (domain.load.published() ? "yes" : "no") << "\n";

  const std::vector<std::uint8_t> payload = make_payload(4096, 23);
  const OperationRequest request = make_request(payload.size());
  DispatchOutcome first = scheduler.plan_reserve_dispatch(
      request, std::span<const std::uint8_t>(payload.data(), payload.size()));
  if (!first.plan_result.planned || !first.dispatch_result.dispatched) {
    std::cerr << "dispatch to the worker failed: " << first.plan_result.status.code << " "
              << first.dispatch_result.status.code << std::endl;
    return 1;
  }
  const ExecutionAttemptId first_attempt = first.plan_result.plan.attempt;
  if (!spin_until([&] {
        ExecutionAttempt stored;
        return scheduler.attempts().get(first_attempt, stored) &&
               stored.state == AttemptState::kCompleted;
      })) {
    std::cerr << "worker never completed the operation" << std::endl;
    return 1;
  }
  ExecutionAttempt first_record;
  if (!scheduler.attempts().get(first_attempt, first_record)) {
    std::cerr << "attempt record disappeared" << std::endl;
    return 1;
  }
  const std::uint64_t expected_digest =
      static_cast<std::uint64_t>(crc32c(payload.data(), payload.size()));
  std::cout << "step 4: real operation executed by the worker: state="
            << to_string(first_record.state) << " digest=" << first_record.result.result_digest
            << " expected=" << expected_digest
            << " provenance=" << to_string(first_record.provenance)
            << " bytes=" << first_record.result.bytes_processed << "\n";
  if (first_record.result.result_digest != expected_digest) {
    std::cerr << "worker result digest does not match the payload" << std::endl;
    return 1;
  }

  std::cout << "step 5: killing the worker process (forceful termination)\n";
  const Status killed = terminate_process(worker.value);
  if (!killed) {
    std::cerr << "terminate_process failed: " << killed.code << std::endl;
    return 1;
  }
  if (!spin_until([&] { return !scheduler.workers().is_current(first_record.worker, first_boot); })) {
    std::cerr << "the dead worker incarnation was never fenced" << std::endl;
    return 1;
  }
  const bool boot_fenced = scheduler.workers().fenced_boot_id(first_boot);
  ExecutionDomainRecord fenced_domain;
  const bool still_present = scheduler.domains().get(domain_id, fenced_domain);
  std::cout << "step 6: boot " << format_id(first_boot.value())
            << " fenced=" << (boot_fenced ? "yes" : "no")
            << " domain_present=" << (still_present ? "yes" : "no")
            << " domain_fenced=" << (fenced_domain.fenced ? "yes" : "no")
            << " generation=" << fenced_domain.generation.value() << "\n";

  const PlanResult after_death = scheduler.plan(request, payload);
  std::cout << "step 7: plan against the fenced domain: planned="
            << (after_death.planned ? "yes" : "no")
            << " outcome=" << to_string(after_death.explanation.outcome)
            << " status=" << after_death.status.code << "\n";
  for (const CandidateEvaluation& candidate : after_death.explanation.candidates) {
    std::cout << "  candidate " << format_id(candidate.domain.value())
              << " eligible=" << (candidate.eligible ? "yes" : "no")
              << " reason=" << to_string(candidate.reason) << " (" << candidate.detail << ")\n";
  }
  if (after_death.planned) {
    std::cerr << "a fenced worker domain must not be planned" << std::endl;
    return 1;
  }

  auto replacement = spawn_tool("tos_worker", worker_arguments, stderr_replacement);
  if (!replacement.ok()) {
    std::cerr << "replacement spawn failed: " << replacement.status.code << std::endl;
    return 1;
  }
  const std::string second_ready = wait_for_line(replacement.value, "WORKER_READY");
  if (second_ready.rfind("WORKER_READY", 0) != 0) {
    std::cerr << "replacement worker did not report readiness" << std::endl;
    return 1;
  }
  std::cout << "step 8: replacement worker process " << process_id(replacement.value)
            << " ready: " << second_ready << "\n";
  if (second_ready == first_ready) {
    std::cerr << "the replacement must announce a fresh boot identity" << std::endl;
    return 1;
  }

  if (!spin_until([&] {
        ExecutionDomainRecord current;
        return scheduler.domains().get(domain_id, current) && !current.fenced &&
               current.load.published() && current.worker_boot != first_boot;
      })) {
    std::cerr << "the replacement worker never restored the domain" << std::endl;
    return 1;
  }
  ExecutionDomainRecord restored;
  if (!scheduler.domains().get(domain_id, restored)) {
    std::cerr << "restored domain disappeared" << std::endl;
    return 1;
  }
  std::cout << "step 9: domain restored with boot " << format_id(restored.worker_boot.value())
            << " fenced=" << (restored.fenced ? "yes" : "no")
            << " generation=" << restored.generation.value()
            << " (was " << fenced_domain.generation.value() << ")\n";
  if (scheduler.workers().fenced_boot_id(restored.worker_boot)) {
    std::cerr << "the replacement boot identity must not be fenced" << std::endl;
    return 1;
  }

  DispatchOutcome second = scheduler.plan_reserve_dispatch(
      request, std::span<const std::uint8_t>(payload.data(), payload.size()));
  if (!second.plan_result.planned || !second.dispatch_result.dispatched) {
    std::cerr << "dispatch to the replacement worker failed: " << second.plan_result.status.code
              << " " << second.dispatch_result.status.code << std::endl;
    return 1;
  }
  const ExecutionAttemptId second_attempt = second.plan_result.plan.attempt;
  if (!spin_until([&] {
        ExecutionAttempt stored;
        return scheduler.attempts().get(second_attempt, stored) &&
               stored.state == AttemptState::kCompleted;
      })) {
    std::cerr << "the replacement worker never completed the operation" << std::endl;
    return 1;
  }
  ExecutionAttempt second_record;
  if (!scheduler.attempts().get(second_attempt, second_record)) {
    std::cerr << "replacement attempt record disappeared" << std::endl;
    return 1;
  }
  std::cout << "step 10: replacement executed the operation: state="
            << to_string(second_record.state) << " digest=" << second_record.result.result_digest
            << " boot=" << format_id(second_record.worker_boot.value()) << "\n";
  if (second_record.result.result_digest != expected_digest ||
      second_record.worker_boot != restored.worker_boot) {
    std::cerr << "replacement result does not match" << std::endl;
    return 1;
  }

  const Status stopped = coordinator.stop();
  if (!stopped) {
    std::cerr << "coordinator stop failed: " << stopped.code << std::endl;
    return 1;
  }
  // stop() asks every live worker to shut down, so the replacement exits on its own.
  const auto replacement_exit = wait_process(replacement.value);
  std::cout << "step 11: coordinator stopped; replacement exited with code "
            << (replacement_exit.ok() ? replacement_exit.value : -1) << "\n";
  close_process(worker.value);
  close_process(replacement.value);
  std::remove(stderr_first.c_str());
  std::remove(stderr_replacement.c_str());
  return replacement_exit.ok() ? 0 : 1;
}
