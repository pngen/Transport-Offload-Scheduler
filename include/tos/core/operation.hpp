// Transport operation model: what must be done, independent of where it executes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#ifndef TOS_CORE_OPERATION_HPP
#define TOS_CORE_OPERATION_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tos/core/enums.hpp"
#include "tos/core/identities.hpp"
#include "tos/util/status.hpp"

namespace tos {

/// Bound on the number of operation classes the runtime will model.
inline constexpr std::size_t kMaxOperationClasses = 128;
/// Bound on the number of distinct execution domains the runtime will model.
inline constexpr std::size_t kMaxExecutionDomains = 100000;
/// Bound on the number of concurrently outstanding reservations.
inline constexpr std::size_t kMaxOutstandingReservations = 200000;
/// Bound on the number of live attempts retained in the ledger.
inline constexpr std::size_t kMaxLiveAttempts = 200000;
/// Bound on retained historical attempt records used for stale-replay rejection.
inline constexpr std::size_t kMaxHistoricalAttempts = 200000;
/// Bound on candidates evaluated and reported in one decision.
inline constexpr std::size_t kMaxReportedCandidates = 4096;
/// Bound on a single payload descriptor, independent of device capability.
inline constexpr std::uint64_t kMaxPayloadBytes = 1ULL << 40;  // 1 TiB

/// Capability flags. A flag describes a proven property of an execution domain.
enum class CapabilityFlag : std::uint32_t {
  kNone = 0,
  kChecksum = 1U << 0,
  kCompression = 1U << 1,
  kDecompression = 1U << 2,
  kEncryption = 1U << 3,
  kDecryption = 1U << 4,
  kDma = 1U << 5,
  kScatterGather = 1U << 6,
  kRdma = 1U << 7,
  kGpudirectAddressable = 1U << 8,
  kPacketProcessing = 1U << 9,
  kHeaderProcessing = 1U << 10,
  kProtocolTransform = 1U << 11,
  kIntegrityVerify = 1U << 12,
  kSegmentation = 1U << 13,
  kReassembly = 1U << 14,
  kStagingCopy = 1U << 15,
  kSideEffectSink = 1U << 16,
};
inline constexpr int kCapabilityFlagCount = 17;

[[nodiscard]] constexpr std::uint32_t flag_bit(CapabilityFlag f) noexcept {
  return static_cast<std::uint32_t>(f);
}
[[nodiscard]] constexpr bool has_flag(std::uint32_t bits, CapabilityFlag f) noexcept {
  return (bits & flag_bit(f)) != 0U;
}
[[nodiscard]] std::string_view to_string(CapabilityFlag value) noexcept;
[[nodiscard]] bool parse_enum(std::string_view text, CapabilityFlag& out) noexcept;
[[nodiscard]] std::vector<CapabilityFlag> all_capability_flags();
[[nodiscard]] std::vector<std::string_view> describe_flags(std::uint32_t bits);

/// Descriptor of an operation class. Classes are registered, bounded and named.
struct OperationClassDescriptor {
  OperationClassId id;
  std::string name;  ///< canonical upper-case token, e.g. "CHECKSUM_CRC32C"
  std::string_view category;
  SideEffectClass side_effect_class{SideEffectClass::kPure};
  TransportClass transport_class{TransportClass::kRawFrames};
  PayloadClass payload_class{PayloadClass::kOpaqueBytes};
  CapabilityFlag required_flag{CapabilityFlag::kNone};
};

/// Registry of operation classes. Built-in classes are always present; additional
/// classes may be registered by the embedding application under a hard bound.
class OperationClassRegistry {
 public:
  OperationClassRegistry();

  [[nodiscard]] static const OperationClassRegistry& builtins();

  [[nodiscard]] std::size_t size() const noexcept { return classes_.size(); }
  [[nodiscard]] const std::vector<OperationClassDescriptor>& classes() const noexcept { return classes_; }

  [[nodiscard]] const OperationClassDescriptor* find(OperationClassId id) const noexcept;
  [[nodiscard]] const OperationClassDescriptor* find(std::string_view name) const noexcept;

  /// Register a new class. Rejects duplicates, malformed names and overflow of the bound.
  [[nodiscard]] Checked<OperationClassId> register_class(OperationClassDescriptor descriptor);

 private:
  std::vector<OperationClassDescriptor> classes_;
};

/// Built-in operation class identities. Stable across releases.
namespace opclass {
[[nodiscard]] OperationClassId checksum_crc32c() noexcept;
[[nodiscard]] OperationClassId compress_rle() noexcept;
[[nodiscard]] OperationClassId decompress_rle() noexcept;
[[nodiscard]] OperationClassId copy_stage() noexcept;
[[nodiscard]] OperationClassId segment_split() noexcept;
[[nodiscard]] OperationClassId reassemble_join() noexcept;
[[nodiscard]] OperationClassId validate_integrity() noexcept;
[[nodiscard]] OperationClassId side_effect_emit() noexcept;
[[nodiscard]] OperationClassId noop_probe() noexcept;
}  // namespace opclass

/// Payload description. Addresses are opaque handles; the scheduler never dereferences them.
struct PayloadDescriptor {
  std::uint64_t size_bytes{0};
  MemoryDomain source_memory{MemoryDomain::kHost};
  MemoryDomain destination_memory{MemoryDomain::kHost};
  std::uint64_t source_handle{0};       ///< opaque addressable handle in the source domain
  std::uint64_t destination_handle{0};  ///< opaque addressable handle in the destination domain
  std::uint32_t alignment_bytes{1};     ///< required alignment of the addressable handle
  std::uint32_t segment_count{1};       ///< number of segments for scatter/gather payloads
  PayloadClass payload_class{PayloadClass::kOpaqueBytes};
  TransportClass transport_class{TransportClass::kRawFrames};
  bool scatter_gather{false};
};

/// Hard locality requirement expressed as a minimum locality class.
enum class LocalityClass : std::uint8_t {
  kUnknown = 0,
  kRemoteHost = 1,
  kSameHost = 2,
  kSameNic = 3,
  kAcceleratorLocalNic = 4,
  kSamePcieSwitch = 5,
  kSameRootComplex = 6,
  kSameNumaNode = 7,
  kDomainLocal = 8,  ///< payload already resides in the executing domain's memory
};

[[nodiscard]] std::string_view to_string(LocalityClass value) noexcept;
[[nodiscard]] bool parse_enum(std::string_view text, LocalityClass& out) noexcept;

/// Constraints the operation places on where it may execute.
struct LocalityConstraints {
  LocalityClass minimum_class{LocalityClass::kUnknown};
  std::uint32_t required_numa_node{0};   ///< 0 = unconstrained
  bool require_same_host{false};         ///< payload and engine on one host
  bool require_local_nic{false};         ///< engine must have a NIC on the same root complex
};

/// Compatibility requirements supplied by the caller or by policy.
struct CompatibilityRequirements {
  std::uint32_t minimum_driver_backend_version{0};
  std::uint32_t minimum_firmware_generation{0};
  std::uint32_t required_protocol_version{0};
  std::uint32_t required_accelerator_arch{0};  ///< 0 = architecture agnostic
  std::string required_backend_family;         ///< empty = any
};

/// Execution economics supplied by policy. Cost is expressed in whole cost units so
/// that ranking stays integer-exact and reproducible.
struct ExecutionEconomics {
  std::uint64_t setup_cost_units{0};
  std::uint64_t per_byte_cost_micro_units{0};
  std::uint64_t maximum_total_cost_units{0};  ///< 0 = no ceiling
  std::uint64_t estimated_transfer_bytes{0};
};

/// Retry permission attached to one operation request.
struct RetryPermission {
  bool allowed{false};
  std::uint32_t max_attempts{1};  ///< total attempts including the first
};

/// Immutable description of one transport-path operation that must be performed now.
struct OperationRequest {
  OperationClassId operation_class;
  PayloadDescriptor payload;
  std::vector<ExecutionDomainType> allowed_domains;    ///< empty = unrestricted
  std::vector<ExecutionDomainType> forbidden_domains;  ///< takes precedence over allowed
  std::vector<ExecutionDomainId> allowed_domain_ids;   ///< empty = unrestricted
  std::uint32_t required_capability_flags{0};          ///< CapabilityFlag bits
  IsolationClass required_isolation{IsolationClass::kUnknown};
  bool require_dma{false};
  bool require_scatter_gather{false};
  LocalityConstraints locality;
  CompatibilityRequirements compatibility;
  std::optional<ExecutionEconomics> economics;
  std::optional<std::uint64_t> latency_slo_ns;
  RetryPermission retry;
  bool fallback_allowed{true};
  /// Side-effect class. Callers may only make the classification more conservative
  /// than the registered class, never less.
  SideEffectClass side_effect_override{SideEffectClass::kUnknown};
  bool completion_may_produce_side_effects{false};
  TransportOperationId operation_id;  ///< assigned by the scheduler when unset
  std::string correlation_label;      ///< bounded diagnostic label
  Provenance provenance_hint{Provenance::kReal};
};

/// Effective side-effect class: the more conservative of registered and override.
[[nodiscard]] SideEffectClass effective_side_effect(SideEffectClass registered,
                                                    SideEffectClass override_value) noexcept;

/// True when the class permits automatic replay at the scheduler boundary.
[[nodiscard]] bool is_replay_safe(SideEffectClass value) noexcept;

/// Validate a request against structural bounds. Returns a machine-readable failure code.
[[nodiscard]] Status validate_request(const OperationRequest& request,
                                      const OperationClassRegistry& registry);

}  // namespace tos

#endif  // TOS_CORE_OPERATION_HPP
