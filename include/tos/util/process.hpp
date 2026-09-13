// Real OS process control used by the distributed proofs.
//
// Killing a worker here is a real process kill, not a simulated flag.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#ifndef TOS_UTIL_PROCESS_HPP
#define TOS_UTIL_PROCESS_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "tos/util/status.hpp"

namespace tos {

struct ProcessOptions {
  std::vector<std::string> argv;  ///< argv[0] is the executable path
  std::string working_directory;
  bool capture_stdout{true};
  bool capture_stderr{true};
  /// When set, the child's standard error is redirected to this file instead of a
  /// pipe. Process-level proofs use it so that a chatty child can never block on a
  /// full pipe while the parent is doing something else.
  std::string stderr_path;
};

struct ProcessHandle;

[[nodiscard]] Checked<std::shared_ptr<ProcessHandle>> spawn_process(const ProcessOptions& options);
[[nodiscard]] bool process_alive(const std::shared_ptr<ProcessHandle>& handle) noexcept;
/// Forceful termination. This is the worker-death scenario, not a timeout.
[[nodiscard]] Status terminate_process(const std::shared_ptr<ProcessHandle>& handle);
/// Wait for natural exit and return the exit code.
[[nodiscard]] Checked<int> wait_process(const std::shared_ptr<ProcessHandle>& handle);
[[nodiscard]] std::uint64_t process_id(const std::shared_ptr<ProcessHandle>& handle) noexcept;
/// Read whatever the process has written so far. Never blocks indefinitely: the
/// stream is read until the pipe would block.
[[nodiscard]] std::string read_process_stdout(const std::shared_ptr<ProcessHandle>& handle);
[[nodiscard]] std::string read_process_stderr(const std::shared_ptr<ProcessHandle>& handle);
/// Close handles. Does not terminate: the caller decides the process's fate.
void close_process(const std::shared_ptr<ProcessHandle>& handle) noexcept;

/// Path of the currently running executable, used to locate sibling binaries in
/// process-level proofs without hardcoding absolute paths.
[[nodiscard]] std::string current_executable_path();

}  // namespace tos

#endif  // TOS_UTIL_PROCESS_HPP
