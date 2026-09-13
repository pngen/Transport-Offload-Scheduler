// Bounded, canonical byte encoding and small filesystem helpers.
//
// Every decoder in the runtime is written against these primitives so that bounds,
// truncation and traversal defences are implemented once.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#ifndef TOS_UTIL_BYTES_HPP
#define TOS_UTIL_BYTES_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "tos/util/status.hpp"

namespace tos {

inline constexpr std::size_t kMaxBlobBytes = 64 * 1024 * 1024;

class ByteWriter {
 public:
  explicit ByteWriter(std::size_t max_size = kMaxBlobBytes) : max_size_(max_size) {}

  void u8(std::uint8_t value);
  void u16(std::uint16_t value);
  void u32(std::uint32_t value);
  void u64(std::uint64_t value);
  void raw(const void* data, std::size_t length);
  /// Length-prefixed byte blob. Rejected when it would exceed the writer bound.
  void blob(const void* data, std::size_t length);
  void blob(const std::vector<std::uint8_t>& data);
  /// Length-prefixed text, bounded by kMaxTextLength.
  void text(std::string_view value);

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] Status status() const;
  [[nodiscard]] const std::vector<std::uint8_t>& data() const noexcept { return data_; }
  [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }
  [[nodiscard]] std::vector<std::uint8_t> take() { return std::move(data_); }

 private:
  std::vector<std::uint8_t> data_;
  std::size_t max_size_;
  bool ok_{true};
  std::string code_{"ok"};
};

class ByteReader {
 public:
  ByteReader(const std::uint8_t* data, std::size_t size) noexcept : data_(data), size_(size) {}
  explicit ByteReader(const std::vector<std::uint8_t>& data) noexcept
      : data_(data.data()), size_(data.size()) {}

  [[nodiscard]] bool u8(std::uint8_t& out);
  [[nodiscard]] bool u16(std::uint16_t& out);
  [[nodiscard]] bool u32(std::uint32_t& out);
  [[nodiscard]] bool u64(std::uint64_t& out);
  [[nodiscard]] bool raw(void* out, std::size_t length);
  [[nodiscard]] bool blob(std::vector<std::uint8_t>& out, std::size_t max_length = kMaxBlobBytes);
  [[nodiscard]] bool text(std::string& out, std::size_t max_length = kMaxTextLength);

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] bool at_end() const noexcept { return offset_ == size_; }
  [[nodiscard]] std::size_t remaining() const noexcept { return size_ - offset_; }
  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

  /// Skip padding bytes, verifying that they are zero.
  [[nodiscard]] bool skip_zero(std::size_t length);

  /// Record a structural rejection from a decoder and return false, so that a
  /// validation failure propagates with a reason like any decoding failure.
  [[nodiscard]] bool reject(std::string_view reason) { return fail(reason); }

 private:
  [[nodiscard]] bool fail(std::string_view reason);
  [[nodiscard]] bool ensure(std::size_t length);

  const std::uint8_t* data_{nullptr};
  std::size_t size_{0};
  std::size_t offset_{0};
  bool ok_{true};
  std::string error_;
};

[[nodiscard]] std::string to_hex(const void* data, std::size_t length);
[[nodiscard]] bool from_hex(std::string_view text, std::vector<std::uint8_t>& out);

/// True when a string is printable ASCII of bounded length. Metadata that fails
/// this check is rejected rather than sanitized.
[[nodiscard]] bool is_printable_ascii(std::string_view text, std::size_t limit) noexcept;

/// Reject absolute paths, drive-relative paths, traversal components and reserved
/// device names. Used before any persistence path is opened.
[[nodiscard]] bool is_safe_relative_path(std::string_view path) noexcept;
[[nodiscard]] bool path_has_traversal(std::string_view path) noexcept;

[[nodiscard]] Checked<std::vector<std::uint8_t>> read_file_bytes(const std::string& path,
                                                                 std::size_t max_bytes);
/// Write bytes to a temporary sibling file, flush, then atomically replace the
/// destination. An interrupted write can never leave a partially applied file.
[[nodiscard]] Status write_file_atomic(const std::string& path,
                                       const std::vector<std::uint8_t>& bytes);
[[nodiscard]] Status remove_file(const std::string& path);
[[nodiscard]] bool file_exists(const std::string& path) noexcept;
[[nodiscard]] std::uint64_t file_size_bytes(const std::string& path) noexcept;

}  // namespace tos

#endif  // TOS_UTIL_BYTES_HPP
