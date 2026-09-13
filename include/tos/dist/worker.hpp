// Offload worker: a real OS process that owns one or more execution domains.
//
// The worker is the only component that talks to an execution backend, and it can
// only publish evidence, accept dispatches and report outcomes through the versioned
// protocol. Fault modes exist so that ambiguous completion and real process death
// can be proven rather than simulated in-process.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#ifndef TOS_DIST_WORKER_HPP
#define TOS_DIST_WORKER_HPP

#include <cstdint>
#include <memory>
#include <string>

#include "tos/backends/backend.hpp"
#include "tos/dist/protocol.hpp"
#include "tos/util/net.hpp"

namespace tos {
namespace dist {

enum class WorkerFault : std::uint8_t {
  kNone = 0,
  kRejectDispatch = 1,      ///< refuse the dispatch before executing
  kDropCompletion = 2,      ///< execute, never report the outcome
  kAmbiguousCompletion = 3, ///< report that the outcome is unknown
  kFailureReport = 4,       ///< execute and report a definitive failure
  kDieAfterApply = 5,       ///< execute and terminate the process before acknowledging
  kStaleDomainGeneration = 6, ///< answer with a stale domain generation
};

[[nodiscard]] std::string_view to_string(WorkerFault value) noexcept;
[[nodiscard]] bool parse_worker_fault(std::string_view text, WorkerFault& out) noexcept;

struct WorkerConfig {
  std::string coordinator_host{"127.0.0.1"};
  std::uint16_t coordinator_port{0};
  std::string name{"worker"};
  std::string host;
  std::shared_ptr<IExecutionBackend> backend;  ///< required
  std::uint64_t domain_id_base{0};             ///< 0 selects a process-unique base
  Provenance provenance{Provenance::kReal};
  WorkerFault fault{WorkerFault::kNone};
  std::uint32_t fault_after_executions{0};  ///< 0 applies the fault immediately
  std::size_t max_frame_bytes{kMaxFrameBytes};
  bool publish_domains{true};
  bool exit_after_register{false};  ///< used by process-level readiness probes
};

class OffloadWorker {
 public:
  explicit OffloadWorker(WorkerConfig config);
  ~OffloadWorker();
  OffloadWorker(const OffloadWorker&) = delete;
  OffloadWorker& operator=(const OffloadWorker&) = delete;

  [[nodiscard]] Status connect_and_register();
  /// Serve frames until stopped, the coordinator closes, or a fatal fault fires.
  [[nodiscard]] Status run();
  void request_stop();
  [[nodiscard]] Status publish_load();

  [[nodiscard]] WorkerId worker_id() const noexcept;
  [[nodiscard]] WorkerBootId boot_id() const noexcept;
  [[nodiscard]] CoordinatorEpoch epoch() const noexcept;
  [[nodiscard]] std::uint64_t executions() const noexcept;
  [[nodiscard]] std::uint64_t executions_accepted() const noexcept;
  [[nodiscard]] const std::vector<ExecutionDomainRecord>& domains() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace dist
}  // namespace tos

#endif  // TOS_DIST_WORKER_HPP
