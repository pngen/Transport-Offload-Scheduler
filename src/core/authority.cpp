// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "tos/core/authority.hpp"

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tos {
namespace {

void record(std::vector<AuthorityMismatch>& mismatches, std::string field, std::uint64_t bound,
            std::uint64_t observed, std::string detail) {
  AuthorityMismatch mismatch;
  mismatch.field = std::move(field);
  mismatch.bound = bound;
  mismatch.observed = observed;
  mismatch.detail = bounded_text(detail);
  mismatches.push_back(std::move(mismatch));
}

void compare_generation(std::vector<AuthorityMismatch>& mismatches, std::string field,
                        std::uint64_t bound, std::uint64_t observed) {
  switch (relate_generations(bound, observed)) {
    case GenerationRelation::kEqual: return;
    case GenerationRelation::kAdvanced:
      record(mismatches, std::move(field), bound, observed, "evidence advanced past the binding");
      return;
    case GenerationRelation::kRegressed:
      record(mismatches, std::move(field), bound, observed, "observed generation regressed");
      return;
    case GenerationRelation::kUnset:
      record(mismatches, std::move(field), bound, observed, "generation was never published");
      return;
  }
}

}  // namespace

bool AuthorityValidation::has_field(std::string_view name) const {
  return std::any_of(mismatches.begin(), mismatches.end(),
                     [name](const AuthorityMismatch& mismatch) { return mismatch.field == name; });
}

AuthorityValidation validate_authority(const AuthorityBinding& binding, const LiveAuthority& live,
                                       const FreshnessRequirements& freshness) {
  AuthorityValidation validation;
  if (!live.domain_present) {
    record(validation.mismatches, "domain_absent", binding.domain.value(), 0,
           "execution domain is not registered");
    validation.code = "authority.domain_absent";
    return validation;
  }
  if (live.domain_fenced) {
    record(validation.mismatches, "domain_fenced", binding.domain.value(), 0,
           "execution domain authority was withdrawn");
    validation.code = "authority.domain_fenced";
    return validation;
  }
  if (binding.coordinator_epoch != live.coordinator_epoch) {
    record(validation.mismatches, "coordinator_epoch", binding.coordinator_epoch.value(),
           live.coordinator_epoch.value(), "coordinator epoch changed");
  }
  if (!binding.worker.valid() || binding.worker != live.worker) {
    record(validation.mismatches, "worker", binding.worker.value(), live.worker.value(),
           "worker identity changed");
  }
  if (!binding.worker_boot.valid() || binding.worker_boot != live.worker_boot) {
    record(validation.mismatches, "worker_boot", binding.worker_boot.value(),
           live.worker_boot.value(), "worker incarnation changed or was fenced");
  }
  compare_generation(validation.mismatches, "domain_generation", binding.domain_generation.value(),
                     live.domain_generation.value());
  compare_generation(validation.mismatches, "backend_generation", binding.backend_generation.value(),
                     live.backend_generation.value());
  compare_generation(validation.mismatches, "policy_generation", binding.policy_generation.value(),
                     live.policy_generation.value());
  if (freshness.capability) {
    compare_generation(validation.mismatches, "capability_generation",
                       binding.capability_generation.value(), live.capability_generation.value());
  }
  if (freshness.compatibility) {
    compare_generation(validation.mismatches, "compatibility_generation",
                       binding.compatibility_generation.value(),
                       live.compatibility_generation.value());
  }
  if (freshness.topology) {
    compare_generation(validation.mismatches, "topology_generation",
                       binding.topology_generation.value(), live.topology_generation.value());
  }
  if (freshness.locality) {
    compare_generation(validation.mismatches, "locality_generation",
                       binding.locality_generation.value(), live.locality_generation.value());
  }
  if (freshness.health) {
    compare_generation(validation.mismatches, "health_generation", binding.health_generation.value(),
                       live.health_generation.value());
  }
  if (freshness.queue) {
    compare_generation(validation.mismatches, "queue_generation", binding.queue_generation.value(),
                       live.queue_generation.value());
  }
  if (freshness.load) {
    compare_generation(validation.mismatches, "load_generation", binding.load_generation.value(),
                       live.load_generation.value());
  }
  if (freshness.isolation) {
    compare_generation(validation.mismatches, "isolation_generation",
                       binding.isolation_generation.value(), live.isolation_generation.value());
  }
  if (freshness.evidence) {
    compare_generation(validation.mismatches, "evidence_generation",
                       binding.evidence_generation.value(), live.evidence_generation.value());
  }
  validation.valid = validation.mismatches.empty();
  validation.code = validation.valid ? "ok" : "authority.stale";
  return validation;
}

LiveAuthority live_authority_of(const ExecutionDomainRecord& record, CoordinatorEpoch epoch,
                               PolicyGeneration policy_generation,
                               IsolationPolicyGeneration isolation_generation) {
  LiveAuthority live;
  live.coordinator_epoch = epoch;
  live.worker = record.worker;
  live.worker_boot = record.worker_boot;
  live.domain_generation = record.generation;
  live.capability_generation = record.capability.generation;
  live.backend_generation = record.backend_generation;
  live.topology_generation = record.topology.generation;
  live.locality_generation = record.locality.generation;
  live.health_generation = record.load.health_generation;
  live.queue_generation = record.load.queue_generation;
  live.load_generation = record.load.load_generation;
  live.policy_generation = policy_generation;
  live.compatibility_generation = record.compatibility.generation;
  live.isolation_generation = isolation_generation;
  live.evidence_generation = record.load.evidence.published() ? record.load.evidence
                                                               : record.capability.evidence;
  live.domain_fenced = record.fenced;
  live.domain_present = true;
  return live;
}

// ---- WorkerAuthority --------------------------------------------------------

struct WorkerAuthority::Impl {
  mutable std::mutex mutex;
  std::unordered_map<std::uint64_t, WorkerBootId> current;
  std::unordered_map<std::uint64_t, WorkerId> boot_owner;
  std::unordered_set<std::uint64_t> fenced;
  std::unordered_map<std::uint64_t, std::string> labels;
  std::size_t max_fenced{4096};
};

WorkerAuthority::WorkerAuthority() : impl_(std::make_unique<Impl>()) {}
WorkerAuthority::~WorkerAuthority() = default;

Status WorkerAuthority::register_boot(WorkerId worker, WorkerBootId boot, std::string_view label) {
  if (!worker.valid()) return Status::failure("worker.invalid_identity");
  if (!boot.valid()) return Status::failure("worker.invalid_boot_identity");
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->fenced.count(boot.value()) != 0U) {
    return Status::failure("worker.boot_fenced", "this worker incarnation was fenced");
  }
  const auto owner = impl_->boot_owner.find(boot.value());
  if (owner != impl_->boot_owner.end() && owner->second != worker) {
    return Status::failure("worker.boot_owned", "boot identity already belongs to another worker");
  }
  const auto existing = impl_->current.find(worker.value());
  if (existing != impl_->current.end()) {
    if (existing->second == boot) {
      return Status::failure("worker.duplicate_registration",
                             "this worker incarnation is already registered");
    }
    // A new incarnation replaces the previous one; the previous boot is fenced.
    impl_->fenced.insert(existing->second.value());
    impl_->boot_owner.erase(existing->second.value());
  }
  if (impl_->fenced.size() > impl_->max_fenced) {
    // Bound the fence set: drop the oldest entries deterministically by value.
    std::vector<std::uint64_t> ordered(impl_->fenced.begin(), impl_->fenced.end());
    std::sort(ordered.begin(), ordered.end());
    while (ordered.size() > impl_->max_fenced / 2) {
      impl_->fenced.erase(ordered.front());
      ordered.erase(ordered.begin());
    }
  }
  impl_->current[worker.value()] = boot;
  impl_->boot_owner[boot.value()] = worker;
  impl_->labels[worker.value()] = bounded_text(label, kMaxLabelLength);
  return Status::success();
}

bool WorkerAuthority::is_current(WorkerId worker, WorkerBootId boot) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto found = impl_->current.find(worker.value());
  if (!boot.valid()) return false;
  return found != impl_->current.end() && found->second == boot &&
         impl_->fenced.count(boot.value()) == 0U;
}

bool WorkerAuthority::is_fenced(WorkerId worker, WorkerBootId boot) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->fenced.count(boot.value()) != 0U) return true;
  const auto found = impl_->current.find(worker.value());
  return found == impl_->current.end() || found->second != boot;
}

bool WorkerAuthority::fenced_boot_id(WorkerBootId boot) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->fenced.count(boot.value()) != 0U;
}

Status WorkerAuthority::fence(WorkerId worker, WorkerBootId boot, std::string_view reason) {
  if (!boot.valid()) return Status::failure("worker.invalid_boot_identity");
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->fenced.insert(boot.value());
  impl_->boot_owner.erase(boot.value());
  const auto found = impl_->current.find(worker.value());
  if (found != impl_->current.end() && found->second == boot) {
    impl_->current.erase(found);
  }
  impl_->labels[worker.value()] = bounded_text(reason, kMaxLabelLength);
  return Status::success();
}

Status WorkerAuthority::fence_boot(WorkerBootId boot, std::string_view reason) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->fenced.insert(boot.value());
  const auto owner = impl_->boot_owner.find(boot.value());
  if (owner != impl_->boot_owner.end()) {
    const auto found = impl_->current.find(owner->second.value());
    if (found != impl_->current.end() && found->second == boot) {
      impl_->current.erase(found);
      impl_->labels[owner->second.value()] = bounded_text(reason, kMaxLabelLength);
    }
    impl_->boot_owner.erase(owner);
  }
  return Status::success();
}

WorkerBootId WorkerAuthority::current_boot(WorkerId worker) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto found = impl_->current.find(worker.value());
  return found == impl_->current.end() ? WorkerBootId{} : found->second;
}

std::vector<WorkerBootId> WorkerAuthority::fenced_boots() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  std::vector<std::uint64_t> ordered(impl_->fenced.begin(), impl_->fenced.end());
  std::sort(ordered.begin(), ordered.end());
  std::vector<WorkerBootId> out;
  out.reserve(ordered.size());
  for (std::uint64_t value : ordered) out.push_back(WorkerBootId(value));
  return out;
}

std::vector<std::pair<WorkerId, WorkerBootId>> WorkerAuthority::live_boots() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  std::vector<std::pair<std::uint64_t, std::uint64_t>> ordered;
  ordered.reserve(impl_->current.size());
  for (const auto& entry : impl_->current) {
    ordered.emplace_back(entry.first, entry.second.value());
  }
  std::sort(ordered.begin(), ordered.end());
  std::vector<std::pair<WorkerId, WorkerBootId>> out;
  out.reserve(ordered.size());
  for (const auto& entry : ordered) {
    out.emplace_back(WorkerId(entry.first), WorkerBootId(entry.second));
  }
  return out;
}

std::size_t WorkerAuthority::tracked_workers() const noexcept {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->current.size();
}

}  // namespace tos
