// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "tos/core/ranking.hpp"

#include <algorithm>

namespace tos {

std::array<std::int32_t, kRankingFactorCount> default_ranking_weights() {
  std::array<std::int32_t, kRankingFactorCount> weights{};
  weights[static_cast<std::size_t>(RankingFactor::kDataLocality)] = 150;
  weights[static_cast<std::size_t>(RankingFactor::kMemoryLocality)] = 100;
  weights[static_cast<std::size_t>(RankingFactor::kNicLocality)] = 60;
  weights[static_cast<std::size_t>(RankingFactor::kAcceleratorLocality)] = 60;
  weights[static_cast<std::size_t>(RankingFactor::kPcieAffinity)] = 40;
  weights[static_cast<std::size_t>(RankingFactor::kAvoidedHostCopies)] = 100;
  weights[static_cast<std::size_t>(RankingFactor::kTransferCost)] = 90;
  weights[static_cast<std::size_t>(RankingFactor::kSetupCost)] = 120;
  weights[static_cast<std::size_t>(RankingFactor::kQueueDelay)] = 50;
  weights[static_cast<std::size_t>(RankingFactor::kExecutionThroughput)] = 70;
  weights[static_cast<std::size_t>(RankingFactor::kCompletionLatency)] = 40;
  weights[static_cast<std::size_t>(RankingFactor::kOffloadOverhead)] = 60;
  weights[static_cast<std::size_t>(RankingFactor::kCpuPreservation)] = 90;
  weights[static_cast<std::size_t>(RankingFactor::kAcceleratorPreservation)] = 30;
  weights[static_cast<std::size_t>(RankingFactor::kPowerEfficiency)] = 20;
  weights[static_cast<std::size_t>(RankingFactor::kUtilizationHeadroom)] = 50;
  weights[static_cast<std::size_t>(RankingFactor::kCongestionAvoidance)] = 40;
  weights[static_cast<std::size_t>(RankingFactor::kFailureDomainDiversity)] = 30;
  weights[static_cast<std::size_t>(RankingFactor::kIsolationQuality)] = 60;
  weights[static_cast<std::size_t>(RankingFactor::kReconfigurationPenalty)] = 40;
  weights[static_cast<std::size_t>(RankingFactor::kFallbackCost)] = 30;
  weights[static_cast<std::size_t>(RankingFactor::kPolicyPreference)] = 200;
  return weights;
}

std::int64_t weighted_score(const FactorVector& factors,
                            const std::array<std::int32_t, kRankingFactorCount>& weights,
                            std::int64_t& total_weight_out) noexcept {
  std::int64_t numerator = 0;
  std::int64_t denominator = 0;
  for (std::size_t i = 0; i < kRankingFactorCount; ++i) {
    const std::int64_t weight = weights[i];
    if (weight <= 0) continue;
    const std::int64_t factor = std::clamp<std::int64_t>(factors[i], 0, kFactorScale);
    numerator += weight * factor;
    denominator += weight;
  }
  total_weight_out = denominator;
  if (denominator == 0) return 0;
  return numerator / denominator;  // integer division: exact, reproducible
}

bool candidate_better(const RankedCandidate& a, const RankedCandidate& b) noexcept {
  if (a.weighted_score != b.weighted_score) return a.weighted_score > b.weighted_score;
  for (std::size_t i = 0; i < kRankingFactorCount; ++i) {
    if (a.factors[i] != b.factors[i]) return a.factors[i] > b.factors[i];
  }
  if (a.domain_type != b.domain_type) return a.domain_type < b.domain_type;
  return a.domain_id < b.domain_id;
}

}  // namespace tos
