// Enumerations for the Transport Offload Scheduler decision model.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#ifndef TOS_CORE_ENUMS_HPP
#define TOS_CORE_ENUMS_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "tos/core/identities.hpp"

namespace tos {

/// Execution domain class. Vendor-specific behaviour is expressed through capability
/// records, never by hardcoding a vendor into the generic model.
enum class ExecutionDomainType : std::uint8_t {
  kCpu = 0,
  kAccelerator = 1,
  kNic = 2,
  kSmartNic = 3,
  kDpu = 4,
  kOtherRegisteredOffloadEngine = 5,
};
inline constexpr int kExecutionDomainTypeCount = 6;

/// Evidence provenance. A synthetic observation is never relabelled as hardware proof.
enum class Provenance : std::uint8_t {
  kReal = 0,        ///< observed/executed against the actual system boundary
  kSynthetic = 1,   ///< produced by the deterministic synthetic backend
  kUnsupported = 2, ///< claimed but no supporting hardware or evidence exists on this host
};

/// Memory domain in which a payload resides or is addressed.
enum class MemoryDomain : std::uint8_t {
  kHost = 0,
  kPinnedHost = 1,
  kDeviceLocal = 2,
  kPeerDevice = 3,
  kNicOnboard = 4,
  kSmartNicOnboard = 5,
  kDpuOnboard = 6,
  kUnknown = 7,
};
inline constexpr int kMemoryDomainCount = 8;

/// Isolation class that a domain can actually prove. UNKNOWN is a real answer.
enum class IsolationClass : std::uint8_t {
  kUnknown = 0,
  kSharedProcess = 1,
  kSeparateProcess = 2,
  kSeparateDeviceFunction = 3,
  kSeparateQueue = 4,
  kSeparateAddressSpace = 5,
  kSeparateExecutionContext = 6,
  kTenantIsolated = 7,
  kDedicatedDeviceService = 8,
};

/// Retry-safety classification of an operation. Replay is never assumed safe.
enum class SideEffectClass : std::uint8_t {
  kPure = 0,               ///< no state change, replay always safe
  kIdempotent = 1,         ///< replay converges to the same state
  kAtMostOnceRequired = 2, ///< replay may duplicate an observable effect
  kNonRepeatable = 3,      ///< replay is forbidden
  kUnknown = 4,            ///< unclassified, treated conservatively
};

/// Transport class the payload belongs to, used for compatibility and capability matching.
enum class TransportClass : std::uint8_t {
  kNone = 0,
  kRawFrames = 1,
  kStreamBytes = 2,
  kDatagramBytes = 3,
  kRdmaMessage = 4,
  kControlPlane = 5,
};
inline constexpr int kTransportClassCount = 6;

/// Payload shape class.
enum class PayloadClass : std::uint8_t {
  kOpaqueBytes = 0,
  kStructuredHeader = 1,
  kScatterGatherList = 2,
  kSegmentedFrames = 3,
};

/// Structural classification of the selected execution domain.
enum class SelectionOutcome : std::uint8_t {
  kOffloadSelected = 0,
  kCpuSelected = 1,
  kAcceleratorSelected = 2,
  kNicSelected = 3,
  kSmartNicSelected = 4,
  kDpuSelected = 5,
  kFallbackSelected = 6,
  kDeferred = 7,
  kNoEligibleDomain = 8,
  kPolicyRejected = 9,
  kCapabilityUnsupported = 10,
  kStaleEvidence = 11,
  kRevalidationRequired = 12,
  kCapacityUnavailable = 13,
  kCompatibilityRejected = 14,
  kIsolationRejected = 15,
  // Scheduler-boundary outcomes that are not selection results.
  kDispatched = 16,
  kFailed = 17,
  kOutcomeUnknown = 18,
  kCancelled = 19,
};

/// How strongly the policy insists that work leaves the CPU.
enum class OffloadRequirement : std::uint8_t {
  kAny = 0,             ///< CPU and offload engines compete on equal terms
  kPreferOffload = 1,   ///< offload preferred when legal, CPU still legal
  kOffloadRequired = 2, ///< CPU is ineligible; rejection is the correct outcome
  kHostRequired = 3,    ///< only CPU (host) execution is legal
};

/// Lifecycle of an execution attempt.
enum class AttemptState : std::uint8_t {
  kPlanned = 0,
  kReserved = 1,
  kDispatching = 2,
  kDispatched = 3,
  kCompleted = 4,
  kFailed = 5,
  kOutcomeUnknown = 6,
  kCancelled = 7,
  kRejectedStale = 8,
  kFenced = 9,
};

/// Terminal or in-flight classification used by reconciliation and recovery.
enum class AttemptResolution : std::uint8_t {
  kNone = 0,
  kAuthoritativeSuccess = 1,
  kAuthoritativeFailure = 2,
  kAmbiguous = 3,
  kCancelledBeforeDispatch = 4,
  kCancelledAmbiguous = 5,
  kFencedNoEffect = 6,
  kFencedMayHaveEffect = 7,
};

/// Named cause for rejecting a candidate during hard eligibility.
enum class IneligibilityReason : std::uint8_t {
  kNone = 0,
  kUnknownOperationClass = 1,
  kOperationNotSupported = 2,
  kMemoryDomainUnsupported = 3,
  kAddressabilityUnsupported = 4,
  kPayloadTooLarge = 5,
  kPayloadTooSmall = 6,
  kAlignmentUnsatisfied = 7,
  kIsolationInsufficient = 8,
  kCompatibilityMismatch = 9,
  kBackendUnsupported = 10,
  kPolicyForbidden = 11,
  kDomainNotAllowed = 12,
  kHealthNotReady = 13,
  kStaleCapability = 14,
  kCapabilityUnproven = 15,
  kLocalityViolation = 16,
  kCapacityUnavailable = 18,
  kDomainFenced = 19,
  kWorkerBootStale = 20,
  kCoordinatorEpochStale = 21,
  kProvenanceDisallowed = 22,
  kGenerationRegressed = 23,
  kTransportClassUnsupported = 24,
  kPayloadClassUnsupported = 25,
  kDmaRequired = 26,
  kScatterGatherRequired = 27,
  kConcurrencyExhausted = 28,
  kReservationConflict = 29,
  kDomainNotRegistered = 30,
  kBackendGenerationStale = 31,
  kAuthorityInvalidated = 32,
};
inline constexpr int kIneligibilityReasonCount = 33;

/// Reason a completion was rejected by completion authority.
enum class CompletionRejection : std::uint8_t {
  kAccepted = 0,
  kDuplicateIdentical = 1,   ///< idempotent replay of the same authoritative completion
  kConflictingDuplicate = 2, ///< same attempt, different result: rejected
  kUnknownAttempt = 3,
  kStaleWorkerBoot = 4,
  kStaleCoordinatorEpoch = 5,
  kStaleDomainGeneration = 6,
  kAttemptGenerationMismatch = 7,
  kDispatchIdMismatch = 8,
  kOperationMismatch = 9,
  kNotDispatched = 10,
  kAlreadyTerminal = 11,
  kIntegrityMismatch = 12,
  kDomainFenced = 13,
  kResultTooLarge = 14,
  kProvenanceMismatch = 15,
  kStaleCapabilityGeneration = 16,
};

/// Reason a dispatch was refused at the dispatch boundary.
enum class DispatchRejection : std::uint8_t {
  kNone = 0,
  kPlanNotFound = 1,
  kPlanConsumed = 2,
  kAuthorityStale = 3,
  kReservationInvalid = 4,
  kShutdownInProgress = 5,
  kAttemptRegistrationFailed = 6,
  kTransportFailure = 7,
  kCancelled = 8,
  kCapacityExhausted = 9,
};

/// Reason a reservation operation failed.
enum class ReservationRejection : std::uint8_t {
  kNone = 0,
  kUnknownResource = 1,
  kInsufficientCapacity = 2,
  kStaleDomainGeneration = 3,
  kStaleCapabilityGeneration = 4,
  kStalePolicyGeneration = 5,
  kAlreadyCommitted = 6,
  kAlreadyReleased = 7,
  kNotFound = 8,
  kLimitExceeded = 9,
  kShutdownInProgress = 10,
  kConflict = 11,
};

/// Reason retry was refused.
enum class RetryRejection : std::uint8_t {
  kNone = 0,
  kAttemptLimitReached = 1,
  kNonRepeatableOperation = 2,
  kAmbiguousOutcome = 3,
  kFailureNotRetryable = 4,
  kPolicyDisallowsRetry = 5,
  kDomainUnavailable = 6,
  kOperationNotRetryable = 7,
  kUnknownSideEffectClass = 8,
};

/// Reason fallback was refused.
enum class FallbackRejection : std::uint8_t {
  kNone = 0,
  kNoFallbackRegistered = 1,
  kFallbackDepthExceeded = 2,
  kHostFallbackForbidden = 3,
  kFallbackTargetIneligible = 4,
  kFallbackCycleDetected = 5,
  kPolicyDisallowsFallback = 6,
  kOriginalAttemptStillAuthoritative = 7,
};

/// Reconciliation discrepancy classes.
enum class DiscrepancyKind : std::uint8_t {
  kDomainMissing = 0,
  kDomainGenerationChanged = 1,
  kWorkerReplaced = 2,
  kCapabilityChanged = 3,
  kOperationNoLongerSupported = 4,
  kQueueSupportChanged = 5,
  kBackendVersionChanged = 6,
  kFirmwareGenerationChanged = 7,
  kCompletedAttemptEngineGone = 8,
  kInFlightAttemptAmbiguous = 9,
  kUnregisteredLiveDomain = 10,
  kGenerationRegressed = 11,
};

/// Resource kinds that may be reserved on an execution domain.
enum class ResourceKind : std::uint8_t {
  kExecutionSlot = 0,
  kQueueDepth = 1,
  kDescriptorRing = 2,
  kProcessingContext = 3,
  kOffloadEngine = 4,
  kDeviceMemoryBytes = 5,
  kScratchMemoryBytes = 6,
  kDmaChannel = 7,
  kQueuePair = 8,
  kVendorExecutionContext = 9,
};
inline constexpr int kResourceKindCount = 10;

/// Bounded stringification helpers. Every enum used in a machine-readable result is named.
[[nodiscard]] std::string_view to_string(ExecutionDomainType value) noexcept;
[[nodiscard]] std::string_view to_string(Provenance value) noexcept;
[[nodiscard]] std::string_view to_string(MemoryDomain value) noexcept;
[[nodiscard]] std::string_view to_string(IsolationClass value) noexcept;
[[nodiscard]] std::string_view to_string(SideEffectClass value) noexcept;
[[nodiscard]] std::string_view to_string(TransportClass value) noexcept;
[[nodiscard]] std::string_view to_string(PayloadClass value) noexcept;
[[nodiscard]] std::string_view to_string(SelectionOutcome value) noexcept;
[[nodiscard]] std::string_view to_string(OffloadRequirement value) noexcept;
[[nodiscard]] std::string_view to_string(AttemptState value) noexcept;
[[nodiscard]] std::string_view to_string(AttemptResolution value) noexcept;
[[nodiscard]] std::string_view to_string(IneligibilityReason value) noexcept;
[[nodiscard]] std::string_view to_string(CompletionRejection value) noexcept;
[[nodiscard]] std::string_view to_string(DispatchRejection value) noexcept;
[[nodiscard]] std::string_view to_string(ReservationRejection value) noexcept;
[[nodiscard]] std::string_view to_string(RetryRejection value) noexcept;
[[nodiscard]] std::string_view to_string(FallbackRejection value) noexcept;
[[nodiscard]] std::string_view to_string(DiscrepancyKind value) noexcept;
[[nodiscard]] std::string_view to_string(ResourceKind value) noexcept;
[[nodiscard]] std::string_view to_string(GenerationRelation value) noexcept;

/// Parse an enum from its canonical lowercase token. Case-insensitive; the input
/// must match a known token exactly, otherwise parsing fails and the enum is untouched.
[[nodiscard]] bool parse_enum(std::string_view text, ExecutionDomainType& out) noexcept;
[[nodiscard]] bool parse_enum(std::string_view text, Provenance& out) noexcept;
[[nodiscard]] bool parse_enum(std::string_view text, MemoryDomain& out) noexcept;
[[nodiscard]] bool parse_enum(std::string_view text, IsolationClass& out) noexcept;
[[nodiscard]] bool parse_enum(std::string_view text, SideEffectClass& out) noexcept;
[[nodiscard]] bool parse_enum(std::string_view text, TransportClass& out) noexcept;
[[nodiscard]] bool parse_enum(std::string_view text, PayloadClass& out) noexcept;
[[nodiscard]] bool parse_enum(std::string_view text, SelectionOutcome& out) noexcept;
[[nodiscard]] bool parse_enum(std::string_view text, OffloadRequirement& out) noexcept;
[[nodiscard]] bool parse_enum(std::string_view text, AttemptState& out) noexcept;
[[nodiscard]] bool parse_enum(std::string_view text, AttemptResolution& out) noexcept;
[[nodiscard]] bool parse_enum(std::string_view text, IneligibilityReason& out) noexcept;
[[nodiscard]] bool parse_enum(std::string_view text, DiscrepancyKind& out) noexcept;
[[nodiscard]] bool parse_enum(std::string_view text, ResourceKind& out) noexcept;

}  // namespace tos

#endif  // TOS_CORE_ENUMS_HPP
