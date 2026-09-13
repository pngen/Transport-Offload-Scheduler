// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "tos/util/bytes.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace tos {
namespace {

constexpr std::array<std::string_view, 22> kReservedNames = {
    "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
    "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};

bool contains_control(std::string_view text) noexcept {
  for (char c : text) {
    const auto uc = static_cast<unsigned char>(c);
    if (uc < 0x20 || uc == 0x7F) return true;
  }
  return false;
}

}  // namespace

// ---- ByteWriter -------------------------------------------------------------

void ByteWriter::u8(std::uint8_t value) { raw(&value, 1); }

void ByteWriter::u16(std::uint16_t value) {
  const std::uint8_t bytes[2] = {static_cast<std::uint8_t>(value & 0xFFU),
                                 static_cast<std::uint8_t>((value >> 8) & 0xFFU)};
  raw(bytes, 2);
}

void ByteWriter::u32(std::uint32_t value) {
  const std::uint8_t bytes[4] = {static_cast<std::uint8_t>(value & 0xFFU),
                                 static_cast<std::uint8_t>((value >> 8) & 0xFFU),
                                 static_cast<std::uint8_t>((value >> 16) & 0xFFU),
                                 static_cast<std::uint8_t>((value >> 24) & 0xFFU)};
  raw(bytes, 4);
}

void ByteWriter::u64(std::uint64_t value) {
  std::uint8_t bytes[8];
  for (int i = 0; i < 8; ++i) bytes[i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFU);
  raw(bytes, 8);
}

void ByteWriter::raw(const void* data, std::size_t length) {
  if (!ok_) return;
  if (length > max_size_ || data_.size() > max_size_ - length) {
    ok_ = false;
    code_ = "buffer.size_limit";
    return;
  }
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  data_.insert(data_.end(), bytes, bytes + length);
}

void ByteWriter::blob(const void* data, std::size_t length) {
  if (length > kMaxBlobBytes) {
    ok_ = false;
    code_ = "buffer.blob_limit";
    return;
  }
  u32(static_cast<std::uint32_t>(length));
  raw(data, length);
}

void ByteWriter::blob(const std::vector<std::uint8_t>& data) {
  blob(data.data(), data.size());
}

void ByteWriter::text(std::string_view value) {
  if (value.size() > kMaxTextLength) {
    ok_ = false;
    code_ = "buffer.text_limit";
    return;
  }
  u32(static_cast<std::uint32_t>(value.size()));
  raw(value.data(), value.size());
}

Status ByteWriter::status() const {
  if (ok_) return Status::success();
  return Status::failure(code_, "encoded buffer exceeded a structural bound");
}

// ---- ByteReader -------------------------------------------------------------

bool ByteReader::fail(std::string_view reason) {
  if (ok_) {
    ok_ = false;
    error_.assign(reason);
  }
  return false;
}

bool ByteReader::ensure(std::size_t length) {
  if (!ok_) return false;
  if (length > size_ - offset_) return fail("buffer.truncated");
  return true;
}

bool ByteReader::u8(std::uint8_t& out) {
  if (!ensure(1)) return false;
  out = data_[offset_++];
  return true;
}

bool ByteReader::u16(std::uint16_t& out) {
  if (!ensure(2)) return false;
  out = static_cast<std::uint16_t>(data_[offset_]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(data_[offset_ + 1]) << 8);
  offset_ += 2;
  return true;
}

bool ByteReader::u32(std::uint32_t& out) {
  if (!ensure(4)) return false;
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(data_[offset_ + i]) << (8 * i);
  offset_ += 4;
  out = value;
  return true;
}

bool ByteReader::u64(std::uint64_t& out) {
  if (!ensure(8)) return false;
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) value |= static_cast<std::uint64_t>(data_[offset_ + i]) << (8 * i);
  offset_ += 8;
  out = value;
  return true;
}

bool ByteReader::raw(void* out, std::size_t length) {
  if (!ensure(length)) return false;
  if (length > 0) std::memcpy(out, data_ + offset_, length);
  offset_ += length;
  return true;
}

bool ByteReader::blob(std::vector<std::uint8_t>& out, std::size_t max_length) {
  std::uint32_t length = 0;
  if (!u32(length)) return false;
  if (static_cast<std::size_t>(length) > max_length) return fail("buffer.blob_limit");
  if (!ensure(length)) return false;
  out.assign(data_ + offset_, data_ + offset_ + length);
  offset_ += length;
  return true;
}

bool ByteReader::text(std::string& out, std::size_t max_length) {
  std::uint32_t length = 0;
  if (!u32(length)) return false;
  if (static_cast<std::size_t>(length) > max_length) return fail("buffer.text_limit");
  if (!ensure(length)) return false;
  out.assign(reinterpret_cast<const char*>(data_ + offset_), length);
  offset_ += length;
  return true;
}

bool ByteReader::skip_zero(std::size_t length) {
  if (!ensure(length)) return false;
  for (std::size_t i = 0; i < length; ++i) {
    if (data_[offset_ + i] != 0) return fail("buffer.nonzero_padding");
  }
  offset_ += length;
  return true;
}

// ---- text helpers -----------------------------------------------------------

std::string to_hex(const void* data, std::size_t length) {
  static constexpr char kDigits[] = "0123456789abcdef";
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  std::string out;
  out.reserve(length * 2);
  for (std::size_t i = 0; i < length; ++i) {
    out.push_back(kDigits[bytes[i] >> 4]);
    out.push_back(kDigits[bytes[i] & 0x0FU]);
  }
  return out;
}

bool from_hex(std::string_view text, std::vector<std::uint8_t>& out) {
  if (text.size() % 2 != 0 || text.size() > kMaxBlobBytes * 2) return false;
  auto value_of = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  out.clear();
  out.reserve(text.size() / 2);
  for (std::size_t i = 0; i < text.size(); i += 2) {
    const int hi = value_of(text[i]);
    const int lo = value_of(text[i + 1]);
    if (hi < 0 || lo < 0) return false;
    out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
  }
  return true;
}

bool is_printable_ascii(std::string_view text, std::size_t limit) noexcept {
  if (text.size() > limit) return false;
  for (char c : text) {
    const auto uc = static_cast<unsigned char>(c);
    if (uc < 0x20 || uc > 0x7E) return false;
  }
  return true;
}

bool path_has_traversal(std::string_view path) noexcept {
  std::size_t start = 0;
  while (start <= path.size()) {
    const std::size_t end = path.find_first_of("/\\", start);
    const std::string_view component =
        path.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
    if (component == "..") return true;
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return false;
}

bool is_safe_relative_path(std::string_view path) noexcept {
  if (path.empty() || path.size() > 512) return false;
  if (contains_control(path)) return false;
  if (path.front() == '/' || path.front() == '\\') return false;
  if (path.size() >= 2 && path[1] == ':') return false;  // drive-relative or absolute
  if (path.find('*') != std::string_view::npos || path.find('?') != std::string_view::npos) {
    return false;
  }
  if (path_has_traversal(path)) return false;
  std::size_t start = 0;
  while (start <= path.size()) {
    const std::size_t end = path.find_first_of("/\\", start);
    const std::string_view component =
        path.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
    if (!component.empty()) {
      std::string upper(component);
      for (char& c : upper) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
      }
      // Strip a trailing extension for the reserved-name comparison (NUL.txt is NUL).
      const std::size_t dot = upper.find('.');
      const std::string_view stem = dot == std::string::npos ? std::string_view(upper)
                                                             : std::string_view(upper).substr(0, dot);
      for (std::string_view reserved : kReservedNames) {
        if (stem == reserved) return false;
      }
      if (component.back() == '.' || component.back() == ' ') return false;
    }
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return true;
}

// ---- files ------------------------------------------------------------------

namespace {

std::filesystem::path to_path(const std::string& text) {
#ifdef _WIN32
  return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.c_str())));
#else
  return std::filesystem::path(text);
#endif
}

}  // namespace

bool file_exists(const std::string& path) noexcept {
  std::error_code ec;
  return std::filesystem::exists(to_path(path), ec);
}

std::uint64_t file_size_bytes(const std::string& path) noexcept {
  std::error_code ec;
  const auto size = std::filesystem::file_size(to_path(path), ec);
  if (ec) return 0;
  return static_cast<std::uint64_t>(size);
}

Checked<std::vector<std::uint8_t>> read_file_bytes(const std::string& path, std::size_t max_bytes) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(to_path(path), ec);
  if (ec) return Checked<std::vector<std::uint8_t>>::bad("file.missing", path);
  if (size > max_bytes) return Checked<std::vector<std::uint8_t>>::bad("file.too_large", path);
  std::ifstream stream(to_path(path), std::ios::binary);
  if (!stream) return Checked<std::vector<std::uint8_t>>::bad("file.open_failed", path);
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  if (size > 0) {
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    if (stream.gcount() != static_cast<std::streamsize>(size)) {
      return Checked<std::vector<std::uint8_t>>::bad("file.truncated", path);
    }
  }
  return Checked<std::vector<std::uint8_t>>::good(std::move(bytes));
}

Status write_file_atomic(const std::string& path, const std::vector<std::uint8_t>& bytes) {
  const std::filesystem::path target = to_path(path);
  if (target.has_parent_path()) {
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
  }
  std::filesystem::path temporary = target;
  temporary += ".tmp";
  {
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) return Status::failure("file.open_failed", path);
    if (!bytes.empty()) {
      stream.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    stream.flush();
    if (!stream) return Status::failure("file.write_failed", path);
  }
#ifdef _WIN32
  if (!::MoveFileExW(temporary.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    std::error_code ec;
    std::filesystem::remove(temporary, ec);
    return Status::failure("file.replace_failed", path);
  }
#else
  std::error_code ec;
  std::filesystem::rename(temporary, target, ec);
  if (ec) {
    std::filesystem::remove(temporary, ec);
    return Status::failure("file.replace_failed", path);
  }
#endif
  return Status::success();
}

Status remove_file(const std::string& path) {
  std::error_code ec;
  std::filesystem::remove(to_path(path), ec);
  if (ec) return Status::failure("file.remove_failed", path);
  return Status::success();
}

}  // namespace tos
