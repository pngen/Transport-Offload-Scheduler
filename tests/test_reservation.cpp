// Reservation ledger: atomic, generation-bound capacity accounting that must close
// exactly once per operation.
//
// Every test builds its own runtime: no test depends on state left by another, and a
// wait that never satisfies its predicate hangs rather than hiding a defect.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "tos_test_support.hpp"

using namespace tos;

namespace {

constexpr std::size_t slot_index = static_cast<std::size_t>(ResourceKind::kExecutionSlot);
constexpr std::size_t queue_index = static_cast<std::size_t>(ResourceKind::kQueueDepth);
constexpr std::size_t ring_index = static_cast<std::size_t>(ResourceKind::kDescriptorRing);

ResourceAmounts amounts_of(std::initializer_list<std::pair<ResourceKind, std::uint64_t>> values) {
  ResourceAmounts amounts{};
  for (const auto& value : values) amounts[static_cast<std::size_t>(value.first)] = value.second;
  return amounts;
}

CapacityVector capacity_of(std::initializer_list<std::pair<ResourceKind, std::uint64_t>> values) {
  CapacityVector capacity{};
  for (const auto& value : values) capacity[static_cast<std::size_t>(value.first)] = value.second;
  return capacity;
}

ReservationRequest reservation_request(
    ExecutionDomainId domain, ResourceAmounts amounts,
    ExecutionDomainGeneration domain_generation = ExecutionDomainGeneration(3),
    CapabilityGeneration capability_generation = CapabilityGeneration(5),
    PolicyGeneration policy_generation = PolicyGeneration(2)) {
  ReservationRequest request;
  request.domain = domain;
  request.domain_generation = domain_generation;
  request.capability_generation = capability_generation;
  request.policy_generation = policy_generation;
  request.worker_boot = WorkerBootId(7);
  request.operation = TransportOperationId(9);
  request.amounts = amounts;
  return request;
}

const DomainAccounting* accounting_for(const ReservationAudit& audit, ExecutionDomainId domain) {
  for (const DomainAccounting& row : audit.domains) {
    if (row.domain == domain) return &row;
  }
  return nullptr;
}

void expect_consistent(const ReservationAudit& audit, const char* what) {
  TOS_CHECK_MSG(audit.consistent, std::string(what) + ": " + audit.detail);
}

void expect_code(const Status& status, const std::string& expected, const char* what) {
  TOS_CHECK_MSG(!status.ok && status.code == expected,
                std::string(what) + ": expected '" + expected + "', got ok=" +
                    (status.ok ? std::string("true") : std::string("false")) + " code='" +
                    status.code + "'");
}

void expect_no_capacity_retained(const DomainAccounting& before, const DomainAccounting& after,
                                 const char* what) {
  TOS_CHECK_MSG(after.reserved == before.reserved, std::string(what) + ": reserved changed");
  TOS_CHECK_MSG(after.committed == before.committed, std::string(what) + ": committed changed");
  TOS_CHECK_MSG(after.capacity == before.capacity, std::string(what) + ": capacity changed");
  TOS_CHECK_MSG(after.active_count == before.active_count, std::string(what) + ": active count changed");
  TOS_CHECK_MSG(after.committed_count == before.committed_count,
                std::string(what) + ": committed count changed");
  TOS_CHECK_MSG(after.released_count == before.released_count,
                std::string(what) + ": released count changed");
  TOS_CHECK_MSG(after.rolled_back_count == before.rolled_back_count,
                std::string(what) + ": rolled back count changed");
  TOS_CHECK_MSG(after.invalidated_count == before.invalidated_count,
                std::string(what) + ": invalidated count changed");
}

/// Policy that requires one execution slot per operation, so the scheduler and the
/// ledger share one capacity pool.
SchedulerPolicy reservation_policy() {
  SchedulerPolicy policy = make_default_policy();
  policy.reservation.enabled = true;
  policy.reservation.require_reservation_for_offload_only = true;
  policy.reservation.per_operation[slot_index] = 1;
  return policy;
}

}  // namespace

// 1. Full lifecycle: plan -> reserve -> revalidate/dispatch -> complete -> release.
TOS_TEST(reservation_lifecycle_closes_accounting_exactly) {
  tos_test::TestRuntime::Config config;
  config.policy = reservation_policy();
  tos_test::TestRuntime runtime(config);

  SyntheticDomainConfig domain = make_synthetic_domain(ExecutionDomainId(901),
                                                       ExecutionDomainType::kDpu, "dpu.lifecycle");
  domain.capacity = CapacityVector{};
  domain.capacity[slot_index] = 1;
  const ExecutionDomainId domain_id = runtime.add_domain(domain);

  ReservationLedger& ledger = runtime.scheduler().reservations();
  TOS_CHECK_EQ(ledger.outstanding_count(), std::uint64_t{0});

  const std::vector<std::uint8_t> payload = tos_test::make_payload(1024, 5);
  OperationRequest request = tos_test::make_request(opclass::checksum_crc32c(), payload.size());
  const std::span<const std::uint8_t> bytes(payload.data(), payload.size());

  PlanResult planned = runtime.scheduler().plan(request, bytes);
  TOS_REQUIRE(planned.planned);
  TOS_CHECK_EQ(planned.explanation.selected_domain, domain_id);

  ExecutionPlan plan = planned.plan;
  TOS_REQUIRE(runtime.scheduler().reserve(plan).ok);
  TOS_REQUIRE(plan.reservation.valid());
  {
    Reservation record;
    TOS_REQUIRE(ledger.get(plan.reservation, record));
    TOS_CHECK_EQ(record.state, ReservationState::kActive);
    TOS_CHECK_EQ(record.domain, domain_id);
    TOS_CHECK_EQ(record.amounts[slot_index], std::uint64_t{1});
  }
  TOS_CHECK_EQ(ledger.outstanding_count(), std::uint64_t{1});

  const DispatchResult dispatched = runtime.scheduler().dispatch(plan);
  TOS_REQUIRE(dispatched.dispatched);
  {
    Reservation record;
    TOS_REQUIRE(ledger.get(plan.reservation, record));
    TOS_CHECK_EQ(record.state, ReservationState::kCommitted);
  }

  // The completion commits and releases the reservation on the same authority path.
  runtime.wait_for(plan.attempt);
  const ExecutionAttempt settled = runtime.wait_for_terminal(plan.attempt);
  TOS_CHECK_EQ(settled.state, AttemptState::kCompleted);
  TOS_CHECK(settled.completion_committed);
  TOS_CHECK_EQ(settled.reservation, plan.reservation);

  Reservation record;
  TOS_REQUIRE(ledger.get(plan.reservation, record));
  TOS_CHECK_EQ(record.state, ReservationState::kReleased);
  TOS_CHECK(!record.close_reason.empty());
  TOS_CHECK_EQ(ledger.outstanding_count(), std::uint64_t{0});

  const ReservationAudit audit = ledger.audit();
  TOS_CHECK_EQ(audit.outstanding, std::uint64_t{0});
  TOS_CHECK_EQ(audit.committed, std::uint64_t{0});
  TOS_CHECK_EQ(audit.leaked, std::uint64_t{0});
  expect_consistent(audit, "lifecycle audit");

  const DomainAccounting* row = accounting_for(audit, domain_id);
  TOS_REQUIRE(row != nullptr);
  TOS_CHECK_EQ(row->reserved[slot_index], std::uint64_t{0});
  TOS_CHECK_EQ(row->committed[slot_index], std::uint64_t{0});
  TOS_CHECK_EQ(row->active_count, std::uint64_t{0});
  TOS_CHECK_EQ(row->committed_count, std::uint64_t{0});
  TOS_CHECK_EQ(row->released_count, std::uint64_t{1});
  TOS_CHECK_EQ(row->invalidated_count, std::uint64_t{0});
  TOS_CHECK_EQ(row->rolled_back_count, std::uint64_t{0});
  TOS_CHECK_EQ(row->capacity[slot_index], std::uint64_t{1});
}

// 2. Acquisition is all-or-nothing: one exhausted kind retains nothing at all.
TOS_TEST(reservation_acquisition_is_all_or_nothing) {
  tos_test::TestRuntime runtime;
  ReservationLedger& ledger = runtime.scheduler().reservations();
  const ExecutionDomainId domain(902);
  TOS_REQUIRE(ledger.set_capacity(domain, capacity_of({{ResourceKind::kExecutionSlot, 2},
                                                       {ResourceKind::kQueueDepth, 2},
                                                       {ResourceKind::kDescriptorRing, 2}}))
                  .ok);

  auto first = ledger.acquire(reservation_request(
      domain, amounts_of({{ResourceKind::kExecutionSlot, 1},
                          {ResourceKind::kQueueDepth, 1},
                          {ResourceKind::kDescriptorRing, 1}})));
  TOS_REQUIRE(first.ok());

  const ReservationAudit before = ledger.audit();
  const DomainAccounting* before_row = accounting_for(before, domain);
  TOS_REQUIRE(before_row != nullptr);
  const DomainAccounting snapshot = *before_row;
  TOS_CHECK_EQ(snapshot.reserved[slot_index], std::uint64_t{1});
  TOS_CHECK_EQ(snapshot.reserved[queue_index], std::uint64_t{1});
  TOS_CHECK_EQ(snapshot.reserved[ring_index], std::uint64_t{1});

  // One more slot than remains, with other kinds still available: the whole request fails.
  auto denied = ledger.acquire(reservation_request(
      domain, amounts_of({{ResourceKind::kExecutionSlot, 2},
                          {ResourceKind::kQueueDepth, 1},
                          {ResourceKind::kDescriptorRing, 1}})));
  TOS_CHECK(!denied.ok());
  expect_code(denied.status, "reservation.insufficient_capacity", "over-requested slot");

  const ReservationAudit after = ledger.audit();
  const DomainAccounting* after_row = accounting_for(after, domain);
  TOS_REQUIRE(after_row != nullptr);
  expect_no_capacity_retained(snapshot, *after_row, "denied acquisition");
  TOS_CHECK_EQ(ledger.outstanding_count(), std::uint64_t{1});
  TOS_CHECK_EQ(ledger.list(64).size(), std::size_t{1});
  expect_consistent(after, "atomic acquisition audit");
}

// 3. Capacity is accounted per domain and per resource kind.
TOS_TEST(reservation_capacity_is_per_domain_and_per_kind) {
  tos_test::TestRuntime runtime;
  ReservationLedger& ledger = runtime.scheduler().reservations();
  const ExecutionDomainId left(903);
  const ExecutionDomainId right(904);
  const CapacityVector capacity = capacity_of({{ResourceKind::kExecutionSlot, 1},
                                               {ResourceKind::kQueueDepth, 1}});
  TOS_REQUIRE(ledger.set_capacity(left, capacity).ok);
  TOS_REQUIRE(ledger.set_capacity(right, capacity).ok);

  auto left_slot = ledger.acquire(
      reservation_request(left, amounts_of({{ResourceKind::kExecutionSlot, 1}})));
  TOS_REQUIRE(left_slot.ok());
  // The same kind on another domain is a different pool.
  auto right_slot = ledger.acquire(
      reservation_request(right, amounts_of({{ResourceKind::kExecutionSlot, 1}})));
  TOS_REQUIRE(right_slot.ok());
  // Another kind on the exhausted domain is still available.
  auto left_queue = ledger.acquire(
      reservation_request(left, amounts_of({{ResourceKind::kQueueDepth, 1}})));
  TOS_REQUIRE(left_queue.ok());

  auto left_slot_again = ledger.acquire(
      reservation_request(left, amounts_of({{ResourceKind::kExecutionSlot, 1}})));
  TOS_CHECK(!left_slot_again.ok());
  expect_code(left_slot_again.status, "reservation.insufficient_capacity", "left slot exhausted");
  auto right_slot_again = ledger.acquire(
      reservation_request(right, amounts_of({{ResourceKind::kExecutionSlot, 1}})));
  TOS_CHECK(!right_slot_again.ok());
  expect_code(right_slot_again.status, "reservation.insufficient_capacity", "right slot exhausted");

  const ReservationAudit audit = ledger.audit();
  const DomainAccounting* left_row = accounting_for(audit, left);
  const DomainAccounting* right_row = accounting_for(audit, right);
  TOS_REQUIRE(left_row != nullptr);
  TOS_REQUIRE(right_row != nullptr);
  TOS_CHECK_EQ(left_row->reserved[slot_index], std::uint64_t{1});
  TOS_CHECK_EQ(left_row->reserved[queue_index], std::uint64_t{1});
  TOS_CHECK_EQ(right_row->reserved[slot_index], std::uint64_t{1});
  TOS_CHECK_EQ(right_row->reserved[queue_index], std::uint64_t{0});
  TOS_CHECK_EQ(audit.outstanding, std::uint64_t{3});
  expect_consistent(audit, "per-domain audit");
}

// 4. Double commit, double release, commit-after-release and unknown ids are rejected.
TOS_TEST(reservation_duplicate_and_unknown_transitions_are_rejected) {
  tos_test::TestRuntime runtime;
  ReservationLedger& ledger = runtime.scheduler().reservations();
  const ExecutionDomainId domain(905);
  TOS_REQUIRE(ledger.set_capacity(domain, capacity_of({{ResourceKind::kExecutionSlot, 4}})).ok);
  const ReservationRequest request =
      reservation_request(domain, amounts_of({{ResourceKind::kExecutionSlot, 1}}));

  auto acquired = ledger.acquire(request);
  TOS_REQUIRE(acquired.ok());
  const ReservationId id = acquired.value;
  TOS_REQUIRE(ledger.commit(id, ExecutionDomainGeneration(3), CapabilityGeneration(5),
                            PolicyGeneration(2))
                  .ok);
  expect_code(ledger.commit(id, ExecutionDomainGeneration(3), CapabilityGeneration(5),
                            PolicyGeneration(2)),
              "reservation.already_committed", "double commit");
  TOS_REQUIRE(ledger.release(id, "done").ok);
  expect_code(ledger.release(id, "again"), "reservation.already_released", "double release");

  // Commit after the reservation was released.
  auto second = ledger.acquire(request);
  TOS_REQUIRE(second.ok());
  const ReservationId second_id = second.value;
  TOS_REQUIRE(ledger.release(second_id, "cancelled").ok);
  expect_code(ledger.commit(second_id, ExecutionDomainGeneration(3), CapabilityGeneration(5),
                            PolicyGeneration(2)),
              "reservation.already_released", "commit after release");

  // A well-formed id that was never issued.
  const ReservationId unknown((std::uint64_t{1} << 56) | 4242ULL);
  expect_code(ledger.release(unknown, "unknown"), "reservation.not_found", "release unknown id");
  expect_code(ledger.commit(unknown, ExecutionDomainGeneration(3), CapabilityGeneration(5),
                            PolicyGeneration(2)),
              "reservation.not_found", "commit unknown id");
  expect_code(ledger.rollback(unknown, "unknown"), "reservation.not_found", "rollback unknown id");

  const ReservationAudit audit = ledger.audit();
  TOS_CHECK_EQ(audit.outstanding, std::uint64_t{0});
  expect_consistent(audit, "duplicate transitions audit");
}

// 5. Commit is bound to the generations captured at acquisition.
TOS_TEST(reservation_stale_generation_commit_is_rejected) {
  tos_test::TestRuntime runtime;
  ReservationLedger& ledger = runtime.scheduler().reservations();
  const ExecutionDomainId domain(906);
  TOS_REQUIRE(ledger.set_capacity(domain, capacity_of({{ResourceKind::kExecutionSlot, 1}})).ok);

  auto acquired = ledger.acquire(reservation_request(
      domain, amounts_of({{ResourceKind::kExecutionSlot, 1}}), ExecutionDomainGeneration(3),
      CapabilityGeneration(5), PolicyGeneration(2)));
  TOS_REQUIRE(acquired.ok());
  const ReservationId id = acquired.value;

  expect_code(ledger.commit(id, ExecutionDomainGeneration(4), CapabilityGeneration(5),
                            PolicyGeneration(2)),
              "reservation.stale_domain_generation", "stale domain generation");
  expect_code(ledger.commit(id, ExecutionDomainGeneration(3), CapabilityGeneration(6),
                            PolicyGeneration(2)),
              "reservation.stale_capability_generation", "stale capability generation");
  expect_code(ledger.commit(id, ExecutionDomainGeneration(3), CapabilityGeneration(5),
                            PolicyGeneration(3)),
              "reservation.stale_policy_generation", "stale policy generation");

  Reservation record;
  TOS_REQUIRE(ledger.get(id, record));
  TOS_CHECK_EQ(record.state, ReservationState::kActive);

  TOS_REQUIRE(ledger.commit(id, ExecutionDomainGeneration(3), CapabilityGeneration(5),
                            PolicyGeneration(2))
                  .ok);
  TOS_REQUIRE(ledger.get(id, record));
  TOS_CHECK_EQ(record.state, ReservationState::kCommitted);
  TOS_REQUIRE(ledger.release(id, "done").ok);
  expect_consistent(ledger.audit(), "stale generation audit");
}

// 6. Rollback returns capacity exactly once.
TOS_TEST(reservation_rollback_returns_capacity_exactly_once) {
  tos_test::TestRuntime runtime;
  ReservationLedger& ledger = runtime.scheduler().reservations();
  const ExecutionDomainId domain(907);
  TOS_REQUIRE(ledger.set_capacity(domain, capacity_of({{ResourceKind::kExecutionSlot, 1}})).ok);
  const ReservationRequest request =
      reservation_request(domain, amounts_of({{ResourceKind::kExecutionSlot, 1}}));

  auto acquired = ledger.acquire(request);
  TOS_REQUIRE(acquired.ok());
  const ReservationId id = acquired.value;
  TOS_REQUIRE(ledger.rollback(id, "never became authoritative").ok);

  Reservation record;
  TOS_REQUIRE(ledger.get(id, record));
  TOS_CHECK_EQ(record.state, ReservationState::kRolledBack);
  TOS_CHECK_EQ(ledger.outstanding_count(), std::uint64_t{0});

  const Status released = ledger.release(id, "late release");
  TOS_CHECK_MSG(!released.ok, "release after rollback must fail, got code '" + released.code + "'");
  TOS_REQUIRE(ledger.get(id, record));
  TOS_CHECK_EQ(record.state, ReservationState::kRolledBack);

  const ReservationAudit audit = ledger.audit();
  TOS_CHECK_EQ(audit.outstanding, std::uint64_t{0});
  expect_consistent(audit, "rollback audit");
  const DomainAccounting* row = accounting_for(audit, domain);
  TOS_REQUIRE(row != nullptr);
  TOS_CHECK_EQ(row->reserved[slot_index], std::uint64_t{0});
  TOS_CHECK_EQ(row->rolled_back_count, std::uint64_t{1});

  // The capacity is genuinely back in the pool.
  auto reacquired = ledger.acquire(request);
  TOS_REQUIRE(reacquired.ok());
  TOS_CHECK_EQ(ledger.outstanding_count(), std::uint64_t{1});
  TOS_REQUIRE(ledger.rollback(reacquired.value, "cleanup").ok);
}

// 7. Domain invalidation returns every held reservation to the pool.
TOS_TEST(reservation_invalidate_domain_releases_everything) {
  tos_test::TestRuntime runtime;
  ReservationLedger& ledger = runtime.scheduler().reservations();
  const ExecutionDomainId domain(908);
  const ExecutionDomainId other(909);
  TOS_REQUIRE(ledger.set_capacity(domain, capacity_of({{ResourceKind::kExecutionSlot, 4},
                                                       {ResourceKind::kQueueDepth, 4}}))
                  .ok);
  TOS_REQUIRE(ledger.set_capacity(other, capacity_of({{ResourceKind::kExecutionSlot, 4}})).ok);

  auto active = ledger.acquire(
      reservation_request(domain, amounts_of({{ResourceKind::kExecutionSlot, 1}})));
  auto committed = ledger.acquire(
      reservation_request(domain, amounts_of({{ResourceKind::kExecutionSlot, 2}})));
  auto queued = ledger.acquire(
      reservation_request(domain, amounts_of({{ResourceKind::kQueueDepth, 1}})));
  auto survivor = ledger.acquire(
      reservation_request(other, amounts_of({{ResourceKind::kExecutionSlot, 1}})));
  TOS_REQUIRE(active.ok() && committed.ok() && queued.ok() && survivor.ok());
  TOS_REQUIRE(ledger.commit(committed.value, ExecutionDomainGeneration(3), CapabilityGeneration(5),
                            PolicyGeneration(2))
                  .ok);

  const std::size_t affected = ledger.invalidate_domain(domain, "domain authority withdrawn");
  TOS_CHECK_EQ(affected, std::size_t{3});

  for (ReservationId id : {active.value, committed.value, queued.value}) {
    Reservation record;
    TOS_REQUIRE(ledger.get(id, record));
    TOS_CHECK_EQ(record.state, ReservationState::kInvalidated);
    TOS_CHECK(record.close_reason == "domain authority withdrawn");
    const Status released = ledger.release(id, "late release");
    TOS_CHECK_MSG(!released.ok, "release after invalidation must fail");
  }
  {
    Reservation record;
    TOS_REQUIRE(ledger.get(survivor.value, record));
    TOS_CHECK_EQ(record.state, ReservationState::kActive);
  }
  TOS_CHECK_EQ(ledger.outstanding_count(), std::uint64_t{1});

  const ReservationAudit audit = ledger.audit();
  TOS_CHECK_EQ(audit.outstanding, std::uint64_t{1});
  expect_consistent(audit, "invalidation audit");
  const DomainAccounting* row = accounting_for(audit, domain);
  TOS_REQUIRE(row != nullptr);
  TOS_CHECK_EQ(row->reserved[slot_index], std::uint64_t{0});
  TOS_CHECK_EQ(row->reserved[queue_index], std::uint64_t{0});
  TOS_CHECK_EQ(row->committed[slot_index], std::uint64_t{0});
  TOS_CHECK_EQ(row->active_count, std::uint64_t{0});
  TOS_CHECK_EQ(row->committed_count, std::uint64_t{0});
  TOS_CHECK_EQ(row->invalidated_count, std::uint64_t{3});

  // Idempotent for a domain that holds nothing, and the pool is reusable afterwards.
  TOS_CHECK_EQ(ledger.invalidate_domain(domain, "again"), std::size_t{0});
  auto reused = ledger.acquire(reservation_request(
      domain, amounts_of({{ResourceKind::kExecutionSlot, 4}, {ResourceKind::kQueueDepth, 4}})));
  TOS_REQUIRE(reused.ok());
  TOS_REQUIRE(ledger.rollback(reused.value, "cleanup").ok);
  TOS_REQUIRE(ledger.rollback(survivor.value, "cleanup").ok);
}

// 8. Capacity may not be published below what is already held.
TOS_TEST(reservation_set_capacity_below_outstanding_is_refused) {
  tos_test::TestRuntime runtime;
  ReservationLedger& ledger = runtime.scheduler().reservations();
  const ExecutionDomainId domain(910);
  TOS_REQUIRE(ledger.set_capacity(domain, capacity_of({{ResourceKind::kExecutionSlot, 2},
                                                       {ResourceKind::kQueueDepth, 2}}))
                  .ok);
  auto acquired = ledger.acquire(reservation_request(
      domain, amounts_of({{ResourceKind::kExecutionSlot, 2}, {ResourceKind::kQueueDepth, 1}})));
  TOS_REQUIRE(acquired.ok());

  expect_code(ledger.set_capacity(domain, capacity_of({{ResourceKind::kExecutionSlot, 1},
                                                       {ResourceKind::kQueueDepth, 2}})),
              "reservation.capacity_below_outstanding", "capacity below reserved slots");
  expect_code(ledger.set_capacity(domain, capacity_of({{ResourceKind::kExecutionSlot, 2},
                                                       {ResourceKind::kQueueDepth, 0}})),
              "reservation.capacity_below_outstanding", "capacity below reserved queue depth");

  // The refused publication changed nothing.
  const ReservationAudit audit = ledger.audit();
  const DomainAccounting* row = accounting_for(audit, domain);
  TOS_REQUIRE(row != nullptr);
  TOS_CHECK_EQ(row->capacity[slot_index], std::uint64_t{2});
  TOS_CHECK_EQ(row->capacity[queue_index], std::uint64_t{2});
  expect_consistent(audit, "refused capacity audit");

  TOS_REQUIRE(ledger.release(acquired.value, "done").ok);
  TOS_REQUIRE(ledger.set_capacity(domain, capacity_of({{ResourceKind::kExecutionSlot, 1},
                                                       {ResourceKind::kQueueDepth, 1}}))
                  .ok);
  const ReservationAudit lowered = ledger.audit();
  const DomainAccounting* lowered_row = accounting_for(lowered, domain);
  TOS_REQUIRE(lowered_row != nullptr);
  TOS_CHECK_EQ(lowered_row->capacity[slot_index], std::uint64_t{1});
  expect_consistent(lowered, "lowered capacity audit");
}

// 9. Identities are unique and stable; list() is sorted and bounded by its limit.
TOS_TEST(reservation_ids_are_unique_stable_and_sorted) {
  tos_test::TestRuntime runtime;
  ReservationLedger& ledger = runtime.scheduler().reservations();
  const ExecutionDomainId first_domain(911);
  const ExecutionDomainId second_domain(912);
  const CapacityVector capacity = capacity_of({{ResourceKind::kExecutionSlot, 8}});
  TOS_REQUIRE(ledger.set_capacity(first_domain, capacity).ok);
  TOS_REQUIRE(ledger.set_capacity(second_domain, capacity).ok);

  std::vector<ReservationId> acquired;
  for (int i = 0; i < 4; ++i) {
    auto result = ledger.acquire(
        reservation_request(first_domain, amounts_of({{ResourceKind::kExecutionSlot, 1}})));
    TOS_REQUIRE(result.ok());
    acquired.push_back(result.value);
  }
  for (int i = 0; i < 2; ++i) {
    auto result = ledger.acquire(
        reservation_request(second_domain, amounts_of({{ResourceKind::kExecutionSlot, 1}})));
    TOS_REQUIRE(result.ok());
    acquired.push_back(result.value);
  }

  std::set<std::uint64_t> unique;
  for (ReservationId id : acquired) {
    TOS_CHECK(id.valid());
    unique.insert(id.value());
  }
  TOS_CHECK_EQ(unique.size(), acquired.size());

  for (ReservationId id : acquired) {
    Reservation record;
    TOS_REQUIRE(ledger.get(id, record));
    TOS_CHECK_EQ(record.id, id);
    TOS_CHECK_EQ(record.state, ReservationState::kActive);
    TOS_CHECK_EQ(record.amounts[slot_index], std::uint64_t{1});
    TOS_CHECK(record.domain == first_domain || record.domain == second_domain);
  }

  // get() hands back a copy: reading twice is stable and mutating the copy is harmless.
  Reservation first_read;
  TOS_REQUIRE(ledger.get(acquired.front(), first_read));
  first_read.state = ReservationState::kReleased;
  first_read.amounts[slot_index] = 999;
  Reservation second_read;
  TOS_REQUIRE(ledger.get(acquired.front(), second_read));
  TOS_CHECK_EQ(second_read.state, ReservationState::kActive);
  TOS_CHECK_EQ(second_read.amounts[slot_index], std::uint64_t{1});

  const std::vector<Reservation> listed = ledger.list(64);
  TOS_CHECK_EQ(listed.size(), acquired.size());
  TOS_CHECK(std::is_sorted(listed.begin(), listed.end(),
                           [](const Reservation& a, const Reservation& b) { return a.id < b.id; }));
  std::vector<ReservationId> listed_ids;
  listed_ids.reserve(listed.size());
  for (const Reservation& record : listed) listed_ids.push_back(record.id);
  std::vector<ReservationId> expected = acquired;
  std::sort(expected.begin(), expected.end());
  TOS_CHECK(listed_ids == expected);

  const std::vector<Reservation> limited = ledger.list(2);
  TOS_CHECK_EQ(limited.size(), std::size_t{2});
  TOS_CHECK_EQ(limited.front().id, expected.front());
  TOS_CHECK_EQ(limited.back().id, expected[1]);
}

// 10. Through the scheduler: one slot of capacity admits exactly one planned operation.
TOS_TEST(scheduler_capacity_admission_bounds_concurrent_plans) {
  tos_test::TestRuntime::Config config;
  config.policy = reservation_policy();
  tos_test::TestRuntime runtime(config);

  SyntheticDomainConfig domain = make_synthetic_domain(ExecutionDomainId(913),
                                                       ExecutionDomainType::kDpu, "dpu.capacity");
  domain.capacity = CapacityVector{};
  domain.capacity[slot_index] = 1;
  const ExecutionDomainId domain_id = runtime.add_domain(domain);

  ReservationLedger& ledger = runtime.scheduler().reservations();
  const std::vector<std::uint8_t> payload = tos_test::make_payload(512, 13);
  const std::span<const std::uint8_t> bytes(payload.data(), payload.size());

  PlanResult first = runtime.scheduler().plan(
      tos_test::make_request(opclass::checksum_crc32c(), payload.size()), bytes);
  TOS_REQUIRE(first.planned);
  TOS_CHECK_EQ(first.explanation.selected_domain, domain_id);
  ExecutionPlan first_plan = first.plan;
  TOS_REQUIRE(runtime.scheduler().reserve(first_plan).ok);
  TOS_REQUIRE(first_plan.reservation.valid());
  TOS_CHECK_EQ(ledger.outstanding_count(), std::uint64_t{1});

  // The second operation must not be admitted while the only slot is held.
  PlanResult second = runtime.scheduler().plan(
      tos_test::make_request(opclass::checksum_crc32c(), payload.size()), bytes);
  if (!second.planned) {
    TOS_CHECK_MSG(second.explanation.outcome == SelectionOutcome::kCapacityUnavailable,
                  std::string("second plan rejected with '") +
                      std::string(to_string(second.explanation.outcome)) +
                      "', expected capacity_unavailable");
    TOS_REQUIRE(runtime.scheduler().reservations().release(first_plan.reservation, "free slot").ok);
    PlanResult retry = runtime.scheduler().plan(
        tos_test::make_request(opclass::checksum_crc32c(), payload.size()), bytes);
    TOS_REQUIRE(retry.planned);
    ExecutionPlan retry_plan = retry.plan;
    TOS_REQUIRE(runtime.scheduler().reserve(retry_plan).ok);
    TOS_REQUIRE(runtime.scheduler().reservations().rollback(retry_plan.reservation, "cleanup").ok);
  } else {
    const Status reserved = runtime.scheduler().reserve(second.plan);
    TOS_CHECK_MSG(!reserved.ok && reserved.code == "reservation.insufficient_capacity",
                  std::string("second reserve must fail with reservation.insufficient_capacity, got ok=") +
                      (reserved.ok ? std::string("true") : std::string("false")) + " code='" +
                      reserved.code + "'");
    TOS_REQUIRE(runtime.scheduler().reservations().release(first_plan.reservation, "free slot").ok);
    // Releasing the first must make the already-planned second operation reservable.
    ExecutionPlan second_plan = second.plan;
    TOS_REQUIRE(runtime.scheduler().reserve(second_plan).ok);
    TOS_REQUIRE(runtime.scheduler().reservations().rollback(second_plan.reservation, "cleanup").ok);
  }

  TOS_CHECK_EQ(ledger.outstanding_count(), std::uint64_t{0});
  expect_consistent(ledger.audit(), "scheduler capacity audit");
}
