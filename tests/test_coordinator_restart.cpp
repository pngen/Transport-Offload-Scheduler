// Coordinator restart proof with real processes and durable state.
//
// The coordinator is a real process, its death is a real kill, and the replacement
// process must reload only what is meaningful across a restart: identities yes,
// authority no.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "tos/dist/client.hpp"
#include "tos_test_support.hpp"

using namespace tos;

namespace {

std::string read_line_with(const std::shared_ptr<ProcessHandle>& handle, const std::string& token) {
  std::string collected;
  tos_test::wait_until([&] {
    collected += read_process_stdout(handle);
    return collected.find(token) != std::string::npos || !process_alive(handle);
  });
  const std::size_t position = collected.find(token);
  if (position == std::string::npos) return std::string();
  const std::size_t end = collected.find('\n', position);
  return collected.substr(position, end == std::string::npos ? std::string::npos : end - position);
}

std::uint16_t parse_port(const std::string& ready_line) {
  const std::size_t first = ready_line.find(' ');
  if (first == std::string::npos) return 0;
  const std::size_t second = ready_line.find(' ', first + 1);
  const std::string token =
      ready_line.substr(first + 1, second == std::string::npos ? std::string::npos : second - first - 1);
  return static_cast<std::uint16_t>(std::strtoul(token.c_str(), nullptr, 10));
}

std::uint64_t parse_epoch(const std::string& ready_line) {
  const std::size_t second = ready_line.find_last_of(' ');
  if (second == std::string::npos) return 0;
  return std::strtoull(ready_line.c_str() + second + 1, nullptr, 10);
}

}  // namespace

TOS_TEST(coordinator_restart_advances_epoch_and_requires_fresh_evidence) {
  const std::string state_path = tos_test::executable_directory() + "\\coordinator_restart.state";
  (void)tos::remove_file(state_path);

  // ---- first coordinator process -------------------------------------------
  auto first = tos_test::spawn_tool("tos_coordinator",
                                    {"--bind", "127.0.0.1", "--port", "0", "--host-node",
                                     "restart.host", "--state", state_path, "--persist",
                                     "--log-level", "warn"},
                                    "coordinator_first.stderr.log");
  TOS_REQUIRE(first.ok());
  const std::string ready_first = read_line_with(first.value, "COORDINATOR_READY");
  TOS_REQUIRE(ready_first.rfind("COORDINATOR_READY", 0) == 0);
  const std::uint16_t port = parse_port(ready_first);
  const std::uint64_t epoch_first = parse_epoch(ready_first);
  TOS_CHECK(port != 0);
  TOS_CHECK(epoch_first >= 1);

  auto worker = tos_test::spawn_tool(
      "tos_worker",
      {"--host", "127.0.0.1", "--port", std::to_string(port), "--name", "restart-dpu", "--type",
       "dpu", "--node", "restart.host", "--domain-base", "65536", "--provenance", "synthetic",
       "--log-level", "warn"},
      "worker_restart.stderr.log");
  TOS_REQUIRE(worker.ok());
  const std::string ready_worker = read_line_with(worker.value, "WORKER_READY");
  TOS_REQUIRE(ready_worker.rfind("WORKER_READY", 0) == 0);

  dist::ClientConfig client_config;
  client_config.host = "127.0.0.1";
  client_config.port = port;
  client_config.name = "restart-client";
  dist::CoordinatorClient client(client_config);
  TOS_REQUIRE(client.connect_and_hello().ok);
  TOS_CHECK_EQ(client.epoch().value(), epoch_first);

  const std::vector<std::uint8_t> payload = tos_test::make_payload(4096, 4242);
  OperationRequest request = tos_test::make_request(opclass::checksum_crc32c(), payload.size());

  // The worker must be visible before the first submission.
  {
    std::uint64_t spins = 0;
    tos_test::wait_until([&] {
      Checked<dist::SubmitResponseMessage> attempt = client.submit(request, payload, false);
      if (spins++ % 200000ULL == 0) {
        std::cout << "PROBE submit ok=" << attempt.ok() << " code=" << attempt.status.code
                  << " detail=" << attempt.status.message;
        if (attempt.ok()) {
          std::cout << " planned=" << attempt.value.planned
                    << " outcome=" << to_string(attempt.value.outcome)
                    << " domain=" << format_id(attempt.value.domain);
        }
        std::cout << std::endl;
      }
      return attempt.ok() && attempt.value.planned;
    });
  }
  Checked<dist::SubmitResponseMessage> submitted = client.submit(request, payload, true);
  TOS_REQUIRE(submitted.ok());
  TOS_CHECK_MSG(submitted.value.planned, submitted.value.code);
  TOS_REQUIRE(submitted.value.planned);
  TOS_CHECK(submitted.value.dispatched);
  const std::uint64_t operation = submitted.value.operation;
  const std::uint64_t attempt = submitted.value.attempt;

  // The operation must complete authoritatively before the restart.
  tos_test::wait_until([&] {
    Checked<dist::QueryResponseMessage> queried =
        client.query(dist::QueryKind::kAttempt, attempt);
    return queried.ok() && queried.value.body.find("COMPLETED") != std::string::npos;
  });
  Checked<dist::QueryResponseMessage> accounting = client.query(dist::QueryKind::kAccounting);
  TOS_REQUIRE(accounting.ok());
  TOS_CHECK(accounting.value.body.find("outstanding=0") != std::string::npos);
  // Durable structural state is established through the control path before the
  // process is killed; an abrupt kill must not be the only reason nothing persisted.
  Checked<dist::AdminResponseMessage> saved =
      client.admin(dist::AdminAction::kSaveState, 0, "checkpoint before restart");
  TOS_REQUIRE(saved.ok());
  TOS_CHECK_MSG(saved.value.ok, saved.value.code);
  const Status closed_first = client.close();
  TOS_CHECK(closed_first.ok);

  // ---- kill the coordinator process ----------------------------------------
  TOS_REQUIRE(process_alive(first.value));
  TOS_REQUIRE(terminate_process(first.value).ok);
  const auto first_exit = wait_process(first.value);
  TOS_CHECK(first_exit.ok());
  // The worker loses its session and exits on its own: no orphan process remains.
  const auto worker_exit = wait_process(worker.value);
  TOS_CHECK(worker_exit.ok());
  TOS_CHECK(tos::file_exists(state_path));

  // ---- replacement coordinator process -------------------------------------
  auto second = tos_test::spawn_tool("tos_coordinator",
                                     {"--bind", "127.0.0.1", "--port", "0", "--host-node",
                                      "restart.host", "--state", state_path, "--persist",
                                      "--log-level", "warn"},
                                     "coordinator_second.stderr.log");
  TOS_REQUIRE(second.ok());
  const std::string ready_second = read_line_with(second.value, "COORDINATOR_READY");
  TOS_REQUIRE(ready_second.rfind("COORDINATOR_READY", 0) == 0);
  const std::uint16_t port_second = parse_port(ready_second);
  const std::uint64_t epoch_second = parse_epoch(ready_second);
  TOS_CHECK_MSG(epoch_second > epoch_first, "epoch did not advance across a restart");
  TOS_CHECK(port_second != 0);

  dist::ClientConfig second_config;
  second_config.host = "127.0.0.1";
  second_config.port = port_second;
  second_config.name = "restart-client-2";
  dist::CoordinatorClient second_client(second_config);
  TOS_REQUIRE(second_client.connect_and_hello().ok);
  TOS_CHECK_EQ(second_client.epoch().value(), epoch_second);

  // The durable identity survived, but no recovered domain has current evidence.
  Checked<dist::QueryResponseMessage> snapshot = second_client.query(dist::QueryKind::kSnapshot);
  TOS_REQUIRE(snapshot.ok());
  TOS_CHECK(snapshot.value.body.find("0x0000000000010000") != std::string::npos);
  TOS_CHECK(snapshot.value.body.find("\"evidence_current\":false") != std::string::npos);
  TOS_CHECK(snapshot.value.body.find("\"completed_attempts\":1") != std::string::npos);

  // A recovered domain is not eligible until fresh evidence arrives.
  Checked<dist::SubmitResponseMessage> refused = second_client.submit(request, payload, true);
  TOS_REQUIRE(refused.ok());
  TOS_CHECK(!refused.value.planned);
  TOS_CHECK(refused.value.outcome == SelectionOutcome::kStaleEvidence ||
            refused.value.outcome == SelectionOutcome::kNoEligibleDomain);

  // Old epoch traffic is rejected at the wire: a worker heartbeat naming the previous
  // coordinator epoch is answered with an error and the session is closed.
  {
    auto socket_result = connect_tcp("127.0.0.1", port_second);
    TOS_REQUIRE(socket_result.ok());
    TcpSocket socket = std::move(socket_result.value);
    dist::HelloMessage hello;
    hello.role = dist::PeerRole::kWorker;
    hello.name = "stale-epoch-worker";
    hello.worker = WorkerId(0xABCDEF01ULL);
    hello.boot = WorkerBootId(0xABCDEF02ULL);
    hello.host = "restart.host";
    auto hello_frame = dist::encode_frame(dist::MessageType::kHello, dist::encode(hello));
    TOS_REQUIRE(hello_frame.ok());
    TOS_REQUIRE(socket.send_all(hello_frame.value.data(), hello_frame.value.size()).ok);

    std::vector<std::uint8_t> header(dist::kFrameHeaderBytes);
    TOS_REQUIRE(socket.recv_exact(header.data(), header.size()).ok);
    dist::FrameHeader parsed;
    TOS_REQUIRE(dist::decode_header(header.data(), header.size(), dist::kMaxFrameBytes, parsed) ==
                dist::DecodeError::kOk);
    std::vector<std::uint8_t> buffer(dist::kFrameHeaderBytes + parsed.length, 0);
    std::copy(header.begin(), header.end(), buffer.begin());
    if (parsed.length > 0) {
      TOS_REQUIRE(socket.recv_exact(buffer.data() + dist::kFrameHeaderBytes, parsed.length).ok);
    }
    auto ack_frame = dist::decode_frame(buffer.data(), buffer.size(), dist::kMaxFrameBytes);
    TOS_REQUIRE(ack_frame.ok());
    dist::HelloAckMessage ack;
    TOS_REQUIRE(dist::decode(ack_frame.value.payload, ack));
    TOS_CHECK(ack.accepted);

    dist::HeartbeatMessage heartbeat;
    heartbeat.worker = hello.worker;
    heartbeat.boot = hello.boot;
    heartbeat.epoch = CoordinatorEpoch(epoch_first);  // the epoch of the dead coordinator
    heartbeat.sequence = 1;
    auto heartbeat_frame =
        dist::encode_frame(dist::MessageType::kHeartbeat, dist::encode(heartbeat));
    TOS_REQUIRE(heartbeat_frame.ok());
    TOS_REQUIRE(socket.send_all(heartbeat_frame.value.data(), heartbeat_frame.value.size()).ok);

    TOS_REQUIRE(socket.recv_exact(header.data(), header.size()).ok);
    TOS_REQUIRE(dist::decode_header(header.data(), header.size(), dist::kMaxFrameBytes, parsed) ==
                dist::DecodeError::kOk);
    TOS_CHECK_EQ(parsed.type, dist::MessageType::kError);
    std::vector<std::uint8_t> error_buffer(dist::kFrameHeaderBytes + parsed.length, 0);
    std::copy(header.begin(), header.end(), error_buffer.begin());
    if (parsed.length > 0) {
      TOS_REQUIRE(
          socket.recv_exact(error_buffer.data() + dist::kFrameHeaderBytes, parsed.length).ok);
    }
    auto error_frame = dist::decode_frame(error_buffer.data(), error_buffer.size(), dist::kMaxFrameBytes);
    TOS_REQUIRE(error_frame.ok());
    dist::ErrorMessage error;
    TOS_REQUIRE(dist::decode(error_frame.value.payload, error));
    TOS_CHECK_EQ(error.code, "coordinator.stale_epoch");
    socket.close();
  }

  // A replacement worker with a fresh incarnation republishes the domain and the
  // runtime becomes usable again, with a strictly greater domain generation.
  auto replacement = tos_test::spawn_tool(
      "tos_worker",
      {"--host", "127.0.0.1", "--port", std::to_string(port_second), "--name", "restart-dpu-2",
       "--type", "dpu", "--node", "restart.host", "--domain-base", "65536", "--provenance",
       "synthetic", "--log-level", "warn"},
      "worker_restart_2.stderr.log");
  TOS_REQUIRE(replacement.ok());
  const std::string ready_replacement = read_line_with(replacement.value, "WORKER_READY");
  TOS_REQUIRE(ready_replacement.rfind("WORKER_READY", 0) == 0);

  Checked<dist::SubmitResponseMessage> replanned =
      [&] {
        Checked<dist::SubmitResponseMessage> outcome = second_client.submit(request, payload, true);
        tos_test::wait_until([&] {
          outcome = second_client.submit(request, payload, true);
          return outcome.ok() && outcome.value.dispatched;
        });
        return outcome;
      }();
  TOS_REQUIRE(replanned.ok());
  TOS_CHECK(replanned.value.dispatched);
  const std::uint64_t replanned_attempt = replanned.value.attempt;
  tos_test::wait_until([&] {
    Checked<dist::QueryResponseMessage> queried =
        second_client.query(dist::QueryKind::kAttempt, replanned_attempt);
    return queried.ok() && queried.value.body.find("COMPLETED") != std::string::npos;
  });
  Checked<dist::QueryResponseMessage> completed =
      second_client.query(dist::QueryKind::kAttempt, replanned_attempt);
  TOS_REQUIRE(completed.ok());
  TOS_CHECK(completed.value.body.find("committed yes") != std::string::npos);
  Checked<dist::QueryResponseMessage> operations =
      second_client.query(dist::QueryKind::kOperation, operation);
  TOS_REQUIRE(operations.ok());
  TOS_CHECK(operations.value.body.find("COMPLETED") != std::string::npos);

  // Shut the second coordinator down through the control path and collect processes.
  Checked<dist::AdminResponseMessage> shutdown =
      second_client.admin(dist::AdminAction::kShutdownCoordinator, 0, "test complete");
  TOS_CHECK(shutdown.ok());
  const auto second_exit = wait_process(second.value);
  TOS_CHECK(second_exit.ok());
  const auto replacement_exit = wait_process(replacement.value);
  TOS_CHECK(replacement_exit.ok());
  const Status closed_second = second_client.close();
  TOS_CHECK(closed_second.ok);
  close_process(first.value);
  close_process(second.value);
  close_process(worker.value);
  close_process(replacement.value);

  TOS_REQUIRE(tos::remove_file(state_path).ok);
  std::remove("coordinator_first.stderr.log");
  std::remove("coordinator_second.stderr.log");
  std::remove("worker_restart.stderr.log");
  std::remove("worker_restart_2.stderr.log");
}
