// tos_coordinator: hosts the scheduler and serves worker processes over TCP.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include <iostream>
#include <string>

#include "cli_support.hpp"
#include "tos/dist/coordinator.hpp"
#include "tos/tos.hpp"

using namespace tos;

int main(int argc, char** argv) {
  tos_cli::Arguments arguments = tos_cli::parse(argc, argv);
  if (arguments.has("help")) {
    tos_cli::print_usage("tos_coordinator", R"(usage: tos_coordinator [options]

  --bind HOST                 address to listen on (default 127.0.0.1)
  --port N                    TCP port, 0 selects an ephemeral port (default 0)
  --host-node NAME            stable identity of this host
  --state PATH                durable state file
  --persist                   enable persistence and recovery
  --no-recovery               start with persistence disabled
  --reserve                   require capacity reservations before dispatch
  --slots N                   execution slots to reserve per operation
  --offload-requirement TOK   any | prefer_offload | offload_required | host_required
  --preference LIST           comma separated domain classes, strongest first
  --max-sessions N            maximum concurrent worker/client sessions
  --log-level L               error | warn | info | debug

Prints "COORDINATOR_READY <port> <epoch>" on stdout once it is serving.)");
    return 0;
  }

  dist::CoordinatorConfig config;
  config.bind_host = arguments.get("bind", "127.0.0.1");
  config.port = arguments.get_u16("port", 0);
  config.max_sessions = static_cast<std::size_t>(arguments.get_u64("max-sessions", 32));
  config.scheduler.policy = make_default_policy();
  config.scheduler.host_node = arguments.get("host-node", "local.host");
  config.scheduler.enable_persistence = arguments.has("persist") && !arguments.has("no-recovery");
  config.scheduler.state_path = arguments.get("state", "transport_offload_scheduler.state");
  if (arguments.has("reserve")) {
    config.scheduler.policy.reservation.enabled = true;
    config.scheduler.policy.reservation.per_operation[static_cast<std::size_t>(
        ResourceKind::kExecutionSlot)] = arguments.get_u64("slots", 1);
  }
  if (arguments.has("offload-requirement")) {
    OffloadRequirement requirement{};
    if (!parse_enum(arguments.get("offload-requirement"), requirement)) {
      std::cerr << "unknown offload requirement token" << std::endl;
      return 2;
    }
    config.scheduler.policy.offload_requirement = requirement;
  }
  if (arguments.has("preference")) {
    std::vector<ExecutionDomainType> order;
    std::string list = arguments.get("preference");
    std::size_t start = 0;
    while (start <= list.size()) {
      const std::size_t comma = list.find(',', start);
      const std::string token = list.substr(start, comma == std::string::npos ? std::string::npos
                                                                             : comma - start);
      if (!token.empty()) {
        ExecutionDomainType type{};
        if (!parse_enum(token, type)) {
          std::cerr << "unknown domain class: " << token << std::endl;
          return 2;
        }
        order.push_back(type);
      }
      if (comma == std::string::npos) break;
      start = comma + 1;
    }
    if (!order.empty()) config.scheduler.policy.preference_order = order;
  }
  LogLevel level = LogLevel::kInfo;
  if (arguments.has("log-level")) {
    const std::string token = arguments.get("log-level");
    if (token == "error") level = LogLevel::kError;
    else if (token == "warn") level = LogLevel::kWarn;
    else if (token == "debug") level = LogLevel::kDebug;
  }
  log_set_level(level);

  dist::OffloadCoordinator coordinator(std::move(config));
  const Status started = coordinator.start();
  if (!started) {
    std::cerr << "coordinator start failed: " << started.code << " " << started.message << std::endl;
    return 1;
  }
  log_write_stdout("COORDINATOR_READY " + std::to_string(coordinator.port()) + " " +
                   std::to_string(coordinator.scheduler().coordinator_epoch().value()));

  // Serve until the control path requests a shutdown. Waiting is event driven: the
  // process is either killed as part of a process-death proof or asked to stop.
  coordinator.wait_for_stop();

  const Status stopped = coordinator.stop();
  if (!stopped) {
    std::cerr << "coordinator stop failed: " << stopped.code << std::endl;
    return 1;
  }
  log_write_stdout("COORDINATOR_STOPPED");
  return 0;
}
