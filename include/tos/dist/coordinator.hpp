// Offload coordinator: hosts the scheduler and serves real worker processes over TCP.
//
// Worker death and coordinator restart are proven against actual OS processes:
// a session ending is the control path that fences a worker incarnation.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#ifndef TOS_DIST_COORDINATOR_HPP
#define TOS_DIST_COORDINATOR_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "tos/core/scheduler.hpp"
#include "tos/dist/protocol.hpp"
#include "tos/util/net.hpp"

namespace tos {
namespace dist {

struct CoordinatorConfig {
  std::string bind_host{"127.0.0.1"};
  std::uint16_t port{0};  ///< 0 selects an ephemeral port
  SchedulerOptions scheduler;
  std::size_t max_sessions{32};
  std::size_t max_frame_bytes{kMaxFrameBytes};
};

struct SessionView {
  std::uint64_t session_id{0};
  std::string peer;
  PeerRole role{PeerRole::kClient};
  std::string name;
  WorkerId worker;
  WorkerBootId boot;
  bool registered{false};
  bool closed{false};
};

class OffloadCoordinator {
 public:
  explicit OffloadCoordinator(CoordinatorConfig config);
  ~OffloadCoordinator();
  OffloadCoordinator(const OffloadCoordinator&) = delete;
  OffloadCoordinator& operator=(const OffloadCoordinator&) = delete;

  /// Start the scheduler and the listener. Idempotent failure is reported, never thrown.
  [[nodiscard]] Status start();
  /// Stop admission, signal workers, close sessions, join threads, stop the scheduler.
  [[nodiscard]] Status stop();
  [[nodiscard]] bool running() const noexcept;
  [[nodiscard]] std::uint16_t port() const noexcept;
  [[nodiscard]] Scheduler& scheduler() noexcept;
  [[nodiscard]] const Scheduler& scheduler() const noexcept;
  [[nodiscard]] std::vector<SessionView> sessions() const;
  [[nodiscard]] std::size_t session_count() const;
  [[nodiscard]] std::shared_ptr<IDispatchChannel> channel() const;
  /// Number of worker connections currently registered as an incarnation.
  [[nodiscard]] std::size_t registered_worker_count() const;
  /// True once a shutdown has been requested through the control path.
  [[nodiscard]] bool stop_requested() const noexcept;
  /// Block until a shutdown is requested. Event driven: no polling, no timeout.
  void wait_for_stop();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace dist
}  // namespace tos

#endif  // TOS_DIST_COORDINATOR_HPP
