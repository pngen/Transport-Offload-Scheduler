// Durable state format: canonical encoding, complete atomic replacement, a
// corruption matrix, generation rollback and path safety.
//
// Every rejection below is asserted at the decoder boundary: a malformed file must
// never produce a partially applied state, and a refused load must leave the live
// runtime byte-for-byte where it was.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "tos_test_support.hpp"

using namespace tos;

namespace {

constexpr std::size_t kFileHeaderBytes = 16;

// ---- field comparison ------------------------------------------------------

bool equal_capability_set(const CapabilitySet& a, const CapabilitySet& b) {
  return a.operations == b.operations && a.flags == b.flags && a.unproven_flags == b.unproven_flags &&
         a.memory_domains == b.memory_domains && a.min_payload_bytes == b.min_payload_bytes &&
         a.max_payload_bytes == b.max_payload_bytes && a.alignment_bytes == b.alignment_bytes &&
         a.max_concurrency == b.max_concurrency && a.queue_capacity == b.queue_capacity &&
         a.transport_classes == b.transport_classes && a.payload_classes == b.payload_classes &&
         a.protocol_version == b.protocol_version &&
         a.driver_backend_version == b.driver_backend_version &&
         a.firmware_generation == b.firmware_generation && a.accelerator_arch == b.accelerator_arch &&
         a.backend_family == b.backend_family;
}

bool equal_capability_record(const CapabilityRecord& a, const CapabilityRecord& b) {
  return a.domain == b.domain && a.generation == b.generation && a.evidence == b.evidence &&
         a.provenance == b.provenance && a.authoritative == b.authoritative &&
         equal_capability_set(a.capability, b.capability);
}

bool equal_load(const DomainLoadEvidence& a, const DomainLoadEvidence& b) {
  return a.load_generation == b.load_generation && a.queue_generation == b.queue_generation &&
         a.health_generation == b.health_generation && a.evidence == b.evidence &&
         a.provenance == b.provenance && a.utilization_percent == b.utilization_percent &&
         a.congestion_percent == b.congestion_percent && a.queue_depth == b.queue_depth &&
         a.in_flight == b.in_flight && a.observed_latency_ns == b.observed_latency_ns &&
         a.throughput_bytes_per_second == b.throughput_bytes_per_second && a.healthy == b.healthy &&
         a.ready == b.ready && a.accepting == b.accepting;
}

bool equal_locality(const DomainLocality& a, const DomainLocality& b) {
  return a.generation == b.generation && a.class_to_payload == b.class_to_payload &&
         a.numa_node == b.numa_node && a.host_index == b.host_index &&
         a.pcie_root_complex == b.pcie_root_complex && a.pcie_switch == b.pcie_switch &&
         a.local_nic == b.local_nic && a.has_local_nic == b.has_local_nic;
}

bool equal_topology(const DomainTopology& a, const DomainTopology& b) {
  return a.generation == b.generation && a.host_node == b.host_node && a.fabric == b.fabric &&
         a.rack == b.rack && a.pod == b.pod;
}

bool equal_compatibility(const DomainCompatibility& a, const DomainCompatibility& b) {
  return a.generation == b.generation && a.driver_backend_version == b.driver_backend_version &&
         a.firmware_generation == b.firmware_generation && a.protocol_version == b.protocol_version &&
         a.accelerator_arch == b.accelerator_arch && a.backend_family == b.backend_family;
}

/// Durable structural fields of a domain record. Volatile load/health/queue
/// evidence is deliberately excluded from the format (the store contract states
/// that a recovered runtime must not resurrect operational truth), so callers
/// assert the exclusion explicitly with load_is_cleared().
bool equal_domain(const ExecutionDomainRecord& a, const ExecutionDomainRecord& b) {
  return a.id == b.id && a.generation == b.generation && a.type == b.type && a.name == b.name &&
         a.parent_device == b.parent_device && a.parent_host == b.parent_host &&
         a.worker == b.worker && a.worker_boot == b.worker_boot && a.provenance == b.provenance &&
         a.registration_sequence == b.registration_sequence &&
         equal_capability_record(a.capability, b.capability) &&
         equal_locality(a.locality, b.locality) && equal_topology(a.topology, b.topology) &&
         equal_compatibility(a.compatibility, b.compatibility) && a.isolation == b.isolation &&
         a.capacity == b.capacity && a.backend_generation == b.backend_generation &&
         a.fenced == b.fenced && a.fence_reason == b.fence_reason;
}

/// True when no volatile evidence survived: every field is the default.
bool load_is_cleared(const DomainLoadEvidence& load) {
  return equal_load(load, DomainLoadEvidence{});
}

bool equal_result(const ExecutionResultPayload& a, const ExecutionResultPayload& b) {
  return a.success == b.success && a.result_digest == b.result_digest &&
         a.bytes_processed == b.bytes_processed && a.duration_ns == b.duration_ns &&
         a.backend_detail == b.backend_detail;
}

bool equal_attempt(const ExecutionAttempt& a, const ExecutionAttempt& b) {
  return a.id == b.id && a.generation == b.generation && a.operation == b.operation &&
         a.operation_class == b.operation_class && a.side_effect == b.side_effect &&
         a.domain == b.domain && a.domain_type == b.domain_type &&
         a.domain_generation == b.domain_generation &&
         a.capability_generation == b.capability_generation && a.worker == b.worker &&
         a.worker_boot == b.worker_boot && a.coordinator_epoch == b.coordinator_epoch &&
         a.reservation == b.reservation && a.dispatch == b.dispatch && a.state == b.state &&
         a.resolution == b.resolution && a.failure == b.failure &&
         a.failure_detail == b.failure_detail && a.close_reason == b.close_reason &&
         a.provenance == b.provenance && a.fallback == b.fallback &&
         a.fallback_from == b.fallback_from && a.retry_index == b.retry_index &&
         a.cancellation_requested == b.cancellation_requested &&
         a.completion_committed == b.completion_committed && equal_result(a.result, b.result) &&
         a.sequence == b.sequence && a.dispatch_sequence == b.dispatch_sequence &&
         a.snapshot == b.snapshot;
}

bool equal_freshness(const FreshnessRequirements& a, const FreshnessRequirements& b) {
  return a.capability == b.capability && a.health == b.health && a.queue == b.queue &&
         a.load == b.load && a.topology == b.topology && a.locality == b.locality &&
         a.compatibility == b.compatibility && a.isolation == b.isolation && a.evidence == b.evidence;
}

bool equal_retry(const RetryPolicy& a, const RetryPolicy& b) {
  return a.enabled == b.enabled && a.max_attempts == b.max_attempts &&
         a.max_ambiguous_retries == b.max_ambiguous_retries && a.rules.retryable == b.rules.retryable;
}

bool equal_fallback(const FallbackPolicy& a, const FallbackPolicy& b) {
  return a.enabled == b.enabled && a.max_depth == b.max_depth &&
         a.allow_host_fallback == b.allow_host_fallback && a.chains == b.chains;
}

bool equal_reservation_policy(const ReservationPolicy& a, const ReservationPolicy& b) {
  return a.enabled == b.enabled && a.per_operation == b.per_operation &&
         a.require_reservation_for_offload_only == b.require_reservation_for_offload_only;
}

bool equal_policy(const SchedulerPolicy& a, const SchedulerPolicy& b) {
  return a.generation == b.generation && a.isolation_generation == b.isolation_generation &&
         a.compatibility_generation == b.compatibility_generation &&
         a.offload_requirement == b.offload_requirement &&
         a.minimum_isolation == b.minimum_isolation &&
         a.require_positive_evidence == b.require_positive_evidence &&
         a.allow_unsupported_provenance == b.allow_unsupported_provenance &&
         a.forbidden_domains == b.forbidden_domains &&
         a.forbidden_domain_ids == b.forbidden_domain_ids &&
         a.forbidden_provenance == b.forbidden_provenance && a.weights == b.weights &&
         a.preference_order == b.preference_order &&
         equal_reservation_policy(a.reservation, b.reservation) && equal_retry(a.retry, b.retry) &&
         equal_fallback(a.fallback, b.fallback) && equal_freshness(a.freshness, b.freshness) &&
         a.type_setup_cost_units == b.type_setup_cost_units &&
         a.type_per_byte_cost_micro_units == b.type_per_byte_cost_micro_units &&
         a.maximum_cost_units == b.maximum_cost_units &&
         a.max_reported_candidates == b.max_reported_candidates;
}

bool equal_totals(const SnapshotTotals& a, const SnapshotTotals& b) {
  return a.domains == b.domains && a.domains_fenced == b.domains_fenced &&
         a.domains_with_current_evidence == b.domains_with_current_evidence &&
         a.live_attempts == b.live_attempts && a.completed_attempts == b.completed_attempts &&
         a.ambiguous_attempts == b.ambiguous_attempts && a.fenced_attempts == b.fenced_attempts &&
         a.outstanding_reservations == b.outstanding_reservations &&
         a.plans_created == b.plans_created && a.plans_dispatched == b.plans_dispatched &&
         a.dispatch_rejections == b.dispatch_rejections &&
         a.completion_rejections == b.completion_rejections && a.fallbacks == b.fallbacks &&
         a.retries == b.retries;
}

bool equal_state(const PersistedState& a, const PersistedState& b) {
  if (a.generation != b.generation || a.last_epoch != b.last_epoch ||
      a.policy_generation != b.policy_generation || a.has_policy != b.has_policy) {
    return false;
  }
  if (a.has_policy && !equal_policy(a.policy, b.policy)) return false;
  for (int i = 0; i < kCounterCount; ++i) {
    if (a.counters[i] != b.counters[i]) return false;
  }
  if (a.domains.size() != b.domains.size() || a.attempts.size() != b.attempts.size() ||
      a.fenced_boots.size() != b.fenced_boots.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.domains.size(); ++i) {
    if (!equal_domain(a.domains[i], b.domains[i])) return false;
    if (!load_is_cleared(b.domains[i].load)) return false;
  }
  for (std::size_t i = 0; i < a.attempts.size(); ++i) {
    if (!equal_attempt(a.attempts[i], b.attempts[i])) return false;
  }
  for (std::size_t i = 0; i < a.fenced_boots.size(); ++i) {
    if (a.fenced_boots[i] != b.fenced_boots[i]) return false;
  }
  return true;
}

std::vector<ExecutionDomainRecord> sorted_domains(std::vector<ExecutionDomainRecord> domains) {
  std::sort(domains.begin(), domains.end(),
            [](const ExecutionDomainRecord& a, const ExecutionDomainRecord& b) { return a.id < b.id; });
  return domains;
}

std::vector<ExecutionAttempt> sorted_attempts(std::vector<ExecutionAttempt> attempts) {
  std::sort(attempts.begin(), attempts.end(),
            [](const ExecutionAttempt& a, const ExecutionAttempt& b) { return a.id < b.id; });
  return attempts;
}

// ---- byte helpers ----------------------------------------------------------

void append_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
  out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFU));
}

void append_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
  }
}

std::vector<std::uint8_t> read_file(const std::string& path) {
  const Checked<std::vector<std::uint8_t>> bytes = read_file_bytes(path, kMaxPersistedBytes);
  TOS_CHECK_MSG(bytes.ok(), "read_file_bytes failed: " + bytes.status.code);
  return bytes.ok() ? bytes.value : std::vector<std::uint8_t>{};
}

std::vector<std::uint8_t> slice(const std::vector<std::uint8_t>& bytes, std::size_t begin,
                                std::size_t end) {
  if (end > bytes.size() || begin > end) return {};
  return std::vector<std::uint8_t>(bytes.begin() + static_cast<std::ptrdiff_t>(begin),
                                   bytes.begin() + static_cast<std::ptrdiff_t>(end));
}

std::uint32_t u32_at(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(bytes[offset + static_cast<std::size_t>(i)]) << (8 * i);
  }
  return value;
}

void set_u16_at(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t value) {
  bytes[offset] = static_cast<std::uint8_t>(value & 0xFFU);
  bytes[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFFU);
}

void set_u32_at(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    bytes[offset + static_cast<std::size_t>(i)] =
        static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFU);
  }
}

/// Rebuild a syntactically valid file around an explicitly chosen body. Used to
/// construct files the canonical encoder would never produce (unsorted records).
std::vector<std::uint8_t> seal_body(const std::vector<std::uint8_t>& body) {
  std::vector<std::uint8_t> file;
  file.push_back('T');
  file.push_back('O');
  file.push_back('S');
  file.push_back('P');
  append_u16(file, kPersistenceFormatVersion);
  append_u16(file, 0);
  append_u32(file, static_cast<std::uint32_t>(body.size()));
  append_u32(file, crc32c(body.data(), body.size()));
  file.insert(file.end(), body.begin(), body.end());
  return file;
}

std::vector<std::uint8_t> body_of(const PersistedState& state) {
  const std::vector<std::uint8_t> bytes = StateStore::encode(state);
  TOS_CHECK(bytes.size() > kFileHeaderBytes);
  return slice(bytes, kFileHeaderBytes, bytes.size());
}

/// Prefix fields shared by every hand-assembled file: generation, epoch, policy
/// generation, has_policy and counters. Keeping one source for the prefix makes
/// the record bytes below sliceable out of a canonical encoding.
PersistedState layout_state() {
  PersistedState state;
  state.generation = PersistenceGeneration(5);
  state.last_epoch = CoordinatorEpoch(4);
  state.policy_generation = PolicyGeneration(1);
  state.has_policy = false;
  for (int i = 0; i < kCounterCount; ++i) state.counters[i] = 1000;
  return state;
}

std::vector<std::uint8_t> layout_prefix() {
  const std::vector<std::uint8_t> body = body_of(layout_state());
  TOS_CHECK(body.size() > 12);
  return slice(body, 0, body.size() - 12);  // drop the three empty collection counts
}

std::vector<std::uint8_t> domain_record_bytes(const ExecutionDomainRecord& domain) {
  PersistedState state = layout_state();
  state.domains.push_back(domain);
  const std::vector<std::uint8_t> body = body_of(state);
  const std::size_t prefix = layout_prefix().size();
  return slice(body, prefix + 4, body.size() - 8);  // count, record, then two zero counts
}

std::vector<std::uint8_t> attempt_record_bytes(const ExecutionAttempt& attempt) {
  PersistedState state = layout_state();
  state.attempts.push_back(attempt);
  const std::vector<std::uint8_t> body = body_of(state);
  const std::size_t prefix = layout_prefix().size();
  return slice(body, prefix + 8, body.size() - 4);  // two counts, record, then one zero count
}

std::vector<std::uint8_t> assemble_body(
    const std::vector<std::vector<std::uint8_t>>& domains,
    const std::vector<std::vector<std::uint8_t>>& attempts,
    const std::vector<std::uint64_t>& boots) {
  std::vector<std::uint8_t> body = layout_prefix();
  append_u32(body, static_cast<std::uint32_t>(domains.size()));
  for (const std::vector<std::uint8_t>& record : domains) body.insert(body.end(), record.begin(), record.end());
  append_u32(body, static_cast<std::uint32_t>(attempts.size()));
  for (const std::vector<std::uint8_t>& record : attempts) body.insert(body.end(), record.begin(), record.end());
  append_u32(body, static_cast<std::uint32_t>(boots.size()));
  for (std::uint64_t boot : boots) {
    for (int shift = 0; shift < 64; shift += 8) {
      body.push_back(static_cast<std::uint8_t>((boot >> shift) & 0xFFU));
    }
  }
  return body;
}

// ---- fixture helpers -------------------------------------------------------

/// Plan, reserve, dispatch and wait for one operation to commit. Returns the
/// terminal ledger record, or a default-constructed one on failure.
ExecutionAttempt commit_one(tos_test::TestRuntime& runtime, std::size_t payload_bytes,
                            std::uint32_t seed) {
  const std::vector<std::uint8_t> payload = tos_test::make_payload(payload_bytes, seed);
  OperationRequest request = tos_test::make_request(opclass::checksum_crc32c(), payload.size());
  const DispatchOutcome outcome = runtime.scheduler().plan_reserve_dispatch(request, payload);
  if (!outcome.plan_result.planned || !outcome.dispatch_result.dispatched) {
    TOS_CHECK_MSG(false, "plan_reserve_dispatch failed: " +
                             outcome.plan_result.status.code + "/" +
                             outcome.dispatch_result.status.code);
    return ExecutionAttempt{};
  }
  const ExecutionAttemptId attempt = outcome.plan_result.plan.attempt;
  runtime.wait_for(attempt);
  const ExecutionAttempt terminal = runtime.wait_for_terminal(attempt);
  TOS_CHECK_EQ(terminal.state, AttemptState::kCompleted);
  TOS_CHECK(terminal.completion_committed);
  return terminal;
}

PersistedState state_from_runtime(tos_test::TestRuntime& runtime, std::uint64_t generation) {
  PersistedState state;
  state.generation = PersistenceGeneration(generation);
  state.last_epoch = runtime.scheduler().coordinator_epoch();
  state.policy = runtime.scheduler().policy();
  state.has_policy = true;
  state.policy_generation = runtime.scheduler().policy_generation();
  state.domains = runtime.backend().discover_domains();
  state.attempts = runtime.scheduler().attempts().list(64);
  for (int i = 0; i < kCounterCount; ++i) {
    state.counters[i] = generation * 10 + static_cast<std::uint64_t>(i);
  }
  return state;
}

std::string decode_code(const std::vector<std::uint8_t>& bytes) {
  return StateStore::decode(bytes).status.code;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. Round trip
// ---------------------------------------------------------------------------

TOS_TEST(persistence_round_trip_preserves_every_field) {
  tos_test::TestRuntime runtime;
  const ExecutionDomainId dpu = runtime.add_domain(
      make_synthetic_domain(ExecutionDomainId(11), ExecutionDomainType::kDpu, "dpu.roundtrip"));
  const ExecutionDomainId smartnic = runtime.add_domain(
      make_synthetic_domain(ExecutionDomainId(12), ExecutionDomainType::kSmartNic, "smartnic.roundtrip"));
  const ExecutionDomainId accelerator = runtime.add_domain(make_synthetic_domain(
      ExecutionDomainId(13), ExecutionDomainType::kAccelerator, "accelerator.roundtrip", false));
  TOS_REQUIRE(dpu.valid() && smartnic.valid() && accelerator.valid());

  const ExecutionAttempt first = commit_one(runtime, 1024, 3);
  const ExecutionAttempt second = commit_one(runtime, 2048, 4);
  TOS_REQUIRE(first.id.valid() && second.id.valid());
  TOS_CHECK_EQ(first.state, AttemptState::kCompleted);
  TOS_CHECK_EQ(second.state, AttemptState::kCompleted);

  PersistedState state = state_from_runtime(runtime, 9);
  // Non-default policy content so that every policy vector is exercised.
  state.policy.forbidden_domains = {ExecutionDomainType::kCpu};
  state.policy.forbidden_domain_ids = {ExecutionDomainId(99), ExecutionDomainId(100)};
  state.policy.forbidden_provenance = {Provenance::kUnsupported};
  state.policy.reservation.enabled = true;
  state.policy.reservation.per_operation[static_cast<std::size_t>(ResourceKind::kExecutionSlot)] = 1;
  state.policy.reservation.per_operation[static_cast<std::size_t>(ResourceKind::kQueuePair)] = 2;
  state.policy.maximum_cost_units = 12345;
  state.policy.weights[0] = 4242;
  state.fenced_boots = {WorkerBootId(0x2222), WorkerBootId(0x1111), WorkerBootId(0x2222)};
  TOS_CHECK_EQ(state.domains.size(), static_cast<std::size_t>(3));
  TOS_CHECK_EQ(state.attempts.size(), static_cast<std::size_t>(2));

  const std::vector<std::uint8_t> bytes = StateStore::encode(state);
  const Checked<PersistedState> decoded = StateStore::decode(bytes);
  TOS_REQUIRE(decoded.ok());
  const PersistedState& out = decoded.value;

  TOS_CHECK_EQ(out.generation.value(), static_cast<std::uint64_t>(9));
  TOS_CHECK_EQ(out.last_epoch.value(), state.last_epoch.value());
  TOS_CHECK_EQ(out.policy_generation.value(), state.policy_generation.value());
  TOS_CHECK_EQ(out.has_policy, true);
  TOS_CHECK(equal_policy(state.policy, out.policy));
  for (int i = 0; i < kCounterCount; ++i) {
    TOS_CHECK_EQ(out.counters[i], state.counters[i]);
  }

  const std::vector<ExecutionDomainRecord> expected_domains = sorted_domains(state.domains);
  TOS_REQUIRE(out.domains.size() == expected_domains.size());
  for (std::size_t i = 0; i < expected_domains.size(); ++i) {
    TOS_CHECK_MSG(equal_domain(expected_domains[i], out.domains[i]),
                  "domain record " + std::to_string(i) + " changed across the round trip");
  }
  TOS_CHECK_EQ(out.domains[0].capacity, expected_domains[0].capacity);
  TOS_CHECK_EQ(out.domains[2].capability.authoritative, false);

  // Volatile load/health/queue evidence is not durable at all: the input carried
  // current evidence and the decoded record must carry none of it.
  TOS_CHECK_MSG(expected_domains[0].load.published(),
                "fixture sanity: the encoded record must have carried live evidence");
  for (std::size_t i = 0; i < out.domains.size(); ++i) {
    TOS_CHECK_MSG(load_is_cleared(out.domains[i].load),
                  "volatile load evidence must not be restored from durable state");
  }

  const std::vector<ExecutionAttempt> expected_attempts = sorted_attempts(state.attempts);
  TOS_REQUIRE(out.attempts.size() == expected_attempts.size());
  for (std::size_t i = 0; i < expected_attempts.size(); ++i) {
    TOS_CHECK_MSG(equal_attempt(expected_attempts[i], out.attempts[i]),
                  "attempt record " + std::to_string(i) + " changed across the round trip");
  }
  TOS_CHECK_EQ(out.attempts[0].result.result_digest, expected_attempts[0].result.result_digest);
  TOS_CHECK(out.attempts[0].completion_committed);

  // Boot fences are canonical: sorted, deduplicated.
  TOS_REQUIRE(out.fenced_boots.size() == static_cast<std::size_t>(2));
  TOS_CHECK_EQ(out.fenced_boots[0].value(), static_cast<std::uint64_t>(0x1111));
  TOS_CHECK_EQ(out.fenced_boots[1].value(), static_cast<std::uint64_t>(0x2222));

  // Re-encoding what was decoded reproduces the identical byte stream.
  TOS_CHECK(StateStore::encode(out) == bytes);
  TOS_CHECK_EQ(StateStore::payload_crc(state), u32_at(bytes, 12));

  std::cout << "persistence round trip: " << bytes.size() << " bytes, " << out.domains.size()
            << " domain(s), " << out.attempts.size() << " attempt(s)" << std::endl;
}

// ---------------------------------------------------------------------------
// 2. Canonical bytes
// ---------------------------------------------------------------------------

TOS_TEST(persistence_encoding_is_canonical) {
  tos_test::TestRuntime runtime;
  TOS_REQUIRE(runtime
                  .add_domain(make_synthetic_domain(ExecutionDomainId(21), ExecutionDomainType::kDpu,
                                                    "dpu.canonical"))
                  .valid());
  TOS_REQUIRE(runtime
                  .add_domain(make_synthetic_domain(ExecutionDomainId(22), ExecutionDomainType::kNic,
                                                    "nic.canonical"))
                  .valid());
  TOS_REQUIRE(runtime
                  .add_domain(make_synthetic_domain(ExecutionDomainId(23),
                                                    ExecutionDomainType::kOtherRegisteredOffloadEngine,
                                                    "engine.canonical"))
                  .valid());
  TOS_REQUIRE(commit_one(runtime, 1024, 5).id.valid());
  TOS_REQUIRE(commit_one(runtime, 512, 6).id.valid());

  PersistedState state = state_from_runtime(runtime, 4);
  state.fenced_boots = {WorkerBootId(0x33), WorkerBootId(0x11), WorkerBootId(0x22)};

  const std::vector<std::uint8_t> first = StateStore::encode(state);
  const std::vector<std::uint8_t> second = StateStore::encode(state);
  TOS_CHECK_MSG(first == second, "encoding the same logical state twice must be byte-identical");

  PersistedState shuffled = state;
  std::reverse(shuffled.domains.begin(), shuffled.domains.end());
  std::reverse(shuffled.attempts.begin(), shuffled.attempts.end());
  std::reverse(shuffled.fenced_boots.begin(), shuffled.fenced_boots.end());
  TOS_CHECK(shuffled.domains.front().id != state.domains.front().id);
  const std::vector<std::uint8_t> third = StateStore::encode(shuffled);
  TOS_CHECK_MSG(third == first, "vector order must not change the canonical encoding");

  // Duplicate boot fences collapse to the same canonical bytes.
  PersistedState duplicated = shuffled;
  duplicated.fenced_boots.push_back(WorkerBootId(0x22));
  TOS_CHECK(StateStore::encode(duplicated) == first);

  TOS_CHECK_EQ(StateStore::payload_crc(state), u32_at(first, 12));
  TOS_CHECK_EQ(StateStore::payload_crc(shuffled), u32_at(third, 12));
  TOS_CHECK_EQ(u32_at(first, 8), static_cast<std::uint32_t>(first.size() - kFileHeaderBytes));

  std::cout << "persistence canonical encoding: " << first.size() << " bytes for "
            << state.domains.size() << " domain(s), " << state.attempts.size()
            << " attempt(s), " << state.fenced_boots.size() << " boot fence(s)" << std::endl;
}

// ---------------------------------------------------------------------------
// 3. Atomic replace
// ---------------------------------------------------------------------------

TOS_TEST(persistence_save_replaces_the_file_completely) {
  const std::string path = "tos_persistence_atomic.state";
  const std::string temporary = path + ".tmp";
  std::remove(path.c_str());
  std::remove(temporary.c_str());

  tos_test::TestRuntime runtime;
  TOS_REQUIRE(runtime
                  .add_domain(make_synthetic_domain(ExecutionDomainId(31), ExecutionDomainType::kDpu,
                                                    "dpu.atomic"))
                  .valid());
  TOS_REQUIRE(runtime
                  .add_domain(make_synthetic_domain(ExecutionDomainId(32), ExecutionDomainType::kSmartNic,
                                                    "smartnic.atomic"))
                  .valid());
  TOS_REQUIRE(commit_one(runtime, 4096, 8).id.valid());

  PersistedState larger = state_from_runtime(runtime, 3);
  larger.fenced_boots = {WorkerBootId(0xAA), WorkerBootId(0xBB)};
  const std::vector<std::uint8_t> larger_bytes = StateStore::encode(larger);

  const StateStore store(path);
  TOS_REQUIRE(store.save(larger).ok);
  TOS_CHECK(store.exists());
  TOS_CHECK_MSG(!std::filesystem::exists(temporary), "a successful save must leave no .tmp sibling");
  TOS_CHECK(read_file(path) == larger_bytes);
  TOS_CHECK_EQ(file_size_bytes(path), static_cast<std::uint64_t>(larger_bytes.size()));

  const Checked<PersistedState> loaded = store.load();
  TOS_REQUIRE(loaded.ok());
  TOS_CHECK(equal_state(larger, loaded.value));

  // A smaller state written over the larger file leaves no residue of the old one.
  PersistedState smaller;
  smaller.generation = PersistenceGeneration(4);
  smaller.last_epoch = CoordinatorEpoch(2);
  smaller.has_policy = false;
  smaller.domains = {runtime.backend().discover_domains().front()};
  const std::vector<std::uint8_t> smaller_bytes = StateStore::encode(smaller);
  TOS_REQUIRE(smaller_bytes.size() < larger_bytes.size());

  TOS_REQUIRE(store.save(smaller).ok);
  TOS_CHECK(read_file(path) == smaller_bytes);
  TOS_CHECK_EQ(file_size_bytes(path), static_cast<std::uint64_t>(smaller_bytes.size()));
  TOS_CHECK_MSG(!std::filesystem::exists(temporary), "a replacing save must leave no .tmp sibling");

  const Checked<PersistedState> reloaded = store.load();
  TOS_REQUIRE(reloaded.ok());
  TOS_CHECK(equal_state(smaller, reloaded.value));
  TOS_CHECK(reloaded.value.attempts.empty());

  TOS_REQUIRE(store.remove().ok);
  TOS_CHECK(!store.exists());
  TOS_CHECK(!store.load().ok());
}

// ---------------------------------------------------------------------------
// 4. Corruption matrix
// ---------------------------------------------------------------------------

TOS_TEST(persistence_corruption_matrix_is_rejected) {
  tos_test::TestRuntime runtime;
  const ExecutionDomainId dpu = runtime.add_domain(
      make_synthetic_domain(ExecutionDomainId(41), ExecutionDomainType::kDpu, "dpu.corrupt"));
  const ExecutionDomainId nic = runtime.add_domain(
      make_synthetic_domain(ExecutionDomainId(42), ExecutionDomainType::kNic, "nic.corrupt"));
  TOS_REQUIRE(dpu.valid() && nic.valid());
  TOS_REQUIRE(commit_one(runtime, 1024, 11).id.valid());
  TOS_REQUIRE(commit_one(runtime, 2048, 12).id.valid());

  PersistedState base = state_from_runtime(runtime, 6);
  base.policy = make_default_policy();
  base.has_policy = true;
  base.policy_generation = base.policy.generation;
  TOS_REQUIRE(base.attempts.size() == static_cast<std::size_t>(2));
  const std::vector<std::uint8_t> good = StateStore::encode(base);
  const std::size_t body_size = good.size() - kFileHeaderBytes;

  // Control: the unmodified bytes decode.
  TOS_CHECK(StateStore::decode(good).ok());

  auto expect_rejected = [](const std::string& label, const std::vector<std::uint8_t>& bytes,
                            const std::string& expected_code) {
    const Checked<PersistedState> decoded = StateStore::decode(bytes);
    if (decoded.ok()) {
      TOS_CHECK_MSG(false, label + ": corrupt state was accepted");
      return;
    }
    if (expected_code.empty()) {
      TOS_CHECK_MSG(decoded.status.code.rfind("state.", 0) == 0,
                    label + ": unexpected code " + decoded.status.code);
    } else {
      TOS_CHECK_MSG(decoded.status.code == expected_code,
                    label + ": code=" + decoded.status.code + " expected=" + expected_code);
    }
    TOS_CHECK_MSG(decoded.value.domains.empty() && decoded.value.attempts.empty() &&
                      decoded.value.fenced_boots.empty() && !decoded.value.generation.published() &&
                      !decoded.value.has_policy && decoded.value.counters[0] == 0ULL,
                  label + ": a rejected decode returned a partially applied state");
  };

  // -- empty buffer
  TOS_CHECK(!StateStore::decode(nullptr, 0).ok());
  TOS_CHECK_EQ(StateStore::decode(nullptr, 0).status.code, std::string("state.empty"));
  const std::string empty_code = decode_code(std::vector<std::uint8_t>{});
  TOS_CHECK_MSG(empty_code == "state.empty" || empty_code == "state.truncated_header",
                "empty buffer code=" + empty_code);

  // -- truncated header
  for (std::size_t length = 1; length < kFileHeaderBytes; ++length) {
    expect_rejected("truncated header " + std::to_string(length), slice(good, 0, length),
                    "state.truncated_header");
  }

  // -- truncated payload
  expect_rejected("payload cut short (stale header length)", slice(good, 0, good.size() - 1),
                  "state.length_mismatch");
  {
    // A payload cut short with a *consistent* header: the reader runs out of bytes.
    PersistedState no_boots = base;
    no_boots.fenced_boots.clear();
    const std::vector<std::uint8_t> canonical = StateStore::encode(no_boots);
    const std::vector<std::uint8_t> body = slice(canonical, kFileHeaderBytes, canonical.size());
    expect_rejected("payload truncated with a repaired header", seal_body(slice(body, 0, body.size() - 1)),
                    "state.truncated");
  }

  // -- corrupt magic
  {
    std::vector<std::uint8_t> bytes = good;
    bytes[0] = static_cast<std::uint8_t>('X');
    expect_rejected("corrupt magic", bytes, "state.bad_magic");
  }

  // -- wrong format version
  {
    std::vector<std::uint8_t> bytes = good;
    set_u16_at(bytes, 4, static_cast<std::uint16_t>(kPersistenceFormatVersion + 1));
    expect_rejected("unsupported version", bytes, "state.unsupported_version");
  }

  // -- wrong flags
  {
    std::vector<std::uint8_t> bytes = good;
    set_u16_at(bytes, 6, 1);
    expect_rejected("unsupported flags", bytes, "state.unsupported_flags");
  }

  // -- wrong declared length
  {
    std::vector<std::uint8_t> bytes = good;
    set_u32_at(bytes, 8, static_cast<std::uint32_t>(body_size + 1));
    expect_rejected("declared length mismatch", bytes, "state.length_mismatch");
  }

  // -- flipped payload byte
  {
    std::vector<std::uint8_t> bytes = good;
    bytes[kFileHeaderBytes + body_size / 2] =
        static_cast<std::uint8_t>(bytes[kFileHeaderBytes + body_size / 2] ^ 0x40U);
    expect_rejected("flipped payload byte", bytes, "state.integrity_failure");
  }

  // -- trailing extra bytes
  {
    std::vector<std::uint8_t> naive = good;
    naive.push_back(0x00);
    expect_rejected("trailing byte (stale header length)", naive, "state.length_mismatch");

    const std::vector<std::uint8_t> body = slice(good, kFileHeaderBytes, good.size());
    std::vector<std::uint8_t> extended = body;
    extended.push_back(0x7F);
    expect_rejected("trailing byte with a repaired header", seal_body(extended), "state.trailing_bytes");
  }

  // -- duplicate / unsorted domain ids
  {
    PersistedState duplicated = base;
    duplicated.domains = {base.domains.front(), base.domains.front()};
    expect_rejected("duplicate domain identity", StateStore::encode(duplicated),
                    "state.duplicate_or_unsorted_domain");

    const std::vector<std::uint8_t> low = domain_record_bytes(base.domains[0]);
    const std::vector<std::uint8_t> high = domain_record_bytes(base.domains[1]);
    // Sorted control: the assembled file is genuinely valid.
    TOS_CHECK(StateStore::decode(seal_body(assemble_body({low, high}, {}, {}))).ok());
    expect_rejected("unsorted domain identities",
                    seal_body(assemble_body({high, low}, {}, {})),
                    "state.duplicate_or_unsorted_domain");
  }

  // -- duplicate / unsorted attempt ids
  {
    const std::vector<std::uint8_t> first = attempt_record_bytes(base.attempts[0]);
    const std::vector<std::uint8_t> second = attempt_record_bytes(base.attempts[1]);
    TOS_CHECK(StateStore::decode(seal_body(assemble_body({}, {first, second}, {}))).ok());
    expect_rejected("unsorted attempt identities",
                    seal_body(assemble_body({}, {second, first}, {})),
                    "state.duplicate_or_unsorted_attempt");
    expect_rejected("duplicate attempt identity",
                    seal_body(assemble_body({}, {first, first}, {})),
                    "state.duplicate_or_unsorted_attempt");
  }

  // -- unsorted boot fences
  {
    TOS_CHECK(StateStore::decode(seal_body(assemble_body({}, {}, {0x11, 0x22}))).ok());
    expect_rejected("unsorted boot fences", seal_body(assemble_body({}, {}, {0x22, 0x11})),
                    "state.duplicate_or_unsorted_boot");
  }

  // -- impossible attempt lifecycles
  {
    ExecutionAttempt completed = base.attempts[0];
    completed.state = AttemptState::kCompleted;
    completed.completion_committed = false;
    expect_rejected("completed without a committed completion",
                    seal_body(assemble_body({}, {attempt_record_bytes(completed)}, {})),
                    "state.impossible_attempt_lifecycle");

    ExecutionAttempt contradictory = base.attempts[0];
    contradictory.state = AttemptState::kCompleted;
    contradictory.completion_committed = true;
    contradictory.result.success = false;
    expect_rejected("completed state contradicts the recorded result",
                    seal_body(assemble_body({}, {attempt_record_bytes(contradictory)}, {})),
                    "state.impossible_attempt_lifecycle");

    ExecutionAttempt failed_with_success = base.attempts[0];
    failed_with_success.state = AttemptState::kFailed;
    failed_with_success.completion_committed = true;
    failed_with_success.result.success = true;
    expect_rejected("failed state contradicts the recorded result",
                    seal_body(assemble_body({}, {attempt_record_bytes(failed_with_success)}, {})),
                    "state.impossible_attempt_lifecycle");
  }

  // -- in-flight attempts
  {
    ExecutionAttempt dispatched = base.attempts[0];
    dispatched.state = AttemptState::kDispatched;
    dispatched.completion_committed = false;
    expect_rejected("dispatched attempt persisted",
                    seal_body(assemble_body({}, {attempt_record_bytes(dispatched)}, {})),
                    "state.in_flight_attempt_persisted");

    ExecutionAttempt planned = base.attempts[0];
    planned.state = AttemptState::kPlanned;
    planned.completion_committed = false;
    expect_rejected("planned attempt persisted",
                    seal_body(assemble_body({}, {attempt_record_bytes(planned)}, {})),
                    "state.in_flight_attempt_persisted");

    ExecutionAttempt reserved = base.attempts[0];
    reserved.state = AttemptState::kReserved;
    reserved.completion_committed = false;
    expect_rejected("reserved attempt persisted",
                    seal_body(assemble_body({}, {attempt_record_bytes(reserved)}, {})),
                    "state.in_flight_attempt_persisted");

    ExecutionAttempt dispatching = base.attempts[0];
    dispatching.state = AttemptState::kDispatching;
    dispatching.completion_committed = false;
    dispatching.reservation.reset();
    expect_rejected("dispatching attempt without a reservation identity",
                    seal_body(assemble_body({}, {attempt_record_bytes(dispatching)}, {})),
                    "state.in_flight_attempt_persisted");

    dispatching.reservation = ReservationId(0x4242);
    expect_rejected("dispatching attempt with a reservation identity",
                    seal_body(assemble_body({}, {attempt_record_bytes(dispatching)}, {})),
                    "state.in_flight_attempt_persisted");
  }

  // -- missing generation
  {
    PersistedState no_generation = base;
    no_generation.generation = PersistenceGeneration{};
    expect_rejected("state without a persistence generation", StateStore::encode(no_generation),
                    "state.missing_generation");
  }

  std::cout << "persistence corruption matrix: base file " << good.size() << " bytes ("
            << base.domains.size() << " domains, " << base.attempts.size() << " attempts)"
            << std::endl;
}

// ---------------------------------------------------------------------------
// 5. Generation rollback
// ---------------------------------------------------------------------------

TOS_TEST(persistence_generation_rollback_is_refused) {
  const std::string path = "tos_persistence_rollback.state";
  std::remove(path.c_str());

  SyntheticBackend backend("rollback.backend");
  const ExecutionDomainId id(51);
  SyntheticDomainConfig config = make_synthetic_domain(id, ExecutionDomainType::kDpu, "dpu.rollback");
  TOS_REQUIRE(backend.add_domain(config).ok);
  TOS_REQUIRE(backend.set_domain_generation(id, ExecutionDomainGeneration(5)).ok);

  SchedulerOptions options;
  options.policy = make_default_policy();
  options.host_node = "test.host";
  Scheduler scheduler(std::move(options));
  TOS_REQUIRE(scheduler.start().ok);

  const std::vector<ExecutionDomainRecord> discovered = backend.discover_domains();
  TOS_REQUIRE(discovered.size() == static_cast<std::size_t>(1));
  TOS_REQUIRE(scheduler.register_domain(discovered.front()).ok);
  ExecutionDomainRecord live;
  TOS_REQUIRE(scheduler.domains().get(id, live));
  TOS_CHECK_EQ(live.generation.value(), static_cast<std::uint64_t>(5));

  ExecutionDomainRecord durable = discovered.front();
  durable.generation = ExecutionDomainGeneration(3);
  PersistedState state;
  state.generation = PersistenceGeneration(2);
  state.last_epoch = CoordinatorEpoch(1);
  state.has_policy = false;
  state.domains = {durable};
  const StateStore store(path);
  TOS_REQUIRE(store.save(state).ok);

  const Status refused = scheduler.load_state_from(path);
  TOS_CHECK(!refused.ok);
  TOS_CHECK_EQ(refused.code, std::string("state.generation_rollback"));
  TOS_CHECK_MSG(refused.message.find("generation") != std::string::npos,
                "rollback rejection must name the generation, got: " + refused.message);

  // The rollback was refused before any mutation.
  ExecutionDomainRecord after;
  TOS_REQUIRE(scheduler.domains().get(id, after));
  TOS_CHECK_EQ(after.generation.value(), static_cast<std::uint64_t>(5));
  TOS_CHECK_EQ(scheduler.domains().size(), static_cast<std::size_t>(1));
  TOS_CHECK_EQ(scheduler.attempts().audit().total, static_cast<std::uint64_t>(0));
  TOS_CHECK_EQ(scheduler.coordinator_epoch().value(), static_cast<std::uint64_t>(1));
  TOS_CHECK_EQ(scheduler.reconcile().domains_compared, static_cast<std::uint64_t>(0));

  // The same file at an equal generation is accepted, proving the guard is about
  // rollback and not about the file itself.
  PersistedState equal = state;
  equal.domains = {live};
  equal.generation = PersistenceGeneration(3);
  TOS_REQUIRE(store.save(equal).ok);
  TOS_CHECK(scheduler.load_state_from(path).ok);
  TOS_CHECK_EQ(scheduler.reconcile().domains_compared, static_cast<std::uint64_t>(1));

  TOS_REQUIRE(scheduler.shutdown().ok);
  TOS_REQUIRE(store.remove().ok);
}

// ---------------------------------------------------------------------------
// 6. Interrupted replacement
// ---------------------------------------------------------------------------

TOS_TEST(persistence_interrupted_replacement_leaves_the_runtime_unchanged) {
  const std::string path = "tos_persistence_interrupted.state";
  std::remove(path.c_str());

  tos_test::TestRuntime runtime;
  TOS_REQUIRE(runtime
                  .add_domain(make_synthetic_domain(ExecutionDomainId(61), ExecutionDomainType::kDpu,
                                                    "dpu.interrupted"))
                  .valid());
  TOS_REQUIRE(runtime
                  .add_domain(make_synthetic_domain(ExecutionDomainId(62), ExecutionDomainType::kNic,
                                                    "nic.interrupted"))
                  .valid());
  TOS_REQUIRE(commit_one(runtime, 1024, 13).id.valid());

  const PersistedState valid = state_from_runtime(runtime, 5);
  const StateStore store(path);
  TOS_REQUIRE(store.save(valid).ok);

  SchedulerOptions options;
  options.policy = make_default_policy();
  options.host_node = "test.host";
  Scheduler scheduler(std::move(options));
  TOS_REQUIRE(scheduler.start().ok);
  TOS_REQUIRE(scheduler.load_state_from(path).ok);

  const SchedulerSnapshot before = scheduler.snapshot();
  TOS_CHECK_EQ(before.totals.domains, static_cast<std::uint64_t>(valid.domains.size()));
  TOS_CHECK_EQ(before.totals.completed_attempts, static_cast<std::uint64_t>(valid.attempts.size()));

  // An interrupted replacement: the file keeps its header but loses its payload tail.
  const std::vector<std::uint8_t> encoded = StateStore::encode(valid);
  TOS_REQUIRE(write_file_atomic(path, slice(encoded, 0, encoded.size() - 8)).ok);

  const Status refused = scheduler.load_state_from(path);
  TOS_CHECK(!refused.ok);
  TOS_CHECK_EQ(refused.code, std::string("state.length_mismatch"));

  const SchedulerSnapshot after = scheduler.snapshot();
  TOS_CHECK_MSG(equal_totals(before.totals, after.totals),
                "a refused load must not change a single runtime total");
  TOS_CHECK_EQ(after.domains.size(), before.domains.size());
  TOS_CHECK_EQ(after.attempts.size(), before.attempts.size());
  TOS_CHECK_EQ(after.mutation_sequence, before.mutation_sequence);
  TOS_CHECK_EQ(after.coordinator_epoch.value(), before.coordinator_epoch.value());
  for (std::size_t i = 0; i < before.domains.size(); ++i) {
    TOS_CHECK(after.domains[i].id == before.domains[i].id);
    TOS_CHECK_EQ(after.domains[i].generation.value(), before.domains[i].generation.value());
    TOS_CHECK(after.domains[i].worker_boot == before.domains[i].worker_boot);
    TOS_CHECK_EQ(after.domains[i].load.published(), before.domains[i].load.published());
  }

  // A later, intact save is still readable: the store was not left wedged.
  TOS_REQUIRE(store.save(valid).ok);
  TOS_CHECK(store.load().ok());

  TOS_REQUIRE(scheduler.shutdown().ok);
  TOS_REQUIRE(store.remove().ok);
}

// ---------------------------------------------------------------------------
// 7. Path safety
// ---------------------------------------------------------------------------

TOS_TEST(persistence_path_safety_and_missing_files) {
  PersistedState state;
  state.generation = PersistenceGeneration(1);
  state.has_policy = false;

  const std::string escaped = ".." + std::string(1, '\\') + "tos_persistence_escape.state";
  {
    const StateStore traversal(escaped);
    const Status saved = traversal.save(state);
    TOS_CHECK(!saved.ok);
    TOS_CHECK_EQ(saved.code, std::string("state.unsafe_path"));
    TOS_CHECK(!traversal.exists());
    TOS_CHECK(!std::filesystem::exists(escaped));
  }
  {
    const StateStore nested("sub/../tos_persistence_escape.state");
    const Status saved = nested.save(state);
    TOS_CHECK(!saved.ok);
    TOS_CHECK_EQ(saved.code, std::string("state.unsafe_path"));
    TOS_CHECK(!nested.exists());
  }
  {
    const StateStore unnamed("");
    TOS_CHECK_EQ(unnamed.save(state).code, std::string("state.no_path"));
    const Checked<PersistedState> loaded = unnamed.load();
    TOS_CHECK(!loaded.ok());
    TOS_CHECK_EQ(loaded.status.code, std::string("state.no_path"));
  }

  const std::string missing = "tos_persistence_absent.state";
  std::remove(missing.c_str());
  {
    const StateStore absent(missing);
    TOS_CHECK(!absent.exists());
    const Checked<PersistedState> loaded = absent.load();
    TOS_CHECK(!loaded.ok());
    TOS_CHECK_MSG(loaded.status.code == "file.missing" || loaded.status.code == "state.missing",
                  "missing file code=" + loaded.status.code);
  }

  SchedulerOptions options;
  options.policy = make_default_policy();
  options.host_node = "test.host";
  Scheduler scheduler(std::move(options));
  TOS_REQUIRE(scheduler.start().ok);
  const Status loaded = scheduler.load_state_from(missing);
  TOS_CHECK(!loaded.ok);
  TOS_CHECK_EQ(loaded.code, std::string("state.missing"));
  TOS_CHECK(!scheduler.load_state_from("..").ok);
  TOS_CHECK_EQ(scheduler.load_state_from("..").code, std::string("state.unsafe_path"));
  TOS_REQUIRE(scheduler.shutdown().ok);
}
