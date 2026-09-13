// Transport Offload Scheduler: planning, authority, reservation, dispatch, completion.
//
// Locking doctrine
// ----------------
// A thread may acquire scheduler locks only in this order and never in reverse:
//   1. registry_mutex        (operation class registry)
//   2. policy_mutex          (scheduling policy)
//   3. plans_mutex           (plans of record)
//   4. DomainRegistry        (internal shared_mutex)
//   5. WorkerAuthority       (internal mutex)
//   6. ReservationLedger     (one shard)
//   7. AttemptLedger         (one shard, one operation shard)
//   8. reports_mutex / recovered_mutex
// No code path holds two shard locks at once. No lock is ever held while:
//   * calling a dispatch channel, backend or transport,
//   * invoking an interleaving hook,
//   * performing filesystem I/O,
//   * joining or creating threads.
// Every such call is made after the relevant lock has been released, which is why
// the code below copies state out of a critical section before acting on it.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "tos/core/scheduler.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "tos/core/planner.hpp"
#include "tos/persist/store.hpp"
#include "tos/util/log.hpp"
#include "tos/util/random.hpp"

namespace tos {
namespace {

SelectionOutcome outcome_for_type(ExecutionDomainType type) noexcept {
  switch (type) {
    case ExecutionDomainType::kCpu: return SelectionOutcome::kCpuSelected;
    case ExecutionDomainType::kAccelerator: return SelectionOutcome::kAcceleratorSelected;
    case ExecutionDomainType::kNic: return SelectionOutcome::kNicSelected;
    case ExecutionDomainType::kSmartNic: return SelectionOutcome::kSmartNicSelected;
    case ExecutionDomainType::kDpu: return SelectionOutcome::kDpuSelected;
    case ExecutionDomainType::kOtherRegisteredOffloadEngine:
      return SelectionOutcome::kOffloadSelected;
  }
  return SelectionOutcome::kOffloadSelected;
}

SelectionOutcome outcome_for_reason(IneligibilityReason reason) noexcept {
  switch (reason) {
    case IneligibilityReason::kPolicyForbidden:
    case IneligibilityReason::kDomainNotAllowed:
    case IneligibilityReason::kProvenanceDisallowed: return SelectionOutcome::kPolicyRejected;
    case IneligibilityReason::kOperationNotSupported:
    case IneligibilityReason::kUnknownOperationClass:
    case IneligibilityReason::kMemoryDomainUnsupported:
    case IneligibilityReason::kAddressabilityUnsupported:
    case IneligibilityReason::kPayloadTooLarge:
    case IneligibilityReason::kPayloadTooSmall:
    case IneligibilityReason::kAlignmentUnsatisfied:
    case IneligibilityReason::kCapabilityUnproven:
    case IneligibilityReason::kBackendUnsupported:
    case IneligibilityReason::kTransportClassUnsupported:
    case IneligibilityReason::kPayloadClassUnsupported:
    case IneligibilityReason::kDmaRequired:
    case IneligibilityReason::kScatterGatherRequired: return SelectionOutcome::kCapabilityUnsupported;
    case IneligibilityReason::kStaleCapability:
    case IneligibilityReason::kHealthNotReady:
    case IneligibilityReason::kWorkerBootStale:
    case IneligibilityReason::kDomainFenced:
    case IneligibilityReason::kGenerationRegressed:
    case IneligibilityReason::kBackendGenerationStale:
    case IneligibilityReason::kAuthorityInvalidated: return SelectionOutcome::kStaleEvidence;
    case IneligibilityReason::kIsolationInsufficient: return SelectionOutcome::kIsolationRejected;
    case IneligibilityReason::kCompatibilityMismatch: return SelectionOutcome::kCompatibilityRejected;
    case IneligibilityReason::kCapacityUnavailable:
    case IneligibilityReason::kConcurrencyExhausted:
    case IneligibilityReason::kReservationConflict: return SelectionOutcome::kCapacityUnavailable;
    case IneligibilityReason::kLocalityViolation:
    case IneligibilityReason::kDomainNotRegistered:
    case IneligibilityReason::kNone: return SelectionOutcome::kNoEligibleDomain;
  }
  return SelectionOutcome::kNoEligibleDomain;
}

void add_reason(DecisionExplanation& explanation, std::string code, std::string detail = {}) {
  DecisionReason reason;
  reason.code = bounded_text(std::move(code), kMaxNameLength);
  reason.detail = bounded_text(detail);
  explanation.reasons.push_back(std::move(reason));
}

bool is_terminal_attempt(AttemptState state) noexcept {
  switch (state) {
    case AttemptState::kCompleted:
    case AttemptState::kFailed:
    case AttemptState::kOutcomeUnknown:
    case AttemptState::kCancelled:
    case AttemptState::kRejectedStale:
    case AttemptState::kFenced: return true;
    default: return false;
  }
}

}  // namespace

struct Scheduler::Impl {
  explicit Impl(SchedulerOptions options_in) : options(std::move(options_in)) {}

  SchedulerOptions options;
  SchedulerLimits limits;

  mutable std::shared_mutex policy_mutex;
  SchedulerPolicy policy;

  mutable std::mutex registry_mutex;
  OperationClassRegistry registry;

  DomainRegistry domains;
  WorkerAuthority workers;
  ReservationLedger reservations;
  AttemptLedger attempts;

  mutable std::mutex channel_mutex;
  std::shared_ptr<IDispatchChannel> channel;

  mutable std::mutex plans_mutex;
  std::unordered_map<std::uint64_t, ExecutionPlan> plans;
  /// Plans that were cancelled or consumed. A caller holding a stale copy of a plan
  /// must not be able to dispatch it, so the cancellation is remembered in the
  /// scheduler and not only in the copy the caller happens to hold.
  std::unordered_set<std::uint64_t> retired_plans;
  std::deque<std::uint64_t> retired_order;

  mutable std::mutex reports_mutex;
  mutable std::vector<ReconciliationReport> reports;

  mutable std::mutex recovered_mutex;
  std::vector<ExecutionDomainRecord> recovered_domains;
  bool recovered_from_state{false};

  std::atomic<bool> running{false};
  std::atomic<bool> stopping{false};
  std::atomic<IInterleavingHook*> hook{nullptr};

  std::atomic<std::uint64_t> operation_counter{0};
  std::atomic<std::uint64_t> plan_sequence{0};
  std::atomic<std::uint64_t> dispatch_counter{0};
  mutable std::atomic<std::uint64_t> snapshot_counter{0};
  std::atomic<std::uint64_t> domain_counter{0};
  PersistenceGeneration persisted_generation;

  CoordinatorEpoch epoch;
  WorkerId local_worker;
  WorkerBootId local_boot;

  std::atomic<std::uint64_t> stat_plans_created{0};
  std::atomic<std::uint64_t> stat_plans_dispatched{0};
  std::atomic<std::uint64_t> stat_dispatch_rejections{0};
  std::atomic<std::uint64_t> stat_completion_rejections{0};
  std::atomic<std::uint64_t> stat_fallbacks{0};
  std::atomic<std::uint64_t> stat_retries{0};

  // ---- helpers -------------------------------------------------------------

  [[nodiscard]] IInterleavingHook* current_hook() const noexcept { return hook.load(); }

  void fire(HookPoint point, const HookContext& context) {
    IInterleavingHook* target = current_hook();
    if (target == nullptr) return;
    HookContext local = context;
    local.point = point;
    target->at(local);
  }

  [[nodiscard]] SchedulerPolicy policy_copy() const {
    std::shared_lock<std::shared_mutex> lock(policy_mutex);
    return policy;
  }

  [[nodiscard]] std::shared_ptr<IDispatchChannel> channel_copy() const {
    std::lock_guard<std::mutex> lock(channel_mutex);
    return channel;
  }

  [[nodiscard]] const OperationClassDescriptor* descriptor(OperationClassId id) {
    std::lock_guard<std::mutex> lock(registry_mutex);
    return registry.find(id);
  }

  [[nodiscard]] std::string operation_class_name(OperationClassId id) {
    std::lock_guard<std::mutex> lock(registry_mutex);
    const OperationClassDescriptor* found = registry.find(id);
    return found == nullptr ? std::string("UNKNOWN") : found->name;
  }

  [[nodiscard]] std::uint64_t next_operation_id() {
    return operation_counter.fetch_add(1, std::memory_order_relaxed) + 1;
  }

  [[nodiscard]] DispatchId next_dispatch_id() {
    return DispatchId(dispatch_counter.fetch_add(1, std::memory_order_relaxed) + 1);
  }

  void store_plan(const ExecutionPlan& plan_in) {
    std::lock_guard<std::mutex> lock(plans_mutex);
    plans[plan_in.attempt.value()] = plan_in;
  }

  [[nodiscard]] bool find_plan(ExecutionAttemptId id, ExecutionPlan& out) const {
    std::lock_guard<std::mutex> lock(plans_mutex);
    const auto found = plans.find(id.value());
    if (found == plans.end()) return false;
    out = found->second;
    return true;
  }

  void erase_plan(ExecutionAttemptId id) {
    std::lock_guard<std::mutex> lock(plans_mutex);
    plans.erase(id.value());
  }

  /// Retire a plan: it can never become dispatchable again, and the fact is kept
  /// under a bound so that a stale caller copy is refused.
  void retire_plan(ExecutionAttemptId id) {
    std::lock_guard<std::mutex> lock(plans_mutex);
    plans.erase(id.value());
    if (retired_plans.insert(id.value()).second) {
      retired_order.push_back(id.value());
      while (retired_order.size() > limits.max_pending_plans) {
        retired_plans.erase(retired_order.front());
        retired_order.pop_front();
      }
    }
  }

  [[nodiscard]] bool plan_retired(ExecutionAttemptId id) const {
    std::lock_guard<std::mutex> lock(plans_mutex);
    return retired_plans.count(id.value()) != 0U;
  }

  [[nodiscard]] bool update_plan_state(ExecutionAttemptId id, PlanState state) {
    std::lock_guard<std::mutex> lock(plans_mutex);
    const auto found = plans.find(id.value());
    if (found == plans.end()) return false;
    found->second.state = state;
    return true;
  }

  [[nodiscard]] std::size_t plan_count() const {
    std::lock_guard<std::mutex> lock(plans_mutex);
    return plans.size();
  }

  // ---- eligibility ---------------------------------------------------------
  [[nodiscard]] CandidateEvaluation evaluate(const ExecutionDomainRecord& domain,
                                             const OperationRequest& request,
                                             const SchedulerPolicy& active_policy,
                                             const OperationClassDescriptor* descriptor_ptr) {
    CandidateEvaluation evaluation;
    evaluation.domain = domain.id;
    evaluation.domain_type = domain.type;
    evaluation.provenance = domain.provenance;
    auto reject = [&evaluation](IneligibilityReason reason, std::string detail) {
      evaluation.eligible = false;
      evaluation.reason = reason;
      evaluation.detail = bounded_text(detail);
      return evaluation;
    };

    if (domain.fenced) {
      return reject(IneligibilityReason::kDomainFenced, "domain authority was withdrawn");
    }
    if (!workers.is_current(domain.worker, domain.worker_boot)) {
      return reject(IneligibilityReason::kWorkerBootStale,
                    "no live worker incarnation owns this domain");
    }
    if (active_policy.domain_type_forbidden(domain.type) ||
        active_policy.domain_id_forbidden(domain.id)) {
      return reject(IneligibilityReason::kPolicyForbidden, "domain is forbidden by policy");
    }
    if (active_policy.offload_requirement == OffloadRequirement::kOffloadRequired &&
        domain.type == ExecutionDomainType::kCpu) {
      return reject(IneligibilityReason::kPolicyForbidden,
                    "policy requires offload execution; host CPU is not a legal fallback");
    }
    if (active_policy.offload_requirement == OffloadRequirement::kHostRequired &&
        domain.type != ExecutionDomainType::kCpu) {
      return reject(IneligibilityReason::kPolicyForbidden,
                    "policy requires host execution; offload engines are not legal");
    }
    if (active_policy.provenance_forbidden(domain.provenance)) {
      return reject(IneligibilityReason::kProvenanceDisallowed, "domain provenance is forbidden");
    }
    if (domain.provenance == Provenance::kUnsupported &&
        !active_policy.allow_unsupported_provenance) {
      return reject(IneligibilityReason::kBackendUnsupported,
                    "domain is UNSUPPORTED on this host and policy does not allow it");
    }
    if (!request.allowed_domain_ids.empty() &&
        std::find(request.allowed_domain_ids.begin(), request.allowed_domain_ids.end(), domain.id) ==
            request.allowed_domain_ids.end()) {
      return reject(IneligibilityReason::kDomainNotAllowed, "domain identity not in the allow list");
    }
    if (!request.allowed_domains.empty() &&
        std::find(request.allowed_domains.begin(), request.allowed_domains.end(), domain.type) ==
            request.allowed_domains.end()) {
      return reject(IneligibilityReason::kDomainNotAllowed, "domain class not in the allow list");
    }
    if (std::find(request.forbidden_domains.begin(), request.forbidden_domains.end(), domain.type) !=
        request.forbidden_domains.end()) {
      return reject(IneligibilityReason::kDomainNotAllowed, "domain class is explicitly forbidden");
    }
    if (descriptor_ptr == nullptr) {
      return reject(IneligibilityReason::kUnknownOperationClass, "operation class is not registered");
    }

    const CapabilityCheck capability =
        check_capability(domain.capability, registry, request, active_policy.require_positive_evidence);
    if (!capability.supported) {
      return reject(capability.reason, capability.detail);
    }

    const IsolationClass required_isolation =
        std::max(active_policy.minimum_isolation, request.required_isolation);
    if (required_isolation != IsolationClass::kUnknown &&
        domain.isolation < required_isolation) {
      return reject(IneligibilityReason::kIsolationInsufficient,
                    std::string("domain isolation ") + std::string(to_string(domain.isolation)) +
                        " is below the required " + std::string(to_string(required_isolation)));
    }

    const CompatibilityRequirements& compatibility = request.compatibility;
    if (domain.compatibility.driver_backend_version < compatibility.minimum_driver_backend_version ||
        domain.capability.capability.driver_backend_version <
            compatibility.minimum_driver_backend_version) {
      return reject(IneligibilityReason::kCompatibilityMismatch,
                    "driver/backend version is below the required minimum");
    }
    if (domain.compatibility.firmware_generation < compatibility.minimum_firmware_generation) {
      return reject(IneligibilityReason::kCompatibilityMismatch,
                    "firmware generation is below the required minimum");
    }
    if (compatibility.required_protocol_version != 0 &&
        domain.capability.capability.protocol_version < compatibility.required_protocol_version) {
      return reject(IneligibilityReason::kCompatibilityMismatch,
                    "protocol version is below the required minimum");
    }
    if (compatibility.required_accelerator_arch != 0 &&
        domain.compatibility.accelerator_arch != compatibility.required_accelerator_arch) {
      return reject(IneligibilityReason::kCompatibilityMismatch,
                    "accelerator architecture does not match the requirement");
    }
    if (!compatibility.required_backend_family.empty() &&
        domain.compatibility.backend_family != compatibility.required_backend_family) {
      return reject(IneligibilityReason::kCompatibilityMismatch,
                    "backend family does not match the requirement");
    }

    if (request.locality.minimum_class != LocalityClass::kUnknown &&
        domain.locality.class_to_payload < request.locality.minimum_class) {
      return reject(IneligibilityReason::kLocalityViolation,
                    std::string("locality ") +
                        std::string(to_string(domain.locality.class_to_payload)) +
                        " is weaker than the required " +
                        std::string(to_string(request.locality.minimum_class)));
    }
    if (request.locality.require_same_host &&
        domain.locality.class_to_payload < LocalityClass::kSameHost) {
      return reject(IneligibilityReason::kLocalityViolation, "payload and engine are not on one host");
    }
    if (request.locality.required_numa_node != 0 &&
        domain.locality.numa_node != request.locality.required_numa_node) {
      return reject(IneligibilityReason::kLocalityViolation, "domain is not on the required NUMA node");
    }
    if (request.locality.require_local_nic && !domain.locality.has_local_nic) {
      return reject(IneligibilityReason::kLocalityViolation, "domain has no local NIC evidence");
    }

    if (active_policy.freshness.health && !domain.load.published()) {
      return reject(IneligibilityReason::kHealthNotReady,
                    "health and load evidence is not current");
    }
    if (domain.load.published() &&
        (!domain.load.healthy || !domain.load.ready || !domain.load.accepting)) {
      return reject(IneligibilityReason::kHealthNotReady, "domain is not healthy and ready");
    }
    if (!active_policy.freshness.health && domain.load.published() && !domain.load.healthy) {
      return reject(IneligibilityReason::kHealthNotReady, "domain reports itself unhealthy");
    }

    if (active_policy.reservation.enabled) {
      const bool needs_reservation =
          !active_policy.reservation.require_reservation_for_offload_only ||
          domain.is_offload_engine();
      if (needs_reservation) {
        ResourceAmounts amounts{};
        for (std::size_t i = 0; i < kResourceKindCount; ++i) {
          amounts[i] = active_policy.reservation.per_operation[i];
        }
        if (!reservations.has_capacity(domain.id, amounts)) {
          return reject(IneligibilityReason::kCapacityUnavailable,
                        "declared execution capacity is exhausted");
        }
      }
    }

    evaluation.eligible = true;
    evaluation.reason = IneligibilityReason::kNone;
    evaluation.detail = "eligible";
    return evaluation;
  }

  // ---- planning ------------------------------------------------------------
  struct PlanContext {
    const OperationRequest* request{nullptr};
    const std::vector<std::uint8_t>* payload{nullptr};
    std::vector<ExecutionDomainType> restrict_types;
    std::vector<ExecutionDomainType> chain_tried;
    bool fallback{false};
    std::uint32_t fallback_depth{0};
    ExecutionDomainId fallback_from;
    std::uint32_t retry_index{0};
  };

  [[nodiscard]] PlanResult plan_internal(const PlanContext& context) {
    const OperationRequest& request = *context.request;
    PlanResult result;
    SchedulerPolicy active_policy = policy_copy();
    DecisionExplanation& explanation = result.explanation;
    explanation.policy_generation = active_policy.generation;

    std::string class_name = "UNKNOWN";
    SideEffectClass class_side_effect = SideEffectClass::kUnknown;
    {
      std::lock_guard<std::mutex> lock(registry_mutex);
      const OperationClassDescriptor* found = registry.find(request.operation_class);
      if (found == nullptr) {
        result.status = Status::failure("request.unknown_operation_class");
        explanation.outcome = SelectionOutcome::kCapabilityUnsupported;
        add_reason(explanation, "operation_class_unregistered");
        return result;
      }
      class_name = found->name;
      class_side_effect = found->side_effect_class;
      const Status valid = validate_request(request, registry);
      if (!valid) {
        result.status = Status::failure(valid.code, valid.message);
        explanation.outcome = SelectionOutcome::kPolicyRejected;
        add_reason(explanation, valid.code, valid.message);
        return result;
      }
    }

    explanation.operation_class_name = class_name;
    explanation.operation = request.operation_id;
    explanation.fallback_used = context.fallback;
    explanation.fallback_depth = context.fallback_depth;
    explanation.fallback_from = context.fallback_from;
    explanation.fallback_chain_tried = context.chain_tried;

    if (context.payload != nullptr && context.payload->size() > limits.max_payload_dispatch_bytes) {
      result.status = Status::failure("plan.payload_too_large");
      explanation.outcome = SelectionOutcome::kPolicyRejected;
      add_reason(explanation, "plan.payload_too_large",
                 "payload exceeds the configured dispatch bound");
      return result;
    }

    std::vector<ExecutionDomainRecord> candidates = domains.all();
    std::vector<ExecutionDomainRecord> eligible;
    std::vector<RankedCandidate> ranking;
    std::map<IneligibilityReason, std::uint64_t> reason_histogram;

    {
      // The registry lock is held only across hard eligibility: capability checks
      // read the operation class table, and no hook or I/O runs inside this scope.
      std::lock_guard<std::mutex> registry_lock(registry_mutex);
      const OperationClassDescriptor* descriptor_ptr = registry.find(request.operation_class);
      for (const ExecutionDomainRecord& domain : candidates) {
        if (!context.restrict_types.empty() &&
            std::find(context.restrict_types.begin(), context.restrict_types.end(), domain.type) ==
                context.restrict_types.end()) {
          continue;
        }
        CandidateEvaluation evaluation = evaluate(domain, request, active_policy, descriptor_ptr);
        ++explanation.evaluated_candidates;
        if (evaluation.eligible) {
          eligible.push_back(domain);
        } else {
          ++explanation.rejected_candidates;
          reason_histogram[evaluation.reason] += 1;
        }
        if (explanation.candidates.size() < active_policy.max_reported_candidates) {
          explanation.candidates.push_back(std::move(evaluation));
        }
      }
    }

    fire(HookPoint::kAfterEligibility, HookContext{});

    if (eligible.empty()) {
      if (candidates.empty()) {
        explanation.outcome = SelectionOutcome::kNoEligibleDomain;
        add_reason(explanation, "no_domains_registered");
      } else {
        IneligibilityReason dominant = IneligibilityReason::kNone;
        std::uint64_t best_count = 0;
        for (const auto& entry : reason_histogram) {
          if (entry.second > best_count ||
              (entry.second == best_count &&
               static_cast<int>(entry.first) < static_cast<int>(dominant))) {
            dominant = entry.first;
            best_count = entry.second;
          }
        }
        if (active_policy.offload_requirement == OffloadRequirement::kOffloadRequired &&
            reason_histogram.count(IneligibilityReason::kPolicyForbidden) != 0U) {
          explanation.outcome = SelectionOutcome::kPolicyRejected;
          add_reason(explanation, "offload_required.no_offload_domain",
                     "policy requires offload execution and no offload domain is eligible");
        } else {
          explanation.outcome = outcome_for_reason(dominant);
          add_reason(explanation, std::string("rejected.") + std::string(to_string(dominant)),
                     std::to_string(best_count) + " candidate(s) rejected");
        }
      }
      result.status = Status::failure("plan.no_eligible_domain",
                                      std::string(to_string(explanation.outcome)));
      return result;
    }

    std::uint64_t total_setup_cost = 1;
    for (const ExecutionDomainRecord& domain : eligible) {
      total_setup_cost = std::max<std::uint64_t>(
          total_setup_cost,
          active_policy.type_setup_cost_units[static_cast<std::size_t>(domain.type)]);
    }

    const ExecutionDomainRecord* previous = nullptr;
    if (context.fallback_from.valid()) {
      for (const ExecutionDomainRecord& domain : candidates) {
        if (domain.id == context.fallback_from) {
          previous = &domain;
          break;
        }
      }
    }

    ranking.reserve(eligible.size());
    for (const ExecutionDomainRecord& domain : eligible) {
      FactorInputs inputs;
      inputs.domain = &domain;
      inputs.capability = &domain.capability.capability;
      inputs.request = &request;
      inputs.policy = &active_policy;
      inputs.previous = previous;
      inputs.total_setup_cost_units = total_setup_cost;
      inputs.fallback_depth = context.fallback_depth;
      RankedCandidate ranked;
      ranked.domain_id = domain.id.value();
      ranked.domain_type = static_cast<std::uint8_t>(domain.type);
      ranked.factors = compute_factors(inputs);
      ranked.weighted_score = weighted_score(ranked.factors, active_policy.weights,
                                             ranked.total_weight);
      ranking.push_back(ranked);
    }
    std::sort(ranking.begin(), ranking.end(), candidate_better);
    for (std::size_t i = 0; i < ranking.size(); ++i) {
      ranking[i].rank = static_cast<std::uint32_t>(i + 1);
    }
    explanation.ranking = ranking;
    fire(HookPoint::kAfterRanking, HookContext{});

    const RankedCandidate& winner = ranking.front();
    const ExecutionDomainRecord* selected = nullptr;
    for (const ExecutionDomainRecord& domain : eligible) {
      if (domain.id.value() == winner.domain_id) {
        selected = &domain;
        break;
      }
    }
    if (selected == nullptr) {
      result.status = Status::failure("plan.internal_inconsistency");
      explanation.outcome = SelectionOutcome::kNoEligibleDomain;
      return result;
    }

    if (plan_count() >= limits.max_pending_plans) {
      result.status = Status::failure("plan.limit_reached");
      explanation.outcome = SelectionOutcome::kDeferred;
      add_reason(explanation, "plan.limit_reached");
      return result;
    }

    OperationRequest effective = request;
    if (!effective.operation_id.valid()) {
      effective.operation_id = TransportOperationId(next_operation_id());
    } else if (effective.operation_id.value() > operation_counter.load()) {
      operation_counter.store(effective.operation_id.value());
    }

    ExecutionPlan plan;
    plan.operation = effective.operation_id;
    plan.operation_class = effective.operation_class;
    plan.operation_class_name = explanation.operation_class_name;
    plan.domain = selected->id;
    plan.domain_type = selected->type;
    plan.provenance = selected->provenance;
    plan.side_effect = effective_side_effect(class_side_effect, effective.side_effect_override);
    plan.attempt = attempts.allocate_id();
    plan.attempt_generation = attempts.allocate_generation(plan.operation);
    plan.fallback = context.fallback;
    plan.fallback_depth = context.fallback_depth;
    plan.fallback_from = context.fallback_from;
    plan.retry_index = context.retry_index;
    plan.plan_sequence = plan_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    plan.request = effective;
    if (context.payload != nullptr) plan.payload = *context.payload;

    plan.binding.coordinator_epoch = epoch;
    plan.binding.worker = selected->worker;
    plan.binding.worker_boot = selected->worker_boot;
    plan.binding.domain = selected->id;
    plan.binding.domain_generation = selected->generation;
    plan.binding.capability_generation = selected->capability.generation;
    plan.binding.backend_generation = selected->backend_generation;
    plan.binding.topology_generation = selected->topology.generation;
    plan.binding.locality_generation = selected->locality.generation;
    plan.binding.health_generation = selected->load.health_generation;
    plan.binding.queue_generation = selected->load.queue_generation;
    plan.binding.load_generation = selected->load.load_generation;
    plan.binding.policy_generation = active_policy.generation;
    plan.binding.compatibility_generation = selected->compatibility.generation;
    plan.binding.isolation_generation = active_policy.isolation_generation;
    plan.binding.evidence_generation =
        selected->load.evidence.published() ? selected->load.evidence : selected->capability.evidence;
    plan.binding.operation = plan.operation;
    plan.binding.attempt = plan.attempt;
    plan.binding.attempt_generation = plan.attempt_generation;

    explanation.outcome = context.fallback ? SelectionOutcome::kFallbackSelected
                                           : outcome_for_type(selected->type);
    explanation.selected_domain = selected->id;
    explanation.selected_domain_type = selected->type;
    explanation.selected_provenance = selected->provenance;
    explanation.selected_is_offload = selected->is_offload_engine();
    explanation.operation = plan.operation;
    add_reason(explanation, std::string("selected.") + std::string(to_string(selected->type)),
               std::string("rank 1 of ") + std::to_string(ranking.size()) + " eligible domain(s)");
    if (context.fallback) {
      add_reason(explanation,
                 std::string("fallback.from.") + std::string(to_string(previous == nullptr
                                                                           ? ExecutionDomainType::kCpu
                                                                           : previous->type)));
      explanation.fallback_chain_tried.push_back(selected->type);
    }

    plan.explanation = explanation;
    store_plan(plan);
    stat_plans_created.fetch_add(1, std::memory_order_relaxed);
    result.planned = true;
    result.plan = std::move(plan);
    result.status = Status::success();
    return result;
  }
};

// ---------------------------------------------------------------------------
// Construction and lifecycle
// ---------------------------------------------------------------------------

Scheduler::Scheduler(SchedulerOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {
  impl_->limits = impl_->options.limits;
  if (impl_->options.limits.max_frame_bytes == 0) impl_->limits.max_frame_bytes = 1024 * 1024;
  // The configured domain bound is enforced by the registry, which is the only place
  // that can decide it atomically.
  const Status bounded = impl_->domains.set_max_domains(impl_->limits.max_domains == 0
                                                            ? kMaxExecutionDomains
                                                            : impl_->limits.max_domains);
  (void)bounded;
}

Scheduler::~Scheduler() {
  if (impl_ && impl_->running.load()) {
    const Status status = shutdown();
    (void)status;
  }
}

const OperationClassRegistry& Scheduler::operation_classes() const noexcept {
  return impl_->registry;
}

Checked<OperationClassId> Scheduler::register_operation_class(OperationClassDescriptor descriptor) {
  std::lock_guard<std::mutex> lock(impl_->registry_mutex);
  if (impl_->registry.size() >= impl_->limits.max_operation_classes) {
    return Checked<OperationClassId>::bad("operation.class_limit");
  }
  return impl_->registry.register_class(std::move(descriptor));
}

SchedulerPolicy Scheduler::policy() const { return impl_->policy_copy(); }

Status Scheduler::set_policy(SchedulerPolicy policy_in) {
  const Status valid = validate_policy(policy_in);
  if (!valid) return valid;
  {
    std::unique_lock<std::shared_mutex> lock(impl_->policy_mutex);
    if (policy_in.generation.value() < impl_->policy.generation.value()) {
      return Status::failure("policy.generation_regression");
    }
    if (policy_in.generation == impl_->policy.generation) {
      policy_in.generation = PolicyGeneration(impl_->policy.generation.value() + 1);
    }
    impl_->policy = policy_in;
  }
  return Status::success();
}

PolicyGeneration Scheduler::policy_generation() const noexcept {
  std::shared_lock<std::shared_mutex> lock(impl_->policy_mutex);
  return impl_->policy.generation;
}

const SchedulerLimits& Scheduler::limits() const noexcept { return impl_->limits; }

void Scheduler::set_hook(IInterleavingHook* hook) noexcept { impl_->hook.store(hook); }

void Scheduler::set_dispatch_channel(std::shared_ptr<IDispatchChannel> channel) {
  std::lock_guard<std::mutex> lock(impl_->channel_mutex);
  impl_->channel = std::move(channel);
}

std::shared_ptr<IDispatchChannel> Scheduler::dispatch_channel() const {
  return impl_->channel_copy();
}

DomainRegistry& Scheduler::domains() noexcept { return impl_->domains; }
ReservationLedger& Scheduler::reservations() noexcept { return impl_->reservations; }
AttemptLedger& Scheduler::attempts() noexcept { return impl_->attempts; }
WorkerAuthority& Scheduler::workers() noexcept { return impl_->workers; }

bool Scheduler::running() const noexcept { return impl_->running.load(); }
CoordinatorEpoch Scheduler::coordinator_epoch() const noexcept { return impl_->epoch; }

Status Scheduler::start() {
  if (impl_->running.load()) return Status::failure("scheduler.already_running");
  impl_->stopping.store(false);

  if (impl_->options.enable_persistence && impl_->options.recover_on_start &&
      !impl_->options.state_path.empty()) {
    const Status recovered = load_state();
    if (!recovered && recovered.code != "state.missing" && recovered.code != "file.missing") {
      return recovered;
    }
  }

  {
    std::unique_lock<std::shared_mutex> lock(impl_->policy_mutex);
    if (!impl_->policy.generation.published()) {
      SchedulerPolicy defaults = make_default_policy();
      if (impl_->options.policy.generation.published()) defaults = impl_->options.policy;
      impl_->policy = defaults;
    } else if (impl_->options.policy.generation.published() &&
               impl_->options.policy.generation > impl_->policy.generation) {
      impl_->policy = impl_->options.policy;
    }
  }

  const std::uint64_t base_epoch =
      std::max(impl_->options.initial_epoch.value(), impl_->persisted_generation.published()
                                                       ? impl_->epoch.value()
                                                       : 0ULL);
  impl_->epoch = CoordinatorEpoch(base_epoch + 1);
  impl_->local_worker = WorkerId(process_unique_u64());
  impl_->local_boot = WorkerBootId(process_unique_u64());
  const Status boot = impl_->workers.register_boot(impl_->local_worker, impl_->local_boot, "local");
  if (!boot) return boot;

  {
    std::lock_guard<std::mutex> lock(impl_->reports_mutex);
    impl_->reports.clear();
  }
  impl_->running.store(true);
  return Status::success();
}

Status Scheduler::shutdown() {
  if (!impl_->running.load()) return Status::success();
  impl_->stopping.store(true);
  impl_->fire(HookPoint::kBeforeShutdownFence, HookContext{});

  const std::vector<ExecutionAttempt> live = impl_->attempts.list_live();
  for (const ExecutionAttempt& attempt : live) {
    if (attempt.state == AttemptState::kDispatched ||
        attempt.state == AttemptState::kDispatching) {
      const Status status =
          impl_->attempts.mark_ambiguous(attempt.id, "scheduler shutdown: outcome unknown");
      (void)status;
    } else {
      const Status status =
          impl_->attempts.mark_fenced(attempt.id, false, "scheduler shutdown before dispatch");
      (void)status;
    }
  }

  for (const ExecutionDomainRecord& domain : impl_->domains.all()) {
    impl_->reservations.invalidate_domain(domain.id, "scheduler shutdown");
  }

  std::shared_ptr<IDispatchChannel> channel = impl_->channel_copy();
  if (channel) channel->shutdown();

  {
    std::lock_guard<std::mutex> lock(impl_->plans_mutex);
    impl_->plans.clear();
  }

  impl_->fire(HookPoint::kAfterShutdownFence, HookContext{});

  if (impl_->options.enable_persistence && !impl_->options.state_path.empty()) {
    const Status saved = save_state();
    if (!saved) {
      log_write(LogLevel::kWarn, "scheduler", std::string("state save on shutdown failed: ") + saved.code);
    }
  }
  impl_->running.store(false);
  return Status::success();
}

// ---------------------------------------------------------------------------
// Domain publication
// ---------------------------------------------------------------------------

Status Scheduler::register_domain(const ExecutionDomainRecord& record) {
  if (impl_->stopping.load()) return Status::failure("scheduler.shutting_down");
  ExecutionDomainRecord effective = record;
  if (!effective.worker.valid() || !effective.worker_boot.valid()) {
    effective.worker = impl_->local_worker;
    effective.worker_boot = impl_->local_boot;
  }
  if (effective.name.empty()) {
    effective.name = std::string("domain.") + std::to_string(effective.id.value());
  }
  auto upserted = impl_->domains.upsert(effective);
  if (!upserted.ok()) return upserted.status;
  if (effective.capacity != CapacityVector{}) {
    const Status capacity = impl_->reservations.set_capacity(effective.id, effective.capacity);
    if (!capacity) return capacity;
  }
  return Status::success();
}

Checked<DomainUpdateResult> Scheduler::update_domain(const ExecutionDomainRecord& record) {
  if (impl_->stopping.load()) return Checked<DomainUpdateResult>::bad("scheduler.shutting_down");
  // A locally hosted backend does not know this runtime's incarnation identity, so a
  // record that names no owner is attributed to the local worker instead of erasing
  // the ownership of an already registered domain.
  ExecutionDomainRecord effective = record;
  if (!effective.worker.valid() || !effective.worker_boot.valid()) {
    effective.worker = impl_->local_worker;
    effective.worker_boot = impl_->local_boot;
  }
  auto updated = impl_->domains.upsert(effective);
  if (updated.ok() && effective.capacity != CapacityVector{}) {
    const Status capacity = impl_->reservations.set_capacity(effective.id, effective.capacity);
    if (!capacity) return Checked<DomainUpdateResult>::bad(capacity.code, capacity.message);
  }
  return updated;
}

Status Scheduler::publish_capability(const CapabilityRecord& capability) {
  return impl_->domains.publish_capability(capability);
}

Status Scheduler::publish_load(ExecutionDomainId domain, const DomainLoadEvidence& load) {
  return impl_->domains.publish_load(domain, load);
}

Status Scheduler::publish_locality(ExecutionDomainId domain, const DomainLocality& locality) {
  return impl_->domains.publish_locality(domain, locality);
}

Status Scheduler::publish_topology(ExecutionDomainId domain, const DomainTopology& topology) {
  return impl_->domains.publish_topology(domain, topology);
}

Status Scheduler::publish_compatibility(ExecutionDomainId domain,
                                         const DomainCompatibility& compatibility) {
  return impl_->domains.publish_compatibility(domain, compatibility);
}

Status Scheduler::publish_capacity(ExecutionDomainId domain, const CapacityVector& capacity) {
  const Status published = impl_->domains.publish_capacity(domain, capacity);
  if (!published) return published;
  return impl_->reservations.set_capacity(domain, capacity);
}

Status Scheduler::register_worker_boot(WorkerId worker, WorkerBootId boot, std::string_view label) {
  return impl_->workers.register_boot(worker, boot, label);
}

// ---------------------------------------------------------------------------
// Planning
// ---------------------------------------------------------------------------

PlanResult Scheduler::plan(const OperationRequest& request,
                           std::span<const std::uint8_t> payload) {
  Impl::PlanContext context;
  context.request = &request;
  std::vector<std::uint8_t> payload_copy;
  if (!payload.empty()) {
    if (payload.size() > impl_->limits.max_payload_dispatch_bytes) {
      PlanResult result;
      result.status = Status::failure("plan.payload_too_large");
      result.explanation.outcome = SelectionOutcome::kPolicyRejected;
      add_reason(result.explanation, "plan.payload_too_large");
      return result;
    }
    payload_copy.assign(payload.begin(), payload.end());
    context.payload = &payload_copy;
  }
  if (impl_->stopping.load()) {
    PlanResult result;
    result.status = Status::failure("scheduler.shutting_down");
    result.explanation.outcome = SelectionOutcome::kDeferred;
    add_reason(result.explanation, "scheduler.shutting_down");
    return result;
  }
  return impl_->plan_internal(context);
}

Status Scheduler::reserve(ExecutionPlan& plan) {
  if (impl_->stopping.load()) return Status::failure("scheduler.shutting_down");
  if (plan.state != PlanState::kCreated) return Status::failure("plan.not_reservable");
  const SchedulerPolicy active_policy = impl_->policy_copy();
  if (!active_policy.reservation.enabled) {
    plan.state = PlanState::kReserved;
    impl_->store_plan(plan);
    return Status::success();
  }
  ResourceAmounts amounts{};
  bool any = false;
  for (std::size_t i = 0; i < kResourceKindCount; ++i) {
    amounts[i] = active_policy.reservation.per_operation[i];
    any = any || amounts[i] != 0;
  }
  if (!any) {
    plan.state = PlanState::kReserved;
    impl_->store_plan(plan);
    return Status::success();
  }

  HookContext hook_context;
  hook_context.operation = plan.operation;
  hook_context.attempt = plan.attempt;
  hook_context.domain = plan.domain;
  hook_context.domain_type = plan.domain_type;
  impl_->fire(HookPoint::kBeforeReserve, hook_context);

  ReservationRequest request;
  request.domain = plan.domain;
  request.domain_generation = plan.binding.domain_generation;
  request.capability_generation = plan.binding.capability_generation;
  request.policy_generation = plan.binding.policy_generation;
  request.worker_boot = plan.binding.worker_boot;
  request.operation = plan.operation;
  request.amounts = amounts;
  auto acquired = impl_->reservations.acquire(request);
  impl_->fire(HookPoint::kAfterReserve, hook_context);
  if (!acquired.ok()) {
    return Status::failure(acquired.status.code, acquired.status.message);
  }
  plan.reservation = acquired.value;
  plan.reserved_amounts = amounts;
  plan.binding.reservation = acquired.value;
  plan.state = PlanState::kReserved;
  impl_->store_plan(plan);
  return Status::success();
}

DispatchResult Scheduler::dispatch(ExecutionPlan& plan) {
  DispatchResult result;
  result.attempt = plan.attempt;
  auto fail = [this, &result](DispatchRejection rejection, std::string code,
                              std::string detail = {}) {
    result.dispatched = false;
    result.rejection = rejection;
    result.status = Status::failure(std::move(code), std::move(detail));
    impl_->stat_dispatch_rejections.fetch_add(1, std::memory_order_relaxed);
    return result;
  };
  if (impl_->stopping.load()) {
    return fail(DispatchRejection::kShutdownInProgress, "scheduler.shutting_down");
  }
  if (plan.state == PlanState::kDispatched || plan.state == PlanState::kConsumed ||
      plan.state == PlanState::kCancelled || plan.state == PlanState::kRejected) {
    return fail(DispatchRejection::kPlanConsumed, "plan.consumed");
  }
  if (impl_->plan_retired(plan.attempt)) {
    // The plan of record was cancelled or consumed after this copy was taken.
    return fail(DispatchRejection::kPlanConsumed, "plan.cancelled");
  }

  const SchedulerPolicy active_policy = impl_->policy_copy();
  bool reservation_required = false;
  if (active_policy.reservation.enabled) {
    for (std::size_t i = 0; i < kResourceKindCount; ++i) {
      reservation_required = reservation_required || active_policy.reservation.per_operation[i] != 0;
    }
  }
  (void)active_policy;  // the pre-race snapshot is used only for the reservation gate
  if (reservation_required && !plan.reservation.valid()) {
    return fail(DispatchRejection::kReservationInvalid, "plan.reservation_required",
                "execution capacity must be reserved before dispatch");
  }

  HookContext hook_context;
  hook_context.operation = plan.operation;
  hook_context.attempt = plan.attempt;
  hook_context.domain = plan.domain;
  hook_context.domain_type = plan.domain_type;
  impl_->fire(HookPoint::kBeforeRevalidate, hook_context);

  // The policy is read again here, after the race window: whatever changed while
  // this dispatch was in flight must invalidate the plan rather than be ignored.
  const SchedulerPolicy revalidation_policy = impl_->policy_copy();
  ExecutionDomainRecord domain_record;
  const bool present = impl_->domains.get(plan.domain, domain_record);
  LiveAuthority live;
  if (present) {
    live = live_authority_of(domain_record, impl_->epoch, revalidation_policy.generation,
                             revalidation_policy.isolation_generation);
  }
  const AuthorityValidation validation =
      validate_authority(plan.binding, live, revalidation_policy.freshness);
  impl_->fire(HookPoint::kAfterRevalidate, hook_context);
  if (!validation.valid) {
    plan.state = PlanState::kRejected;
    plan.explanation.outcome = SelectionOutcome::kRevalidationRequired;
    add_reason(plan.explanation, validation.code,
               validation.mismatches.empty() ? std::string()
                                             : validation.mismatches.front().field);
    impl_->store_plan(plan);
    if (plan.reservation.valid()) {
      const Status rolled = impl_->reservations.rollback(plan.reservation, "dispatch revalidation");
      (void)rolled;
    }
    return fail(DispatchRejection::kAuthorityStale, validation.code,
                validation.mismatches.empty() ? std::string("stale authority")
                                              : validation.mismatches.front().field);
  }

  if (revalidation_policy.reservation.enabled && plan.reservation.valid()) {
    impl_->fire(HookPoint::kBeforeReservationCommit, hook_context);
    const Status committed = impl_->reservations.commit(
        plan.reservation, plan.binding.domain_generation, plan.binding.capability_generation,
        plan.binding.policy_generation);
    impl_->fire(HookPoint::kAfterReservationCommit, hook_context);
    if (!committed) {
      plan.state = PlanState::kRejected;
      const Status rolled = impl_->reservations.rollback(plan.reservation, committed.code);
      (void)rolled;
      return fail(DispatchRejection::kReservationInvalid, committed.code, committed.message);
    }
  }

  // The dispatch identity is allocated before the attempt is registered so that a
  // completion can always be validated against the dispatch it answers.
  const DispatchId dispatch_id = impl_->next_dispatch_id();
  AttemptRegistration registration;
  registration.id = plan.attempt;
  registration.generation = plan.attempt_generation;
  registration.operation = plan.operation;
  registration.operation_class = plan.operation_class;
  registration.side_effect = plan.side_effect;
  registration.domain = plan.domain;
  registration.domain_type = plan.domain_type;
  registration.domain_generation = plan.binding.domain_generation;
  registration.capability_generation = plan.binding.capability_generation;
  registration.worker = plan.binding.worker;
  registration.worker_boot = plan.binding.worker_boot;
  registration.coordinator_epoch = plan.binding.coordinator_epoch;
  registration.reservation = plan.reservation;
  registration.dispatch = dispatch_id;
  registration.provenance = plan.provenance;
  registration.fallback = plan.fallback;
  registration.fallback_from = plan.fallback_from;
  registration.retry_index = plan.retry_index;

  impl_->fire(HookPoint::kBeforeAttemptRegistration, hook_context);
  const Status registered = impl_->attempts.register_attempt(registration);
  impl_->fire(HookPoint::kAfterAttemptRegistration, hook_context);
  if (!registered) {
    if (plan.reservation.valid()) {
      const Status rolled = impl_->reservations.rollback(plan.reservation, registered.code);
      (void)rolled;
    }
    return fail(DispatchRejection::kAttemptRegistrationFailed, registered.code, registered.message);
  }

  DispatchEnvelope envelope;
  envelope.attempt = plan.attempt;
  envelope.attempt_generation = plan.attempt_generation;
  envelope.dispatch = dispatch_id;
  envelope.coordinator_epoch = plan.binding.coordinator_epoch;
  envelope.worker = plan.binding.worker;
  envelope.worker_boot = plan.binding.worker_boot;
  envelope.domain = plan.domain;
  envelope.domain_generation = plan.binding.domain_generation;
  envelope.capability_generation = plan.binding.capability_generation;
  envelope.policy_generation = plan.binding.policy_generation;
  envelope.operation = plan.operation;
  envelope.operation_class = plan.operation_class;
  envelope.side_effect = plan.side_effect;
  envelope.provenance = plan.provenance;
  envelope.payload = plan.request.payload;
  envelope.bytes = plan.payload;
  envelope.fallback = plan.fallback;
  envelope.fallback_depth = plan.fallback_depth;
  envelope.retry_index = plan.retry_index;

  std::shared_ptr<IDispatchChannel> channel = impl_->channel_copy();
  if (!channel) {
    const Status failed = impl_->attempts.mark_failed(plan.attempt, FailureKind::kTransportFailure,
                                                      "no dispatch channel is configured");
    (void)failed;
    if (plan.reservation.valid()) {
      const Status released = impl_->reservations.release(plan.reservation, "no dispatch channel");
      (void)released;
    }
    return fail(DispatchRejection::kTransportFailure, "channel.absent");
  }

  impl_->fire(HookPoint::kBeforeChannelSend, hook_context);
  const Status sent = channel->send(envelope);
  impl_->fire(HookPoint::kAfterChannelSend, hook_context);

  if (!sent) {
    const Status failed =
        impl_->attempts.mark_failed(plan.attempt, FailureKind::kTransportFailure, sent.code);
    (void)failed;
    if (plan.reservation.valid()) {
      const Status released = impl_->reservations.release(plan.reservation, "transport failure");
      (void)released;
    }
    plan.state = PlanState::kRejected;
    impl_->store_plan(plan);
    return fail(DispatchRejection::kTransportFailure, sent.code, sent.message);
  }

  const Status marked = impl_->attempts.mark_dispatched(plan.attempt, dispatch_id);
  if (!marked && marked.code != "attempt.not_dispatching") {
    // A fast completion may already have moved the attempt past DISPATCHING, which
    // is exactly why the attempt was registered before the channel was called.
    return fail(DispatchRejection::kAttemptRegistrationFailed, marked.code, marked.message);
  }
  plan.dispatch = dispatch_id;
  plan.state = PlanState::kDispatched;
  impl_->store_plan(plan);

  result.dispatched = true;
  result.dispatch = dispatch_id;
  result.state = AttemptState::kDispatched;
  result.status = Status::success();
  impl_->stat_plans_dispatched.fetch_add(1, std::memory_order_relaxed);
  return result;
}

DispatchOutcome Scheduler::plan_reserve_dispatch(const OperationRequest& request,
                                                 std::span<const std::uint8_t> payload) {
  DispatchOutcome outcome;
  outcome.plan_result = plan(request, payload);
  if (!outcome.plan_result.planned) return outcome;
  ExecutionPlan working = outcome.plan_result.plan;
  const Status reserved = reserve(working);
  if (!reserved) {
    working.state = PlanState::kRejected;
    outcome.plan_result.plan = working;
    outcome.dispatch_result.status = reserved;
    outcome.dispatch_result.rejection = DispatchRejection::kReservationInvalid;
    outcome.dispatch_result.attempt = working.attempt;
    return outcome;
  }
  outcome.dispatch_result = dispatch(working);
  outcome.plan_result.plan = working;
  return outcome;
}

// ---------------------------------------------------------------------------
// Completion authority
// ---------------------------------------------------------------------------

CompletionOutcome Scheduler::complete(const CompletionSubmission& submission) {
  HookContext hook_context;
  hook_context.attempt = submission.attempt;
  hook_context.operation = submission.operation;
  impl_->fire(HookPoint::kBeforeCompletionCommit, hook_context);

  CompletionOutcome outcome;
  if (submission.coordinator_epoch != impl_->epoch) {
    // Traffic from a previous coordinator epoch is rejected before it can touch
    // current state, even if it names an attempt that still exists.
    outcome.rejection = CompletionRejection::kStaleCoordinatorEpoch;
    ExecutionAttempt stored;
    if (impl_->attempts.get(submission.attempt, stored)) outcome.attempt = stored;
    impl_->stat_completion_rejections.fetch_add(1, std::memory_order_relaxed);
    impl_->fire(HookPoint::kAfterCompletionCommit, hook_context);
    return outcome;
  }

  outcome = impl_->attempts.commit_completion(submission);
  impl_->fire(HookPoint::kAfterCompletionCommit, hook_context);

  if (outcome.committed && !outcome.idempotent) {
    if (outcome.attempt.reservation.valid()) {
      const Status released =
          impl_->reservations.release(outcome.attempt.reservation, "completion committed");
      (void)released;
    }
    impl_->erase_plan(outcome.attempt.id);
  } else if (!outcome.committed) {
    impl_->stat_completion_rejections.fetch_add(1, std::memory_order_relaxed);
  }
  return outcome;
}

Status Scheduler::report_failure(ExecutionAttemptId attempt, FailureKind kind,
                                 std::string_view detail) {
  ExecutionAttempt stored;
  if (!impl_->attempts.get(attempt, stored)) return Status::failure("attempt.not_found");
  Status status = Status::success();
  bool release_reservation = false;
  switch (kind) {
    case FailureKind::kAmbiguousOutcome:
    case FailureKind::kWorkerDeath:
      status = impl_->attempts.mark_ambiguous(attempt, detail);
      break;
    case FailureKind::kTransportFailure:
    case FailureKind::kPreDispatchRejection:
    case FailureKind::kBackendRejection:
    case FailureKind::kCapacityExhausted:
    case FailureKind::kDomainUnavailable:
    case FailureKind::kIntegrityFailure:
    case FailureKind::kExecutionFailure:
    case FailureKind::kPostDispatchRejection:
    case FailureKind::kCoordinatorRestart:
    case FailureKind::kCancelled:
    case FailureKind::kNone:
      status = impl_->attempts.mark_failed(attempt, kind, detail);
      release_reservation = true;
      break;
  }
  if (status && release_reservation && stored.reservation.valid()) {
    const Status released = impl_->reservations.release(stored.reservation, to_string(kind));
    (void)released;
  }
  // The plan of record is retained: retry re-runs eligibility from the request the
  // plan carries. It is retired when the operation is superseded, cancelled or
  // completed, which bounds the map.
  return status;
}

// ---------------------------------------------------------------------------
// Retry and fallback
// ---------------------------------------------------------------------------

RetryResult Scheduler::retry(ExecutionAttemptId attempt) {
  RetryResult result;
  ExecutionAttempt stored;
  if (!impl_->attempts.get(attempt, stored)) {
    result.status = Status::failure("attempt.not_found");
    result.rejection = RetryRejection::kPolicyDisallowsRetry;
    return result;
  }
  const SchedulerPolicy active_policy = impl_->policy_copy();
  if (!active_policy.retry.enabled) {
    result.rejection = RetryRejection::kPolicyDisallowsRetry;
    result.status = Status::failure("retry.policy_disabled");
    return result;
  }
  if (!is_terminal_attempt(stored.state)) {
    result.rejection = RetryRejection::kFailureNotRetryable;
    result.status = Status::failure("retry.attempt_not_terminal");
    return result;
  }
  if (stored.resolution == AttemptResolution::kAmbiguous ||
      stored.resolution == AttemptResolution::kFencedMayHaveEffect ||
      stored.resolution == AttemptResolution::kCancelledAmbiguous) {
    result.rejection = RetryRejection::kAmbiguousOutcome;
    result.status = Status::failure("retry.outcome_ambiguous",
                                    "an operation that may already have executed is never replayed "
                                    "automatically");
    return result;
  }
  if (!is_replay_safe(stored.side_effect)) {
    result.rejection = RetryRejection::kNonRepeatableOperation;
    result.status = Status::failure("retry.side_effect_not_replay_safe");
    return result;
  }
  if (!active_policy.retry.rules.allows(stored.failure)) {
    result.rejection = RetryRejection::kFailureNotRetryable;
    result.status = Status::failure("retry.failure_not_retryable", to_string(stored.failure));
    return result;
  }
  const std::uint32_t next_index = stored.retry_index + 1;
  if (next_index + 1 > active_policy.retry.max_attempts ||
      next_index + 1 > impl_->limits.max_retry_attempts) {
    result.rejection = RetryRejection::kAttemptLimitReached;
    result.status = Status::failure("retry.attempt_limit_reached");
    return result;
  }
  ExecutionPlan previous_plan;
  if (!impl_->find_plan(attempt, previous_plan)) {
    result.rejection = RetryRejection::kPolicyDisallowsRetry;
    result.status = Status::failure("retry.plan_not_retained");
    return result;
  }

  Impl::PlanContext context;
  context.request = &previous_plan.request;
  context.payload = &previous_plan.payload;
  context.retry_index = next_index;
  context.fallback = previous_plan.fallback;
  context.fallback_depth = previous_plan.fallback_depth;
  context.fallback_from = previous_plan.fallback_from;
  context.chain_tried = previous_plan.explanation.fallback_chain_tried;
  context.restrict_types = {previous_plan.domain_type};
  PlanResult planned = impl_->plan_internal(context);
  if (!planned.planned) {
    result.rejection = RetryRejection::kDomainUnavailable;
    result.status = planned.status;
    return result;
  }
  impl_->stat_retries.fetch_add(1, std::memory_order_relaxed);
  impl_->retire_plan(attempt);
  add_reason(planned.explanation, "retry.new_generation",
             std::to_string(planned.plan.attempt_generation.value()));
  result.retry_permitted = true;
  result.attempt = planned.plan.attempt;
  result.generation = planned.plan.attempt_generation;
  result.plan = std::move(planned.plan);
  result.status = Status::success();
  return result;
}

FallbackResult Scheduler::fallback(const ExecutionPlan& plan, FailureKind trigger) {
  FallbackResult result;
  const SchedulerPolicy active_policy = impl_->policy_copy();
  if (!active_policy.fallback.enabled) {
    result.rejection = FallbackRejection::kPolicyDisallowsFallback;
    result.status = Status::failure("fallback.policy_disabled");
    return result;
  }
  const std::uint32_t next_depth = plan.fallback_depth + 1;
  if (next_depth > active_policy.fallback.max_depth ||
      next_depth > impl_->limits.max_fallback_depth) {
    result.rejection = FallbackRejection::kFallbackDepthExceeded;
    result.status = Status::failure("fallback.depth_exceeded");
    return result;
  }
  if (fallback_graph_has_cycle(active_policy.fallback)) {
    result.rejection = FallbackRejection::kFallbackCycleDetected;
    result.status = Status::failure("fallback.cycle_detected");
    return result;
  }
  const std::vector<ExecutionDomainType>& chain = active_policy.fallback_chain(plan.domain_type);
  if (chain.empty()) {
    result.rejection = FallbackRejection::kNoFallbackRegistered;
    result.status = Status::failure("fallback.no_chain_registered", to_string(plan.domain_type));
    return result;
  }

  HookContext hook_context;
  hook_context.operation = plan.operation;
  hook_context.attempt = plan.attempt;
  hook_context.domain = plan.domain;
  hook_context.domain_type = plan.domain_type;
  hook_context.fallback_depth = next_depth;
  hook_context.failure = trigger;
  impl_->fire(HookPoint::kBeforeFallback, hook_context);

  std::vector<ExecutionDomainType> tried = plan.explanation.fallback_chain_tried;
  DecisionExplanation last_explanation;
  Status last_status = Status::failure("fallback.no_eligible_target");
  for (ExecutionDomainType target : chain) {
    if (target == plan.domain_type) continue;
    if (std::find(tried.begin(), tried.end(), target) != tried.end()) continue;
    if (target == ExecutionDomainType::kCpu && !active_policy.fallback.allow_host_fallback) {
      continue;
    }
    Impl::PlanContext context;
    context.request = &plan.request;
    context.payload = &plan.payload;
    context.restrict_types = {target};
    context.chain_tried = tried;
    context.fallback = true;
    context.fallback_depth = next_depth;
    context.fallback_from = plan.domain;
    context.retry_index = plan.retry_index;
    PlanResult planned = impl_->plan_internal(context);
    if (planned.planned) {
      add_reason(planned.explanation,
                 std::string("fallback.reason.") + std::string(to_string(trigger)));
      add_reason(planned.explanation,
                 std::string("fallback.from.") + std::string(to_string(plan.domain_type)));
      impl_->stat_fallbacks.fetch_add(1, std::memory_order_relaxed);
      impl_->fire(HookPoint::kAfterFallbackSelection, hook_context);
      result.fallback_selected = true;
      result.plan = std::move(planned.plan);
      result.explanation = std::move(planned.explanation);
      result.chain_tried = tried;
      result.chain_tried.push_back(target);
      result.status = Status::success();
      return result;
    }
    last_explanation = planned.explanation;
    last_status = planned.status;
    tried.push_back(target);
  }

  if (!active_policy.fallback.allow_host_fallback) {
    bool host_in_chain = false;
    for (ExecutionDomainType target : chain) {
      host_in_chain = host_in_chain || target == ExecutionDomainType::kCpu;
    }
    if (host_in_chain) {
      result.rejection = FallbackRejection::kHostFallbackForbidden;
      result.status = Status::failure("fallback.host_forbidden");
      result.explanation = last_explanation;
      result.chain_tried = tried;
      return result;
    }
  }
  result.rejection = FallbackRejection::kFallbackTargetIneligible;
  result.status = last_status;
  result.explanation = last_explanation;
  result.chain_tried = tried;
  return result;
}

// ---------------------------------------------------------------------------
// Cancellation and fencing
// ---------------------------------------------------------------------------

CancellationResult Scheduler::cancel(ExecutionAttemptId attempt) {
  CancellationResult result;
  HookContext hook_context;
  hook_context.attempt = attempt;
  impl_->fire(HookPoint::kDuringCancellation, hook_context);

  ExecutionAttempt stored;
  if (impl_->attempts.get(attempt, stored)) {
    if (is_terminal_attempt(stored.state)) {
      result.state = stored.state;
      result.resolution = stored.resolution;
      result.status = Status::failure("attempt.already_terminal");
      return result;
    }
    const bool crossed =
        stored.state == AttemptState::kDispatched || stored.state == AttemptState::kDispatching;
    std::shared_ptr<IDispatchChannel> channel = impl_->channel_copy();
    if (crossed && channel) {
      CancelEnvelope envelope;
      envelope.attempt = stored.id;
      envelope.attempt_generation = stored.generation;
      envelope.dispatch = stored.dispatch;
      envelope.coordinator_epoch = stored.coordinator_epoch;
      envelope.worker_boot = stored.worker_boot;
      envelope.domain = stored.domain;
      envelope.may_have_executed = true;
      const bool requested = channel->request_cancel(envelope);
      result.ambiguous = !requested || !is_replay_safe(stored.side_effect);
    }
    const Status cancelled = impl_->attempts.mark_cancelled(attempt, crossed, "cancelled by caller");
    if (!cancelled) {
      result.status = cancelled;
      return result;
    }
    if (stored.reservation.valid()) {
      const Status released = impl_->reservations.release(stored.reservation, "cancelled");
      (void)released;
    }
    impl_->erase_plan(attempt);
    result.cancelled = true;
    result.state = AttemptState::kCancelled;
    result.resolution = crossed ? AttemptResolution::kCancelledAmbiguous
                                : AttemptResolution::kCancelledBeforeDispatch;
    result.status = Status::success();
    return result;
  }

  ExecutionPlan pending;
  if (impl_->find_plan(attempt, pending)) {
    if (pending.state != PlanState::kCreated && pending.state != PlanState::kReserved) {
      result.state = AttemptState::kDispatched;
      result.status = Status::failure("plan.consumed");
      return result;
    }
    if (pending.reservation.valid()) {
      Status closed = impl_->reservations.rollback(pending.reservation, "plan cancelled");
      if (!closed) closed = impl_->reservations.release(pending.reservation, "plan cancelled");
      (void)closed;
    }
    impl_->retire_plan(attempt);
    result.cancelled = true;
    result.state = AttemptState::kCancelled;
    result.resolution = AttemptResolution::kCancelledBeforeDispatch;
    result.status = Status::success();
    return result;
  }

  result.status = Status::failure("attempt.not_found");
  return result;
}

Status Scheduler::fence_domain(ExecutionDomainId domain, std::string_view reason) {
  ExecutionDomainRecord record;
  if (!impl_->domains.get(domain, record)) return Status::failure("domain.not_registered");
  const Status fenced = impl_->domains.fence(domain, reason);
  if (!fenced) return fenced;
  impl_->reservations.invalidate_domain(domain, reason);
  for (const ExecutionAttempt& attempt : impl_->attempts.list_live()) {
    if (attempt.domain != domain) continue;
    const bool may_have_effect =
        attempt.state == AttemptState::kDispatched || attempt.state == AttemptState::kDispatching;
    if (may_have_effect) {
      const Status status = impl_->attempts.mark_ambiguous(attempt.id, reason);
      (void)status;
    } else {
      const Status status = impl_->attempts.mark_fenced(attempt.id, false, reason);
      (void)status;
    }
    impl_->erase_plan(attempt.id);
  }
  if (impl_->options.enable_persistence && impl_->options.persist_on_mutation) {
    const Status saved = save_state();
    (void)saved;
  }
  return Status::success();
}

Status Scheduler::fence_worker_boot(WorkerBootId boot, std::string_view reason) {
  const Status fenced = impl_->workers.fence_boot(boot, reason);
  if (!fenced) return fenced;
  const std::size_t affected = impl_->domains.fence_worker_boot(boot, reason);
  for (const ExecutionDomainRecord& domain : impl_->domains.all()) {
    if (domain.worker_boot == boot) impl_->reservations.invalidate_domain(domain.id, reason);
  }
  for (const ExecutionAttempt& attempt : impl_->attempts.list_live()) {
    if (attempt.worker_boot != boot) continue;
    const bool may_have_effect =
        attempt.state == AttemptState::kDispatched || attempt.state == AttemptState::kDispatching;
    if (may_have_effect) {
      const Status status = impl_->attempts.mark_ambiguous(
          attempt.id, std::string("worker incarnation fenced: ") + std::string(reason));
      (void)status;
    } else {
      const Status status = impl_->attempts.mark_fenced(
          attempt.id, false, std::string("worker incarnation fenced: ") + std::string(reason));
      (void)status;
    }
    impl_->erase_plan(attempt.id);
  }
  log_write(LogLevel::kInfo, "scheduler",
            "fenced worker boot " + format_id(boot.value()) + " affecting " +
                std::to_string(affected) + " domain(s)");
  if (impl_->options.enable_persistence && impl_->options.persist_on_mutation) {
    const Status saved = save_state();
    (void)saved;
  }
  return Status::success();
}

Status Scheduler::withdraw_evidence(ExecutionDomainId domain, std::string_view reason) {
  return impl_->domains.mark_domain_evidence_stale(domain, reason);
}

Status Scheduler::remove_domain(ExecutionDomainId domain) {
  impl_->reservations.invalidate_domain(domain, "domain removed");
  return impl_->domains.remove(domain);
}

// ---------------------------------------------------------------------------
// Inspection
// ---------------------------------------------------------------------------

SchedulerSnapshot Scheduler::snapshot() const {
  SchedulerSnapshot view;
  view.generation =
      SnapshotGeneration(impl_->snapshot_counter.fetch_add(1, std::memory_order_relaxed) + 1);
  view.coordinator_epoch = impl_->epoch;
  view.policy_generation = policy_generation();
  view.running = impl_->running.load();
  view.mutation_sequence = impl_->domains.mutation_sequence();
  view.domains = impl_->domains.all();
  view.policy_summary = impl_->policy_copy();

  const std::vector<std::pair<WorkerId, WorkerBootId>> live = impl_->workers.live_boots();
  const std::vector<WorkerBootId> fenced = impl_->workers.fenced_boots();
  for (const auto& entry : live) {
    WorkerBootView boot;
    boot.worker = entry.first;
    boot.boot = entry.second;
    boot.current = true;
    boot.fenced = false;
    view.workers.push_back(boot);
  }
  for (WorkerBootId boot : fenced) {
    const bool known = std::any_of(live.begin(), live.end(),
                                   [boot](const std::pair<WorkerId, WorkerBootId>& entry) {
                                     return entry.second == boot;
                                   });
    if (known) continue;
    WorkerBootView fenced_view;
    fenced_view.boot = boot;
    fenced_view.current = false;
    fenced_view.fenced = true;
    view.workers.push_back(fenced_view);
  }
  std::sort(view.workers.begin(), view.workers.end(),
            [](const WorkerBootView& a, const WorkerBootView& b) { return a.boot < b.boot; });

  view.reservations = impl_->reservations.list(impl_->limits.max_outstanding_reservations);
  view.attempts =
      impl_->attempts.list(impl_->limits.max_live_attempts + impl_->limits.max_historical_attempts);
  {
    std::lock_guard<std::mutex> lock(impl_->reports_mutex);
    view.reconciliations = impl_->reports;
  }

  view.totals.domains = view.domains.size();
  for (const ExecutionDomainRecord& domain : view.domains) {
    if (domain.fenced) ++view.totals.domains_fenced;
    if (domain.load.published()) ++view.totals.domains_with_current_evidence;
  }
  for (const ExecutionAttempt& attempt : view.attempts) {
    switch (attempt.state) {
      case AttemptState::kCompleted: ++view.totals.completed_attempts; break;
      case AttemptState::kOutcomeUnknown: ++view.totals.ambiguous_attempts; break;
      case AttemptState::kFenced: ++view.totals.fenced_attempts; break;
      default:
        if (!is_terminal_attempt(attempt.state)) ++view.totals.live_attempts;
        break;
    }
  }
  view.totals.outstanding_reservations = impl_->reservations.outstanding_count();
  view.totals.plans_created = impl_->stat_plans_created.load();
  view.totals.plans_dispatched = impl_->stat_plans_dispatched.load();
  view.totals.dispatch_rejections = impl_->stat_dispatch_rejections.load();
  view.totals.completion_rejections = impl_->stat_completion_rejections.load();
  view.totals.fallbacks = impl_->stat_fallbacks.load();
  view.totals.retries = impl_->stat_retries.load();
  return view;
}

ReconciliationReport Scheduler::reconcile() const {
  ReconciliationReport report;
  report.coordinator_epoch = impl_->epoch;
  report.snapshot =
      SnapshotGeneration(impl_->snapshot_counter.fetch_add(1, std::memory_order_relaxed) + 1);

  std::vector<ExecutionDomainRecord> expected;
  {
    std::lock_guard<std::mutex> lock(impl_->recovered_mutex);
    expected = impl_->recovered_domains;
  }
  const std::vector<ExecutionDomainRecord> live = impl_->domains.all();

  std::map<std::uint64_t, const ExecutionDomainRecord*> live_by_id;
  for (const ExecutionDomainRecord& domain : live) live_by_id[domain.id.value()] = &domain;

  for (const ExecutionDomainRecord& durable : expected) {
    ++report.domains_compared;
    const auto found = live_by_id.find(durable.id.value());
    if (found == live_by_id.end()) {
      ReconciliationItem item;
      item.kind = DiscrepancyKind::kDomainMissing;
      item.domain = durable.id;
      item.domain_name = durable.name;
      item.expected_generation = durable.generation;
      item.detail = "durable domain identity is not present in the live runtime";
      report.items.push_back(std::move(item));
      continue;
    }
    const ExecutionDomainRecord& current = *found->second;
    live_by_id.erase(found);
    bool matched = true;
    if (current.generation.value() < durable.generation.value()) {
      ReconciliationItem item;
      item.kind = DiscrepancyKind::kGenerationRegressed;
      item.domain = durable.id;
      item.domain_name = durable.name;
      item.expected_generation = durable.generation;
      item.observed_generation = current.generation;
      item.detail = "live domain generation is below the durable generation";
      report.items.push_back(std::move(item));
      matched = false;
    } else if (current.generation != durable.generation) {
      ReconciliationItem item;
      item.kind = DiscrepancyKind::kDomainGenerationChanged;
      item.domain = durable.id;
      item.domain_name = durable.name;
      item.expected_generation = durable.generation;
      item.observed_generation = current.generation;
      item.detail = "domain generation advanced since the durable record";
      report.items.push_back(std::move(item));
      matched = false;
    }
    if (current.worker_boot != durable.worker_boot) {
      ReconciliationItem item;
      item.kind = DiscrepancyKind::kWorkerReplaced;
      item.domain = durable.id;
      item.domain_name = durable.name;
      item.expected_boot = durable.worker_boot;
      item.observed_boot = current.worker_boot;
      item.detail = "a different worker incarnation owns this domain";
      report.items.push_back(std::move(item));
      matched = false;
    }
    if (current.capability.capability.operations != durable.capability.capability.operations) {
      ReconciliationItem item;
      item.kind = DiscrepancyKind::kOperationNoLongerSupported;
      item.domain = durable.id;
      item.domain_name = durable.name;
      item.detail = "published operation classes changed";
      report.items.push_back(std::move(item));
      matched = false;
    }
    if (current.capability.capability.max_payload_bytes !=
            durable.capability.capability.max_payload_bytes ||
        current.capability.capability.queue_capacity !=
            durable.capability.capability.queue_capacity) {
      ReconciliationItem item;
      item.kind = DiscrepancyKind::kQueueSupportChanged;
      item.domain = durable.id;
      item.domain_name = durable.name;
      item.detail = "payload or queue capacity changed";
      report.items.push_back(std::move(item));
      matched = false;
    }
    if (current.compatibility.driver_backend_version !=
        durable.compatibility.driver_backend_version) {
      ReconciliationItem item;
      item.kind = DiscrepancyKind::kBackendVersionChanged;
      item.domain = durable.id;
      item.domain_name = durable.name;
      item.detail = "driver/backend version changed";
      report.items.push_back(std::move(item));
      matched = false;
    }
    if (current.compatibility.firmware_generation != durable.compatibility.firmware_generation) {
      ReconciliationItem item;
      item.kind = DiscrepancyKind::kFirmwareGenerationChanged;
      item.domain = durable.id;
      item.domain_name = durable.name;
      item.detail = "firmware generation changed";
      report.items.push_back(std::move(item));
      matched = false;
    }
    if (!current.load.published()) {
      ReconciliationItem item;
      item.kind = DiscrepancyKind::kCapabilityChanged;
      item.domain = durable.id;
      item.domain_name = durable.name;
      item.detail = "dynamic evidence is not current: revalidation required";
      report.items.push_back(std::move(item));
      matched = false;
    }
    if (matched) ++report.domains_matched;
  }

  for (const auto& entry : live_by_id) {
    const ExecutionDomainRecord& domain = *entry.second;
    ReconciliationItem item;
    item.kind = DiscrepancyKind::kUnregisteredLiveDomain;
    item.domain = domain.id;
    item.domain_name = domain.name;
    item.observed_generation = domain.generation;
    item.observed_boot = domain.worker_boot;
    item.detail = "live execution domain has no durable counterpart";
    report.items.push_back(std::move(item));
  }

  for (const ExecutionAttempt& attempt : impl_->attempts.list_live()) {
    ++report.attempts_classified;
    ReconciliationItem item;
    item.kind = DiscrepancyKind::kInFlightAttemptAmbiguous;
    item.domain = attempt.domain;
    item.attempt = attempt.id;
    item.detail = "in-flight attempt remains conservative until its executor answers";
    report.items.push_back(std::move(item));
  }

  report.discrepancies = report.items.size();
  report.conservative = true;
  {
    std::lock_guard<std::mutex> lock(impl_->reports_mutex);
    std::vector<ReconciliationReport>& stored = impl_->reports;
    stored.push_back(report);
    if (stored.size() > impl_->limits.max_snapshot_history) {
      stored.erase(stored.begin(),
                   stored.begin() + static_cast<std::ptrdiff_t>(
                                        stored.size() - impl_->limits.max_snapshot_history));
    }
  }
  return report;
}

// ---------------------------------------------------------------------------
// Persistence and recovery
// ---------------------------------------------------------------------------

Status Scheduler::save_state() { return save_state_to(impl_->options.state_path); }

Status Scheduler::save_state_to(std::string_view path) {
  if (path.empty()) return Status::failure("state.no_path");
  PersistedState state;
  state.generation = PersistenceGeneration(
      impl_->persisted_generation.published() ? impl_->persisted_generation.value() + 1 : 1);
  state.last_epoch = impl_->epoch;
  state.policy_generation = policy_generation();
  state.policy = impl_->policy_copy();
  state.has_policy = true;
  state.counters[kCounterOperation] = impl_->operation_counter.load();
  state.counters[kCounterPlanSequence] = impl_->plan_sequence.load();
  state.counters[kCounterDispatch] = impl_->dispatch_counter.load();
  state.counters[kCounterSnapshot] = impl_->snapshot_counter.load();
  state.counters[kCounterDomain] = impl_->domain_counter.load();
  state.domains = impl_->domains.all();
  state.fenced_boots = impl_->workers.fenced_boots();

  for (const ExecutionAttempt& attempt :
       impl_->attempts.list(impl_->limits.max_historical_attempts)) {
    ExecutionAttempt record = attempt;
    if (!is_terminal_attempt(record.state)) {
      // In-flight work is never persisted as authoritative. It is recorded as
      // ambiguous when it may have taken effect, and as fenced otherwise.
      const bool may_have_effect =
          record.state == AttemptState::kDispatched || record.state == AttemptState::kDispatching;
      record.state = may_have_effect ? AttemptState::kOutcomeUnknown : AttemptState::kFenced;
      record.resolution = may_have_effect ? AttemptResolution::kAmbiguous
                                          : AttemptResolution::kFencedNoEffect;
      record.completion_committed = false;
      record.close_reason = "recovered from durable state: in-flight at checkpoint";
      record.failure = may_have_effect ? FailureKind::kAmbiguousOutcome : FailureKind::kWorkerDeath;
    }
    state.attempts.push_back(std::move(record));
  }

  const StateStore store{std::string(path)};
  const Status saved = store.save(state);
  if (saved) impl_->persisted_generation = state.generation;
  return saved;
}

Status Scheduler::load_state() { return load_state_from(impl_->options.state_path); }

Status Scheduler::load_state_from(std::string_view path) {
  if (path.empty()) return Status::failure("state.no_path");
  const StateStore store{std::string(path)};
  if (!store.exists()) return Status::failure("state.missing", std::string(path));
  Checked<PersistedState> loaded = store.load();
  if (!loaded.ok()) return Status::failure(loaded.status.code, loaded.status.message);
  const PersistedState state = loaded.value;

  // Phase one: validate the whole durable state against the live runtime before a
  // single value is applied. A refused load must leave the runtime unchanged.
  for (const ExecutionDomainRecord& domain : state.domains) {
    ExecutionDomainRecord existing;
    if (impl_->domains.get(domain.id, existing) &&
        existing.generation.value() > domain.generation.value()) {
      return Status::failure("state.generation_rollback",
                             "durable domain generation is below the live generation");
    }
  }
  for (const ExecutionAttempt& attempt : state.attempts) {
    ExecutionAttempt existing;
    if (impl_->attempts.get(attempt.id, existing)) {
      return Status::failure("attempt.duplicate_identity",
                             "durable attempt identity already exists in the live ledger");
    }
  }

  // Phase two: apply. Every precondition has already been checked.
  std::vector<ExecutionAttemptId> attempt_ids;
  std::vector<ReservationId> reservation_ids;
  for (const ExecutionAttempt& attempt : state.attempts) {
    const Status restored = impl_->attempts.restore_attempt(attempt);
    if (!restored) {
      return Status::failure("state.apply_failed",
                             std::string("attempt restore rejected after validation: ") +
                                 restored.code);
    }
    attempt_ids.push_back(attempt.id);
    if (attempt.reservation.valid()) reservation_ids.push_back(attempt.reservation);
  }
  impl_->attempts.adopt_ids(attempt_ids);
  impl_->reservations.adopt_ids(reservation_ids);

  for (const ExecutionDomainRecord& domain : state.domains) {
    auto upserted = impl_->domains.upsert(domain, true);
    if (!upserted.ok()) {
      return Status::failure("state.apply_failed",
                             std::string("domain restore rejected after validation: ") +
                                 upserted.status.code);
    }
    if (domain.capacity != CapacityVector{}) {
      const Status capacity = impl_->reservations.set_capacity(domain.id, domain.capacity);
      if (!capacity) return capacity;
    }
  }
  (void)impl_->domains.mark_dynamic_evidence_stale(
      "recovered: dynamic evidence requires revalidation");

  for (WorkerBootId boot : state.fenced_boots) {
    const Status fenced = impl_->workers.fence_boot(boot, "persisted fence");
    (void)fenced;
  }
  for (const ExecutionDomainRecord& domain : state.domains) {
    if (domain.worker_boot.valid()) {
      // A recovered boot identity is never current: the worker must register again.
      const Status fenced = impl_->workers.fence_boot(
          domain.worker_boot, "recovered boot identity requires re-registration");
      (void)fenced;
    }
  }

  impl_->persisted_generation = state.generation;
  impl_->epoch = state.last_epoch;
  impl_->operation_counter.store(
      std::max(impl_->operation_counter.load(), state.counters[kCounterOperation]));
  impl_->plan_sequence.store(
      std::max(impl_->plan_sequence.load(), state.counters[kCounterPlanSequence]));
  impl_->dispatch_counter.store(
      std::max(impl_->dispatch_counter.load(), state.counters[kCounterDispatch]));
  impl_->snapshot_counter.store(
      std::max(impl_->snapshot_counter.load(), state.counters[kCounterSnapshot]));
  impl_->domain_counter.store(std::max(impl_->domain_counter.load(), state.counters[kCounterDomain]));

  if (state.has_policy) {
    std::unique_lock<std::shared_mutex> lock(impl_->policy_mutex);
    if (state.policy.generation >= impl_->policy.generation) {
      impl_->policy = state.policy;
    }
  }

  {
    std::lock_guard<std::mutex> lock(impl_->recovered_mutex);
    impl_->recovered_domains = state.domains;
    impl_->recovered_from_state = true;
  }
  log_write(LogLevel::kInfo, "scheduler",
            "recovered durable state: " + std::to_string(state.domains.size()) + " domain(s), " +
                std::to_string(state.attempts.size()) + " attempt record(s)");
  return Status::success();
}

}  // namespace tos
