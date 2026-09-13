// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "tos/core/state.hpp"

#include <algorithm>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace tos {
namespace {

bool same_capability(const CapabilitySet& a, const CapabilitySet& b) {
  return a.operations == b.operations && a.flags == b.flags && a.unproven_flags == b.unproven_flags &&
         a.memory_domains == b.memory_domains && a.min_payload_bytes == b.min_payload_bytes &&
         a.max_payload_bytes == b.max_payload_bytes && a.alignment_bytes == b.alignment_bytes &&
         a.max_concurrency == b.max_concurrency && a.queue_capacity == b.queue_capacity &&
         a.transport_classes == b.transport_classes && a.payload_classes == b.payload_classes &&
         a.protocol_version == b.protocol_version &&
         a.driver_backend_version == b.driver_backend_version &&
         a.firmware_generation == b.firmware_generation && a.accelerator_arch == b.accelerator_arch &&
         a.backend_family == b.backend_family;
}

bool same_load(const DomainLoadEvidence& a, const DomainLoadEvidence& b) {
  return a.utilization_percent == b.utilization_percent &&
         a.congestion_percent == b.congestion_percent && a.queue_depth == b.queue_depth &&
         a.in_flight == b.in_flight && a.observed_latency_ns == b.observed_latency_ns &&
         a.throughput_bytes_per_second == b.throughput_bytes_per_second && a.healthy == b.healthy &&
         a.ready == b.ready && a.accepting == b.accepting;
}

bool same_locality(const DomainLocality& a, const DomainLocality& b) {
  return a.class_to_payload == b.class_to_payload && a.numa_node == b.numa_node &&
         a.host_index == b.host_index && a.pcie_root_complex == b.pcie_root_complex &&
         a.pcie_switch == b.pcie_switch && a.local_nic == b.local_nic &&
         a.has_local_nic == b.has_local_nic;
}

bool same_topology(const DomainTopology& a, const DomainTopology& b) {
  return a.host_node == b.host_node && a.fabric == b.fabric && a.rack == b.rack && a.pod == b.pod;
}

bool same_compatibility(const DomainCompatibility& a, const DomainCompatibility& b) {
  return a.driver_backend_version == b.driver_backend_version &&
         a.firmware_generation == b.firmware_generation && a.protocol_version == b.protocol_version &&
         a.accelerator_arch == b.accelerator_arch && a.backend_family == b.backend_family;
}

bool same_capacity(const CapacityVector& a, const CapacityVector& b) { return a == b; }

}  // namespace

struct DomainRegistry::Impl {
  mutable std::shared_mutex mutex;
  std::unordered_map<std::uint64_t, ExecutionDomainRecord> domains;
  std::unordered_map<std::string, std::uint64_t> by_name;
  std::uint64_t mutation_sequence{0};
  std::size_t max_domains{kMaxExecutionDomains};
};

DomainRegistry::DomainRegistry() : impl_(std::make_unique<Impl>()) {}
DomainRegistry::~DomainRegistry() = default;

Status DomainRegistry::set_max_domains(std::size_t maximum) {
  std::unique_lock<std::shared_mutex> lock(impl_->mutex);
  if (maximum == 0) return Status::failure("domain.invalid_limit");
  if (maximum < impl_->domains.size()) {
    return Status::failure("domain.limit_below_registered",
                           "the limit is below the number of registered domains");
  }
  impl_->max_domains = std::min<std::size_t>(maximum, kMaxExecutionDomains);
  return Status::success();
}

Checked<DomainUpdateResult> DomainRegistry::upsert(const ExecutionDomainRecord& record,
                                                   bool allow_generation_regression) {
  const Status valid = validate_domain_record(record);
  if (!valid) return Checked<DomainUpdateResult>::bad(valid.code, valid.message);

  std::unique_lock<std::shared_mutex> lock(impl_->mutex);
  const auto existing = impl_->domains.find(record.id.value());
  if (existing == impl_->domains.end()) {
    if (impl_->domains.size() >= impl_->max_domains) {
      return Checked<DomainUpdateResult>::bad("domain.limit_reached");
    }
    const auto name_owner = impl_->by_name.find(record.name);
    if (name_owner != impl_->by_name.end() && name_owner->second != record.id.value()) {
      return Checked<DomainUpdateResult>::bad("domain.name_conflict", record.name);
    }
    ExecutionDomainRecord stored = record;
    stored.capability.capability.canonicalize();
    if (!stored.generation.published()) stored.generation = ExecutionDomainGeneration::first();
    if (!stored.capability.generation.published()) {
      stored.capability.generation = CapabilityGeneration::first();
    }
    if (!stored.backend_generation.published()) stored.backend_generation = BackendGeneration::first();
    if (stored.capability.evidence.published()) {
      // keep publisher evidence generation
    } else {
      stored.capability.evidence = EvidenceGeneration::first();
    }
    stored.registration_sequence = ++impl_->mutation_sequence;
    DomainUpdateResult result;
    result.kind = DomainUpdateKind::kRegistered;
    result.generation = stored.generation;
    result.accepted = true;
    impl_->by_name[stored.name] = stored.id.value();
    impl_->domains[stored.id.value()] = std::move(stored);
    return Checked<DomainUpdateResult>::good(result);
  }

  ExecutionDomainRecord& current = existing->second;
  // A different worker incarnation claiming the same domain identity re-bases the
  // generation counters: the coordinator owns those counters, so a replacement
  // process (whose own counters restart) republishes content without being able to
  // move authority backwards. Within one incarnation, regressions stay rejected.
  const bool incarnation_changed = current.worker_boot.valid() && record.worker_boot.valid() &&
                                   record.worker_boot != current.worker_boot;
  if (record.generation.value() < current.generation.value() && !allow_generation_regression &&
      !incarnation_changed) {
    return Checked<DomainUpdateResult>::bad(
        "domain.generation_regression",
        "domain generation would move backwards within one worker incarnation");
  }
  if (!allow_generation_regression && !incarnation_changed &&
      record.generation.value() == current.generation.value() && record.generation.published() &&
      !same_capability(record.capability.capability, current.capability.capability)) {
    return Checked<DomainUpdateResult>::bad(
        "domain.capability_conflict",
        "capability content changed without advancing the domain generation");
  }

  // Generation families that the coordinator owns are driven by content, not by the
  // publisher's counter: a record whose content is unchanged keeps the authoritative
  // generation, so an unrelated re-publication cannot invalidate dependent plans.
  const bool capability_content_changed =
      !same_capability(record.capability.capability, current.capability.capability);
  const bool locality_content_changed = !same_locality(record.locality, current.locality);
  const bool topology_content_changed = !same_topology(record.topology, current.topology);
  const bool compatibility_content_changed =
      !same_compatibility(record.compatibility, current.compatibility);

  DomainUpdateKind kind = DomainUpdateKind::kUnchanged;
  if (capability_content_changed) {
    kind = DomainUpdateKind::kCapabilityChanged;
  } else if (!same_load(record.load, current.load)) {
    kind = DomainUpdateKind::kLoadChanged;
  } else if (record.load.health_generation != current.load.health_generation ||
             record.load.ready != current.load.ready) {
    kind = DomainUpdateKind::kHealthChanged;
  } else if (!same_locality(record.locality, current.locality)) {
    kind = DomainUpdateKind::kLocalityChanged;
  } else if (!same_topology(record.topology, current.topology)) {
    kind = DomainUpdateKind::kTopologyChanged;
  } else if (!same_compatibility(record.compatibility, current.compatibility)) {
    kind = DomainUpdateKind::kCompatibilityChanged;
  } else if (record.isolation != current.isolation) {
    kind = DomainUpdateKind::kIsolationChanged;
  } else if (!same_capacity(record.capacity, current.capacity)) {
    kind = DomainUpdateKind::kCapacityChanged;
  } else if (record.backend_generation != current.backend_generation) {
    kind = DomainUpdateKind::kBackendGenerationChanged;
  } else if (record.fenced && !current.fenced) {
    kind = DomainUpdateKind::kFenced;
  }

  const ExecutionDomainGeneration next_generation =
      incarnation_changed ? current.generation.next()
                          : (record.generation.published() &&
                                     record.generation.value() > current.generation.value()
                                 ? record.generation
                                 : current.generation);
  ExecutionDomainRecord stored = record;
  stored.capability.capability.canonicalize();
  stored.generation = next_generation;
  stored.registration_sequence = current.registration_sequence;
  if (incarnation_changed) {
    // Fresh incarnation: its counters are a new baseline, and the fence that
    // applied to the previous incarnation no longer describes this one.
    stored.publisher_sequence = PublisherSequence{};
    stored.publisher_sequence.boot = record.worker_boot;
    stored.capability.generation = current.capability.generation.next();
    stored.backend_generation = current.backend_generation.next();
    stored.topology.generation = current.topology.generation.next();
    stored.locality.generation = current.locality.generation.next();
    stored.compatibility.generation = current.compatibility.generation.next();
    stored.fenced = record.fenced;
    if (!stored.fenced) stored.fence_reason.clear();
  } else {
    if (capability_content_changed) {
      stored.capability.generation = current.capability.generation.next();
      stored.capability.publisher = record.capability.publisher.valid()
                                        ? record.capability.publisher
                                        : record.worker_boot;
      stored.publisher_sequence.capability = record.capability.generation.value();
    } else {
      stored.capability = current.capability;
    }
    if (stored.backend_generation.value() < current.backend_generation.value()) {
      stored.backend_generation = current.backend_generation;
    }
    if (topology_content_changed) {
      stored.topology.generation = current.topology.generation.next();
    } else {
      stored.topology = current.topology;
    }
    if (locality_content_changed) {
      stored.locality.generation = current.locality.generation.next();
    } else {
      stored.locality = current.locality;
    }
    if (compatibility_content_changed) {
      stored.compatibility.generation = current.compatibility.generation.next();
    } else {
      stored.compatibility = current.compatibility;
    }
    stored.fenced = record.fenced || current.fenced;
    if (current.fenced && !record.fenced) stored.fence_reason = current.fence_reason;
    if (stored.publisher_sequence.boot != record.worker_boot) {
      stored.publisher_sequence = PublisherSequence{};
      stored.publisher_sequence.boot = record.worker_boot;
    }
  }
  impl_->by_name.erase(current.name);
  impl_->by_name[stored.name] = stored.id.value();
  current = std::move(stored);
  ++impl_->mutation_sequence;

  DomainUpdateResult result;
  result.kind = kind;
  result.generation = current.generation;
  result.accepted = true;
  return Checked<DomainUpdateResult>::good(result);
}

Status DomainRegistry::publish_capability(const CapabilityRecord& capability) {
  std::unique_lock<std::shared_mutex> lock(impl_->mutex);
  const auto found = impl_->domains.find(capability.domain.value());
  if (found == impl_->domains.end()) return Status::failure("domain.not_registered");
  ExecutionDomainRecord& current = found->second;
  const WorkerBootId publisher =
      capability.publisher.valid() ? capability.publisher : current.worker_boot;
  if (!publisher.valid()) {
    return Status::failure("domain.capability_without_publisher",
                           "a capability record must name the incarnation that proved it");
  }
  CapabilitySet canonical = capability.capability;
  canonical.canonicalize();
  const bool same_publisher = current.publisher_sequence.boot == publisher;
  const bool content_changed = !same_capability(canonical, current.capability.capability);
  if (same_publisher) {
    if (capability.generation.value() <= current.publisher_sequence.capability) {
      // A duplicate or reordered publication from the same incarnation. Identical
      // content is an idempotent refresh that must not invalidate dependents;
      // different content at or below the consumed sequence is stale and rejected.
      if (!content_changed) return Status::success();
      return Status::failure("domain.capability_stale_publication",
                             "capability sequence did not advance within one incarnation");
    }
    if (!content_changed) {
      // The publisher advanced its sequence without changing the facts. Nothing that
      // depends on this capability becomes invalid, so the authoritative generation
      // does not move; only the watermark does.
      current.publisher_sequence.capability = capability.generation.value();
      current.capability.publisher = publisher;
      return Status::success();
    }
  } else {
    // A different incarnation publishes with its own counter space: the coordinator
    // re-bases the authoritative generation and starts a fresh watermark.
    current.publisher_sequence = PublisherSequence{};
    current.publisher_sequence.boot = publisher;
  }
  current.capability.capability = std::move(canonical);
  current.capability.generation = current.capability.generation.next();
  current.capability.publisher = publisher;
  current.publisher_sequence.capability = capability.generation.value();
  current.capability.provenance = capability.provenance;
  current.capability.authoritative = capability.authoritative;
  if (capability.evidence.published()) current.capability.evidence = capability.evidence;
  // Content that actually changed is a new domain state: the domain generation moves
  // so that any plan bound to the previous capability is invalidated.
  if (content_changed) {
    current.generation = ExecutionDomainGeneration(current.generation.value() + 1);
  }
  ++impl_->mutation_sequence;
  return Status::success();
}

Status DomainRegistry::publish_load(ExecutionDomainId domain, const DomainLoadEvidence& load) {
  std::unique_lock<std::shared_mutex> lock(impl_->mutex);
  const auto found = impl_->domains.find(domain.value());
  if (found == impl_->domains.end()) return Status::failure("domain.not_registered");
  ExecutionDomainRecord& current = found->second;
  if (!load.published()) {
    return Status::failure("domain.load_not_published",
                           "load evidence must carry health, load and queue generations");
  }
  if (!current.worker_boot.valid()) {
    return Status::failure("domain.load_without_incarnation",
                           "load evidence requires an owning worker incarnation");
  }
  const bool same_publisher = current.publisher_sequence.boot == current.worker_boot;
  const bool advanced = load.health_generation.value() > current.publisher_sequence.health ||
                        load.queue_generation.value() > current.publisher_sequence.queue ||
                        load.load_generation.value() > current.publisher_sequence.load;
  if (same_publisher && !advanced) {
    if (same_load(load, current.load)) return Status::success();
    return Status::failure("domain.load_stale_publication",
                           "load sequence did not advance within one incarnation");
  }
  if (!same_publisher) {
    current.publisher_sequence = PublisherSequence{};
    current.publisher_sequence.boot = current.worker_boot;
  }
  current.publisher_sequence.load = load.load_generation.value();
  current.publisher_sequence.queue = load.queue_generation.value();
  current.publisher_sequence.health = load.health_generation.value();
  current.load = load;
  ++impl_->mutation_sequence;
  return Status::success();
}

Status DomainRegistry::publish_locality(ExecutionDomainId domain, const DomainLocality& locality) {
  std::unique_lock<std::shared_mutex> lock(impl_->mutex);
  const auto found = impl_->domains.find(domain.value());
  if (found == impl_->domains.end()) return Status::failure("domain.not_registered");
  ExecutionDomainRecord& current = found->second;
  if (locality.generation.value() < current.locality.generation.value()) {
    return Status::failure("domain.locality_generation_regression");
  }
  if (!locality.generation.published()) {
    return Status::failure("domain.locality_not_published");
  }
  current.locality = locality;
  current.generation = ExecutionDomainGeneration(current.generation.value() + 1);
  ++impl_->mutation_sequence;
  return Status::success();
}

Status DomainRegistry::publish_topology(ExecutionDomainId domain, const DomainTopology& topology) {
  std::unique_lock<std::shared_mutex> lock(impl_->mutex);
  const auto found = impl_->domains.find(domain.value());
  if (found == impl_->domains.end()) return Status::failure("domain.not_registered");
  ExecutionDomainRecord& current = found->second;
  if (topology.generation.value() < current.topology.generation.value()) {
    return Status::failure("domain.topology_generation_regression");
  }
  if (!topology.generation.published()) return Status::failure("domain.topology_not_published");
  current.topology = topology;
  current.generation = ExecutionDomainGeneration(current.generation.value() + 1);
  ++impl_->mutation_sequence;
  return Status::success();
}

Status DomainRegistry::publish_compatibility(ExecutionDomainId domain,
                                             const DomainCompatibility& compatibility) {
  std::unique_lock<std::shared_mutex> lock(impl_->mutex);
  const auto found = impl_->domains.find(domain.value());
  if (found == impl_->domains.end()) return Status::failure("domain.not_registered");
  ExecutionDomainRecord& current = found->second;
  if (compatibility.generation.value() < current.compatibility.generation.value()) {
    return Status::failure("domain.compatibility_generation_regression");
  }
  if (!compatibility.generation.published()) {
    return Status::failure("domain.compatibility_not_published");
  }
  current.compatibility = compatibility;
  current.generation = ExecutionDomainGeneration(current.generation.value() + 1);
  ++impl_->mutation_sequence;
  return Status::success();
}

Status DomainRegistry::publish_capacity(ExecutionDomainId domain, const CapacityVector& capacity) {
  for (std::size_t i = 0; i < capacity.size(); ++i) {
    const auto kind = static_cast<ResourceKind>(i);
    const std::uint64_t limit = (kind == ResourceKind::kDeviceMemoryBytes ||
                                 kind == ResourceKind::kScratchMemoryBytes)
                                    ? (1ULL << 48)
                                    : (1ULL << 32);
    if (capacity[i] > limit) return Status::failure("domain.capacity_out_of_range");
  }
  std::unique_lock<std::shared_mutex> lock(impl_->mutex);
  const auto found = impl_->domains.find(domain.value());
  if (found == impl_->domains.end()) return Status::failure("domain.not_registered");
  found->second.capacity = capacity;
  found->second.generation = ExecutionDomainGeneration(found->second.generation.value() + 1);
  ++impl_->mutation_sequence;
  return Status::success();
}

Status DomainRegistry::fence(ExecutionDomainId domain, std::string_view reason) {
  std::unique_lock<std::shared_mutex> lock(impl_->mutex);
  const auto found = impl_->domains.find(domain.value());
  if (found == impl_->domains.end()) return Status::failure("domain.not_registered");
  ExecutionDomainRecord& current = found->second;
  if (!current.fenced) {
    current.fenced = true;
    current.fence_reason = bounded_text(reason);
    current.generation = ExecutionDomainGeneration(current.generation.value() + 1);
    current.load = DomainLoadEvidence{};
  }
  ++impl_->mutation_sequence;
  return Status::success();
}

std::size_t DomainRegistry::fence_worker_boot(WorkerBootId boot, std::string_view reason) {
  std::unique_lock<std::shared_mutex> lock(impl_->mutex);
  std::size_t affected = 0;
  for (auto& entry : impl_->domains) {
    ExecutionDomainRecord& record = entry.second;
    if (record.worker_boot == boot && !record.fenced) {
      record.fenced = true;
      record.fence_reason = bounded_text(reason);
      record.generation = ExecutionDomainGeneration(record.generation.value() + 1);
      record.load = DomainLoadEvidence{};
      ++affected;
    }
  }
  if (affected > 0) ++impl_->mutation_sequence;
  return affected;
}

Status DomainRegistry::remove(ExecutionDomainId domain) {
  std::unique_lock<std::shared_mutex> lock(impl_->mutex);
  const auto found = impl_->domains.find(domain.value());
  if (found == impl_->domains.end()) return Status::failure("domain.not_registered");
  impl_->by_name.erase(found->second.name);
  impl_->domains.erase(found);
  ++impl_->mutation_sequence;
  return Status::success();
}

bool DomainRegistry::get(ExecutionDomainId domain, ExecutionDomainRecord& out) const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  const auto found = impl_->domains.find(domain.value());
  if (found == impl_->domains.end()) return false;
  out = found->second;
  return true;
}

std::optional<ExecutionDomainRecord> DomainRegistry::find(ExecutionDomainId domain) const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  const auto found = impl_->domains.find(domain.value());
  if (found == impl_->domains.end()) return std::nullopt;
  return found->second;
}

std::optional<ExecutionDomainRecord> DomainRegistry::find_by_name(std::string_view name) const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  const auto owner = impl_->by_name.find(std::string(name));
  if (owner == impl_->by_name.end()) return std::nullopt;
  const auto found = impl_->domains.find(owner->second);
  if (found == impl_->domains.end()) return std::nullopt;
  return found->second;
}

std::vector<ExecutionDomainRecord> DomainRegistry::all() const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  std::vector<ExecutionDomainRecord> out;
  out.reserve(impl_->domains.size());
  for (const auto& entry : impl_->domains) out.push_back(entry.second);
  std::sort(out.begin(), out.end(), [](const ExecutionDomainRecord& a, const ExecutionDomainRecord& b) {
    return a.id < b.id;
  });
  return out;
}

std::size_t DomainRegistry::size() const noexcept {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  return impl_->domains.size();
}

std::uint64_t DomainRegistry::mutation_sequence() const noexcept {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  return impl_->mutation_sequence;
}

Status DomainRegistry::mark_domain_evidence_stale(ExecutionDomainId domain,
                                                std::string_view reason) {
  std::unique_lock<std::shared_mutex> lock(impl_->mutex);
  const auto found = impl_->domains.find(domain.value());
  if (found == impl_->domains.end()) return Status::failure("domain.not_registered");
  ExecutionDomainRecord& record = found->second;
  const bool had_evidence = record.load.published();
  record.load = DomainLoadEvidence{};
  record.generation = ExecutionDomainGeneration(record.generation.value() + 1);
  if (had_evidence) {
    // The withdrawal itself is the reason no plan may keep using the old evidence.
    record.fence_reason = bounded_text(reason);
    ++impl_->mutation_sequence;
  }
  return Status::success();
}

std::size_t DomainRegistry::mark_dynamic_evidence_stale(std::string_view reason) {
  std::unique_lock<std::shared_mutex> lock(impl_->mutex);
  std::size_t affected = 0;
  for (auto& entry : impl_->domains) {
    ExecutionDomainRecord& record = entry.second;
    if (record.load.published()) ++affected;
    record.load = DomainLoadEvidence{};  // queue/load/health are never current after restart
    if (!record.fence_reason.empty()) {
      record.fence_reason = bounded_text(reason);
    }
  }
  if (affected > 0) ++impl_->mutation_sequence;
  return affected;
}

}  // namespace tos
