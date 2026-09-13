// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "tos/util/log.hpp"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>

#include "tos/util/status.hpp"

namespace tos {
namespace {

std::atomic<int> g_level{static_cast<int>(LogLevel::kWarn)};
std::mutex g_mutex;

const char* level_token(LogLevel level) noexcept {
  switch (level) {
    case LogLevel::kError: return "error";
    case LogLevel::kWarn: return "warn";
    case LogLevel::kInfo: return "info";
    case LogLevel::kDebug: return "debug";
  }
  return "info";
}

}  // namespace

void log_set_level(LogLevel level) noexcept { g_level.store(static_cast<int>(level)); }

LogLevel log_level() noexcept { return static_cast<LogLevel>(g_level.load()); }

void log_write(LogLevel level, std::string_view component, std::string_view message) {
  if (static_cast<int>(level) > g_level.load()) return;
  const std::string line = std::string(level_token(level)) + " [" + std::string(component) + "] " +
                           bounded_text(message, kMaxTextLength);
  std::lock_guard<std::mutex> lock(g_mutex);
  std::fputs(line.c_str(), stderr);
  std::fputc('\n', stderr);
  std::fflush(stderr);
}

void log_write_stdout(std::string_view line) {
  std::lock_guard<std::mutex> lock(g_mutex);
  std::fwrite(line.data(), 1, line.size(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

}  // namespace tos
