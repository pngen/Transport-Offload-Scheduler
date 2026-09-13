// Lifecycle and resource-bound limits.
//
// Repeated start/stop cycles must commit real work and leave no accounting open;
// shutdown with work in flight must classify conservatively; every configured bound
// must actually refuse the work that exceeds it.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "tos/dist/coordinator.hpp"
#include "tos/dist/protocol.hpp"
#include "tos_test_support.hpp"

using namespace tos;

namespace {

/// Reservation accounting only proves anything when capacity is actually reserved,
/// so these fixtures enable the reservation policy for one execution slot.
tos_test::TestRuntime::Config reservation_config() {
  tos_test::TestRuntime::Config config;
  config.policy = make_default_policy();
  config.policy.reservation.enabled = true;
  config.policy.reservation.per_operation[static_cast<std::size_t>(ResourceKind::kExecutionSlot)] = 1;
  return config;
}

/// A structurally valid synthetic domain record.
ExecutionDomainRecord synthetic_record(ExecutionDomainId id, ExecutionDomainType type,
                                       const std::string& name) {
  SyntheticBackend backend("lifecycle.probe");
  const Status added = backend.add_domain(make_synthetic_domain(id, type, name));
  if (!added.ok) {
    tos_test::report_failure(__FILE__, __LINE__, "synthetic add_domain failed: " + added.code);
    return {};
  }
  const std::vector<ExecutionDomainRecord> discovered = backend.discover_domains();
  if (discovered.empty()) {
    tos_test::report_failure(__FILE__, __LINE__, "synthetic discovery returned no domain");
    return {};
  }
  return discovered.front();
}

/// Dispatch one attempt whose channel send fails, which is the one path that leaves
/// a rejected plan retained in the plan map.
struct SendFailureAttempt {
  ExecutionAttemptId attempt;
  Status dispatch_status;
};

SendFailureAttempt dispatch_send_failure(tos_test::TestRuntime& runtime, std::uint32_t seed) {
  SendFailureAttempt result;
  runtime.channel().inject_fault(ChannelFault::kSendFailure);
  const std::vector<std::uint8_t> payload = tos_test::make_payload(256, seed);
  const OperationRequest request = tos_test::make_request(opclass::checksum_crc32c(), payload.size());
  const DispatchOutcome outcome = runtime.scheduler().plan_reserve_dispatch(
      request, std::span<const std::uint8_t>(payload.data(), payload.size()));
  TOS_CHECK_MSG(outcome.plan_result.planned, "the plan must be created before the send is attempted");
  TOS_CHECK_MSG(!outcome.dispatch_result.dispatched, "an injected send failure must not dispatch");
  result.attempt = outcome.plan_result.plan.attempt;
  result.dispatch_status = outcome.dispatch_result.status;
  return result;
}

// ---- raw protocol peer, used only to prove the coordinator serves a bound port --

class RawWorkerSession {
 public:
  explicit RawWorkerSession(std::uint16_t port) {
    Checked<TcpSocket> connected = connect_tcp("127.0.0.1", port);
    if (!connected.ok()) {
      tos_test::report_failure(__FILE__, __LINE__, "connect failed: " + connected.status.code);
      return;
    }
    socket_ = std::move(connected.value);
    socket_.set_nodelay(true);
    connected_ = true;
  }

  [[nodiscard]] bool connected() const noexcept { return connected_; }

  bool send(const std::vector<std::uint8_t>& bytes) {
    const Status sent = socket_.send_all(bytes.data(), bytes.size());
    if (!sent.ok) {
      tos_test::report_failure(__FILE__, __LINE__, "send failed: " + sent.code);
      return false;
    }
    return true;
  }

  Checked<dist::Frame> read_frame() {
    std::vector<std::uint8_t> header(dist::kFrameHeaderBytes);
    const Status read = socket_.recv_exact(header.data(), header.size());
    if (!read.ok) return Checked<dist::Frame>::bad(read.code, read.message);
    dist::FrameHeader parsed;
    const dist::DecodeError error =
        dist::decode_header(header.data(), header.size(), dist::kMaxFrameBytes, parsed);
    if (error != dist::DecodeError::kOk) {
      return Checked<dist::Frame>::bad(std::string("protocol.") +
                                       std::string(dist::to_string(error)));
    }
    std::vector<std::uint8_t> buffer(dist::kFrameHeaderBytes + parsed.length, 0);
    for (std::size_t i = 0; i < header.size(); ++i) buffer[i] = header[i];
    if (parsed.length > 0) {
      const Status body =
          socket_.recv_exact(buffer.data() + dist::kFrameHeaderBytes, parsed.length);
      if (!body.ok) return Checked<dist::Frame>::bad(body.code, body.message);
    }
    return dist::decode_frame(buffer.data(), buffer.size(), dist::kMaxFrameBytes);
  }

  /// Handshake as a worker incarnation and answer with the server's verdict.
  bool register_worker(WorkerId worker, WorkerBootId boot) {
    dist::HelloMessage hello;
    hello.role = dist::PeerRole::kWorker;
    hello.name = "lifecycle.worker";
    hello.worker = worker;
    hello.boot = boot;
    hello.host = "127.0.0.1";
    hello.pid = 99;
    hello.version = std::string(kVersionString);
    const Checked<std::vector<std::uint8_t>> encoded =
        dist::encode_frame(dist::MessageType::kHello, dist::encode(hello));
    if (!encoded.ok()) return false;
    if (!send(encoded.value)) return false;
    const Checked<dist::Frame> frame = read_frame();
    if (!frame.ok()) {
      tos_test::report_failure(__FILE__, __LINE__, "hello ack read failed: " + frame.status.code);
      return false;
    }
    TOS_CHECK_EQ(frame.value.type, dist::MessageType::kHelloAck);
    dist::HelloAckMessage ack;
    if (!dist::decode(frame.value.payload, ack)) return false;
    return ack.accepted;
  }

  /// A protocol query over the same connection proves the port serves the protocol.
  bool query_version() {
    dist::QueryRequestMessage query;
    query.kind = dist::QueryKind::kVersion;
    const Checked<std::vector<std::uint8_t>> encoded =
        dist::encode_frame(dist::MessageType::kQueryRequest, dist::encode(query));
    if (!encoded.ok()) return false;
    if (!send(encoded.value)) return false;
    const Checked<dist::Frame> frame = read_frame();
    if (!frame.ok()) {
      tos_test::report_failure(__FILE__, __LINE__, "query response read failed: " + frame.status.code);
      return false;
    }
    if (frame.value.type != dist::MessageType::kQueryResponse) return false;
    dist::QueryResponseMessage response;
    if (!dist::decode(frame.value.payload, response)) return false;
    return response.ok && !response.body.empty();
  }

  void close() { socket_.close(); }

 private:
  TcpSocket socket_;
  bool connected_{false};
};

}  // namespace

// ---------------------------------------------------------------------------
// Repeated start/stop cycles with real work in each cycle.
// ---------------------------------------------------------------------------

TOS_TEST(lifecycle_repeated_start_stop_cycles_commit_real_work) {
  constexpr std::size_t kCycles = 25;
  for (std::size_t cycle = 0; cycle < kCycles; ++cycle) {
    tos_test::TestRuntime runtime(reservation_config());
    const std::string suffix = std::to_string(cycle);
    const ExecutionDomainId domain = runtime.add_domain(make_synthetic_domain(
        ExecutionDomainId(500 + cycle), ExecutionDomainType::kDpu, "synthetic.dpu.cycle." + suffix));
    TOS_REQUIRE(domain.valid());

    const std::vector<std::uint8_t> payload =
        tos_test::make_payload(1024, static_cast<std::uint32_t>(cycle + 1));
    const OperationRequest request =
        tos_test::make_request(opclass::checksum_crc32c(), payload.size());
    const DispatchOutcome dispatched = runtime.scheduler().plan_reserve_dispatch(
        request, std::span<const std::uint8_t>(payload.data(), payload.size()));
    TOS_REQUIRE(dispatched.plan_result.planned);
    TOS_REQUIRE(dispatched.dispatch_result.dispatched);
    const ExecutionAttemptId attempt = dispatched.plan_result.plan.attempt;

    runtime.wait_for(attempt);
    const ExecutionAttempt settled = runtime.wait_for_terminal(attempt);
    TOS_CHECK_MSG(settled.state == AttemptState::kCompleted,
                  "cycle " + suffix + ": every cycle must reach a committed completion");
    TOS_CHECK(settled.completion_committed);
    TOS_CHECK_EQ(settled.result.result_digest,
                 static_cast<std::uint64_t>(crc32c(payload.data(), payload.size())));
    TOS_CHECK_EQ(runtime.commit_count(), std::size_t{1});

    // shutdown() is idempotent: the first call stops the runtime, every later call
    // reports the same success without touching anything again.
    TOS_REQUIRE(runtime.scheduler().shutdown().ok);
    TOS_CHECK(runtime.scheduler().shutdown().ok);
    TOS_CHECK(!runtime.scheduler().running());

    const AttemptAudit attempts = runtime.scheduler().attempts().audit();
    TOS_CHECK_MSG(attempts.consistent, "cycle " + suffix + ": " + attempts.detail);
    TOS_CHECK_EQ(attempts.live, std::uint64_t{0});
    TOS_CHECK_EQ(attempts.completed, std::uint64_t{1});
    TOS_CHECK_EQ(attempts.failed, std::uint64_t{0});
    TOS_CHECK_EQ(attempts.ambiguous, std::uint64_t{0});
    TOS_CHECK_EQ(attempts.fenced, std::uint64_t{0});
    TOS_CHECK_MSG(runtime.scheduler().attempts().list_live().empty(),
                  "cycle " + suffix + ": no attempt may be left live");

    const ReservationAudit reservations = runtime.scheduler().reservations().audit();
    TOS_CHECK_MSG(reservations.consistent,
                  "cycle " + suffix + ": reservation audit must stay consistent");
    TOS_CHECK_EQ(reservations.outstanding, std::uint64_t{0});
    TOS_CHECK_EQ(reservations.committed, std::uint64_t{0});
    TOS_CHECK_EQ(reservations.leaked, std::uint64_t{0});
    TOS_CHECK_EQ(runtime.scheduler().reservations().outstanding_count(), std::uint64_t{0});
  }
}

// ---------------------------------------------------------------------------
// Shutdown while work is in flight.
// ---------------------------------------------------------------------------

TOS_TEST(lifecycle_shutdown_classifies_in_flight_work_conservatively) {
  tos_test::TestRuntime runtime(reservation_config());
  const ExecutionDomainId domain = runtime.add_domain(make_synthetic_domain(
      ExecutionDomainId(4100), ExecutionDomainType::kDpu, "synthetic.dpu.shutdown"));
  TOS_REQUIRE(domain.valid());

  // A plan that exists but was never dispatched, kept for the post-shutdown refusal.
  const std::vector<std::uint8_t> pending_payload = tos_test::make_payload(64, 31);
  const PlanResult pending = runtime.scheduler().plan(
      tos_test::make_request(opclass::checksum_crc32c(), pending_payload.size()));
  TOS_REQUIRE(pending.planned);
  ExecutionPlan pending_plan = pending.plan;

  // A failed dispatch, whose plan stays retained in the plan map.
  const SendFailureAttempt failed = dispatch_send_failure(runtime, 32);
  TOS_REQUIRE(failed.attempt.valid());
  TOS_CHECK_EQ(failed.dispatch_status.code, std::string("channel.injected_send_failure"));

  // A successful dispatch whose completion is withheld.
  runtime.channel().inject_fault(ChannelFault::kDelayedCompletion);
  const std::vector<std::uint8_t> in_flight_payload = tos_test::make_payload(512, 33);
  const OperationRequest in_flight_request =
      tos_test::make_request(opclass::checksum_crc32c(), in_flight_payload.size());
  const DispatchOutcome in_flight = runtime.scheduler().plan_reserve_dispatch(
      in_flight_request,
      std::span<const std::uint8_t>(in_flight_payload.data(), in_flight_payload.size()));
  TOS_REQUIRE(in_flight.plan_result.planned);
  TOS_REQUIRE(in_flight.dispatch_result.dispatched);
  const ExecutionAttemptId attempt = in_flight.plan_result.plan.attempt;

  // The executor runs on a pool thread: wait until it has produced the completion
  // and the channel has withheld it, so the shutdown below is deterministic.
  tos_test::wait_until([&runtime] { return runtime.channel().delayed_count() == 1; });

  ExecutionAttempt before_shutdown;
  TOS_REQUIRE(runtime.scheduler().attempts().get(attempt, before_shutdown));
  TOS_CHECK_MSG(before_shutdown.state == AttemptState::kDispatched,
                "the attempt must have left DISPATCHING before shutdown");
  TOS_CHECK(!runtime.attempt_is_terminal(attempt));
  TOS_CHECK_EQ(runtime.total_deliveries(), std::size_t{0});
  TOS_CHECK_EQ(runtime.channel().delayed_count(), std::size_t{1});

  TOS_REQUIRE(runtime.scheduler().shutdown().ok);
  TOS_CHECK(runtime.scheduler().shutdown().ok);
  TOS_CHECK(!runtime.scheduler().running());

  // The in-flight attempt is classified conservatively, never as completed.
  ExecutionAttempt settled;
  TOS_REQUIRE(runtime.scheduler().attempts().get(attempt, settled));
  TOS_CHECK_MSG(settled.state == AttemptState::kOutcomeUnknown || settled.state == AttemptState::kFenced,
                std::string("an in-flight attempt must be classified conservatively, got ") +
                    std::string(to_string(settled.state)));
  TOS_CHECK(settled.state != AttemptState::kCompleted);
  TOS_CHECK(!settled.completion_committed);
  TOS_CHECK_EQ(settled.resolution, AttemptResolution::kAmbiguous);

  ExecutionAttempt failed_after;
  TOS_REQUIRE(runtime.scheduler().attempts().get(failed.attempt, failed_after));
  TOS_CHECK_EQ(failed_after.state, AttemptState::kFailed);
  TOS_CHECK(!failed_after.completion_committed);

  const AttemptAudit attempts = runtime.scheduler().attempts().audit();
  TOS_CHECK_MSG(attempts.consistent, attempts.detail);
  TOS_CHECK_EQ(attempts.live, std::uint64_t{0});
  TOS_CHECK_EQ(attempts.completed, std::uint64_t{0});
  TOS_CHECK(runtime.scheduler().attempts().list_live().empty());

  const ReservationAudit reservations = runtime.scheduler().reservations().audit();
  TOS_CHECK_MSG(reservations.consistent, reservations.detail);
  TOS_CHECK_EQ(reservations.outstanding, std::uint64_t{0});
  TOS_CHECK_EQ(reservations.committed, std::uint64_t{0});
  TOS_CHECK_EQ(reservations.leaked, std::uint64_t{0});
  TOS_CHECK_EQ(runtime.scheduler().reservations().outstanding_count(), std::uint64_t{0});

  // The plan map is empty after shutdown. There is no public plan-map observer, so
  // the claim is proven through retry(): the only way retry() may produce a new
  // generation is the retained plan of the failed attempt, and that plan was
  // dropped by shutdown(). The control in the next test shows the same attempt is
  // retryable while the plan is still retained.
  const RetryResult retry = runtime.scheduler().retry(failed.attempt);
  TOS_CHECK_MSG(!retry.retry_permitted, "a plan dropped by shutdown must not authorize a retry");
  TOS_CHECK_EQ(retry.status.code, std::string("retry.plan_not_retained"));

  // Dispatch is refused once shutdown has started.
  const DispatchResult refused = runtime.scheduler().dispatch(pending_plan);
  TOS_CHECK(!refused.dispatched);
  TOS_CHECK_EQ(refused.rejection, DispatchRejection::kShutdownInProgress);
  TOS_CHECK_EQ(refused.status.code, std::string("scheduler.shutting_down"));

  const PlanResult planned_after =
      runtime.scheduler().plan(tos_test::make_request(opclass::checksum_crc32c(), 64));
  TOS_CHECK(!planned_after.planned);
  TOS_CHECK_EQ(planned_after.status.code, std::string("scheduler.shutting_down"));
  TOS_CHECK_EQ(planned_after.explanation.outcome, SelectionOutcome::kDeferred);

  const Status reserved_after = runtime.scheduler().reserve(pending_plan);
  TOS_CHECK_EQ(reserved_after.code, std::string("scheduler.shutting_down"));

  const ExecutionDomainRecord late = synthetic_record(ExecutionDomainId(4199),
                                                      ExecutionDomainType::kDpu,
                                                      "synthetic.dpu.late");
  TOS_REQUIRE(late.id.valid());
  TOS_CHECK_EQ(runtime.scheduler().register_domain(late).code,
               std::string("scheduler.shutting_down"));
}

TOS_TEST(lifecycle_retained_plan_authorizes_retry_before_shutdown) {
  // Control for the plan-map probe above: without shutdown the very same failed
  // attempt still has its retained plan, so retry() plans a new generation.
  tos_test::TestRuntime runtime(reservation_config());
  const ExecutionDomainId domain = runtime.add_domain(make_synthetic_domain(
      ExecutionDomainId(4101), ExecutionDomainType::kDpu, "synthetic.dpu.retained"));
  TOS_REQUIRE(domain.valid());

  const SendFailureAttempt failed = dispatch_send_failure(runtime, 44);
  TOS_REQUIRE(failed.attempt.valid());
  const RetryResult retry = runtime.scheduler().retry(failed.attempt);
  TOS_CHECK_MSG(retry.retry_permitted,
                "a retained plan must authorize a retry: " + retry.status.code);
  TOS_CHECK_MSG(retry.status.code != "retry.plan_not_retained",
                "the plan must still be retained before shutdown");
  TOS_REQUIRE(runtime.scheduler().shutdown().ok);
}

// ---------------------------------------------------------------------------
// Resource bounds.
// ---------------------------------------------------------------------------

TOS_TEST(limits_pending_plan_bound_defers_new_plans) {
  tos_test::TestRuntime::Config config;
  config.limits.max_pending_plans = 1;
  tos_test::TestRuntime runtime(config);
  const ExecutionDomainId domain = runtime.add_domain(make_synthetic_domain(
      ExecutionDomainId(4200), ExecutionDomainType::kDpu, "synthetic.dpu.pending"));
  TOS_REQUIRE(domain.valid());
  TOS_CHECK_EQ(runtime.scheduler().limits().max_pending_plans, std::size_t{1});

  const OperationRequest request = tos_test::make_request(opclass::checksum_crc32c(), 128);
  const PlanResult first = runtime.scheduler().plan(request);
  TOS_REQUIRE(first.planned);

  const PlanResult second = runtime.scheduler().plan(request);
  TOS_CHECK_MSG(!second.planned, "a plan beyond the pending bound must be deferred");
  TOS_CHECK_EQ(second.explanation.outcome, SelectionOutcome::kDeferred);
  TOS_CHECK_EQ(second.status.code, std::string("plan.limit_reached"));
  TOS_CHECK(second.explanation.has_reason("plan.limit_reached"));
  TOS_REQUIRE(runtime.scheduler().shutdown().ok);
}

TOS_TEST(limits_operation_class_registration_stops_at_the_bound) {
  const std::size_t builtins = OperationClassRegistry::builtins().size();
  TOS_CHECK_EQ(builtins, std::size_t{9});

  SchedulerOptions options;
  options.policy = make_default_policy();
  options.limits.max_operation_classes = builtins + 1;
  options.host_node = "test.host";
  Scheduler scheduler(std::move(options));
  TOS_REQUIRE(scheduler.start().ok);
  TOS_CHECK_EQ(scheduler.operation_classes().size(), builtins);

  OperationClassDescriptor first;
  first.name = "AGENT_EXTRA_CLASS";
  first.category = "test";
  first.side_effect_class = SideEffectClass::kPure;
  first.transport_class = TransportClass::kRawFrames;
  first.payload_class = PayloadClass::kOpaqueBytes;
  const Checked<OperationClassId> accepted = scheduler.register_operation_class(first);
  TOS_CHECK_MSG(accepted.ok(),
                "the last registration inside the bound must succeed: " + accepted.status.code);
  TOS_REQUIRE(accepted.ok());
  TOS_CHECK_EQ(scheduler.operation_classes().size(), builtins + 1);

  OperationClassDescriptor second = first;
  second.name = "AGENT_EXTRA_CLASS_TWO";
  const Checked<OperationClassId> refused = scheduler.register_operation_class(second);
  TOS_CHECK_MSG(!refused.ok(), "a registration beyond the bound must be refused");
  TOS_CHECK_EQ(refused.status.code, std::string("operation.class_limit"));
  TOS_CHECK_EQ(scheduler.operation_classes().size(), builtins + 1);
  TOS_REQUIRE(scheduler.shutdown().ok);
}

TOS_TEST(limits_dispatch_payload_bound_refuses_oversized_requests) {
  tos_test::TestRuntime::Config config;
  config.limits.max_payload_dispatch_bytes = 128;
  tos_test::TestRuntime runtime(config);
  const ExecutionDomainId domain = runtime.add_domain(make_synthetic_domain(
      ExecutionDomainId(4201), ExecutionDomainType::kDpu, "synthetic.dpu.payload"));
  TOS_REQUIRE(domain.valid());
  TOS_CHECK_EQ(runtime.scheduler().limits().max_payload_dispatch_bytes, std::size_t{128});

  const std::vector<std::uint8_t> too_large = tos_test::make_payload(129, 5);
  const PlanResult refused = runtime.scheduler().plan(
      tos_test::make_request(opclass::checksum_crc32c(), too_large.size()), too_large);
  TOS_CHECK_MSG(!refused.planned, "a payload beyond the dispatch bound must be refused");
  TOS_CHECK_EQ(refused.status.code, std::string("plan.payload_too_large"));
  TOS_CHECK_EQ(refused.explanation.outcome, SelectionOutcome::kPolicyRejected);
  TOS_CHECK(refused.explanation.has_reason("plan.payload_too_large"));

  const std::vector<std::uint8_t> at_bound = tos_test::make_payload(128, 6);
  const PlanResult accepted = runtime.scheduler().plan(
      tos_test::make_request(opclass::checksum_crc32c(), at_bound.size()), at_bound);
  TOS_CHECK_MSG(accepted.planned, "a payload exactly at the bound must still be planned");
  TOS_REQUIRE(runtime.scheduler().shutdown().ok);
}

// DEFECT: SchedulerLimits::max_domains is never applied to the DomainRegistry.
// SchedulerOptions::limits is copied into Scheduler::Impl::limits, but the only
// consumer of a domain bound is DomainRegistry::Impl::max_domains, which stays at
// the compile-time kMaxExecutionDomains (100000) and has no setter. The documented
// bound therefore does not exist at runtime: register_domain() keeps accepting
// domains past limits().max_domains. This test asserts the specified behaviour
// (a registration beyond the configured bound is refused with "domain.limit_reached")
// and is expected to fail until the limit is wired through.
TOS_TEST(limits_domain_bound_refuses_registration_beyond_max_domains) {
  SchedulerOptions options;
  options.policy = make_default_policy();
  options.limits.max_domains = 2;
  options.host_node = "test.host";
  Scheduler scheduler(std::move(options));
  TOS_REQUIRE(scheduler.start().ok);
  TOS_CHECK_EQ(scheduler.limits().max_domains, std::size_t{2});

  for (std::uint64_t index = 1; index <= 2; ++index) {
    const ExecutionDomainRecord record = synthetic_record(
        ExecutionDomainId(index), ExecutionDomainType::kDpu,
        "synthetic.dpu.bound." + std::to_string(index));
    TOS_REQUIRE(record.id.valid());
    const Status registered = scheduler.register_domain(record);
    TOS_CHECK_MSG(registered.ok,
                  "registration inside the bound must succeed: " + registered.code);
  }

  const ExecutionDomainRecord beyond = synthetic_record(ExecutionDomainId(3),
                                                        ExecutionDomainType::kDpu,
                                                        "synthetic.dpu.bound.3");
  TOS_REQUIRE(beyond.id.valid());
  const Status refused = scheduler.register_domain(beyond);
  TOS_CHECK_MSG(!refused.ok,
                "registering a domain beyond SchedulerLimits::max_domains must be refused");
  TOS_CHECK_EQ(refused.code, std::string("domain.limit_reached"));
  TOS_CHECK_MSG(scheduler.domains().size() <= scheduler.limits().max_domains,
                "the registry must never grow past the configured domain bound");
  TOS_REQUIRE(scheduler.shutdown().ok);
}

TOS_TEST(limits_thread_pool_refuses_work_beyond_its_queue_bound) {
  ThreadPool pool(1, 1);
  TOS_CHECK_EQ(pool.thread_count(), std::size_t{1});
  TOS_CHECK_EQ(pool.queued(), std::size_t{0});

  std::mutex mutex;
  std::condition_variable condition;
  bool started = false;
  bool released = false;

  const Status first = pool.submit([&mutex, &condition, &started, &released] {
    std::unique_lock<std::mutex> lock(mutex);
    started = true;
    condition.notify_all();
    condition.wait(lock, [&released] { return released; });
  });
  TOS_CHECK_MSG(first.ok, "the first task must be accepted: " + first.code);
  TOS_REQUIRE(first.ok);
  {
    // Wait for the worker to be inside the blocking task: the queue is then empty.
    std::unique_lock<std::mutex> lock(mutex);
    condition.wait(lock, [&started] { return started; });
  }
  TOS_CHECK_EQ(pool.active(), std::size_t{1});
  TOS_CHECK_EQ(pool.queued(), std::size_t{0});

  const Status second = pool.submit([] {});
  TOS_CHECK_MSG(second.ok, "the queue bound of one must still accept one task: " + second.code);
  TOS_CHECK_EQ(pool.queued(), std::size_t{1});

  const Status third = pool.submit([] {});
  TOS_CHECK_MSG(!third.ok, "a task beyond the queue bound must be refused");
  TOS_CHECK_EQ(third.code, std::string("pool.full"));
  TOS_CHECK_EQ(pool.queued(), std::size_t{1});

  {
    std::lock_guard<std::mutex> lock(mutex);
    released = true;
  }
  condition.notify_all();
  pool.shutdown();
  TOS_CHECK(pool.stopped());
}

TOS_TEST(limits_frame_encoder_refuses_oversized_payload) {
  const std::size_t max_frame = dist::kFrameHeaderBytes + 16;
  const std::vector<std::uint8_t> too_large(17, 0x33U);
  const Checked<std::vector<std::uint8_t>> refused =
      dist::encode_frame(dist::MessageType::kHeartbeat, too_large, max_frame);
  TOS_CHECK(!refused.ok());
  TOS_CHECK_EQ(refused.status.code, std::string("protocol.oversized_payload"));

  const std::vector<std::uint8_t> at_bound(16, 0x33U);
  const Checked<std::vector<std::uint8_t>> accepted =
      dist::encode_frame(dist::MessageType::kHeartbeat, at_bound, max_frame);
  TOS_CHECK(accepted.ok());
  TOS_CHECK_EQ(accepted.value.size(), max_frame);

  const std::vector<std::uint8_t> huge(2U * 1024U * 1024U, 0);
  const Checked<std::vector<std::uint8_t>> refused_huge =
      dist::encode_frame(dist::MessageType::kHeartbeat, huge, dist::kMaxFrameBytes);
  TOS_CHECK(!refused_huge.ok());
  TOS_CHECK_EQ(refused_huge.status.code, std::string("protocol.oversized_payload"));
}

// ---------------------------------------------------------------------------
// Coordinator lifecycle on one reused port.
// ---------------------------------------------------------------------------

// Regression guard: a live, registered worker session is held open across stop()
// for every cycle. stop() used to join the session threads while holding
// sessions_mutex, which on_session_closed() needs in order to erase the session: a
// wait-for cycle that hung the coordinator (reproduced: two threads, no CPU use, the
// session socket left in CloseWait and the listening port refusing connections while
// the call never returned). Threads are now swapped out and joined with no
// coordinator lock held. If this test ever hangs again, that class of defect is back,
// and a hang here is the defect -- not a missing watchdog.
TOS_TEST(lifecycle_coordinator_rebinds_the_same_port_after_stop) {
  std::uint16_t port = 0;
  {
    dist::CoordinatorConfig probe_config;
    probe_config.bind_host = "127.0.0.1";
    probe_config.port = 0;
    dist::OffloadCoordinator probe(std::move(probe_config));
    TOS_REQUIRE(probe.start().ok);
    port = probe.port();
    TOS_CHECK(port != 0);
    TOS_REQUIRE(probe.stop().ok);
    TOS_CHECK_EQ(probe.session_count(), std::size_t{0});
  }

  constexpr std::size_t kCycles = 12;
  for (std::size_t cycle = 0; cycle < kCycles; ++cycle) {
    dist::CoordinatorConfig config;
    config.bind_host = "127.0.0.1";
    config.port = port;
    dist::OffloadCoordinator coordinator(std::move(config));
    const Status started = coordinator.start();
    TOS_REQUIRE(started.ok);
    TOS_CHECK_EQ(coordinator.port(), port);
    TOS_CHECK(coordinator.running());
    TOS_CHECK_EQ(coordinator.session_count(), std::size_t{0});
    TOS_CHECK_EQ(coordinator.registered_worker_count(), std::size_t{0});

    // A live, registered worker session is open across stop(): the coordinator must
    // join it and leave nothing behind.
    RawWorkerSession session(port);
    TOS_REQUIRE(session.connected());
    const WorkerId worker(0x9000 + cycle);
    const WorkerBootId boot(0xA000 + cycle);
    TOS_CHECK_MSG(session.register_worker(worker, boot), "the reused port must accept a handshake");
    TOS_CHECK_EQ(coordinator.session_count(), std::size_t{1});
    TOS_CHECK_EQ(coordinator.registered_worker_count(), std::size_t{1});
    TOS_CHECK_MSG(session.query_version(), "the reused port must serve the protocol");
    TOS_CHECK(coordinator.scheduler().workers().is_current(worker, boot));

    TOS_REQUIRE(coordinator.stop().ok);
    TOS_CHECK(!coordinator.running());
    TOS_CHECK_EQ(coordinator.session_count(), std::size_t{0});
    TOS_CHECK_EQ(coordinator.registered_worker_count(), std::size_t{0});
    TOS_CHECK(!coordinator.scheduler().running());
    TOS_CHECK(coordinator.stop().ok);
    session.close();
  }
}
