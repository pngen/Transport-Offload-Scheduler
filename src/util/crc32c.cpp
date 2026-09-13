// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "tos/util/crc32c.hpp"

#include <array>

namespace tos {
namespace {

constexpr std::uint32_t kPolynomial = 0x82F63B78U;  // reflected Castagnoli

constexpr std::array<std::uint32_t, 256> make_table() {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t crc = i;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1U) != 0U ? (crc >> 1) ^ kPolynomial : (crc >> 1);
    }
    table[i] = crc;
  }
  return table;
}

constexpr auto kTable = make_table();

}  // namespace

std::uint32_t crc32c_extend(std::uint32_t previous, const void* data, std::size_t length) noexcept {
  const auto* bytes = static_cast<const unsigned char*>(data);
  std::uint32_t crc = ~previous;
  for (std::size_t i = 0; i < length; ++i) {
    crc = kTable[(crc ^ bytes[i]) & 0xFFU] ^ (crc >> 8);
  }
  return ~crc;
}

std::uint32_t crc32c(const void* data, std::size_t length, std::uint32_t seed) noexcept {
  return crc32c_extend(seed, data, length);
}

std::uint64_t fnv1a64_extend(std::uint64_t previous, const void* data, std::size_t length) noexcept {
  const auto* bytes = static_cast<const unsigned char*>(data);
  std::uint64_t hash = previous;
  for (std::size_t i = 0; i < length; ++i) {
    hash ^= static_cast<std::uint64_t>(bytes[i]);
    hash *= 0x100000001B3ULL;
  }
  return hash;
}

std::uint64_t fnv1a64(const void* data, std::size_t length, std::uint64_t seed) noexcept {
  return fnv1a64_extend(seed, data, length);
}

}  // namespace tos
