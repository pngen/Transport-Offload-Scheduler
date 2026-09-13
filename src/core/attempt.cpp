// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "tos/core/attempt.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace tos {
namespace {

constexpr std::uint64_t kShardShift = 56;
constexpr std::uint64_t kMaxShards = 256;

bool is_terminal(AttemptState state) noexcept {
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

struct AttemptLedger::Impl {
  struct Shard {
    mutable std::mutex mutex;
    std::unordered_map<std::uint64_t, ExecutionAttempt> attempts;
    std::uint64_t counter{0};
  };
  struct OperationShard {
    mutable std::mutex mutex;
    std::unordered_map<std::uint64_t, std::uint64_t> highest_generation;
  };

  explicit Impl(std::size_t count) {
    shard_count = std::max<std::size_t>(1, std::min<std::size_t>(count, kMaxShards));
    shards.reserve(shard_count);
    for (std::size_t i = 0; i < shard_count; ++i) shards.push_back(std::make_unique<Shard>());
    operation_shards.reserve(shard_count);
    for (std::size_t i = 0; i < shard_count; ++i) {
      operation_shards.push_back(std::make_unique<OperationShard>());
    }
  }

  [[nodiscard]] std::size_t shard_for_id(ExecutionAttemptId id) const {
    const std::size_t index = static_cast<std::size_t>(id.value() >> kShardShift);
    if (index == 0 || index > shard_count) return shard_count;
    return index - 1;
  }
  [[nodiscard]] std::size_t operation_shard(TransportOperationId operation) const {
    return static_cast<std::size_t>(StrongIdHash<TransportOperationIdTag>{}(operation) %
                                    shard_count);
  }

  std::size_t shard_count{1};
  std::vector<std::unique_ptr<Shard>> shards;
  std::vector<std::unique_ptr<OperationShard>> operation_shards;
  std::atomic<std::uint64_t> allocation_counter{0};
  std::atomic<std::uint64_t> conflicting_duplicates{0};
  std::atomic<std::uint64_t> duplicate_replays{0};
  std::size_t max_attempts{kMaxLiveAttempts + kMaxHistoricalAttempts};
  std::atomic<std::uint64_t> total_registered{0};

  /// Shared transition helper for the terminal marking operations.
  Status transition(ExecutionAttemptId id, AttemptState state, AttemptResolution resolution,
                    FailureKind failure, std::string_view detail, bool require_live) {
    const std::size_t index = shard_for_id(id);
    if (index >= shard_count) return Status::failure("attempt.not_found");
    Shard& shard = *shards[index];
    std::lock_guard<std::mutex> lock(shard.mutex);
    const auto found = shard.attempts.find(id.value());
    if (found == shard.attempts.end()) return Status::failure("attempt.not_found");
    ExecutionAttempt& attempt = found->second;
    if (attempt.completion_committed) return Status::failure("attempt.already_completed");
    if (require_live && is_terminal(attempt.state)) {
      return Status::failure("attempt.already_terminal");
    }
    attempt.state = state;
    attempt.resolution = resolution;
    if (failure != FailureKind::kNone) attempt.failure = failure;
    attempt.close_reason = bounded_text(detail);
    return Status::success();
  }
};

AttemptLedger::AttemptLedger(std::size_t shard_count) : impl_(std::make_unique<Impl>(shard_count)) {}
AttemptLedger::~AttemptLedger() = default;

ExecutionAttemptId AttemptLedger::allocate_id() {
  const std::uint64_t sequence = impl_->allocation_counter.fetch_add(1, std::memory_order_relaxed);
  const std::size_t index = static_cast<std::size_t>(sequence % impl_->shard_count);
  Impl::Shard& shard = *impl_->shards[index];
  std::lock_guard<std::mutex> lock(shard.mutex);
  shard.counter += 1;
  const std::uint64_t counter = shard.counter & ((1ULL << kShardShift) - 1ULL);
  return ExecutionAttemptId(((static_cast<std::uint64_t>(index) + 1) << kShardShift) | counter);
}

ExecutionAttemptGeneration AttemptLedger::allocate_generation(TransportOperationId operation) {
  const std::size_t index = impl_->operation_shard(operation);
  Impl::OperationShard& shard = *impl_->operation_shards[index];
  std::lock_guard<std::mutex> lock(shard.mutex);
  std::uint64_t& highest = shard.highest_generation[operation.value()];
  highest += 1;  // a pruned map entry restarts at 1 only for an operation that never existed
  return ExecutionAttemptGeneration(highest);
}

Status AttemptLedger::register_attempt(const AttemptRegistration& registration) {
  if (!registration.id.valid()) return Status::failure("attempt.missing_identity");
  if (!registration.operation.valid()) return Status::failure("attempt.missing_operation");
  if (registration.generation.value() <
      highest_generation(registration.operation).value()) {
    return Status::failure("attempt.generation_regression");
  }
  const std::size_t index = impl_->shard_for_id(registration.id);
  if (index >= impl_->shard_count) return Status::failure("attempt.invalid_identity");
  Impl::Shard& shard = *impl_->shards[index];
  std::lock_guard<std::mutex> lock(shard.mutex);
  if (shard.attempts.count(registration.id.value()) != 0U) {
    return Status::failure("attempt.duplicate_identity");
  }
  if (shard.attempts.size() >= impl_->max_attempts) {
    return Status::failure("attempt.limit_reached");
  }
  ExecutionAttempt attempt;
  attempt.id = registration.id;
  attempt.generation = registration.generation;
  attempt.operation = registration.operation;
  attempt.operation_class = registration.operation_class;
  attempt.side_effect = registration.side_effect;
  attempt.domain = registration.domain;
  attempt.domain_type = registration.domain_type;
  attempt.domain_generation = registration.domain_generation;
  attempt.capability_generation = registration.capability_generation;
  attempt.worker = registration.worker;
  attempt.worker_boot = registration.worker_boot;
  attempt.coordinator_epoch = registration.coordinator_epoch;
  attempt.reservation = registration.reservation;
  attempt.dispatch = registration.dispatch;
  attempt.provenance = registration.provenance;
  attempt.fallback = registration.fallback;
  attempt.fallback_from = registration.fallback_from;
  attempt.retry_index = registration.retry_index;
  attempt.state = AttemptState::kDispatching;  // registered before any external call
  attempt.sequence = registration.id.value();
  shard.attempts[attempt.id.value()] = attempt;
  impl_->total_registered.fetch_add(1, std::memory_order_relaxed);
  return Status::success();
}

Status AttemptLedger::mark_dispatched(ExecutionAttemptId id, DispatchId dispatch) {
  const std::size_t index = impl_->shard_for_id(id);
  if (index >= impl_->shard_count) return Status::failure("attempt.not_found");
  Impl::Shard& shard = *impl_->shards[index];
  std::lock_guard<std::mutex> lock(shard.mutex);
  const auto found = shard.attempts.find(id.value());
  if (found == shard.attempts.end()) return Status::failure("attempt.not_found");
  ExecutionAttempt& attempt = found->second;
  if (attempt.state != AttemptState::kDispatching) {
    return Status::failure("attempt.not_dispatching");
  }
  attempt.dispatch = dispatch;
  attempt.state = AttemptState::kDispatched;
  attempt.dispatch_sequence = dispatch.value();
  return Status::success();
}

Status AttemptLedger::mark_failed(ExecutionAttemptId id, FailureKind kind, std::string_view detail) {
  return impl_->transition(id, AttemptState::kFailed, AttemptResolution::kAuthoritativeFailure, kind,
                           detail, true);
}

Status AttemptLedger::mark_ambiguous(ExecutionAttemptId id, std::string_view detail) {
  return impl_->transition(id, AttemptState::kOutcomeUnknown, AttemptResolution::kAmbiguous,
                           FailureKind::kAmbiguousOutcome, detail, true);
}

Status AttemptLedger::mark_cancelled(ExecutionAttemptId id, bool crossed_dispatch_boundary,
                                     std::string_view detail) {
  const AttemptResolution resolution = crossed_dispatch_boundary
                                           ? AttemptResolution::kCancelledAmbiguous
                                           : AttemptResolution::kCancelledBeforeDispatch;
  return impl_->transition(id, AttemptState::kCancelled, resolution, FailureKind::kCancelled, detail,
                           true);
}

Status AttemptLedger::mark_fenced(ExecutionAttemptId id, bool may_have_effect,
                                  std::string_view detail) {
  const AttemptResolution resolution =
      may_have_effect ? AttemptResolution::kFencedMayHaveEffect : AttemptResolution::kFencedNoEffect;
  return impl_->transition(id, AttemptState::kFenced, resolution,
                           may_have_effect ? FailureKind::kAmbiguousOutcome : FailureKind::kWorkerDeath,
                           detail, true);
}

Status AttemptLedger::mark_rejected_stale(ExecutionAttemptId id, std::string_view detail) {
  return impl_->transition(id, AttemptState::kRejectedStale, AttemptResolution::kNone,
                           FailureKind::kPreDispatchRejection, detail, false);
}

CompletionOutcome AttemptLedger::commit_completion(const CompletionSubmission& submission,
                                                  bool allow_provenance_mismatch) {
  CompletionOutcome outcome;
  const std::size_t index = impl_->shard_for_id(submission.attempt);
  if (index >= impl_->shard_count) {
    outcome.rejection = CompletionRejection::kUnknownAttempt;
    return outcome;
  }
  Impl::Shard& shard = *impl_->shards[index];
  std::lock_guard<std::mutex> lock(shard.mutex);
  const auto found = shard.attempts.find(submission.attempt.value());
  if (found == shard.attempts.end()) {
    outcome.rejection = CompletionRejection::kUnknownAttempt;
    return outcome;
  }
  ExecutionAttempt& attempt = found->second;
  outcome.attempt = attempt;

  if (submission.generation != attempt.generation) {
    outcome.rejection = CompletionRejection::kAttemptGenerationMismatch;
    return outcome;
  }
  if (attempt.dispatch.valid() && submission.dispatch != attempt.dispatch) {
    outcome.rejection = CompletionRejection::kDispatchIdMismatch;
    return outcome;
  }
  if (submission.worker.valid() && attempt.worker.valid() && submission.worker != attempt.worker) {
    outcome.rejection = CompletionRejection::kStaleWorkerBoot;
    return outcome;
  }
  if (submission.worker_boot != attempt.worker_boot) {
    outcome.rejection = CompletionRejection::kStaleWorkerBoot;
    return outcome;
  }
  if (submission.coordinator_epoch != attempt.coordinator_epoch) {
    outcome.rejection = CompletionRejection::kStaleCoordinatorEpoch;
    return outcome;
  }
  if (submission.domain_generation != attempt.domain_generation) {
    outcome.rejection = CompletionRejection::kStaleDomainGeneration;
    return outcome;
  }
  if (submission.capability_generation.published() &&
      attempt.capability_generation.published() &&
      submission.capability_generation != attempt.capability_generation) {
    outcome.rejection = CompletionRejection::kStaleCapabilityGeneration;
    return outcome;
  }
  if (submission.operation != attempt.operation) {
    outcome.rejection = CompletionRejection::kOperationMismatch;
    return outcome;
  }
  if (!allow_provenance_mismatch && submission.provenance != attempt.provenance) {
    outcome.rejection = CompletionRejection::kProvenanceMismatch;
    return outcome;
  }
  if (attempt.completion_committed) {
    const bool identical = attempt.result.success == submission.result.success &&
                           attempt.result.result_digest == submission.result.result_digest &&
                           attempt.result.bytes_processed == submission.result.bytes_processed;
    if (identical) {
      outcome.rejection = CompletionRejection::kDuplicateIdentical;
      outcome.committed = true;
      outcome.idempotent = true;
      impl_->duplicate_replays.fetch_add(1, std::memory_order_relaxed);
      return outcome;
    }
    outcome.rejection = CompletionRejection::kConflictingDuplicate;
    impl_->conflicting_duplicates.fetch_add(1, std::memory_order_relaxed);
    return outcome;
  }
  if (attempt.state == AttemptState::kFenced || attempt.state == AttemptState::kRejectedStale) {
    outcome.rejection = attempt.state == AttemptState::kFenced ? CompletionRejection::kDomainFenced
                                                               : CompletionRejection::kAlreadyTerminal;
    return outcome;
  }
  if (attempt.state == AttemptState::kCancelled) {
    outcome.rejection = CompletionRejection::kAlreadyTerminal;
    return outcome;
  }
  if (attempt.state == AttemptState::kCompleted || attempt.state == AttemptState::kFailed ||
      attempt.state == AttemptState::kOutcomeUnknown) {
    outcome.rejection = CompletionRejection::kAlreadyTerminal;
    return outcome;
  }
  if (attempt.state != AttemptState::kDispatched && attempt.state != AttemptState::kDispatching) {
    outcome.rejection = CompletionRejection::kNotDispatched;
    return outcome;
  }
  if (submission.result.bytes_processed > kMaxPayloadBytes) {
    outcome.rejection = CompletionRejection::kResultTooLarge;
    return outcome;
  }

  attempt.state = submission.result.success ? AttemptState::kCompleted : AttemptState::kFailed;
  attempt.resolution = submission.result.success ? AttemptResolution::kAuthoritativeSuccess
                                                 : AttemptResolution::kAuthoritativeFailure;
  attempt.result = submission.result;
  attempt.completion_committed = true;
  // The executor's taxonomy is preserved: retry policy distinguishes a backend
  // rejection from an execution failure, so collapsing them would deny legal retries.
  attempt.failure = submission.result.success
                        ? FailureKind::kNone
                        : (submission.result.failure == FailureKind::kNone
                               ? FailureKind::kExecutionFailure
                               : submission.result.failure);
  attempt.close_reason = bounded_text(submission.result.backend_detail);
  outcome.rejection = CompletionRejection::kAccepted;
  outcome.committed = true;
  outcome.attempt = attempt;
  return outcome;
}

bool AttemptLedger::get(ExecutionAttemptId id, ExecutionAttempt& out) const {
  const std::size_t index = impl_->shard_for_id(id);
  if (index >= impl_->shard_count) return false;
  Impl::Shard& shard = *impl_->shards[index];
  std::lock_guard<std::mutex> lock(shard.mutex);
  const auto found = shard.attempts.find(id.value());
  if (found == shard.attempts.end()) return false;
  out = found->second;
  return true;
}

std::vector<ExecutionAttempt> AttemptLedger::list(std::size_t limit) const {
  std::vector<ExecutionAttempt> out;
  for (const auto& shard_ptr : impl_->shards) {
    Impl::Shard& shard = *shard_ptr;
    std::lock_guard<std::mutex> lock(shard.mutex);
    for (const auto& entry : shard.attempts) out.push_back(entry.second);
  }
  std::sort(out.begin(), out.end(),
            [](const ExecutionAttempt& a, const ExecutionAttempt& b) { return a.id < b.id; });
  if (out.size() > limit) out.resize(limit);
  return out;
}

std::vector<ExecutionAttempt> AttemptLedger::list_by_operation(TransportOperationId operation,
                                                               std::size_t limit) const {
  std::vector<ExecutionAttempt> out;
  for (const auto& shard_ptr : impl_->shards) {
    Impl::Shard& shard = *shard_ptr;
    std::lock_guard<std::mutex> lock(shard.mutex);
    for (const auto& entry : shard.attempts) {
      if (entry.second.operation == operation) out.push_back(entry.second);
    }
  }
  std::sort(out.begin(), out.end(), [](const ExecutionAttempt& a, const ExecutionAttempt& b) {
    if (a.generation != b.generation) return a.generation < b.generation;
    return a.id < b.id;
  });
  if (out.size() > limit) out.resize(limit);
  return out;
}

std::vector<ExecutionAttempt> AttemptLedger::list_live() const {
  std::vector<ExecutionAttempt> out;
  for (const auto& shard_ptr : impl_->shards) {
    Impl::Shard& shard = *shard_ptr;
    std::lock_guard<std::mutex> lock(shard.mutex);
    for (const auto& entry : shard.attempts) {
      if (!is_terminal(entry.second.state)) out.push_back(entry.second);
    }
  }
  std::sort(out.begin(), out.end(),
            [](const ExecutionAttempt& a, const ExecutionAttempt& b) { return a.id < b.id; });
  return out;
}

ExecutionAttemptGeneration AttemptLedger::highest_generation(TransportOperationId operation) const {
  const std::size_t index = impl_->operation_shard(operation);
  Impl::OperationShard& shard = *impl_->operation_shards[index];
  std::lock_guard<std::mutex> lock(shard.mutex);
  const auto found = shard.highest_generation.find(operation.value());
  if (found == shard.highest_generation.end()) return ExecutionAttemptGeneration{};
  return ExecutionAttemptGeneration(found->second);
}

std::uint64_t AttemptLedger::live_count() const noexcept {
  std::uint64_t total = 0;
  for (const auto& shard_ptr : impl_->shards) {
    Impl::Shard& shard = *shard_ptr;
    std::lock_guard<std::mutex> lock(shard.mutex);
    for (const auto& entry : shard.attempts) {
      if (!is_terminal(entry.second.state)) ++total;
    }
  }
  return total;
}

AttemptAudit AttemptLedger::audit() const {
  AttemptAudit audit;
  for (const auto& shard_ptr : impl_->shards) {
    Impl::Shard& shard = *shard_ptr;
    std::lock_guard<std::mutex> lock(shard.mutex);
    for (const auto& entry : shard.attempts) {
      const ExecutionAttempt& attempt = entry.second;
      ++audit.total;
      switch (attempt.state) {
        case AttemptState::kCompleted: ++audit.completed; break;
        case AttemptState::kFailed: ++audit.failed; break;
        case AttemptState::kOutcomeUnknown: ++audit.ambiguous; break;
        case AttemptState::kCancelled: ++audit.cancelled; break;
        case AttemptState::kFenced: ++audit.fenced; break;
        case AttemptState::kRejectedStale: break;
        default: ++audit.live; break;
      }
      if (attempt.completion_committed &&
          (attempt.state == AttemptState::kCompleted || attempt.state == AttemptState::kFailed)) {
        if ((attempt.state == AttemptState::kCompleted) != attempt.result.success) {
          audit.consistent = false;
          audit.detail = "attempt result contradicts terminal state";
        }
      }
      if (attempt.completion_committed && attempt.resolution == AttemptResolution::kNone) {
        audit.consistent = false;
        audit.detail = "committed completion without a resolution";
      }
    }
  }
  audit.multiple_completions = impl_->conflicting_duplicates.load(std::memory_order_relaxed);
  if (audit.consistent) audit.detail = "attempt ledger consistent";
  return audit;
}

std::size_t AttemptLedger::prune_history(std::size_t keep) {
  std::vector<ExecutionAttempt> terminal;
  for (const auto& shard_ptr : impl_->shards) {
    Impl::Shard& shard = *shard_ptr;
    std::lock_guard<std::mutex> lock(shard.mutex);
    for (const auto& entry : shard.attempts) {
      if (is_terminal(entry.second.state)) terminal.push_back(entry.second);
    }
  }
  if (terminal.size() <= keep) return 0;
  std::sort(terminal.begin(), terminal.end(),
            [](const ExecutionAttempt& a, const ExecutionAttempt& b) { return a.id < b.id; });
  const std::size_t excess = terminal.size() - keep;
  std::size_t removed = 0;
  for (std::size_t i = 0; i < excess; ++i) {
    const std::size_t index = impl_->shard_for_id(terminal[i].id);
    if (index >= impl_->shard_count) continue;
    Impl::Shard& shard = *impl_->shards[index];
    std::lock_guard<std::mutex> lock(shard.mutex);
    removed += shard.attempts.erase(terminal[i].id.value());
  }
  return removed;
}

Status AttemptLedger::restore_attempt(const ExecutionAttempt& attempt) {
  if (!attempt.id.valid() || !attempt.operation.valid()) {
    return Status::failure("attempt.invalid_identity");
  }
  switch (attempt.state) {
    case AttemptState::kCompleted:
    case AttemptState::kFailed:
    case AttemptState::kOutcomeUnknown:
    case AttemptState::kCancelled:
    case AttemptState::kRejectedStale:
    case AttemptState::kFenced: break;
    default:
      return Status::failure("attempt.in_flight_restore_rejected",
                             "in-flight attempts are classified conservatively before restore");
  }
  const std::size_t index = impl_->shard_for_id(attempt.id);
  if (index >= impl_->shard_count) return Status::failure("attempt.invalid_identity");
  Impl::Shard& shard = *impl_->shards[index];
  {
    std::lock_guard<std::mutex> lock(shard.mutex);
    if (shard.attempts.count(attempt.id.value()) != 0U) {
      return Status::failure("attempt.duplicate_identity");
    }
    if (shard.attempts.size() >= impl_->max_attempts) {
      return Status::failure("attempt.limit_reached");
    }
    shard.attempts[attempt.id.value()] = attempt;
    const std::uint64_t counter = attempt.id.value() & ((1ULL << kShardShift) - 1ULL);
    if (counter > shard.counter) shard.counter = counter;
  }
  const std::size_t op_index = impl_->operation_shard(attempt.operation);
  Impl::OperationShard& op_shard = *impl_->operation_shards[op_index];
  std::lock_guard<std::mutex> op_lock(op_shard.mutex);
  std::uint64_t& highest = op_shard.highest_generation[attempt.operation.value()];
  if (attempt.generation.value() > highest) highest = attempt.generation.value();
  return Status::success();
}

void AttemptLedger::adopt_ids(const std::vector<ExecutionAttemptId>& ids) {
  for (ExecutionAttemptId id : ids) {
    const std::size_t index = impl_->shard_for_id(id);
    if (index >= impl_->shard_count) continue;
    Impl::Shard& shard = *impl_->shards[index];
    std::lock_guard<std::mutex> lock(shard.mutex);
    const std::uint64_t counter = id.value() & ((1ULL << kShardShift) - 1ULL);
    if (counter > shard.counter) shard.counter = counter;
  }
}

}  // namespace tos
