// Strong identities and generation counters.
//
// Every generation in this header corresponds to a real invalidation, authority,
// compatibility or lifecycle boundary. Decorative generations are not permitted:
// if a counter does not gate a decision or reject stale state, it does not belong here.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#ifndef TOS_CORE_IDENTITIES_HPP
#define TOS_CORE_IDENTITIES_HPP

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace tos {

/// A strongly typed 64-bit identity. Zero is reserved to mean "unset".
template <class Tag>
class StrongId {
 public:
  using tag_type = Tag;

  constexpr StrongId() noexcept = default;
  constexpr explicit StrongId(std::uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }
  constexpr void reset() noexcept { value_ = 0; }

  [[nodiscard]] friend constexpr bool operator==(const StrongId&, const StrongId&) noexcept = default;
  [[nodiscard]] friend constexpr auto operator<=>(const StrongId&, const StrongId&) noexcept = default;

 private:
  std::uint64_t value_{0};
};

template <class Tag>
struct StrongIdHash {
  [[nodiscard]] std::size_t operator()(const StrongId<Tag>& id) const noexcept {
    // Splitmix64 finalizer: stable, no dependency on std::hash implementation.
    std::uint64_t x = id.value() + 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    x = x ^ (x >> 31);
    return static_cast<std::size_t>(x);
  }
};

/// Identity tags. Distinct types, therefore distinct overload sets.
struct TransportOperationIdTag {};
struct OperationClassIdTag {};
struct ExecutionDomainIdTag {};
struct ExecutionAttemptIdTag {};
struct ReservationIdTag {};
struct DispatchIdTag {};
struct WorkerIdTag {};
struct WorkerBootIdTag {};
struct CompletionIdTag {};

using TransportOperationId = StrongId<TransportOperationIdTag>;
using OperationClassId = StrongId<OperationClassIdTag>;
using ExecutionDomainId = StrongId<ExecutionDomainIdTag>;
using ExecutionAttemptId = StrongId<ExecutionAttemptIdTag>;
using ReservationId = StrongId<ReservationIdTag>;
using DispatchId = StrongId<DispatchIdTag>;
using WorkerId = StrongId<WorkerIdTag>;
using WorkerBootId = StrongId<WorkerBootIdTag>;
using CompletionId = StrongId<CompletionIdTag>;

/// A monotonically increasing counter bound to a specific freshness/authority boundary.
/// Generation values start at 1 when a boundary is first published; 0 means "never published".
template <class Tag>
class Generation {
 public:
  using tag_type = Tag;

  constexpr Generation() noexcept = default;
  constexpr explicit Generation(std::uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] static constexpr Generation first() noexcept { return Generation(1); }
  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool published() const noexcept { return value_ != 0; }

  /// Advance to the next generation. Saturates instead of wrapping: a wrapping
  /// generation could re-authorize stale state, which is never acceptable.
  [[nodiscard]] Generation next() const noexcept {
    if (value_ == kSaturated) return *this;
    return Generation(value_ + 1);
  }

  [[nodiscard]] static constexpr std::uint64_t saturated() noexcept { return kSaturated; }

  [[nodiscard]] friend constexpr bool operator==(const Generation&, const Generation&) noexcept = default;
  [[nodiscard]] friend constexpr auto operator<=>(const Generation&, const Generation&) noexcept = default;

 private:
  static constexpr std::uint64_t kSaturated = 0xFFFFFFFFFFFFFFFFULL;
  std::uint64_t value_{0};
};

template <class Tag>
struct GenerationHash {
  [[nodiscard]] std::size_t operator()(const Generation<Tag>& g) const noexcept {
    return StrongIdHash<Tag>{}(StrongId<Tag>(g.value()));
  }
};

/// Result of comparing an observed generation with a bound generation.
enum class GenerationRelation : std::uint8_t {
  kEqual = 0,     ///< observed == bound: authority still valid
  kAdvanced = 1,  ///< observed > bound: evidence moved forward, revalidation required
  kRegressed = 2, ///< observed < bound: rollback or corrupt state, must be rejected
  kUnset = 3,     ///< either side never published: not authoritative
};

[[nodiscard]] constexpr GenerationRelation relate_generations(std::uint64_t bound,
                                                              std::uint64_t observed) noexcept {
  if (bound == 0 || observed == 0) return GenerationRelation::kUnset;
  if (observed == bound) return GenerationRelation::kEqual;
  if (observed > bound) return GenerationRelation::kAdvanced;
  return GenerationRelation::kRegressed;
}

struct ExecutionDomainGenerationTag {};
struct ExecutionAttemptGenerationTag {};
struct CapabilityGenerationTag {};
struct TopologyGenerationTag {};
struct LocalityGenerationTag {};
struct HealthGenerationTag {};
struct QueueGenerationTag {};
struct LoadGenerationTag {};
struct PolicyGenerationTag {};
struct CompatibilityGenerationTag {};
struct IsolationPolicyGenerationTag {};
struct CoordinatorEpochTag {};
struct EvidenceGenerationTag {};
struct SnapshotGenerationTag {};
struct BackendGenerationTag {};
struct PersistenceGenerationTag {};

using ExecutionDomainGeneration = Generation<ExecutionDomainGenerationTag>;
using ExecutionAttemptGeneration = Generation<ExecutionAttemptGenerationTag>;
using CapabilityGeneration = Generation<CapabilityGenerationTag>;
using TopologyGeneration = Generation<TopologyGenerationTag>;
using LocalityGeneration = Generation<LocalityGenerationTag>;
using HealthGeneration = Generation<HealthGenerationTag>;
using QueueGeneration = Generation<QueueGenerationTag>;
using LoadGeneration = Generation<LoadGenerationTag>;
using PolicyGeneration = Generation<PolicyGenerationTag>;
using CompatibilityGeneration = Generation<CompatibilityGenerationTag>;
using IsolationPolicyGeneration = Generation<IsolationPolicyGenerationTag>;
using CoordinatorEpoch = Generation<CoordinatorEpochTag>;
using EvidenceGeneration = Generation<EvidenceGenerationTag>;
using SnapshotGeneration = Generation<SnapshotGenerationTag>;
using BackendGeneration = Generation<BackendGenerationTag>;
using PersistenceGeneration = Generation<PersistenceGenerationTag>;

/// Render an identity as a fixed-width, sortable hex string ("0x%016llx").
[[nodiscard]] std::string format_id(std::uint64_t value);
[[nodiscard]] std::string format_generation(std::uint64_t value);
[[nodiscard]] bool parse_id(std::string_view text, std::uint64_t& out) noexcept;

template <class Tag>
[[nodiscard]] std::string to_string(const StrongId<Tag>& id) {
  return format_id(id.value());
}

template <class Tag>
[[nodiscard]] std::string to_string(const Generation<Tag>& g) {
  return format_generation(g.value());
}

}  // namespace tos

#endif  // TOS_CORE_IDENTITIES_HPP
