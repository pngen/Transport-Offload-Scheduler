// Attempt ledger: the record of what the runtime believes happened.
//
// An attempt exists before any external call is made, so a fast completion can
// never arrive before the runtime knows the attempt exists. At most one
// authoritative completion may commit per attempt generation.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#ifndef TOS_CORE_ATTEMPT_HPP
#define TOS_CORE_ATTEMPT_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "tos/core/enums.hpp"
#include "tos/core/identities.hpp"
#include "tos/core/policy.hpp"

namespace tos {

/// Everything the ledger needs to register an attempt before dispatch.
struct AttemptRegistration {
  ExecutionAttemptId id;
  ExecutionAttemptGeneration generation;
  TransportOperationId operation;
  OperationClassId operation_class;
  SideEffectClass side_effect{SideEffectClass::kUnknown};
  ExecutionDomainId domain;
  ExecutionDomainType domain_type{ExecutionDomainType::kCpu};
  ExecutionDomainGeneration domain_generation;
  CapabilityGeneration capability_generation;
  WorkerId worker;
  WorkerBootId worker_boot;
  CoordinatorEpoch coordinator_epoch;
  ReservationId reservation;
  DispatchId dispatch;
  Provenance provenance{Provenance::kUnsupported};
  bool fallback{false};
  ExecutionDomainId fallback_from;
  std::uint32_t retry_index{0};
};

/// Result payload reported by an executor. Only a bounded digest travels by default.
struct ExecutionResultPayload {
  bool success{false};
  std::uint64_t result_digest{0};
  std::uint64_t bytes_processed{0};
  std::uint64_t duration_ns{0};
  /// Failure taxonomy reported by the executor. It reaches the retry policy, so a
  /// backend rejection and an execution failure are not collapsed into one kind.
  FailureKind failure{FailureKind::kNone};
  std::string backend_detail;
};

/// Completion as submitted to completion authority.
struct CompletionSubmission {
  ExecutionAttemptId attempt;
  ExecutionAttemptGeneration generation;
  DispatchId dispatch;
  CompletionId completion;
  WorkerId worker;
  WorkerBootId worker_boot;
  CoordinatorEpoch coordinator_epoch;
  ExecutionDomainGeneration domain_generation;
  CapabilityGeneration capability_generation;
  TransportOperationId operation;
  Provenance provenance{Provenance::kUnsupported};
  ExecutionResultPayload result;
};

/// A terminal or in-flight attempt record.
struct ExecutionAttempt {
  ExecutionAttemptId id;
  ExecutionAttemptGeneration generation;
  TransportOperationId operation;
  OperationClassId operation_class;
  SideEffectClass side_effect{SideEffectClass::kUnknown};
  ExecutionDomainId domain;
  ExecutionDomainType domain_type{ExecutionDomainType::kCpu};
  ExecutionDomainGeneration domain_generation;
  CapabilityGeneration capability_generation;
  WorkerId worker;
  WorkerBootId worker_boot;
  CoordinatorEpoch coordinator_epoch;
  ReservationId reservation;
  DispatchId dispatch;
  AttemptState state{AttemptState::kPlanned};
  AttemptResolution resolution{AttemptResolution::kNone};
  FailureKind failure{FailureKind::kNone};
  std::string failure_detail;
  std::string close_reason;
  Provenance provenance{Provenance::kUnsupported};
  bool fallback{false};
  ExecutionDomainId fallback_from;
  std::uint32_t retry_index{0};
  bool cancellation_requested{false};
  bool completion_committed{false};
  ExecutionResultPayload result;
  std::uint64_t sequence{0};
  std::uint64_t dispatch_sequence{0};
  SnapshotGeneration snapshot;
};

struct CompletionOutcome {
  CompletionRejection rejection{CompletionRejection::kAccepted};
  bool committed{false};       ///< true when this submission became (or already was) authoritative
  bool idempotent{false};      ///< true when it replayed an identical committed completion
  ExecutionAttempt attempt;
};

struct AttemptAudit {
  bool consistent{true};
  std::uint64_t total{0};
  std::uint64_t live{0};
  std::uint64_t completed{0};
  std::uint64_t failed{0};
  std::uint64_t ambiguous{0};
  std::uint64_t cancelled{0};
  std::uint64_t fenced{0};
  std::uint64_t multiple_completions{0};
  std::string detail;
};

/// Concurrency: sharded by attempt id. Completion commits are single-shard
/// critical sections; no caller holds two shards simultaneously.
class AttemptLedger {
 public:
  explicit AttemptLedger(std::size_t shard_count = 64);
  ~AttemptLedger();
  AttemptLedger(const AttemptLedger&) = delete;
  AttemptLedger& operator=(const AttemptLedger&) = delete;

  /// Allocate an attempt identity. The ledger owns the identity space so that
  /// sharding is an internal detail.
  [[nodiscard]] ExecutionAttemptId allocate_id();
  /// Allocate the next attempt generation for an operation. Generations are
  /// strictly increasing per operation and never regress.
  [[nodiscard]] ExecutionAttemptGeneration allocate_generation(TransportOperationId operation);

  /// Insert a persisted terminal record during recovery. Rejects in-flight states:
  /// a recovered in-flight attempt must be classified conservatively first.
  [[nodiscard]] Status restore_attempt(const ExecutionAttempt& attempt);
  /// Raise the internal id counters above identities that already exist, so that
  /// recovered identities are never reallocated.
  void adopt_ids(const std::vector<ExecutionAttemptId>& ids);

  [[nodiscard]] Status register_attempt(const AttemptRegistration& registration);
  [[nodiscard]] Status mark_dispatched(ExecutionAttemptId id, DispatchId dispatch);
  [[nodiscard]] Status mark_failed(ExecutionAttemptId id, FailureKind kind, std::string_view detail);
  [[nodiscard]] Status mark_ambiguous(ExecutionAttemptId id, std::string_view detail);
  [[nodiscard]] Status mark_cancelled(ExecutionAttemptId id, bool crossed_dispatch_boundary,
                                      std::string_view detail);
  [[nodiscard]] Status mark_fenced(ExecutionAttemptId id, bool may_have_effect,
                                   std::string_view detail);
  [[nodiscard]] Status mark_rejected_stale(ExecutionAttemptId id, std::string_view detail);

  /// Completion authority. Validates every binding, then commits at most once.
  [[nodiscard]] CompletionOutcome commit_completion(const CompletionSubmission& submission,
                                                    bool allow_provenance_mismatch = false);

  [[nodiscard]] bool get(ExecutionAttemptId id, ExecutionAttempt& out) const;
  [[nodiscard]] std::vector<ExecutionAttempt> list(std::size_t limit) const;
  [[nodiscard]] std::vector<ExecutionAttempt> list_by_operation(TransportOperationId operation,
                                                                std::size_t limit) const;
  [[nodiscard]] std::vector<ExecutionAttempt> list_live() const;

  /// Highest attempt generation observed for an operation. Used to prove that
  /// attempt generations never regress.
  [[nodiscard]] ExecutionAttemptGeneration highest_generation(TransportOperationId operation) const;

  [[nodiscard]] std::uint64_t live_count() const noexcept;
  [[nodiscard]] AttemptAudit audit() const;

  /// Drop historical (terminal) records older than the retention bound.
  std::size_t prune_history(std::size_t keep);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tos

#endif  // TOS_CORE_ATTEMPT_HPP
