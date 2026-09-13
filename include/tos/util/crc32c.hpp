// CRC32C (Castagnoli) used for frame integrity, persistence integrity and the
// CHECKSUM_CRC32C transport operation. Software implementation: identical results
// on every platform, no hardware dependency.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#ifndef TOS_UTIL_CRC32C_HPP
#define TOS_UTIL_CRC32C_HPP

#include <cstddef>
#include <cstdint>

namespace tos {

[[nodiscard]] std::uint32_t crc32c(const void* data, std::size_t length,
                                   std::uint32_t seed = 0U) noexcept;
[[nodiscard]] std::uint32_t crc32c_extend(std::uint32_t previous, const void* data,
                                          std::size_t length) noexcept;

/// 64-bit digest used for cheap result verification. FNV-1a, endian independent.
[[nodiscard]] std::uint64_t fnv1a64(const void* data, std::size_t length,
                                    std::uint64_t seed = 0xCBF29CE484222325ULL) noexcept;
[[nodiscard]] std::uint64_t fnv1a64_extend(std::uint64_t previous, const void* data,
                                           std::size_t length) noexcept;

}  // namespace tos

#endif  // TOS_UTIL_CRC32C_HPP
