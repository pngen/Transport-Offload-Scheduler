// Bounded thread pool. The runtime never spawns one thread per request.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#ifndef TOS_UTIL_THREAD_POOL_HPP
#define TOS_UTIL_THREAD_POOL_HPP

#include <cstddef>
#include <functional>
#include <memory>

#include "tos/util/status.hpp"

namespace tos {

class ThreadPool {
 public:
  /// Creates a pool with a fixed worker count and a bounded task queue.
  ThreadPool(std::size_t thread_count, std::size_t max_queue);
  ~ThreadPool();
  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  /// Enqueue a task. Rejected with "pool.full" when the queue bound is reached;
  /// the caller decides whether that is back pressure or a failure.
  [[nodiscard]] Status submit(std::function<void()> task);

  /// Stop accepting, run queued tasks to completion, join all threads.
  void shutdown();
  [[nodiscard]] bool stopped() const noexcept;
  [[nodiscard]] std::size_t thread_count() const noexcept;
  [[nodiscard]] std::size_t queued() const noexcept;
  [[nodiscard]] std::size_t active() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tos

#endif  // TOS_UTIL_THREAD_POOL_HPP
