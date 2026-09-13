// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "tos/util/thread_pool.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace tos {

struct ThreadPool::Impl {
  std::mutex mutex;
  std::condition_variable available;
  std::condition_variable drained;
  std::deque<std::function<void()>> tasks;
  std::vector<std::thread> threads;
  std::size_t max_queue{0};
  std::size_t active{0};
  bool stopping{false};

  void worker_loop() {
    for (;;) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(mutex);
        available.wait(lock, [this] { return stopping || !tasks.empty(); });
        if (tasks.empty()) {
          if (stopping) return;
          continue;
        }
        task = std::move(tasks.front());
        tasks.pop_front();
        ++active;
      }
      task();
      {
        std::lock_guard<std::mutex> lock(mutex);
        --active;
        if (tasks.empty() && active == 0) drained.notify_all();
      }
    }
  }
};

ThreadPool::ThreadPool(std::size_t thread_count, std::size_t max_queue)
    : impl_(std::make_unique<Impl>()) {
  const std::size_t count = thread_count == 0 ? 1 : thread_count;
  impl_->max_queue = max_queue == 0 ? 1 : max_queue;
  impl_->threads.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    impl_->threads.emplace_back([this] { impl_->worker_loop(); });
  }
}

ThreadPool::~ThreadPool() { shutdown(); }

Status ThreadPool::submit(std::function<void()> task) {
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->stopping) return Status::failure("pool.stopped");
    if (impl_->tasks.size() >= impl_->max_queue) return Status::failure("pool.full");
    impl_->tasks.push_back(std::move(task));
  }
  impl_->available.notify_one();
  return Status::success();
}

void ThreadPool::shutdown() {
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->stopping && impl_->threads.empty()) return;
    impl_->stopping = true;
  }
  impl_->available.notify_all();
  for (std::thread& thread : impl_->threads) {
    if (thread.joinable()) {
      if (thread.get_id() == std::this_thread::get_id()) {
        // A pool thread must never join itself. Detach instead of deadlocking;
        // shutdown is documented as not callable from a pool task.
        thread.detach();
      } else {
        thread.join();
      }
    }
  }
  impl_->threads.clear();
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->tasks.clear();
}

bool ThreadPool::stopped() const noexcept {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->stopping;
}

std::size_t ThreadPool::thread_count() const noexcept { return impl_->threads.size(); }

std::size_t ThreadPool::queued() const noexcept {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->tasks.size();
}

std::size_t ThreadPool::active() const noexcept {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->active;
}

}  // namespace tos
