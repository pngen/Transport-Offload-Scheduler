// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "tos/core/identities.hpp"

#include <cstdio>
#include <cstdlib>

namespace tos {
namespace {

std::string format_unsigned(std::uint64_t value) {
  char buffer[32] = {0};
  std::snprintf(buffer, sizeof(buffer), "0x%016llx", static_cast<unsigned long long>(value));
  return std::string(buffer);
}

}  // namespace

std::string format_id(std::uint64_t value) { return format_unsigned(value); }

std::string format_generation(std::uint64_t value) { return format_unsigned(value); }

bool parse_id(std::string_view text, std::uint64_t& out) noexcept {
  if (text.empty() || text.size() > 20) return false;
  std::string buffer(text);
  char* end = nullptr;
  const int base = (buffer.size() > 2 && buffer[0] == '0' && (buffer[1] == 'x' || buffer[1] == 'X'))
                       ? 16
                       : (buffer[0] == '0' && buffer.size() > 1 ? 8 : 10);
  const unsigned long long value = std::strtoull(buffer.c_str(), &end, base);
  if (end == buffer.c_str() || (end != nullptr && *end != '\0')) return false;
  out = static_cast<std::uint64_t>(value);
  return true;
}

}  // namespace tos
