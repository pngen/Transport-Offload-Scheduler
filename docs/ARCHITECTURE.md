# Architecture

Component map, locking doctrine, generation model, lifecycle state machines and the boundary between
evidence and authority for Transport Offload Scheduler 1.0.0.

Everything in this document is derived from the headers in `include/tos/**`, the sources in
`src/**` and the tests in `tests/**`. Where a statement is a consequence of code rather than a
comment, the file and symbol are named.

---

## 1. Component map

### 1.1 Scheduler (`include/tos/core/scheduler.hpp`, `src/core/scheduler.cpp`)

The public facade and the only entry point most callers need. `Scheduler::Impl` owns the whole
runtime:

| Member | Type | Role |
|--------|------|------|
| `registry` | `OperationClassRegistry` | operation class table (9 built-ins, up to 128) |
| `policy` | `SchedulerPolicy` | weighted, hard-restriction, retry, fallback, freshness and reservation policy |
| `plans` | `std::unordered_map<std::uint64_t, ExecutionPlan>` | plans of record, keyed by attempt identity |
| `retired_plans` | `std::unordered_set<std::uint64_t>` | plans that can never be dispatched again |
| `domains` | `DomainRegistry` | execution domain records |
| `workers` | `WorkerAuthority` | worker-incarnation fencing |
| `reservations` | `ReservationLedger` | capacity accounting |
| `attempts` | `AttemptLedger` | attempt records and completion authority |
| `channel` | `std::shared_ptr<IDispatchChannel>` | dispatch boundary |
| `reports` | `std::vector<ReconciliationReport>` | bounded reconciliation history |
| `recovered_domains` | `std::vector<ExecutionDomainRecord>` | durable baseline for reconciliation |
| counters | `std::atomic<std::uint64_t>` | operation, plan sequence, dispatch, snapshot, domain, statistics |
| `hook` | `std::atomic<IInterleavingHook*>` | deterministic interleaving hook |

Public surface, grouped as in the header: lifecycle (`start`, `shutdown`, `running`,
`coordinator_epoch`); configuration (`operation_classes`, `register_operation_class`, `policy`,
`set_policy`, `policy_generation`, `limits`, `set_hook`, `set_dispatch_channel`); domain
publication (`register_domain`, `update_domain`, `publish_capability`, `publish_load`,
`publish_locality`, `publish_topology`, `publish_compatibility`, `publish_capacity`,
`withdraw_evidence`, `fence_domain`, `fence_worker_boot`, `remove_domain`,
`register_worker_boot`); planning (`plan`, `reserve`, `dispatch`, `plan_reserve_dispatch`);
execution boundary (`complete`, `report_failure`, `retry`, `fallback`, `cancel`); inspection
(`snapshot`, `reconcile`); persistence (`save_state`, `save_state_to`, `load_state`,
`load_state_from`); component access (`domains`, `reservations`, `attempts`, `workers`,
`dispatch_channel`).

`Scheduler::~Scheduler()` calls `shutdown()` when the runtime is still running. `start()` performs
optional recovery, installs a default policy if none is published, publishes a new coordinator epoch,
registers the process-local worker boot ("local") and clears the reconciliation history.

### 1.2 DomainRegistry (`include/tos/core/state.hpp`, `src/core/state.cpp`)

A `shared_mutex`-guarded map of `ExecutionDomainRecord` by identity, plus a name index and a
mutation sequence. Writers: `upsert`, `publish_capability`, `publish_load`, `publish_locality`,
`publish_topology`, `publish_compatibility`, `publish_capacity`, `fence`,
`fence_worker_boot`, `remove`, `mark_domain_evidence_stale`, `mark_dynamic_evidence_stale`.
Readers copy value records out under a shared lock: `get`, `find`, `find_by_name`, `all`
(deterministic order by identity), `size`, `mutation_sequence`.

Publication rules enforced here, not in the callers:

- `upsert` rejects a name already owned by another identity (`domain.name_conflict`) and refuses to
  move a domain generation backwards within one worker incarnation
  (`domain.generation_regression`); a *different* incarnation re-bases the counters (its own counter
  space restarts) and advances capability, backend, topology, locality and compatibility generations.
- A change in capability content without advancing the domain generation is refused
  (`domain.capability_conflict`).
- `publish_capability` keeps a per-incarnation `PublisherSequence` watermark: a duplicate or
  reordered publication from the same incarnation with identical content is an idempotent refresh
  that changes nothing, while different content at or below the consumed sequence is rejected
  (`domain.capability_stale_publication`).
- `publish_load` requires published health/load/queue generations and an owning incarnation, and
  rejects a load publication whose sequence did not advance within one incarnation
  (`domain.load_stale_publication`). It does **not** advance the domain generation - it advances the
  load, queue, health and evidence generations the plan binding already checks.
- `fence`, `fence_worker_boot` and `mark_domain_evidence_stale` advance the domain generation and
  clear the volatile load evidence, so every plan bound to the old evidence is invalidated.

### 1.3 ReservationLedger (`include/tos/core/reservation.hpp`, `src/core/reservation.cpp`)

Sharded by execution domain (64 shards by default, capped at 256), each shard holding its own mutex,
its reservation map and its accounting rows. Every acquire/release touches exactly one shard, so no
caller ever holds two shard locks. `acquire` is all-or-nothing: it checks every requested resource
kind before retaining anything. `commit`, `release`, `rollback` and `invalidate_domain` close the
accounting exactly once and reject double transitions by name. `audit()` recomputes each row from
the reservation records and reports `consistent`, `outstanding`, `committed` and `leaked`.

### 1.4 AttemptLedger (`include/tos/core/attempt.hpp`, `src/core/attempt.cpp`)

Sharded by attempt identity (64 shards by default, capped at 256) plus a second set of shards keyed
by operation for generation allocation, so the identity space is owned by the ledger. The attempt
identity encodes its shard in the high bits, which makes shard lookup a shift rather than a hash.
`register_attempt` inserts the record as `kDispatching` *before* any external call is made.
`commit_completion` is the single completion-authority critical section: one shard lock, a fixed
validation order, at most one commit per attempt generation. The ledger never calls out to a
callback while a shard is locked.

### 1.5 WorkerAuthority (`include/tos/core/authority.hpp`, `src/core/authority.cpp`)

A single-mutex registry of current boot identities per worker, boot ownership, the fenced set and
labels. `register_boot` rejects a fenced boot, rejects a boot already owned by another worker,
and - when a worker registers a *new* incarnation - fences the previous one. The fenced set is
bounded (`max_fenced`, 4096) and trimmed deterministically by ascending value.
`current_boot`, `fenced_boots`, `live_boots` and `tracked_workers` are the read side.

### 1.6 Planner and ranking (`include/tos/core/planner.hpp`, `include/tos/core/ranking.hpp`, `src/core/planner.cpp`, `src/core/ranking.cpp`)

`compute_factors(FactorInputs)` is a pure function producing the 22-element `FactorVector`;
`weighted_score()` is integer-exact; `candidate_better()` is the total order. `normalize_*`
helpers clamp into `[0, kFactorScale]` where `kFactorScale == 1000000`. `describe_factors()`
renders a factor vector as `name=value` pairs for explanations and tooling.

### 1.7 Snapshot and reconciliation (`include/tos/core/snapshot.hpp`, `include/tos/core/reconcile.hpp`, `src/core/snapshot.cpp`, `src/core/reconcile.cpp`)

`Scheduler::snapshot()` copies the runtime into a `SchedulerSnapshot` value: generation, epoch,
policy generation, running flag, mutation sequence, domains, worker boot views (current and fenced),
reservations, attempts, retained reconciliation reports, totals and a policy summary.
`render_json()` and `render_text()` are const methods on that *value*; they have no handle on the
runtime, so rendering cannot re-enter state. `Scheduler::reconcile()` compares the durable baseline
(`recovered_domains`) with the live registry and returns a `ReconciliationReport` value; it stores
the report in a bounded history.

### 1.8 Backends (`include/tos/backends/**`, `src/backends/**`)

`IExecutionBackend` is deliberately narrow: `name`, `provenance`, `generation`,
`discover_domains`, `query_capability`, `query_load`, `reserve`, `release`, `execute`,
`supports_cancel`, `cancel`, `shutdown`. A backend is never given a path to mutate scheduler state.

| Backend | File | Provenance | Notes |
|---------|------|-----------|-------|
| `CpuBackend` | `src/backends/cpu_backend.cpp` | `REAL` | real thread count, CPUID brand, host name, physical memory; authoritative capability; every discovery advances the volatile generations |
| `SyntheticBackend` | `src/backends/synthetic.cpp` | `SYNTHETIC` | deterministic domain classes and deterministic faults; runs the same host transform in-process. `provenance()` always reports `SYNTHETIC`; a *published record* carries the provenance its `SyntheticDomainConfig` names (`make_synthetic_domain()` defaults it to `SYNTHETIC`) |
| NIC discovery | `src/backends/nic_probe.cpp` | `REAL` | real adapter enumeration; deliberately non-authoritative capability with an empty proven operation list |
| `LocalDomainPublisher` | `src/backends/local_publisher.cpp` | - | drives the same publication path a remote worker drives over the protocol |
| `LocalDispatchChannel` | `src/backends/local_channel.cpp` | - | in-process dispatch channel over a bounded `ThreadPool`, with explicit fault injection |

`execute_operation_on_host()` (`src/backends/backend.cpp`) is the shared host transform: CRC32C,
RLE compress/decompress, staging copy, segmentation, reassembly, integrity validation, an
append-only side-effect sink and a no-op probe. `make_host_executor_capability()` derives the
published capability from that executor - flags the executor does not implement are published as
`unproven_flags`, never as proven.

### 1.9 Persistence (`include/tos/persist/store.hpp`, `src/persist/store.cpp`, `src/persist/codec.cpp`)

`StateStore` owns a path and provides `save`, `load`, `remove`, `exists`, plus the canonical
`encode`/`decode` pair and `payload_crc`. The codec in `src/persist/codec.cpp` writes domains,
attempts and the policy field by field through the bounded `ByteWriter`/`ByteReader` primitives.

### 1.10 Protocol (`include/tos/dist/protocol.hpp`, `src/dist/protocol.cpp`)

Framing (`encode_frame`, `decode_header`, `decode_frame`), the 24 message types and a
hand-written `encode`/`decode` pair per message. `frame_of<Message>()` combines message encoding
and framing. Decoding is total: every failure has a named `DecodeError`.

### 1.11 Coordinator, worker, client (`include/tos/dist/**`, `src/dist/**`)

- `OffloadCoordinator` (`src/dist/coordinator.cpp`): owns a `Scheduler`, a listener, a session map,
  one thread per session, one accept thread, and a `RemoteChannel` dispatch channel that routes an
  envelope to the session owning the target domain.
- `OffloadWorker` (`src/dist/worker.cpp`): owns a backend, a socket, a boot identity, the published
  domain records and a published-identity to backend-identity map. It answers dispatches by executing
  and reporting a completion or a failure report, and it never reports transport acceptance as
  completion.
- `CoordinatorClient` (`src/dist/client.cpp`): hello handshake plus a strict
  `exchange(type, payload)` - one request frame, one response frame.

---

## 2. Locking doctrine

Copied verbatim from the header comment at the top of `src/core/scheduler.cpp` (lines 3-20):

```text
Locking doctrine
----------------
A thread may acquire scheduler locks only in this order and never in reverse:
  1. registry_mutex        (operation class registry)
  2. policy_mutex          (scheduling policy)
  3. plans_mutex           (plans of record)
  4. DomainRegistry        (internal shared_mutex)
  5. WorkerAuthority       (internal mutex)
  6. ReservationLedger     (one shard)
  7. AttemptLedger         (one shard, one operation shard)
  8. reports_mutex / recovered_mutex
No code path holds two shard locks at once. No lock is ever held while:
  * calling a dispatch channel, backend or transport,
  * invoking an interleaving hook,
  * performing filesystem I/O,
  * joining or creating threads.
Every such call is made after the relevant lock has been released, which is why
the code below copies state out of a critical section before acting on it.
```

How the doctrine is realised, by mechanism:

- **Copy-out on read.** `policy_copy()`, `channel_copy()`, `find_plan()`, `store_plan()`,
  `erase_plan()`, `retire_plan()`, `plan_retired()`, `plan_count()` each take one lock, copy or
  mutate, and release before returning. `DomainRegistry` readers return value copies.
- **Nested acquisition is forward-only.** The only nested case in the scheduler is
  `plan_internal` holding `registry_mutex` (1) across hard eligibility, inside which
  `WorkerAuthority::is_current` (5) and `ReservationLedger::has_capacity` (6) are called. Neither
  component calls back into the scheduler, so the order cannot invert.
- **No lock across I/O, callbacks or threads.** Dispatch (`channel->send`), cancellation
  (`channel->request_cancel`), interleaving hooks (`fire`), persistence (`StateStore::save` /
  `load`) and channel shutdown (`channel->shutdown`) are all called with no scheduler lock held.
- **Hooks are lock-free by construction.** `hook` is a `std::atomic<IInterleavingHook*>`; `fire()`
  loads it, copies the context and calls it. Every `fire()` call site in `scheduler.cpp` is outside
  every lock scope (eligibility, ranking, reserve, revalidate, reservation commit, attempt
  registration, channel send, completion commit, fallback, cancellation, shutdown fence).
- **Sharded ledgers never nest.** `ReservationLedger` and `AttemptLedger` methods each lock at most
  one shard; the multi-shard readers (`list`, `list_live`, `live_count`, `audit`, `outstanding_count`,
  `prune_history`) acquire and release shards one at a time in a loop, never nested.
- **The shared mutex is never upgraded.** No path takes a shared lock on `DomainRegistry` and then an
  exclusive one on the same object; writers enter `upsert` and the `publish_*` family directly.
- **The lock order is documented, and deviations are enumerated in `docs/DEADLOCK_AUDIT.md`.**

---

## 3. Generation model

Generations are defined in `include/tos/core/identities.hpp`. `Generation<Tag>` starts at 1 when a
boundary is first published (0 means "never published"), advances with `next()` and **saturates**
instead of wrapping, because a wrapping counter could re-authorize stale state.
`relate_generations(bound, observed)` classifies a comparison as `kEqual`, `kAdvanced`,
`kRegressed` or `kUnset`; `validate_authority()` records every non-equal relation as an
`AuthorityMismatch`.

### 3.1 What each generation gates

| Generation | Bound in the plan | Advanced by | Consequence of a mismatch at dispatch |
|-----------|-------------------|-------------|----------------------------------------|
| `ExecutionDomainGeneration` | `binding.domain_generation` | `upsert` (record or capability content change), `publish_capability`, `publish_locality`, `publish_topology`, `publish_compatibility`, `publish_capacity`, `fence`, `fence_worker_boot`, `mark_domain_evidence_stale` | `authority.stale` (`domain_generation`) |
| `CapabilityGeneration` | `binding.capability_generation` | `publish_capability` (always `next()`), incarnation change in `upsert` | `authority.stale` (`capability_generation`); checked when `freshness.capability` (default true) |
| `BackendGeneration` | `binding.backend_generation` | incarnation change in `upsert`; published by the backend | `authority.stale` (`backend_generation`) |
| `TopologyGeneration` | `binding.topology_generation` | `publish_topology`, incarnation change | checked when `freshness.topology` (default false) |
| `LocalityGeneration` | `binding.locality_generation` | `publish_locality`, incarnation change | checked when `freshness.locality` (default false) |
| `HealthGeneration` | `binding.health_generation` | the publisher's `publish_load` | checked when `freshness.health` (default true) - this is how a load/health change invalidates a plan |
| `QueueGeneration` | `binding.queue_generation` | the publisher's `publish_load` | checked when `freshness.queue` (default false) |
| `LoadGeneration` | `binding.load_generation` | the publisher's `publish_load` | checked when `freshness.load` (default false) |
| `PolicyGeneration` | `binding.policy_generation` | `set_policy` (auto-advances when the new policy repeats the current value) | **always** checked: `policy_generation` |
| `CompatibilityGeneration` | `binding.compatibility_generation` | `publish_compatibility`, incarnation change | checked when `freshness.compatibility` (default true) |
| `IsolationPolicyGeneration` | `binding.isolation_generation` | the policy itself | checked when `freshness.isolation` (default false) |
| `EvidenceGeneration` | `binding.evidence_generation` | the publisher (load evidence, capability evidence) | checked when `freshness.evidence` (default true) |
| `CoordinatorEpoch` | `binding.coordinator_epoch` | every `Scheduler::start()`: `max(initial_epoch, persisted epoch) + 1` | **always** - and `Scheduler::complete()` rejects a foreign epoch before touching any state |
| `ExecutionAttemptGeneration` | `binding.attempt_generation`, `registration.generation`, `submission.generation` | `allocate_generation(operation)`, strictly increasing per operation | `CompletionRejection::kAttemptGenerationMismatch`; `register_attempt` refuses a regression with `attempt.generation_regression` |
| `SnapshotGeneration` | `DecisionExplanation::snapshot`, reports | every `snapshot()` / `reconcile()` | identifies the state a decision or report was taken from |
| `PersistenceGeneration` | `PersistedState::generation` | every successful `save_state_to` | identifies the state-file revision; a refused load reports `state.generation_rollback` when a *durable domain* generation is below the live one |
| `WorkerBootId` (identity, not a counter) | `binding.worker_boot`, `registration.worker_boot` | every process incarnation | `CompletionRejection::kStaleWorkerBoot`, `IneligibilityReason::kWorkerBootStale` |

### 3.2 Publisher watermarks

Authoritative generations are owned by the coordinator and always advance. A publisher (worker or
backend) has its own counter space, so the coordinator keeps a `PublisherSequence` watermark per
incarnation recording how far that publisher's stream has been consumed (capability, load, queue,
health, locality, topology, compatibility, domain). A publication at or below the watermark with
identical content is an idempotent refresh; with different content it is a stale publication and is
rejected. When the incarnation changes, the watermark is reset and the authoritative generations are
re-based by advancing them, so a replacement process cannot move authority backwards and cannot
reuse its own restarted counter space to look current.

---

## 4. Lifecycle state machines

### 4.1 Plan (`PlanState`, `include/tos/core/dispatch.hpp`)

```text
                 plan()
                   |
                   v
              [ kCreated ] --reserve()--> [ kReserved ] --dispatch()--> [ kDispatched ]
                   |                            |                             |
                   |                            |                             +--> completion -> plan erased
                   |                            |
                   +--- dispatch() revalidation failure ---> [ kRejected ]   (reservation rolled back)
                   +--- dispatch() transport failure ------> [ kRejected ]   (attempt failed, reservation released)
                   +--- cancel() before dispatch ----------> retired          (rejection code "plan.consumed")
                   +--- retry() supersedes ----------------> retired          (rejection code "plan.consumed")
```

`fallback()` is deliberately *not* in that list: it produces a new plan of record for another domain
class and leaves the original plan untouched, so the caller decides whether the original is abandoned
(retired) or still dispatchable. The `channel.absent` path of `dispatch()` is the other asymmetry: it
marks the attempt failed and releases the reservation without changing the plan's state.

Retirement is recorded in `Scheduler::Impl::retired_plans` by `retire_plan()` rather than by mutating
a caller-held copy, which is why a stale copy of a plan cannot be dispatched even though
`PlanState::kConsumed` is only ever *tested* by `dispatch()` - the enum value exists and is parsed,
but no code path assigns it. `plan.state != kCreated` makes a plan non-reservable
(`plan.not_reservable`).

Failure reports are the exception that proves the rule: `Scheduler::report_failure()` classifies the
attempt but keeps the plan of record, because retry re-runs eligibility from the request the plan
carries. Only supersession (retry, cancellation) retires it.

Storage: the plan of record is stored on creation, on reserve, on revalidation failure and on
dispatch, keyed by attempt identity. It is erased on a committed completion, on cancellation and when
its domain or worker incarnation is fenced (`erase_plan`), and it is *retained* after a failure
report - deliberately, so that `retry()` can re-run eligibility from the request the plan carries.
It is retired (`retire_plan`, which erases the stored plan and remembers the attempt as retired) when
`retry()` or a cancellation supersedes it. Cap: `SchedulerLimits::max_pending_plans` (65536),
enforced in `plan_internal` with `SelectionOutcome::kDeferred`.

### 4.2 Reservation (`ReservationState`, `include/tos/core/reservation.hpp`)

```text
   acquire()
      |
      v
  [ kActive ] --commit()--> [ kCommitted ] --release()--> [ kReleased ]
      |                          |
      |                          +--invalidate_domain()----> [ kInvalidated ]
      +--rollback()---------------------------------------> [ kRolledBack ]
      +--release()----------------------------------------> [ kReleased ]
      +--invalidate_domain()------------------------------> [ kInvalidated ]
```

`commit()` rejects `kCommitted` (`reservation.already_committed`) and any closed state
(`reservation.already_released`); `release()` rejects an already released, rolled back or
invalidated reservation; `rollback()` only applies to `kActive`. Each transition updates the
per-domain accounting exactly once.

### 4.3 Attempt (`AttemptState`, `AttemptResolution`, `include/tos/core/enums.hpp`)

```text
  register_attempt()  (before any external call)
        |
        v
   [ kDispatching ] --mark_dispatched()--> [ kDispatched ]
        |                                        |
        |                                        +-- commit_completion(success) --> [ kCompleted ]
        |                                        +-- commit_completion(failure) --> [ kFailed ]
        |                                        +-- mark_ambiguous() -----------> [ kOutcomeUnknown ]
        |                                        +-- mark_cancelled(crossed) ----> [ kCancelled ]
        |                                        +-- mark_fenced(may_have_effect)-> [ kFenced ]
        |
        +-- mark_failed() ---------------------> [ kFailed ]
        +-- mark_ambiguous() ------------------> [ kOutcomeUnknown ]
        +-- mark_cancelled(false) -------------> [ kCancelled ]
        +-- mark_fenced(false) ----------------> [ kFenced ]
        +-- mark_rejected_stale() -------------> [ kRejectedStale ]
```

The ledger never assigns `AttemptState::kPlanned` or `kReserved`: `register_attempt` inserts the
record directly as `kDispatching`, because the attempt must exist before the runtime calls out.
Terminal states are `kCompleted`, `kFailed`, `kOutcomeUnknown`, `kCancelled`, `kRejectedStale`
and `kFenced`; `completion_committed` is the separate "at most once" latch.

Resolution (`AttemptResolution`) is the durable classification: `kAuthoritativeSuccess`,
`kAuthoritativeFailure`, `kAmbiguous`, `kCancelledBeforeDispatch`, `kCancelledAmbiguous`,
`kFencedNoEffect`, `kFencedMayHaveEffect` (or `kNone` before classification). It is what retry
consults, and it is never optimistic: an ambiguous resolution blocks automatic replay.

### 4.4 Worker boot (`WorkerAuthority`, `include/tos/core/authority.hpp`)

```text
  register_boot(worker, boot)                       (rejected if the boot is fenced or owned elsewhere)
        |
        v
   [ current ] --register_boot(worker, new boot)--> previous boot [ fenced ]
        |
        +-- fence(worker, boot) ------------------> [ fenced ]
        +-- fence_boot(boot) ---------------------> [ fenced ]
        +-- worker session closes -----------------> [ fenced ]   (coordinator)
        +-- recovered from durable state ----------> [ fenced ]   (requires re-registration)
        +-- (note) the fenced set is trimmed at its bound when a boot registers,
            dropping the lowest values deterministically
```

A fenced boot can never be current again: `is_current` requires membership in `current` *and*
absence from `fenced`. Recovery fences every recovered boot; the coordinator fences a boot when its
session ends.

### 4.5 Coordinator epoch (`CoordinatorEpoch`)

```text
  Scheduler::start()
     base = max(options.initial_epoch, persisted_epoch_if_published)
     epoch = base + 1                                       --> new incarnation
  load_state_from()
     epoch = persisted last_epoch                           --> restored, then advanced by the next start()
  complete(submission)
     submission.coordinator_epoch != epoch  -> kStaleCoordinatorEpoch, state untouched
  heartbeat / failure report with another epoch -> session closed / rejected
```

Work planned under an old epoch can therefore never complete under a new one, even if the identities
happen to match.

---

## 5. Evidence and authority

The runtime separates two questions that are easy to conflate:

**Evidence** answers "what do we currently observe, and who proved it?" It is carried by
`CapabilityRecord` (with `provenance`, `authoritative` and an `EvidenceGeneration`),
`DomainLoadEvidence` (with load, queue and health generations and a `published()` predicate),
`DomainLocality`, `DomainTopology`, `DomainCompatibility`, the `IsolationClass`, the
`CapacityVector` and the `BackendGeneration`. Evidence is:

- **Generation-bound.** Every family carries its own counter, so a listener can tell "same evidence"
  from "evidence that moved" without comparing payloads.
- **Incarnation-bound.** `PublisherSequence` and `dynamic_evidence_current(live_boot)` make
  evidence from a dead or replaced process unusable, even when the bytes are identical.
- **Explicitly fallible.** `unproven_flags` records properties a publisher could *not* prove, and
  `CapabilitySet::canonicalize()` guarantees a flag can never be simultaneously proven and unproven.
  Absent is not the same as unknown, and unknown is never promoted to eligible.
- **Never upgraded by the runtime.** `validate_domain_record()` refuses a REAL capability under a
  SYNTHETIC domain and refuses an UNSUPPORTED domain with a REAL capability.

**Authority** answers "may this exact attempt run in this exact place, right now?" It is the
`AuthorityBinding` created at planning time and rechecked at dispatch, plus the three things
evidence cannot express on its own:

1. **The fence.** A fenced domain or a fenced worker incarnation is ineligible and any plan bound to
   it is rejected, regardless of how good its evidence looks (`authority.domain_fenced`,
   `IneligibilityReason::kDomainFenced`).
2. **The epoch.** Authority is scoped to one coordinator incarnation.
3. **The policy.** Authorization is decided under a specific policy generation, including the
   offload requirement, forbidden domains, provenance rules, minimum isolation, positive-evidence
   requirement and the reservation policy.

The plan-to-execution boundary is enforced in one place. `validate_authority(binding, live, freshness)` compares the
binding against `live_authority_of(record, epoch, policy_generation, isolation_generation)` - that
is, against the *current* domain record plus the *current* epoch and policy - and reports every field
that moved. Nothing else may authorize an execution: the plan path builds the binding, the dispatch
path validates it, and completion authority re-checks the attempt-side identities (attempt
generation, dispatch identity, worker incarnation, coordinator epoch, domain generation, capability
generation, operation identity, provenance) before it commits anything. Between the two checks there
is no window in which a decision can silently become a different decision.

---

## 6. Reading order for a new contributor

1. `include/tos/core/enums.hpp` - the vocabulary (domain classes, provenance, state machines,
   outcome codes).
2. `include/tos/core/identities.hpp` - identities, generations and `relate_generations`.
3. `include/tos/core/operation.hpp` and `capability.hpp` - what is asked for, and what is proven.
4. `include/tos/core/authority.hpp` - what makes a plan legal, and what invalidates it.
5. `include/tos/core/ranking.hpp` - the factor model and the total order.
6. `src/core/scheduler.cpp` - the locking doctrine and the whole decision path.
7. `docs/DEADLOCK_AUDIT.md` - the concurrency audit, including the one hazard that is *not*
   proven safe.
