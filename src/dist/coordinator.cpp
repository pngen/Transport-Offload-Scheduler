// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "tos/dist/coordinator.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

#include "tos/util/log.hpp"
#include "tos/version.hpp"

namespace tos {
namespace dist {

struct OffloadCoordinator::Impl {
  /// One worker or client connection. The socket is owned by the session; the thread
  /// that runs it is owned by the coordinator, so a session can never join itself.
  struct Session {
    Impl* owner{nullptr};
    std::uint64_t id{0};
    std::size_t max_frame{kMaxFrameBytes};
    TcpSocket socket;
    mutable std::mutex write_mutex;
    std::atomic<bool> closed{false};
    SessionView view;
    std::map<std::uint64_t, ExecutionDomainId> domains;  ///< guarded by owner->sessions_mutex
    std::atomic<std::uint64_t> frames_in{0};
    std::atomic<std::uint64_t> frames_out{0};

    [[nodiscard]] Status send_frame(MessageType type, const std::vector<std::uint8_t>& payload) {
      auto encoded = encode_frame(type, payload, max_frame);
      if (!encoded.ok()) return encoded.status;
      std::lock_guard<std::mutex> lock(write_mutex);
      if (closed.load()) return Status::failure("session.closed");
      frames_out.fetch_add(1, std::memory_order_relaxed);
      return socket.send_all(encoded.value.data(), encoded.value.size());
    }

    [[nodiscard]] Status send_error(std::string code, std::string detail) {
      ErrorMessage message;
      message.code = std::move(code);
      message.detail = std::move(detail);
      return send_frame(MessageType::kError, encode(message));
    }

    /// Best-effort variants for paths where a failure to reply cannot change the
    /// decision that has already been made.
    void emit_error(std::string code, std::string detail) {
      const Status ignored = send_error(std::move(code), std::move(detail));
      (void)ignored;
    }
    void emit(MessageType type, const std::vector<std::uint8_t>& payload) {
      const Status ignored = send_frame(type, payload);
      (void)ignored;
    }

    void close() {
      if (closed.exchange(true)) return;
      socket.shutdown_both();
      socket.close();
    }
  };

  explicit Impl(CoordinatorConfig config_in)
      : config(std::move(config_in)), scheduler(config.scheduler) {}

  CoordinatorConfig config;
  Scheduler scheduler;
  SocketRuntime sockets;
  TcpListener listener;
  std::shared_ptr<IDispatchChannel> channel;
  std::atomic<bool> running{false};
  std::atomic<bool> stopping{false};
  std::atomic<std::uint64_t> next_session_id{0};

  mutable std::mutex sessions_mutex;
  std::map<std::uint64_t, std::shared_ptr<Session>> sessions;
  std::vector<std::thread> session_threads;
  std::thread accept_thread;

  /// Dispatch channel that routes envelopes to the session owning a domain.
  class RemoteChannel : public IDispatchChannel {
   public:
    explicit RemoteChannel(Impl* owner) : owner_(owner) {}

    [[nodiscard]] std::string_view name() const noexcept override { return "tcp.worker"; }

    [[nodiscard]] Status send(const DispatchEnvelope& envelope) override {
      std::shared_ptr<Session> session = owner_->session_for_domain(envelope.domain);
      if (!session) {
        return Status::failure("channel.no_worker",
                               "no live worker session owns domain " +
                                   format_id(envelope.domain.value()));
      }
      DispatchRequestMessage message;
      message.envelope = envelope;
      const Status sent = session->send_frame(MessageType::kDispatchRequest, encode(message));
      if (!sent) return sent;
      log_write(LogLevel::kDebug, "coordinator",
                "dispatched attempt " + format_id(envelope.attempt.value()) + " to domain " +
                    format_id(envelope.domain.value()));
      return Status::success();
    }

    bool request_cancel(const CancelEnvelope& envelope) override {
      std::shared_ptr<Session> session = owner_->session_for_domain(envelope.domain);
      if (!session) return false;
      CancelRequestMessage message;
      message.envelope = envelope;
      const Status sent = session->send_frame(MessageType::kCancelRequest, encode(message));
      return static_cast<bool>(sent);
    }

    void shutdown() override {}

   private:
    Impl* owner_;
  };

  [[nodiscard]] std::shared_ptr<Session> session_for_domain(ExecutionDomainId domain) {
    std::lock_guard<std::mutex> lock(sessions_mutex);
    for (const auto& entry : sessions) {
      const std::shared_ptr<Session>& session = entry.second;
      if (!session->closed.load() && session->view.registered &&
          session->view.role == PeerRole::kWorker &&
          session->domains.count(domain.value()) != 0U) {
        return session;
      }
    }
    return nullptr;
  }

  mutable std::mutex stop_mutex;
  std::condition_variable stop_cv;
  bool stop_signalled{false};

  void request_stop() {
    {
      std::lock_guard<std::mutex> lock(stop_mutex);
      stop_signalled = true;
    }
    stopping.store(true);
    stop_cv.notify_all();
  }

  [[nodiscard]] bool session_owns_domain(const Session& session,
                                         ExecutionDomainId domain) const {
    std::lock_guard<std::mutex> lock(sessions_mutex);
    return session.domains.count(domain.value()) != 0U;
  }

  [[nodiscard]] Status validate_worker_identity(const Session& session, WorkerId worker,
                                                WorkerBootId boot) const {
    if (session.view.role != PeerRole::kWorker || !session.view.registered) {
      return Status::failure("session.not_registered");
    }
    if (session.view.worker != worker || session.view.boot != boot) {
      return Status::failure("session.identity_mismatch",
                             "frame identity does not match the registered incarnation");
    }
    return Status::success();
  }

  void accept_loop();
  void session_loop(std::shared_ptr<Session> session);
  void handle_frame(Session& session, const Frame& frame);
  void on_session_closed(const std::shared_ptr<Session>& session);
};

void OffloadCoordinator::Impl::accept_loop() {
  while (!stopping.load()) {
    auto accepted = listener.accept_one();
    if (!accepted.ok()) {
      if (stopping.load()) return;
      if (accepted.status.code == "net.listener_closed") return;
      log_write(LogLevel::kWarn, "coordinator", "accept failed: " + accepted.status.code);
      continue;
    }
    if (stopping.load()) {
      // A connection that completed while shutdown was starting is dropped instead of
      // becoming a session that outlives the stop.
      accepted.value.close();
      return;
    }
    auto session = std::make_shared<Session>();
    session->owner = this;
    session->id = next_session_id.fetch_add(1, std::memory_order_relaxed) + 1;
    session->max_frame = config.max_frame_bytes;
    session->socket = std::move(accepted.value);
    session->socket.set_nodelay(true);
    session->view.session_id = session->id;
    session->view.peer = session->socket.peer_text();

    bool admitted = false;
    {
      std::lock_guard<std::mutex> lock(sessions_mutex);
      if (sessions.size() >= config.max_sessions) {
        log_write(LogLevel::kWarn, "coordinator", "session limit reached; refusing connection");
      } else {
        sessions[session->id] = session;
        // The thread holds its own reference for its whole lifetime, so the session
        // outlives its registry entry without anybody joining it from inside itself.
        session_threads.emplace_back([this, session] { session_loop(session); });
        admitted = true;
      }
    }
    if (!admitted) {
      session->emit_error("coordinator.session_limit", "too many sessions");
      session->close();
    }
  }
}

void OffloadCoordinator::Impl::session_loop(std::shared_ptr<Session> session) {
  std::vector<std::uint8_t> header(kFrameHeaderBytes);
  std::vector<std::uint8_t> buffer;
  while (!stopping.load() && !session->closed.load()) {
    const Status read_header = session->socket.recv_exact(header.data(), kFrameHeaderBytes);
    if (!read_header) break;
    FrameHeader parsed;
    const DecodeError error =
        decode_header(header.data(), header.size(), session->max_frame, parsed);
    if (error != DecodeError::kOk) {
      session->emit_error(std::string("protocol.") + std::string(to_string(error)),
                          "frame header rejected");
      break;
    }
    buffer.assign(kFrameHeaderBytes + parsed.length, 0);
    std::copy(header.begin(), header.end(), buffer.begin());
    if (parsed.length > 0) {
      const Status read_body =
          session->socket.recv_exact(buffer.data() + kFrameHeaderBytes, parsed.length);
      if (!read_body) break;
    }
    auto frame = decode_frame(buffer.data(), buffer.size(), session->max_frame);
    if (!frame.ok()) {
      session->emit_error(frame.status.code, "frame rejected");
      break;
    }
    session->frames_in.fetch_add(1, std::memory_order_relaxed);
    handle_frame(*session, frame.value);
  }
  on_session_closed(session);
}

void OffloadCoordinator::Impl::on_session_closed(const std::shared_ptr<Session>& session) {
  session->close();
  {
    std::lock_guard<std::mutex> lock(sessions_mutex);
    sessions.erase(session->id);
  }
  if (session->view.role == PeerRole::kWorker && session->view.registered) {
    // Losing the connection is the authoritative signal that the incarnation is
    // gone: fence it, invalidate its domains and classify its in-flight attempts.
    const Status fenced = scheduler.fence_worker_boot(session->view.boot, "worker session closed");
    (void)fenced;
    log_write(LogLevel::kWarn, "coordinator",
              "worker boot " + format_id(session->view.boot.value()) +
                  " fenced after its session closed");
  }
}

void OffloadCoordinator::Impl::handle_frame(Session& session, const Frame& frame) {
  switch (frame.type) {
    case MessageType::kHello: {
      HelloMessage hello;
      if (!decode(frame.payload, hello)) {
        session.emit_error("protocol.malformed_hello", "hello payload rejected");
        session.close();
        return;
      }
      HelloAckMessage ack;
      ack.epoch = scheduler.coordinator_epoch();
      if (hello.role == PeerRole::kWorker) {
        const Status registered =
            scheduler.register_worker_boot(hello.worker, hello.boot, hello.name);
        if (!registered) {
          ack.accepted = false;
          ack.code = registered.code;
          ack.detail = registered.message;
          session.emit(MessageType::kHelloAck, encode(ack));
          session.close();
          return;
        }
        session.view.role = PeerRole::kWorker;
        session.view.name = hello.name;
        session.view.worker = hello.worker;
        session.view.boot = hello.boot;
        session.view.registered = true;
        ack.accepted = true;
        ack.code = "ok";
        session.emit(MessageType::kHelloAck, encode(ack));
        log_write(LogLevel::kInfo, "coordinator",
                  "worker " + hello.name + " registered boot " + format_id(hello.boot.value()));
        return;
      }
      session.view.role = hello.role;
      session.view.name = hello.name;
      session.view.registered = true;
      ack.accepted = true;
      ack.code = "ok";
      session.emit(MessageType::kHelloAck, encode(ack));
      return;
    }
    case MessageType::kDomainPublish: {
      DomainPublishMessage message;
      if (!decode(frame.payload, message)) {
        session.emit_error("protocol.malformed_domain", "domain record rejected");
        return;
      }
      const Status identity =
          validate_worker_identity(session, message.record.worker, message.record.worker_boot);
      if (!identity) {
        session.emit_error(identity.code, identity.message);
        return;
      }
      auto updated = scheduler.update_domain(message.record);
      if (!updated.ok()) {
        session.emit_error(updated.status.code, updated.status.message);
        return;
      }
      {
        std::lock_guard<std::mutex> lock(sessions_mutex);
        session.domains[message.record.id.value()] = message.record.id;
      }
      log_write(LogLevel::kInfo, "coordinator",
                "domain " + format_id(message.record.id.value()) + " (" +
                    std::string(to_string(message.record.type)) + "/" +
                    std::string(to_string(message.record.provenance)) + ") published by boot " +
                    format_id(message.record.worker_boot.value()));
      return;
    }
    case MessageType::kCapabilityPublish: {
      CapabilityPublishMessage message;
      if (!decode(frame.payload, message)) {
        session.emit_error("protocol.malformed_capability", "capability record rejected");
        return;
      }
      if (!session_owns_domain(session, message.capability.domain)) {
        session.emit_error("session.domain_not_owned",
                           "this session does not own the domain it published capability for");
        return;
      }
      const Status published = scheduler.publish_capability(message.capability);
      if (!published) session.emit_error(published.code, published.message);
      return;
    }
    case MessageType::kLoadUpdate: {
      LoadUpdateMessage message;
      if (!decode(frame.payload, message)) {
        session.emit_error("protocol.malformed_load", "load evidence rejected");
        return;
      }
      if (!session_owns_domain(session, message.domain)) {
        session.emit_error("session.domain_not_owned",
                           "this session does not own the domain it published load for");
        return;
      }
      const Status published = scheduler.publish_load(message.domain, message.load);
      if (!published) session.emit_error(published.code, published.message);
      return;
    }
    case MessageType::kHeartbeat: {
      HeartbeatMessage message;
      if (!decode(frame.payload, message)) {
        session.emit_error("protocol.malformed_heartbeat", "heartbeat rejected");
        return;
      }
      const Status identity = validate_worker_identity(session, message.worker, message.boot);
      if (!identity) {
        session.emit_error(identity.code, identity.message);
        session.close();
        return;
      }
      if (message.epoch != scheduler.coordinator_epoch()) {
        session.emit_error("coordinator.stale_epoch",
                           "heartbeat epoch does not match the current coordinator epoch");
        session.close();
        return;
      }
      return;
    }
    case MessageType::kCompletion: {
      CompletionMessage message;
      if (!decode(frame.payload, message)) {
        session.emit_error("protocol.malformed_completion", "completion rejected");
        return;
      }
      log_write(LogLevel::kInfo, "coordinator",
                "completion received for attempt " + format_id(message.submission.attempt.value()));
      const Status identity = validate_worker_identity(session, message.submission.worker,
                                                       message.submission.worker_boot);
      if (!identity) {
        session.emit_error(identity.code, identity.message);
        return;
      }
      const CompletionOutcome outcome = scheduler.complete(message.submission);
      log_write(LogLevel::kInfo, "coordinator",
                std::string("completion for attempt ") +
                    format_id(message.submission.attempt.value()) + ": " +
                    std::string(to_string(outcome.rejection)) +
                    (outcome.committed ? " committed" : " not_committed"));
      CompletionAckMessage ack;
      ack.attempt = message.submission.attempt;
      ack.rejection = outcome.rejection;
      ack.committed = outcome.committed;
      ack.idempotent = outcome.idempotent;
      session.emit(MessageType::kCompletionAck, encode(ack));
      return;
    }
    case MessageType::kFailureReport: {
      FailureReportMessage message;
      if (!decode(frame.payload, message)) {
        session.emit_error("protocol.malformed_failure", "failure report rejected");
        return;
      }
      const Status identity = validate_worker_identity(session, session.view.worker, message.boot);
      if (!identity) {
        session.emit_error(identity.code, identity.message);
        return;
      }
      if (message.epoch != scheduler.coordinator_epoch()) {
        session.emit_error("coordinator.stale_epoch", "failure report carries a stale epoch");
        return;
      }
      const Status reported =
          scheduler.report_failure(message.attempt, message.kind, message.detail);
      if (!reported) session.emit_error(reported.code, reported.message);
      return;
    }
    case MessageType::kSubmitRequest: {
      SubmitRequestMessage message;
      if (!decode(frame.payload, message)) {
        session.emit_error("protocol.malformed_submit", "submit request rejected");
        return;
      }
      SubmitResponseMessage response;
      const std::span<const std::uint8_t> payload(message.payload.data(), message.payload.size());
      if (message.dispatch_now) {
        DispatchOutcome outcome = scheduler.plan_reserve_dispatch(message.request, payload);
        response.planned = outcome.plan_result.planned;
        response.dispatched = outcome.dispatch_result.dispatched;
        response.outcome = outcome.plan_result.explanation.outcome;
        response.operation = outcome.plan_result.plan.operation.value();
        response.domain = outcome.plan_result.plan.domain.value();
        response.domain_type = outcome.plan_result.plan.domain_type;
        response.provenance = outcome.plan_result.plan.provenance;
        response.attempt = outcome.plan_result.plan.attempt.value();
        response.attempt_generation = outcome.plan_result.plan.attempt_generation;
        response.attempt_state = outcome.dispatch_result.state;
        response.dispatch_rejection = outcome.dispatch_result.rejection;
        response.code = outcome.dispatch_result.dispatched
                           ? "dispatched"
                           : (outcome.plan_result.status.ok ? outcome.dispatch_result.status.code
                                                            : outcome.plan_result.status.code);
        response.detail = outcome.dispatch_result.dispatched
                              ? std::string()
                              : outcome.dispatch_result.status.message;
        response.explanation_json = render_explanation_json(outcome.plan_result.explanation);
      } else {
        PlanResult planned = scheduler.plan(message.request, payload);
        response.planned = planned.planned;
        response.outcome = planned.explanation.outcome;
        response.operation = planned.plan.operation.value();
        response.domain = planned.plan.domain.value();
        response.domain_type = planned.plan.domain_type;
        response.provenance = planned.plan.provenance;
        response.attempt = planned.plan.attempt.value();
        response.attempt_generation = planned.plan.attempt_generation;
        response.code = planned.status.ok ? "planned" : planned.status.code;
        response.detail = planned.status.message;
        response.explanation_json = render_explanation_json(planned.explanation);
      }
      session.emit(MessageType::kSubmitResponse, encode(response));
      return;
    }
    case MessageType::kQueryRequest: {
      QueryRequestMessage message;
      if (!decode(frame.payload, message)) {
        session.emit_error("protocol.malformed_query", "query rejected");
        return;
      }
      QueryResponseMessage response;
      switch (message.kind) {
        case QueryKind::kSnapshot:
          response.ok = true;
          response.code = "ok";
          response.body = scheduler.snapshot().render_json();
          break;
        case QueryKind::kReconcile:
          response.ok = true;
          response.code = "ok";
          response.body = scheduler.reconcile().render_text();
          break;
        case QueryKind::kVersion:
          response.ok = true;
          response.code = "ok";
          response.body = std::string(kProjectName) + " " + std::string(kVersionString) +
                          " protocol=" + std::to_string(kProtocolVersion) +
                          " epoch=" + std::to_string(scheduler.coordinator_epoch().value());
          break;
        case QueryKind::kAccounting: {
          const ReservationAudit audit = scheduler.reservations().audit();
          const AttemptAudit attempts = scheduler.attempts().audit();
          response.ok = true;
          response.code = "ok";
          response.body = std::string("reservations consistent=") + (audit.consistent ? "yes" : "no") +
                          " outstanding=" + std::to_string(audit.outstanding) +
                          " committed=" + std::to_string(audit.committed) +
                          " held_not_dispatched=" + std::to_string(audit.leaked) +
                          "\nattempts total=" + std::to_string(attempts.total) +
                          " live=" + std::to_string(attempts.live) +
                          " completed=" + std::to_string(attempts.completed) +
                          " failed=" + std::to_string(attempts.failed) +
                          " ambiguous=" + std::to_string(attempts.ambiguous) +
                          " fenced=" + std::to_string(attempts.fenced) +
                          " conflicting_duplicates=" +
                          std::to_string(attempts.multiple_completions) + "\n";
          break;
        }
        case QueryKind::kAttempt: {
          ExecutionAttempt attempt;
          if (!scheduler.attempts().get(ExecutionAttemptId(message.id), attempt)) {
            response.ok = false;
            response.code = "attempt.not_found";
          } else {
            response.ok = true;
            response.code = "ok";
            response.body = std::string("attempt ") + format_id(attempt.id.value()) + " state " +
                            std::string(to_string(attempt.state)) + " resolution " +
                            std::string(to_string(attempt.resolution)) + " generation " +
                            std::to_string(attempt.generation.value()) + " digest " +
                            std::to_string(attempt.result.result_digest) + " committed " +
                            (attempt.completion_committed ? "yes" : "no");
          }
          break;
        }
        case QueryKind::kOperation: {
          const std::vector<ExecutionAttempt> found = scheduler.attempts().list_by_operation(
              TransportOperationId(message.id), 64);
          response.ok = true;
          response.code = "ok";
          for (const ExecutionAttempt& attempt : found) {
            response.body += std::string("attempt ") + format_id(attempt.id.value()) + " gen " +
                             std::to_string(attempt.generation.value()) + " " +
                             std::string(to_string(attempt.state)) + "\n";
          }
          break;
        }
      }
      session.emit(MessageType::kQueryResponse, encode(response));
      return;
    }
    case MessageType::kAdminRequest: {
      AdminRequestMessage message;
      if (!decode(frame.payload, message)) {
        session.emit_error("protocol.malformed_admin", "admin request rejected");
        return;
      }
      AdminResponseMessage response;
      bool stop_after_reply = false;
      switch (message.action) {
        case AdminAction::kFenceDomain: {
          const Status fenced = scheduler.fence_domain(ExecutionDomainId(message.id), message.reason);
          response.ok = fenced.ok;
          response.code = fenced.code;
          response.detail = fenced.message;
          break;
        }
        case AdminAction::kFenceBoot: {
          const Status fenced = scheduler.fence_worker_boot(WorkerBootId(message.id), message.reason);
          response.ok = fenced.ok;
          response.code = fenced.code;
          response.detail = fenced.message;
          break;
        }
        case AdminAction::kReconcile: {
          const ReconciliationReport report = scheduler.reconcile();
          response.ok = true;
          response.code = "ok";
          response.body = report.render_text();
          break;
        }
        case AdminAction::kSaveState: {
          const Status saved = scheduler.save_state();
          response.ok = saved.ok;
          response.code = saved.code;
          response.detail = saved.message;
          break;
        }
        case AdminAction::kSetOffloadRequirement: {
          SchedulerPolicy policy = scheduler.policy();
          policy.offload_requirement = static_cast<OffloadRequirement>(message.value);
          const Status applied = scheduler.set_policy(std::move(policy));
          response.ok = applied.ok;
          response.code = applied.code;
          response.detail = applied.message;
          break;
        }
        case AdminAction::kShutdownCoordinator: {
          response.ok = true;
          response.code = "ok";
          response.detail = "coordinator shutting down";
          stop_after_reply = true;
          break;
        }
      }
      session.emit(MessageType::kAdminResponse, encode(response));
      if (stop_after_reply) request_stop();
      return;
    }
    case MessageType::kDispatchAccepted: {
      DispatchAcceptedMessage message;
      if (!decode(frame.payload, message)) {
        session.emit_error("protocol.malformed_dispatch_ack", "dispatch acknowledgement rejected");
        return;
      }
      // A worker accepting a dispatch is transport evidence, not completion
      // authority: the authoritative record is still the completion or failure.
      log_write(LogLevel::kDebug, "coordinator",
                "attempt " + format_id(message.attempt.value()) + " accepted by worker");
      return;
    }
    case MessageType::kDispatchRejected: {
      DispatchRejectedMessage message;
      if (!decode(frame.payload, message)) {
        session.emit_error("protocol.malformed_dispatch_rejection",
                           "dispatch rejection rejected");
        return;
      }
      const Status reported =
          scheduler.report_failure(message.attempt, FailureKind::kPreDispatchRejection,
                                   message.code + " " + message.detail);
      if (!reported && reported.code != "attempt.not_found") {
        session.emit_error(reported.code, reported.message);
      }
      return;
    }
    case MessageType::kCancelResult: {
      CancelResultMessage message;
      if (!decode(frame.payload, message)) {
        session.emit_error("protocol.malformed_cancel_result", "cancel result rejected");
        return;
      }
      log_write(LogLevel::kDebug, "coordinator",
                std::string("cancellation result for ") + format_id(message.attempt.value()) + ": " +
                    (message.cancelled ? "honoured" : "refused"));
      return;
    }
    case MessageType::kGoodbye: {
      session.close();
      return;
    }
    default: {
      session.emit_error("protocol.unexpected_message_type",
                         std::string("coordinator does not accept message type ") +
                             std::to_string(static_cast<int>(frame.type)));
      return;
    }
  }
}

OffloadCoordinator::OffloadCoordinator(CoordinatorConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {
  impl_->channel = std::make_shared<Impl::RemoteChannel>(impl_.get());
}

OffloadCoordinator::~OffloadCoordinator() {
  if (impl_) {
    const Status stopped = stop();
    (void)stopped;
  }
}

bool OffloadCoordinator::running() const noexcept { return impl_->running.load(); }
std::uint16_t OffloadCoordinator::port() const noexcept { return impl_->listener.port(); }
Scheduler& OffloadCoordinator::scheduler() noexcept { return impl_->scheduler; }
const Scheduler& OffloadCoordinator::scheduler() const noexcept { return impl_->scheduler; }
std::shared_ptr<IDispatchChannel> OffloadCoordinator::channel() const { return impl_->channel; }

Status OffloadCoordinator::start() {
  if (impl_->running.load()) return Status::failure("coordinator.already_running");
  impl_->stopping.store(false);
  const Status started = impl_->scheduler.start();
  if (!started) return started;
  const Status listening = impl_->listener.listen_on(impl_->config.bind_host, impl_->config.port);
  if (!listening) {
    const Status stopped = impl_->scheduler.shutdown();
    (void)stopped;
    return listening;
  }
  impl_->scheduler.set_dispatch_channel(impl_->channel);
  impl_->accept_thread = std::thread([this] { impl_->accept_loop(); });
  impl_->running.store(true);
  log_write(LogLevel::kInfo, "coordinator",
            "listening on " + impl_->config.bind_host + ":" + std::to_string(port()) +
                " epoch " + std::to_string(impl_->scheduler.coordinator_epoch().value()));
  return Status::success();
}

Status OffloadCoordinator::stop() {
  if (!impl_->running.load()) return Status::success();
  impl_->stopping.store(true);
  // Wake a pending accept() by connecting to it rather than relying on closesocket()
  // to cancel a blocking accept on another thread. The accept loop sees the stopping
  // flag and closes the probe connection without creating a session.
  {
    auto wake = connect_tcp(impl_->config.bind_host, port());
    if (wake.ok()) wake.value.close();
  }
  impl_->listener.close();

  std::vector<std::shared_ptr<Impl::Session>> open;
  {
    std::lock_guard<std::mutex> lock(impl_->sessions_mutex);
    for (const auto& entry : impl_->sessions) open.push_back(entry.second);
  }
  for (const std::shared_ptr<Impl::Session>& session : open) {
    if (session->view.role == PeerRole::kWorker && session->view.registered) {
      ShutdownRequestMessage goodbye;
      goodbye.reason = "coordinator shutting down";
      const Status sent = session->send_frame(MessageType::kShutdownRequest, encode(goodbye));
      (void)sent;
    }
    session->close();
  }
  if (impl_->accept_thread.joinable()) impl_->accept_thread.join();

  // Threads are joined with no coordinator lock held: a session's epilogue takes the
  // session registry lock to erase itself, so joining under that lock would be a
  // wait-for cycle. The thread handles are moved out first.
  std::vector<std::thread> threads;
  {
    std::lock_guard<std::mutex> lock(impl_->sessions_mutex);
    threads.swap(impl_->session_threads);
  }
  for (std::thread& thread : threads) {
    if (thread.joinable()) thread.join();
  }
  {
    std::lock_guard<std::mutex> lock(impl_->sessions_mutex);
    impl_->sessions.clear();
  }
  const Status stopped = impl_->scheduler.shutdown();
  impl_->running.store(false);
  log_write(LogLevel::kInfo, "coordinator", "stopped");
  return stopped;
}

std::vector<SessionView> OffloadCoordinator::sessions() const {
  std::lock_guard<std::mutex> lock(impl_->sessions_mutex);
  std::vector<SessionView> out;
  out.reserve(impl_->sessions.size());
  for (const auto& entry : impl_->sessions) out.push_back(entry.second->view);
  std::sort(out.begin(), out.end(),
            [](const SessionView& a, const SessionView& b) { return a.session_id < b.session_id; });
  return out;
}

std::size_t OffloadCoordinator::session_count() const {
  std::lock_guard<std::mutex> lock(impl_->sessions_mutex);
  return impl_->sessions.size();
}

bool OffloadCoordinator::stop_requested() const noexcept {
  std::lock_guard<std::mutex> lock(impl_->stop_mutex);
  return impl_->stop_signalled;
}

void OffloadCoordinator::wait_for_stop() {
  std::unique_lock<std::mutex> lock(impl_->stop_mutex);
  impl_->stop_cv.wait(lock, [this] { return impl_->stop_signalled; });
}

std::size_t OffloadCoordinator::registered_worker_count() const {
  std::lock_guard<std::mutex> lock(impl_->sessions_mutex);
  std::size_t count = 0;
  for (const auto& entry : impl_->sessions) {
    if (entry.second->view.role == PeerRole::kWorker && entry.second->view.registered) ++count;
  }
  return count;
}

}  // namespace dist
}  // namespace tos