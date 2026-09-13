// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "tos/util/status.hpp"

#include <algorithm>
#include <cctype>

namespace tos {

bool is_valid_name(std::string_view text) noexcept {
  if (text.empty() || text.size() > kMaxNameLength) return false;
  for (char c : text) {
    const auto uc = static_cast<unsigned char>(c);
    const bool ok = (uc >= 'A' && uc <= 'Z') || (uc >= 'a' && uc <= 'z') ||
                    (uc >= '0' && uc <= '9') || c == '_' || c == '.' || c == '-' || c == ':';
    if (!ok) return false;
  }
  return true;
}

std::string bounded_text(std::string_view text, std::size_t limit) {
  const std::size_t count = std::min(text.size(), limit);
  std::string out(text.substr(0, count));
  for (char& c : out) {
    const auto uc = static_cast<unsigned char>(c);
    if (uc < 0x20 || uc == 0x7F) c = '?';
  }
  return out;
}

}  // namespace tos
