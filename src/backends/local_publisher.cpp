// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "tos/backends/local_publisher.hpp"

#include <algorithm>
#include <mutex>

#include "tos/util/log.hpp"

namespace tos {

LocalDomainPublisher::LocalDomainPublisher(Scheduler& scheduler,
                                           std::shared_ptr<IExecutionBackend> backend)
    : scheduler_(&scheduler), backend_(std::move(backend)) {}

Status LocalDomainPublisher::sync() {
  const std::vector<ExecutionDomainRecord> discovered = backend_->discover_domains();
  for (const ExecutionDomainRecord& record : discovered) {
    ExecutionDomainRecord existing;
    const bool present = scheduler_->domains().get(record.id, existing);
    CapabilityRecord capability_record = record.capability;
    if (!capability_record.publisher.valid()) capability_record.publisher = record.worker_boot;

    if (!present) {
      const Status registered = scheduler_->register_domain(record);
      if (!registered) return registered;
      const Status capability = scheduler_->publish_capability(capability_record);
      if (!capability) return capability;
      const Status locality = scheduler_->publish_locality(record.id, record.locality);
      if (!locality) return locality;
      const Status topology = scheduler_->publish_topology(record.id, record.topology);
      if (!topology) return topology;
      const Status compatibility =
          scheduler_->publish_compatibility(record.id, record.compatibility);
      if (!compatibility) return compatibility;
      const Status capacity = scheduler_->publish_capacity(record.id, record.capacity);
      if (!capacity) return capacity;
      if (record.load.published()) {
        const Status load = scheduler_->publish_load(record.id, record.load);
        if (!load) return load;
      } else {
        // A domain that has not published operating evidence yet is registered but
        // not eligible. It must never abort the publication of its peers.
        const Status withdrawn = scheduler_->withdraw_evidence(
            record.id, "no current dynamic evidence published by the backend");
        if (!withdrawn) return withdrawn;
      }
      published_generations_[record.id.value()] = record.generation;
      continue;
    }

    // A domain generation advance means the backend considers this a different
    // domain state: publish the full record so that bound plans are invalidated.
    const auto published_generation = published_generations_.find(record.id.value());
    const bool generation_advanced =
        record.generation.value() > existing.generation.value() ||
        (published_generation != published_generations_.end() &&
         record.generation.value() > published_generation->second.value());
    if (generation_advanced && record.generation != existing.generation) {
      auto updated = scheduler_->update_domain(record);
      if (!updated.ok() && updated.status.code != "domain.generation_regression") {
        return updated.status;
      }
      published_generations_[record.id.value()] = record.generation;
    }

    if (record.capability.capability.operations != existing.capability.capability.operations ||
        record.capability.capability.flags != existing.capability.capability.flags ||
        record.capability.capability.max_payload_bytes !=
            existing.capability.capability.max_payload_bytes ||
        record.capability.capability.queue_capacity != existing.capability.capability.queue_capacity) {
      const Status capability = scheduler_->publish_capability(capability_record);
      if (!capability) return capability;
    }
    if (record.capacity != existing.capacity) {
      const Status capacity = scheduler_->publish_capacity(record.id, record.capacity);
      if (!capacity) return capacity;
    }
    if (record.locality.generation != existing.locality.generation ||
        record.locality.class_to_payload != existing.locality.class_to_payload) {
      const Status locality = scheduler_->publish_locality(record.id, record.locality);
      if (!locality) return locality;
    }
    if (record.topology.generation != existing.topology.generation) {
      const Status topology = scheduler_->publish_topology(record.id, record.topology);
      if (!topology) return topology;
    }
    if (record.compatibility.generation != existing.compatibility.generation ||
        record.compatibility.driver_backend_version !=
            existing.compatibility.driver_backend_version) {
      const Status compatibility =
          scheduler_->publish_compatibility(record.id, record.compatibility);
      if (!compatibility) return compatibility;
    }
    if (record.load.published()) {
      const Status load = scheduler_->publish_load(record.id, record.load);
      if (!load && load.code != "domain.load_stale_publication") return load;
    } else if (existing.load.published()) {
      // The backend withdrew its dynamic evidence: the scheduler must stop treating
      // the previously published load and health state as current.
      const Status withdrawn = scheduler_->withdraw_evidence(
          record.id, "backend withdrew dynamic evidence for this domain");
      if (!withdrawn) return withdrawn;
    }
    published_generations_[record.id.value()] = record.generation;
  }
  return Status::success();
}

Status LocalDomainPublisher::publish_load() {
  const std::vector<ExecutionDomainRecord> discovered = backend_->discover_domains();
  for (const ExecutionDomainRecord& record : discovered) {
    if (!record.load.published()) continue;
    const Status load = scheduler_->publish_load(record.id, record.load);
    if (!load && load.code != "domain.load_stale_publication") return load;
  }
  return Status::success();
}

Status LocalDomainPublisher::withdraw_missing() {
  const std::vector<ExecutionDomainRecord> discovered = backend_->discover_domains();
  std::vector<ExecutionDomainId> live;
  live.reserve(discovered.size());
  for (const ExecutionDomainRecord& record : discovered) live.push_back(record.id);
  for (const auto& entry : scheduler_->domains().all()) {
    const bool still_present = std::find(live.begin(), live.end(), entry.id) != live.end();
    if (!still_present) {
      const Status removed = scheduler_->remove_domain(entry.id);
      if (!removed) return removed;
      published_generations_.erase(entry.id.value());
    }
  }
  return Status::success();
}

Status LocalDomainPublisher::fence_boot(WorkerBootId boot, std::string_view reason) {
  return scheduler_->fence_worker_boot(boot, reason);
}

std::vector<ExecutionDomainRecord> LocalDomainPublisher::last_discovered() const {
  return backend_->discover_domains();
}

}  // namespace tos
