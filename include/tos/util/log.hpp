// Minimal leveled logger. Writes to stderr only.
// No telemetry, no analytics, no network, no hidden reporting.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#ifndef TOS_UTIL_LOG_HPP
#define TOS_UTIL_LOG_HPP

#include <string>
#include <string_view>

namespace tos {

enum class LogLevel : int { kError = 0, kWarn = 1, kInfo = 2, kDebug = 3 };

void log_set_level(LogLevel level) noexcept;
[[nodiscard]] LogLevel log_level() noexcept;
/// Emit one line. The component name must be a static literal; the message is
/// truncated to kMaxTextLength so that untrusted input cannot flood the log.
void log_write(LogLevel level, std::string_view component, std::string_view message);
/// Emit to stdout instead of stderr. Used by process-level proofs that need a
/// machine-readable readiness line while diagnostics stay on stderr.
void log_write_stdout(std::string_view line);

}  // namespace tos

#endif  // TOS_UTIL_LOG_HPP
