// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "tos/core/reservation.hpp"

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace tos {
namespace {

constexpr std::uint64_t kShardShift = 56;
constexpr std::uint64_t kMaxShards = 256;

std::uint64_t shard_prefix(std::size_t shard) { return static_cast<std::uint64_t>(shard + 1); }

}  // namespace

struct ReservationLedger::Impl {
  struct Shard {
    mutable std::mutex mutex;
    std::unordered_map<std::uint64_t, Reservation> reservations;
    std::unordered_map<std::uint64_t, DomainAccounting> accounting;
    std::uint64_t counter{0};
  };

  explicit Impl(std::size_t count) {
    shard_count = std::max<std::size_t>(1, std::min<std::size_t>(count, kMaxShards));
    shards.reserve(shard_count);
    for (std::size_t i = 0; i < shard_count; ++i) shards.push_back(std::make_unique<Shard>());
  }

  [[nodiscard]] std::size_t shard_for_domain(ExecutionDomainId domain) const {
    return static_cast<std::size_t>(StrongIdHash<ExecutionDomainIdTag>{}(domain) % shard_count);
  }
  [[nodiscard]] std::size_t shard_for_id(ReservationId id) const {
    const std::size_t index = static_cast<std::size_t>(id.value() >> kShardShift);
    if (index == 0 || index > shard_count) return shard_count;  // invalid marker
    return index - 1;
  }

  std::size_t shard_count{1};
  std::vector<std::unique_ptr<Shard>> shards;
};

ReservationLedger::ReservationLedger(std::size_t shard_count) : impl_(std::make_unique<Impl>(shard_count)) {}
ReservationLedger::~ReservationLedger() = default;

Status ReservationLedger::set_capacity(ExecutionDomainId domain, const CapacityVector& capacity) {
  const std::size_t index = impl_->shard_for_domain(domain);
  Impl::Shard& shard = *impl_->shards[index];
  std::lock_guard<std::mutex> lock(shard.mutex);
  DomainAccounting& accounting = shard.accounting[domain.value()];
  for (std::size_t k = 0; k < capacity.size(); ++k) {
    if (accounting.reserved[k] + accounting.committed[k] > capacity[k]) {
      return Status::failure("reservation.capacity_below_outstanding",
                             std::string("resource ") + std::string(to_string(static_cast<ResourceKind>(k))));
    }
  }
  accounting.domain = domain;
  accounting.capacity = capacity;
  return Status::success();
}

Checked<ReservationId> ReservationLedger::acquire(const ReservationRequest& request) {
  if (!request.domain.valid()) return Checked<ReservationId>::bad("reservation.invalid_domain");
  const std::size_t index = impl_->shard_for_domain(request.domain);
  Impl::Shard& shard = *impl_->shards[index];
  std::lock_guard<std::mutex> lock(shard.mutex);
  DomainAccounting& accounting = shard.accounting[request.domain.value()];
  accounting.domain = request.domain;

  for (std::size_t k = 0; k < request.amounts.size(); ++k) {
    const std::uint64_t want = request.amounts[k];
    if (want == 0) continue;
    const std::uint64_t held = accounting.reserved[k] + accounting.committed[k];
    const std::uint64_t capacity = accounting.capacity[k];
    if (want > capacity || held > capacity - want) {
      return Checked<ReservationId>::bad(
          "reservation.insufficient_capacity",
          std::string("resource ") + std::string(to_string(static_cast<ResourceKind>(k))) +
              " requested " + std::to_string(want) + " with " + std::to_string(capacity - held) +
              " available");
    }
  }

  Reservation reservation;
  reservation.domain = request.domain;
  reservation.domain_generation = request.domain_generation;
  reservation.capability_generation = request.capability_generation;
  reservation.policy_generation = request.policy_generation;
  reservation.worker_boot = request.worker_boot;
  reservation.operation = request.operation;
  reservation.amounts = request.amounts;
  reservation.state = ReservationState::kActive;
  shard.counter += 1;
  if (shard.counter >= (1ULL << kShardShift)) {
    return Checked<ReservationId>::bad("reservation.id_space_exhausted");
  }
  reservation.id = ReservationId((shard_prefix(index) << kShardShift) | shard.counter);
  reservation.sequence = reservation.id.value();

  for (std::size_t k = 0; k < request.amounts.size(); ++k) accounting.reserved[k] += request.amounts[k];
  accounting.active_count += 1;
  shard.reservations[reservation.id.value()] = reservation;
  return Checked<ReservationId>::good(reservation.id);
}

Status ReservationLedger::commit(ReservationId id, ExecutionDomainGeneration domain_generation,
                                 CapabilityGeneration capability_generation,
                                 PolicyGeneration policy_generation) {
  const std::size_t index = impl_->shard_for_id(id);
  if (index >= impl_->shard_count) return Status::failure("reservation.not_found");
  Impl::Shard& shard = *impl_->shards[index];
  std::lock_guard<std::mutex> lock(shard.mutex);
  const auto found = shard.reservations.find(id.value());
  if (found == shard.reservations.end()) return Status::failure("reservation.not_found");
  Reservation& reservation = found->second;
  switch (reservation.state) {
    case ReservationState::kCommitted:
      return Status::failure("reservation.already_committed");
    case ReservationState::kReleased:
    case ReservationState::kRolledBack:
    case ReservationState::kInvalidated:
      return Status::failure("reservation.already_released");
    case ReservationState::kActive: break;
  }
  if (reservation.domain_generation != domain_generation) {
    return Status::failure("reservation.stale_domain_generation");
  }
  if (reservation.capability_generation != capability_generation) {
    return Status::failure("reservation.stale_capability_generation");
  }
  if (reservation.policy_generation != policy_generation) {
    return Status::failure("reservation.stale_policy_generation");
  }
  DomainAccounting& accounting = shard.accounting[reservation.domain.value()];
  for (std::size_t k = 0; k < reservation.amounts.size(); ++k) {
    accounting.reserved[k] -= reservation.amounts[k];
    accounting.committed[k] += reservation.amounts[k];
  }
  accounting.active_count -= 1;
  accounting.committed_count += 1;
  reservation.state = ReservationState::kCommitted;
  return Status::success();
}

Status ReservationLedger::release(ReservationId id, std::string_view reason) {
  const std::size_t index = impl_->shard_for_id(id);
  if (index >= impl_->shard_count) return Status::failure("reservation.not_found");
  Impl::Shard& shard = *impl_->shards[index];
  std::lock_guard<std::mutex> lock(shard.mutex);
  const auto found = shard.reservations.find(id.value());
  if (found == shard.reservations.end()) return Status::failure("reservation.not_found");
  Reservation& reservation = found->second;
  DomainAccounting& accounting = shard.accounting[reservation.domain.value()];
  switch (reservation.state) {
    case ReservationState::kActive:
      for (std::size_t k = 0; k < reservation.amounts.size(); ++k) {
        accounting.reserved[k] -= reservation.amounts[k];
      }
      accounting.active_count -= 1;
      break;
    case ReservationState::kCommitted:
      for (std::size_t k = 0; k < reservation.amounts.size(); ++k) {
        accounting.committed[k] -= reservation.amounts[k];
      }
      accounting.committed_count -= 1;
      break;
    case ReservationState::kReleased:
      return Status::failure("reservation.already_released");
    case ReservationState::kRolledBack:
      return Status::failure("reservation.already_rolled_back");
    case ReservationState::kInvalidated:
      return Status::failure("reservation.already_invalidated");
  }
  reservation.state = ReservationState::kReleased;
  reservation.close_reason = bounded_text(reason);
  accounting.released_count += 1;
  return Status::success();
}

Status ReservationLedger::rollback(ReservationId id, std::string_view reason) {
  const std::size_t index = impl_->shard_for_id(id);
  if (index >= impl_->shard_count) return Status::failure("reservation.not_found");
  Impl::Shard& shard = *impl_->shards[index];
  std::lock_guard<std::mutex> lock(shard.mutex);
  const auto found = shard.reservations.find(id.value());
  if (found == shard.reservations.end()) return Status::failure("reservation.not_found");
  Reservation& reservation = found->second;
  if (reservation.state != ReservationState::kActive) {
    return Status::failure(reservation.state == ReservationState::kCommitted
                               ? "reservation.already_committed"
                               : "reservation.already_released");
  }
  DomainAccounting& accounting = shard.accounting[reservation.domain.value()];
  for (std::size_t k = 0; k < reservation.amounts.size(); ++k) {
    accounting.reserved[k] -= reservation.amounts[k];
  }
  accounting.active_count -= 1;
  accounting.rolled_back_count += 1;
  reservation.state = ReservationState::kRolledBack;
  reservation.close_reason = bounded_text(reason);
  return Status::success();
}

std::size_t ReservationLedger::invalidate_domain(ExecutionDomainId domain, std::string_view reason) {
  const std::size_t index = impl_->shard_for_domain(domain);
  Impl::Shard& shard = *impl_->shards[index];
  std::lock_guard<std::mutex> lock(shard.mutex);
  std::size_t affected = 0;
  DomainAccounting& accounting = shard.accounting[domain.value()];
  for (auto& entry : shard.reservations) {
    Reservation& reservation = entry.second;
    if (reservation.domain != domain) continue;
    if (reservation.state == ReservationState::kActive) {
      for (std::size_t k = 0; k < reservation.amounts.size(); ++k) {
        accounting.reserved[k] -= reservation.amounts[k];
      }
      accounting.active_count -= 1;
    } else if (reservation.state == ReservationState::kCommitted) {
      for (std::size_t k = 0; k < reservation.amounts.size(); ++k) {
        accounting.committed[k] -= reservation.amounts[k];
      }
      accounting.committed_count -= 1;
    } else {
      continue;
    }
    reservation.state = ReservationState::kInvalidated;
    reservation.close_reason = bounded_text(reason);
    accounting.invalidated_count += 1;
    ++affected;
  }
  return affected;
}

bool ReservationLedger::get(ReservationId id, Reservation& out) const {
  const std::size_t index = impl_->shard_for_id(id);
  if (index >= impl_->shard_count) return false;
  Impl::Shard& shard = *impl_->shards[index];
  std::lock_guard<std::mutex> lock(shard.mutex);
  const auto found = shard.reservations.find(id.value());
  if (found == shard.reservations.end()) return false;
  out = found->second;
  return true;
}

std::vector<Reservation> ReservationLedger::list(std::size_t limit) const {
  std::vector<Reservation> out;
  for (const auto& shard_ptr : impl_->shards) {
    Impl::Shard& shard = *shard_ptr;
    std::lock_guard<std::mutex> lock(shard.mutex);
    for (const auto& entry : shard.reservations) out.push_back(entry.second);
  }
  std::sort(out.begin(), out.end(),
            [](const Reservation& a, const Reservation& b) { return a.id < b.id; });
  if (out.size() > limit) out.resize(limit);
  return out;
}

bool ReservationLedger::has_capacity(ExecutionDomainId domain, const ResourceAmounts& amounts) const {
  const std::size_t index = impl_->shard_for_domain(domain);
  Impl::Shard& shard = *impl_->shards[index];
  std::lock_guard<std::mutex> lock(shard.mutex);
  const auto found = shard.accounting.find(domain.value());
  if (found == shard.accounting.end()) {
    for (std::size_t k = 0; k < amounts.size(); ++k) {
      if (amounts[k] != 0) return false;
    }
    return true;
  }
  const DomainAccounting& accounting = found->second;
  for (std::size_t k = 0; k < amounts.size(); ++k) {
    if (amounts[k] == 0) continue;
    const std::uint64_t held = accounting.reserved[k] + accounting.committed[k];
    if (amounts[k] > accounting.capacity[k] || held > accounting.capacity[k] - amounts[k]) {
      return false;
    }
  }
  return true;
}

std::uint64_t ReservationLedger::outstanding_count() const noexcept {
  std::uint64_t total = 0;
  for (const auto& shard_ptr : impl_->shards) {
    Impl::Shard& shard = *shard_ptr;
    std::lock_guard<std::mutex> lock(shard.mutex);
    for (const auto& entry : shard.reservations) {
      if (entry.second.state == ReservationState::kActive ||
          entry.second.state == ReservationState::kCommitted) {
        ++total;
      }
    }
  }
  return total;
}

ReservationAudit ReservationLedger::audit() const {
  ReservationAudit audit;
  for (const auto& shard_ptr : impl_->shards) {
    Impl::Shard& shard = *shard_ptr;
    std::lock_guard<std::mutex> lock(shard.mutex);
    for (const auto& entry : shard.accounting) {
      const DomainAccounting& accounting = entry.second;
      ResourceAmounts expected_reserved{};
      ResourceAmounts expected_committed{};
      std::uint64_t active = 0;
      std::uint64_t committed = 0;
      for (const auto& reservation_entry : shard.reservations) {
        const Reservation& reservation = reservation_entry.second;
        if (reservation.domain.value() != entry.first) continue;
        if (reservation.state == ReservationState::kActive) {
          for (std::size_t k = 0; k < reservation.amounts.size(); ++k) {
            expected_reserved[k] += reservation.amounts[k];
          }
          ++active;
        } else if (reservation.state == ReservationState::kCommitted) {
          for (std::size_t k = 0; k < reservation.amounts.size(); ++k) {
            expected_committed[k] += reservation.amounts[k];
          }
          ++committed;
        }
      }
      bool row_ok = expected_reserved == accounting.reserved &&
                    expected_committed == accounting.committed && active == accounting.active_count &&
                    committed == accounting.committed_count;
      for (std::size_t k = 0; k < accounting.reserved.size(); ++k) {
        if (accounting.reserved[k] + accounting.committed[k] > accounting.capacity[k]) row_ok = false;
      }
      if (!row_ok) {
        audit.consistent = false;
        audit.detail = "reservation accounting row mismatch";
      }
      DomainAccounting row = accounting;
      audit.domains.push_back(row);
      audit.outstanding += active + committed;
      audit.committed += committed;
      audit.leaked += active;
    }
  }
  std::sort(audit.domains.begin(), audit.domains.end(),
            [](const DomainAccounting& a, const DomainAccounting& b) { return a.domain < b.domain; });
  if (audit.consistent) audit.detail = "reservation accounting consistent";
  return audit;
}

void ReservationLedger::adopt_ids(const std::vector<ReservationId>& ids) {
  for (ReservationId id : ids) {
    const std::size_t index = impl_->shard_for_id(id);
    if (index >= impl_->shard_count) continue;
    Impl::Shard& shard = *impl_->shards[index];
    std::lock_guard<std::mutex> lock(shard.mutex);
    const std::uint64_t counter = id.value() & ((1ULL << kShardShift) - 1ULL);
    if (counter > shard.counter) shard.counter = counter;
  }
}

}  // namespace tos
