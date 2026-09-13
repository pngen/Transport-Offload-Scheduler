// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "tos/backends/synthetic.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <utility>

#include "domain_factory.hpp"
#include "tos/util/log.hpp"

namespace tos {

std::string_view to_string(SyntheticFault value) noexcept {
  switch (value) {
    case SyntheticFault::kNone: return "none";
    case SyntheticFault::kExecutionFailure: return "execution_failure";
    case SyntheticFault::kAmbiguousOutcome: return "ambiguous_outcome";
    case SyntheticFault::kIntegrityFailure: return "integrity_failure";
    case SyntheticFault::kRefuseExecution: return "refuse_execution";
    case SyntheticFault::kReserveRefused: return "reserve_refused";
  }
  return "none";
}

struct SyntheticBackend::Impl {
  mutable std::mutex mutex;
  std::map<std::uint64_t, SyntheticDomainConfig> domains;
  std::map<SyntheticFault, std::uint32_t> faults;
  BackendGeneration generation;
  std::uint64_t cancellation_requests{0};

  [[nodiscard]] SyntheticFault take_fault() {
    for (auto& entry : faults) {
      if (entry.second > 0) {
        --entry.second;
        return entry.first;
      }
    }
    return SyntheticFault::kNone;
  }

  [[nodiscard]] ExecutionDomainRecord record_of(const SyntheticDomainConfig& config) const {
    ExecutionDomainRecord record;
    record.id = config.id;
    record.generation = config.domain_generation.published() ? config.domain_generation
                                                             : ExecutionDomainGeneration::first();
    record.type = config.type;
    record.name = config.name;
    record.parent_device = config.parent_device;
    record.parent_host = config.parent_host;
    record.worker = config.worker;
    record.worker_boot = config.worker_boot;
    record.provenance = config.provenance;
    record.registration_sequence = config.id.value();
    record.capability.domain = config.id;
    record.capability.generation = config.capability_generation.published()
                                       ? config.capability_generation
                                       : CapabilityGeneration::first();
    record.capability.evidence = config.evidence_generation.published()
                                     ? config.evidence_generation
                                     : EvidenceGeneration::first();
    record.capability.provenance =
        config.provenance == Provenance::kSynthetic ? Provenance::kSynthetic : config.provenance;
    record.capability.authoritative = config.authoritative;
    record.capability.capability = config.capability;
    record.capability.capability.canonicalize();
    if (config.dynamic_evidence_published) {
      record.load.load_generation = LoadGeneration(config.capability_generation.value());
      record.load.queue_generation = QueueGeneration(config.capability_generation.value());
      record.load.health_generation = HealthGeneration(config.capability_generation.value());
      record.load.evidence = record.capability.evidence;
      record.load.provenance = Provenance::kSynthetic;
      record.load.utilization_percent = config.utilization_percent;
      record.load.congestion_percent = config.congestion_percent;
      record.load.queue_depth = config.queue_depth;
      record.load.in_flight = config.in_flight;
      record.load.observed_latency_ns = config.observed_latency_ns;
      record.load.throughput_bytes_per_second = config.throughput_bytes_per_second;
      record.load.healthy = config.healthy;
      record.load.ready = config.ready;
      record.load.accepting = config.accepting;
    }
    record.locality = config.locality;
    record.topology = config.topology;
    record.compatibility = config.compatibility;
    record.isolation = config.isolation;
    record.capacity = config.capacity;
    record.backend_generation = config.backend_generation.published()
                                    ? config.backend_generation
                                    : BackendGeneration::first();
    return record;
  }

  [[nodiscard]] Status ensure_generations(SyntheticDomainConfig& config) {
    if (!config.capability_generation.published()) {
      config.capability_generation = CapabilityGeneration::first();
    }
    if (!config.evidence_generation.published()) {
      config.evidence_generation = EvidenceGeneration::first();
    }
    if (!config.domain_generation.published()) {
      config.domain_generation = ExecutionDomainGeneration::first();
    }
    if (!config.backend_generation.published()) config.backend_generation = BackendGeneration::first();
    if (!config.locality.generation.published()) config.locality.generation = LocalityGeneration::first();
    if (!config.topology.generation.published()) {
      config.topology.generation = TopologyGeneration::first();
    }
    if (!config.compatibility.generation.published()) {
      config.compatibility.generation = CompatibilityGeneration::first();
    }
    if (config.name.empty()) config.name = std::string("synthetic.") + std::to_string(config.id.value());
    return Status::success();
  }
};

SyntheticBackend::SyntheticBackend(std::string name)
    : impl_(std::make_unique<Impl>()), name_(std::move(name)) {
  if (name_.empty()) name_ = "synthetic";
  impl_->generation = BackendGeneration::first();
}

SyntheticBackend::~SyntheticBackend() = default;

BackendGeneration SyntheticBackend::generation() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->generation;
}

Status SyntheticBackend::add_domain(SyntheticDomainConfig config) {
  const Status prepared = impl_->ensure_generations(config);
  if (!prepared) return prepared;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->domains.size() >= kMaxExecutionDomains) return Status::failure("synthetic.domain_limit");
  impl_->domains[config.id.value()] = std::move(config);
  impl_->generation = impl_->generation.next();
  return Status::success();
}

Status SyntheticBackend::remove_domain(ExecutionDomainId domain) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto erased = impl_->domains.erase(domain.value());
  if (erased == 0) return Status::failure("synthetic.unknown_domain");
  impl_->generation = impl_->generation.next();
  return Status::success();
}

std::size_t SyntheticBackend::domain_count() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->domains.size();
}

Status SyntheticBackend::set_capability(ExecutionDomainId domain, CapabilitySet capability,
                                        bool authoritative) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto found = impl_->domains.find(domain.value());
  if (found == impl_->domains.end()) return Status::failure("synthetic.unknown_domain");
  SyntheticDomainConfig& config = found->second;
  capability.canonicalize();
  config.capability = std::move(capability);
  config.capability_generation = config.capability_generation.published()
                                     ? config.capability_generation.next()
                                     : CapabilityGeneration::first();
  config.evidence_generation = config.evidence_generation.published()
                                   ? config.evidence_generation.next()
                                   : EvidenceGeneration::first();
  config.authoritative = authoritative;
  return Status::success();
}

Status SyntheticBackend::set_operation_support(ExecutionDomainId domain, OperationClassId operation,
                                               bool supported) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto found = impl_->domains.find(domain.value());
  if (found == impl_->domains.end()) return Status::failure("synthetic.unknown_domain");
  SyntheticDomainConfig& config = found->second;
  std::vector<OperationClassId>& operations = config.capability.operations;
  const auto existing = std::find(operations.begin(), operations.end(), operation);
  if (supported && existing == operations.end()) {
    operations.push_back(operation);
  } else if (!supported && existing != operations.end()) {
    operations.erase(existing);
  }
  std::sort(operations.begin(), operations.end());
  config.capability.canonicalize();
  config.capability_generation = config.capability_generation.published()
                                     ? config.capability_generation.next()
                                     : CapabilityGeneration::first();
  config.evidence_generation = config.evidence_generation.published()
                                   ? config.evidence_generation.next()
                                   : EvidenceGeneration::first();
  return Status::success();
}

Status SyntheticBackend::set_load(ExecutionDomainId domain, std::uint32_t utilization_percent,
                                  std::uint32_t congestion_percent, std::uint32_t queue_depth,
                                  std::uint32_t in_flight, std::uint64_t observed_latency_ns) {
  if (utilization_percent > 100 || congestion_percent > 100) {
    return Status::failure("synthetic.invalid_load");
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto found = impl_->domains.find(domain.value());
  if (found == impl_->domains.end()) return Status::failure("synthetic.unknown_domain");
  SyntheticDomainConfig& config = found->second;
  config.utilization_percent = utilization_percent;
  config.congestion_percent = congestion_percent;
  config.queue_depth = queue_depth;
  config.in_flight = in_flight;
  config.observed_latency_ns = observed_latency_ns;
  config.dynamic_evidence_published = true;
  config.capability_generation = config.capability_generation.published()
                                     ? config.capability_generation.next()
                                     : CapabilityGeneration::first();
  return Status::success();
}

Status SyntheticBackend::set_health(ExecutionDomainId domain, bool healthy, bool ready,
                                    bool accepting) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto found = impl_->domains.find(domain.value());
  if (found == impl_->domains.end()) return Status::failure("synthetic.unknown_domain");
  SyntheticDomainConfig& config = found->second;
  config.healthy = healthy;
  config.ready = ready;
  config.accepting = accepting;
  config.dynamic_evidence_published = config.dynamic_evidence_published || healthy;
  config.capability_generation = config.capability_generation.published()
                                     ? config.capability_generation.next()
                                     : CapabilityGeneration::first();
  config.evidence_generation = config.evidence_generation.published()
                                   ? config.evidence_generation.next()
                                   : EvidenceGeneration::first();
  return Status::success();
}

Status SyntheticBackend::set_locality(ExecutionDomainId domain, const DomainLocality& locality) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto found = impl_->domains.find(domain.value());
  if (found == impl_->domains.end()) return Status::failure("synthetic.unknown_domain");
  found->second.locality = locality;
  if (!found->second.locality.generation.published()) {
    found->second.locality.generation = LocalityGeneration::first();
  }
  return Status::success();
}

Status SyntheticBackend::set_topology(ExecutionDomainId domain, const DomainTopology& topology) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto found = impl_->domains.find(domain.value());
  if (found == impl_->domains.end()) return Status::failure("synthetic.unknown_domain");
  found->second.topology = topology;
  if (!found->second.topology.generation.published()) {
    found->second.topology.generation = TopologyGeneration::first();
  }
  return Status::success();
}

Status SyntheticBackend::set_compatibility(ExecutionDomainId domain,
                                           const DomainCompatibility& compatibility) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto found = impl_->domains.find(domain.value());
  if (found == impl_->domains.end()) return Status::failure("synthetic.unknown_domain");
  found->second.compatibility = compatibility;
  if (!found->second.compatibility.generation.published()) {
    found->second.compatibility.generation = CompatibilityGeneration::first();
  }
  return Status::success();
}

Status SyntheticBackend::set_capacity(ExecutionDomainId domain, const CapacityVector& capacity) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto found = impl_->domains.find(domain.value());
  if (found == impl_->domains.end()) return Status::failure("synthetic.unknown_domain");
  found->second.capacity = capacity;
  return Status::success();
}

Status SyntheticBackend::set_worker(ExecutionDomainId domain, WorkerId worker, WorkerBootId boot) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto found = impl_->domains.find(domain.value());
  if (found == impl_->domains.end()) return Status::failure("synthetic.unknown_domain");
  found->second.worker = worker;
  found->second.worker_boot = boot;
  return Status::success();
}

Status SyntheticBackend::set_domain_generation(ExecutionDomainId domain,
                                               ExecutionDomainGeneration generation) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto found = impl_->domains.find(domain.value());
  if (found == impl_->domains.end()) return Status::failure("synthetic.unknown_domain");
  if (generation.value() < found->second.domain_generation.value()) {
    return Status::failure("synthetic.generation_regression");
  }
  found->second.domain_generation = generation;
  return Status::success();
}

Status SyntheticBackend::set_backend_generation(ExecutionDomainId domain,
                                                BackendGeneration generation) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto found = impl_->domains.find(domain.value());
  if (found == impl_->domains.end()) return Status::failure("synthetic.unknown_domain");
  found->second.backend_generation = generation;
  return Status::success();
}

Status SyntheticBackend::withdraw_dynamic_evidence(ExecutionDomainId domain) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto found = impl_->domains.find(domain.value());
  if (found == impl_->domains.end()) return Status::failure("synthetic.unknown_domain");
  found->second.dynamic_evidence_published = false;
  found->second.healthy = false;
  found->second.ready = false;
  found->second.accepting = false;
  return Status::success();
}

void SyntheticBackend::set_fault(SyntheticFault fault, std::uint32_t count) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->faults[fault] = count;
}

void SyntheticBackend::clear_faults() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->faults.clear();
}

std::vector<ExecutionDomainRecord> SyntheticBackend::discover_domains() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  std::vector<ExecutionDomainRecord> out;
  out.reserve(impl_->domains.size());
  for (const auto& entry : impl_->domains) out.push_back(impl_->record_of(entry.second));
  return out;
}

Checked<CapabilityRecord> SyntheticBackend::query_capability(ExecutionDomainId domain) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto found = impl_->domains.find(domain.value());
  if (found == impl_->domains.end()) {
    return Checked<CapabilityRecord>::bad("synthetic.unknown_domain");
  }
  const ExecutionDomainRecord record = impl_->record_of(found->second);
  return Checked<CapabilityRecord>::good(record.capability);
}

Checked<DomainLoadEvidence> SyntheticBackend::query_load(ExecutionDomainId domain) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto found = impl_->domains.find(domain.value());
  if (found == impl_->domains.end()) {
    return Checked<DomainLoadEvidence>::bad("synthetic.unknown_domain");
  }
  const ExecutionDomainRecord record = impl_->record_of(found->second);
  if (!record.load.published()) {
    return Checked<DomainLoadEvidence>::bad("synthetic.evidence_withheld");
  }
  return Checked<DomainLoadEvidence>::good(record.load);
}

Status SyntheticBackend::reserve(ExecutionDomainId domain, ReservationId id,
                                 const ResourceAmounts& amounts) {
  (void)id;
  (void)amounts;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->domains.find(domain.value()) == impl_->domains.end()) {
    return Status::failure("synthetic.unknown_domain");
  }
  if (impl_->take_fault() == SyntheticFault::kReserveRefused) {
    return Status::failure("synthetic.reserve_refused");
  }
  return Status::success();
}

Status SyntheticBackend::release(ExecutionDomainId domain, ReservationId id) {
  (void)id;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->domains.find(domain.value()) == impl_->domains.end()) {
    return Status::failure("synthetic.unknown_domain");
  }
  return Status::success();
}

ExecutionOutcome SyntheticBackend::execute(const ExecutionInvocation& invocation) {
  SyntheticFault fault = SyntheticFault::kNone;
  std::uint64_t synthetic_latency = 0;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto found = impl_->domains.find(invocation.domain.value());
    if (found == impl_->domains.end()) {
      return ExecutionOutcome::failed(FailureKind::kDomainUnavailable,
                                      "synthetic domain is not registered");
    }
    synthetic_latency = found->second.observed_latency_ns;
    fault = impl_->take_fault();
  }
  executed_ += 1;
  switch (fault) {
    case SyntheticFault::kRefuseExecution:
      return ExecutionOutcome::failed(FailureKind::kPreDispatchRejection,
                                      "synthetic backend refused execution");
    case SyntheticFault::kExecutionFailure:
      return ExecutionOutcome::failed(FailureKind::kExecutionFailure,
                                      "synthetic execution failure");
    case SyntheticFault::kAmbiguousOutcome:
      return ExecutionOutcome::unknown("synthetic backend reported an unknown outcome");
    case SyntheticFault::kIntegrityFailure: {
      ExecutionOutcome outcome = ExecutionOutcome::failed(FailureKind::kIntegrityFailure,
                                                          "synthetic integrity failure");
      outcome.executed = true;
      outcome.result_digest = 0xDEADBEEFULL;
      return outcome;
    }
    case SyntheticFault::kReserveRefused:
    case SyntheticFault::kNone: break;
  }
  // The synthetic domain performs the same deterministic host-side transform the
  // real CPU path performs, so results remain verifiable across domain classes.
  ExecutionOutcome outcome =
      execute_operation_on_host(invocation, OperationClassRegistry::builtins());
  if (outcome.duration_ns == 0) outcome.duration_ns = synthetic_latency;
  outcome.detail += outcome.detail.empty() ? "synthetic" : " synthetic";
  return outcome;
}

Status SyntheticBackend::cancel(ExecutionAttemptId id) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  (void)id;
  impl_->cancellation_requests += 1;
  return Status::success();
}

SyntheticDomainConfig make_synthetic_domain(ExecutionDomainId id, ExecutionDomainType type,
                                            std::string name, bool authoritative) {
  SyntheticDomainConfig config;
  config.id = id;
  config.type = type;
  config.name = name.empty() ? std::string("synthetic.") + std::to_string(id.value())
                             : std::move(name);
  config.provenance = Provenance::kSynthetic;
  config.domain_generation = ExecutionDomainGeneration::first();
  config.capability_generation = CapabilityGeneration::first();
  config.evidence_generation = EvidenceGeneration::first();
  config.backend_generation = BackendGeneration::first();
  config.authoritative = authoritative;
  config.parent_device = config.name + ".device";
  config.parent_host = detail::host_name();
  config.topology.generation = TopologyGeneration::first();
  config.topology.host_node = detail::host_name();
  config.topology.fabric = "synthetic.fabric";
  config.locality.generation = LocalityGeneration::first();
  config.locality.class_to_payload = LocalityClass::kSameHost;
  config.locality.numa_node = 0;
  config.locality.host_index = 0;
  config.locality.pcie_root_complex = 0;
  config.locality.pcie_switch = 0;
  config.locality.has_local_nic = type == ExecutionDomainType::kNic ||
                                  type == ExecutionDomainType::kSmartNic ||
                                  type == ExecutionDomainType::kDpu;
  config.compatibility.generation = CompatibilityGeneration::first();
  config.compatibility.driver_backend_version = 1;
  config.compatibility.firmware_generation = 1;
  config.compatibility.protocol_version = 1;
  config.compatibility.backend_family = "synthetic." + std::string(to_string(type));
  config.isolation = type == ExecutionDomainType::kCpu ? IsolationClass::kSharedProcess
                                                       : IsolationClass::kSeparateExecutionContext;
  config.capability = detail::make_host_executor_capability(
      kMaxDispatchPayloadBytes, 256, 8, "synthetic." + std::string(to_string(type)),
      memory_bit(MemoryDomain::kHost) | memory_bit(MemoryDomain::kPinnedHost) |
          memory_bit(MemoryDomain::kDeviceLocal) | memory_bit(MemoryDomain::kNicOnboard) |
          memory_bit(MemoryDomain::kSmartNicOnboard) | memory_bit(MemoryDomain::kDpuOnboard),
      1);
  config.capacity[static_cast<std::size_t>(ResourceKind::kExecutionSlot)] = 8;
  config.capacity[static_cast<std::size_t>(ResourceKind::kQueueDepth)] = 256;
  config.capacity[static_cast<std::size_t>(ResourceKind::kDescriptorRing)] = 64;
  config.capacity[static_cast<std::size_t>(ResourceKind::kProcessingContext)] = 8;
  config.capacity[static_cast<std::size_t>(ResourceKind::kDeviceMemoryBytes)] = 1ULL << 30;
  config.capacity[static_cast<std::size_t>(ResourceKind::kScratchMemoryBytes)] = 1ULL << 28;
  config.observed_latency_ns = 1000;
  config.throughput_bytes_per_second = 1ULL << 30;
  return config;
}

}  // namespace tos
