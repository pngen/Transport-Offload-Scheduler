// Real CUDA accelerator execution backend: host side.
//
// This translation unit contains no CUDA call and includes no CUDA header. It reaches
// the device through the private bridge implemented in src/backends/cuda_kernels.cu,
// which is the only translation unit compiled by nvcc. Everything the header promises
// about real allocations, real copies, a real kernel, a real synchronisation, the host
// parity check and real cleanup is implemented across those two files.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "tos/backends/cuda_backend.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "domain_factory.hpp"
#include "tos/util/crc32c.hpp"

/* Private bridge status codes. This block must stay identical to the copy in
   src/backends/cuda_kernels.cu: the two translation units are the only users of the
   bridge and there is deliberately no shared CUDA header. */
#define TOS_CUDA_OK 0
#define TOS_CUDA_NOT_COMPILED 1
#define TOS_CUDA_NO_DEVICE 2
#define TOS_CUDA_DRIVER_ERROR 3
#define TOS_CUDA_INVALID_DEVICE 4
#define TOS_CUDA_ALREADY_OPEN 5
#define TOS_CUDA_NOT_OPEN 6
#define TOS_CUDA_ALLOCATION_FAILED 7
#define TOS_CUDA_COPY_FAILED 8
#define TOS_CUDA_LAUNCH_FAILED 9
#define TOS_CUDA_SYNCHRONIZE_FAILED 10
#define TOS_CUDA_INVALID_ARGUMENT 11
#define TOS_CUDA_INTERNAL_ERROR 12

/* Private bridge. Plain C types only; definitions live in src/backends/cuda_kernels.cu. */
extern "C" int tos_cuda_probe(int device_index, char* error_out, int error_capacity);
extern "C" int tos_cuda_open(int device_index, char* error_out, int error_capacity);
extern "C" void tos_cuda_close(void);
extern "C" int tos_cuda_device_name(int device_index, char* name_out, int name_capacity);
extern "C" int tos_cuda_compute_capability(int device_index, int* major, int* minor);
extern "C" int tos_cuda_total_memory(int device_index, unsigned long long* bytes);
extern "C" int tos_cuda_driver_version(int* version);
extern "C" int tos_cuda_runtime_version(int* version);
extern "C" int tos_cuda_memory_info(unsigned long long* free_bytes, unsigned long long* total_bytes);
extern "C" unsigned long long tos_cuda_outstanding_bytes(void);
extern "C" unsigned long long tos_cuda_kernel_launches(void);
extern "C" unsigned long long tos_cuda_device_bytes(void);
extern "C" int tos_cuda_checksum_crc32c(const unsigned char* data, unsigned long long length,
                                        unsigned int* crc_out, unsigned long long* total_ns,
                                        unsigned long long* kernel_ns, char* error_out,
                                        int error_capacity);

namespace tos {
namespace {

constexpr std::size_t kErrorCapacity = 256;

/// Real device facts, read from the CUDA runtime by the process's single device
/// context. "ready" is set only after a device was really initialised.
struct DeviceFacts {
  bool ready{false};
  std::string reason;
  std::string name;
  int major{0};
  int minor{0};
  std::uint32_t compute_capability{0};
  std::uint64_t memory_bytes{0};
  std::uint32_t driver_version{0};
  std::uint32_t runtime_version{0};
};

/// Lower-case, dot-separated, bounded device label. Never invents characters.
std::string sanitize_label(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char raw : text) {
    const char lowered = (raw >= 'A' && raw <= 'Z') ? static_cast<char>(raw - 'A' + 'a') : raw;
    if ((lowered >= 'a' && lowered <= 'z') || (lowered >= '0' && lowered <= '9')) {
      out.push_back(lowered);
    } else if (!out.empty() && out.back() != '.') {
      out.push_back('.');
    }
  }
  while (!out.empty() && out.back() == '.') out.pop_back();
  if (out.size() > kMaxNameLength) out.resize(kMaxNameLength);
  return out.empty() ? std::string("unknown") : out;
}

/// Keeps an in-flight counter exact on every return path.
struct counter_guard {
  std::atomic<std::uint64_t>& counter;
  explicit counter_guard(std::atomic<std::uint64_t>& value) : counter(value) {
    counter.fetch_add(1, std::memory_order_relaxed);
  }
  ~counter_guard() { counter.fetch_sub(1, std::memory_order_relaxed); }
  counter_guard(const counter_guard&) = delete;
  counter_guard& operator=(const counter_guard&) = delete;
};

std::string backend_family_of(const DeviceFacts& facts) {
  if (!facts.ready) return "cuda.unavailable";
  return "cuda.sm" + std::to_string(facts.compute_capability);
}

/// Build the published domain record. When the device was not initialised the record
/// proves nothing: no operation class, no proven flag, no addressable memory, no
/// capacity, not healthy, provenance UNSUPPORTED.
ExecutionDomainRecord describe_record(const CudaBackend::Options& options, const DeviceFacts& facts) {
  ExecutionDomainRecord record;
  record.id = options.domain_id;
  record.generation = ExecutionDomainGeneration::first();
  record.type = ExecutionDomainType::kAccelerator;
  record.name = options.name;
  record.parent_host = options.parent_host.empty() ? detail::host_name() : options.parent_host;
  record.worker = options.worker;
  record.worker_boot = options.worker_boot;
  record.registration_sequence = options.domain_id.value();
  record.backend_generation = BackendGeneration::first();
  record.capability.domain = options.domain_id;
  record.capability.generation = CapabilityGeneration::first();
  record.capability.evidence = EvidenceGeneration::first();
  record.capability.publisher = options.worker_boot;
  record.load.load_generation = LoadGeneration::first();
  record.load.queue_generation = QueueGeneration::first();
  record.load.health_generation = HealthGeneration::first();
  record.load.evidence = EvidenceGeneration::first();
  record.locality.generation = LocalityGeneration::first();
  record.topology.generation = TopologyGeneration::first();
  record.topology.host_node = record.parent_host;
  record.compatibility.generation = CompatibilityGeneration::first();

  if (!facts.ready) {
    record.provenance = Provenance::kUnsupported;
    record.parent_device = "gpu.unavailable";
    record.capability.provenance = Provenance::kUnsupported;
    record.capability.authoritative = false;
    CapabilitySet& capability = record.capability.capability;
    capability.flags = 0;
    capability.unproven_flags = 0;
    for (const CapabilityFlag flag : all_capability_flags()) {
      capability.unproven_flags |= flag_bit(flag);
    }
    capability.operations.clear();
    capability.memory_domains = 0;
    capability.min_payload_bytes = 0;
    capability.max_payload_bytes = 0;
    capability.alignment_bytes = 8;
    capability.max_concurrency = 1;
    capability.queue_capacity = 0;
    capability.transport_classes = 0;
    capability.payload_classes = 0;
    capability.protocol_version = 1;
    capability.driver_backend_version = 0;
    capability.firmware_generation = 0;
    capability.accelerator_arch = 0;
    capability.backend_family = backend_family_of(facts);
    capability.canonicalize();
    record.load.provenance = Provenance::kUnsupported;
    record.load.healthy = false;
    record.load.ready = false;
    record.load.accepting = false;
    record.locality.class_to_payload = LocalityClass::kUnknown;
    record.locality.has_local_nic = false;
    record.compatibility.driver_backend_version = 0;
    record.compatibility.firmware_generation = 0;
    record.compatibility.protocol_version = 1;
    record.compatibility.accelerator_arch = 0;
    record.compatibility.backend_family = capability.backend_family;
    record.isolation = IsolationClass::kUnknown;
    return record;
  }

  record.provenance = Provenance::kReal;
  record.parent_device = "gpu." + sanitize_label(facts.name);
  record.capability.provenance = Provenance::kReal;
  record.capability.authoritative = true;
  // The capability set starts from the host executor's shape (memory domains, transport
  // classes, payload classes, protocol version) and is then narrowed to what this
  // device path really proves: one operation class and one proven flag. Every flag the
  // host executor sets for a class this device does not implement becomes UNPROVEN.
  const std::uint64_t max_payload =
      facts.memory_bytes == 0
          ? options.max_payload_bytes
          : std::min<std::uint64_t>(options.max_payload_bytes, facts.memory_bytes);
  CapabilitySet& capability = record.capability.capability;
  capability = make_host_executor_capability(
      max_payload, options.queue_capacity, options.max_concurrency, backend_family_of(facts),
      memory_bit(MemoryDomain::kHost) | memory_bit(MemoryDomain::kPinnedHost) |
          memory_bit(MemoryDomain::kDeviceLocal) | memory_bit(MemoryDomain::kPeerDevice),
      facts.driver_version);
  capability.unproven_flags |= (capability.flags & ~flag_bit(CapabilityFlag::kChecksum));
  capability.flags = flag_bit(CapabilityFlag::kChecksum);
  capability.operations = {opclass::checksum_crc32c()};
  capability.accelerator_arch = facts.compute_capability;
  capability.canonicalize();

  record.load.provenance = Provenance::kReal;
  record.load.healthy = true;
  record.load.ready = true;
  record.load.accepting = true;
  // The payload is staged from host memory, so it is on this host but not in device
  // memory. NUMA node, PCIe path and local-NIC facts are not reported by the CUDA
  // runtime API and are left at their unknown values instead of being guessed.
  record.locality.class_to_payload = LocalityClass::kSameHost;
  record.locality.numa_node = 0;
  record.locality.host_index = 0;
  record.locality.has_local_nic = false;

  record.compatibility.driver_backend_version = facts.driver_version;
  record.compatibility.firmware_generation = 0;
  record.compatibility.protocol_version = 1;
  record.compatibility.accelerator_arch = facts.compute_capability;
  record.compatibility.backend_family = capability.backend_family;

  record.isolation = IsolationClass::kSharedProcess;
  record.capacity[static_cast<std::size_t>(ResourceKind::kExecutionSlot)] =
      options.max_concurrency;
  record.capacity[static_cast<std::size_t>(ResourceKind::kQueueDepth)] = options.queue_capacity;
  record.capacity[static_cast<std::size_t>(ResourceKind::kProcessingContext)] =
      options.max_concurrency;
  record.capacity[static_cast<std::size_t>(ResourceKind::kDeviceMemoryBytes)] = facts.memory_bytes;
  record.capacity[static_cast<std::size_t>(ResourceKind::kScratchMemoryBytes)] = max_payload;
  return record;
}

}  // namespace

struct CudaBackend::Impl {
  Options options;
  DeviceFacts facts;
  ExecutionDomainRecord record;
  std::atomic<std::uint64_t> executed{0};
  std::atomic<std::uint64_t> in_flight{0};
  std::atomic<std::uint64_t> busy{0};
  std::atomic<std::uint64_t> publication{0};
  std::atomic<std::uint64_t> last_duration_ns{0};
  std::atomic<std::uint64_t> last_bytes{0};
  bool closed{false};
  std::mutex reservation_mutex;
  std::map<std::uint64_t, std::uint64_t> device_reservations;
  std::uint64_t reserved_device_bytes{0};

  /// Measured load evidence: how many execution requests this backend is holding, how
  /// many of them had to wait for the device, and what the last device path really
  /// measured. Nothing here is modelled.
  [[nodiscard]] DomainLoadEvidence load_evidence(std::uint64_t publication_sequence) const {
    DomainLoadEvidence load;
    load.load_generation = LoadGeneration(publication_sequence);
    load.queue_generation = QueueGeneration(publication_sequence);
    load.health_generation = HealthGeneration(publication_sequence);
    load.evidence = EvidenceGeneration(publication_sequence);
    load.provenance = facts.ready ? Provenance::kReal : Provenance::kUnsupported;
    const std::uint64_t in_flight_count = in_flight.load(std::memory_order_relaxed);
    const std::uint64_t busy_count = busy.load(std::memory_order_relaxed);
    const std::uint64_t waiting = in_flight_count > busy_count ? in_flight_count - busy_count : 0;
    const std::uint64_t slots = std::max<std::uint64_t>(1, options.max_concurrency);
    load.in_flight = static_cast<std::uint32_t>(in_flight_count);
    load.queue_depth = static_cast<std::uint32_t>(std::min<std::uint64_t>(waiting, 0xFFFFFFFFULL));
    load.utilization_percent =
        static_cast<std::uint32_t>(std::min<std::uint64_t>(100, (in_flight_count * 100) / slots));
    load.congestion_percent = static_cast<std::uint32_t>(
        in_flight_count == 0 ? 0 : std::min<std::uint64_t>(100, (waiting * 100) / in_flight_count));
    const std::uint64_t duration_ns = last_duration_ns.load(std::memory_order_relaxed);
    const std::uint64_t bytes = last_bytes.load(std::memory_order_relaxed);
    load.observed_latency_ns = duration_ns;
    load.throughput_bytes_per_second = duration_ns == 0 ? 0 : (bytes * 1000000000ULL) / duration_ns;
    load.healthy = facts.ready;
    load.ready = facts.ready;
    load.accepting = facts.ready;
    return load;
  }
};

CudaBackend::CudaBackend(Options options) : impl_(std::make_unique<Impl>()) {
  if (options.max_concurrency == 0) options.max_concurrency = 1;
  impl_->options = std::move(options);

  char error[kErrorCapacity] = {0};
  const int status = tos_cuda_open(impl_->options.device_index, error, kErrorCapacity);
  if (status == TOS_CUDA_OK) {
    impl_->facts.ready = true;
    char name[128] = {0};
    if (tos_cuda_device_name(impl_->options.device_index, name, 128) == TOS_CUDA_OK) {
      impl_->facts.name = name;
    }
    int major = 0;
    int minor = 0;
    if (tos_cuda_compute_capability(impl_->options.device_index, &major, &minor) == TOS_CUDA_OK) {
      impl_->facts.major = major;
      impl_->facts.minor = minor;
      impl_->facts.compute_capability = static_cast<std::uint32_t>(major * 10 + minor);
    }
    unsigned long long memory_bytes = 0;
    if (tos_cuda_total_memory(impl_->options.device_index, &memory_bytes) == TOS_CUDA_OK) {
      impl_->facts.memory_bytes = memory_bytes;
    }
    int driver_version = 0;
    if (tos_cuda_driver_version(&driver_version) == TOS_CUDA_OK && driver_version > 0) {
      impl_->facts.driver_version = static_cast<std::uint32_t>(driver_version);
    }
    int runtime_version = 0;
    if (tos_cuda_runtime_version(&runtime_version) == TOS_CUDA_OK && runtime_version > 0) {
      impl_->facts.runtime_version = static_cast<std::uint32_t>(runtime_version);
    }
    if (impl_->facts.name.empty()) impl_->facts.name = "cuda.device";
  } else {
    impl_->facts.ready = false;
    impl_->facts.reason = error[0] == '\0' ? std::string("the CUDA device could not be initialised")
                                            : std::string(error);
  }
  impl_->record = describe_record(impl_->options, impl_->facts);
}

CudaBackend::~CudaBackend() {
  if (impl_ && impl_->facts.ready) {
    tos_cuda_close();
    impl_->facts.ready = false;
  }
}

std::string_view CudaBackend::name() const noexcept { return impl_->options.name; }

Provenance CudaBackend::provenance() const noexcept {
  return impl_->facts.ready ? Provenance::kReal : Provenance::kUnsupported;
}

BackendGeneration CudaBackend::generation() const { return impl_->record.backend_generation; }

std::uint64_t CudaBackend::executed_count() const noexcept {
  return impl_->executed.load(std::memory_order_relaxed);
}

std::string CudaBackend::unavailable_reason() const { return impl_->facts.reason; }

std::vector<ExecutionDomainRecord> CudaBackend::discover_domains() {
  ExecutionDomainRecord record = impl_->record;
  // Every discovery is a fresh publication: volatile evidence carries a new generation
  // so that no plan can bind stale load state.
  const std::uint64_t publication =
      impl_->publication.fetch_add(1, std::memory_order_relaxed) + 1;
  record.load = impl_->load_evidence(publication);
  return {record};
}

Checked<CapabilityRecord> CudaBackend::query_capability(ExecutionDomainId domain) {
  if (domain != impl_->record.id) return Checked<CapabilityRecord>::bad("cuda.unknown_domain");
  return Checked<CapabilityRecord>::good(impl_->record.capability);
}

Checked<DomainLoadEvidence> CudaBackend::query_load(ExecutionDomainId domain) {
  if (domain != impl_->record.id) return Checked<DomainLoadEvidence>::bad("cuda.unknown_domain");
  const std::uint64_t publication =
      impl_->publication.fetch_add(1, std::memory_order_relaxed) + 1;
  return Checked<DomainLoadEvidence>::good(impl_->load_evidence(publication));
}

Status CudaBackend::reserve(ExecutionDomainId domain, ReservationId id,
                            const ResourceAmounts& amounts) {
  if (domain != impl_->record.id) return Status::failure("cuda.unknown_domain");
  if (!impl_->facts.ready) return Status::failure("cuda.unsupported", impl_->facts.reason);
  const std::uint64_t requested =
      amounts[static_cast<std::size_t>(ResourceKind::kDeviceMemoryBytes)];
  if (requested == 0) return Status::success();
  unsigned long long free_bytes = 0;
  unsigned long long total_bytes = 0;
  if (tos_cuda_memory_info(&free_bytes, &total_bytes) != TOS_CUDA_OK) {
    return Status::failure("cuda.memory_query_failed",
                           "free device memory could not be read from the device");
  }
  std::lock_guard<std::mutex> lock(impl_->reservation_mutex);
  const std::uint64_t already = impl_->reserved_device_bytes;
  if (requested > free_bytes || already > free_bytes - requested) {
    return Status::failure("cuda.insufficient_device_memory",
                           "the device cannot back the requested device-memory claim");
  }
  impl_->device_reservations[id.value()] = requested;
  impl_->reserved_device_bytes = already + requested;
  return Status::success();
}

Status CudaBackend::release(ExecutionDomainId domain, ReservationId id) {
  if (domain != impl_->record.id) return Status::failure("cuda.unknown_domain");
  std::lock_guard<std::mutex> lock(impl_->reservation_mutex);
  const auto found = impl_->device_reservations.find(id.value());
  if (found == impl_->device_reservations.end()) {
    return Status::failure("cuda.unknown_reservation",
                           "no device-memory claim was held under this reservation");
  }
  impl_->reserved_device_bytes -= found->second;
  impl_->device_reservations.erase(found);
  return Status::success();
}

ExecutionOutcome CudaBackend::execute(const ExecutionInvocation& invocation) {
  counter_guard in_flight(impl_->in_flight);
  if (!impl_->facts.ready || impl_->closed) {
    return ExecutionOutcome::failed(
        FailureKind::kBackendRejection,
        std::string("cuda.unavailable: ") +
            (impl_->facts.reason.empty() ? std::string("the device context is not open")
                                         : impl_->facts.reason));
  }
  if (invocation.domain != impl_->record.id) {
    return ExecutionOutcome::failed(FailureKind::kBackendRejection,
                                    "cuda.unknown_domain: the invocation names another domain");
  }
  const OperationClassId operation = invocation.operation_class;
  if (operation != opclass::checksum_crc32c()) {
    const OperationClassDescriptor* descriptor = OperationClassRegistry::builtins().find(operation);
    const std::string class_name =
        descriptor == nullptr ? std::string("an unregistered class") : descriptor->name;
    // No host fallback: a result produced on the host would make this domain's REAL
    // provenance a false statement about where the work ran.
    return ExecutionOutcome::failed(FailureKind::kBackendRejection,
                                    "cuda.device_path_does_not_implement: " + class_name);
  }
  const std::vector<std::uint8_t>& input = invocation.input;
  if (static_cast<std::uint64_t>(input.size()) > impl_->options.max_payload_bytes) {
    return ExecutionOutcome::failed(FailureKind::kBackendRejection,
                                    "cuda.payload_exceeds_device_bound");
  }

  // The reference result is computed on the host, outside every lock: it is the parity
  // baseline the device digest must reproduce exactly.
  const std::uint32_t host_crc = crc32c(input.data(), input.size());
  unsigned int device_crc = 0;
  unsigned long long total_ns = 0;
  unsigned long long kernel_ns = 0;
  char error[kErrorCapacity] = {0};
  int status = TOS_CUDA_INTERNAL_ERROR;
  {
    // Only the device phase counts as busy; waiting for the device does not.
    counter_guard busy(impl_->busy);
    status = tos_cuda_checksum_crc32c(input.data(), static_cast<unsigned long long>(input.size()),
                                      &device_crc, &total_ns, &kernel_ns, error, kErrorCapacity);
  }
  if (status != TOS_CUDA_OK) {
    return ExecutionOutcome::failed(
        FailureKind::kExecutionFailure,
        "cuda.checksum_crc32c failed (status " + std::to_string(status) +
            "): " + (error[0] == '\0' ? std::string("no detail") : std::string(error)));
  }
  impl_->last_duration_ns.store(total_ns, std::memory_order_relaxed);
  impl_->last_bytes.store(static_cast<std::uint64_t>(input.size()), std::memory_order_relaxed);
  impl_->executed.fetch_add(1, std::memory_order_relaxed);

  if (device_crc != host_crc) {
    // The device ran and produced a digest that disagrees with the host reference. The
    // result is reported as an integrity failure carrying the untrusted device value,
    // never as a successful execution.
    ExecutionOutcome mismatch = ExecutionOutcome::failed(
        FailureKind::kIntegrityFailure,
        "cuda.device_digest_disagrees_with_host_reference: device=" +
            format_id(static_cast<std::uint64_t>(device_crc)) +
            " host=" + format_id(static_cast<std::uint64_t>(host_crc)));
    mismatch.executed = true;
    mismatch.result_digest = static_cast<std::uint64_t>(device_crc);
    mismatch.bytes_processed = static_cast<std::uint64_t>(input.size());
    mismatch.duration_ns = total_ns;
    return mismatch;
  }

  ExecutionOutcome outcome = ExecutionOutcome::ok(static_cast<std::uint64_t>(device_crc),
                                                  static_cast<std::uint64_t>(input.size()), total_ns);
  outcome.output.resize(4);
  for (int index = 0; index < 4; ++index) {
    outcome.output[static_cast<std::size_t>(index)] =
        static_cast<std::uint8_t>((device_crc >> (8 * index)) & 0xFFU);
  }
  outcome.detail = "cuda " + impl_->facts.name + " sm_" +
                   std::to_string(impl_->facts.compute_capability) +
                   " device_path_ns=" + std::to_string(total_ns) +
                   " kernel_ns=" + std::to_string(kernel_ns) +
                   " crc32c=" + format_id(static_cast<std::uint64_t>(device_crc)) +
                   " host_reference=exact";
  return outcome;
}

Status CudaBackend::shutdown() {
  const std::uint64_t outstanding = tos_cuda_outstanding_bytes();
  if (impl_->facts.ready) {
    tos_cuda_close();
    impl_->facts.ready = false;
    impl_->closed = true;
  }
  if (outstanding != 0) {
    return Status::failure("cuda.leaked_device_memory",
                           "device or pinned memory was still allocated at shutdown");
  }
  return Status::success();
}

bool CudaBackend::available(std::int32_t device_index) noexcept {
  if (device_index != 0) return tos_cuda_probe(device_index, nullptr, 0) == TOS_CUDA_OK;
  static const bool kDefaultDeviceAvailable = tos_cuda_probe(0, nullptr, 0) == TOS_CUDA_OK;
  return kDefaultDeviceAvailable;
}

std::string CudaBackend::device_name(std::int32_t device_index) {
  char name[128] = {0};
  if (tos_cuda_device_name(device_index, name, 128) != TOS_CUDA_OK) return std::string();
  return std::string(name);
}

std::uint32_t CudaBackend::device_compute_capability(std::int32_t device_index) {
  int major = 0;
  int minor = 0;
  if (tos_cuda_compute_capability(device_index, &major, &minor) != TOS_CUDA_OK) return 0;
  if (major <= 0) return 0;
  return static_cast<std::uint32_t>(major * 10 + minor);
}

std::uint64_t CudaBackend::device_memory_bytes(std::int32_t device_index) {
  unsigned long long bytes = 0;
  if (tos_cuda_total_memory(device_index, &bytes) != TOS_CUDA_OK) return 0;
  return bytes;
}

std::uint64_t CudaBackend::outstanding_device_bytes() noexcept {
  return tos_cuda_outstanding_bytes();
}

std::uint64_t CudaBackend::kernel_launches() noexcept { return tos_cuda_kernel_launches(); }

std::uint64_t CudaBackend::device_bytes_copied() noexcept { return tos_cuda_device_bytes(); }

}  // namespace tos
