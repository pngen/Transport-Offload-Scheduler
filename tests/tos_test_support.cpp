// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "tos_test_support.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <thread>
#include <utility>

#include "tos/backends/cpu_backend.hpp"
#include "tos/util/log.hpp"

namespace tos_test {
namespace {

struct Registry {
  std::vector<std::pair<std::string, TestFunction>> tests;
};

Registry& registry() {
  static Registry instance;
  return instance;
}

int g_failures = 0;
std::string g_current;

}  // namespace

void register_test(const char* name, TestFunction function) {
  registry().tests.emplace_back(name, function);
}

void report_failure(const char* file, int line, const std::string& message) {
  ++g_failures;
  std::ostringstream stream;
  stream << "FAIL " << g_current << " " << file << ":" << line << " " << message;
  std::cout << stream.str() << std::endl;
}

std::string current_test_name() { return g_current; }

int failure_count() { return g_failures; }

std::vector<std::uint8_t> make_payload(std::size_t size, std::uint32_t seed) {
  std::vector<std::uint8_t> payload(size);
  std::uint32_t state = seed * 2654435761U + 1U;
  for (std::size_t i = 0; i < size; ++i) {
    state = state * 1664525U + 1013904223U;
    // Repeat-friendly but not constant: exercises both run-length and literal paths.
    payload[i] = static_cast<std::uint8_t>((state >> 16) & 0x3FU);
  }
  return payload;
}

int run_all(int argc, char** argv) {
  std::string filter;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    const std::string prefix = "--filter=";
    if (argument.rfind(prefix, 0) == 0) filter = argument.substr(prefix.size());
  }
  int ran = 0;
  for (const auto& entry : registry().tests) {
    if (!filter.empty() && entry.first.find(filter) == std::string::npos) continue;
    g_current = entry.first;
    std::cout << "RUN  " << entry.first << std::endl;
    entry.second();
    ++ran;
  }
  std::cout << (g_failures == 0 ? "PASS " : "FAIL ") << ran << " test(s), " << g_failures
            << " failure(s)" << std::endl;
  return g_failures == 0 ? 0 : 1;
}

void wait_until(const std::function<bool()>& predicate) {
  while (!predicate()) {
    std::this_thread::yield();
  }
}

std::string executable_directory() {
  const std::string path = tos::current_executable_path();
  const std::size_t separator = path.find_last_of("\\/");
  if (separator == std::string::npos) return ".";
  return path.substr(0, separator);
}

tos::Checked<std::shared_ptr<tos::ProcessHandle>> spawn_tool(
    const std::string& tool, const std::vector<std::string>& arguments,
    const std::string& stderr_path) {
  // Test executables live in a subdirectory of the build tree, so the tool is
  // located by probing the usual layouts instead of hardcoding a path.
  const std::string directory = executable_directory();
  const std::string candidates[] = {directory + "\\" + tool + ".exe",
                                    directory + "\\..\\..\\" + tool + ".exe",
                                    directory + "\\..\\..\\Release\\" + tool + ".exe",
                                    directory + "\\..\\..\\Debug\\" + tool + ".exe"};
  std::string resolved;
  for (const std::string& candidate : candidates) {
    if (tos::file_exists(candidate)) {
      resolved = candidate;
      break;
    }
  }
  if (resolved.empty()) {
    return tos::Checked<std::shared_ptr<tos::ProcessHandle>>::bad(
        "test.tool_not_found", tool + " was not found next to the test executable");
  }
  tos::ProcessOptions options;
  options.argv.push_back(resolved);
  for (const std::string& argument : arguments) options.argv.push_back(argument);
  options.working_directory = executable_directory();
  options.capture_stdout = true;
  options.capture_stderr = false;
  options.stderr_path = stderr_path;
  return tos::spawn_process(options);
}

// ---- TestRuntime -----------------------------------------------------------

TestRuntime::TestRuntime(Config config) {
  tos::SchedulerOptions options;
  options.policy = config.policy;
  options.limits = config.limits;
  options.enable_persistence = config.enable_persistence;
  options.state_path = config.state_path;
  options.local_provenance = config.local_provenance;
  options.host_node = "test.host";
  scheduler_ = std::make_unique<tos::Scheduler>(std::move(options));
  const tos::Status started = scheduler_->start();
  if (!started) {
    report_failure(__FILE__, __LINE__, "scheduler.start failed: " + started.code);
  }
  backend_ = std::make_shared<tos::SyntheticBackend>("test.synthetic");
  channel_ = std::make_shared<tos::LocalDispatchChannel>(
      backend_, [this](const tos::CompletionSubmission& submission) { on_completion(submission); },
      config.channel);
  scheduler_->set_dispatch_channel(channel_);
  publisher_ = std::make_unique<tos::LocalDomainPublisher>(*scheduler_, backend_);
}

TestRuntime::~TestRuntime() {
  if (scheduler_) {
    const tos::Status stopped = scheduler_->shutdown();
    (void)stopped;
  }
}

void TestRuntime::on_completion(const tos::CompletionSubmission& submission) {
  const tos::CompletionOutcome outcome = scheduler_->complete(submission);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    deliveries_[submission.attempt.value()] += 1;
    outcomes_.push_back(outcome);
    if (outcome.committed && !outcome.idempotent) {
      ++commits_;
    } else if (!outcome.committed) {
      ++rejections_;
    }
  }
  condition_.notify_all();
}

tos::ExecutionDomainId TestRuntime::add_domain(tos::SyntheticDomainConfig config) {
  const tos::ExecutionDomainId id = config.id;
  const tos::Status added = backend_->add_domain(std::move(config));
  if (!added) {
    report_failure(__FILE__, __LINE__, "backend.add_domain failed: " + added.code);
    return id;
  }
  const tos::Status synced = publisher_->sync();
  if (!synced) {
    report_failure(__FILE__, __LINE__, "publisher.sync failed: " + synced.code);
  }
  return id;
}

tos::Status TestRuntime::refresh() { return publisher_->sync(); }

void TestRuntime::wait_for(tos::ExecutionAttemptId attempt, std::size_t expected) {
  std::unique_lock<std::mutex> lock(mutex_);
  condition_.wait(lock, [this, attempt, expected] {
    const auto found = deliveries_.find(attempt.value());
    return found != deliveries_.end() && found->second >= expected;
  });
}

void TestRuntime::wait_for_total(std::size_t expected) {
  std::unique_lock<std::mutex> lock(mutex_);
  condition_.wait(lock, [this, expected] { return outcomes_.size() >= expected; });
}

bool TestRuntime::attempt_is_terminal(tos::ExecutionAttemptId attempt) const {
  tos::ExecutionAttempt stored;
  if (!scheduler_->attempts().get(attempt, stored)) return false;
  switch (stored.state) {
    case tos::AttemptState::kCompleted:
    case tos::AttemptState::kFailed:
    case tos::AttemptState::kOutcomeUnknown:
    case tos::AttemptState::kCancelled:
    case tos::AttemptState::kRejectedStale:
    case tos::AttemptState::kFenced: return true;
    default: return false;
  }
}

tos::ExecutionAttempt TestRuntime::wait_for_terminal(tos::ExecutionAttemptId attempt) {
  std::unique_lock<std::mutex> lock(mutex_);
  condition_.wait(lock, [this, attempt] {
    tos::ExecutionAttempt stored;
    if (!scheduler_->attempts().get(attempt, stored)) return false;
    switch (stored.state) {
      case tos::AttemptState::kCompleted:
      case tos::AttemptState::kFailed:
      case tos::AttemptState::kOutcomeUnknown:
      case tos::AttemptState::kCancelled:
      case tos::AttemptState::kRejectedStale:
      case tos::AttemptState::kFenced: return true;
      default: return false;
    }
  });
  tos::ExecutionAttempt stored;
  (void)scheduler_->attempts().get(attempt, stored);
  return stored;
}

std::size_t TestRuntime::deliveries(tos::ExecutionAttemptId attempt) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = deliveries_.find(attempt.value());
  return found == deliveries_.end() ? 0 : found->second;
}

std::size_t TestRuntime::total_deliveries() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return outcomes_.size();
}

std::size_t TestRuntime::commit_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return commits_;
}

std::size_t TestRuntime::rejection_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return rejections_;
}

tos::CompletionOutcome TestRuntime::last_outcome() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return outcomes_.empty() ? tos::CompletionOutcome{} : outcomes_.back();
}

std::vector<tos::CompletionOutcome> TestRuntime::outcomes() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return outcomes_;
}

void TestRuntime::clear_completions() {
  std::lock_guard<std::mutex> lock(mutex_);
  outcomes_.clear();
  deliveries_.clear();
  commits_ = 0;
  rejections_ = 0;
}

tos::OperationRequest make_request(tos::OperationClassId operation, std::uint64_t payload_bytes,
                                   tos::MemoryDomain source, tos::MemoryDomain destination) {
  tos::OperationRequest request;
  request.operation_class = operation;
  request.payload.size_bytes = payload_bytes;
  request.payload.source_memory = source;
  request.payload.destination_memory = destination;
  request.payload.transport_class = tos::TransportClass::kRawFrames;
  request.payload.payload_class = tos::PayloadClass::kOpaqueBytes;
  request.payload.alignment_bytes = 8;
  request.payload.segment_count = 1;
  request.retry.allowed = true;
  request.retry.max_attempts = 2;
  return request;
}

tos::ExecutionDomainId add_cpu_domain(TestRuntime& runtime, std::uint64_t domain_id) {
  tos::CpuBackend::Options options;
  options.domain_id = tos::ExecutionDomainId(domain_id);
  options.name = "cpu.host." + std::to_string(domain_id);
  options.parent_host = "test.host";
  tos::CpuBackend probe(options);
  const std::vector<tos::ExecutionDomainRecord> discovered = probe.discover_domains();
  if (discovered.empty()) return tos::ExecutionDomainId{};
  const tos::Status registered = runtime.scheduler().register_domain(discovered.front());
  if (!registered) {
    report_failure(__FILE__, __LINE__, "register cpu domain failed: " + registered.code);
    return tos::ExecutionDomainId{};
  }
  const tos::Status capability = runtime.scheduler().publish_capability(discovered.front().capability);
  if (!capability) {
    report_failure(__FILE__, __LINE__, "publish cpu capability failed: " + capability.code);
  }
  const tos::Status load = runtime.scheduler().publish_load(discovered.front().id, discovered.front().load);
  if (!load) {
    report_failure(__FILE__, __LINE__, "publish cpu load failed: " + load.code);
  }
  const tos::Status capacity = runtime.scheduler().publish_capacity(discovered.front().id, discovered.front().capacity);
  if (!capacity) {
    report_failure(__FILE__, __LINE__, "publish cpu capacity failed: " + capacity.code);
  }
  return discovered.front().id;
}

}  // namespace tos_test
// Each test executable is driven by this entry point. Arguments are forwarded so a
// single test can be selected while diagnosing a failure.
int main(int argc, char** argv) {
  // Test executables keep operational logging at info level so that a failing
  // process-level proof leaves a usable trail on standard error.
  tos::log_set_level(tos::LogLevel::kInfo);
  return tos_test::run_all(argc, argv);
}
