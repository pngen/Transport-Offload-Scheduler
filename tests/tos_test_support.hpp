// Test support: a deliberately small harness plus the shared runtime fixture.
//
// There are no timeouts anywhere in this suite. A wait that never satisfies its
// predicate is a defect in the runtime, and the suite must hang rather than hide it.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#ifndef TOS_TEST_SUPPORT_HPP
#define TOS_TEST_SUPPORT_HPP

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "tos/tos.hpp"

namespace tos_test {

using TestFunction = void (*)();

void register_test(const char* name, TestFunction function);
int run_all(int argc, char** argv);
void report_failure(const char* file, int line, const std::string& message);
[[nodiscard]] std::string current_test_name();
[[nodiscard]] int failure_count();

/// Deterministic payload used across tests.
[[nodiscard]] std::vector<std::uint8_t> make_payload(std::size_t size, std::uint32_t seed = 1);

/// Spin until a predicate holds. There is deliberately no deadline: a predicate that
/// never becomes true hangs the test, which is the intended failure mode for a
/// broken runtime. Used only for cross-process readiness, where no condition
/// variable can be shared.
void wait_until(const std::function<bool()>& predicate);

/// Directory of the currently running executable, so process-level proofs locate
/// sibling binaries without hardcoded paths.
[[nodiscard]] std::string executable_directory();

/// Spawn a sibling tool (name without extension) with the given arguments.
[[nodiscard]] tos::Checked<std::shared_ptr<tos::ProcessHandle>> spawn_tool(
    const std::string& tool, const std::vector<std::string>& arguments,
    const std::string& stderr_path = std::string());

}  // namespace tos_test

#define TOS_TEST(name)                                                              \
  static void name();                                                               \
  namespace {                                                                       \
  struct name##_registrar {                                                         \
    name##_registrar() { ::tos_test::register_test(#name, &name); }                 \
  } name##_registrar_instance;                                                      \
  }                                                                                 \
  static void name()

#define TOS_CHECK(expression)                                                       \
  do {                                                                              \
    if (!(expression)) {                                                            \
      ::tos_test::report_failure(__FILE__, __LINE__, "CHECK failed: " #expression); \
    }                                                                               \
  } while (false)

#define TOS_CHECK_MSG(expression, message)                                          \
  do {                                                                              \
    if (!(expression)) {                                                            \
      ::tos_test::report_failure(__FILE__, __LINE__,                                \
                                 std::string("CHECK failed: " #expression " | ") +  \
                                     std::string(message));                         \
    }                                                                               \
  } while (false)

#define TOS_CHECK_EQ(actual, expected)                                              \
  do {                                                                              \
    const auto tos_test_actual = (actual);                                          \
    const auto tos_test_expected = (expected);                                      \
    if (!(tos_test_actual == tos_test_expected)) {                                  \
      ::tos_test::report_failure(__FILE__, __LINE__,                                \
                                 std::string("CHECK_EQ failed: " #actual));         \
    }                                                                               \
  } while (false)

#define TOS_REQUIRE(expression)                                                     \
  do {                                                                              \
    if (!(expression)) {                                                            \
      ::tos_test::report_failure(__FILE__, __LINE__, "REQUIRE failed: " #expression); \
      return;                                                                       \
    }                                                                               \
  } while (false)

namespace tos_test {

/// A complete in-process runtime: scheduler, synthetic backend, local dispatch
/// channel and domain publisher wired together exactly as a deployment would be.
class TestRuntime {
 public:
  struct Config {
    tos::SchedulerPolicy policy = tos::make_default_policy();
    tos::SchedulerLimits limits;
    bool enable_persistence = false;
    std::string state_path;
    tos::Provenance local_provenance = tos::Provenance::kSynthetic;
    tos::LocalChannelOptions channel;
  };

  explicit TestRuntime(Config config = {});
  ~TestRuntime();
  TestRuntime(const TestRuntime&) = delete;
  TestRuntime& operator=(const TestRuntime&) = delete;

  [[nodiscard]] tos::Scheduler& scheduler() noexcept { return *scheduler_; }
  [[nodiscard]] tos::SyntheticBackend& backend() noexcept { return *backend_; }
  [[nodiscard]] tos::LocalDispatchChannel& channel() noexcept { return *channel_; }
  [[nodiscard]] tos::LocalDomainPublisher& publisher() noexcept { return *publisher_; }

  /// Add a synthetic domain and publish it through the normal evidence path.
  tos::ExecutionDomainId add_domain(tos::SyntheticDomainConfig config);
  /// Re-publish current evidence for every backend domain.
  tos::Status refresh();

  /// Block until the given number of completions has been delivered for an attempt.
  void wait_for(tos::ExecutionAttemptId attempt, std::size_t expected = 1);
  /// Block until the given number of completions has been delivered in total.
  void wait_for_total(std::size_t expected);
  /// Block until the attempt reaches a terminal state in the ledger.
  tos::ExecutionAttempt wait_for_terminal(tos::ExecutionAttemptId attempt);

  [[nodiscard]] std::size_t deliveries(tos::ExecutionAttemptId attempt) const;
  [[nodiscard]] std::size_t total_deliveries() const;
  [[nodiscard]] std::size_t commit_count() const;
  [[nodiscard]] std::size_t rejection_count() const;
  [[nodiscard]] tos::CompletionOutcome last_outcome() const;
  [[nodiscard]] std::vector<tos::CompletionOutcome> outcomes() const;
  [[nodiscard]] bool attempt_is_terminal(tos::ExecutionAttemptId attempt) const;
  void clear_completions();

 private:
  void on_completion(const tos::CompletionSubmission& submission);

  std::unique_ptr<tos::Scheduler> scheduler_;
  std::shared_ptr<tos::SyntheticBackend> backend_;
  std::shared_ptr<tos::LocalDispatchChannel> channel_;
  std::unique_ptr<tos::LocalDomainPublisher> publisher_;

  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::map<std::uint64_t, std::size_t> deliveries_;
  std::vector<tos::CompletionOutcome> outcomes_;
  std::size_t commits_{0};
  std::size_t rejections_{0};
};

/// Build an operation request with the project's canonical defaults.
[[nodiscard]] tos::OperationRequest make_request(tos::OperationClassId operation,
                                                 std::uint64_t payload_bytes,
                                                 tos::MemoryDomain source = tos::MemoryDomain::kHost,
                                                 tos::MemoryDomain destination = tos::MemoryDomain::kHost);

/// Publish a real host CPU domain backed by the CPU execution backend.
tos::ExecutionDomainId add_cpu_domain(TestRuntime& runtime, std::uint64_t domain_id);

}  // namespace tos_test

#endif  // TOS_TEST_SUPPORT_HPP
