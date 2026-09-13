// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "tos/core/planner.hpp"

#include <algorithm>
#include <string>

namespace tos {
namespace {

/// Memory domain a domain class addresses natively, without a staging copy.
MemoryDomain native_memory(ExecutionDomainType type) noexcept {
  switch (type) {
    case ExecutionDomainType::kCpu: return MemoryDomain::kHost;
    case ExecutionDomainType::kAccelerator: return MemoryDomain::kDeviceLocal;
    case ExecutionDomainType::kNic: return MemoryDomain::kNicOnboard;
    case ExecutionDomainType::kSmartNic: return MemoryDomain::kSmartNicOnboard;
    case ExecutionDomainType::kDpu: return MemoryDomain::kDpuOnboard;
    case ExecutionDomainType::kOtherRegisteredOffloadEngine: return MemoryDomain::kHost;
  }
  return MemoryDomain::kUnknown;
}

std::int32_t locality_score(LocalityClass value) noexcept {
  switch (value) {
    case LocalityClass::kDomainLocal: return 1000000;
    case LocalityClass::kSameNumaNode: return 900000;
    case LocalityClass::kSameRootComplex: return 800000;
    case LocalityClass::kAcceleratorLocalNic: return 750000;
    case LocalityClass::kSamePcieSwitch: return 700000;
    case LocalityClass::kSameNic: return 700000;
    case LocalityClass::kSameHost: return 500000;
    case LocalityClass::kRemoteHost: return 200000;
    case LocalityClass::kUnknown: return 300000;
  }
  return 300000;
}

std::int32_t throughput_rank(ExecutionDomainType type) noexcept {
  switch (type) {
    case ExecutionDomainType::kCpu: return 400000;
    case ExecutionDomainType::kAccelerator: return 1000000;
    case ExecutionDomainType::kNic: return 700000;
    case ExecutionDomainType::kSmartNic: return 850000;
    case ExecutionDomainType::kDpu: return 900000;
    case ExecutionDomainType::kOtherRegisteredOffloadEngine: return 600000;
  }
  return 500000;
}

std::int32_t power_rank(ExecutionDomainType type) noexcept {
  switch (type) {
    case ExecutionDomainType::kCpu: return 500000;
    case ExecutionDomainType::kAccelerator: return 300000;
    case ExecutionDomainType::kNic: return 350000;
    case ExecutionDomainType::kSmartNic: return 400000;
    case ExecutionDomainType::kDpu: return 700000;
    case ExecutionDomainType::kOtherRegisteredOffloadEngine: return 400000;
  }
  return 400000;
}

std::int32_t isolation_rank(IsolationClass value) noexcept {
  const auto ordinal = static_cast<std::int32_t>(value);
  const std::int32_t max = static_cast<std::int32_t>(IsolationClass::kDedicatedDeviceService);
  if (ordinal <= 0 || max <= 0) return 0;
  return (ordinal * kFactorScale) / max;
}

std::int32_t clamp_factor(std::int64_t value) noexcept {
  if (value < 0) return 0;
  if (value > kFactorScale) return kFactorScale;
  return static_cast<std::int32_t>(value);
}

}  // namespace

std::int32_t normalize_lower_is_better(std::uint64_t value, std::uint64_t worst) noexcept {
  if (worst == 0) return kFactorScale;
  const std::uint64_t bounded = std::min(value, worst);
  return clamp_factor(static_cast<std::int64_t>(kFactorScale) -
                      static_cast<std::int64_t>((bounded * static_cast<std::uint64_t>(kFactorScale)) / worst));
}

std::int32_t normalize_higher_is_better(std::uint64_t value, std::uint64_t best) noexcept {
  if (best == 0) return 0;
  const std::uint64_t bounded = std::min(value, best);
  return clamp_factor(static_cast<std::int64_t>(
      (bounded * static_cast<std::uint64_t>(kFactorScale)) / best));
}

FactorVector compute_factors(const FactorInputs& inputs) {
  FactorVector factors{};
  const ExecutionDomainRecord& domain = *inputs.domain;
  const CapabilitySet& capability = *inputs.capability;
  const OperationRequest& request = *inputs.request;
  const SchedulerPolicy& policy = *inputs.policy;
  const auto index = static_cast<std::size_t>(domain.type);
  const MemoryDomain native = native_memory(domain.type);
  const std::uint64_t payload_bytes = request.payload.size_bytes;

  const bool source_native = request.payload.source_memory == native;
  const bool destination_native = request.payload.destination_memory == native;
  const std::uint32_t copies =
      (source_native ? 0U : 1U) + (destination_native ? 0U : 1U);

  const std::uint64_t setup_cost_units =
      policy.type_setup_cost_units[index] +
      (request.economics.has_value() ? request.economics->setup_cost_units : 0ULL);
  const std::uint64_t per_byte_micro =
      policy.type_per_byte_cost_micro_units[index] +
      (request.economics.has_value() ? request.economics->per_byte_cost_micro_units : 0ULL);
  const std::uint64_t transfer_bytes = source_native ? 0ULL : payload_bytes;
  const std::uint64_t transfer_cost_units =
      (transfer_bytes / 1000000ULL) * per_byte_micro +
      ((transfer_bytes % 1000000ULL) * per_byte_micro) / 1000000ULL;

  factors[static_cast<std::size_t>(RankingFactor::kDataLocality)] =
      source_native ? kFactorScale : locality_score(domain.locality.class_to_payload);
  factors[static_cast<std::size_t>(RankingFactor::kMemoryLocality)] =
      destination_native ? kFactorScale : locality_score(domain.locality.class_to_payload);

  const bool nic_like = domain.type == ExecutionDomainType::kNic ||
                        domain.type == ExecutionDomainType::kSmartNic ||
                        domain.type == ExecutionDomainType::kDpu;
  factors[static_cast<std::size_t>(RankingFactor::kNicLocality)] =
      nic_like ? kFactorScale
               : (domain.locality.has_local_nic ? 900000
                                                : (domain.type == ExecutionDomainType::kCpu ? 500000 : 400000));
  factors[static_cast<std::size_t>(RankingFactor::kAcceleratorLocality)] = [&] {
    switch (domain.type) {
      case ExecutionDomainType::kAccelerator:
        return request.payload.source_memory == MemoryDomain::kDeviceLocal ||
                       request.payload.source_memory == MemoryDomain::kPeerDevice
                   ? kFactorScale
                   : 600000;
      case ExecutionDomainType::kDpu:
      case ExecutionDomainType::kSmartNic: return 800000;
      case ExecutionDomainType::kCpu: return 400000;
      default: return 500000;
    }
  }();

  factors[static_cast<std::size_t>(RankingFactor::kPcieAffinity)] = [&] {
    switch (domain.locality.class_to_payload) {
      case LocalityClass::kDomainLocal: return kFactorScale;
      case LocalityClass::kSameNumaNode: return 900000;
      case LocalityClass::kSamePcieSwitch: return 800000;
      case LocalityClass::kSameRootComplex: return 800000;
      case LocalityClass::kSameNic: return 800000;
      case LocalityClass::kAcceleratorLocalNic: return 800000;
      case LocalityClass::kSameHost: return 500000;
      case LocalityClass::kRemoteHost: return 200000;
      case LocalityClass::kUnknown: return 400000;
    }
    return 400000;
  }();

  factors[static_cast<std::size_t>(RankingFactor::kAvoidedHostCopies)] =
      clamp_factor(kFactorScale - static_cast<std::int64_t>(copies) * 400000);
  factors[static_cast<std::size_t>(RankingFactor::kTransferCost)] =
      normalize_lower_is_better(transfer_cost_units,
                                std::max<std::uint64_t>(1, transfer_cost_units + setup_cost_units));
  factors[static_cast<std::size_t>(RankingFactor::kSetupCost)] =
      normalize_lower_is_better(setup_cost_units,
                                std::max<std::uint64_t>(1, inputs.total_setup_cost_units));

  if (!domain.load.published()) {
    factors[static_cast<std::size_t>(RankingFactor::kQueueDelay)] = 500000;
  } else if (capability.queue_capacity == 0) {
    factors[static_cast<std::size_t>(RankingFactor::kQueueDelay)] = 800000;
  } else {
    const std::uint64_t bounded = std::min<std::uint64_t>(domain.load.queue_depth,
                                                          capability.queue_capacity);
    factors[static_cast<std::size_t>(RankingFactor::kQueueDelay)] = clamp_factor(
        kFactorScale - static_cast<std::int64_t>((bounded * static_cast<std::uint64_t>(kFactorScale)) /
                                                 capability.queue_capacity));
  }

  const std::uint32_t utilization =
      domain.load.published() ? std::min<std::uint32_t>(domain.load.utilization_percent, 100U) : 100U;
  factors[static_cast<std::size_t>(RankingFactor::kExecutionThroughput)] = clamp_factor(
      (static_cast<std::int64_t>(throughput_rank(domain.type)) * (100 - static_cast<std::int64_t>(utilization))) /
      100);

  const std::uint64_t latency_ns = domain.load.published() ? domain.load.observed_latency_ns : 0ULL;
  if (latency_ns == 0) {
    factors[static_cast<std::size_t>(RankingFactor::kCompletionLatency)] = 600000;
  } else if (request.latency_slo_ns.has_value() && request.latency_slo_ns.value() > 0) {
    const std::uint64_t slo = request.latency_slo_ns.value();
    factors[static_cast<std::size_t>(RankingFactor::kCompletionLatency)] = static_cast<std::int32_t>(
        std::min<std::uint64_t>(kFactorScale, (slo * static_cast<std::uint64_t>(kFactorScale)) / latency_ns));
  } else {
    constexpr std::uint64_t kTenMilliseconds = 10ULL * 1000ULL * 1000ULL;
    const std::uint64_t bounded = std::min(latency_ns, kTenMilliseconds);
    factors[static_cast<std::size_t>(RankingFactor::kCompletionLatency)] = clamp_factor(
        kFactorScale - static_cast<std::int64_t>((bounded * static_cast<std::uint64_t>(kFactorScale)) /
                                                 kTenMilliseconds));
  }

  factors[static_cast<std::size_t>(RankingFactor::kOffloadOverhead)] =
      domain.is_offload_engine() ? 800000 : 300000;
  factors[static_cast<std::size_t>(RankingFactor::kCpuPreservation)] =
      domain.type == ExecutionDomainType::kCpu ? 0 : kFactorScale;
  factors[static_cast<std::size_t>(RankingFactor::kAcceleratorPreservation)] =
      domain.type == ExecutionDomainType::kAccelerator ? 0 : kFactorScale;
  factors[static_cast<std::size_t>(RankingFactor::kPowerEfficiency)] = power_rank(domain.type);
  factors[static_cast<std::size_t>(RankingFactor::kUtilizationHeadroom)] = clamp_factor(
      (static_cast<std::int64_t>(100 - static_cast<std::int64_t>(utilization)) * kFactorScale) / 100);
  const std::uint32_t congestion =
      domain.load.published() ? std::min<std::uint32_t>(domain.load.congestion_percent, 100U) : 100U;
  factors[static_cast<std::size_t>(RankingFactor::kCongestionAvoidance)] = clamp_factor(
      (static_cast<std::int64_t>(100 - static_cast<std::int64_t>(congestion)) * kFactorScale) / 100);

  if (inputs.previous == nullptr) {
    factors[static_cast<std::size_t>(RankingFactor::kFailureDomainDiversity)] = kFactorScale;
  } else if (inputs.previous->worker_boot != domain.worker_boot) {
    factors[static_cast<std::size_t>(RankingFactor::kFailureDomainDiversity)] = kFactorScale;
  } else if (inputs.previous->parent_host != domain.parent_host) {
    factors[static_cast<std::size_t>(RankingFactor::kFailureDomainDiversity)] = 800000;
  } else {
    factors[static_cast<std::size_t>(RankingFactor::kFailureDomainDiversity)] = 200000;
  }

  factors[static_cast<std::size_t>(RankingFactor::kIsolationQuality)] = isolation_rank(domain.isolation);
  factors[static_cast<std::size_t>(RankingFactor::kReconfigurationPenalty)] =
      source_native ? kFactorScale : factors[static_cast<std::size_t>(RankingFactor::kSetupCost)];

  const std::uint32_t max_depth = std::max<std::uint32_t>(1, policy.fallback.max_depth);
  factors[static_cast<std::size_t>(RankingFactor::kFallbackCost)] = clamp_factor(
      kFactorScale - static_cast<std::int64_t>(inputs.fallback_depth) * (kFactorScale / max_depth));

  std::int32_t preference = 400000;
  const std::vector<ExecutionDomainType>& order = policy.preference_order;
  const auto position = std::find(order.begin(), order.end(), domain.type);
  if (position != order.end()) {
    const std::size_t index_in_order = static_cast<std::size_t>(std::distance(order.begin(), position));
    const std::size_t count = std::max<std::size_t>(1, order.size());
    const std::int32_t step = static_cast<std::int32_t>(800000 / count);
    preference = clamp_factor(kFactorScale - static_cast<std::int64_t>(index_in_order) * step);
  }
  switch (policy.offload_requirement) {
    case OffloadRequirement::kPreferOffload:
      preference = domain.is_offload_engine() ? std::max<std::int32_t>(preference, 900000)
                                              : std::min<std::int32_t>(preference, 200000);
      break;
    case OffloadRequirement::kOffloadRequired:
      preference = domain.is_offload_engine() ? std::max<std::int32_t>(preference, 900000) : 0;
      break;
    case OffloadRequirement::kHostRequired:
      preference = domain.type == ExecutionDomainType::kCpu ? kFactorScale : 0;
      break;
    case OffloadRequirement::kAny: break;
  }
  factors[static_cast<std::size_t>(RankingFactor::kPolicyPreference)] = preference;
  return factors;
}

std::string describe_factors(const FactorVector& factors) {
  std::string out;
  for (std::size_t i = 0; i < kRankingFactorCount; ++i) {
    if (i > 0) out.push_back(' ');
    out += std::string(to_string(static_cast<RankingFactor>(i)));
    out.push_back('=');
    out += std::to_string(factors[i]);
  }
  return out;
}

}  // namespace tos
