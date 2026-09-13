// tos_inspect: inspection and administrative command line tool.
//
// The tool is deliberately split into two dispatch paths:
//
//   * dispatch_read_only()   - inspection. It reads process-local facts, builds an
//                              in-process deterministic scenario, or asks a running
//                              coordinator a read-only question. It never mutates.
//   * dispatch_administrative() - mutation. It must be typed explicitly as the first
//                              argument, every command prints "mutation", and every
//                              command states the effect it had.
//
// The two paths never call each other and no read-only command can mutate state.
// The tool never spawns a process and never builds a command line from its arguments:
// every value read from argv is passed to a typed API call as a value.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cli_support.hpp"
#include "tos/backends/cpu_backend.hpp"
#include "tos/backends/local_publisher.hpp"
#include "tos/backends/nic_probe.hpp"
#include "tos/backends/synthetic.hpp"
#include "tos/dist/client.hpp"
#include "tos/dist/protocol.hpp"
#include "tos/tos.hpp"

using namespace tos;

namespace {

// ---------------------------------------------------------------------------
// Printing helpers
// ---------------------------------------------------------------------------

std::string flags_text(std::uint32_t bits) {
  std::string out;
  for (std::string_view name : describe_flags(bits)) {
    if (!out.empty()) out.push_back(' ');
    out += name;
  }
  return out.empty() ? std::string("(none)") : out;
}

std::string operation_text(const OperationClassRegistry& registry,
                           const std::vector<OperationClassId>& ids) {
  std::string out;
  for (OperationClassId id : ids) {
    const OperationClassDescriptor* descriptor = registry.find(id);
    if (!out.empty()) out.push_back(' ');
    out += descriptor == nullptr ? std::string("UNKNOWN") : descriptor->name;
  }
  return out.empty() ? std::string("(none)") : out;
}

std::string memory_domains_text(const CapabilitySet& capability) {
  std::string out;
  for (int i = 0; i < kMemoryDomainCount; ++i) {
    const auto value = static_cast<MemoryDomain>(i);
    if (!capability.memory_domain_supported(value)) continue;
    if (!out.empty()) out.push_back(' ');
    out += to_string(value);
  }
  return out.empty() ? std::string("(none)") : out;
}

std::string transport_classes_text(const CapabilitySet& capability) {
  std::string out;
  for (int i = 0; i < kTransportClassCount; ++i) {
    const auto value = static_cast<TransportClass>(i);
    if ((capability.transport_classes & transport_bit(value)) == 0U) continue;
    if (!out.empty()) out.push_back(' ');
    out += to_string(value);
  }
  return out.empty() ? std::string("(none)") : out;
}

std::string payload_classes_text(const CapabilitySet& capability) {
  std::string out;
  for (int i = 0; i < 4; ++i) {
    const auto value = static_cast<PayloadClass>(i);
    if ((capability.payload_classes & payload_bit(value)) == 0U) continue;
    if (!out.empty()) out.push_back(' ');
    out += to_string(value);
  }
  return out.empty() ? std::string("(none)") : out;
}

void print_capability(const OperationClassRegistry& registry, const CapabilitySet& capability,
                      std::uint32_t driver_backend_version, std::uint32_t protocol_version) {
  std::cout << "  operations: " << operation_text(registry, capability.operations) << "\n";
  std::cout << "  proven flags: " << flags_text(capability.flags) << "\n";
  std::cout << "  unproven flags: " << flags_text(capability.unproven_flags) << "\n";
  std::cout << "  memory domains: " << memory_domains_text(capability) << "\n";
  std::cout << "  transport classes: " << transport_classes_text(capability) << "\n";
  std::cout << "  payload classes: " << payload_classes_text(capability) << "\n";
  std::cout << "  payload bytes: min=" << capability.min_payload_bytes
            << " max=" << capability.max_payload_bytes << "\n";
  std::cout << "  alignment=" << capability.alignment_bytes
            << " max_concurrency=" << capability.max_concurrency
            << " queue_capacity=" << capability.queue_capacity << "\n";
  std::cout << "  backend_family=" << capability.backend_family
            << " driver_backend_version=" << driver_backend_version
            << " protocol_version=" << protocol_version << "\n";
}

void print_capacity(const CapacityVector& capacity) {
  std::cout << "  capacity:";
  for (std::size_t i = 0; i < capacity.size(); ++i) {
    std::cout << " " << to_string(static_cast<ResourceKind>(i)) << "=" << capacity[i];
  }
  std::cout << "\n";
}

// ---------------------------------------------------------------------------
// Local (in-process) inspection runtime
// ---------------------------------------------------------------------------

/// Scheduler plus synthetic backend and publisher, wired exactly as a deployment
/// wires them. Used only by read-only commands; nothing it does leaves the process.
struct InspectionRuntime {
  std::unique_ptr<Scheduler> scheduler;
  std::shared_ptr<SyntheticBackend> backend;
  std::unique_ptr<LocalDomainPublisher> publisher;
};

Status make_runtime(SchedulerPolicy policy, std::string host_node, InspectionRuntime& out) {
  SchedulerOptions options;
  options.policy = std::move(policy);
  options.host_node = std::move(host_node);
  out.scheduler = std::make_unique<Scheduler>(std::move(options));
  const Status started = out.scheduler->start();
  if (!started) return started;
  out.backend = std::make_shared<SyntheticBackend>("inspect.synthetic");
  out.publisher = std::make_unique<LocalDomainPublisher>(*out.scheduler, out.backend);
  return Status::success();
}

/// Publish one backend record through the normal evidence path.
Status publish_record(Scheduler& scheduler, const ExecutionDomainRecord& record) {
  const Status registered = scheduler.register_domain(record);
  if (!registered) return registered;
  const Status capability = scheduler.publish_capability(record.capability);
  if (!capability) return capability;
  const Status locality = scheduler.publish_locality(record.id, record.locality);
  if (!locality) return locality;
  const Status topology = scheduler.publish_topology(record.id, record.topology);
  if (!topology) return topology;
  const Status compatibility = scheduler.publish_compatibility(record.id, record.compatibility);
  if (!compatibility) return compatibility;
  const Status capacity = scheduler.publish_capacity(record.id, record.capacity);
  if (!capacity) return capacity;
  return scheduler.publish_load(record.id, record.load);
}

/// Publish a real host CPU domain through the normal evidence path.
Status register_cpu_domain(Scheduler& scheduler, ExecutionDomainId id, std::string_view name,
                           std::string_view host_node) {
  CpuBackend::Options options;
  options.domain_id = id;
  options.name = std::string(name);
  options.parent_host = std::string(host_node);
  CpuBackend probe(options);
  const std::vector<ExecutionDomainRecord> discovered = probe.discover_domains();
  if (discovered.empty()) return Status::failure("inspect.cpu_not_discovered");
  return publish_record(scheduler, discovered.front());
}

Status add_synthetic(InspectionRuntime& runtime, SyntheticDomainConfig config) {
  const Status added = runtime.backend->add_domain(std::move(config));
  if (!added) return added;
  return runtime.publisher->sync();
}

using FactorRow = std::pair<std::string, std::int64_t>;

std::vector<FactorRow> factor_rows(const FactorVector& factors,
                                   const std::array<std::int32_t, kRankingFactorCount>& weights) {
  std::vector<FactorRow> rows;
  rows.reserve(kRankingFactorCount);
  for (std::size_t i = 0; i < kRankingFactorCount; ++i) {
    const std::int32_t weight = weights[i];
    if (weight <= 0) continue;
    const std::int64_t contribution = static_cast<std::int64_t>(weight) *
                                      static_cast<std::int64_t>(factors[i]);
    rows.emplace_back(std::string(to_string(static_cast<RankingFactor>(i))), contribution);
  }
  std::sort(rows.begin(), rows.end(), [](const FactorRow& a, const FactorRow& b) {
    if (a.second != b.second) return a.second > b.second;
    return a.first < b.first;
  });
  return rows;
}

void print_explanation(const DecisionExplanation& explanation,
                       const std::array<std::int32_t, kRankingFactorCount>& weights) {
  std::cout << "outcome: " << to_string(explanation.outcome) << "\n";
  std::cout << "selected_domain: " << format_id(explanation.selected_domain.value())
            << " type=" << to_string(explanation.selected_domain_type)
            << " provenance=" << to_string(explanation.selected_provenance)
            << " offload=" << (explanation.selected_is_offload ? "yes" : "no") << "\n";
  std::cout << "evaluated_candidates=" << explanation.evaluated_candidates
            << " rejected_candidates=" << explanation.rejected_candidates
            << " fallback_used=" << (explanation.fallback_used ? "yes" : "no") << "\n";

  std::cout << "candidates:\n";
  for (const CandidateEvaluation& candidate : explanation.candidates) {
    std::cout << "  domain " << format_id(candidate.domain.value())
              << " type=" << to_string(candidate.domain_type)
              << " provenance=" << to_string(candidate.provenance)
              << " eligible=" << (candidate.eligible ? "yes" : "no")
              << " reason=" << to_string(candidate.reason) << "\n";
    std::cout << "    detail: " << candidate.detail << "\n";
  }

  std::cout << "ranking:\n";
  for (const RankedCandidate& ranked : explanation.ranking) {
    std::cout << "  rank " << ranked.rank << " domain " << format_id(ranked.domain_id)
              << " type=" << to_string(static_cast<ExecutionDomainType>(ranked.domain_type))
              << " score=" << ranked.weighted_score << " total_weight=" << ranked.total_weight
              << "\n";
    for (const FactorRow& row : factor_rows(ranked.factors, weights)) {
      std::cout << "    " << row.first << " contribution=" << row.second << "\n";
    }
  }

  std::cout << "reasons:\n";
  for (const DecisionReason& reason : explanation.reasons) {
    std::cout << "  " << reason.code;
    if (!reason.detail.empty()) std::cout << " (" << reason.detail << ")";
    std::cout << "\n";
  }
  std::cout << "explanation_json:\n" << render_explanation_json(explanation) << "\n";
}

// ---------------------------------------------------------------------------
// Remote coordinator session
// ---------------------------------------------------------------------------

/// CoordinatorClient::connect_and_hello() writes the hello frame twice and reads one
/// acknowledgement, so a session starts exactly one frame behind: the frame read by
/// the next exchange answers the previous request. This session detects that once and
/// then pairs every request with its own answer by sending a read-only version probe
/// whose response - the answer to the real request - is the frame that is read and
/// returned. A mutation is therefore sent exactly once and never replayed.
struct RemoteSession {
  RemoteSession(std::string host, std::uint16_t port)
      : client(tos::dist::ClientConfig{std::move(host), port, "tos_inspect",
                                       tos::dist::kMaxFrameBytes, 4}) {}

  tos::dist::CoordinatorClient client;
  bool lag_checked{false};
  bool lagged{false};
};

std::string frame_type_text(tos::dist::MessageType type) {
  return std::to_string(static_cast<unsigned>(type));
}

Status open_remote(RemoteSession& session) {
  const Status hello = session.client.connect_and_hello();
  if (!hello) {
    return Status::failure("inspect.connect_failed", hello.code + ": " + hello.message);
  }
  return Status::success();
}

Checked<tos::dist::Frame> exchange_request(RemoteSession& session, tos::dist::MessageType type,
                                           const std::vector<std::uint8_t>& payload,
                                           tos::dist::MessageType expected) {
  auto first = session.client.exchange(type, payload);
  if (!first.ok()) {
    return Checked<tos::dist::Frame>::bad(first.status.code, first.status.message);
  }
  if (!session.lag_checked) {
    session.lag_checked = true;
    session.lagged = first.value.type != expected;
  }
  if (!session.lagged) {
    if (first.value.type != expected) {
      return Checked<tos::dist::Frame>::bad(
          "inspect.unexpected_frame",
          "expected frame type " + frame_type_text(expected) + ", received " +
              frame_type_text(first.value.type));
    }
    return first;
  }
  // The lagged frame answered the previous request. A read-only version probe is sent
  // so that the next frame read is the answer to this request.
  tos::dist::QueryRequestMessage probe;
  probe.kind = tos::dist::QueryKind::kVersion;
  auto answer = session.client.exchange(tos::dist::MessageType::kQueryRequest,
                                        tos::dist::encode(probe));
  if (!answer.ok()) {
    return Checked<tos::dist::Frame>::bad(answer.status.code, answer.status.message);
  }
  if (answer.value.type != expected) {
    return Checked<tos::dist::Frame>::bad(
        "inspect.unexpected_frame",
        "expected frame type " + frame_type_text(expected) + ", received " +
            frame_type_text(answer.value.type));
  }
  return answer;
}

Checked<tos::dist::QueryResponseMessage> remote_query(RemoteSession& session,
                                                      tos::dist::QueryKind kind,
                                                      std::uint64_t id) {
  tos::dist::QueryRequestMessage request;
  request.kind = kind;
  request.id = id;
  auto frame = exchange_request(session, tos::dist::MessageType::kQueryRequest,
                                tos::dist::encode(request),
                                tos::dist::MessageType::kQueryResponse);
  if (!frame.ok()) {
    return Checked<tos::dist::QueryResponseMessage>::bad(frame.status.code, frame.status.message);
  }
  tos::dist::QueryResponseMessage response;
  if (!tos::dist::decode(frame.value.payload, response)) {
    return Checked<tos::dist::QueryResponseMessage>::bad("protocol.malformed_query_response");
  }
  return Checked<tos::dist::QueryResponseMessage>::good(std::move(response));
}

Checked<tos::dist::AdminResponseMessage> remote_admin(RemoteSession& session,
                                                      tos::dist::AdminAction action,
                                                      std::uint64_t id, std::string reason,
                                                      std::uint64_t value) {
  tos::dist::AdminRequestMessage request;
  request.action = action;
  request.id = id;
  request.reason = std::move(reason);
  request.value = value;
  auto frame = exchange_request(session, tos::dist::MessageType::kAdminRequest,
                                tos::dist::encode(request),
                                tos::dist::MessageType::kAdminResponse);
  if (!frame.ok()) {
    return Checked<tos::dist::AdminResponseMessage>::bad(frame.status.code, frame.status.message);
  }
  tos::dist::AdminResponseMessage response;
  if (!tos::dist::decode(frame.value.payload, response)) {
    return Checked<tos::dist::AdminResponseMessage>::bad("protocol.malformed_admin_response");
  }
  return Checked<tos::dist::AdminResponseMessage>::good(std::move(response));
}

bool remote_endpoint(const tos_cli::Arguments& arguments, std::string& host, std::uint16_t& port) {
  host = arguments.get("host", "127.0.0.1");
  port = arguments.get_u16("port", 0);
  return port != 0;
}

// ---------------------------------------------------------------------------
// Usage
// ---------------------------------------------------------------------------

void print_usage() {
  tos_cli::print_usage("tos_inspect", R"(usage: tos_inspect <command> [options]

Read-only inspection (never mutates anything):
  version                     library identity, version, protocol, persistence format
  cpu                         real host CPU facts and the CPU capability set
  nics                        real network adapter inventory
  explain                     in-process deterministic decision with every candidate
  snapshot                    coordinator snapshot as JSON          (--host/--port)
  accounting                  coordinator reservation accounting   (--host/--port)
  reconcile                   coordinator reconciliation report    (--host/--port)
  attempt <id>                one attempt record                   (--host/--port)
  operation <id>              attempts of one operation            (--host/--port)
  version                     also queries the coordinator when --port is given

Administrative commands (MUTATION, typed explicitly, all require --host/--port):
  fence-domain <id> --reason R      withdraw one domain's authority
  fence-boot <id> --reason R        fence one worker incarnation
  save-state                        persist coordinator state now
  set-offload-requirement <token>   any | prefer_offload | offload_required | host_required
  shutdown                          ask the coordinator to stop

Options:
  --host HOST                 coordinator address (default 127.0.0.1)
  --port N                    coordinator TCP port (required for every remote command)
  --reason R                  reason recorded by fence-domain and fence-boot
  --help                      print this text

Exit codes: 0 success, 1 runtime failure, 2 usage error.)");
}

// ---------------------------------------------------------------------------
// Read-only commands
// ---------------------------------------------------------------------------

int command_version(const tos_cli::Arguments& arguments) {
  std::cout << "library: " << kProjectName << "\n";
  std::cout << "version: " << kVersionString << "\n";
  std::cout << "build_version_string: " << TOS_VERSION_STRING << "\n";
  std::cout << "protocol_version: " << kProtocolVersion << "\n";
  std::cout << "persistence_format_version: " << kPersistenceFormatVersion << "\n";
  std::cout << "copyright: " << kCopyrightNotice << "\n";
  std::cout << "capability_flags: " << kCapabilityFlagCount
            << " ranking_factors: " << kRankingFactorCount
            << " resource_kinds: " << kResourceKindCount << "\n";
  std::string host;
  std::uint16_t port = 0;
  if (!arguments.has("port") && !arguments.has("host")) return 0;
  if (!remote_endpoint(arguments, host, port)) {
    std::cerr << "--port is required to query a coordinator" << std::endl;
    return 2;
  }
  RemoteSession session(host, port);
  const Status opened = open_remote(session);
  if (!opened) {
    std::cerr << "remote open failed: " << opened.code << " " << opened.message << std::endl;
    return 1;
  }
  auto response = remote_query(session, tos::dist::QueryKind::kVersion, 0);
  if (!response.ok()) {
    std::cerr << "remote query failed: " << response.status.code << " " << response.status.message
              << std::endl;
    return 1;
  }
  std::cout << "coordinator: " << host << ":" << port << " epoch="
            << session.client.epoch().value() << "\n";
  std::cout << "coordinator_version: " << response.value.body << "\n";
  return 0;
}

int command_cpu() {
  std::cout << "cpu_brand: " << CpuBackend::cpu_brand() << "\n";
  std::cout << "hardware_threads: " << CpuBackend::hardware_threads() << "\n";
  CpuBackend::Options options;
  options.domain_id = ExecutionDomainId(1);
  options.name = "cpu.host.0";
  const ExecutionDomainRecord record = CpuBackend::describe(options);
  std::cout << "domain: " << record.name << " id=" << format_id(record.id.value())
            << " type=" << to_string(record.type)
            << " provenance=" << to_string(record.provenance)
            << " isolation=" << to_string(record.isolation) << "\n";
  std::cout << "parent_device: " << record.parent_device << "\n";
  std::cout << "parent_host: " << record.parent_host << "\n";
  std::cout << "locality: class_to_payload=" << to_string(record.locality.class_to_payload)
            << " numa_node=" << record.locality.numa_node
            << " has_local_nic=" << (record.locality.has_local_nic ? "yes" : "no") << "\n";
  std::cout << "capability authoritative=" << (record.capability.authoritative ? "yes" : "no")
            << " generation=" << record.capability.generation.value()
            << " evidence=" << record.capability.evidence.value() << "\n";
  print_capability(OperationClassRegistry::builtins(), record.capability.capability,
                   record.compatibility.driver_backend_version,
                   record.compatibility.protocol_version);
  print_capacity(record.capacity);
  return 0;
}

int command_nics() {
  const std::vector<NicInfo> nics = enumerate_nics();
  const std::string inventory = describe_nic_inventory(nics);
  std::cout << "adapters: " << nics.size() << "\n";
  std::cout << inventory;
  if (!inventory.empty() && inventory.back() != '\n') std::cout << "\n";
  for (const NicInfo& nic : nics) {
    std::cout << "index=" << nic.index << " name=" << nic.name
              << " up=" << (nic.up ? "yes" : "no")
              << " loopback=" << (nic.loopback ? "yes" : "no")
              << " link_speed_bps=" << nic.link_speed_bps
              << " mac=" << (nic.mac.empty() ? std::string("(unknown)") : nic.mac) << "\n";
    std::cout << "  description: "
              << (nic.description.empty() ? std::string("(none)") : nic.description) << "\n";
  }
  std::cout << "note: a discovered adapter proves the adapter exists, not that it "
               "implements transport offload; such a domain is published UNPROVEN.\n";
  return 0;
}

int command_explain() {
  SchedulerPolicy policy = make_default_policy();
  // Host execution shares the caller's process; this scenario requires an engine that
  // proves a separate execution context, so the real CPU domain is hard-ineligible.
  policy.minimum_isolation = IsolationClass::kSeparateProcess;
  InspectionRuntime runtime;
  const Status built = make_runtime(policy, "inspect.host", runtime);
  if (!built) {
    std::cerr << "scenario construction failed: " << built.code << " " << built.message
              << std::endl;
    return 1;
  }
  const Status cpu = register_cpu_domain(*runtime.scheduler, ExecutionDomainId(1), "cpu.host.0",
                                         "inspect.host");
  if (!cpu) {
    std::cerr << "cpu domain publication failed: " << cpu.code << std::endl;
    return 1;
  }
  SyntheticDomainConfig dpu = make_synthetic_domain(ExecutionDomainId(2), ExecutionDomainType::kDpu,
                                                    "synthetic.dpu0");
  dpu.locality.class_to_payload = LocalityClass::kSameNumaNode;
  if (!add_synthetic(runtime, std::move(dpu))) {
    std::cerr << "dpu publication failed" << std::endl;
    return 1;
  }
  SyntheticDomainConfig smartnic = make_synthetic_domain(
      ExecutionDomainId(3), ExecutionDomainType::kSmartNic, "synthetic.smartnic0");
  if (!add_synthetic(runtime, std::move(smartnic))) {
    std::cerr << "smartnic publication failed" << std::endl;
    return 1;
  }
  SyntheticDomainConfig accelerator = make_synthetic_domain(
      ExecutionDomainId(4), ExecutionDomainType::kAccelerator, "synthetic.accelerator0");
  accelerator.capability.operations.clear();
  if (!add_synthetic(runtime, std::move(accelerator))) {
    std::cerr << "accelerator publication failed" << std::endl;
    return 1;
  }
  NicInfo nic;
  nic.index = 0;
  nic.name = "deterministic.nic0";
  nic.description = "adapter described without proven offload capability";
  nic.link_speed_bps = 100000000000ULL;
  nic.mac = "02:00:00:00:00:01";
  nic.up = true;
  const ExecutionDomainRecord nic_record = make_nic_domain(
      nic, ExecutionDomainId(5), "inspect.host", WorkerId{}, WorkerBootId{});
  const Status nic_published = publish_record(*runtime.scheduler, nic_record);
  if (!nic_published) {
    std::cerr << "nic domain publication failed: " << nic_published.code << " "
              << nic_published.message << std::endl;
    return 1;
  }

  OperationRequest request;
  request.operation_class = opclass::checksum_crc32c();
  request.payload.size_bytes = 65536;
  request.payload.source_memory = MemoryDomain::kHost;
  request.payload.destination_memory = MemoryDomain::kHost;
  request.payload.transport_class = TransportClass::kRawFrames;
  request.payload.payload_class = PayloadClass::kOpaqueBytes;
  request.payload.alignment_bytes = 8;
  request.payload.segment_count = 1;

  const PlanResult result = runtime.scheduler->plan(request);
  std::cout << "scenario: in-process deterministic decision, 5 published domains\n";
  std::cout << "policy: offload_requirement=" << to_string(policy.offload_requirement)
            << " minimum_isolation=" << to_string(policy.minimum_isolation)
            << " require_positive_evidence=" << (policy.require_positive_evidence ? "yes" : "no")
            << "\n";
  std::cout << "plan_status: " << (result.status.ok ? std::string("ok") : result.status.code)
            << "\n";
  print_explanation(result.explanation, runtime.scheduler->policy().weights);
  const Status stopped = runtime.scheduler->shutdown();
  (void)stopped;
  return 0;
}

int command_remote_query(const std::string& command, const tos_cli::Arguments& arguments) {
  std::string host;
  std::uint16_t port = 0;
  if (!remote_endpoint(arguments, host, port)) {
    std::cerr << "--port is required for " << command << std::endl;
    return 2;
  }
  tos::dist::QueryKind kind = tos::dist::QueryKind::kSnapshot;
  std::uint64_t id = 0;
  if (command == "snapshot") {
    kind = tos::dist::QueryKind::kSnapshot;
  } else if (command == "accounting") {
    kind = tos::dist::QueryKind::kAccounting;
  } else if (command == "reconcile") {
    kind = tos::dist::QueryKind::kReconcile;
  } else if (command == "attempt" || command == "operation") {
    if (arguments.positional.size() < 2) {
      std::cerr << command << " requires an identity argument" << std::endl;
      return 2;
    }
    if (!parse_id(arguments.positional[1], id)) {
      std::cerr << "identity must be decimal or 0x-prefixed hexadecimal" << std::endl;
      return 2;
    }
    kind = command == "attempt" ? tos::dist::QueryKind::kAttempt : tos::dist::QueryKind::kOperation;
  } else {
    std::cerr << "unknown read-only command: " << command << std::endl;
    return 2;
  }

  RemoteSession session(host, port);
  const Status opened = open_remote(session);
  if (!opened) {
    std::cerr << "remote open failed: " << opened.code << " " << opened.message << std::endl;
    return 1;
  }
  auto response = remote_query(session, kind, id);
  if (!response.ok()) {
    std::cerr << "remote query failed: " << response.status.code << " " << response.status.message
              << std::endl;
    return 1;
  }
  std::cout << "coordinator: " << host << ":" << port
            << " epoch=" << session.client.epoch().value() << "\n";
  std::cout << "query: " << command << " id=" << format_id(id)
            << " status=" << response.value.code << "\n";
  std::cout << response.value.body;
  if (!response.value.body.empty() && response.value.body.back() != '\n') std::cout << "\n";
  return response.value.ok ? 0 : 1;
}

int dispatch_read_only(const std::string& command, const tos_cli::Arguments& arguments) {
  if (command == "version") return command_version(arguments);
  if (command == "cpu") return command_cpu();
  if (command == "nics") return command_nics();
  if (command == "explain") return command_explain();
  if (command == "snapshot" || command == "accounting" || command == "reconcile" ||
      command == "attempt" || command == "operation") {
    return command_remote_query(command, arguments);
  }
  std::cerr << "unknown read-only command: " << command << std::endl;
  return 2;
}

// ---------------------------------------------------------------------------
// Administrative commands: separate path, explicit, always labelled as mutation
// ---------------------------------------------------------------------------

struct AdministrativeCommand {
  std::string_view name;
  std::string_view effect;
  bool requires_identity;
  bool requires_reason;
  bool requires_token;
};

constexpr AdministrativeCommand kAdministrativeCommands[] = {
    {"fence-domain", "withdraws one domain's authority; plans bound to it are rejected",
     true, true, false},
    {"fence-boot", "fences one worker incarnation and every domain it owns", true, true, false},
    {"save-state", "writes the coordinator's durable state file now", false, false, false},
    {"set-offload-requirement", "replaces the coordinator's offload requirement", false, false,
     true},
    {"shutdown", "asks the coordinator to stop serving and shut the scheduler down", false, false,
     false},
};

const AdministrativeCommand* find_administrative(std::string_view name) {
  for (const AdministrativeCommand& command : kAdministrativeCommands) {
    if (command.name == name) return &command;
  }
  return nullptr;
}

bool is_administrative(std::string_view name) { return find_administrative(name) != nullptr; }

int administrative_usage_error(const AdministrativeCommand& command, std::string_view detail) {
  std::cerr << "mutation command " << command.name << ": " << detail << std::endl;
  return 2;
}

int dispatch_administrative(const std::string& command, const tos_cli::Arguments& arguments) {
  const AdministrativeCommand* spec = find_administrative(command);
  if (spec == nullptr) {
    std::cerr << "unknown administrative command: " << command << std::endl;
    return 2;
  }

  std::uint64_t id = 0;
  if (spec->requires_identity) {
    if (arguments.positional.size() < 2) {
      return administrative_usage_error(*spec, "an identity argument is required");
    }
    if (!parse_id(arguments.positional[1], id)) {
      return administrative_usage_error(*spec, "identity must be decimal or 0x-prefixed hex");
    }
  }
  const std::string reason = arguments.get("reason");
  if (spec->requires_reason && reason.empty()) {
    return administrative_usage_error(*spec, "--reason R is required");
  }
  std::uint64_t value = 0;
  if (spec->requires_token) {
    if (arguments.positional.size() < 2) {
      return administrative_usage_error(*spec,
                                        "an offload requirement token is required "
                                        "(any | prefer_offload | offload_required | host_required)");
    }
    OffloadRequirement requirement{};
    if (!parse_enum(arguments.positional[1], requirement)) {
      return administrative_usage_error(*spec, "unknown offload requirement token");
    }
    value = static_cast<std::uint64_t>(requirement);
  }

  std::string host;
  std::uint16_t port = 0;
  if (!remote_endpoint(arguments, host, port)) {
    return administrative_usage_error(*spec, "--port N is required: administrative commands "
                                              "act on a running coordinator");
  }

  tos::dist::AdminAction action = tos::dist::AdminAction::kFenceDomain;
  if (command == "fence-domain") action = tos::dist::AdminAction::kFenceDomain;
  else if (command == "fence-boot") action = tos::dist::AdminAction::kFenceBoot;
  else if (command == "save-state") action = tos::dist::AdminAction::kSaveState;
  else if (command == "set-offload-requirement") action = tos::dist::AdminAction::kSetOffloadRequirement;
  else if (command == "shutdown") action = tos::dist::AdminAction::kShutdownCoordinator;

  std::cout << "MUTATION: " << command << "\n";
  std::cout << "  target: " << host << ":" << port << "\n";
  std::cout << "  effect: " << spec->effect << "\n";
  if (spec->requires_identity) std::cout << "  identity: " << format_id(id) << "\n";
  if (spec->requires_reason) std::cout << "  reason: " << reason << "\n";
  if (spec->requires_token) {
    std::cout << "  offload_requirement: "
              << to_string(static_cast<OffloadRequirement>(value)) << "\n";
  }

  RemoteSession session(host, port);
  const Status opened = open_remote(session);
  if (!opened) {
    std::cout << "  result: failed (" << opened.code << " " << opened.message << ")\n";
    std::cerr << "remote open failed: " << opened.code << " " << opened.message << std::endl;
    return 1;
  }
  auto response = remote_admin(session, action, id, reason, value);
  if (!response.ok()) {
    std::cout << "  result: failed (" << response.status.code << " " << response.status.message
              << ")\n";
    std::cerr << "administrative request failed: " << response.status.code << " "
              << response.status.message << std::endl;
    return 1;
  }
  std::cout << "  result: " << (response.value.ok ? "applied" : "refused")
            << " code=" << response.value.code << "\n";
  if (!response.value.detail.empty()) std::cout << "  detail: " << response.value.detail << "\n";
  if (!response.value.body.empty()) {
    std::cout << "  body:\n" << response.value.body;
    if (response.value.body.back() != '\n') std::cout << "\n";
  }
  std::cout << "  note: this command mutated coordinator state; read-only commands do not.\n";
  return response.value.ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  const tos_cli::Arguments arguments = tos_cli::parse(argc, argv);
  if (arguments.has("help") || arguments.positional.empty()) {
    print_usage();
    return arguments.positional.empty() && !arguments.has("help") ? 2 : 0;
  }
  const std::string command = arguments.positional[0];
  // The two paths are disjoint: an administrative command never runs through the
  // read-only dispatcher and vice versa.
  if (is_administrative(command)) {
    return dispatch_administrative(command, arguments);
  }
  return dispatch_read_only(command, arguments);
}
