// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "tos/dist/worker.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <map>
#include <utility>
#include <vector>

#include "tos/util/log.hpp"
#include "tos/util/random.hpp"
#include "tos/version.hpp"

namespace tos {
namespace dist {

std::string_view to_string(WorkerFault value) noexcept {
  switch (value) {
    case WorkerFault::kNone: return "none";
    case WorkerFault::kRejectDispatch: return "reject_dispatch";
    case WorkerFault::kDropCompletion: return "drop_completion";
    case WorkerFault::kAmbiguousCompletion: return "ambiguous_completion";
    case WorkerFault::kFailureReport: return "failure_report";
    case WorkerFault::kDieAfterApply: return "die_after_apply";
    case WorkerFault::kStaleDomainGeneration: return "stale_domain_generation";
  }
  return "none";
}

bool parse_worker_fault(std::string_view text, WorkerFault& out) noexcept {
  static constexpr std::pair<std::string_view, WorkerFault> kTable[] = {
      {"none", WorkerFault::kNone},
      {"reject-dispatch", WorkerFault::kRejectDispatch},
      {"drop-completion", WorkerFault::kDropCompletion},
      {"ambiguous-completion", WorkerFault::kAmbiguousCompletion},
      {"failure-report", WorkerFault::kFailureReport},
      {"die-after-apply", WorkerFault::kDieAfterApply},
      {"stale-domain-generation", WorkerFault::kStaleDomainGeneration},
  };
  for (const auto& entry : kTable) {
    if (entry.first == text) {
      out = entry.second;
      return true;
    }
  }
  return false;
}

struct OffloadWorker::Impl {
  WorkerConfig config;
  SocketRuntime sockets;
  TcpSocket socket;
  WorkerId worker;
  WorkerBootId boot;
  CoordinatorEpoch epoch;
  std::atomic<bool> stopping{false};
  std::atomic<bool> connected{false};
  std::atomic<std::uint64_t> executions{0};
  std::atomic<std::uint64_t> accepted{0};
  std::vector<ExecutionDomainRecord> domains;
  /// Published identity to backend-local identity. The coordinator-visible domain
  /// identity is assigned by this worker; the backend keeps its own local ids.
  std::map<std::uint64_t, ExecutionDomainId> backend_identity;

  [[nodiscard]] Status send_frame(MessageType type, const std::vector<std::uint8_t>& payload) {
    auto encoded = encode_frame(type, payload, config.max_frame_bytes);
    if (!encoded.ok()) return encoded.status;
    return socket.send_all(encoded.value.data(), encoded.value.size());
  }

  /// Best-effort replies: a failed reply cannot change the decision already made.
  void emit(MessageType type, const std::vector<std::uint8_t>& payload) {
    const Status ignored = send_frame(type, payload);
    (void)ignored;
  }

  void emit_error(std::string code, std::string detail) {
    ErrorMessage message;
    message.code = std::move(code);
    message.detail = std::move(detail);
    emit(MessageType::kError, encode(message));
  }

  void emit_rejection(const DispatchEnvelope& envelope, std::string code, std::string detail) {
    DispatchRejectedMessage rejected;
    rejected.attempt = envelope.attempt;
    rejected.dispatch = envelope.dispatch;
    rejected.code = std::move(code);
    rejected.detail = std::move(detail);
    emit(MessageType::kDispatchRejected, encode(rejected));
  }

  void emit_failure(const DispatchEnvelope& envelope, FailureKind kind, std::string detail) {
    FailureReportMessage failure;
    failure.attempt = envelope.attempt;
    failure.generation = envelope.attempt_generation;
    failure.dispatch = envelope.dispatch;
    failure.boot = boot;
    failure.epoch = epoch;
    failure.kind = kind;
    failure.detail = std::move(detail);
    emit(MessageType::kFailureReport, encode(failure));
  }

  [[nodiscard]] ExecutionDomainId local_domain(ExecutionDomainId published) const {
    const auto found = backend_identity.find(published.value());
    return found == backend_identity.end() ? published : found->second;
  }

  [[nodiscard]] ExecutionDomainRecord* find_domain(ExecutionDomainId id) {
    for (ExecutionDomainRecord& record : domains) {
      if (record.id == id) return &record;
    }
    return nullptr;
  }

  void handle_dispatch(const DispatchRequestMessage& message);
  [[nodiscard]] bool fault_active() const {
    return config.fault != WorkerFault::kNone &&
           executions.load(std::memory_order_relaxed) >= config.fault_after_executions;
  }
};

OffloadWorker::OffloadWorker(WorkerConfig config) : impl_(std::make_unique<Impl>()) {
  impl_->config = std::move(config);
  impl_->worker = WorkerId(process_unique_u64());
  impl_->boot = WorkerBootId(process_unique_u64());
}

OffloadWorker::~OffloadWorker() {
  if (impl_ && impl_->connected.load()) {
    GoodbyeMessage goodbye;
    goodbye.reason = "worker exiting";
    impl_->emit(MessageType::kGoodbye, encode(goodbye));
    impl_->socket.close();
  }
}

WorkerId OffloadWorker::worker_id() const noexcept { return impl_->worker; }
WorkerBootId OffloadWorker::boot_id() const noexcept { return impl_->boot; }
CoordinatorEpoch OffloadWorker::epoch() const noexcept { return impl_->epoch; }
std::uint64_t OffloadWorker::executions() const noexcept {
  return impl_->executions.load(std::memory_order_relaxed);
}
std::uint64_t OffloadWorker::executions_accepted() const noexcept {
  return impl_->accepted.load(std::memory_order_relaxed);
}
const std::vector<ExecutionDomainRecord>& OffloadWorker::domains() const noexcept {
  return impl_->domains;
}

void OffloadWorker::request_stop() { impl_->stopping.store(true); }

Status OffloadWorker::connect_and_register() {
  if (impl_->config.backend == nullptr) return Status::failure("worker.no_backend");
  auto connected =
      connect_tcp_retry(impl_->config.coordinator_host, impl_->config.coordinator_port, 20);
  if (!connected.ok()) return connected.status;
  impl_->socket = std::move(connected.value);
  impl_->socket.set_nodelay(true);

  HelloMessage hello;
  hello.role = PeerRole::kWorker;
  hello.name = impl_->config.name;
  hello.worker = impl_->worker;
  hello.boot = impl_->boot;
  hello.host = impl_->config.host;
  hello.version = std::string(kVersionString);
  const Status sent = impl_->send_frame(MessageType::kHello, encode(hello));
  if (!sent) return sent;
  if (!impl_->socket.valid()) return Status::failure("worker.socket_closed");

  std::vector<std::uint8_t> header(kFrameHeaderBytes);
  const Status read = impl_->socket.recv_exact(header.data(), kFrameHeaderBytes);
  if (!read) return read;
  FrameHeader parsed;
  const DecodeError header_error =
      decode_header(header.data(), header.size(), impl_->config.max_frame_bytes, parsed);
  if (header_error != DecodeError::kOk) {
    return Status::failure(std::string("protocol.") + std::string(to_string(header_error)));
  }
  std::vector<std::uint8_t> buffer(kFrameHeaderBytes + parsed.length, 0);
  std::copy(header.begin(), header.end(), buffer.begin());
  if (parsed.length > 0) {
    const Status body = impl_->socket.recv_exact(buffer.data() + kFrameHeaderBytes, parsed.length);
    if (!body) return body;
  }
  auto frame = decode_frame(buffer.data(), buffer.size(), impl_->config.max_frame_bytes);
  if (!frame.ok()) return frame.status;
  if (frame.value.type != MessageType::kHelloAck) {
    return Status::failure("worker.unexpected_handshake",
                           std::to_string(static_cast<int>(frame.value.type)));
  }
  HelloAckMessage ack;
  if (!decode(frame.value.payload, ack)) return Status::failure("worker.malformed_hello_ack");
  if (!ack.accepted) {
    return Status::failure(ack.code.empty() ? "worker.rejected" : ack.code, ack.detail);
  }
  impl_->epoch = ack.epoch;
  impl_->connected.store(true);

  if (impl_->config.publish_domains) {
    const std::vector<ExecutionDomainRecord> discovered = impl_->config.backend->discover_domains();
    const std::uint64_t base = impl_->config.domain_id_base != 0
                                   ? impl_->config.domain_id_base
                                   : ((process_unique_u64() & 0x0000FFFFFFFFULL) << 16);
    std::uint64_t offset = 0;
    for (const ExecutionDomainRecord& record : discovered) {
      ExecutionDomainRecord published = record;
      published.id = ExecutionDomainId(base + offset);
      impl_->backend_identity[published.id.value()] = record.id;
      ++offset;
      published.worker = impl_->worker;
      published.worker_boot = impl_->boot;
      published.parent_host =
          impl_->config.host.empty() ? record.parent_host : impl_->config.host;
      if (impl_->config.provenance != Provenance::kReal) {
        published.provenance = impl_->config.provenance;
        published.capability.provenance = impl_->config.provenance;
      }
      published.capability.domain = published.id;
      published.capability.generation = CapabilityGeneration::first();
      published.capability.evidence = EvidenceGeneration::first();
      published.capability.publisher = impl_->boot;
      published.generation = ExecutionDomainGeneration::first();
      published.locality.generation = LocalityGeneration::first();
      published.topology.generation = TopologyGeneration::first();
      published.topology.host_node = published.parent_host;
      published.compatibility.generation = CompatibilityGeneration::first();
      published.backend_generation = BackendGeneration::first();
      impl_->domains.push_back(published);

      DomainPublishMessage domain_message;
      domain_message.record = published;
      const Status published_status =
          impl_->send_frame(MessageType::kDomainPublish, encode(domain_message));
      if (!published_status) return published_status;

      CapabilityPublishMessage capability_message;
      capability_message.capability = published.capability;
      const Status capability_status =
          impl_->send_frame(MessageType::kCapabilityPublish, encode(capability_message));
      if (!capability_status) return capability_status;

      LoadUpdateMessage load_message;
      load_message.domain = published.id;
      load_message.load = published.load;
      const Status load_status =
          impl_->send_frame(MessageType::kLoadUpdate, encode(load_message));
      if (!load_status) return load_status;
    }
    log_write(LogLevel::kInfo, "worker",
              impl_->config.name + " published " + std::to_string(impl_->domains.size()) +
                  " domain(s) as boot " + format_id(impl_->boot.value()));
  }
  return Status::success();
}

Status OffloadWorker::publish_load() {
  for (const ExecutionDomainRecord& record : impl_->domains) {
    LoadUpdateMessage load;
    load.domain = record.id;
    load.load = record.load;
    load.load.load_generation = LoadGeneration(record.load.load_generation.value() + 1);
    load.load.queue_generation = QueueGeneration(record.load.queue_generation.value() + 1);
    load.load.health_generation = HealthGeneration(record.load.health_generation.value() + 1);
    load.load.evidence = EvidenceGeneration(record.load.evidence.value() + 1);
    const Status sent = impl_->send_frame(MessageType::kLoadUpdate, encode(load));
    if (!sent) return sent;
  }
  return Status::success();
}

void OffloadWorker::Impl::handle_dispatch(const DispatchRequestMessage& message) {
  const DispatchEnvelope& envelope = message.envelope;

  if (envelope.coordinator_epoch != epoch) {
    emit_rejection(envelope, "worker.stale_epoch",
                   "dispatch carries a coordinator epoch this worker does not serve");
    return;
  }
  if (envelope.worker_boot != boot) {
    emit_rejection(envelope, "worker.stale_boot",
                   "dispatch targets a different worker incarnation");
    return;
  }
  ExecutionDomainRecord* domain = find_domain(envelope.domain);
  if (domain == nullptr) {
    emit_rejection(envelope, "worker.unknown_domain",
                   "this worker does not own the requested domain");
    return;
  }
  if (!envelope.domain_generation.published()) {
    emit_rejection(envelope, "worker.unpublished_domain_generation",
                   "dispatch does not name the domain generation it was planned against");
    return;
  }
  // Domain generation authority belongs to the coordinator: it revalidates the
  // binding against the registry before dispatch. The worker verifies the identity
  // it can actually know (epoch, incarnation, domain ownership) and never treats its
  // own local counter as the authoritative generation.
  if (config.fault == WorkerFault::kRejectDispatch && fault_active()) {
    emit_rejection(envelope, "worker.injected_rejection",
                   "worker is configured to refuse dispatches");
    return;
  }

  DispatchAcceptedMessage accepted_message;
  accepted_message.attempt = envelope.attempt;
  accepted_message.generation = envelope.attempt_generation;
  accepted_message.dispatch = envelope.dispatch;
  accepted_message.boot = boot;
  const Status accepted_status =
      send_frame(MessageType::kDispatchAccepted, encode(accepted_message));
  if (!accepted_status) {
    stopping.store(true);
    return;
  }
  accepted.fetch_add(1, std::memory_order_relaxed);

  ExecutionInvocation invocation;
  invocation.attempt = envelope.attempt;
  invocation.attempt_generation = envelope.attempt_generation;
  invocation.dispatch = envelope.dispatch;
  invocation.coordinator_epoch = envelope.coordinator_epoch;
  invocation.worker = envelope.worker;
  invocation.worker_boot = envelope.worker_boot;
  invocation.domain = local_domain(envelope.domain);
  invocation.domain_generation = envelope.domain_generation;
  invocation.capability_generation = envelope.capability_generation;
  invocation.operation_class = envelope.operation_class;
  invocation.side_effect = envelope.side_effect;
  invocation.payload = envelope.payload;
  invocation.input = envelope.bytes;
  invocation.retry_index = envelope.retry_index;
  invocation.fallback = envelope.fallback;
  invocation.provenance = envelope.provenance;

  const ExecutionOutcome outcome = config.backend->execute(invocation);
  executions.fetch_add(1, std::memory_order_relaxed);
  const bool faulted = fault_active();

  if (faulted && config.fault == WorkerFault::kDieAfterApply) {
    // The effect has been applied and the acknowledgement is never sent: a real,
    // abrupt process death in the middle of the dispatch/completion window.
    log_write(LogLevel::kError, "worker", "terminating after applying the operation (injected)");
    std::_Exit(21);
  }
  if (faulted && config.fault == WorkerFault::kDropCompletion) {
    log_write(LogLevel::kWarn, "worker", "dropping the completion (injected)");
    return;
  }
  if (faulted && config.fault == WorkerFault::kAmbiguousCompletion) {
    emit_failure(envelope, FailureKind::kAmbiguousOutcome,
                 "worker applied the operation but cannot confirm the outcome");
    return;
  }
  if (faulted && config.fault == WorkerFault::kFailureReport) {
    emit_failure(envelope, FailureKind::kExecutionFailure,
                 "worker reported a definitive execution failure (injected)");
    return;
  }

  if (outcome.ambiguous) {
    emit_failure(envelope, FailureKind::kAmbiguousOutcome,
                 outcome.detail.empty() ? "worker cannot confirm the outcome" : outcome.detail);
    return;
  }
  if (!outcome.executed) {
    // The backend never applied the operation: this is a rejection, not a
    // completion, so completion authority must not record a result for it.
    emit_failure(envelope,
                 outcome.failure == FailureKind::kNone ? FailureKind::kBackendRejection
                                                       : outcome.failure,
                 outcome.detail.empty() ? "backend rejected the operation" : outcome.detail);
    return;
  }

  CompletionMessage completion;
  completion.submission.attempt = envelope.attempt;
  completion.submission.generation = envelope.attempt_generation;
  completion.submission.dispatch = envelope.dispatch;
  completion.submission.completion = CompletionId(envelope.attempt.value());
  completion.submission.worker = worker;
  completion.submission.worker_boot = boot;
  completion.submission.coordinator_epoch = epoch;
  completion.submission.domain_generation = envelope.domain_generation;
  completion.submission.capability_generation = envelope.capability_generation;
  completion.submission.operation = envelope.operation;
  completion.submission.provenance = envelope.provenance;
  completion.submission.result.success = outcome.success;
  completion.submission.result.result_digest = outcome.result_digest;
  completion.submission.result.bytes_processed = outcome.bytes_processed;
  completion.submission.result.duration_ns = outcome.duration_ns;
  completion.submission.result.failure =
      outcome.success ? FailureKind::kNone : outcome.failure;
  completion.submission.result.backend_detail = outcome.detail.empty() ? "worker" : outcome.detail;
  const Status sent = send_frame(MessageType::kCompletion, encode(completion));
  if (!sent) stopping.store(true);
}

Status OffloadWorker::run() {
  if (!impl_->connected.load()) return Status::failure("worker.not_connected");
  std::vector<std::uint8_t> header(kFrameHeaderBytes);
  std::vector<std::uint8_t> buffer;
  while (!impl_->stopping.load()) {
    const Status read_header = impl_->socket.recv_exact(header.data(), kFrameHeaderBytes);
    if (!read_header) break;
    FrameHeader parsed;
    const DecodeError header_error =
        decode_header(header.data(), header.size(), impl_->config.max_frame_bytes, parsed);
    if (header_error != DecodeError::kOk) {
      impl_->emit_error(std::string("protocol.") + std::string(to_string(header_error)),
                        "frame header rejected");
      break;
    }
    buffer.assign(kFrameHeaderBytes + parsed.length, 0);
    std::copy(header.begin(), header.end(), buffer.begin());
    if (parsed.length > 0) {
      const Status body =
          impl_->socket.recv_exact(buffer.data() + kFrameHeaderBytes, parsed.length);
      if (!body) break;
    }
    auto frame = decode_frame(buffer.data(), buffer.size(), impl_->config.max_frame_bytes);
    if (!frame.ok()) {
      impl_->emit_error(frame.status.code, "frame rejected");
      break;
    }
    switch (frame.value.type) {
      case MessageType::kDispatchRequest: {
        DispatchRequestMessage dispatch;
        if (!decode(frame.value.payload, dispatch)) {
          impl_->emit_error("protocol.malformed_dispatch", "dispatch rejected");
          break;
        }
        impl_->handle_dispatch(dispatch);
        break;
      }
      case MessageType::kCancelRequest: {
        CancelRequestMessage cancel;
        if (!decode(frame.value.payload, cancel)) {
          impl_->emit_error("protocol.malformed_cancel", "cancel rejected");
          break;
        }
        CancelResultMessage result;
        result.attempt = cancel.envelope.attempt;
        const Status cancelled = impl_->config.backend->cancel(cancel.envelope.attempt);
        result.cancelled = static_cast<bool>(cancelled);
        result.may_have_executed = !cancelled.ok || cancel.envelope.may_have_executed;
        result.detail = cancelled.ok ? "cancellation honoured" : cancelled.code;
        impl_->emit(MessageType::kCancelResult, encode(result));
        break;
      }
      case MessageType::kShutdownRequest: {
        ShutdownRequestMessage shutdown;
        if (decode(frame.value.payload, shutdown)) {
          log_write(LogLevel::kInfo, "worker", "shutdown requested: " + shutdown.reason);
        }
        GoodbyeMessage goodbye;
        goodbye.reason = "worker acknowledged shutdown";
        impl_->emit(MessageType::kGoodbye, encode(goodbye));
        impl_->stopping.store(true);
        break;
      }
      case MessageType::kFence: {
        FenceMessage fence;
        if (decode(frame.value.payload, fence)) {
          log_write(LogLevel::kWarn, "worker",
                    "fence notice for domain " + format_id(fence.domain.value()));
        }
        break;
      }
      case MessageType::kError: {
        ErrorMessage error;
        if (decode(frame.value.payload, error)) {
          log_write(LogLevel::kWarn, "worker", "coordinator error: " + error.code + " " + error.detail);
        }
        break;
      }
      case MessageType::kCompletionAck: {
        CompletionAckMessage ack;
        if (decode(frame.value.payload, ack)) {
          log_write(LogLevel::kDebug, "worker",
                    "completion ack " + std::string(to_string(ack.rejection)));
        }
        break;
      }
      default: {
        impl_->emit_error("protocol.unexpected_message_type",
                          std::to_string(static_cast<int>(frame.value.type)));
        break;
      }
    }
  }
  impl_->socket.close();
  impl_->connected.store(false);
  return Status::success();
}

}  // namespace dist
}  // namespace tos