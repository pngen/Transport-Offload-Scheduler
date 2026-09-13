# Transport Offload Scheduler

Vendor-neutral C++20 runtime that decides where a transport-path operation executes, and refuses to
execute it anywhere once that decision stops being legal.

- Version: 1.0.0 (`include/tos/version.hpp`)
- Wire protocol version: 1, persistence format version: 1
- License: Apache License 2.0, Copyright 2026 Summon Software Labs
- No telemetry, no analytics, no network reporting of any kind

## What Transport Offload Scheduler is

Transport Offload Scheduler (TOS) is a scheduling runtime for one narrow but load-bearing decision.
An execution domain - host CPU, accelerator, NIC, SmartNIC, DPU or another explicitly registered
offload engine - is modelled as a generation-bound record of *published evidence*: a capability
record, volatile load/queue/health evidence, locality, topology, compatibility, isolation and
capacity, each carried with its own generation counter and its owning worker incarnation. A caller
submits an `OperationRequest` that describes what must be done (operation class, payload shape,
memory domains, locality, isolation, compatibility, economics, retry permission, side-effect class)
and names no device. The runtime evaluates every registered domain against hard eligibility rules,
ranks the survivors with a named-factor integer-exact total order, records a plan bound to the exact
generations that made it legal, reserves scarce engine capacity, revalidates the binding against live
authority at the dispatch boundary, registers the attempt *before* any external call is made, and
admits at most one authoritative completion per attempt generation.

**The core systems question.** For a transport-path operation that must run now, which execution
domain (CPU, accelerator, NIC, SmartNIC, DPU or another registered offload engine) is the
deterministic legal place to execute it under capability, locality, topology, queue/load, cost,
isolation, compatibility, policy, health and generation-bound authority - and what deterministic
outcome follows when that domain becomes stale, unavailable, invalid, overloaded, incompatible or
unauthorized before dispatch or before completion? The first half is answered by hard eligibility
followed by deterministic ranking; the second half is answered by revalidation, by completion
authority and by the explicit outcome codes documented below. Neither half is answered by a guess:
a domain that cannot *prove* it may run the operation is not eligible, and a plan whose bound
generations moved is rejected rather than reinterpreted.

## Systems boundary

**What it owns.**

- The decision: which registered execution domain is the deterministic legal place to run one
  transport-path operation, and the structured explanation of that decision.
- Hard eligibility, capability checking, isolation/compatibility/locality gating and policy gating.
- Deterministic ranking of eligible candidates with a named factor vector and integer-exact scores.
- Plans of record, their authority binding, and their invalidation when the binding moves.
- Capacity reservation accounting per domain and per resource kind (`ReservationLedger`).
- The attempt ledger and completion authority: at most one authoritative completion per attempt
  generation, with stale, conflicting, duplicated and impossible submissions rejected by name.
- Worker-incarnation fencing (`WorkerAuthority`) and coordinator-epoch checking.
- Durable state: what may survive a restart, and what must be re-proved afterwards.
- The protocol between coordinator, workers and clients, and the coordinator's dispatch channel.

**What it does not own.**

- Path computation and route selection.
- Packet routing, forwarding or the data plane itself.
- Topology ownership or discovery of the fabric beyond what a domain publishes as evidence.
- RDMA buffer registration, memory registration, or queue-pair creation (`kRdmaMessage` and
  `CapabilityFlag::kRdma` exist as model vocabulary; no RDMA execution path exists in this build).
- GPU or accelerator memory allocation.
- General job scheduling: TOS schedules transport-path operations, not arbitrary processes or jobs.
- Congestion control.
- Vendor driver internals.
- Firmware management.

The runtime never dereferences a payload handle: `PayloadDescriptor` carries opaque addressable
handles, and the scheduler's public API never resolves them.

## Architectural doctrine

These rules are the reason the runtime exists. Each is enforced by code cited in
`docs/ARCHITECTURE.md`.

1. **Capability is not authority.** A capability record is proof that an engine *can* technically
   perform an operation class (`include/tos/core/capability.hpp`). Whether it *may* run this
   attempt now is decided separately by policy, health, load, generation freshness and reservation
   state.
2. **Availability is not authority.** A domain that is registered, reachable and healthy is still
   not authorized unless its worker incarnation is current and its bound generations still match.
3. **UNKNOWN never silently becomes ELIGIBLE.** With `require_positive_evidence` set (the default in
   `make_default_policy()`), a non-authoritative capability record, or a capability flag published as
   unproven, is rejected with `IneligibilityReason::kCapabilityUnproven`. `CapabilitySet::canonicalize()`
   clears any flag that also appears in `unproven_flags`, so an unproven property can never be
   claimed as proven by construction.
4. **A device that supported an operation earlier does not stay eligible after its capability
   generation changes.** Every plan binds the capability generation it was decided under; any change
   makes the binding stale and the dispatch is refused (`authority.stale`).
5. **A reachable worker whose process incarnation changed cannot publish completion for a prior
   attempt.** Completions are bound to `worker_boot`; a submission from a different incarnation is
   rejected with `CompletionRejection::kStaleWorkerBoot`, and fencing an incarnation invalidates its
   domains and classifies its in-flight attempts.
6. **A persisted domain record does not become current after restart.** Recovery restores durable
   structure only: dynamic (queue/load/health) evidence is cleared and every recovered worker boot is
   fenced, so the worker must register again before any of its domains can be eligible.

## Execution-domain model

`ExecutionDomainType` (`include/tos/core/enums.hpp`) has exactly six values: `kCpu`, `kAccelerator`,
`kNic`, `kSmartNic`, `kDpu`, `kOtherRegisteredOffloadEngine`. The generic model carries no vendor
assumption; vendor facts travel in capability and compatibility evidence.

An `ExecutionDomainRecord` (`include/tos/core/domain.hpp`) contains: identity and generation, type,
a bounded stable name, parent device and parent host, owning `WorkerId`/`WorkerBootId`, provenance,
a registration sequence used as a deterministic tie-break input, the `CapabilityRecord`, volatile
`DomainLoadEvidence`, `DomainLocality`, `DomainTopology`, `DomainCompatibility`, an `IsolationClass`,
a per-resource-kind `CapacityVector`, a `BackendGeneration`, a `PublisherSequence` watermark per
publisher incarnation, and the fence flag and reason.

Volatile evidence is explicitly *not* authoritative after a restart: `DomainLoadEvidence` carries
load, queue and health generations plus an evidence generation, and
`ExecutionDomainRecord::dynamic_evidence_current(live_boot)` is true only when that evidence was
published by the live worker boot and the domain is not fenced.

Capacity is expressed per `ResourceKind`: `kExecutionSlot`, `kQueueDepth`, `kDescriptorRing`,
`kProcessingContext`, `kOffloadEngine`, `kDeviceMemoryBytes`, `kScratchMemoryBytes`, `kDmaChannel`,
`kQueuePair`, `kVendorExecutionContext`.

## Operation model

An operation class is a registered, bounded, named descriptor (`OperationClassDescriptor`): a
canonical upper-case token, a category, a `SideEffectClass`, a `TransportClass`, a `PayloadClass` and
a required `CapabilityFlag`. Nine classes are built in and are stable across releases
(`src/core/operation.cpp`):

| Id | Name | Category | Side-effect class | Required flag |
|----|------|----------|-------------------|---------------|
| 1 | `CHECKSUM_CRC32C` | integrity | `kPure` | `kChecksum` |
| 2 | `COMPRESS_RLE` | transform | `kPure` | `kCompression` |
| 3 | `DECOMPRESS_RLE` | transform | `kPure` | `kDecompression` |
| 4 | `COPY_STAGE` | staging | `kIdempotent` | `kStagingCopy` |
| 5 | `SEGMENT_SPLIT` | framing | `kPure` | `kSegmentation` |
| 6 | `REASSEMBLE_JOIN` | framing | `kPure` | `kReassembly` |
| 7 | `VALIDATE_INTEGRITY` | integrity | `kPure` | `kIntegrityVerify` |
| 8 | `SIDE_EFFECT_EMIT` | telemetry | `kNonRepeatable` | `kSideEffectSink` |
| 9 | `NOOP_PROBE` | probe | `kPure` | none |

Additional classes may be registered by the embedding application under `kMaxOperationClasses`
(128) with canonical validated names.

`OperationRequest` carries the class, a `PayloadDescriptor` (size, source/destination memory domain,
opaque handles, alignment, segment count, payload and transport class, scatter/gather flag), allowed
and forbidden domain classes and domain identities, required capability flags, required isolation,
DMA and scatter/gather requirements, `LocalityConstraints`, `CompatibilityRequirements`, optional
`ExecutionEconomics`, an optional latency SLO, `RetryPermission`, a fallback flag, a side-effect
override and a provenance hint.

Replay is never assumed safe. `SideEffectClass` is `kPure`, `kIdempotent`, `kAtMostOnceRequired`,
`kNonRepeatable` or `kUnknown`; `effective_side_effect()` takes the more conservative of the
registered class and the caller's override, `kUnknown` is treated as *not* replay-safe, and
`is_replay_safe()` is true only for `kPure` and `kIdempotent`.

`validate_request()` rejects unknown classes, payloads above `kMaxPayloadBytes` (1 TiB),
non-power-of-two alignment, impossible segment counts, scatter/gather without segments,
contradictory domain filters, invalid retry bounds, oversized filters and a cost ceiling below the
setup cost.

## Capability model

`CapabilitySet` records proven properties: a canonical (sorted, unique) operation list, `flags`
proven present, `unproven_flags` explicitly unproven, addressable memory-domain bits, minimum and
maximum payload bytes, alignment, maximum concurrency, queue capacity, transport and payload class
bits, protocol/driver/firmware/architecture versions and a bounded `backend_family`.

`CapabilityRecord` binds that set to a domain, a `CapabilityGeneration`, an `EvidenceGeneration`, the
publishing `WorkerBootId`, a `Provenance` and an `authoritative` flag; `published()` requires a valid
domain, a published generation and evidence generation, and authority.

`check_capability()` fails closed: an UNSUPPORTED provenance, a non-authoritative record when
positive evidence is required, an unsupported operation class, a missing required flag, an
addressability gap, a payload outside the declared range, insufficient alignment, or an unsupported
transport/payload class each produce a named `IneligibilityReason`.

## Authority binding

Planning does not grant permission to execute. `AuthorityBinding` (`include/tos/core/authority.hpp`)
records every identity and generation that participated in a decision:

- `coordinator_epoch`
- `worker`, `worker_boot`
- `domain`, `domain_generation`
- `capability_generation`, `backend_generation`
- `topology_generation`, `locality_generation`
- `health_generation`, `queue_generation`, `load_generation`
- `policy_generation`, `compatibility_generation`, `isolation_generation`, `evidence_generation`
- `operation`, `attempt`, `attempt_generation`
- `reservation`

At dispatch the binding is compared against `LiveAuthority` extracted from the current domain record
and the live policy (`live_authority_of()` / `validate_authority()`). A missing domain yields
`authority.domain_absent`; a fenced domain yields `authority.domain_fenced`; any identity or
generation mismatch or an unpublished generation yields `authority.stale` with a per-field
`AuthorityMismatch` list naming the field, the bound value and the observed value. Which generation
families participate is governed by `FreshnessRequirements` (capability, health, compatibility and
evidence are required by default; queue, load, topology, locality and isolation are not).

`WorkerAuthority` is the fenced worker-boot registry: `register_boot`, `is_current`, `is_fenced`,
`fenced_boot_id`, `fence`, `fence_boot`, `current_boot`. A dead incarnation's boot identity is never
reused, a new incarnation for the same worker fences the previous one, and the fence set is bounded.

## Eligibility, then deterministic ranking

**Hard eligibility** (`Scheduler::Impl::evaluate` in `src/core/scheduler.cpp`) is decided before
ranking is consulted. In order: fenced domain (`kDomainFenced`); worker incarnation not current
(`kWorkerBootStale`); policy-forbidden domain class or identity (`kPolicyForbidden`);
`kOffloadRequired` with a CPU candidate or `kHostRequired` with an offload candidate
(`kPolicyForbidden`); forbidden provenance (`kProvenanceDisallowed`); UNSUPPORTED provenance when
policy does not allow it (`kBackendUnsupported`); domain not in the allow list or in the forbidden
list (`kDomainNotAllowed`); unregistered operation class (`kUnknownOperationClass`); capability
(`check_capability`); insufficient isolation (`kIsolationInsufficient`); compatibility mismatch on
driver/backend version, firmware generation, protocol version, accelerator architecture or backend
family (`kCompatibilityMismatch`); locality below the required class, wrong NUMA node, no local NIC,
or not on one host (`kLocalityViolation`); missing or unhealthy load evidence (`kHealthNotReady`);
declared capacity exhausted (`kCapacityUnavailable`).

**Ranking** produces a `FactorVector` of 22 named factors (`enum class RankingFactor`,
`include/tos/core/ranking.hpp`), each normalized to `[0, 1'000'000]` where a larger value always means
"more desirable" - including cost factors, which are published as their complement:

`kDataLocality`, `kMemoryLocality`, `kNicLocality`, `kAcceleratorLocality`, `kPcieAffinity`,
`kAvoidedHostCopies`, `kTransferCost`, `kSetupCost`, `kQueueDelay`, `kExecutionThroughput`,
`kCompletionLatency`, `kOffloadOverhead`, `kCpuPreservation`, `kAcceleratorPreservation`,
`kPowerEfficiency`, `kUtilizationHeadroom`, `kCongestionAvoidance`, `kFailureDomainDiversity`,
`kIsolationQuality`, `kReconfigurationPenalty`, `kFallbackCost`, `kPolicyPreference`.

Factors are a pure function of their inputs: identical inputs always produce identical output
(`compute_factors()`), using only integer arithmetic and `normalize_lower_is_better()` /
`normalize_higher_is_better()`.

The score is integer-exact: `weighted_score()` sums `weight * factor` over factors with a positive
weight and divides by the total weight using 64-bit integer division - no floating point anywhere in
the ranking path. `candidate_better()` then defines a **total order**:

1. higher `weighted_score`;
2. then factor-by-factor in `RankingFactor` enum order, higher value wins;
3. then lower `domain_type` ordinal;
4. then lower `domain_id`.

Because step 4 is a unique identity, the order is total and the result is reproducible for a given
state and request. Ranks are assigned 1..N in that order, and the winner is rank 1.

## Planning, reservation, dispatch, completion authority, retry and fallback

**Planning.** `Scheduler::plan()` evaluates eligibility, ranks, and stores a plan of record
(`PlanState::kCreated`) bound to the winning domain's generations. The optional payload is copied
into the plan under `SchedulerLimits::max_payload_dispatch_bytes` (512 KiB by default). Planning
consumes no capacity and grants no execution authority. A `DecisionExplanation` accompanies every
plan: outcome, selected domain and provenance, candidates with their named rejection reasons, the
full ranking with factor vectors, reason codes (`selected.<type>`, `fallback.from.<type>`,
`rejected.<reason>`, `no_domains_registered`, ...), evaluated/rejected counts, snapshot generation
and policy generation.

`SelectionOutcome` values: `kOffloadSelected`, `kCpuSelected`, `kAcceleratorSelected`,
`kNicSelected`, `kSmartNicSelected`, `kDpuSelected`, `kFallbackSelected`, `kDeferred`,
`kNoEligibleDomain`, `kPolicyRejected`, `kCapabilityUnsupported`, `kStaleEvidence`,
`kRevalidationRequired`, `kCapacityUnavailable`, `kCompatibilityRejected`, `kIsolationRejected`,
plus the scheduler-boundary outcomes `kDispatched`, `kFailed`, `kOutcomeUnknown`, `kCancelled`.

**Reservation.** When `ReservationPolicy::enabled` is set, `Scheduler::reserve()` acquires the
configured per-operation amounts all-or-nothing from the `ReservationLedger`. `ReservationState`
evolves `kActive` -> `kCommitted` (bound to an authoritative dispatch) -> `kReleased`, or to
`kRolledBack` (never became authoritative) or `kInvalidated` (domain authority withdrawn while
held). The ledger fails closed rather than overcommitting: `set_capacity()` refuses a total below
what is already reserved or committed, and `commit()` rejects stale domain, capability or policy
generations with `reservation.stale_domain_generation`, `reservation.stale_capability_generation`
and `reservation.stale_policy_generation`. `ReservationLedger::audit()` recomputes every accounting
row from the reservation records and reports `consistent` plus a `leaked` count (held but never
dispatched).

**Dispatch.** `Scheduler::dispatch()` refuses a plan when the runtime is shutting down
(`DispatchRejection::kShutdownInProgress`), when the plan was already consumed, dispatched, cancelled
or rejected (`kPlanConsumed`), when a required reservation is missing (`kReservationInvalid`), or
when revalidation fails (`kAuthorityStale`, which also rolls the reservation back and sets
`SelectionOutcome::kRevalidationRequired`). The policy is read *again* after the pre-revalidation
hook, so a policy change that lands while a dispatch is in flight invalidates the plan instead of
being ignored. Otherwise it commits the reservation, allocates the dispatch identity,
**registers the attempt before calling the channel** (`kDispatching`), hands a
`DispatchEnvelope` to the channel, and only then marks the attempt `kDispatched`. A completion that
arrives before `mark_dispatched` returns is accepted - `attempt.not_dispatching` is tolerated
precisely because the attempt already existed. Channel absence yields
`DispatchRejection::kTransportFailure` with code `channel.absent`; a refused send fails the attempt
and releases the reservation.

`DispatchRejection` values: `kNone`, `kPlanNotFound`, `kPlanConsumed`, `kAuthorityStale`,
`kReservationInvalid`, `kShutdownInProgress`, `kAttemptRegistrationFailed`, `kTransportFailure`,
`kCancelled`, `kCapacityExhausted`.

**Completion authority.** `AttemptLedger::commit_completion()` validates every binding in order -
attempt generation, dispatch identity, worker identity and incarnation, coordinator epoch, domain
generation, capability generation, operation identity, provenance - and commits at most once. A
replay of an identical committed result is `kDuplicateIdentical` and reported as idempotent; a
different result for the same attempt is `kConflictingDuplicate` and is rejected without mutation.
`CompletionRejection` values: `kAccepted`, `kDuplicateIdentical`, `kConflictingDuplicate`,
`kUnknownAttempt`, `kStaleWorkerBoot`, `kStaleCoordinatorEpoch`, `kStaleDomainGeneration`,
`kAttemptGenerationMismatch`, `kDispatchIdMismatch`, `kOperationMismatch`, `kNotDispatched`,
`kAlreadyTerminal`, `kIntegrityMismatch`, `kDomainFenced`, `kResultTooLarge`,
`kProvenanceMismatch`, `kStaleCapabilityGeneration`. `Scheduler::complete()` additionally rejects
any submission whose coordinator epoch is not the live one *before* touching state. On a committed,
non-idempotent completion the reservation is released and the plan is erased.

**Retry.** `Scheduler::retry()` re-plans a new attempt generation on the *same* domain class as the
failed attempt. It refuses when retry is disabled (`kPolicyDisallowsRetry`), when the attempt is not
terminal (`kFailureNotRetryable`), when the outcome was ambiguous - `AttemptResolution::kAmbiguous`,
`kFencedMayHaveEffect` or `kCancelledAmbiguous` (`kAmbiguousOutcome`: an operation that may already
have executed is never replayed automatically), when the side-effect class is not replay-safe
(`kNonRepeatableOperation`), when the failure kind is not retryable under policy, or when the attempt
bound is reached (`kAttemptLimitReached`). `RetryRejection` also includes `kDomainUnavailable`,
`kOperationNotRetryable` and `kUnknownSideEffectClass`. A failure report deliberately leaves the
plan of record in place so retry can re-run eligibility from the request the plan carries; the plan
is retired when the operation is superseded, cancelled or completed, which bounds the map. Retiring a
plan means a stale caller copy can never be dispatched.

**Fallback.** `Scheduler::fallback()` walks the policy's ordered per-source chain for the failed
domain class, skipping the source class itself, classes already tried, and the host when
`allow_host_fallback` is false. Each candidate is planned through the same eligibility and ranking
path with `restrict_types` set, so a fallback target must be independently eligible; the result is a
new plan of record with a fresh attempt generation. `FallbackRejection` values: `kNone`,
`kNoFallbackRegistered`, `kFallbackDepthExceeded`, `kHostFallbackForbidden`,
`kFallbackTargetIneligible`, `kFallbackCycleDetected`, `kPolicyDisallowsFallback`,
`kOriginalAttemptStillAuthoritative`. Fallback graphs are validated for cycles both when the policy
is installed and again when fallback runs.

**Ambiguous completion.** Ambiguity is a first-class, conservative outcome, never promoted to
success. `AttemptState::kOutcomeUnknown` with `AttemptResolution::kAmbiguous` is recorded when a
worker dies mid-flight, when the scheduler shuts down with work in flight, or when a backend reports
that the effect may or may not have happened (`ExecutionOutcome::unknown()`). Cancellation of an
attempt that crossed the dispatch boundary records `AttemptResolution::kCancelledAmbiguous`; fencing
records `kFencedMayHaveEffect` when the effect may have landed and `kFencedNoEffect` otherwise. An
ambiguous attempt rejects any later completion as `kAlreadyTerminal`, and retry refuses it.

## Distributed architecture

Real OS processes, real TCP sockets, a versioned framed protocol.

- **Coordinator** (`tos::dist::OffloadCoordinator`) hosts a `Scheduler`, listens on a bound host/port
  (port 0 selects an ephemeral port), accepts bounded sessions (`max_sessions`, default 32), runs one
  thread per session plus one accept thread, and installs a `RemoteChannel` as the scheduler's
  dispatch channel. Stopping closes the listener, sends a shutdown request to every registered
  worker session, closes the sessions, joins the threads and stops the scheduler.
- **Worker** (`tos::dist::OffloadWorker`) is a real process that owns a backend, registers a
  `WorkerId`/`WorkerBootId`, publishes its domains (record + capability + load), serves frames until
  stopped, and is the only component that talks to an execution backend.
- **Client** (`tos::dist::CoordinatorClient`) performs a hello handshake and a strict
  one-request/one-response exchange, and backs the `tos_inspect` remote commands.

**Framing.** Every frame is bounded and validated before use: 4-byte magic `TOS1`, `u16` protocol
version, `u16` type, `u16` flags, `u16` reserved, `u32` length and `u32` CRC32C over the payload, in
a 20-byte little-endian header. Maximum frame 1 MiB, maximum protocol payload 512 KiB. Wrong magic,
unsupported version, unknown type, oversized length, truncation, trailing bytes, malformed payload,
unsupported flags and integrity failure are each rejected with a named `DecodeError`. The worker
sets `TCP_NODELAY` on its socket.

**Message set** (`enum class MessageType`, values 1..61): `kHello` (1), `kHelloAck` (2), `kError` (3),
`kGoodbye` (4), `kDomainPublish` (10), `kCapabilityPublish` (11), `kLoadUpdate` (12), `kHeartbeat`
(13), `kDispatchRequest` (20), `kDispatchAccepted` (21), `kDispatchRejected` (22), `kCompletion`
(23), `kFailureReport` (24), `kCancelRequest` (25), `kCancelResult` (26), `kCompletionAck` (27),
`kSubmitRequest` (30), `kSubmitResponse` (31), `kQueryRequest` (40), `kQueryResponse` (41),
`kAdminRequest` (50), `kAdminResponse` (51), `kShutdownRequest` (60), `kFence` (61).

**Worker-incarnation fencing.** A worker's identity is the pair (`WorkerId`, `WorkerBootId`). The
coordinator accepts a worker only if `register_boot` succeeds; every subsequent frame must carry the
registered incarnation or it is rejected with `session.identity_mismatch`. When a worker session
closes, the coordinator fences that boot (`scheduler.fence_worker_boot`), which fences the worker
authority record, fences every domain owned by that incarnation, invalidates its reservations,
classifies its in-flight attempts as ambiguous when they may have taken effect, and logs the count of
affected domains. A fenced boot identity can never be registered again.

**Coordinator epoch.** Each `Scheduler::start()` publishes a new `CoordinatorEpoch` strictly greater
than the previous one (and greater than any recovered epoch). Plans bind the epoch they were made
under, completions must carry it, heartbeats naming another epoch close the session, and
`Scheduler::complete()` rejects a foreign epoch before touching state. A recovered runtime is
therefore a new incarnation that cannot accept traffic from the old one.

## Persistence and recovery

Persistence is opt-in (`SchedulerOptions::enable_persistence`, `state_path`). The store
(`tos::StateStore`) writes a magic `TOSP` header, a `u16` format version, a `u16` flags word, a `u32`
body length and a `u32` CRC32C over the body, followed by the body. Writes are atomic: a temporary
sibling file is written and flushed, then renamed over the destination, so an interrupted save can
never leave a partially valid file. Decoding validates magic, version, flags, length, integrity,
bounds (64 MiB, 65536 domains, 200000 attempts), canonical ordering, duplicate identities,
impossible attempt lifecycles and in-flight states *before* returning any value; an in-flight
attempt persisted in the file is refused with `state.in_flight_attempt_persisted`.

What is persisted: the persistence generation, the last coordinator epoch, the policy and its
generation, durable domain records, terminal attempt records, fenced boot identities and the
identity counters (operation, plan sequence, dispatch, snapshot, domain). Volatile queue, load and
health evidence is **not** authoritative across a restart.

Recovery (`Scheduler::load_state_from`) is two-phase. Phase one validates the entire durable state
against the live runtime - a durable domain generation below the live one is
`state.generation_rollback`, a durable attempt identity already present is
`attempt.duplicate_identity` - and a refused load leaves the runtime unchanged. Phase two applies:
attempts are restored (in-flight states are rejected), identity counters are adopted so recovered
identities are never reallocated, domains are upserted with generation-regression tolerance, all
dynamic evidence is marked stale (`mark_dynamic_evidence_stale`), and every recovered worker boot
identity is fenced with "recovered boot identity requires re-registration". In-flight work is never
persisted as authoritative: `save_state_to()` rewrites non-terminal attempts as
`kOutcomeUnknown`/`kAmbiguous` when they may have taken effect and `kFenced`/`kFencedNoEffect`
otherwise.

## Reconciliation

`Scheduler::reconcile()` compares the durable domain records recovered at startup with the live
registry and produces a `ReconciliationReport` with one `ReconciliationItem` per discrepancy, plus a
classification pass over every live attempt. `DiscrepancyKind` values: `kDomainMissing`,
`kDomainGenerationChanged`, `kWorkerReplaced`, `kCapabilityChanged`,
`kOperationNoLongerSupported`, `kQueueSupportChanged`, `kBackendVersionChanged`,
`kFirmwareGenerationChanged`, `kCompletedAttemptEngineGone`, `kInFlightAttemptAmbiguous`,
`kUnregisteredLiveDomain`, `kGenerationRegressed`. Every live attempt is reported as
`kInFlightAttemptAmbiguous` with the detail "in-flight attempt remains conservative until its
executor answers", and the report is always marked `conservative = true`. The most recent reports
are retained (bounded by `SchedulerLimits::max_snapshot_history`, default 16) and included in
snapshots.

## REAL, SYNTHETIC and UNSUPPORTED

Provenance is carried by every domain record, capability record and execution outcome. The canonical
values (`to_string`) are exactly:

| Value | Token | Meaning |
|-------|-------|---------|
| `Provenance::kReal` | `REAL` | observed or executed against the actual system boundary |
| `Provenance::kSynthetic` | `SYNTHETIC` | produced by the deterministic synthetic backend |
| `Provenance::kUnsupported` | `UNSUPPORTED` | claimed, but no supporting hardware or evidence exists on this host |

Parse tokens are the lowercase forms `real`, `synthetic` and `unsupported`.

Provenance is a *publisher claim*, and the runtime's job is to keep claims internally consistent and
to refuse the ones it can prove false - not to verify the claim itself:

- `SyntheticBackend::provenance()` always reports `kSynthetic` (`include/tos/backends/synthetic.hpp`),
  and `make_synthetic_domain()` defaults every record it builds to `kSynthetic`; the shipped worker
  CLI also defaults every non-CPU domain class to `kSynthetic`.
- A *published record* carries the provenance its configuration names
  (`record.provenance = config.provenance` in `src/backends/synthetic.cpp`), so a deployment that
  deliberately publishes another provenance is publishing its own claim.
- The runtime never rewrites provenance, and it rejects the inconsistencies it can detect:
  `validate_domain_record()` refuses a REAL capability provenance under a SYNTHETIC domain
  (`domain.provenance_overclaim`) and an UNSUPPORTED domain carrying a REAL capability
  (`domain.provenance_mismatch`).
- The default policy refuses UNSUPPORTED provenance outright
  (`allow_unsupported_provenance = false`), and `check_capability()` rejects an UNSUPPORTED capability
  record with `kBackendUnsupported` regardless of policy.

See "Actual limitations" for what this does *not* protect against.

## Hardware validation on this machine

This is what has actually been validated on the development machine - nothing more is claimed.

- **CPU: REAL, and really executes.** The machine has an AMD Ryzen 7 9800X3D. `CpuBackend`
  (`src/backends/cpu_backend.cpp`) reads the real hardware thread count
  (`std::thread::hardware_concurrency`), the CPUID brand string, the host name, the NUMA node count
  and the physical memory size, and publishes them with `Provenance::kReal` and an authoritative
  capability record. `execute_operation_on_host()` runs the nine built-in operation classes for real
  on host memory, including a software CRC32C with no hardware dependency.
  `tests/test_operation_cpu.cpp` and `tests/test_hardware_probe.cpp` execute operations and compare
  results against the independent `crc32c()` implementation.
- **Accelerator: REAL when a CUDA toolkit is present, otherwise UNSUPPORTED.**
  `CudaBackend` (`include/tos/backends/cuda_backend.hpp`, `src/backends/cuda_backend.cpp`,
  `src/backends/cuda_kernels.cu`) is an accelerator execution domain that publishes
  `Provenance::kReal` only after it has opened a device. Each execution allocates a device buffer and
  a pinned staging buffer, copies the input to the device, runs a real CRC32C kernel, synchronizes,
  copies the result back, compares it with the host reference and frees everything; the backend
  exposes `outstanding_device_bytes()` so cleanup is checkable, and the proof is
  `tests/test_cuda.cpp`. Measured on this machine (NVIDIA GeForce RTX 5090, compute capability 120,
  34,162,016,256 bytes of device memory, CUDA 13.1, Ninja build with `-DTOS_ENABLE_CUDA=ON` and
  `-DCMAKE_CUDA_COMPILER=<toolkit>/bin/nvcc.exe`):

  ```
  cuda device: NVIDIA GeForce RTX 5090 compute_capability=120 memory_bytes=34162016256
  cuda direct: bytes=1048579 device_digest=0x612c9f10 host_digest=0x612c9f10 host_reference=exact
  cuda dispatch: state=COMPLETED provenance=REAL digest=0x29ce5b1b host_digest=0x29ce5b1b committed_once
  cuda cleanup: outstanding_device_bytes=0 kernel_launches=34 device_bytes_copied=7567890
  ```

  When the toolkit or the device is absent (for example `CUDA_VISIBLE_DEVICES=-1`), the same test
  prints `SKIP cuda: no device` and the backend reports `Provenance::kUnsupported` with an empty
  capability set, so no build ever claims a device it cannot use. The CUDA toolkit must be able to
  determine its host compiler: with the Visual Studio generator CMake needs the Visual Studio CUDA
  integration, which this machine's Build Tools installation does not have, so the CUDA-enabled build
  is configured with the Ninja generator from a `vcvars64.bat` environment. This proves accelerator
  execution only; it proves nothing about NIC, SmartNIC, DPU or GPUDirect behaviour.
- **NIC: REAL discovery only.** `enumerate_nics()` (`src/backends/nic_probe.cpp`) enumerates the
  host adapters through `GetAdaptersAddresses` and reports identity, link speed, operational state
  and MAC address; the machine's adapters, including a Realtek adapter, are discovered.
  `make_nic_domain()` then publishes the adapter with `Provenance::kReal` but with a
  **non-authoritative** capability record: an empty proven operation list and every offload-related
  flag in `unproven_flags`. **No NIC offload is claimed or provable**: a discovered NIC is rejected
  as `kCapabilityUnproven` under the default policy and as `kOperationNotSupported` even when
  positive evidence is not required, and no execution path exists that runs an operation on a NIC.
  The admission rules are enforced by `tests/test_hardware_probe.cpp`.
- **SmartNIC, DPU and RDMA: UNSUPPORTED on hardware, SYNTHETIC when exercised.** There is no
  SmartNIC, no DPU and no RDMA hardware on this machine, and no vendor backend that could prove one.
  Those domain classes exist in the model and are exercised only through the deterministic synthetic
  backend, whose records are `SYNTHETIC` by default (`make_synthetic_domain()` and the worker CLI
  both default to it). `CapabilityFlag::kRdma` and
  `TransportClass::kRdmaMessage` are model vocabulary; the host executor publishes `kRdma` as an
  unproven flag and nothing implements RDMA execution.

## Build

Requirements: CMake 3.20 or newer, a C++20 compiler (MSVC 2022 / Visual Studio 17 2022 is the
supported configuration), and no third-party dependency - the library links only `Threads::Threads`
and, on Windows, `ws2_32` and `iphlpapi`.

Configure and build with the Visual Studio generator:

    cmake -S . -B build -G "Visual Studio 17 2022" -A x64
    cmake --build build --config Release
    cmake --build build --config Debug

Because the Visual Studio generator is multi-configuration, artifacts land in a per-configuration
subdirectory: the library at `build\Release\transport_offload_scheduler.lib` (Debug builds use the
`d` postfix: `transport_offload_schedulerd.lib`), tools at `build\Release\*.exe`, tests at
`build\tests\Release\*.exe`.

For a single-configuration generator such as Ninja:

    cmake -S . -B build-ninja -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build-ninja

Build options (all `ON` by default except ASan): `TOS_BUILD_TESTS`, `TOS_BUILD_EXAMPLES`,
`TOS_BUILD_BENCHMARKS`, `TOS_BUILD_APPS`, `TOS_WARNINGS_AS_ERRORS`, `TOS_ENABLE_LOGGING`,
`TOS_ENABLE_ASAN`, `TOS_ENABLE_CUDA` (adds the real accelerator backend when a toolkit is found;
 otherwise the bridge falls back to an UNSUPPORTED stub). First-party
code is compiled with `/W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc` and, by default, `/WX`.

## Tests

Every test file in `tests/` becomes its own executable driven by CTest; no test declares a timeout -
a wait that never satisfies its predicate is treated as a defect and the suite hangs rather than
hiding it.

    ctest --test-dir build -C Release --output-on-failure
    ctest --test-dir build -C Debug --output-on-failure

Individual executables can be run directly; each accepts an optional `--filter=<substring>` that
selects tests by name:

    build\tests\Release\test_eligibility.exe
    build\tests\Release\test_authority.exe --filter=coordinator_epoch
    build\tests\Release\test_multiprocess.exe

`test_multiprocess` and the worker-fencing example locate `tos_worker.exe` relative to their own
executable rather than through a hardcoded path: the test support probes `.`, `..\..\`,
`..\..\Release\` and `..\..\Debug\`, and the example probes `.` and `..\..\Release\`. Run them from the
build tree they were built in.

The suites are organised by property: eligibility and policy gating (`test_eligibility`), authority
binding and staleness (`test_authority`), completion authority (`test_completion`), reservation
accounting (`test_reservation`), retry and fallback (`test_fallback_retry`), real CPU execution
(`test_operation_cpu`), hardware honesty (`test_hardware_probe`), persistence and recovery
(`test_persistence`, `test_recovery`), protocol adversarial input (`test_protocol_adversarial`),
lifecycle and hard bounds (`test_lifecycle_limits`), real multi-process worker death
(`test_multiprocess`), and forced-interleaving races (`test_races`). Interleaving is made
reproducible by `IInterleavingHook`: the runtime exposes 19 named hook points (fired from 20 call
sites) and every one of them fires with no internal lock held, so the race suite can force a specific
ordering instead of relying on thread timing.

## Examples

Seven examples are built from `examples/`, each a self-contained program that prints the decision it
observed (`example_<name>.exe` in the build tree):

| Target | Shows |
|--------|-------|
| `example_basic_selection` | CPU versus a SYNTHETIC DPU for several payload sizes, with scores and deciding factors |
| `example_hard_eligibility` | hard eligibility rejecting fast-but-unproven candidates before ranking is consulted |
| `example_explanation` | byte-for-byte reproducible explanations, and which named factor moved a decision |
| `example_reservation` | plan -> reserve -> revalidate -> dispatch -> complete -> release, with ledger audits |
| `example_fallback` | a fenced domain invalidating a plan, explicit fallback, and `offload_required` rejecting instead of using the CPU |
| `example_worker_fencing` | a real coordinator, a real worker process, a real kill and a real replacement incarnation |
| `example_installed_consumer` | the installed-package consumer path (see below) |

## Command-line tools

`tos_inspect` is the inspection and administrative tool. Read-only inspection never mutates anything;
administrative commands are a separate path, must be typed explicitly, and report the effect they
had.

    tos_inspect version                      # library identity, version, protocol, persistence format
    tos_inspect cpu                          # real host CPU facts and the CPU capability set
    tos_inspect nics                         # real network adapter inventory
    tos_inspect explain                      # in-process deterministic decision with every candidate
    tos_inspect snapshot    --host H --port N
    tos_inspect accounting  --host H --port N
    tos_inspect reconcile   --host H --port N
    tos_inspect attempt <id>    --host H --port N
    tos_inspect operation <id>  --host H --port N
    tos_inspect version         --port N      # also queries the coordinator

    # Administrative (MUTATION); all require --host and --port
    tos_inspect fence-domain <id> --reason R
    tos_inspect fence-boot <id> --reason R
    tos_inspect save-state
    tos_inspect set-offload-requirement <any|prefer_offload|offload_required|host_required>
    tos_inspect shutdown

Exit codes are 0 for success, 1 for a runtime failure and 2 for a usage error.

    tos_coordinator [--bind HOST] [--port N] [--host-node NAME] [--state PATH] [--persist]
                    [--no-recovery] [--reserve] [--slots N]
                    [--offload-requirement TOK] [--preference LIST] [--max-sessions N]
                    [--log-level L]
    tos_worker --port N [--host HOST] [--name NAME] [--node NAME]
               [--type cpu|accelerator|nic|smartnic|dpu|other] [--domains N]
               [--provenance real|synthetic|unsupported] [--domain-base N]
               [--fault none|reject-dispatch|drop-completion|ambiguous-completion|
                        failure-report|die-after-apply]
               [--fault-after N] [--publish-load N] [--log-level L]

`tos_coordinator` prints `COORDINATOR_READY <port> <epoch>` once it is serving and `tos_worker` prints
`WORKER_READY <worker-id> <boot-id>` once it has registered.

## CMake installation and downstream use

    cmake --install build --config Release --prefix C:/tos

The install step exports the library, its headers, the package configuration files and the LICENSE.
A downstream project then uses:

    cmake_minimum_required(VERSION 3.20)
    project(consumer LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)

    find_package(TransportOffloadScheduler CONFIG REQUIRED)

    add_executable(consumer main.cpp)
    target_link_libraries(consumer PRIVATE SummonSoftwareLabs::TransportOffloadScheduler)

Point `CMAKE_PREFIX_PATH` at the install prefix (`-DCMAKE_PREFIX_PATH=C:/tos`). The imported target
name is the project namespace plus the target's `EXPORT_NAME`:
`install(EXPORT TransportOffloadSchedulerTargets NAMESPACE SummonSoftwareLabs::)` over the
`transport_offload_scheduler` target, whose `EXPORT_NAME` is `TransportOffloadScheduler`. The build
tree additionally provides the alias `tos::tos`, which is **not** exported; use
`SummonSoftwareLabs::TransportOffloadScheduler` in anything that must work against an installed
package. `examples/example_installed_consumer.cpp` is such a consumer: it includes only
`<tos/tos.hpp>` and uses no internal header.

A minimal downstream program:

    #include <tos/tos.hpp>

    #include <iostream>
    #include <memory>
    #include <utility>

    int main() {
      tos::SchedulerOptions options;
      options.policy = tos::make_default_policy();
      options.host_node = "downstream.host";

      tos::Scheduler scheduler(std::move(options));
      if (!scheduler.start()) return 1;

      // A real host CPU execution domain, discovered from the operating system.
      tos::CpuBackend::Options cpu_options;
      cpu_options.domain_id = tos::ExecutionDomainId(1);
      cpu_options.name = "cpu.host.0";
      cpu_options.parent_host = "downstream.host";
      auto backend = std::make_shared<tos::CpuBackend>(cpu_options);

      // Publish the domain through the normal generation-checked evidence path.
      tos::LocalDomainPublisher publisher(scheduler, backend);
      if (!publisher.sync()) return 1;

      tos::OperationRequest request;
      request.operation_class = tos::opclass::checksum_crc32c();
      request.payload.size_bytes = 4096;
      request.payload.source_memory = tos::MemoryDomain::kHost;
      request.payload.destination_memory = tos::MemoryDomain::kHost;
      request.payload.transport_class = tos::TransportClass::kRawFrames;
      request.payload.payload_class = tos::PayloadClass::kOpaqueBytes;
      request.payload.alignment_bytes = 8;

      const tos::PlanResult planned = scheduler.plan(request);
      if (!planned.planned) {
        std::cerr << "planning refused: " << planned.status.code << "\n";
        return 1;
      }
      std::cout << "outcome=" << tos::to_string(planned.explanation.outcome)
                << " domain=" << tos::format_id(planned.plan.domain.value())
                << " provenance=" << tos::to_string(planned.plan.provenance) << "\n";

      return scheduler.shutdown().ok ? 0 : 1;
    }

To dispatch and observe a completion, install a channel and a completion sink (see
`examples/example_installed_consumer.cpp`, which wires `CpuBackend` + `LocalDispatchChannel` +
`LocalDomainPublisher` and waits for the committed completion).

## Actual limitations

- **No SmartNIC, DPU or RDMA hardware exists on this machine.** Those domain classes are modelled and
  are exercised only through the deterministic synthetic backend, whose records are `SYNTHETIC` by
  default. Nothing in this repository proves SmartNIC, DPU or RDMA capability, and no such execution
  has been performed.
- **Provenance is trusted from the publisher.** The runtime enforces *structural* consistency (the
  checks listed under "REAL, SYNTHETIC and UNSUPPORTED") but it cannot verify that a peer really has
  the hardware it claims. `tos_worker` accepts `--provenance real`, which publishes a REAL record for
  any domain class using the in-process host executor capability set, and the coordinator will
  register it. Treat the coordinator's listener as a trusted boundary; nothing here authenticates a
  worker.
- **The accelerator backend needs a CUDA toolkit to be real.** Without one it is compiled against a
  bridge stub that reports `UNSUPPORTED` with an empty capability set, so the accelerator domain
  class can then only be exercised as SYNTHETIC. Device execution is proven for CRC32C only; no other
  operation class is offloaded to the device.
- **No NIC offload exists or is claimed.** NIC discovery is real, but the capability record is
  deliberately non-authoritative, the proven operation list is empty, and there is no code path that
  executes an operation on a NIC.
- **Synthetic execution shares the host executor transform.** `SyntheticBackend::execute()` runs the
  same in-process host transform as the CPU backend; only the *records* it publishes are synthetic.
  It proves scheduling behaviour (eligibility, authority, reservation, dispatch, completion,
  retry/fallback, fencing), not device behaviour or device performance.
- **Queue and load evidence quality depends on the backend.** The runtime never invents operating
  evidence. `CpuBackend` derives utilization from its own in-flight counter and reports zero queue
  depth; the synthetic backend reports whatever its configuration says. A real NIC or DPU would need
  a vendor backend to publish measured queue, congestion and latency evidence - and no such backend
  is present here.
- **The dispatch payload bound is 512 KiB** (`kMaxDispatchPayloadBytes`), and result payloads share
  it (`kMaxResultPayloadBytes`). `SchedulerLimits::max_payload_dispatch_bytes` bounds the payload
  copied into a plan; larger transfers must be segmented by the caller.
- **Single coordinator.** There is one coordinator process with one `Scheduler`; there is no
  coordinator election, no quorum and no replication. A coordinator restart is a new epoch, and all
  work in flight at that moment is conservatively classified as ambiguous or fenced.
- **Shutdown classifies in-flight work conservatively.** `Scheduler::shutdown()` marks dispatched or
  dispatching attempts `kOutcomeUnknown`/`kAmbiguous` and everything else
  `kFenced`/`kFencedNoEffect`, then invalidates every domain's reservations. It does not wait for
  outstanding work to finish and cannot report whether it took effect.
- **Retry is deliberately narrow.** Ambiguous outcomes are never retried automatically
  (`max_ambiguous_retries` defaults to 0 and ambiguous resolutions are refused outright), and only
  replay-safe side-effect classes may be retried at all. `kNonRepeatable`, `kAtMostOnceRequired` and
  `kUnknown` are refused.
- **Bounded everything.** Domains (100000), operation classes (128), pending plans (65536),
  outstanding reservations (200000), live and historical attempts (200000 each), reported candidates
  (4096), snapshot history (16), fenced boot set (4096), frame size (1 MiB) and state file size
  (64 MiB) are all bounded; reaching a bound is an explicit, named refusal, not growth.
- **The ledger and registry are in-memory.** Durability is an explicit or mutation-triggered
  checkpoint of structure (`persist_on_mutation`, on shutdown, or on demand) plus conservatively
  reclassified attempts; there is no write-ahead log and no exactly-once guarantee across a crash.
- **NIC link-speed reporting, and a stale in-tree comment.** `enumerate_nics()` maps the all-ones
  `IP_ADAPTER_ADDRESSES::TransmitLinkSpeed` sentinel to `0`, which is the documented "the operating
  system does not report a speed" value (`include/tos/backends/nic_probe.hpp`). The comment block in
  `tests/test_hardware_probe.cpp` still describes the earlier behaviour (publishing the raw sentinel)
  and states that its assertion "is expected to fail"; with the mapping in place that assertion
  passes, so the comment is out of date rather than the code being defective. The scheduler does not
  consume `link_speed_bps` at all, so scheduling decisions were never affected either way.
- **The coordinator shutdown path joins session threads while holding the session registry mutex**,
  and each session thread must take that same mutex to remove itself from the registry. The window is
  narrow and the existing lifecycle test exercises the path, but this is a real lock-across-join
  inversion rather than a proven-safe one; see `docs/DEADLOCK_AUDIT.md`, item 4.
- **No telemetry, analytics or hidden reporting.** The runtime has no network client other than the
  coordinator/worker/client protocol it documents, and logging writes to `stderr` (or `stdout` for
  machine-readable readiness lines) only. No usage data leaves the process.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
