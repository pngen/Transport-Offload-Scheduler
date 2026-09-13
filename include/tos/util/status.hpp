// Small bounded result/status types used across the public API.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#ifndef TOS_UTIL_STATUS_HPP
#define TOS_UTIL_STATUS_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace tos {

/// Maximum length of any human-readable string carried across the public API.
/// Bounds exist so that untrusted peers cannot force unbounded allocation.
inline constexpr std::size_t kMaxTextLength = 512;
inline constexpr std::size_t kMaxNameLength = 96;
inline constexpr std::size_t kMaxLabelLength = 128;

/// A bounded, ASCII-printable string. Constructed only through validation.
[[nodiscard]] bool is_valid_name(std::string_view text) noexcept;
/// Truncate to a hard bound. Never grows the input.
[[nodiscard]] std::string bounded_text(std::string_view text, std::size_t limit = kMaxTextLength);

struct Status {
  bool ok{true};
  std::string code;    ///< stable machine-readable code, never prose to be parsed
  std::string message; ///< bounded human-readable detail

  [[nodiscard]] static Status success() { return Status{}; }
  [[nodiscard]] static Status failure(std::string code_in, std::string_view message_in = {}) {
    Status s;
    s.ok = false;
    s.code = bounded_text(code_in, kMaxNameLength);
    s.message = bounded_text(message_in);
    return s;
  }
  [[nodiscard]] explicit operator bool() const noexcept { return ok; }
};

template <class T>
struct Checked {
  Status status;
  T value{};

  [[nodiscard]] bool ok() const noexcept { return status.ok; }
  [[nodiscard]] static Checked good(T v) {
    Checked c;
    c.value = std::move(v);
    return c;
  }
  [[nodiscard]] static Checked bad(std::string code, std::string message = {}) {
    Checked c;
    c.status = Status::failure(std::move(code), std::move(message));
    return c;
  }
};

}  // namespace tos

#endif  // TOS_UTIL_STATUS_HPP
