// Narrow execution-backend interface.
//
// A backend reports what it can do, executes when told, and answers questions
// about its own engine generation. It is never given a path to mutate scheduler
// state, and the public API exposes no backend-native mutation.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#ifndef TOS_BACKENDS_BACKEND_HPP
#define TOS_BACKENDS_BACKEND_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "tos/core/attempt.hpp"
#include "tos/core/domain.hpp"
#include "tos/core/identities.hpp"
#include "tos/core/reservation.hpp"
#include "tos/util/status.hpp"

namespace tos {

/// Bound on the payload bytes carried with a single dispatch.
inline constexpr std::size_t kMaxDispatchPayloadBytes = 512 * 1024;
/// Bound on the result bytes returned by a single execution.
inline constexpr std::size_t kMaxResultPayloadBytes = 512 * 1024;

/// One concrete execution request handed to a backend.
struct ExecutionInvocation {
  ExecutionAttemptId attempt;
  ExecutionAttemptGeneration attempt_generation;
  DispatchId dispatch;
  CoordinatorEpoch coordinator_epoch;
  WorkerId worker;
  WorkerBootId worker_boot;
  ExecutionDomainId domain;
  ExecutionDomainGeneration domain_generation;
  CapabilityGeneration capability_generation;
  OperationClassId operation_class;
  SideEffectClass side_effect{SideEffectClass::kUnknown};
  PayloadDescriptor payload;
  std::vector<std::uint8_t> input;
  std::uint32_t retry_index{0};
  bool fallback{false};
  Provenance provenance{Provenance::kUnsupported};
};

struct ExecutionOutcome {
  bool executed{false};   ///< the backend applied the operation
  bool success{false};    ///< the outcome is proven successful
  bool ambiguous{false};  ///< the effect may or may not have happened
  FailureKind failure{FailureKind::kNone};
  std::uint64_t result_digest{0};
  std::uint64_t bytes_processed{0};
  std::uint64_t duration_ns{0};
  std::vector<std::uint8_t> output;
  std::string detail;

  [[nodiscard]] static ExecutionOutcome ok(std::uint64_t digest, std::uint64_t bytes,
                                           std::uint64_t duration_ns) {
    ExecutionOutcome o;
    o.executed = true;
    o.success = true;
    o.result_digest = digest;
    o.bytes_processed = bytes;
    o.duration_ns = duration_ns;
    return o;
  }
  [[nodiscard]] static ExecutionOutcome failed(FailureKind kind, std::string detail_in) {
    ExecutionOutcome o;
    o.executed = false;
    o.success = false;
    o.failure = kind;
    o.detail = std::move(detail_in);
    return o;
  }
  [[nodiscard]] static ExecutionOutcome unknown(std::string detail_in) {
    ExecutionOutcome o;
    o.executed = true;
    o.success = false;
    o.ambiguous = true;
    o.failure = FailureKind::kAmbiguousOutcome;
    o.detail = std::move(detail_in);
    return o;
  }
};

class IExecutionBackend {
 public:
  virtual ~IExecutionBackend() = default;

  [[nodiscard]] virtual std::string_view name() const noexcept = 0;
  [[nodiscard]] virtual Provenance provenance() const noexcept = 0;
  [[nodiscard]] virtual BackendGeneration generation() const = 0;

  [[nodiscard]] virtual std::vector<ExecutionDomainRecord> discover_domains() = 0;
  [[nodiscard]] virtual Checked<CapabilityRecord> query_capability(ExecutionDomainId domain) = 0;
  [[nodiscard]] virtual Checked<DomainLoadEvidence> query_load(ExecutionDomainId domain) = 0;

  /// Optional capacity handshake with the device. The scheduler keeps its own
  /// ledger; this reports whether the device accepted the claim.
  [[nodiscard]] virtual Status reserve(ExecutionDomainId domain, ReservationId id,
                                       const ResourceAmounts& amounts) = 0;
  [[nodiscard]] virtual Status release(ExecutionDomainId domain, ReservationId id) = 0;

  /// Execute synchronously. Called from a bounded pool thread; implementations may
  /// block on device work, but must never call back into the scheduler.
  [[nodiscard]] virtual ExecutionOutcome execute(const ExecutionInvocation& invocation) = 0;

  [[nodiscard]] virtual bool supports_cancel() const noexcept { return false; }
  [[nodiscard]] virtual Status cancel(ExecutionAttemptId id);
  [[nodiscard]] virtual Status shutdown();
};

/// Execute a transport operation class on host memory. This is the REAL CPU path:
/// the same code runs for local dispatch and for a CPU worker process.
[[nodiscard]] ExecutionOutcome execute_operation_on_host(const ExecutionInvocation& invocation,
                                                         const OperationClassRegistry& registry);

/// Operation classes the built-in host executor implements. Exposed so that custom
/// backends and worker processes can publish an honest capability set instead of
/// claiming support they cannot prove.
[[nodiscard]] std::vector<OperationClassId> host_executor_operations();

/// Capability set for an engine that implements exactly the host executor operation
/// classes over the given memory domains. Capability flags that the host executor
/// does not implement are published as unproven, never as supported.
[[nodiscard]] CapabilitySet make_host_executor_capability(std::uint64_t max_payload_bytes,
                                                          std::uint32_t queue_capacity,
                                                          std::uint32_t max_concurrency,
                                                          std::string backend_family,
                                                          std::uint32_t memory_domain_mask,
                                                          std::uint32_t driver_backend_version);

/// Memory domains an engine of the given class can address on this runtime: the
/// engine's own memory plus host memory where the class supports staging.
[[nodiscard]] std::uint32_t memory_domain_mask_for(ExecutionDomainType type);

}  // namespace tos

#endif  // TOS_BACKENDS_BACKEND_HPP
