// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "tos/backends/cpu_backend.hpp"

#include <atomic>
#include <thread>

#include "domain_factory.hpp"

#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace tos {
namespace {

std::string brand_string() {
#if defined(_MSC_VER)
  int registers[4] = {0, 0, 0, 0};
  char brand[49] = {0};
  for (int leaf = 0; leaf < 3; ++leaf) {
    __cpuid(registers, static_cast<int>(0x80000002U + static_cast<unsigned>(leaf)));
    for (int i = 0; i < 4; ++i) {
      const auto value = static_cast<std::uint32_t>(registers[i]);
      brand[leaf * 16 + i * 4 + 0] = static_cast<char>(value & 0xFFU);
      brand[leaf * 16 + i * 4 + 1] = static_cast<char>((value >> 8) & 0xFFU);
      brand[leaf * 16 + i * 4 + 2] = static_cast<char>((value >> 16) & 0xFFU);
      brand[leaf * 16 + i * 4 + 3] = static_cast<char>((value >> 24) & 0xFFU);
    }
  }
  std::string out(brand);
  const std::size_t first = out.find_first_not_of(' ');
  if (first == std::string::npos) return "unknown";
  out = out.substr(first);
  while (!out.empty() && out.back() == ' ') out.pop_back();
  std::string sanitized;
  for (char c : out) {
    const char lowered = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    if ((lowered >= 'a' && lowered <= 'z') || (lowered >= '0' && lowered <= '9')) {
      sanitized.push_back(lowered);
    } else if (lowered == ' ' || lowered == '-' || lowered == '(' || lowered == ')' ||
               lowered == '@' || lowered == '.') {
      sanitized.push_back('.');
    }
  }
  if (sanitized.size() > kMaxNameLength) sanitized.resize(kMaxNameLength);
  return sanitized.empty() ? std::string("unknown") : sanitized;
#else
  return "unknown";
#endif
}

}  // namespace

struct CpuBackend::Impl {
  Options options;
  ExecutionDomainRecord record;
  std::atomic<std::uint64_t> executed{0};
  std::atomic<std::uint64_t> in_flight{0};
  std::atomic<std::uint64_t> publication{0};
};

CpuBackend::CpuBackend(Options options) : impl_(std::make_unique<Impl>()) {
  if (options.max_concurrency == 0) options.max_concurrency = hardware_threads();
  impl_->options = std::move(options);
  impl_->record = describe(impl_->options);
}

CpuBackend::~CpuBackend() = default;

std::string_view CpuBackend::name() const noexcept { return impl_->options.name; }

BackendGeneration CpuBackend::generation() const { return impl_->record.backend_generation; }

std::uint32_t CpuBackend::hardware_threads() {
  const unsigned int count = std::thread::hardware_concurrency();
  return count == 0 ? 1U : count;
}

std::string CpuBackend::cpu_brand() { return brand_string(); }

std::uint64_t CpuBackend::executed_count() const noexcept {
  return impl_->executed.load(std::memory_order_relaxed);
}

ExecutionDomainRecord CpuBackend::describe(const Options& options) {
  ExecutionDomainRecord record;
  record.id = options.domain_id;
  record.generation = ExecutionDomainGeneration::first();
  record.type = ExecutionDomainType::kCpu;
  record.name = options.name;
  record.parent_device = "cpu." + cpu_brand();
  record.parent_host = options.parent_host.empty() ? detail::host_name() : options.parent_host;
  record.worker = options.worker;
  record.worker_boot = options.worker_boot;
  record.provenance = Provenance::kReal;
  record.registration_sequence = options.domain_id.value();
  record.capability.domain = options.domain_id;
  record.capability.generation = CapabilityGeneration::first();
  record.capability.evidence = EvidenceGeneration::first();
  record.capability.provenance = Provenance::kReal;
  record.capability.authoritative = true;
  record.capability.capability = detail::make_host_executor_capability(
      options.max_payload_bytes, options.queue_capacity,
      options.max_concurrency == 0 ? hardware_threads() : options.max_concurrency, "cpu.host",
      memory_bit(MemoryDomain::kHost) | memory_bit(MemoryDomain::kPinnedHost), 1);
  record.load.load_generation = LoadGeneration::first();
  record.load.queue_generation = QueueGeneration::first();
  record.load.health_generation = HealthGeneration::first();
  record.load.evidence = EvidenceGeneration::first();
  record.load.provenance = Provenance::kReal;
  record.load.healthy = true;
  record.load.ready = true;
  record.load.accepting = true;
  record.load.utilization_percent = 0;
  record.load.congestion_percent = 0;
  record.load.queue_depth = 0;
  record.load.in_flight = 0;
  record.load.observed_latency_ns = 0;
  record.locality.generation = LocalityGeneration::first();
  record.locality.class_to_payload = LocalityClass::kDomainLocal;
  record.locality.numa_node = 0;
  record.locality.host_index = 0;
  record.locality.has_local_nic = true;  // the host CPU can reach the host NICs
  record.topology.generation = TopologyGeneration::first();
  record.topology.host_node = record.parent_host;
  record.topology.fabric.clear();
  record.compatibility.generation = CompatibilityGeneration::first();
  record.compatibility.driver_backend_version = 1;
  record.compatibility.firmware_generation = 0;
  record.compatibility.protocol_version = 1;
  record.compatibility.backend_family = "cpu.host";
  record.isolation = IsolationClass::kSharedProcess;
  const std::uint64_t threads = record.capability.capability.max_concurrency;
  record.capacity[static_cast<std::size_t>(ResourceKind::kExecutionSlot)] = threads;
  record.capacity[static_cast<std::size_t>(ResourceKind::kQueueDepth)] = options.queue_capacity;
  record.capacity[static_cast<std::size_t>(ResourceKind::kDescriptorRing)] = 0;
  record.capacity[static_cast<std::size_t>(ResourceKind::kProcessingContext)] = threads;
  const std::uint64_t physical = detail::physical_memory_bytes();
  record.capacity[static_cast<std::size_t>(ResourceKind::kScratchMemoryBytes)] =
      physical == 0 ? 0 : physical / 8;
  record.capacity[static_cast<std::size_t>(ResourceKind::kDeviceMemoryBytes)] = 0;
  record.backend_generation = BackendGeneration::first();
  return record;
}

std::vector<ExecutionDomainRecord> CpuBackend::discover_domains() {
  ExecutionDomainRecord record = impl_->record;
  // Every discovery is a fresh publication: volatile evidence carries a new
  // generation so that no plan can bind stale load state.
  const std::uint64_t publication =
      impl_->publication.fetch_add(1, std::memory_order_relaxed) + 1;
  record.load.load_generation = LoadGeneration(publication);
  record.load.queue_generation = QueueGeneration(publication);
  record.load.health_generation = HealthGeneration(publication);
  record.load.evidence = EvidenceGeneration(publication);
  const std::uint32_t in_flight = static_cast<std::uint32_t>(
      impl_->in_flight.load(std::memory_order_relaxed));
  record.load.in_flight = in_flight;
  record.load.queue_depth = 0;
  const std::uint64_t slots = std::max<std::uint64_t>(
      1, record.capacity[static_cast<std::size_t>(ResourceKind::kExecutionSlot)]);
  record.load.utilization_percent =
      static_cast<std::uint32_t>(std::min<std::uint64_t>(100, (in_flight * 100) / slots));
  return {record};
}

Checked<CapabilityRecord> CpuBackend::query_capability(ExecutionDomainId domain) {
  if (domain != impl_->record.id) return Checked<CapabilityRecord>::bad("cpu.unknown_domain");
  return Checked<CapabilityRecord>::good(impl_->record.capability);
}

Checked<DomainLoadEvidence> CpuBackend::query_load(ExecutionDomainId domain) {
  if (domain != impl_->record.id) return Checked<DomainLoadEvidence>::bad("cpu.unknown_domain");
  ExecutionDomainRecord record = discover_domains().front();
  return Checked<DomainLoadEvidence>::good(record.load);
}

Status CpuBackend::reserve(ExecutionDomainId domain, ReservationId id,
                           const ResourceAmounts& amounts) {
  (void)id;
  (void)amounts;
  if (domain != impl_->record.id) return Status::failure("cpu.unknown_domain");
  return Status::success();
}

Status CpuBackend::release(ExecutionDomainId domain, ReservationId id) {
  (void)id;
  if (domain != impl_->record.id) return Status::failure("cpu.unknown_domain");
  return Status::success();
}

ExecutionOutcome CpuBackend::execute(const ExecutionInvocation& invocation) {
  impl_->in_flight.fetch_add(1, std::memory_order_relaxed);
  ExecutionOutcome outcome =
      execute_operation_on_host(invocation, OperationClassRegistry::builtins());
  impl_->in_flight.fetch_sub(1, std::memory_order_relaxed);
  impl_->executed.fetch_add(1, std::memory_order_relaxed);
  return outcome;
}

}  // namespace tos
