// tos_worker: a real worker process owning one or more execution domains.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include <iostream>
#include <memory>
#include <string>

#include "cli_support.hpp"
#include "tos/backends/cpu_backend.hpp"
#include "tos/backends/synthetic.hpp"
#include "tos/dist/worker.hpp"
#include "tos/tos.hpp"

using namespace tos;

namespace {

std::shared_ptr<IExecutionBackend> make_backend(const tos_cli::Arguments& arguments,
                                                Provenance provenance, std::string_view type_token,
                                                ExecutionDomainType type, std::uint32_t domain_count) {
  const std::string name = arguments.get("name", "worker");
  const std::string node = arguments.get("node", "local.host");
  if (type == ExecutionDomainType::kCpu && provenance == Provenance::kReal) {
    CpuBackend::Options options;
    options.domain_id = ExecutionDomainId(1);
    options.name = name + ".cpu";
    options.parent_host = node;
    return std::make_shared<CpuBackend>(options);
  }
  auto backend = std::make_shared<SyntheticBackend>(name + "." + std::string(type_token));
  for (std::uint32_t i = 0; i < domain_count; ++i) {
    SyntheticDomainConfig config = make_synthetic_domain(
        ExecutionDomainId(i + 1), type, name + "." + std::string(type_token) + "." + std::to_string(i),
        provenance != Provenance::kUnsupported);
    config.provenance = provenance;
    config.parent_host = node;
    config.topology.host_node = node;
    config.compatibility.backend_family = name + "." + std::string(type_token);
    config.authoritative = provenance != Provenance::kUnsupported;
    if (provenance == Provenance::kUnsupported) {
      // No hardware and no deterministic backend: nothing is claimed at all.
      config.capability.operations.clear();
      config.capability.flags = 0;
      config.capability.unproven_flags = 0;
    } else {
      config.capability = make_host_executor_capability(
          kMaxDispatchPayloadBytes, 256, 8, config.compatibility.backend_family,
          memory_domain_mask_for(type), 1);
    }
    const Status added = backend->add_domain(std::move(config));
    if (!added) {
      std::cerr << "failed to configure domain: " << added.code << std::endl;
      return nullptr;
    }
  }
  return backend;
}

}  // namespace

int main(int argc, char** argv) {
  tos_cli::Arguments arguments = tos_cli::parse(argc, argv);
  if (arguments.has("help")) {
    tos_cli::print_usage("tos_worker", R"(usage: tos_worker --port N [options]

  --host HOST                 coordinator address (default 127.0.0.1)
  --port N                    coordinator port (required)
  --name NAME                 worker name
  --node NAME                 host identity this worker reports
  --type CLASS                cpu | accelerator | nic | smartnic | dpu | other
  --domains N                 number of domains to publish (default 1)
  --provenance P              real | synthetic | unsupported (default depends on the class)
  --domain-base N             base identity for published domains
  --fault F                   none | reject-dispatch | drop-completion |
                              ambiguous-completion | failure-report | die-after-apply
  --fault-after N             apply the fault once N executions have completed
  --publish-load N            re-publish load evidence N times after registering
  --log-level L               error | warn | info | debug

Prints "WORKER_READY <worker-id> <boot-id>" on stdout once registered.)");
    return 0;
  }

  ExecutionDomainType type = ExecutionDomainType::kCpu;
  std::string type_token = arguments.get("type", "cpu");
  if (!parse_enum(type_token, type)) {
    std::cerr << "unknown domain class: " << type_token << std::endl;
    return 2;
  }
  Provenance provenance = type == ExecutionDomainType::kCpu ? Provenance::kReal
                                                            : Provenance::kSynthetic;
  if (arguments.has("provenance")) {
    if (!parse_enum(arguments.get("provenance"), provenance)) {
      std::cerr << "unknown provenance token" << std::endl;
      return 2;
    }
  }
  const std::uint16_t port = arguments.get_u16("port", 0);
  if (port == 0) {
    std::cerr << "--port is required" << std::endl;
    return 2;
  }
  LogLevel level = LogLevel::kInfo;
  if (arguments.has("log-level")) {
    const std::string token = arguments.get("log-level");
    if (token == "error") level = LogLevel::kError;
    else if (token == "warn") level = LogLevel::kWarn;
    else if (token == "debug") level = LogLevel::kDebug;
  }
  log_set_level(level);

  const std::uint32_t domain_count =
      static_cast<std::uint32_t>(arguments.get_u64("domains", 1));
  std::shared_ptr<IExecutionBackend> backend =
      make_backend(arguments, provenance, type_token, type, domain_count == 0 ? 1 : domain_count);
  if (backend == nullptr) return 1;

  dist::WorkerConfig config;
  config.coordinator_host = arguments.get("host", "127.0.0.1");
  config.coordinator_port = port;
  config.name = arguments.get("name", "worker");
  config.host = arguments.get("node", "local.host");
  config.backend = backend;
  config.provenance = provenance;
  config.domain_id_base = arguments.get_u64("domain-base", 0);
  if (arguments.has("fault")) {
    dist::WorkerFault fault{};
    if (!dist::parse_worker_fault(arguments.get("fault"), fault)) {
      std::cerr << "unknown fault token" << std::endl;
      return 2;
    }
    config.fault = fault;
  }
  config.fault_after_executions = static_cast<std::uint32_t>(arguments.get_u64("fault-after", 0));

  dist::OffloadWorker worker(std::move(config));
  const Status registered = worker.connect_and_register();
  if (!registered) {
    std::cerr << "worker registration failed: " << registered.code << " " << registered.message
              << std::endl;
    return 1;
  }
  log_write_stdout("WORKER_READY " + format_id(worker.worker_id().value()) + " " +
                   format_id(worker.boot_id().value()));

  const std::uint64_t republish = arguments.get_u64("publish-load", 0);
  for (std::uint64_t i = 0; i < republish; ++i) {
    const Status published = worker.publish_load();
    if (!published) {
      std::cerr << "load publication failed: " << published.code << std::endl;
      return 1;
    }
  }

  const Status served = worker.run();
  log_write_stdout("WORKER_STOPPED executions=" + std::to_string(worker.executions()));
  return static_cast<bool>(served) ? 0 : 1;
}
