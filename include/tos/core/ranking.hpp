// Deterministic ranking model.
//
// Ranking never collapses into an unexplained scalar: every candidate carries a
// named factor vector, and the comparison order is a total order defined by
// integer-exact arithmetic followed by explicit tie-breaks.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#ifndef TOS_CORE_RANKING_HPP
#define TOS_CORE_RANKING_HPP

#include <array>
#include <cstdint>
#include <string_view>

namespace tos {

/// Named ranking factors. Each factor is normalized to [0, 1'000'000] where a larger
/// value always means "more desirable" for the scheduler, including cost factors,
/// which are published as their complement.
enum class RankingFactor : std::uint8_t {
  kDataLocality = 0,
  kMemoryLocality = 1,
  kNicLocality = 2,
  kAcceleratorLocality = 3,
  kPcieAffinity = 4,
  kAvoidedHostCopies = 5,
  kTransferCost = 6,
  kSetupCost = 7,
  kQueueDelay = 8,
  kExecutionThroughput = 9,
  kCompletionLatency = 10,
  kOffloadOverhead = 11,
  kCpuPreservation = 12,
  kAcceleratorPreservation = 13,
  kPowerEfficiency = 14,
  kUtilizationHeadroom = 15,
  kCongestionAvoidance = 16,
  kFailureDomainDiversity = 17,
  kIsolationQuality = 18,
  kReconfigurationPenalty = 19,
  kFallbackCost = 20,
  kPolicyPreference = 21,
};
inline constexpr std::size_t kRankingFactorCount = 22;

[[nodiscard]] std::string_view to_string(RankingFactor value) noexcept;
[[nodiscard]] bool parse_enum(std::string_view text, RankingFactor& out) noexcept;

using FactorVector = std::array<std::int32_t, kRankingFactorCount>;

/// Normalized factor scale.
inline constexpr std::int32_t kFactorScale = 1000000;

[[nodiscard]] std::array<std::int32_t, kRankingFactorCount> default_ranking_weights();

struct RankedCandidate {
  std::uint64_t domain_id{0};
  std::uint8_t domain_type{0};
  std::int64_t weighted_score{0};
  std::int64_t total_weight{0};
  FactorVector factors{};
  std::uint32_t rank{0};
};

/// Compare two ranked candidates. Returns true when a is strictly better.
/// Order: weighted score, then factor-by-factor in enum order, then domain type
/// ordinal, then domain id. This is a total order, so results are reproducible.
[[nodiscard]] bool candidate_better(const RankedCandidate& a, const RankedCandidate& b) noexcept;

/// Compute the integer-exact weighted score of a factor vector under a weight vector.
[[nodiscard]] std::int64_t weighted_score(const FactorVector& factors,
                                          const std::array<std::int32_t, kRankingFactorCount>& weights,
                                          std::int64_t& total_weight_out) noexcept;

}  // namespace tos

#endif  // TOS_CORE_RANKING_HPP
