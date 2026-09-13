// Shared runtime state: domain registry and coordinator epoch bookkeeping.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#ifndef TOS_CORE_STATE_HPP
#define TOS_CORE_STATE_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "tos/core/authority.hpp"
#include "tos/core/domain.hpp"
#include "tos/core/policy.hpp"
#include "tos/util/status.hpp"

namespace tos {

/// Which part of a domain record changed. Used by reconciliation and by callers
/// that want to know why a plan was invalidated.
enum class DomainUpdateKind : std::uint8_t {
  kRegistered = 0,
  kCapabilityChanged = 1,
  kLoadChanged = 2,
  kHealthChanged = 3,
  kLocalityChanged = 4,
  kTopologyChanged = 5,
  kCompatibilityChanged = 6,
  kIsolationChanged = 7,
  kCapacityChanged = 8,
  kBackendGenerationChanged = 9,
  kFenced = 10,
  kRemoved = 11,
  kUnchanged = 12,
};

[[nodiscard]] std::string_view to_string(DomainUpdateKind value) noexcept;
[[nodiscard]] bool parse_enum(std::string_view text, DomainUpdateKind& out) noexcept;

struct DomainUpdateResult {
  DomainUpdateKind kind{DomainUpdateKind::kUnchanged};
  ExecutionDomainGeneration generation;
  bool accepted{false};
};

/// Thread-safe registry of execution domains.
///
/// Locking: one shared_mutex per registry. Readers (planning, snapshots) take a
/// shared lock and copy out value records; writers take an exclusive lock and
/// never perform I/O, callbacks or allocation-heavy work beyond the update itself.
class DomainRegistry {
 public:
  DomainRegistry();
  ~DomainRegistry();
  DomainRegistry(const DomainRegistry&) = delete;
  DomainRegistry& operator=(const DomainRegistry&) = delete;

  /// Bound the number of registered domains. Lowering it below the current count is
  /// refused rather than silently dropping domains.
  [[nodiscard]] Status set_max_domains(std::size_t maximum);

  /// Register or replace a domain. A replacement advances the domain generation;
  /// a regressed generation is rejected (persisted state must not become current).
  [[nodiscard]] Checked<DomainUpdateResult> upsert(const ExecutionDomainRecord& record,
                                                   bool allow_generation_regression = false);

  [[nodiscard]] Status publish_capability(const CapabilityRecord& capability);
  [[nodiscard]] Status publish_load(ExecutionDomainId domain, const DomainLoadEvidence& load);
  [[nodiscard]] Status publish_locality(ExecutionDomainId domain, const DomainLocality& locality);
  [[nodiscard]] Status publish_topology(ExecutionDomainId domain, const DomainTopology& topology);
  [[nodiscard]] Status publish_compatibility(ExecutionDomainId domain,
                                             const DomainCompatibility& compatibility);
  [[nodiscard]] Status publish_capacity(ExecutionDomainId domain, const CapacityVector& capacity);

  [[nodiscard]] Status fence(ExecutionDomainId domain, std::string_view reason);
  [[nodiscard]] std::size_t fence_worker_boot(WorkerBootId boot, std::string_view reason);
  [[nodiscard]] Status remove(ExecutionDomainId domain);

  [[nodiscard]] bool get(ExecutionDomainId domain, ExecutionDomainRecord& out) const;
  [[nodiscard]] std::optional<ExecutionDomainRecord> find(ExecutionDomainId domain) const;
  [[nodiscard]] std::optional<ExecutionDomainRecord> find_by_name(std::string_view name) const;
  [[nodiscard]] std::vector<ExecutionDomainRecord> all() const;  ///< deterministic order
  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] std::uint64_t mutation_sequence() const noexcept;

  /// Mark every domain's volatile evidence as unpublished. Called after a restart:
  /// persisted queue/load/health is never restored as current authority.
  [[nodiscard]] std::size_t mark_dynamic_evidence_stale(std::string_view reason);

  /// Unpublish one domain's volatile evidence and advance its domain generation, so
  /// that plans bound to the withdrawn evidence are invalidated instead of silently
  /// continuing to use stale load or health state.
  [[nodiscard]] Status mark_domain_evidence_stale(ExecutionDomainId domain,
                                                  std::string_view reason);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tos

#endif  // TOS_CORE_STATE_HPP
