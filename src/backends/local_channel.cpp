// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "tos/backends/local_channel.hpp"

#include <chrono>
#include <mutex>
#include <utility>

namespace tos {

std::string_view to_string(ChannelFault value) noexcept {
  switch (value) {
    case ChannelFault::kNone: return "none";
    case ChannelFault::kSendFailure: return "send_failure";
    case ChannelFault::kDropCompletion: return "drop_completion";
    case ChannelFault::kAmbiguousCompletion: return "ambiguous_completion";
    case ChannelFault::kConflictingCompletion: return "conflicting_completion";
    case ChannelFault::kDuplicateCompletion: return "duplicate_completion";
    case ChannelFault::kDelayedCompletion: return "delayed_completion";
  }
  return "none";
}

struct LocalDispatchChannel::Impl {
  std::shared_ptr<IExecutionBackend> backend;
  CompletionSink sink;
  ThreadPool pool;
  mutable std::mutex mutex;
  std::map<ChannelFault, std::uint32_t> faults;
  std::vector<CompletionSubmission> delayed;
  std::atomic<bool> closed{false};

  Impl(std::shared_ptr<IExecutionBackend> backend_in, CompletionSink sink_in,
       const LocalChannelOptions& options)
      : backend(std::move(backend_in)),
        sink(std::move(sink_in)),
        pool(options.threads == 0 ? 1 : options.threads,
             options.max_queue == 0 ? 1 : options.max_queue) {}

  [[nodiscard]] ChannelFault take_fault() {
    std::lock_guard<std::mutex> lock(mutex);
    for (auto& entry : faults) {
      if (entry.second > 0) {
        --entry.second;
        return entry.first;
      }
    }
    return ChannelFault::kNone;
  }

  void deliver(const CompletionSubmission& submission) {
    if (closed.load()) return;
    CompletionSink local_sink;
    {
      std::lock_guard<std::mutex> lock(mutex);
      local_sink = sink;
    }
    if (local_sink) local_sink(submission);
  }
};

LocalDispatchChannel::LocalDispatchChannel(std::shared_ptr<IExecutionBackend> backend,
                                           CompletionSink sink, LocalChannelOptions options)
    : impl_(std::make_unique<Impl>(std::move(backend), std::move(sink), options)) {}

LocalDispatchChannel::~LocalDispatchChannel() { shutdown(); }

void LocalDispatchChannel::inject_fault(ChannelFault fault, std::uint32_t count) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->faults[fault] = count;
}

std::size_t LocalDispatchChannel::release_delayed() {
  std::vector<CompletionSubmission> pending;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    pending.swap(impl_->delayed);
  }
  for (const CompletionSubmission& submission : pending) impl_->deliver(submission);
  return pending.size();
}

std::size_t LocalDispatchChannel::delayed_count() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->delayed.size();
}

Status LocalDispatchChannel::send(const DispatchEnvelope& envelope) {
  if (impl_->closed.load()) return Status::failure("channel.closed");
  const ChannelFault fault = impl_->take_fault();
  if (fault == ChannelFault::kSendFailure) {
    return Status::failure("channel.injected_send_failure");
  }
  dispatched_.fetch_add(1, std::memory_order_relaxed);

  ExecutionInvocation invocation;
  invocation.attempt = envelope.attempt;
  invocation.attempt_generation = envelope.attempt_generation;
  invocation.dispatch = envelope.dispatch;
  invocation.coordinator_epoch = envelope.coordinator_epoch;
  invocation.worker = envelope.worker;
  invocation.worker_boot = envelope.worker_boot;
  invocation.domain = envelope.domain;
  invocation.domain_generation = envelope.domain_generation;
  invocation.capability_generation = envelope.capability_generation;
  invocation.operation_class = envelope.operation_class;
  invocation.side_effect = envelope.side_effect;
  invocation.payload = envelope.payload;
  invocation.input = envelope.bytes;
  invocation.retry_index = envelope.retry_index;
  invocation.fallback = envelope.fallback;
  invocation.provenance = envelope.provenance;

  std::shared_ptr<IExecutionBackend> backend = impl_->backend;
  const Status submitted = impl_->pool.submit([this, backend, invocation, fault, envelope] {
    const auto started = std::chrono::steady_clock::now();
    ExecutionOutcome outcome = backend->execute(invocation);
    const auto finished = std::chrono::steady_clock::now();
    if (outcome.duration_ns == 0) {
      outcome.duration_ns = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count());
    }
    CompletionSubmission submission;
    submission.attempt = invocation.attempt;
    submission.generation = invocation.attempt_generation;
    submission.dispatch = invocation.dispatch;
    submission.completion = CompletionId(invocation.attempt.value());
    submission.worker = invocation.worker;
    submission.worker_boot = invocation.worker_boot;
    submission.coordinator_epoch = invocation.coordinator_epoch;
    submission.domain_generation = invocation.domain_generation;
    submission.capability_generation = invocation.capability_generation;
    submission.operation = envelope.operation;
    submission.result.backend_detail = "local channel";
    submission.provenance = invocation.provenance;
    submission.result.success = outcome.success;
    submission.result.result_digest = outcome.result_digest;
    submission.result.bytes_processed = outcome.bytes_processed;
    submission.result.duration_ns = outcome.duration_ns;
    submission.result.failure = outcome.success ? FailureKind::kNone : outcome.failure;
    submission.result.backend_detail = outcome.detail;

    if (fault == ChannelFault::kAmbiguousCompletion) return;
    if (fault == ChannelFault::kDropCompletion) {
      // The operation was applied and the acknowledgement is lost: the attempt
      // stays in flight until the runtime classifies the outcome.
      return;
    }
    if (fault == ChannelFault::kDelayedCompletion) {
      std::lock_guard<std::mutex> lock(impl_->mutex);
      impl_->delayed.push_back(submission);
      return;
    }
    if (fault == ChannelFault::kConflictingCompletion) {
      CompletionSubmission conflicting = submission;
      conflicting.result.success = !outcome.success;
      conflicting.result.result_digest = outcome.result_digest ^ 0xFFFFFFFFFFFFFFFFULL;
      impl_->deliver(submission);
      impl_->deliver(conflicting);
      return;
    }
    impl_->deliver(submission);
    if (fault == ChannelFault::kDuplicateCompletion) impl_->deliver(submission);
  });
  return submitted;
}

bool LocalDispatchChannel::request_cancel(const CancelEnvelope& envelope) {
  std::shared_ptr<IExecutionBackend> backend = impl_->backend;
  if (!backend->supports_cancel()) return false;
  const Status cancelled = backend->cancel(envelope.attempt);
  return static_cast<bool>(cancelled);
}

void LocalDispatchChannel::shutdown() {
  if (impl_->closed.exchange(true)) return;
  impl_->pool.shutdown();
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->delayed.clear();
  impl_->sink = nullptr;
}

}  // namespace tos
