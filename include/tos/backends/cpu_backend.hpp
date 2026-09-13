// Real host CPU execution backend.
//
// Everything this backend publishes is measured or read from the operating system:
// hardware thread count, NUMA node count, physical memory and the CPU brand. It
// executes transport operations for real on host memory.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#ifndef TOS_BACKENDS_CPU_BACKEND_HPP
#define TOS_BACKENDS_CPU_BACKEND_HPP

#include <cstdint>
#include <memory>
#include <string>

#include "tos/backends/backend.hpp"
#include "tos/core/domain.hpp"

namespace tos {

class CpuBackend : public IExecutionBackend {
 public:
  struct Options {
    ExecutionDomainId domain_id{ExecutionDomainId(1)};
    std::string name{"cpu.host.0"};
    std::uint64_t max_payload_bytes{kMaxDispatchPayloadBytes};
    std::uint32_t queue_capacity{4096};
    std::uint32_t max_concurrency{0};  ///< 0 selects the hardware thread count
    WorkerId worker;
    WorkerBootId worker_boot;
    std::string parent_host;
  };

  explicit CpuBackend(Options options = {});
  ~CpuBackend() override;

  [[nodiscard]] std::string_view name() const noexcept override;
  [[nodiscard]] Provenance provenance() const noexcept override { return Provenance::kReal; }
  [[nodiscard]] BackendGeneration generation() const override;

  [[nodiscard]] std::vector<ExecutionDomainRecord> discover_domains() override;
  [[nodiscard]] Checked<CapabilityRecord> query_capability(ExecutionDomainId domain) override;
  [[nodiscard]] Checked<DomainLoadEvidence> query_load(ExecutionDomainId domain) override;
  [[nodiscard]] Status reserve(ExecutionDomainId domain, ReservationId id,
                               const ResourceAmounts& amounts) override;
  [[nodiscard]] Status release(ExecutionDomainId domain, ReservationId id) override;
  [[nodiscard]] ExecutionOutcome execute(const ExecutionInvocation& invocation) override;

  [[nodiscard]] std::uint64_t executed_count() const noexcept;

  /// Real host facts used by the published record.
  [[nodiscard]] static std::uint32_t hardware_threads();
  [[nodiscard]] static std::string cpu_brand();
  [[nodiscard]] static ExecutionDomainRecord describe(const Options& options);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tos

#endif  // TOS_BACKENDS_CPU_BACKEND_HPP
