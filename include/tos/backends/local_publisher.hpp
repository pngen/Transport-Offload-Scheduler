// Local domain publisher: keeps a scheduler in step with an in-process backend.
//
// The same evidence flow a remote worker performs over the protocol, applied
// locally. Every update goes through the normal generation-checked publication
// path, so a local backend cannot bypass authority either.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#ifndef TOS_BACKENDS_LOCAL_PUBLISHER_HPP
#define TOS_BACKENDS_LOCAL_PUBLISHER_HPP

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "tos/backends/backend.hpp"
#include "tos/core/scheduler.hpp"

namespace tos {

class LocalDomainPublisher {
 public:
  LocalDomainPublisher(Scheduler& scheduler, std::shared_ptr<IExecutionBackend> backend);

  /// Discover the backend's domains and publish everything that changed:
  /// registration, capability, locality, topology, compatibility, capacity and
  /// current load evidence.
  [[nodiscard]] Status sync();
  /// Refresh only the volatile operating evidence.
  [[nodiscard]] Status publish_load();
  /// Remove domains the backend no longer reports.
  [[nodiscard]] Status withdraw_missing();
  /// Fence every domain of a worker incarnation, as loss of a live worker does.
  [[nodiscard]] Status fence_boot(WorkerBootId boot, std::string_view reason);

  [[nodiscard]] std::vector<ExecutionDomainRecord> last_discovered() const;

 private:
  Scheduler* scheduler_;
  std::shared_ptr<IExecutionBackend> backend_;
  std::map<std::uint64_t, ExecutionDomainGeneration> published_generations_;
};

}  // namespace tos

#endif  // TOS_BACKENDS_LOCAL_PUBLISHER_HPP
