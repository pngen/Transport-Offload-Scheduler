// In-process dispatch channel.
//
// It moves an envelope to a backend through a bounded thread pool and delivers the
// completion through the same authority path a remote worker uses. Deterministic
// fault injection lives here so that transport-boundary behaviour can be tested
// without relying on thread timing: every fault is explicit and reproducible.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#ifndef TOS_BACKENDS_LOCAL_CHANNEL_HPP
#define TOS_BACKENDS_LOCAL_CHANNEL_HPP

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string_view>
#include <vector>

#include "tos/backends/backend.hpp"
#include "tos/core/dispatch.hpp"
#include "tos/util/thread_pool.hpp"

namespace tos {

enum class ChannelFault : std::uint8_t {
  kNone = 0,
  kSendFailure = 1,           ///< send() reports a transport failure; nothing executes
  kDropCompletion = 2,        ///< the operation executes, no completion is delivered
  kAmbiguousCompletion = 3,   ///< nothing is reported: the outcome stays unknown
  kConflictingCompletion = 4, ///< two completions with different results are delivered
  kDuplicateCompletion = 5,   ///< the same completion is delivered twice
  kDelayedCompletion = 6,     ///< the completion is withheld until explicitly released
};

[[nodiscard]] std::string_view to_string(ChannelFault value) noexcept;

struct LocalChannelOptions {
  std::size_t threads{4};
  std::size_t max_queue{1024};
};

class LocalDispatchChannel : public IDispatchChannel {
 public:
  using CompletionSink = std::function<void(const CompletionSubmission&)>;

  LocalDispatchChannel(std::shared_ptr<IExecutionBackend> backend, CompletionSink sink,
                       LocalChannelOptions options = {});
  ~LocalDispatchChannel() override;
  LocalDispatchChannel(const LocalDispatchChannel&) = delete;
  LocalDispatchChannel& operator=(const LocalDispatchChannel&) = delete;

  [[nodiscard]] std::string_view name() const noexcept override { return "local"; }
  [[nodiscard]] Status send(const DispatchEnvelope& envelope) override;
  bool request_cancel(const CancelEnvelope& envelope) override;
  void shutdown() override;

  /// Deterministic fault injection: the fault applies to the next count sends.
  void inject_fault(ChannelFault fault, std::uint32_t count = 1);
  /// Release completions withheld by kDelayedCompletion. Returns how many were sent.
  std::size_t release_delayed();
  [[nodiscard]] std::size_t delayed_count() const;
  [[nodiscard]] std::uint64_t dispatched_count() const noexcept {
    return dispatched_.load(std::memory_order_relaxed);
  }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::atomic<std::uint64_t> dispatched_{0};
};

}  // namespace tos

#endif  // TOS_BACKENDS_LOCAL_CHANNEL_HPP
