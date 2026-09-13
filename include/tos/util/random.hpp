// Identity allocation and seeded deterministic generators.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#ifndef TOS_UTIL_RANDOM_HPP
#define TOS_UTIL_RANDOM_HPP

#include <cstdint>

namespace tos {

/// Process-unique non-zero 64-bit value suitable for worker boot identities.
/// Mixes a process-wide counter, the process id and the steady clock; never zero.
[[nodiscard]] std::uint64_t process_unique_u64() noexcept;

/// Deterministic splitmix64 stream. Used by seeded property tests so that a
/// failing schedule can be replayed exactly from its printed seed.
class DeterministicRng {
 public:
  explicit DeterministicRng(std::uint64_t seed) noexcept : state_(seed) {}
  [[nodiscard]] std::uint64_t next_u64() noexcept;
  [[nodiscard]] std::uint32_t below(std::uint32_t bound) noexcept;  ///< uniform in [0, bound)
  [[nodiscard]] bool chance(std::uint32_t numerator, std::uint32_t denominator) noexcept;
  [[nodiscard]] std::uint64_t seed() const noexcept { return seed_; }

 private:
  std::uint64_t state_;
  std::uint64_t seed_;
};

}  // namespace tos

#endif  // TOS_UTIL_RANDOM_HPP
