// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "tos/util/process.hpp"

#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <io.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace tos {

struct ProcessHandle {
#ifdef _WIN32
  PROCESS_INFORMATION info{};
  HANDLE stdout_read{nullptr};
  HANDLE stderr_read{nullptr};
#else
  int pid{-1};
  int stdout_fd{-1};
  int stderr_fd{-1};
  int status{-1};
#endif
  std::mutex mutex;
  bool reaped{false};
};

namespace {

#ifdef _WIN32

/// Quote one argument per the Windows command-line parsing rules.
void append_quoted(std::wstring& out, const std::wstring& argument) {
  const bool needs_quotes =
      argument.empty() || argument.find_first_of(L" \t\n\v\"") != std::wstring::npos;
  if (!needs_quotes) {
    out += argument;
    return;
  }
  out.push_back(L'"');
  std::size_t backslashes = 0;
  for (wchar_t c : argument) {
    if (c == L'\\') {
      ++backslashes;
      continue;
    }
    if (c == L'"') {
      out.append(backslashes * 2 + 1, L'\\');
      out.push_back(L'"');
      backslashes = 0;
      continue;
    }
    out.append(backslashes, L'\\');
    backslashes = 0;
    out.push_back(c);
  }
  out.append(backslashes * 2, L'\\');
  out.push_back(L'"');
}

std::wstring widen(const std::string& text) {
  if (text.empty()) return std::wstring();
  const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0);
  std::wstring out(static_cast<std::size_t>(size), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size);
  return out;
}

void drain(HANDLE pipe, std::string& out) {
  if (pipe == nullptr) return;
  for (;;) {
    DWORD available = 0;
    if (!::PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) return;
    if (available == 0) return;
    char buffer[4096];
    DWORD read = 0;
    const DWORD want = available > sizeof(buffer) ? static_cast<DWORD>(sizeof(buffer)) : available;
    if (!::ReadFile(pipe, buffer, want, &read, nullptr) || read == 0) return;
    out.append(buffer, static_cast<std::size_t>(read));
  }
}

#else

void drain(int fd, std::string& out) {
  if (fd < 0) return;
  char buffer[4096];
  for (;;) {
    const ssize_t read = ::read(fd, buffer, sizeof(buffer));
    if (read <= 0) return;
    out.append(buffer, static_cast<std::size_t>(read));
  }
}

#endif

}  // namespace

Checked<std::shared_ptr<ProcessHandle>> spawn_process(const ProcessOptions& options) {
  if (options.argv.empty() || options.argv[0].empty()) {
    return Checked<std::shared_ptr<ProcessHandle>>::bad("process.empty_argv");
  }
  auto handle = std::make_shared<ProcessHandle>();
#ifdef _WIN32
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;

  HANDLE stdout_read = nullptr;
  HANDLE stdout_write = nullptr;
  if (options.capture_stdout) {
    if (!::CreatePipe(&stdout_read, &stdout_write, &attributes, 0)) {
      return Checked<std::shared_ptr<ProcessHandle>>::bad("process.pipe_failed");
    }
    ::SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
  }
  HANDLE stderr_write = nullptr;
  HANDLE stderr_read = nullptr;
  if (!options.stderr_path.empty()) {
    stderr_write = ::CreateFileW(widen(options.stderr_path).c_str(), GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (stderr_write == INVALID_HANDLE_VALUE) {
      if (stdout_read != nullptr) ::CloseHandle(stdout_read);
      if (stdout_write != nullptr) ::CloseHandle(stdout_write);
      return Checked<std::shared_ptr<ProcessHandle>>::bad("process.stderr_file_failed");
    }
  } else if (options.capture_stderr) {
    if (!::CreatePipe(&stderr_read, &stderr_write, &attributes, 0)) {
      if (stdout_read != nullptr) ::CloseHandle(stdout_read);
      if (stdout_write != nullptr) ::CloseHandle(stdout_write);
      return Checked<std::shared_ptr<ProcessHandle>>::bad("process.pipe_failed");
    }
    ::SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);
  }

  std::wstring command_line;
  for (const std::string& argument : options.argv) {
    if (!command_line.empty()) command_line.push_back(L' ');
    append_quoted(command_line, widen(argument));
  }

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = stdout_write != nullptr ? stdout_write : ::GetStdHandle(STD_OUTPUT_HANDLE);
  startup.hStdError = stderr_write != nullptr ? stderr_write : ::GetStdHandle(STD_ERROR_HANDLE);

  PROCESS_INFORMATION info{};
  const std::wstring working = widen(options.working_directory);
  const BOOL created = ::CreateProcessW(
      nullptr, command_line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
      working.empty() ? nullptr : working.c_str(), &startup, &info);
  if (stdout_write != nullptr) ::CloseHandle(stdout_write);
  if (stderr_write != nullptr) ::CloseHandle(stderr_write);
  if (!created) {
    if (stdout_read != nullptr) ::CloseHandle(stdout_read);
    if (stderr_read != nullptr) ::CloseHandle(stderr_read);
    return Checked<std::shared_ptr<ProcessHandle>>::bad("process.create_failed",
                                                        std::to_string(::GetLastError()));
  }
  ::CloseHandle(info.hThread);
  handle->info = info;
  handle->stdout_read = stdout_read;
  handle->stderr_read = stderr_read;
#else
  int stdout_pipe[2] = {-1, -1};
  int stderr_pipe[2] = {-1, -1};
  if (options.capture_stdout && ::pipe(stdout_pipe) != 0) {
    return Checked<std::shared_ptr<ProcessHandle>>::bad("process.pipe_failed");
  }
  if (options.capture_stderr && options.stderr_path.empty() && ::pipe(stderr_pipe) != 0) {
    return Checked<std::shared_ptr<ProcessHandle>>::bad("process.pipe_failed");
  }
  std::vector<char*> argv;
  argv.reserve(options.argv.size() + 1);
  for (const std::string& argument : options.argv) {
    argv.push_back(const_cast<char*>(argument.c_str()));
  }
  argv.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  if (options.capture_stdout) {
    posix_spawn_file_actions_adddup2(&actions, stdout_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, stdout_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, stdout_pipe[1]);
  }
  if (!options.stderr_path.empty()) {
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, options.stderr_path.c_str(),
                                     O_WRONLY | O_CREAT | O_TRUNC, 0644);
  } else if (options.capture_stderr) {
    posix_spawn_file_actions_adddup2(&actions, stderr_pipe[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, stderr_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, stderr_pipe[1]);
  }
  pid_t pid = -1;
  const int spawned = ::posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  if (options.capture_stdout) {
    ::close(stdout_pipe[1]);
  }
  if (options.capture_stderr && options.stderr_path.empty()) {
    ::close(stderr_pipe[1]);
  }
  if (spawned != 0) {
    if (options.capture_stdout) ::close(stdout_pipe[0]);
    if (options.capture_stderr && options.stderr_path.empty()) ::close(stderr_pipe[0]);
    return Checked<std::shared_ptr<ProcessHandle>>::bad("process.spawn_failed",
                                                        std::strerror(spawned));
  }
  handle->pid = static_cast<int>(pid);
  handle->stdout_fd = options.capture_stdout ? stdout_pipe[0] : -1;
  handle->stderr_fd =
      (options.capture_stderr && options.stderr_path.empty()) ? stderr_pipe[0] : -1;
#endif
  return Checked<std::shared_ptr<ProcessHandle>>::good(std::move(handle));
}

std::uint64_t process_id(const std::shared_ptr<ProcessHandle>& handle) noexcept {
  if (!handle) return 0;
#ifdef _WIN32
  return static_cast<std::uint64_t>(handle->info.dwProcessId);
#else
  return static_cast<std::uint64_t>(handle->pid);
#endif
}

bool process_alive(const std::shared_ptr<ProcessHandle>& handle) noexcept {
  if (!handle) return false;
#ifdef _WIN32
  return ::WaitForSingleObject(handle->info.hProcess, 0) == WAIT_TIMEOUT;
#else
  int status = 0;
  const pid_t result = ::waitpid(handle->pid, &status, WNOHANG);
  if (result == 0) return true;
  if (result == handle->pid) {
    handle->status = status;
    handle->reaped = true;
  }
  return false;
#endif
}

Status terminate_process(const std::shared_ptr<ProcessHandle>& handle) {
  if (!handle) return Status::failure("process.invalid_handle");
#ifdef _WIN32
  if (!::TerminateProcess(handle->info.hProcess, 1)) {
    return Status::failure("process.terminate_failed", std::to_string(::GetLastError()));
  }
#else
  if (::kill(handle->pid, SIGKILL) != 0) {
    return Status::failure("process.terminate_failed", std::strerror(errno));
  }
#endif
  return Status::success();
}

Checked<int> wait_process(const std::shared_ptr<ProcessHandle>& handle) {
  if (!handle) return Checked<int>::bad("process.invalid_handle");
#ifdef _WIN32
  if (::WaitForSingleObject(handle->info.hProcess, INFINITE) != WAIT_OBJECT_0) {
    return Checked<int>::bad("process.wait_failed");
  }
  DWORD code = 0;
  if (!::GetExitCodeProcess(handle->info.hProcess, &code)) {
    return Checked<int>::bad("process.exit_code_failed");
  }
  return Checked<int>::good(static_cast<int>(code));
#else
  int status = 0;
  for (;;) {
    const pid_t result = ::waitpid(handle->pid, &status, 0);
    if (result == handle->pid) break;
    if (result < 0 && errno == EINTR) continue;
    return Checked<int>::bad("process.wait_failed", std::strerror(errno));
  }
  handle->status = status;
  handle->reaped = true;
  if (WIFEXITED(status)) return Checked<int>::good(WEXITSTATUS(status));
  if (WIFSIGNALED(status)) return Checked<int>::good(128 + WTERMSIG(status));
  return Checked<int>::good(-1);
#endif
}

std::string read_process_stdout(const std::shared_ptr<ProcessHandle>& handle) {
  std::string out;
  if (!handle) return out;
  std::lock_guard<std::mutex> lock(handle->mutex);
#ifdef _WIN32
  drain(handle->stdout_read, out);
#else
  drain(handle->stdout_fd, out);
#endif
  return out;
}

std::string read_process_stderr(const std::shared_ptr<ProcessHandle>& handle) {
  std::string out;
  if (!handle) return out;
  std::lock_guard<std::mutex> lock(handle->mutex);
#ifdef _WIN32
  drain(handle->stderr_read, out);
#else
  drain(handle->stderr_fd, out);
#endif
  return out;
}

void close_process(const std::shared_ptr<ProcessHandle>& handle) noexcept {
  if (!handle) return;
#ifdef _WIN32
  if (handle->stdout_read != nullptr) ::CloseHandle(handle->stdout_read);
  if (handle->stderr_read != nullptr) ::CloseHandle(handle->stderr_read);
  if (handle->info.hProcess != nullptr) ::CloseHandle(handle->info.hProcess);
  handle->stdout_read = nullptr;
  handle->stderr_read = nullptr;
  handle->info.hProcess = nullptr;
#else
  if (handle->stdout_fd >= 0) ::close(handle->stdout_fd);
  if (handle->stderr_fd >= 0) ::close(handle->stderr_fd);
  handle->stdout_fd = -1;
  handle->stderr_fd = -1;
#endif
}

std::string current_executable_path() {
#ifdef _WIN32
  wchar_t buffer[4096] = {0};
  const DWORD written = ::GetModuleFileNameW(nullptr, buffer, 4096);
  if (written == 0) return std::string();
  const int size = ::WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(written), nullptr, 0,
                                         nullptr, nullptr);
  std::string out(static_cast<std::size_t>(size), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(written), out.data(), size, nullptr,
                        nullptr);
  return out;
#else
  char buffer[4096] = {0};
  const ssize_t written = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
  if (written <= 0) return std::string();
  return std::string(buffer, static_cast<std::size_t>(written));
#endif
}

}  // namespace tos
