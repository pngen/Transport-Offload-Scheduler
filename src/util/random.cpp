// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "tos/util/random.hpp"

#include <atomic>
#include <chrono>
#include <random>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace tos {
namespace {

std::atomic<std::uint64_t> g_counter{0};

std::uint64_t mix(std::uint64_t x) noexcept {
  x += 0x9E3779B97F4A7C15ULL;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
  return x ^ (x >> 31);
}

std::uint32_t current_pid() noexcept {
#ifdef _WIN32
  return static_cast<std::uint32_t>(::_getpid());
#else
  return static_cast<std::uint32_t>(::getpid());
#endif
}

}  // namespace

std::uint64_t process_unique_u64() noexcept {
  const auto counter = g_counter.fetch_add(1, std::memory_order_relaxed) + 1;
  const auto clock = static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  const std::uint64_t entropy = static_cast<std::uint64_t>(std::random_device{}());
  std::uint64_t value = mix(counter ^ (static_cast<std::uint64_t>(current_pid()) << 32));
  value ^= mix(clock);
  value ^= mix(entropy);
  value = mix(value);
  return value == 0 ? 1U : value;
}

std::uint64_t DeterministicRng::next_u64() noexcept {
  state_ += 0x9E3779B97F4A7C15ULL;
  std::uint64_t z = state_;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

std::uint32_t DeterministicRng::below(std::uint32_t bound) noexcept {
  if (bound == 0) return 0;
  return static_cast<std::uint32_t>(next_u64() % static_cast<std::uint64_t>(bound));
}

bool DeterministicRng::chance(std::uint32_t numerator, std::uint32_t denominator) noexcept {
  if (denominator == 0) return false;
  return below(denominator) < numerator;
}

}  // namespace tos
