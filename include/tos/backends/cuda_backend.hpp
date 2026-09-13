// Real CUDA accelerator execution backend.
//
// WHAT THIS BACKEND ACTUALLY DOES
// -------------------------------
// Every execution the device path accepts performs, in order:
//   1. a real cudaMallocHost pinned block holding the staged input and the result slot,
//      filled by a host-to-pinned memcpy,
//   2. a real cudaMalloc device block holding the input image, the per-chunk digest
//      table and the folded result,
//   3. a real pinned-to-device cudaMemcpyAsync,
//   4. two real CUDA kernel launches: a per-chunk kernel that hashes contiguous slices
//      of the payload in parallel and folds them in shared memory, and a fold kernel
//      that combines the chunk digests into the digest of the whole buffer,
//   5. a real cudaStreamSynchronize on the owning stream,
//   6. a real device-to-pinned cudaMemcpyAsync of the result,
//   7. a comparison of the device result with the host reference result computed by
//      tos::crc32c over the same bytes (exact parity), and
//   8. cudaFree/cudaFreeHost of both allocations.
// Nothing in this list is simulated, cached from a previous run or inferred. If the
// device result does not match the host reference the call is reported as
// FailureKind::kIntegrityFailure with the untrusted device digest, never as success.
//
// PROVENANCE
// ----------
// provenance() is Provenance::kReal only when a device was really initialised by this
// process (device enumerated, primary context created, stream and events created).
// When CUDA is absent, the driver is older than the runtime, no device is present or
// initialisation fails, the backend reports Provenance::kUnsupported, publishes a
// domain record that proves no operation class and is not healthy, and every
// execute() call returns FailureKind::kBackendRejection. It never claims REAL.
//
// IMPLEMENTED OPERATION CLASSES
// -----------------------------
// The device path implements exactly one operation class: CHECKSUM_CRC32C, a
// table-free bitwise CRC32C (reflected Castagnoli polynomial 0x82F63B78, the same
// init/final convention as tos::crc32c). The published capability set contains
// exactly that class, so the scheduler refuses the other classes at eligibility
// instead of being told later.
//
// COMPRESS_RLE and DECOMPRESS_RLE are NOT implemented on the device and this backend
// deliberately does NOT fall back to the host executor for them: a result produced on
// the host would make the accelerator domain's REAL provenance a false statement
// about where the work ran. execute() returns FailureKind::kBackendRejection for every
// operation class other than CHECKSUM_CRC32C.
//
// RESULT SHAPE
// ------------
// CHECKSUM_CRC32C returns the CRC32C value in result_digest and the same value as four
// little-endian bytes in output, exactly as the host executor does, so the two paths
// are interchangeable for callers.
//
// DEVICE OWNERSHIP AND LOCKING
// ----------------------------
// A process owns at most one device context (device, primary context, stream, timing
// events). The context lives in src/backends/cuda_kernels.cu and is guarded by that
// file's own device mutex; this backend never holds a backend lock across a device
// call, never blocks on device work while holding any lock of its own, and never calls
// back into the scheduler. Host-side work that does not need the device (the reference
// CRC, the outcome post-processing) runs outside every lock.
//
// Because one stream is shared by all executions, the device path serialises them, and
// the published max_concurrency is 1. queue_capacity is 1: the backend accepts work
// without a software queue of its own, and contention is reported through the real
// in-flight counters in discover_domains().
//
// MACHINE-READABLE HONESTY OF THE PUBLISHED RECORD
// ------------------------------------------------
// * memory_domains = host | pinned_host | device_local | peer_device. host, pinned_host
//   and device_local are exercised by every executed call. peer_device follows the
//   runtime's addressability mask for ExecutionDomainType::kAccelerator
//   (tos::memory_domain_mask_for); it is a memory-model statement and is NOT proof
//   that a peer device exists on this host. No peer or P2P operation class is
//   published.
// * flags: only CapabilityFlag::kChecksum is proven. Every flag that
//   make_host_executor_capability() sets is moved to unproven_flags, because no other
//   class runs on this device path.
// * accelerator_arch / backend_family / max_payload_bytes / device memory capacity are
//   read from the device (compute capability, device name, totalGlobalMem). The
//   published max_payload_bytes is the smaller of the configured bound and the real
//   device memory, so a payload that cannot be staged is never advertised.
// * locality.numa_node, pcie_root_complex, pcie_switch and has_local_nic are left at
//   their "unknown/false" values: the CUDA runtime API does not report them and this
//   backend does not guess. The payload is staged from host memory, so the published
//   locality class is kSameHost, not kDomainLocal.
// * load evidence is measured: in_flight is the number of execution requests the
//   backend is holding, queue_depth is how many of them were waiting for the device,
//   observed_latency_ns and throughput_bytes_per_second are the last really observed
//   device-path duration and the bytes it processed. No sleep and no modelled number
//   is ever published.
//
// ALLOCATION ACCOUNTING
// ---------------------
// Every cudaMalloc/cudaMallocHost performed by the device path increments a process
// wide counter and every cudaFree/cudaFreeHost decrements it, so
// outstanding_device_bytes() reports the bytes still held by the CUDA allocator on
// behalf of this backend. It is zero after a call returns, on the success path and on
// every error path, because the device path releases each allocation through a scope
// guard. A non-zero value is a real leak, never a bookkeeping artefact.
//
// PLATFORM
// --------
// The kernels are compiled by nvcc for sm_120 (RTX 50 series) with a compute_75
// fallback; src/backends/cuda_kernels.cu is compiled by nvcc only. If that file is
// compiled by a host compiler instead, every entry point of its private bridge reports
// "no toolkit", so a build without CUDA can never claim a real device.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#ifndef TOS_BACKENDS_CUDA_BACKEND_HPP
#define TOS_BACKENDS_CUDA_BACKEND_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "tos/backends/backend.hpp"
#include "tos/core/domain.hpp"

namespace tos {

/// Execution backend for one NVIDIA CUDA device.
///
/// This header includes no CUDA header: the rest of the project builds and links
/// without a toolkit. The CUDA runtime is reached only through the private bridge
/// defined in src/backends/cuda_kernels.cu and declared in src/backends/cuda_backend.cpp.
class CudaBackend : public IExecutionBackend {
 public:
  struct Options {
    ExecutionDomainId domain_id{ExecutionDomainId(2)};
    std::string name{"cuda.0"};
    /// Upper bound on the payload a single execution may stage. The published
    /// capability is additionally bounded by the real device memory.
    std::uint64_t max_payload_bytes{kMaxDispatchPayloadBytes};
    /// Dispatch requests the backend accepts without a software queue of its own.
    std::uint32_t queue_capacity{1};
    /// Device executions that may be in flight at once. One device context and one
    /// stream are shared by the process, so the device path serialises executions.
    std::uint32_t max_concurrency{1};
    std::int32_t device_index{0};
    std::string parent_host;  ///< empty selects the real host name
    WorkerId worker;          ///< empty is adopted by the scheduler's local boot
    WorkerBootId worker_boot;
  };

  explicit CudaBackend(Options options = {});
  ~CudaBackend() override;
  CudaBackend(const CudaBackend&) = delete;
  CudaBackend& operator=(const CudaBackend&) = delete;

  [[nodiscard]] std::string_view name() const noexcept override;
  /// Provenance::kReal exactly when this instance really initialised a device.
  [[nodiscard]] Provenance provenance() const noexcept override;
  [[nodiscard]] BackendGeneration generation() const override;

  [[nodiscard]] std::vector<ExecutionDomainRecord> discover_domains() override;
  [[nodiscard]] Checked<CapabilityRecord> query_capability(ExecutionDomainId domain) override;
  [[nodiscard]] Checked<DomainLoadEvidence> query_load(ExecutionDomainId domain) override;
  [[nodiscard]] Status reserve(ExecutionDomainId domain, ReservationId id,
                               const ResourceAmounts& amounts) override;
  [[nodiscard]] Status release(ExecutionDomainId domain, ReservationId id) override;
  [[nodiscard]] ExecutionOutcome execute(const ExecutionInvocation& invocation) override;
  /// Destroys the process device context owned by this instance. Reports
  /// "cuda.leaked_device_memory" when allocations were still outstanding.
  [[nodiscard]] Status shutdown() override;

  /// Executions this instance completed through the device path.
  [[nodiscard]] std::uint64_t executed_count() const noexcept;
  /// Why this instance is not REAL, empty when it is.
  [[nodiscard]] std::string unavailable_reason() const;

  /// True when device 0 (or the requested index) can really be initialised on this
  /// host. Does not create the execution context and leaves no state behind.
  [[nodiscard]] static bool available(std::int32_t device_index = 0) noexcept;
  /// Real device name as reported by the CUDA runtime, empty when unavailable.
  [[nodiscard]] static std::string device_name(std::int32_t device_index = 0);
  /// major * 10 + minor of the device (sm_120 reports 120), 0 when unavailable.
  [[nodiscard]] static std::uint32_t device_compute_capability(std::int32_t device_index = 0);
  /// Total device global memory in bytes, 0 when unavailable.
  [[nodiscard]] static std::uint64_t device_memory_bytes(std::int32_t device_index = 0);
  /// Bytes currently held by the CUDA allocator for this backend: device memory plus
  /// pinned host staging. Zero after every call returns.
  [[nodiscard]] static std::uint64_t outstanding_device_bytes() noexcept;
  /// Kernels really launched by this process through the device path.
  [[nodiscard]] static std::uint64_t kernel_launches() noexcept;
  /// Bytes really copied between host and device by this process.
  [[nodiscard]] static std::uint64_t device_bytes_copied() noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tos

#endif  // TOS_BACKENDS_CUDA_BACKEND_HPP
