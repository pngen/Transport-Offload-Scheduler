# Deadlock audit

Manual, mechanism-level audit of the concurrency in Transport Offload Scheduler 1.0.0.

**Scope.** The files named in the audit brief: `src/core/scheduler.cpp`, `src/core/attempt.cpp`,
`src/core/reservation.cpp`, `src/core/state.cpp`, `src/backends/local_channel.cpp`,
`src/util/thread_pool.cpp`, `src/dist/coordinator.cpp`, `src/dist/worker.cpp`. Supporting
material was read where a claim depends on it (`include/tos/**`, `src/core/authority.cpp`,
`src/util/net.cpp`, `src/util/log.cpp`, `src/backends/*.cpp`, `tests/**`).

**Method.** Every lock acquisition in the audited files was enumerated and classified by what is held
across it. A deadlock requires a cycle in the *wait-for* graph: a thread must hold one lock while
waiting for another. The audit therefore asks, for each acquisition site, "which locks are already
held here?" and, for each blocking wait (mutex, condition variable, join, socket close/shutdown),
"what must another thread do for this wait to be satisfied, and can that thread be blocked on
something this thread holds?" Each verdict below states that argument, not a test result.

**Citations.** Mechanisms in `src/core/scheduler.cpp` are cited by function or member name rather
than by line number, because that file is the one most likely to move; the other audited files are
cited by line number and were verified unchanged across the audit.

**This is a static review.** No dynamic deadlock detector was run while writing this document (the
build offers `TOS_ENABLE_ASAN`; there is no ThreadSanitizer configuration), and the test suite
deliberately declares no timeouts (`tests/tos_test_support.hpp:3-4`), so a hang would surface as a
hang rather than as a failure. The verdicts are arguments over the code as written.

**Summary.**

| # | Item | Verdict |
|---|------|---------|
| 1 | read -> write reentrancy | Safe: no read path takes a write lock; readers copy out |
| 2 | mutex re-entry through completion callbacks | Safe: the sink is copied out and invoked with no channel lock held |
| 3 | worker teardown while holding coordinator state | Safe for the teardown itself; see item 4 for the join inversion |
| 4 | session destruction on its own reader thread | **Not proven safe - lock-across-join inversion** (`coordinator.cpp:718-725`) |
| 5 | joining workers while holding attempt/reservation locks | Safe: no join site holds a ledger lock |
| 6 | shutdown waiting on a queue whose consumer is blocked by shutdown-held state | Safe: the pool is drained before the channel mutex is taken |
| 7 | event emission beneath locks | Safe by construction for hooks; one in-lock log call has no reverse edge |
| 8 | persistence beneath mutation locks | Safe: state is copied out, file I/O happens with no lock held |
| 9 | retry/fallback acquiring locks in reverse order | Safe: acquisitions are sequential, and the one nesting is forward-only |
| 10 | snapshot rendering that re-enters state | Safe: rendering is a const method on a value copy |
| 11 | transport close while another path waits on transport-owned locks | Safe: close takes no lock a sender holds |

---

## 1. read -> write reentrancy

**Mechanism.** `DomainRegistry` (`src/core/state.cpp`) guards one `std::shared_mutex` over the domain
map. Every read is a copy-out under a shared lock:

```cpp
bool DomainRegistry::get(ExecutionDomainId domain, ExecutionDomainRecord& out) const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  ...
  out = found->second;
  return true;
}
```
(`src/core/state.cpp:399-405`; the same shape appears in `find` 407-412, `find_by_name` 414-421,
`all` 423-432, `size` 434-437, `mutation_sequence` 439-442.)

The writers - `upsert` (64), `publish_capability` (207), `publish_load` (255), `publish_locality`
(289), `publish_topology` (306), `publish_compatibility` (321), `publish_capacity` (339),
`fence` (357), `fence_worker_boot` (372), `remove` (389), `mark_domain_evidence_stale` (444),
`mark_dynamic_evidence_stale` (461) - each take the exclusive lock and, inside it, touch only the
record they own. None of them calls another registry method, and none calls a scheduler method, a
hook or I/O.

**Why it cannot deadlock.** `std::shared_mutex` is not recursive and is not upgradeable here: a
thread that took the shared lock and then tried to take the exclusive lock on the same object would
deadlock against itself. That cannot happen because the two lock scopes never overlap in one call
stack. The one place where a caller reads and then writes the registry is
`LocalDomainPublisher::sync()`, and it does so *sequentially*, not nested:
`scheduler_->domains().get(record.id, existing)` (`src/backends/local_publisher.cpp:19`) returns
after releasing the shared lock, and only then does the function call
`scheduler_->register_domain(record)` (`:24`) or `scheduler_->update_domain(record)` (`:59`).
The same holds for the scheduler's own sequences: `Scheduler::dispatch()` calls
`impl_->domains.get(...)` to build `LiveAuthority` and does not write the registry;
`Scheduler::fence_worker_boot()` calls `impl_->domains.fence_worker_boot(...)` and then iterates the
copy returned by `impl_->domains.all()`; `LocalDomainPublisher::withdraw_missing()` iterates the copy
from `domains().all()` and calls `remove_domain` on it (`src/backends/local_publisher.cpp:124-131`).
Reading a snapshot of the map and acting on the snapshot is the pattern everywhere, so a thread never
waits for a lock it already holds.

## 2. mutex re-entry through completion callbacks

**Mechanism.** `LocalDispatchChannel` owns a `std::mutex` guarding its fault map, its delayed list
and its completion sink (`src/backends/local_channel.cpp:27-28`). The sink is the callback that
carries a completion back into the scheduler. It is never invoked under that mutex:

```cpp
void deliver(const CompletionSubmission& submission) {
  if (closed.load()) return;
  CompletionSink local_sink;
  {
    std::lock_guard<std::mutex> lock(mutex);
    local_sink = sink;          // copy the callable under the lock
  }
  if (local_sink) local_sink(submission);   // invoke it with the lock released
}
```
(`src/backends/local_channel.cpp:50-58`.)

The other two paths that can invoke the sink follow the same rule: `release_delayed()` swaps the
delayed vector out inside a scope and delivers after the scope closes (`:72-80`), and `send()`'s pool
task calls `impl_->deliver(...)` from outside any lock (`:141-161`); its only locked section is the
`kDelayedCompletion` push (`:147-151`), which returns without delivering.

**Why it cannot deadlock.** A re-entrant callback deadlocks only if the callback needs a lock the
caller still holds. Here the callback runs with the channel mutex released, and the callback's target
- `Scheduler::complete()` - acquires a completely different set of locks (one `AttemptLedger` shard,
then one `ReservationLedger` shard, then `plans_mutex`), each of which is released before the next is
taken (`src/core/attempt.cpp:207-312`, `src/core/reservation.cpp:148-181`, and the `store_plan` /
`erase_plan` helpers in `scheduler.cpp`). The channel mutex is not on that path at all, so the
wait-for graph has no edge from `Scheduler::complete` back to the channel mutex. Symmetrically,
`Scheduler::complete` never calls back into the channel, so no cycle can form.

## 3. worker teardown while holding coordinator state

**Mechanism.** Worker teardown runs in `OffloadCoordinator::Impl::on_session_closed()`
(`src/dist/coordinator.cpp:243-258`):

```cpp
void OffloadCoordinator::Impl::on_session_closed(const std::shared_ptr<Session>& session) {
  session->close();
  {
    std::lock_guard<std::mutex> lock(sessions_mutex);
    sessions.erase(session->id);
  }
  if (session->view.role == PeerRole::kWorker && session->view.registered) {
    const Status fenced = scheduler.fence_worker_boot(session->view.boot, "worker session closed");
    ...
  }
}
```

The dismissal of the session from the registry happens under `sessions_mutex`; the actual dismantling
of the worker incarnation (`fence_worker_boot`) happens *after* that scope has closed.

**Why it cannot deadlock (for the teardown itself).** The expensive, multi-component teardown of a
worker incarnation therefore holds no coordinator session lock. `Scheduler::fence_worker_boot()`
acquires its component locks strictly one at a time - `workers.fence_boot` (WorkerAuthority mutex),
`domains.fence_worker_boot` (registry exclusive), `domains.all()` (registry shared),
`reservations.invalidate_domain` (one reservation shard), `attempts.list_live()` (attempt shards, one
at a time) and the `mark_*` transitions (one attempt shard each) - and releases each before acquiring
the next. Since `sessions_mutex` is released first, the session lock is not in that chain at all. The
only lock shared between the two halves is `sessions_mutex`, and the ordering is always "session
lock, then release, then scheduler locks", so no cycle exists.

**Caveat.** This item is about the teardown path. The *join* of these same session threads is a
different story and is recorded as the finding under item 4: `OffloadCoordinator::stop()` joins the
session threads while holding `sessions_mutex`, and those threads need `sessions_mutex` to finish.

## 4. session destruction on its own reader thread

**Mechanism.** A session is never destroyed by the thread that reads it, and no thread joins itself:

- The session object is a `std::shared_ptr<Session>` held by the registry map *and* by the session
  thread's own lambda capture: `session_threads.emplace_back([this, session] { session_loop(session); })`
  (`src/dist/coordinator.cpp:200`, with the intent stated at `:198-199`). The thread therefore keeps
  the object alive for its whole lifetime.
- `std::thread` values live in `session_threads` and are joined by `stop()` on the stopping thread,
  never by a session thread.
- `on_session_closed()` (`:243-258`) does not destroy the session; it closes the socket, erases the
  registry entry and fences the incarnation.

**The hazard.** `OffloadCoordinator::stop()` joins those threads **while holding `sessions_mutex`**:

```cpp
  if (impl_->accept_thread.joinable()) impl_->accept_thread.join();
  {
    std::lock_guard<std::mutex> lock(impl_->sessions_mutex);
    for (std::thread& thread : impl_->session_threads) {
      if (thread.joinable()) thread.join();
    }
    impl_->session_threads.clear();
    impl_->sessions.clear();
  }
```
(`src/dist/coordinator.cpp:717-725`.)

Every session thread's final action is `on_session_closed(session)` (`:240`), which must acquire that
same `sessions_mutex` to erase itself (`:246`) - and, while still inside a frame-handling path, it may
need `sessions_mutex` even earlier, through `session_for_domain` (`:123-134`),
`session_owns_domain` (`:149-153`) or the domain-publication handler (`:319-322`).

So the wait-for graph contains a real edge: `stop()` holds `sessions_mutex` and waits for a session
thread; that session thread waits for `sessions_mutex`. If `stop()` acquires the mutex before any one
session thread reaches its erase, the coordinator hangs and cannot be stopped. The window between
`session->close()` (`:715`, which wakes a blocked reader) and the mutex acquisition at `:719` is
narrow - it contains only `accept_thread.join()` at `:717` - which is why a short-lived session often
finishes its erase first and the hang does not reproduce on every run. It is a race, not a guarantee.

**Why the existing tests do not rule it out.** `tests/test_lifecycle_limits.cpp:551-608`
(`lifecycle_coordinator_rebinds_the_same_port_after_stop`) does exactly the risky thing: it opens a
live, registered worker session (a raw protocol peer), asserts `session_count() == 1` and
`registered_worker_count() == 1`, and then calls `coordinator.stop()` in twelve consecutive cycles.
That test passing means the session thread won the race in those runs; it does not mean the race is
absent. The same applies to `tests/test_protocol_adversarial.cpp`, which calls `stop()` with client
sessions open, and to `tests/test_multiprocess.cpp:207`/`:297`, where the worker processes are
usually already dead and their session threads already finished.

**Verdict.** This item is **not proven safe**. The audit cannot certify it. The defect is a
lock-across-join inversion and is recorded here as a finding rather than as an accepted design. A
read-only audit cannot fix it (the fix is to release `sessions_mutex` before joining: copy the thread
handles out, join them unlocked, then re-take the lock to clear the containers).

## 5. joining workers while holding attempt/reservation locks

**Mechanism.** There are exactly two `join` sites in `src/` outside the thread pool:
`src/dist/coordinator.cpp:717` (accept thread) and `src/dist/coordinator.cpp:721` (session threads,
discussed in item 4). `src/util/thread_pool.cpp:83` is the pool's own join. Nothing in
`src/core/**` joins or creates a thread: `Scheduler` owns no threads, and the dispatch pool belongs
to `LocalDispatchChannel`. (`SchedulerLimits::worker_threads` exists as a declared limit but no code
under `src/` currently reads it; `SchedulerLimits::max_frame_bytes` is read once by the `Scheduler`
constructor to substitute a default.)

**Why it cannot deadlock.** No join site can be reached with a ledger shard lock held, because every
ledger method takes its shard inside the method and releases it before returning:

- `AttemptLedger` - `transition()` and `commit_completion()` hold one shard for the duration of a
  lookup and a field update (`src/core/attempt.cpp:71-89`, `:207-312`); `list`, `list_live`,
  `live_count`, `audit` and `prune_history` take and release shards one at a time in a loop
  (`:325-336`, `:356-368`, `:379-389`, `:391-424`, `:426-448`).
- `ReservationLedger` - the same pattern in `set_capacity` (49-63), `acquire` (65-107),
  `commit` (109-146), `release` (148-181), `rollback` (183-205), `invalidate_domain` (207-235),
  `get` (237-246), `list` (248-259), `has_capacity` (261-281), `outstanding_count` (283-296),
  `audit` (298-345).

The scheduler obeys the same rule at the call sites: `Scheduler::shutdown()` copies live attempts with
`impl_->attempts.list_live()`, runs the `mark_*` transitions on that copy, iterates
`impl_->domains.all()` calling `reservations.invalidate_domain` per domain, then copies the channel
handle out with `channel_copy()` and only afterwards calls `channel->shutdown()` - the call that
actually joins the dispatch pool. At that point no ledger lock, no registry lock and no plans lock is
held. `Scheduler::fence_domain()` and `Scheduler::fence_worker_boot()` follow the same copy-then-act
shape.

## 6. shutdown waiting on a queue whose consumer is blocked by shutdown-held state

**Mechanism.** Two shutdown paths matter, and both drain before locking.

`LocalDispatchChannel::shutdown()` (`src/backends/local_channel.cpp:173-179`):

```cpp
void LocalDispatchChannel::shutdown() {
  if (impl_->closed.exchange(true)) return;   // consumers stop delivering
  impl_->pool.shutdown();                     // joins the pool with NO channel lock held
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->delayed.clear();
  impl_->sink = nullptr;
}
```

`ThreadPool::shutdown()` (`src/util/thread_pool.cpp:69-90`) sets `stopping` inside a scope, releases
the mutex, notifies, and only then joins:

```cpp
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->stopping && impl_->threads.empty()) return;
    impl_->stopping = true;
  }
  impl_->available.notify_all();
  for (std::thread& thread : impl_->threads) { ... thread.join(); ... }
  impl_->threads.clear();
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->tasks.clear();
```

The worker loop pops a task *inside* the mutex and runs it *outside*
(`src/util/thread_pool.cpp:26-36`), so a running task never needs the pool mutex.

**Why it cannot deadlock.** A pool task that is executing an operation calls `impl_->deliver(...)` on
completion, which needs the *channel* mutex (`src/backends/local_channel.cpp:54`) and then the
scheduler's locks through the sink. During `pool.shutdown()` the channel mutex is **not** held - it is
taken only after every pool thread has been joined (`:176`). So a consumer is never blocked by state
that the shutdown path holds while it waits. The pool's own workers are likewise never blocked by the
pool mutex, because they hold it only while popping and while decrementing the active count
(`:26-35`, `:37-41`). The `drained` condition variable is declared (`:15`) and notified (`:40`) but
nothing ever waits on it, so no predicate can be starved by a held lock.

Two further details make the path total: a task submitted after shutdown is refused with
`pool.stopped` (`:61`), and `shutdown()` called from inside a pool thread detaches rather than
joining itself (`:78-81`) - the self-join that would otherwise be an unconditional deadlock. The
channel's `closed` flag (set first, checked in `deliver`) means in-flight tasks that finish after
shutdown began deliver nothing, so they cannot re-enter the scheduler after it has torn down.

## 7. event emission beneath locks

**Mechanism.** Two kinds of event emission exist: interleaving hooks and log lines.

Interleaving hooks: `Scheduler::Impl::hook` is `std::atomic<IInterleavingHook*>`; `Impl::fire()` loads
it, copies the context and calls it. There are 20 `fire()` call sites in `src/core/scheduler.cpp`
covering the 19 `HookPoint` values (`kAfterCompletionCommit` is fired on both the early stale-epoch
return and the normal path of `Scheduler::complete()`), and every one of them sits outside every lock
scope - the eligibility hook is fired after the `registry_mutex` scope has closed, and the reserve,
revalidate, reservation-commit, attempt-registration, channel-send, completion, fallback, cancellation
and shutdown-fence hooks are each fired with no scheduler lock held. The header states the contract
explicitly (`include/tos/core/hooks.hpp:5-7`): "Hooks are invoked with no internal lock held".

Logging: `log_write` takes a single process-wide mutex, formats a bounded line and writes it to
`stderr` (`src/util/log.cpp:15`, `:33-41`). It performs no callback and takes no other lock.

**Why it cannot deadlock.** A hook implementation that re-enters the scheduler cannot deadlock on an
internal lock, because none is held; the hooks header documents that this is deliberate
(`include/tos/core/hooks.hpp:5-7`). The only in-lock event emission in the audited files is a log
line inside the session-admission lock:

```cpp
    {
      std::lock_guard<std::mutex> lock(sessions_mutex);
      if (sessions.size() >= config.max_sessions) {
        log_write(LogLevel::kWarn, "coordinator", "session limit reached; refusing connection");
      } else { ... }
    }
```
(`src/dist/coordinator.cpp:192-203`.)

That establishes the edge `sessions_mutex -> log_mutex`. A cycle would need the reverse edge, and it
does not exist: `log_write` acquires only `log_mutex` and never `sessions_mutex`, and every other
`log_write` call in the audited files is made with no coordinator lock held - for example
`on_session_closed` logs at `src/dist/coordinator.cpp:254` *after* releasing `sessions_mutex`
(`:248`), and `Scheduler::fence_worker_boot` logs with no lock held. The one other emission point
worth naming is `Session::emit`/`emit_error` (`src/dist/coordinator.cpp:51-58`), which takes the
session's own `write_mutex`; it is never called while `sessions_mutex` is held (the
domain-publication handler's `emit_error` calls at `:316` and `:342` happen outside the locked scope
at `:319-322`).

## 8. persistence beneath mutation locks

**Mechanism.** Saving copies the state out, then writes with no lock held
(`Scheduler::save_state_to` in `src/core/scheduler.cpp`): the domain list is copied with
`impl_->domains.all()` (registry shared lock taken and released inside `all()`), the fenced boots with
`impl_->workers.fenced_boots()` (WorkerAuthority mutex released inside the call), and the attempts in
a loop over `impl_->attempts.list(...)` (attempt shards taken and released inside `list()`). Only
after all of that does it construct a `StateStore` and call `store.save(state)`, which performs the
atomic file replacement in `write_file_atomic`.

Loading is the mirror image: `StateStore::load()` reads and fully decodes the file *before* any
runtime lock is taken; phase one validates against the live runtime using `impl_->domains.get` /
`impl_->attempts.get`, each of which locks and unlocks internally; phase two applies through the
ordinary mutating APIs (`attempts.restore_attempt`, `domains.upsert`, `reservations.set_capacity`,
`mark_dynamic_evidence_stale`, `workers.fence_boot`), each of which takes exactly one lock for the
duration of one record.

The mutation-triggered saves follow the same rule: `Scheduler::fence_domain()` and
`Scheduler::fence_worker_boot()` persist only after their classification loops have completed and
released every lock. `Scheduler::shutdown()` saves after the channel has been shut down and the plans
map cleared, with no lock held.

**Why it cannot deadlock.** File I/O is a blocking operation, and the rule that matters is that no
lock is held across it. Here the only lock that could be held across the write would be one of the
runtime's own, and none is: every value written into `PersistedState` was already copied out, and
every value read back is applied one API call at a time. Persistence therefore adds no wait-for edge
between runtime locks and the filesystem; it can only add latency. (The bounded sizes -
`kMaxPersistedBytes` 64 MiB, `kMaxPersistedDomains` 65536, `kMaxPersistedAttempts` 200000 - bound
that latency.)

## 9. retry/fallback acquiring locks in reverse order

**Mechanism.** `Scheduler::retry()` acquires, in order: `impl_->attempts.get` (one attempt shard,
released inside the call); `impl_->policy_copy()` (shared policy lock, released);
`impl_->find_plan` (`plans_mutex`, released); then `plan_internal`, which itself takes
`policy_copy()`, the `registry_mutex` scope around hard eligibility, `plan_count()`
(`plans_mutex`), and `store_plan()` (`plans_mutex`); and finally `impl_->retire_plan`
(`plans_mutex`). Note that a failure report deliberately leaves the plan of record in place, so the
`find_plan` step succeeds after a reported failure; if it did not, retry would return
`retry.plan_not_retained` without touching any other lock.

`Scheduler::fallback()` acquires `policy_copy()`, then calls `plan_internal` once per candidate
domain class, each of which follows the sequence above.

**Why it cannot deadlock.** Every acquisition is released before the next one is taken; the only
*nested* acquisition in the whole path is inside `plan_internal`, where `registry_mutex` (doctrine
level 1) is held while `evaluate()` calls `workers.is_current` (level 5) and
`reservations.has_capacity` (level 6). That nesting runs *forward* down the documented order, and
neither `WorkerAuthority` (`src/core/authority.cpp`) nor `ReservationLedger` calls back into the
scheduler, the registry or the policy, so the reverse path a cycle would need does not exist. Note in
particular that the attempt ledger (level 7) is touched *before* the registry (level 1) in `retry()`
- which would be a reverse-order violation if the two overlapped, but `attempts.get` returns before
`plan_internal` is entered. Reverse *order* alone is not a deadlock; only nested reverse *acquisition*
is, and there is none. The plans lock (level 3) is likewise only ever taken by itself, never while a
registry, worker or ledger lock is held: `plan_count()` and `store_plan()` are outside the
`registry_mutex` scope, and `retire_plan` runs after `plan_internal` has returned.

## 10. snapshot rendering that re-enters state

**Mechanism.** `Scheduler::snapshot() const` builds a `SchedulerSnapshot` *value*: it copies the
domain list, the policy, the worker boot views (from `workers.live_boots()` and
`workers.fenced_boots()`), the reservations and the attempts, and reads the statistics.
`render_json()` and `render_text()` are const member functions of that value
(`src/core/snapshot.cpp:56`, `:252`) and have no pointer or reference to a `Scheduler`, a registry
or a ledger. `ReconciliationReport::render_text()` is the same shape
(`src/core/reconcile.cpp:15-34`).

**Why it cannot deadlock.** Rendering cannot re-enter runtime state because the rendering functions
have no handle on runtime state: the only inputs are the value's own members. The copy phase takes
each lock for the duration of one copy and releases it (`domains.all()`, `workers.live_boots()`,
`reservations.list()`, `attempts.list()`, and the `reports_mutex` scope inside `snapshot()`), and it
never nests two of them. The header states this as an invariant
(`include/tos/core/snapshot.hpp:43-45`): "Rendering never re-enters runtime state, so a snapshot can
be produced while the runtime is mutating without deadlock." `Scheduler::reconcile()` follows the
same pattern: it copies the recovered baseline under `recovered_mutex` in a scoped block, copies the
live domains, compares them with no lock held, and stores the report in a bounded history under
`reports_mutex`.

## 11. transport close while another path waits on transport-owned locks

**Mechanism.** A coordinator session has one lock, `write_mutex`, guarding writes only
(`src/dist/coordinator.cpp:26`). `send_frame` takes it, checks `closed`, and writes:

```cpp
[[nodiscard]] Status send_frame(MessageType type, const std::vector<std::uint8_t>& payload) {
  auto encoded = encode_frame(type, payload, max_frame);
  if (!encoded.ok()) return encoded.status;
  std::lock_guard<std::mutex> lock(write_mutex);
  if (closed.load()) return Status::failure("session.closed");
  frames_out.fetch_add(1, std::memory_order_relaxed);
  return socket.send_all(encoded.value.data(), encoded.value.size());
}
```

`close()` deliberately does **not** take that lock:

```cpp
void close() {
  if (closed.exchange(true)) return;
  socket.shutdown_both();
  socket.close();
}
```
(`src/dist/coordinator.cpp:33-40` and `:60-64`.)

**Why it cannot deadlock.** The closing thread acquires no lock at all, so it can never be waiting on
a lock that a writer holds; it only calls `shutdown_both()` and `close()` on the socket. A thread
blocked in `send_all` under `write_mutex` is released by the socket shutdown/close or by the write
completing or failing, and returns having released the mutex. The two directions are therefore
independent: the closer never waits for the writer's lock, and the writer never waits for the closer's
lock (it only observes `closed`). The same reasoning covers `OffloadCoordinator::stop()`, which sends
a shutdown request and then closes each session while holding no lock (`:708-716`) - the copy of the
session list it iterates was taken under `sessions_mutex` and the lock released at `:707`. On the
worker side, the socket has exactly one user, the `run()` loop (`src/dist/worker.cpp:401-497`), which
closes it at `:494` on the same thread that was blocked in `recv_exact`; there is no second thread to
contend with. (The coordinator's join of session threads is governed by item 4, not by this
mechanism - the join there is a `sessions_mutex` problem, not a socket-lock problem.)

---

## 12. Residual hazards and what this audit does not cover

1. **Lock-across-join in `OffloadCoordinator::stop()` (item 4).** `src/dist/coordinator.cpp:718-725`
   holds `sessions_mutex` while joining session threads whose epilogue needs `sessions_mutex`
   (`:246`). This is the single unresolved hazard found by this audit. It is a race with a narrow
   window, it is exercised by `tests/test_lifecycle_limits.cpp:551-608`, and it is recorded as a
   limitation in `README.md`.
2. **Blocking, not deadlock, in the shutdown paths.** `Scheduler::shutdown()` calls
   `channel->shutdown()`, which joins the dispatch pool; a long-running backend execution therefore
   delays shutdown for as long as that execution takes. This is by design (the pool drains rather than
   abandons work) and is not a deadlock, but it is an unbounded wait in wall-clock terms.
3. **No dynamic analysis.** No ThreadSanitizer or equivalent was run for this audit, and the suite has
   no timeouts by design (`tests/tos_test_support.hpp:3-4`). A hang would appear as a hang.
4. **Scope of the argument.** The "cannot deadlock" arguments above are arguments about *this* code as
   written. Three extension points can break them from the outside, and all three are stated as
   contracts in the headers: an `IInterleavingHook` implementation that blocks while another thread
   needs the scheduler could stall the runtime (hooks are called lock-free, so this is a hook defect,
   not a lock-order defect); an `IExecutionBackend::execute` implementation that never returns would
   block the dispatch pool and therefore `LocalDispatchChannel::shutdown()`
   (`include/tos/backends/backend.hpp:107-109` documents that backends may block on device work); and
   an `IDispatchChannel` implementation that calls back into the scheduler from inside `send()` would
   violate the documented contract at `include/tos/core/dispatch.hpp:151-153` (the local and remote
   channels both obey it).
5. **The one lock-ordering statement to remember.** The header comment at the top of
   `src/core/scheduler.cpp` is the normative order. The audit found no nested acquisition that runs
   against it; the only nested acquisition is `registry_mutex` -> `WorkerAuthority` /
   `ReservationLedger` inside `plan_internal`'s eligibility loop, which is forward.
