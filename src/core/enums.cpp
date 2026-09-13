// Enum naming and parsing. Every enum that crosses a machine boundary is named.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "tos/core/enums.hpp"

#include <array>
#include <utility>

#include "tos/core/dispatch.hpp"
#include "tos/core/hooks.hpp"
#include "tos/core/operation.hpp"
#include "tos/core/policy.hpp"
#include "tos/core/ranking.hpp"
#include "tos/core/reservation.hpp"
#include "tos/core/state.hpp"

namespace tos {
namespace {

template <class Enum, std::size_t N>
bool parse_from(std::string_view text, const std::pair<std::string_view, Enum> (&table)[N],
                Enum& out) noexcept {
  for (const auto& entry : table) {
    if (entry.first.size() != text.size()) continue;
    bool equal = true;
    for (std::size_t i = 0; i < text.size(); ++i) {
      char a = text[i];
      char b = entry.first[i];
      if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
      if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
      if (a != b) {
        equal = false;
        break;
      }
    }
    if (equal) {
      out = entry.second;
      return true;
    }
  }
  return false;
}

}  // namespace

std::string_view to_string(ExecutionDomainType value) noexcept {
  switch (value) {
    case ExecutionDomainType::kCpu: return "cpu";
    case ExecutionDomainType::kAccelerator: return "accelerator";
    case ExecutionDomainType::kNic: return "nic";
    case ExecutionDomainType::kSmartNic: return "smartnic";
    case ExecutionDomainType::kDpu: return "dpu";
    case ExecutionDomainType::kOtherRegisteredOffloadEngine: return "other_registered_offload_engine";
  }
  return "unknown";
}

bool parse_enum(std::string_view text, ExecutionDomainType& out) noexcept {
  static constexpr std::pair<std::string_view, ExecutionDomainType> kTable[] = {
      {"cpu", ExecutionDomainType::kCpu},
      {"accelerator", ExecutionDomainType::kAccelerator},
      {"gpu", ExecutionDomainType::kAccelerator},
      {"nic", ExecutionDomainType::kNic},
      {"smartnic", ExecutionDomainType::kSmartNic},
      {"dpu", ExecutionDomainType::kDpu},
      {"other", ExecutionDomainType::kOtherRegisteredOffloadEngine},
      {"other_registered_offload_engine", ExecutionDomainType::kOtherRegisteredOffloadEngine},
  };
  return parse_from(text, kTable, out);
}

std::string_view to_string(Provenance value) noexcept {
  switch (value) {
    case Provenance::kReal: return "REAL";
    case Provenance::kSynthetic: return "SYNTHETIC";
    case Provenance::kUnsupported: return "UNSUPPORTED";
  }
  return "UNKNOWN";
}

bool parse_enum(std::string_view text, Provenance& out) noexcept {
  static constexpr std::pair<std::string_view, Provenance> kTable[] = {
      {"real", Provenance::kReal},
      {"synthetic", Provenance::kSynthetic},
      {"unsupported", Provenance::kUnsupported},
  };
  return parse_from(text, kTable, out);
}

std::string_view to_string(MemoryDomain value) noexcept {
  switch (value) {
    case MemoryDomain::kHost: return "host";
    case MemoryDomain::kPinnedHost: return "pinned_host";
    case MemoryDomain::kDeviceLocal: return "device_local";
    case MemoryDomain::kPeerDevice: return "peer_device";
    case MemoryDomain::kNicOnboard: return "nic_onboard";
    case MemoryDomain::kSmartNicOnboard: return "smartnic_onboard";
    case MemoryDomain::kDpuOnboard: return "dpu_onboard";
    case MemoryDomain::kUnknown: return "unknown";
  }
  return "unknown";
}

bool parse_enum(std::string_view text, MemoryDomain& out) noexcept {
  static constexpr std::pair<std::string_view, MemoryDomain> kTable[] = {
      {"host", MemoryDomain::kHost},
      {"pinned_host", MemoryDomain::kPinnedHost},
      {"device_local", MemoryDomain::kDeviceLocal},
      {"peer_device", MemoryDomain::kPeerDevice},
      {"nic_onboard", MemoryDomain::kNicOnboard},
      {"smartnic_onboard", MemoryDomain::kSmartNicOnboard},
      {"dpu_onboard", MemoryDomain::kDpuOnboard},
      {"unknown", MemoryDomain::kUnknown},
  };
  return parse_from(text, kTable, out);
}

std::string_view to_string(IsolationClass value) noexcept {
  switch (value) {
    case IsolationClass::kUnknown: return "unknown";
    case IsolationClass::kSharedProcess: return "shared_process";
    case IsolationClass::kSeparateProcess: return "separate_process";
    case IsolationClass::kSeparateDeviceFunction: return "separate_device_function";
    case IsolationClass::kSeparateQueue: return "separate_queue";
    case IsolationClass::kSeparateAddressSpace: return "separate_address_space";
    case IsolationClass::kSeparateExecutionContext: return "separate_execution_context";
    case IsolationClass::kTenantIsolated: return "tenant_isolated";
    case IsolationClass::kDedicatedDeviceService: return "dedicated_device_service";
  }
  return "unknown";
}

bool parse_enum(std::string_view text, IsolationClass& out) noexcept {
  static constexpr std::pair<std::string_view, IsolationClass> kTable[] = {
      {"unknown", IsolationClass::kUnknown},
      {"shared_process", IsolationClass::kSharedProcess},
      {"separate_process", IsolationClass::kSeparateProcess},
      {"separate_device_function", IsolationClass::kSeparateDeviceFunction},
      {"separate_queue", IsolationClass::kSeparateQueue},
      {"separate_address_space", IsolationClass::kSeparateAddressSpace},
      {"separate_execution_context", IsolationClass::kSeparateExecutionContext},
      {"tenant_isolated", IsolationClass::kTenantIsolated},
      {"dedicated_device_service", IsolationClass::kDedicatedDeviceService},
  };
  return parse_from(text, kTable, out);
}

std::string_view to_string(SideEffectClass value) noexcept {
  switch (value) {
    case SideEffectClass::kPure: return "PURE";
    case SideEffectClass::kIdempotent: return "IDEMPOTENT";
    case SideEffectClass::kAtMostOnceRequired: return "AT_MOST_ONCE_REQUIRED";
    case SideEffectClass::kNonRepeatable: return "NON_REPEATABLE";
    case SideEffectClass::kUnknown: return "UNKNOWN";
  }
  return "UNKNOWN";
}

bool parse_enum(std::string_view text, SideEffectClass& out) noexcept {
  static constexpr std::pair<std::string_view, SideEffectClass> kTable[] = {
      {"pure", SideEffectClass::kPure},
      {"idempotent", SideEffectClass::kIdempotent},
      {"at_most_once_required", SideEffectClass::kAtMostOnceRequired},
      {"non_repeatable", SideEffectClass::kNonRepeatable},
      {"unknown", SideEffectClass::kUnknown},
  };
  return parse_from(text, kTable, out);
}

std::string_view to_string(TransportClass value) noexcept {
  switch (value) {
    case TransportClass::kNone: return "none";
    case TransportClass::kRawFrames: return "raw_frames";
    case TransportClass::kStreamBytes: return "stream_bytes";
    case TransportClass::kDatagramBytes: return "datagram_bytes";
    case TransportClass::kRdmaMessage: return "rdma_message";
    case TransportClass::kControlPlane: return "control_plane";
  }
  return "none";
}

bool parse_enum(std::string_view text, TransportClass& out) noexcept {
  static constexpr std::pair<std::string_view, TransportClass> kTable[] = {
      {"none", TransportClass::kNone},
      {"raw_frames", TransportClass::kRawFrames},
      {"stream_bytes", TransportClass::kStreamBytes},
      {"datagram_bytes", TransportClass::kDatagramBytes},
      {"rdma_message", TransportClass::kRdmaMessage},
      {"control_plane", TransportClass::kControlPlane},
  };
  return parse_from(text, kTable, out);
}

std::string_view to_string(PayloadClass value) noexcept {
  switch (value) {
    case PayloadClass::kOpaqueBytes: return "opaque_bytes";
    case PayloadClass::kStructuredHeader: return "structured_header";
    case PayloadClass::kScatterGatherList: return "scatter_gather_list";
    case PayloadClass::kSegmentedFrames: return "segmented_frames";
  }
  return "opaque_bytes";
}

bool parse_enum(std::string_view text, PayloadClass& out) noexcept {
  static constexpr std::pair<std::string_view, PayloadClass> kTable[] = {
      {"opaque_bytes", PayloadClass::kOpaqueBytes},
      {"structured_header", PayloadClass::kStructuredHeader},
      {"scatter_gather_list", PayloadClass::kScatterGatherList},
      {"segmented_frames", PayloadClass::kSegmentedFrames},
  };
  return parse_from(text, kTable, out);
}

std::string_view to_string(SelectionOutcome value) noexcept {
  switch (value) {
    case SelectionOutcome::kOffloadSelected: return "OFFLOAD_SELECTED";
    case SelectionOutcome::kCpuSelected: return "CPU_SELECTED";
    case SelectionOutcome::kAcceleratorSelected: return "ACCELERATOR_SELECTED";
    case SelectionOutcome::kNicSelected: return "NIC_SELECTED";
    case SelectionOutcome::kSmartNicSelected: return "SMARTNIC_SELECTED";
    case SelectionOutcome::kDpuSelected: return "DPU_SELECTED";
    case SelectionOutcome::kFallbackSelected: return "FALLBACK_SELECTED";
    case SelectionOutcome::kDeferred: return "DEFERRED";
    case SelectionOutcome::kNoEligibleDomain: return "NO_ELIGIBLE_DOMAIN";
    case SelectionOutcome::kPolicyRejected: return "POLICY_REJECTED";
    case SelectionOutcome::kCapabilityUnsupported: return "CAPABILITY_UNSUPPORTED";
    case SelectionOutcome::kStaleEvidence: return "STALE_EVIDENCE";
    case SelectionOutcome::kRevalidationRequired: return "REVALIDATION_REQUIRED";
    case SelectionOutcome::kCapacityUnavailable: return "CAPACITY_UNAVAILABLE";
    case SelectionOutcome::kCompatibilityRejected: return "COMPATIBILITY_REJECTED";
    case SelectionOutcome::kIsolationRejected: return "ISOLATION_REJECTED";
    case SelectionOutcome::kDispatched: return "DISPATCHED";
    case SelectionOutcome::kFailed: return "FAILED";
    case SelectionOutcome::kOutcomeUnknown: return "OUTCOME_UNKNOWN";
    case SelectionOutcome::kCancelled: return "CANCELLED";
  }
  return "NO_ELIGIBLE_DOMAIN";
}

bool parse_enum(std::string_view text, SelectionOutcome& out) noexcept {
  static constexpr std::pair<std::string_view, SelectionOutcome> kTable[] = {
      {"offload_selected", SelectionOutcome::kOffloadSelected},
      {"cpu_selected", SelectionOutcome::kCpuSelected},
      {"accelerator_selected", SelectionOutcome::kAcceleratorSelected},
      {"nic_selected", SelectionOutcome::kNicSelected},
      {"smartnic_selected", SelectionOutcome::kSmartNicSelected},
      {"dpu_selected", SelectionOutcome::kDpuSelected},
      {"fallback_selected", SelectionOutcome::kFallbackSelected},
      {"deferred", SelectionOutcome::kDeferred},
      {"no_eligible_domain", SelectionOutcome::kNoEligibleDomain},
      {"policy_rejected", SelectionOutcome::kPolicyRejected},
      {"capability_unsupported", SelectionOutcome::kCapabilityUnsupported},
      {"stale_evidence", SelectionOutcome::kStaleEvidence},
      {"revalidation_required", SelectionOutcome::kRevalidationRequired},
      {"capacity_unavailable", SelectionOutcome::kCapacityUnavailable},
      {"compatibility_rejected", SelectionOutcome::kCompatibilityRejected},
      {"isolation_rejected", SelectionOutcome::kIsolationRejected},
      {"dispatched", SelectionOutcome::kDispatched},
      {"failed", SelectionOutcome::kFailed},
      {"outcome_unknown", SelectionOutcome::kOutcomeUnknown},
      {"cancelled", SelectionOutcome::kCancelled},
  };
  return parse_from(text, kTable, out);
}

std::string_view to_string(OffloadRequirement value) noexcept {
  switch (value) {
    case OffloadRequirement::kAny: return "any";
    case OffloadRequirement::kPreferOffload: return "prefer_offload";
    case OffloadRequirement::kOffloadRequired: return "offload_required";
    case OffloadRequirement::kHostRequired: return "host_required";
  }
  return "any";
}

bool parse_enum(std::string_view text, OffloadRequirement& out) noexcept {
  static constexpr std::pair<std::string_view, OffloadRequirement> kTable[] = {
      {"any", OffloadRequirement::kAny},
      {"prefer_offload", OffloadRequirement::kPreferOffload},
      {"offload_required", OffloadRequirement::kOffloadRequired},
      {"host_required", OffloadRequirement::kHostRequired},
  };
  return parse_from(text, kTable, out);
}

std::string_view to_string(AttemptState value) noexcept {
  switch (value) {
    case AttemptState::kPlanned: return "PLANNED";
    case AttemptState::kReserved: return "RESERVED";
    case AttemptState::kDispatching: return "DISPATCHING";
    case AttemptState::kDispatched: return "DISPATCHED";
    case AttemptState::kCompleted: return "COMPLETED";
    case AttemptState::kFailed: return "FAILED";
    case AttemptState::kOutcomeUnknown: return "OUTCOME_UNKNOWN";
    case AttemptState::kCancelled: return "CANCELLED";
    case AttemptState::kRejectedStale: return "REJECTED_STALE";
    case AttemptState::kFenced: return "FENCED";
  }
  return "PLANNED";
}

bool parse_enum(std::string_view text, AttemptState& out) noexcept {
  static constexpr std::pair<std::string_view, AttemptState> kTable[] = {
      {"planned", AttemptState::kPlanned},
      {"reserved", AttemptState::kReserved},
      {"dispatching", AttemptState::kDispatching},
      {"dispatched", AttemptState::kDispatched},
      {"completed", AttemptState::kCompleted},
      {"failed", AttemptState::kFailed},
      {"outcome_unknown", AttemptState::kOutcomeUnknown},
      {"cancelled", AttemptState::kCancelled},
      {"rejected_stale", AttemptState::kRejectedStale},
      {"fenced", AttemptState::kFenced},
  };
  return parse_from(text, kTable, out);
}

std::string_view to_string(AttemptResolution value) noexcept {
  switch (value) {
    case AttemptResolution::kNone: return "NONE";
    case AttemptResolution::kAuthoritativeSuccess: return "AUTHORITATIVE_SUCCESS";
    case AttemptResolution::kAuthoritativeFailure: return "AUTHORITATIVE_FAILURE";
    case AttemptResolution::kAmbiguous: return "AMBIGUOUS";
    case AttemptResolution::kCancelledBeforeDispatch: return "CANCELLED_BEFORE_DISPATCH";
    case AttemptResolution::kCancelledAmbiguous: return "CANCELLED_AMBIGUOUS";
    case AttemptResolution::kFencedNoEffect: return "FENCED_NO_EFFECT";
    case AttemptResolution::kFencedMayHaveEffect: return "FENCED_MAY_HAVE_EFFECT";
  }
  return "NONE";
}

bool parse_enum(std::string_view text, AttemptResolution& out) noexcept {
  static constexpr std::pair<std::string_view, AttemptResolution> kTable[] = {
      {"none", AttemptResolution::kNone},
      {"authoritative_success", AttemptResolution::kAuthoritativeSuccess},
      {"authoritative_failure", AttemptResolution::kAuthoritativeFailure},
      {"ambiguous", AttemptResolution::kAmbiguous},
      {"cancelled_before_dispatch", AttemptResolution::kCancelledBeforeDispatch},
      {"cancelled_ambiguous", AttemptResolution::kCancelledAmbiguous},
      {"fenced_no_effect", AttemptResolution::kFencedNoEffect},
      {"fenced_may_have_effect", AttemptResolution::kFencedMayHaveEffect},
  };
  return parse_from(text, kTable, out);
}

std::string_view to_string(IneligibilityReason value) noexcept {
  switch (value) {
    case IneligibilityReason::kNone: return "none";
    case IneligibilityReason::kUnknownOperationClass: return "unknown_operation_class";
    case IneligibilityReason::kOperationNotSupported: return "operation_not_supported";
    case IneligibilityReason::kMemoryDomainUnsupported: return "memory_domain_unsupported";
    case IneligibilityReason::kAddressabilityUnsupported: return "addressability_unsupported";
    case IneligibilityReason::kPayloadTooLarge: return "payload_too_large";
    case IneligibilityReason::kPayloadTooSmall: return "payload_too_small";
    case IneligibilityReason::kAlignmentUnsatisfied: return "alignment_unsatisfied";
    case IneligibilityReason::kIsolationInsufficient: return "isolation_insufficient";
    case IneligibilityReason::kCompatibilityMismatch: return "compatibility_mismatch";
    case IneligibilityReason::kBackendUnsupported: return "backend_unsupported";
    case IneligibilityReason::kPolicyForbidden: return "policy_forbidden";
    case IneligibilityReason::kDomainNotAllowed: return "domain_not_allowed";
    case IneligibilityReason::kHealthNotReady: return "health_not_ready";
    case IneligibilityReason::kStaleCapability: return "stale_capability";
    case IneligibilityReason::kCapabilityUnproven: return "capability_unproven";
    case IneligibilityReason::kLocalityViolation: return "locality_violation";
    case IneligibilityReason::kCapacityUnavailable: return "capacity_unavailable";
    case IneligibilityReason::kDomainFenced: return "domain_fenced";
    case IneligibilityReason::kWorkerBootStale: return "worker_boot_stale";
    case IneligibilityReason::kCoordinatorEpochStale: return "coordinator_epoch_stale";
    case IneligibilityReason::kProvenanceDisallowed: return "provenance_disallowed";
    case IneligibilityReason::kGenerationRegressed: return "generation_regressed";
    case IneligibilityReason::kTransportClassUnsupported: return "transport_class_unsupported";
    case IneligibilityReason::kPayloadClassUnsupported: return "payload_class_unsupported";
    case IneligibilityReason::kDmaRequired: return "dma_required";
    case IneligibilityReason::kScatterGatherRequired: return "scatter_gather_required";
    case IneligibilityReason::kConcurrencyExhausted: return "concurrency_exhausted";
    case IneligibilityReason::kReservationConflict: return "reservation_conflict";
    case IneligibilityReason::kDomainNotRegistered: return "domain_not_registered";
    case IneligibilityReason::kBackendGenerationStale: return "backend_generation_stale";
    case IneligibilityReason::kAuthorityInvalidated: return "authority_invalidated";
  }
  return "none";
}

bool parse_enum(std::string_view text, IneligibilityReason& out) noexcept {
  static constexpr std::pair<std::string_view, IneligibilityReason> kTable[] = {
      {"none", IneligibilityReason::kNone},
      {"unknown_operation_class", IneligibilityReason::kUnknownOperationClass},
      {"operation_not_supported", IneligibilityReason::kOperationNotSupported},
      {"memory_domain_unsupported", IneligibilityReason::kMemoryDomainUnsupported},
      {"addressability_unsupported", IneligibilityReason::kAddressabilityUnsupported},
      {"payload_too_large", IneligibilityReason::kPayloadTooLarge},
      {"payload_too_small", IneligibilityReason::kPayloadTooSmall},
      {"alignment_unsatisfied", IneligibilityReason::kAlignmentUnsatisfied},
      {"isolation_insufficient", IneligibilityReason::kIsolationInsufficient},
      {"compatibility_mismatch", IneligibilityReason::kCompatibilityMismatch},
      {"backend_unsupported", IneligibilityReason::kBackendUnsupported},
      {"policy_forbidden", IneligibilityReason::kPolicyForbidden},
      {"domain_not_allowed", IneligibilityReason::kDomainNotAllowed},
      {"health_not_ready", IneligibilityReason::kHealthNotReady},
      {"stale_capability", IneligibilityReason::kStaleCapability},
      {"capability_unproven", IneligibilityReason::kCapabilityUnproven},
      {"locality_violation", IneligibilityReason::kLocalityViolation},
      {"capacity_unavailable", IneligibilityReason::kCapacityUnavailable},
      {"domain_fenced", IneligibilityReason::kDomainFenced},
      {"worker_boot_stale", IneligibilityReason::kWorkerBootStale},
      {"coordinator_epoch_stale", IneligibilityReason::kCoordinatorEpochStale},
      {"provenance_disallowed", IneligibilityReason::kProvenanceDisallowed},
      {"generation_regressed", IneligibilityReason::kGenerationRegressed},
      {"transport_class_unsupported", IneligibilityReason::kTransportClassUnsupported},
      {"payload_class_unsupported", IneligibilityReason::kPayloadClassUnsupported},
      {"dma_required", IneligibilityReason::kDmaRequired},
      {"scatter_gather_required", IneligibilityReason::kScatterGatherRequired},
      {"concurrency_exhausted", IneligibilityReason::kConcurrencyExhausted},
      {"reservation_conflict", IneligibilityReason::kReservationConflict},
      {"domain_not_registered", IneligibilityReason::kDomainNotRegistered},
      {"backend_generation_stale", IneligibilityReason::kBackendGenerationStale},
      {"authority_invalidated", IneligibilityReason::kAuthorityInvalidated},
  };
  return parse_from(text, kTable, out);
}

std::string_view to_string(CompletionRejection value) noexcept {
  switch (value) {
    case CompletionRejection::kAccepted: return "accepted";
    case CompletionRejection::kDuplicateIdentical: return "duplicate_identical";
    case CompletionRejection::kConflictingDuplicate: return "conflicting_duplicate";
    case CompletionRejection::kUnknownAttempt: return "unknown_attempt";
    case CompletionRejection::kStaleWorkerBoot: return "stale_worker_boot";
    case CompletionRejection::kStaleCoordinatorEpoch: return "stale_coordinator_epoch";
    case CompletionRejection::kStaleDomainGeneration: return "stale_domain_generation";
    case CompletionRejection::kAttemptGenerationMismatch: return "attempt_generation_mismatch";
    case CompletionRejection::kDispatchIdMismatch: return "dispatch_id_mismatch";
    case CompletionRejection::kOperationMismatch: return "operation_mismatch";
    case CompletionRejection::kNotDispatched: return "not_dispatched";
    case CompletionRejection::kAlreadyTerminal: return "already_terminal";
    case CompletionRejection::kIntegrityMismatch: return "integrity_mismatch";
    case CompletionRejection::kDomainFenced: return "domain_fenced";
    case CompletionRejection::kResultTooLarge: return "result_too_large";
    case CompletionRejection::kProvenanceMismatch: return "provenance_mismatch";
    case CompletionRejection::kStaleCapabilityGeneration: return "stale_capability_generation";
  }
  return "unknown";
}

std::string_view to_string(DispatchRejection value) noexcept {
  switch (value) {
    case DispatchRejection::kNone: return "none";
    case DispatchRejection::kPlanNotFound: return "plan_not_found";
    case DispatchRejection::kPlanConsumed: return "plan_consumed";
    case DispatchRejection::kAuthorityStale: return "authority_stale";
    case DispatchRejection::kReservationInvalid: return "reservation_invalid";
    case DispatchRejection::kShutdownInProgress: return "shutdown_in_progress";
    case DispatchRejection::kAttemptRegistrationFailed: return "attempt_registration_failed";
    case DispatchRejection::kTransportFailure: return "transport_failure";
    case DispatchRejection::kCancelled: return "cancelled";
    case DispatchRejection::kCapacityExhausted: return "capacity_exhausted";
  }
  return "none";
}

std::string_view to_string(ReservationRejection value) noexcept {
  switch (value) {
    case ReservationRejection::kNone: return "none";
    case ReservationRejection::kUnknownResource: return "unknown_resource";
    case ReservationRejection::kInsufficientCapacity: return "insufficient_capacity";
    case ReservationRejection::kStaleDomainGeneration: return "stale_domain_generation";
    case ReservationRejection::kStaleCapabilityGeneration: return "stale_capability_generation";
    case ReservationRejection::kStalePolicyGeneration: return "stale_policy_generation";
    case ReservationRejection::kAlreadyCommitted: return "already_committed";
    case ReservationRejection::kAlreadyReleased: return "already_released";
    case ReservationRejection::kNotFound: return "not_found";
    case ReservationRejection::kLimitExceeded: return "limit_exceeded";
    case ReservationRejection::kShutdownInProgress: return "shutdown_in_progress";
    case ReservationRejection::kConflict: return "conflict";
  }
  return "none";
}

std::string_view to_string(RetryRejection value) noexcept {
  switch (value) {
    case RetryRejection::kNone: return "none";
    case RetryRejection::kAttemptLimitReached: return "attempt_limit_reached";
    case RetryRejection::kNonRepeatableOperation: return "non_repeatable_operation";
    case RetryRejection::kAmbiguousOutcome: return "ambiguous_outcome";
    case RetryRejection::kFailureNotRetryable: return "failure_not_retryable";
    case RetryRejection::kPolicyDisallowsRetry: return "policy_disallows_retry";
    case RetryRejection::kDomainUnavailable: return "domain_unavailable";
    case RetryRejection::kOperationNotRetryable: return "operation_not_retryable";
    case RetryRejection::kUnknownSideEffectClass: return "unknown_side_effect_class";
  }
  return "none";
}

std::string_view to_string(FallbackRejection value) noexcept {
  switch (value) {
    case FallbackRejection::kNone: return "none";
    case FallbackRejection::kNoFallbackRegistered: return "no_fallback_registered";
    case FallbackRejection::kFallbackDepthExceeded: return "fallback_depth_exceeded";
    case FallbackRejection::kHostFallbackForbidden: return "host_fallback_forbidden";
    case FallbackRejection::kFallbackTargetIneligible: return "fallback_target_ineligible";
    case FallbackRejection::kFallbackCycleDetected: return "fallback_cycle_detected";
    case FallbackRejection::kPolicyDisallowsFallback: return "policy_disallows_fallback";
    case FallbackRejection::kOriginalAttemptStillAuthoritative: return "original_attempt_still_authoritative";
  }
  return "none";
}

std::string_view to_string(DiscrepancyKind value) noexcept {
  switch (value) {
    case DiscrepancyKind::kDomainMissing: return "domain_missing";
    case DiscrepancyKind::kDomainGenerationChanged: return "domain_generation_changed";
    case DiscrepancyKind::kWorkerReplaced: return "worker_replaced";
    case DiscrepancyKind::kCapabilityChanged: return "capability_changed";
    case DiscrepancyKind::kOperationNoLongerSupported: return "operation_no_longer_supported";
    case DiscrepancyKind::kQueueSupportChanged: return "queue_support_changed";
    case DiscrepancyKind::kBackendVersionChanged: return "backend_version_changed";
    case DiscrepancyKind::kFirmwareGenerationChanged: return "firmware_generation_changed";
    case DiscrepancyKind::kCompletedAttemptEngineGone: return "completed_attempt_engine_gone";
    case DiscrepancyKind::kInFlightAttemptAmbiguous: return "in_flight_attempt_ambiguous";
    case DiscrepancyKind::kUnregisteredLiveDomain: return "unregistered_live_domain";
    case DiscrepancyKind::kGenerationRegressed: return "generation_regressed";
  }
  return "domain_missing";
}

bool parse_enum(std::string_view text, DiscrepancyKind& out) noexcept {
  static constexpr std::pair<std::string_view, DiscrepancyKind> kTable[] = {
      {"domain_missing", DiscrepancyKind::kDomainMissing},
      {"domain_generation_changed", DiscrepancyKind::kDomainGenerationChanged},
      {"worker_replaced", DiscrepancyKind::kWorkerReplaced},
      {"capability_changed", DiscrepancyKind::kCapabilityChanged},
      {"operation_no_longer_supported", DiscrepancyKind::kOperationNoLongerSupported},
      {"queue_support_changed", DiscrepancyKind::kQueueSupportChanged},
      {"backend_version_changed", DiscrepancyKind::kBackendVersionChanged},
      {"firmware_generation_changed", DiscrepancyKind::kFirmwareGenerationChanged},
      {"completed_attempt_engine_gone", DiscrepancyKind::kCompletedAttemptEngineGone},
      {"in_flight_attempt_ambiguous", DiscrepancyKind::kInFlightAttemptAmbiguous},
      {"unregistered_live_domain", DiscrepancyKind::kUnregisteredLiveDomain},
      {"generation_regressed", DiscrepancyKind::kGenerationRegressed},
  };
  return parse_from(text, kTable, out);
}

std::string_view to_string(ResourceKind value) noexcept {
  switch (value) {
    case ResourceKind::kExecutionSlot: return "execution_slot";
    case ResourceKind::kQueueDepth: return "queue_depth";
    case ResourceKind::kDescriptorRing: return "descriptor_ring";
    case ResourceKind::kProcessingContext: return "processing_context";
    case ResourceKind::kOffloadEngine: return "offload_engine";
    case ResourceKind::kDeviceMemoryBytes: return "device_memory_bytes";
    case ResourceKind::kScratchMemoryBytes: return "scratch_memory_bytes";
    case ResourceKind::kDmaChannel: return "dma_channel";
    case ResourceKind::kQueuePair: return "queue_pair";
    case ResourceKind::kVendorExecutionContext: return "vendor_execution_context";
  }
  return "execution_slot";
}

bool parse_enum(std::string_view text, ResourceKind& out) noexcept {
  static constexpr std::pair<std::string_view, ResourceKind> kTable[] = {
      {"execution_slot", ResourceKind::kExecutionSlot},
      {"queue_depth", ResourceKind::kQueueDepth},
      {"descriptor_ring", ResourceKind::kDescriptorRing},
      {"processing_context", ResourceKind::kProcessingContext},
      {"offload_engine", ResourceKind::kOffloadEngine},
      {"device_memory_bytes", ResourceKind::kDeviceMemoryBytes},
      {"scratch_memory_bytes", ResourceKind::kScratchMemoryBytes},
      {"dma_channel", ResourceKind::kDmaChannel},
      {"queue_pair", ResourceKind::kQueuePair},
      {"vendor_execution_context", ResourceKind::kVendorExecutionContext},
  };
  return parse_from(text, kTable, out);
}

std::string_view to_string(GenerationRelation value) noexcept {
  switch (value) {
    case GenerationRelation::kEqual: return "equal";
    case GenerationRelation::kAdvanced: return "advanced";
    case GenerationRelation::kRegressed: return "regressed";
    case GenerationRelation::kUnset: return "unset";
  }
  return "unset";
}

std::string_view to_string(CapabilityFlag value) noexcept {
  switch (value) {
    case CapabilityFlag::kNone: return "none";
    case CapabilityFlag::kChecksum: return "checksum";
    case CapabilityFlag::kCompression: return "compression";
    case CapabilityFlag::kDecompression: return "decompression";
    case CapabilityFlag::kEncryption: return "encryption";
    case CapabilityFlag::kDecryption: return "decryption";
    case CapabilityFlag::kDma: return "dma";
    case CapabilityFlag::kScatterGather: return "scatter_gather";
    case CapabilityFlag::kRdma: return "rdma";
    case CapabilityFlag::kGpudirectAddressable: return "gpudirect_addressable";
    case CapabilityFlag::kPacketProcessing: return "packet_processing";
    case CapabilityFlag::kHeaderProcessing: return "header_processing";
    case CapabilityFlag::kProtocolTransform: return "protocol_transform";
    case CapabilityFlag::kIntegrityVerify: return "integrity_verify";
    case CapabilityFlag::kSegmentation: return "segmentation";
    case CapabilityFlag::kReassembly: return "reassembly";
    case CapabilityFlag::kStagingCopy: return "staging_copy";
    case CapabilityFlag::kSideEffectSink: return "side_effect_sink";
  }
  return "none";
}

bool parse_enum(std::string_view text, CapabilityFlag& out) noexcept {
  static constexpr std::pair<std::string_view, CapabilityFlag> kTable[] = {
      {"checksum", CapabilityFlag::kChecksum},
      {"compression", CapabilityFlag::kCompression},
      {"decompression", CapabilityFlag::kDecompression},
      {"encryption", CapabilityFlag::kEncryption},
      {"decryption", CapabilityFlag::kDecryption},
      {"dma", CapabilityFlag::kDma},
      {"scatter_gather", CapabilityFlag::kScatterGather},
      {"rdma", CapabilityFlag::kRdma},
      {"gpudirect_addressable", CapabilityFlag::kGpudirectAddressable},
      {"packet_processing", CapabilityFlag::kPacketProcessing},
      {"header_processing", CapabilityFlag::kHeaderProcessing},
      {"protocol_transform", CapabilityFlag::kProtocolTransform},
      {"integrity_verify", CapabilityFlag::kIntegrityVerify},
      {"segmentation", CapabilityFlag::kSegmentation},
      {"reassembly", CapabilityFlag::kReassembly},
      {"staging_copy", CapabilityFlag::kStagingCopy},
      {"side_effect_sink", CapabilityFlag::kSideEffectSink},
  };
  return parse_from(text, kTable, out);
}

std::string_view to_string(LocalityClass value) noexcept {
  switch (value) {
    case LocalityClass::kUnknown: return "unknown";
    case LocalityClass::kRemoteHost: return "remote_host";
    case LocalityClass::kSameHost: return "same_host";
    case LocalityClass::kSameNic: return "same_nic";
    case LocalityClass::kAcceleratorLocalNic: return "accelerator_local_nic";
    case LocalityClass::kSamePcieSwitch: return "same_pcie_switch";
    case LocalityClass::kSameRootComplex: return "same_root_complex";
    case LocalityClass::kSameNumaNode: return "same_numa_node";
    case LocalityClass::kDomainLocal: return "domain_local";
  }
  return "unknown";
}

bool parse_enum(std::string_view text, LocalityClass& out) noexcept {
  static constexpr std::pair<std::string_view, LocalityClass> kTable[] = {
      {"unknown", LocalityClass::kUnknown},
      {"remote_host", LocalityClass::kRemoteHost},
      {"same_host", LocalityClass::kSameHost},
      {"same_nic", LocalityClass::kSameNic},
      {"accelerator_local_nic", LocalityClass::kAcceleratorLocalNic},
      {"same_pcie_switch", LocalityClass::kSamePcieSwitch},
      {"same_root_complex", LocalityClass::kSameRootComplex},
      {"same_numa_node", LocalityClass::kSameNumaNode},
      {"domain_local", LocalityClass::kDomainLocal},
  };
  return parse_from(text, kTable, out);
}

std::string_view to_string(FailureKind value) noexcept {
  switch (value) {
    case FailureKind::kNone: return "none";
    case FailureKind::kTransportFailure: return "transport_failure";
    case FailureKind::kPreDispatchRejection: return "pre_dispatch_rejection";
    case FailureKind::kPostDispatchRejection: return "post_dispatch_rejection";
    case FailureKind::kWorkerDeath: return "worker_death";
    case FailureKind::kDomainUnavailable: return "domain_unavailable";
    case FailureKind::kCapacityExhausted: return "capacity_exhausted";
    case FailureKind::kBackendRejection: return "backend_rejection";
    case FailureKind::kIntegrityFailure: return "integrity_failure";
    case FailureKind::kExecutionFailure: return "execution_failure";
    case FailureKind::kAmbiguousOutcome: return "ambiguous_outcome";
    case FailureKind::kCancelled: return "cancelled";
    case FailureKind::kCoordinatorRestart: return "coordinator_restart";
  }
  return "none";
}

bool parse_enum(std::string_view text, FailureKind& out) noexcept {
  static constexpr std::pair<std::string_view, FailureKind> kTable[] = {
      {"none", FailureKind::kNone},
      {"transport_failure", FailureKind::kTransportFailure},
      {"pre_dispatch_rejection", FailureKind::kPreDispatchRejection},
      {"post_dispatch_rejection", FailureKind::kPostDispatchRejection},
      {"worker_death", FailureKind::kWorkerDeath},
      {"domain_unavailable", FailureKind::kDomainUnavailable},
      {"capacity_exhausted", FailureKind::kCapacityExhausted},
      {"backend_rejection", FailureKind::kBackendRejection},
      {"integrity_failure", FailureKind::kIntegrityFailure},
      {"execution_failure", FailureKind::kExecutionFailure},
      {"ambiguous_outcome", FailureKind::kAmbiguousOutcome},
      {"cancelled", FailureKind::kCancelled},
      {"coordinator_restart", FailureKind::kCoordinatorRestart},
  };
  return parse_from(text, kTable, out);
}

std::string_view to_string(RankingFactor value) noexcept {
  switch (value) {
    case RankingFactor::kDataLocality: return "data_locality";
    case RankingFactor::kMemoryLocality: return "memory_locality";
    case RankingFactor::kNicLocality: return "nic_locality";
    case RankingFactor::kAcceleratorLocality: return "accelerator_locality";
    case RankingFactor::kPcieAffinity: return "pcie_affinity";
    case RankingFactor::kAvoidedHostCopies: return "avoided_host_copies";
    case RankingFactor::kTransferCost: return "transfer_cost";
    case RankingFactor::kSetupCost: return "setup_cost";
    case RankingFactor::kQueueDelay: return "queue_delay";
    case RankingFactor::kExecutionThroughput: return "execution_throughput";
    case RankingFactor::kCompletionLatency: return "completion_latency";
    case RankingFactor::kOffloadOverhead: return "offload_overhead";
    case RankingFactor::kCpuPreservation: return "cpu_preservation";
    case RankingFactor::kAcceleratorPreservation: return "accelerator_preservation";
    case RankingFactor::kPowerEfficiency: return "power_efficiency";
    case RankingFactor::kUtilizationHeadroom: return "utilization_headroom";
    case RankingFactor::kCongestionAvoidance: return "congestion_avoidance";
    case RankingFactor::kFailureDomainDiversity: return "failure_domain_diversity";
    case RankingFactor::kIsolationQuality: return "isolation_quality";
    case RankingFactor::kReconfigurationPenalty: return "reconfiguration_penalty";
    case RankingFactor::kFallbackCost: return "fallback_cost";
    case RankingFactor::kPolicyPreference: return "policy_preference";
  }
  return "unknown";
}

bool parse_enum(std::string_view text, RankingFactor& out) noexcept {
  static constexpr std::pair<std::string_view, RankingFactor> kTable[] = {
      {"data_locality", RankingFactor::kDataLocality},
      {"memory_locality", RankingFactor::kMemoryLocality},
      {"nic_locality", RankingFactor::kNicLocality},
      {"accelerator_locality", RankingFactor::kAcceleratorLocality},
      {"pcie_affinity", RankingFactor::kPcieAffinity},
      {"avoided_host_copies", RankingFactor::kAvoidedHostCopies},
      {"transfer_cost", RankingFactor::kTransferCost},
      {"setup_cost", RankingFactor::kSetupCost},
      {"queue_delay", RankingFactor::kQueueDelay},
      {"execution_throughput", RankingFactor::kExecutionThroughput},
      {"completion_latency", RankingFactor::kCompletionLatency},
      {"offload_overhead", RankingFactor::kOffloadOverhead},
      {"cpu_preservation", RankingFactor::kCpuPreservation},
      {"accelerator_preservation", RankingFactor::kAcceleratorPreservation},
      {"power_efficiency", RankingFactor::kPowerEfficiency},
      {"utilization_headroom", RankingFactor::kUtilizationHeadroom},
      {"congestion_avoidance", RankingFactor::kCongestionAvoidance},
      {"failure_domain_diversity", RankingFactor::kFailureDomainDiversity},
      {"isolation_quality", RankingFactor::kIsolationQuality},
      {"reconfiguration_penalty", RankingFactor::kReconfigurationPenalty},
      {"fallback_cost", RankingFactor::kFallbackCost},
      {"policy_preference", RankingFactor::kPolicyPreference},
  };
  return parse_from(text, kTable, out);
}

std::string_view to_string(ReservationState value) noexcept {
  switch (value) {
    case ReservationState::kActive: return "ACTIVE";
    case ReservationState::kCommitted: return "COMMITTED";
    case ReservationState::kReleased: return "RELEASED";
    case ReservationState::kRolledBack: return "ROLLED_BACK";
    case ReservationState::kInvalidated: return "INVALIDATED";
  }
  return "ACTIVE";
}

bool parse_enum(std::string_view text, ReservationState& out) noexcept {
  static constexpr std::pair<std::string_view, ReservationState> kTable[] = {
      {"active", ReservationState::kActive},
      {"committed", ReservationState::kCommitted},
      {"released", ReservationState::kReleased},
      {"rolled_back", ReservationState::kRolledBack},
      {"invalidated", ReservationState::kInvalidated},
  };
  return parse_from(text, kTable, out);
}

std::string_view to_string(DomainUpdateKind value) noexcept {
  switch (value) {
    case DomainUpdateKind::kRegistered: return "registered";
    case DomainUpdateKind::kCapabilityChanged: return "capability_changed";
    case DomainUpdateKind::kLoadChanged: return "load_changed";
    case DomainUpdateKind::kHealthChanged: return "health_changed";
    case DomainUpdateKind::kLocalityChanged: return "locality_changed";
    case DomainUpdateKind::kTopologyChanged: return "topology_changed";
    case DomainUpdateKind::kCompatibilityChanged: return "compatibility_changed";
    case DomainUpdateKind::kIsolationChanged: return "isolation_changed";
    case DomainUpdateKind::kCapacityChanged: return "capacity_changed";
    case DomainUpdateKind::kBackendGenerationChanged: return "backend_generation_changed";
    case DomainUpdateKind::kFenced: return "fenced";
    case DomainUpdateKind::kRemoved: return "removed";
    case DomainUpdateKind::kUnchanged: return "unchanged";
  }
  return "unchanged";
}

bool parse_enum(std::string_view text, DomainUpdateKind& out) noexcept {
  static constexpr std::pair<std::string_view, DomainUpdateKind> kTable[] = {
      {"registered", DomainUpdateKind::kRegistered},
      {"capability_changed", DomainUpdateKind::kCapabilityChanged},
      {"load_changed", DomainUpdateKind::kLoadChanged},
      {"health_changed", DomainUpdateKind::kHealthChanged},
      {"locality_changed", DomainUpdateKind::kLocalityChanged},
      {"topology_changed", DomainUpdateKind::kTopologyChanged},
      {"compatibility_changed", DomainUpdateKind::kCompatibilityChanged},
      {"isolation_changed", DomainUpdateKind::kIsolationChanged},
      {"capacity_changed", DomainUpdateKind::kCapacityChanged},
      {"backend_generation_changed", DomainUpdateKind::kBackendGenerationChanged},
      {"fenced", DomainUpdateKind::kFenced},
      {"removed", DomainUpdateKind::kRemoved},
      {"unchanged", DomainUpdateKind::kUnchanged},
  };
  return parse_from(text, kTable, out);
}

std::string_view to_string(PlanState value) noexcept {
  switch (value) {
    case PlanState::kCreated: return "CREATED";
    case PlanState::kReserved: return "RESERVED";
    case PlanState::kDispatched: return "DISPATCHED";
    case PlanState::kConsumed: return "CONSUMED";
    case PlanState::kCancelled: return "CANCELLED";
    case PlanState::kRejected: return "REJECTED";
  }
  return "CREATED";
}

bool parse_enum(std::string_view text, PlanState& out) noexcept {
  static constexpr std::pair<std::string_view, PlanState> kTable[] = {
      {"created", PlanState::kCreated},
      {"reserved", PlanState::kReserved},
      {"dispatched", PlanState::kDispatched},
      {"consumed", PlanState::kConsumed},
      {"cancelled", PlanState::kCancelled},
      {"rejected", PlanState::kRejected},
  };
  return parse_from(text, kTable, out);
}

std::string_view to_string(HookPoint value) noexcept {
  switch (value) {
    case HookPoint::kAfterEligibility: return "after_eligibility";
    case HookPoint::kAfterRanking: return "after_ranking";
    case HookPoint::kBeforeReserve: return "before_reserve";
    case HookPoint::kAfterReserve: return "after_reserve";
    case HookPoint::kBeforeRevalidate: return "before_revalidate";
    case HookPoint::kAfterRevalidate: return "after_revalidate";
    case HookPoint::kBeforeAttemptRegistration: return "before_attempt_registration";
    case HookPoint::kAfterAttemptRegistration: return "after_attempt_registration";
    case HookPoint::kBeforeChannelSend: return "before_channel_send";
    case HookPoint::kAfterChannelSend: return "after_channel_send";
    case HookPoint::kBeforeCompletionCommit: return "before_completion_commit";
    case HookPoint::kAfterCompletionCommit: return "after_completion_commit";
    case HookPoint::kBeforeFallback: return "before_fallback";
    case HookPoint::kAfterFallbackSelection: return "after_fallback_selection";
    case HookPoint::kDuringCancellation: return "during_cancellation";
    case HookPoint::kBeforeShutdownFence: return "before_shutdown_fence";
    case HookPoint::kAfterShutdownFence: return "after_shutdown_fence";
    case HookPoint::kBeforeReservationCommit: return "before_reservation_commit";
    case HookPoint::kAfterReservationCommit: return "after_reservation_commit";
  }
  return "after_eligibility";
}

bool parse_enum(std::string_view text, HookPoint& out) noexcept {
  static constexpr std::pair<std::string_view, HookPoint> kTable[] = {
      {"after_eligibility", HookPoint::kAfterEligibility},
      {"after_ranking", HookPoint::kAfterRanking},
      {"before_reserve", HookPoint::kBeforeReserve},
      {"after_reserve", HookPoint::kAfterReserve},
      {"before_revalidate", HookPoint::kBeforeRevalidate},
      {"after_revalidate", HookPoint::kAfterRevalidate},
      {"before_attempt_registration", HookPoint::kBeforeAttemptRegistration},
      {"after_attempt_registration", HookPoint::kAfterAttemptRegistration},
      {"before_channel_send", HookPoint::kBeforeChannelSend},
      {"after_channel_send", HookPoint::kAfterChannelSend},
      {"before_completion_commit", HookPoint::kBeforeCompletionCommit},
      {"after_completion_commit", HookPoint::kAfterCompletionCommit},
      {"before_fallback", HookPoint::kBeforeFallback},
      {"after_fallback_selection", HookPoint::kAfterFallbackSelection},
      {"during_cancellation", HookPoint::kDuringCancellation},
      {"before_shutdown_fence", HookPoint::kBeforeShutdownFence},
      {"after_shutdown_fence", HookPoint::kAfterShutdownFence},
      {"before_reservation_commit", HookPoint::kBeforeReservationCommit},
      {"after_reservation_commit", HookPoint::kAfterReservationCommit},
  };
  return parse_from(text, kTable, out);
}

}  // namespace tos
