// Reservation ledger.
//
// Offload engines expose scarce execution capacity. A plan does not consume that
// capacity; a reservation does. Reservations are atomic, generation-bound, and
// must close their accounting exactly once.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#ifndef TOS_CORE_RESERVATION_HPP
#define TOS_CORE_RESERVATION_HPP

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "tos/core/domain.hpp"
#include "tos/core/enums.hpp"
#include "tos/core/identities.hpp"
#include "tos/core/operation.hpp"
#include "tos/util/status.hpp"

namespace tos {

enum class ReservationState : std::uint8_t {
  kActive = 0,     ///< capacity held, not yet bound to a dispatch
  kCommitted = 1,  ///< bound to an authoritative dispatch
  kReleased = 2,   ///< returned to the pool after completion or cancellation
  kRolledBack = 3, ///< never became authoritative
  kInvalidated = 4, ///< domain authority was withdrawn while the reservation was held
};

[[nodiscard]] std::string_view to_string(ReservationState value) noexcept;
[[nodiscard]] bool parse_enum(std::string_view text, ReservationState& out) noexcept;

using ResourceAmounts = std::array<std::uint64_t, kResourceKindCount>;

struct ReservationRequest {
  ExecutionDomainId domain;
  ExecutionDomainGeneration domain_generation;
  CapabilityGeneration capability_generation;
  PolicyGeneration policy_generation;
  WorkerBootId worker_boot;
  TransportOperationId operation;
  ResourceAmounts amounts{};
};

struct Reservation {
  ReservationId id;
  ExecutionDomainId domain;
  ExecutionDomainGeneration domain_generation;
  CapabilityGeneration capability_generation;
  PolicyGeneration policy_generation;
  WorkerBootId worker_boot;
  TransportOperationId operation;
  ResourceAmounts amounts{};
  ReservationState state{ReservationState::kActive};
  std::uint64_t sequence{0};
  std::string close_reason;
};

/// Per-domain accounting row, exposed for inspection and invariant checking.
struct DomainAccounting {
  ExecutionDomainId domain;
  CapacityVector capacity{};
  ResourceAmounts reserved{};
  ResourceAmounts committed{};
  std::uint64_t active_count{0};
  std::uint64_t committed_count{0};
  std::uint64_t released_count{0};
  std::uint64_t rolled_back_count{0};
  std::uint64_t invalidated_count{0};
};

struct ReservationAudit {
  bool consistent{true};
  std::uint64_t outstanding{0};
  std::uint64_t committed{0};
  std::uint64_t leaked{0};  ///< held but not bound to a live attempt
  std::string detail;
  std::vector<DomainAccounting> domains;
};

/// Concurrency: the ledger is sharded by execution domain. Every acquire/release
/// touches exactly one shard, so a single shard mutex is sufficient and no caller
/// ever holds two shards at once.
class ReservationLedger {
 public:
  explicit ReservationLedger(std::size_t shard_count = 64);
  ~ReservationLedger();
  ReservationLedger(const ReservationLedger&) = delete;
  ReservationLedger& operator=(const ReservationLedger&) = delete;

  /// Declare the governable capacity of a domain. Re-publishing with lower totals
  /// than currently reserved fails rather than silently overcommitting.
  [[nodiscard]] Status set_capacity(ExecutionDomainId domain, const CapacityVector& capacity);

  /// Acquire all-or-nothing. On failure nothing is retained.
  [[nodiscard]] Checked<ReservationId> acquire(const ReservationRequest& request);

  /// Bind an active reservation to a dispatch. Rejects stale domain/capability/policy
  /// generations, double commit and unknown reservations.
  [[nodiscard]] Status commit(ReservationId id, ExecutionDomainGeneration domain_generation,
                              CapabilityGeneration capability_generation,
                              PolicyGeneration policy_generation);

  /// Release a committed or active reservation. Double release is rejected.
  [[nodiscard]] Status release(ReservationId id, std::string_view reason);

  /// Roll back a reservation that never became authoritative.
  [[nodiscard]] Status rollback(ReservationId id, std::string_view reason);

  /// Withdraw domain authority: every reservation held on the domain is invalidated
  /// and its capacity returned. Returns the number of reservations affected.
  std::size_t invalidate_domain(ExecutionDomainId domain, std::string_view reason);

  /// Raise the internal id counters above identities that already exist.
  void adopt_ids(const std::vector<ReservationId>& ids);

  [[nodiscard]] bool get(ReservationId id, Reservation& out) const;
  [[nodiscard]] std::vector<Reservation> list(std::size_t limit) const;

  [[nodiscard]] bool has_capacity(ExecutionDomainId domain, const ResourceAmounts& amounts) const;
  [[nodiscard]] std::uint64_t outstanding_count() const noexcept;
  [[nodiscard]] ReservationAudit audit() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tos

#endif  // TOS_CORE_RESERVATION_HPP
