// Adversarial protocol proof: hostile input is rejected by the decoder and by the
// transport boundary before it can change any scheduler state.
//
// Pure decoder cases call tos::dist::decode_header / tos::dist::decode_frame
// directly and assert the exact DecodeError value. Transport cases drive a real
// OffloadCoordinator over real TCP sockets: the server must answer with an Error
// frame carrying a protocol.* code, close the connection, and keep serving.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "tos/dist/client.hpp"
#include "tos/dist/coordinator.hpp"
#include "tos/dist/protocol.hpp"
#include "tos_test_support.hpp"

using namespace tos;

namespace {

// ---- canonical state fingerprint -------------------------------------------

/// Canonical rendering of the authoritative state an attack must not touch:
/// domains, attempts and reservation accounting. Counters that legitimately move
/// when a hostile frame is rejected (completion_rejections, dispatch_rejections)
/// are deliberately excluded, and SchedulerSnapshot::render_json() is not used
/// directly because every snapshot() call advances its own snapshot_generation.
std::string authority_fingerprint(Scheduler& scheduler) {
  std::ostringstream out;
  out << "epoch=" << scheduler.coordinator_epoch().value()
      << " policy=" << scheduler.policy_generation().value()
      << " domains=" << scheduler.domains().size()
      << " mutation=" << scheduler.domains().mutation_sequence() << ';';
  for (const ExecutionDomainRecord& domain : scheduler.domains().all()) {
    out << "domain:" << domain.id.value() << ':' << domain.generation.value() << ':'
        << domain.capability.generation.value() << ':' << static_cast<int>(domain.type) << ':'
        << static_cast<int>(domain.provenance) << ':' << (domain.fenced ? 1 : 0) << ':'
        << domain.worker.value() << ':' << domain.worker_boot.value() << ':'
        << (domain.capability.authoritative ? 1 : 0) << ':'
        << domain.capability.capability.operations.size() << ':'
        << domain.load.health_generation.value() << ';';
  }
  for (const ExecutionAttempt& attempt : scheduler.attempts().list(4096)) {
    out << "attempt:" << attempt.id.value() << ':' << attempt.generation.value() << ':'
        << static_cast<int>(attempt.state) << ':' << static_cast<int>(attempt.resolution) << ':'
        << (attempt.completion_committed ? 1 : 0) << ':' << attempt.result.result_digest << ':'
        << (attempt.result.success ? 1 : 0) << ':' << attempt.worker.value() << ':'
        << attempt.worker_boot.value() << ':' << attempt.coordinator_epoch.value() << ';';
  }
  const ReservationAudit reservations = scheduler.reservations().audit();
  out << "reservations:" << reservations.outstanding << ':' << reservations.committed << ':'
      << reservations.leaked << ':' << (reservations.consistent ? 1 : 0) << ';';
  return out.str();
}

/// Worker-incarnation view. Kept separate because a rejected worker handshake ends
/// the session, and a closed worker session legitimately fences its boot through
/// the control path.
std::string worker_fingerprint(const WorkerAuthority& workers) {
  std::ostringstream out;
  std::vector<std::pair<WorkerId, WorkerBootId>> live = workers.live_boots();
  std::sort(live.begin(), live.end(),
            [](const std::pair<WorkerId, WorkerBootId>& a,
               const std::pair<WorkerId, WorkerBootId>& b) { return a.second < b.second; });
  for (const std::pair<WorkerId, WorkerBootId>& entry : live) {
    out << "live:" << entry.first.value() << ':' << entry.second.value() << ';';
  }
  std::vector<WorkerBootId> fenced = workers.fenced_boots();
  std::sort(fenced.begin(), fenced.end());
  for (WorkerBootId boot : fenced) out << "fenced:" << boot.value() << ';';
  return out.str();
}

// ---- frame construction ----------------------------------------------------

void patch_u16(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t value) {
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

void patch_u32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

/// One frame header with every field set explicitly. Layout: magic u32 | version
/// u16 | type u16 | flags u16 | reserved u16 | length u32 | crc32c u32.
std::vector<std::uint8_t> header_bytes(std::uint32_t magic, std::uint16_t version,
                                       std::uint16_t type, std::uint16_t flags,
                                       std::uint16_t reserved, std::uint32_t length,
                                       std::uint32_t crc) {
  std::vector<std::uint8_t> bytes(dist::kFrameHeaderBytes, 0);
  patch_u32(bytes, 0, magic);
  patch_u16(bytes, 4, version);
  patch_u16(bytes, 6, type);
  patch_u16(bytes, 8, flags);
  patch_u16(bytes, 10, reserved);
  patch_u32(bytes, 12, length);
  patch_u32(bytes, 16, crc);
  return bytes;
}

/// A syntactically valid header-only frame for the given type.
std::vector<std::uint8_t> empty_frame(dist::MessageType type) {
  return header_bytes(dist::kFrameMagic, kProtocolVersion, static_cast<std::uint16_t>(type), 0, 0,
                      0, crc32c(nullptr, 0));
}

Checked<std::vector<std::uint8_t>> frame_bytes(dist::MessageType type,
                                               const std::vector<std::uint8_t>& payload) {
  return dist::encode_frame(type, payload, dist::kMaxFrameBytes);
}

std::vector<std::uint8_t> hello_frame(dist::PeerRole role, const std::string& name, WorkerId worker,
                                      WorkerBootId boot) {
  dist::HelloMessage hello;
  hello.role = role;
  hello.name = name;
  hello.worker = worker;
  hello.boot = boot;
  hello.host = "127.0.0.1";
  hello.pid = 4242;
  hello.version = std::string(kVersionString);
  return dist::encode(hello);
}

/// A structurally valid synthetic domain record, used as the payload of hostile
/// DOMAIN_PUBLISH frames.
ExecutionDomainRecord synthetic_record(ExecutionDomainId id, ExecutionDomainType type,
                                       const std::string& name) {
  SyntheticBackend backend("protocol.adversarial.probe");
  const Status added = backend.add_domain(make_synthetic_domain(id, type, name));
  if (!added.ok) {
    tos_test::report_failure(__FILE__, __LINE__, "synthetic add_domain failed: " + added.code);
    return {};
  }
  const std::vector<ExecutionDomainRecord> discovered = backend.discover_domains();
  if (discovered.empty()) {
    tos_test::report_failure(__FILE__, __LINE__, "synthetic discovery returned no domain");
    return {};
  }
  return discovered.front();
}

// ---- raw transport peer ----------------------------------------------------

/// A raw TCP peer with no protocol help: every byte it sends is chosen by the test.
class RawConnection {
 public:
  explicit RawConnection(std::uint16_t port) {
    Checked<TcpSocket> connected = connect_tcp("127.0.0.1", port);
    if (!connected.ok()) {
      tos_test::report_failure(__FILE__, __LINE__,
                               "connect failed: " + connected.status.code + " " +
                                   connected.status.message);
      return;
    }
    socket_ = std::move(connected.value);
    socket_.set_nodelay(true);
  }

  [[nodiscard]] bool valid() const noexcept { return socket_.valid(); }
  [[nodiscard]] TcpSocket& socket() noexcept { return socket_; }

  bool send(const std::vector<std::uint8_t>& bytes) {
    const Status sent = socket_.send_all(bytes.data(), bytes.size());
    if (!sent.ok) {
      tos_test::report_failure(__FILE__, __LINE__, "send failed: " + sent.code);
      return false;
    }
    return true;
  }

  Checked<dist::Frame> read_frame() {
    std::vector<std::uint8_t> header(dist::kFrameHeaderBytes);
    const Status read = socket_.recv_exact(header.data(), header.size());
    if (!read.ok) return Checked<dist::Frame>::bad(read.code, read.message);
    dist::FrameHeader parsed;
    const dist::DecodeError error =
        dist::decode_header(header.data(), header.size(), dist::kMaxFrameBytes, parsed);
    if (error != dist::DecodeError::kOk) {
      return Checked<dist::Frame>::bad(std::string("protocol.") +
                                       std::string(dist::to_string(error)));
    }
    std::vector<std::uint8_t> buffer(dist::kFrameHeaderBytes + parsed.length, 0);
    std::copy(header.begin(), header.end(), buffer.begin());
    if (parsed.length > 0) {
      const Status body =
          socket_.recv_exact(buffer.data() + dist::kFrameHeaderBytes, parsed.length);
      if (!body.ok) return Checked<dist::Frame>::bad(body.code, body.message);
    }
    return dist::decode_frame(buffer.data(), buffer.size(), dist::kMaxFrameBytes);
  }

  dist::HelloAckMessage expect_hello_ack() {
    dist::HelloAckMessage ack;
    const Checked<dist::Frame> frame = read_frame();
    if (!frame.ok()) {
      tos_test::report_failure(__FILE__, __LINE__, "hello ack read failed: " + frame.status.code);
      return ack;
    }
    TOS_CHECK_EQ(frame.value.type, dist::MessageType::kHelloAck);
    if (frame.value.type != dist::MessageType::kHelloAck) return ack;
    TOS_CHECK_MSG(dist::decode(frame.value.payload, ack), "hello acknowledgement must decode");
    return ack;
  }

  dist::ErrorMessage expect_error() {
    dist::ErrorMessage error;
    const Checked<dist::Frame> frame = read_frame();
    if (!frame.ok()) {
      tos_test::report_failure(__FILE__, __LINE__, "error frame read failed: " + frame.status.code);
      return error;
    }
    TOS_CHECK_EQ(frame.value.type, dist::MessageType::kError);
    if (frame.value.type != dist::MessageType::kError) return error;
    TOS_CHECK_MSG(dist::decode(frame.value.payload, error), "error frame must decode");
    return error;
  }

  dist::DispatchRequestMessage expect_dispatch_request() {
    dist::DispatchRequestMessage message;
    const Checked<dist::Frame> frame = read_frame();
    if (!frame.ok()) {
      tos_test::report_failure(__FILE__, __LINE__,
                               "dispatch request read failed: " + frame.status.code);
      return message;
    }
    TOS_CHECK_EQ(frame.value.type, dist::MessageType::kDispatchRequest);
    if (frame.value.type != dist::MessageType::kDispatchRequest) return message;
    TOS_CHECK_MSG(dist::decode(frame.value.payload, message), "dispatch request must decode");
    return message;
  }

  dist::CompletionAckMessage expect_completion_ack() {
    dist::CompletionAckMessage ack;
    const Checked<dist::Frame> frame = read_frame();
    if (!frame.ok()) {
      tos_test::report_failure(__FILE__, __LINE__,
                               "completion ack read failed: " + frame.status.code);
      return ack;
    }
    TOS_CHECK_EQ(frame.value.type, dist::MessageType::kCompletionAck);
    if (frame.value.type != dist::MessageType::kCompletionAck) return ack;
    TOS_CHECK_MSG(dist::decode(frame.value.payload, ack), "completion acknowledgement must decode");
    return ack;
  }

  /// The peer must have closed the connection: a read returns end-of-stream.
  void expect_peer_closed() {
    std::uint8_t byte = 0;
    const Checked<std::size_t> result = socket_.recv_some(&byte, 1);
    const bool closed = !result.ok() || result.value == 0;
    TOS_CHECK_MSG(closed, "the coordinator must close a session whose frame was rejected");
  }

 private:
  TcpSocket socket_;
};

/// A legitimate client handshake proves the coordinator is still serving.
void expect_legitimate_client_hello(std::uint16_t port, dist::OffloadCoordinator& coordinator) {
  RawConnection client(port);
  TOS_CHECK(client.valid());
  if (!client.valid()) return;
  const std::vector<std::uint8_t> hello =
      hello_frame(dist::PeerRole::kClient, "adversarial.probe", WorkerId{}, WorkerBootId{});
  client.send(frame_bytes(dist::MessageType::kHello, hello).value);
  const dist::HelloAckMessage ack = client.expect_hello_ack();
  TOS_CHECK_MSG(ack.accepted, "the coordinator must keep accepting legitimate sessions");
  TOS_CHECK_EQ(ack.code, std::string("ok"));
  TOS_CHECK(coordinator.running());
}

// ---- pure decoder assertions ----------------------------------------------

void expect_header_error(const std::vector<std::uint8_t>& bytes, dist::DecodeError expected) {
  dist::FrameHeader header;
  const dist::DecodeError error =
      dist::decode_header(bytes.data(), bytes.size(), dist::kMaxFrameBytes, header);
  TOS_CHECK_MSG(error == expected,
                std::string("decode_header returned ") + std::string(dist::to_string(error)) +
                    ", expected " + std::string(dist::to_string(expected)));
}

void expect_frame_error(const std::vector<std::uint8_t>& bytes, dist::DecodeError expected) {
  const Checked<dist::Frame> decoded =
      dist::decode_frame(bytes.data(), bytes.size(), dist::kMaxFrameBytes);
  TOS_CHECK_MSG(!decoded.ok(), "a malformed frame must be rejected");
  TOS_CHECK_EQ(decoded.status.code,
               std::string("protocol.") + std::string(dist::to_string(expected)));
}

}  // namespace

// ---------------------------------------------------------------------------
// Legitimate client flow: the handshake must not desynchronize the stream.
// ---------------------------------------------------------------------------

// Regression guard: the handshake must send exactly one HELLO. An earlier revision
// framed and sent HELLO explicitly and then called exchange(kHello, ...), which sent
// it a second time; the coordinator answered both, one HelloAck stayed buffered in
// the client socket, and the next exchange -- any submit(), query() or admin() --
// consumed that stale handshake reply instead of its own response and failed with
// "client.unexpected_response".
TOS_TEST(protocol_client_handshake_does_not_desynchronize_the_stream) {
  dist::CoordinatorConfig coordinator_config;
  coordinator_config.bind_host = "127.0.0.1";
  coordinator_config.port = 0;
  dist::OffloadCoordinator coordinator(std::move(coordinator_config));
  TOS_REQUIRE(coordinator.start().ok);

  dist::ClientConfig client_config;
  client_config.host = "127.0.0.1";
  client_config.port = coordinator.port();
  client_config.name = "protocol.adversarial.client";
  dist::CoordinatorClient client(std::move(client_config));
  const Status handshake = client.connect_and_hello();
  TOS_CHECK_MSG(handshake.ok, "the legitimate client handshake must succeed: " + handshake.code);
  TOS_REQUIRE(handshake.ok);

  const Checked<dist::QueryResponseMessage> response = client.query(dist::QueryKind::kVersion);
  TOS_CHECK_MSG(response.ok(),
                "the first exchange after a handshake must answer its own request, got: " +
                    response.status.code + " " + response.status.message);
  TOS_REQUIRE(response.ok());
  TOS_CHECK(response.value.ok);
  TOS_CHECK(!response.value.body.empty());

  // The stream stays usable: a second exchange must answer in turn.
  const Checked<dist::QueryResponseMessage> snapshot = client.query(dist::QueryKind::kSnapshot);
  TOS_CHECK_MSG(snapshot.ok(), "the session must remain usable after the first exchange: " +
                                   snapshot.status.code);
  TOS_CHECK(client.close().ok);
  TOS_REQUIRE(coordinator.stop().ok);
}

// ---------------------------------------------------------------------------
// Pure decoder: every hostile buffer is rejected with its exact DecodeError.
// ---------------------------------------------------------------------------

TOS_TEST(protocol_decoder_rejects_malformed_buffers) {
  // Empty buffer.
  {
    dist::FrameHeader header;
    TOS_CHECK_EQ(dist::decode_header(nullptr, 0, dist::kMaxFrameBytes, header),
                 dist::DecodeError::kTruncatedHeader);
    const Checked<dist::Frame> empty = dist::decode_frame(nullptr, 0, dist::kMaxFrameBytes);
    TOS_CHECK(!empty.ok());
    TOS_CHECK_EQ(empty.status.code, std::string("protocol.empty"));
    const std::vector<std::uint8_t> nothing;
    expect_frame_error(nothing, dist::DecodeError::kEmpty);
  }

  // Every truncated header length from one byte to one byte short of a header.
  {
    const std::vector<std::uint8_t> complete = empty_frame(dist::MessageType::kHello);
    for (std::size_t length = 1; length < dist::kFrameHeaderBytes; ++length) {
      const std::vector<std::uint8_t> partial(complete.begin(),
                                              complete.begin() + static_cast<std::ptrdiff_t>(length));
      expect_header_error(partial, dist::DecodeError::kTruncatedHeader);
      expect_frame_error(partial, dist::DecodeError::kTruncatedHeader);
    }
  }

  // Bad magic.
  {
    std::vector<std::uint8_t> bytes = empty_frame(dist::MessageType::kHello);
    patch_u32(bytes, 0, dist::kFrameMagic ^ 0xFFFFFFFFU);
    expect_header_error(bytes, dist::DecodeError::kBadMagic);
    expect_frame_error(bytes, dist::DecodeError::kBadMagic);
  }

  // Unsupported version.
  {
    std::vector<std::uint8_t> bytes = empty_frame(dist::MessageType::kHello);
    patch_u16(bytes, 4, static_cast<std::uint16_t>(kProtocolVersion + 1));
    expect_header_error(bytes, dist::DecodeError::kBadVersion);
    expect_frame_error(bytes, dist::DecodeError::kBadVersion);
  }

  // Unknown message type: zero and far beyond the last defined type.
  {
    std::vector<std::uint8_t> zero = empty_frame(dist::MessageType::kHello);
    patch_u16(zero, 6, static_cast<std::uint16_t>(0));
    expect_header_error(zero, dist::DecodeError::kUnknownType);
    expect_frame_error(zero, dist::DecodeError::kUnknownType);

    std::vector<std::uint8_t> unknown = empty_frame(dist::MessageType::kHello);
    patch_u16(unknown, 6, static_cast<std::uint16_t>(9999));
    expect_header_error(unknown, dist::DecodeError::kUnknownType);
    expect_frame_error(unknown, dist::DecodeError::kUnknownType);
  }

  // Non-zero flags and non-zero reserved field are both structural rejections.
  {
    std::vector<std::uint8_t> flags = empty_frame(dist::MessageType::kHello);
    patch_u16(flags, 8, static_cast<std::uint16_t>(0x0001));
    expect_header_error(flags, dist::DecodeError::kUnsupportedFlags);
    expect_frame_error(flags, dist::DecodeError::kUnsupportedFlags);

    std::vector<std::uint8_t> reserved = empty_frame(dist::MessageType::kHello);
    patch_u16(reserved, 10, static_cast<std::uint16_t>(0x0080));
    expect_header_error(reserved, dist::DecodeError::kUnsupportedFlags);
    expect_frame_error(reserved, dist::DecodeError::kUnsupportedFlags);
  }

  // Declared length larger than the bound: rejected from the header alone, before
  // any body is read or allocated.
  {
    std::vector<std::uint8_t> bytes =
        header_bytes(dist::kFrameMagic, kProtocolVersion,
                     static_cast<std::uint16_t>(dist::MessageType::kHello), 0, 0, 2U * 1024U * 1024U,
                     crc32c(nullptr, 0));
    expect_header_error(bytes, dist::DecodeError::kOversized);
    expect_frame_error(bytes, dist::DecodeError::kOversized);
  }

  // Declared length smaller than the body, and larger than the body.
  {
    const std::vector<std::uint8_t> payload = tos_test::make_payload(64, 5);
    Checked<std::vector<std::uint8_t>> encoded =
        frame_bytes(dist::MessageType::kHello, payload);
    TOS_REQUIRE(encoded.ok());
    std::vector<std::uint8_t> smaller = encoded.value;
    patch_u32(smaller, 12, static_cast<std::uint32_t>(32));
    expect_header_error(smaller, dist::DecodeError::kOk);
    expect_frame_error(smaller, dist::DecodeError::kLengthMismatch);

    std::vector<std::uint8_t> larger = encoded.value;
    patch_u32(larger, 12, static_cast<std::uint32_t>(200));
    expect_header_error(larger, dist::DecodeError::kOk);
    expect_frame_error(larger, dist::DecodeError::kLengthMismatch);
  }

  // Good header, flipped payload byte: integrity failure, not acceptance.
  {
    const std::vector<std::uint8_t> payload = tos_test::make_payload(96, 9);
    Checked<std::vector<std::uint8_t>> encoded =
        frame_bytes(dist::MessageType::kHello, payload);
    TOS_REQUIRE(encoded.ok());
    std::vector<std::uint8_t> corrupted = encoded.value;
    corrupted[dist::kFrameHeaderBytes + 3] ^= 0x40U;
    expect_header_error(corrupted, dist::DecodeError::kOk);
    expect_frame_error(corrupted, dist::DecodeError::kIntegrityFailure);
  }

  // Trailing bytes after one complete frame are never accepted as that frame.
  {
    const std::vector<std::uint8_t> payload = tos_test::make_payload(32, 11);
    Checked<std::vector<std::uint8_t>> encoded =
        frame_bytes(dist::MessageType::kHello, payload);
    TOS_REQUIRE(encoded.ok());
    std::vector<std::uint8_t> trailing = encoded.value;
    trailing.insert(trailing.end(), 4, 0xABU);
    expect_frame_error(trailing, dist::DecodeError::kLengthMismatch);
  }

  // A frame bound below the header size is refused rather than treated as a frame.
  {
    const Checked<std::vector<std::uint8_t>> refused =
        dist::encode_frame(dist::MessageType::kHello, {}, dist::kFrameHeaderBytes - 1);
    TOS_CHECK(!refused.ok());
    TOS_CHECK_EQ(refused.status.code, std::string("protocol.bad_bound"));
  }
}

TOS_TEST(protocol_decoder_accepts_well_formed_frames) {
  const std::vector<std::uint8_t> payload = tos_test::make_payload(300, 3);
  Checked<std::vector<std::uint8_t>> encoded = frame_bytes(dist::MessageType::kHello, payload);
  TOS_REQUIRE(encoded.ok());

  dist::FrameHeader header;
  TOS_CHECK_EQ(dist::decode_header(encoded.value.data(), encoded.value.size(), dist::kMaxFrameBytes,
                                   header),
               dist::DecodeError::kOk);
  TOS_CHECK_EQ(header.type, dist::MessageType::kHello);
  TOS_CHECK_EQ(header.length, static_cast<std::uint32_t>(payload.size()));

  const Checked<dist::Frame> decoded =
      dist::decode_frame(encoded.value.data(), encoded.value.size(), dist::kMaxFrameBytes);
  TOS_REQUIRE(decoded.ok());
  TOS_CHECK_EQ(decoded.value.type, dist::MessageType::kHello);
  TOS_CHECK(decoded.value.payload == payload);
  TOS_CHECK_EQ(crc32c(payload.data(), payload.size()),
               crc32c(decoded.value.payload.data(), decoded.value.payload.size()));

  // The declared length may consume the whole bound exactly.
  const std::size_t bound = dist::kFrameHeaderBytes + 16;
  const std::vector<std::uint8_t> exact(16, 0x5AU);
  const Checked<std::vector<std::uint8_t>> at_bound =
      dist::encode_frame(dist::MessageType::kHeartbeat, exact, bound);
  TOS_CHECK(at_bound.ok());
  const Checked<dist::Frame> round_trip =
      dist::decode_frame(at_bound.value.data(), at_bound.value.size(), bound);
  TOS_REQUIRE(round_trip.ok());
  TOS_CHECK(round_trip.value.payload == exact);
}

// ---------------------------------------------------------------------------
// Transport: a real coordinator rejects hostile headers and keeps serving.
// ---------------------------------------------------------------------------

TOS_TEST(protocol_transport_rejects_hostile_headers_and_stays_alive) {
  dist::CoordinatorConfig config;
  config.bind_host = "127.0.0.1";
  config.port = 0;
  dist::OffloadCoordinator coordinator(std::move(config));
  TOS_REQUIRE(coordinator.start().ok);
  const std::uint16_t port = coordinator.port();
  TOS_CHECK(port != 0);
  Scheduler& scheduler = coordinator.scheduler();

  struct Attack {
    std::string name;
    std::vector<std::uint8_t> bytes;
    std::string expected_code;
  };
  std::vector<Attack> attacks;
  {
    std::vector<std::uint8_t> bytes = empty_frame(dist::MessageType::kHello);
    patch_u32(bytes, 0, 0xDEADBEEFU);
    attacks.push_back({"bad magic", std::move(bytes), "protocol.bad_magic"});
  }
  {
    std::vector<std::uint8_t> bytes = empty_frame(dist::MessageType::kHello);
    patch_u16(bytes, 4, static_cast<std::uint16_t>(kProtocolVersion + 7));
    attacks.push_back({"unsupported version", std::move(bytes), "protocol.bad_version"});
  }
  {
    std::vector<std::uint8_t> bytes = empty_frame(dist::MessageType::kHello);
    patch_u16(bytes, 6, static_cast<std::uint16_t>(0));
    attacks.push_back({"message type zero", std::move(bytes), "protocol.unknown_type"});
  }
  {
    std::vector<std::uint8_t> bytes = empty_frame(dist::MessageType::kHello);
    patch_u16(bytes, 6, static_cast<std::uint16_t>(9999));
    attacks.push_back({"unknown message type", std::move(bytes), "protocol.unknown_type"});
  }
  {
    std::vector<std::uint8_t> bytes = empty_frame(dist::MessageType::kHello);
    patch_u16(bytes, 8, static_cast<std::uint16_t>(0x00FF));
    attacks.push_back({"non-zero flags", std::move(bytes), "protocol.unsupported_flags"});
  }
  {
    std::vector<std::uint8_t> bytes = empty_frame(dist::MessageType::kHello);
    patch_u16(bytes, 10, static_cast<std::uint16_t>(0x0100));
    attacks.push_back({"non-zero reserved field", std::move(bytes), "protocol.unsupported_flags"});
  }
  {
    std::vector<std::uint8_t> bytes =
        header_bytes(dist::kFrameMagic, kProtocolVersion,
                     static_cast<std::uint16_t>(dist::MessageType::kHello), 0, 0, 2U * 1024U * 1024U,
                     crc32c(nullptr, 0));
    attacks.push_back({"declared length of two mebibytes", std::move(bytes), "protocol.oversized"});
  }

  const std::string before = authority_fingerprint(scheduler) + worker_fingerprint(scheduler.workers());
  for (const Attack& attack : attacks) {
    RawConnection connection(port);
    TOS_REQUIRE(connection.valid());
    TOS_CHECK(connection.send(attack.bytes));
    const dist::ErrorMessage error = connection.expect_error();
    TOS_CHECK_MSG(error.code == attack.expected_code,
                  attack.name + ": expected error code '" + attack.expected_code + "', got '" +
                      error.code + "'");
    TOS_CHECK_MSG(error.code.rfind("protocol.", 0) == 0,
                  attack.name + ": the rejection code must be in the protocol.* namespace");
    connection.expect_peer_closed();
    TOS_CHECK_MSG(authority_fingerprint(scheduler) + worker_fingerprint(scheduler.workers()) == before,
                  attack.name + ": a rejected frame must not change scheduler state");
    expect_legitimate_client_hello(port, coordinator);
  }

  TOS_CHECK(coordinator.running());
  TOS_REQUIRE(coordinator.stop().ok);
  TOS_CHECK_EQ(coordinator.session_count(), std::size_t{0});
}

TOS_TEST(protocol_transport_rejects_corrupted_payload) {
  dist::CoordinatorConfig config;
  config.bind_host = "127.0.0.1";
  config.port = 0;
  dist::OffloadCoordinator coordinator(std::move(config));
  TOS_REQUIRE(coordinator.start().ok);
  const std::uint16_t port = coordinator.port();
  Scheduler& scheduler = coordinator.scheduler();

  const std::vector<std::uint8_t> payload =
      hello_frame(dist::PeerRole::kWorker, "corrupt.worker", WorkerId(0x7777), WorkerBootId(0x8888));
  Checked<std::vector<std::uint8_t>> encoded = frame_bytes(dist::MessageType::kHello, payload);
  TOS_REQUIRE(encoded.ok());
  std::vector<std::uint8_t> corrupted = encoded.value;
  corrupted[dist::kFrameHeaderBytes + 2] ^= 0x01U;

  const std::string before = authority_fingerprint(scheduler) + worker_fingerprint(scheduler.workers());
  RawConnection connection(port);
  TOS_REQUIRE(connection.valid());
  TOS_CHECK(connection.send(corrupted));
  const dist::ErrorMessage error = connection.expect_error();
  TOS_CHECK_EQ(error.code, std::string("protocol.integrity_failure"));
  connection.expect_peer_closed();
  TOS_CHECK_EQ(authority_fingerprint(scheduler) + worker_fingerprint(scheduler.workers()), before);
  TOS_CHECK(!scheduler.workers().is_current(WorkerId(0x7777), WorkerBootId(0x8888)));

  expect_legitimate_client_hello(port, coordinator);
  TOS_REQUIRE(coordinator.stop().ok);
}

TOS_TEST(protocol_transport_rejects_trailing_bytes_after_a_complete_frame) {
  dist::CoordinatorConfig config;
  config.bind_host = "127.0.0.1";
  config.port = 0;
  dist::OffloadCoordinator coordinator(std::move(config));
  TOS_REQUIRE(coordinator.start().ok);
  const std::uint16_t port = coordinator.port();
  Scheduler& scheduler = coordinator.scheduler();

  const std::vector<std::uint8_t> hello =
      hello_frame(dist::PeerRole::kClient, "trailing.probe", WorkerId{}, WorkerBootId{});
  Checked<std::vector<std::uint8_t>> encoded = frame_bytes(dist::MessageType::kHello, hello);
  TOS_REQUIRE(encoded.ok());
  std::vector<std::uint8_t> bytes = encoded.value;
  const std::vector<std::uint8_t> garbage = header_bytes(0x01020304U, kProtocolVersion, 1, 0, 0, 0, 0);
  bytes.insert(bytes.end(), garbage.begin(), garbage.end());

  const std::string before = authority_fingerprint(scheduler) + worker_fingerprint(scheduler.workers());
  RawConnection connection(port);
  TOS_REQUIRE(connection.valid());
  TOS_CHECK(connection.send(bytes));

  // The complete frame is served; the trailing header is rejected as its own frame.
  const dist::HelloAckMessage ack = connection.expect_hello_ack();
  TOS_CHECK(ack.accepted);
  const dist::ErrorMessage error = connection.expect_error();
  TOS_CHECK_EQ(error.code, std::string("protocol.bad_magic"));
  connection.expect_peer_closed();

  TOS_CHECK_EQ(authority_fingerprint(scheduler) + worker_fingerprint(scheduler.workers()), before);
  expect_legitimate_client_hello(port, coordinator);
  TOS_REQUIRE(coordinator.stop().ok);
}

TOS_TEST(protocol_transport_truncated_header_is_never_admitted) {
  dist::CoordinatorConfig config;
  config.bind_host = "127.0.0.1";
  config.port = 0;
  dist::OffloadCoordinator coordinator(std::move(config));
  TOS_REQUIRE(coordinator.start().ok);
  const std::uint16_t port = coordinator.port();
  Scheduler& scheduler = coordinator.scheduler();

  const std::vector<std::uint8_t> complete = empty_frame(dist::MessageType::kHello);
  const std::string before = authority_fingerprint(scheduler) + worker_fingerprint(scheduler.workers());
  for (std::size_t length = 1; length < dist::kFrameHeaderBytes; ++length) {
    RawConnection connection(port);
    TOS_REQUIRE(connection.valid());
    const std::vector<std::uint8_t> partial(
        complete.begin(), complete.begin() + static_cast<std::ptrdiff_t>(length));
    TOS_CHECK(connection.send(partial));
    // The server admitted the connection and is waiting for the rest of the header:
    // the session exists while the frame is incomplete.
    tos_test::wait_until([&coordinator] { return coordinator.session_count() >= 1; });
    // A header the peer never finished is not a frame: the coordinator cannot answer
    // it, so the only correct behaviour is to end the session without any effect.
    connection.socket().close();
    tos_test::wait_until([&coordinator] { return coordinator.session_count() == 0; });
    TOS_CHECK_MSG(authority_fingerprint(scheduler) + worker_fingerprint(scheduler.workers()) == before,
                  "a partial header must not change scheduler state");
  }

  expect_legitimate_client_hello(port, coordinator);
  TOS_REQUIRE(coordinator.stop().ok);
}

// ---------------------------------------------------------------------------
// Identity replay: handshakes that must never grant an incarnation.
// ---------------------------------------------------------------------------

TOS_TEST(protocol_worker_boot_identity_attacks_are_rejected) {
  dist::CoordinatorConfig config;
  config.bind_host = "127.0.0.1";
  config.port = 0;
  dist::OffloadCoordinator coordinator(std::move(config));
  TOS_REQUIRE(coordinator.start().ok);
  const std::uint16_t port = coordinator.port();
  Scheduler& scheduler = coordinator.scheduler();

  // (a) HELLO naming an incarnation that was already fenced.
  const WorkerId fenced_worker(0x1101);
  const WorkerBootId fenced_boot(0x2202);
  TOS_REQUIRE(scheduler.register_worker_boot(fenced_worker, fenced_boot, "fenced.worker").ok);
  TOS_REQUIRE(scheduler.fence_worker_boot(fenced_boot, "adversarial test").ok);
  TOS_CHECK(scheduler.workers().fenced_boot_id(fenced_boot));
  {
    const std::string before =
        authority_fingerprint(scheduler) + worker_fingerprint(scheduler.workers());
    RawConnection connection(port);
    TOS_REQUIRE(connection.valid());
    TOS_CHECK(connection.send(frame_bytes(dist::MessageType::kHello,
                                          hello_frame(dist::PeerRole::kWorker, "fenced.worker",
                                                      fenced_worker, fenced_boot))
                                  .value));
    const dist::HelloAckMessage ack = connection.expect_hello_ack();
    TOS_CHECK_MSG(!ack.accepted, "a fenced incarnation must never be admitted again");
    TOS_CHECK_EQ(ack.code, std::string("worker.boot_fenced"));
    TOS_CHECK_EQ(authority_fingerprint(scheduler) + worker_fingerprint(scheduler.workers()), before);
    TOS_CHECK(scheduler.workers().fenced_boot_id(fenced_boot));
    TOS_CHECK(!scheduler.workers().is_current(fenced_worker, fenced_boot));
  }

  // (b) Duplicate registration of the same boot on one session.
  {
    const WorkerId worker(0x3303);
    const WorkerBootId boot(0x4404);
    RawConnection connection(port);
    TOS_REQUIRE(connection.valid());
    TOS_CHECK(connection.send(
        frame_bytes(dist::MessageType::kHello,
                    hello_frame(dist::PeerRole::kWorker, "duplicate.worker", worker, boot))
            .value));
    const dist::HelloAckMessage first = connection.expect_hello_ack();
    TOS_REQUIRE(first.accepted);
    const std::string before = authority_fingerprint(scheduler);

    TOS_CHECK(connection.send(
        frame_bytes(dist::MessageType::kHello,
                    hello_frame(dist::PeerRole::kWorker, "duplicate.worker", worker, boot))
            .value));
    const dist::HelloAckMessage second = connection.expect_hello_ack();
    TOS_CHECK_MSG(!second.accepted, "a replayed handshake must not re-register an incarnation");
    TOS_CHECK_EQ(second.code, std::string("worker.duplicate_registration"));
    TOS_CHECK_EQ(authority_fingerprint(scheduler), before);
    // Losing the connection is the control path that fences the incarnation; the
    // rejected replay must not leave the boot live.
    tos_test::wait_until([&scheduler, boot] { return scheduler.workers().fenced_boot_id(boot); });
  }

  expect_legitimate_client_hello(port, coordinator);
  TOS_REQUIRE(coordinator.stop().ok);
}

TOS_TEST(protocol_domain_publication_attacks_are_rejected) {
  dist::CoordinatorConfig config;
  config.bind_host = "127.0.0.1";
  config.port = 0;
  dist::OffloadCoordinator coordinator(std::move(config));
  TOS_REQUIRE(coordinator.start().ok);
  const std::uint16_t port = coordinator.port();
  Scheduler& scheduler = coordinator.scheduler();

  const WorkerId worker(0x5151);
  const WorkerBootId boot(0x6161);
  RawConnection connection(port);
  TOS_REQUIRE(connection.valid());
  TOS_CHECK(connection.send(
      frame_bytes(dist::MessageType::kHello,
                  hello_frame(dist::PeerRole::kWorker, "identity.worker", worker, boot))
          .value));
  const dist::HelloAckMessage ack = connection.expect_hello_ack();
  TOS_REQUIRE(ack.accepted);

  // (a) DOMAIN_PUBLISH whose record names a foreign incarnation.
  {
    const std::string before =
        authority_fingerprint(scheduler) + worker_fingerprint(scheduler.workers());
    ExecutionDomainRecord record = synthetic_record(ExecutionDomainId(9101),
                                                    ExecutionDomainType::kDpu,
                                                    "synthetic.dpu.foreign");
    TOS_REQUIRE(record.id.valid());
    record.worker = worker;
    record.worker_boot = WorkerBootId(0xDEAD);
    dist::DomainPublishMessage publish;
    publish.record = record;
    TOS_CHECK(connection.send(frame_bytes(dist::MessageType::kDomainPublish, dist::encode(publish)).value));
    const dist::ErrorMessage error = connection.expect_error();
    TOS_CHECK_EQ(error.code, std::string("session.identity_mismatch"));
    TOS_CHECK_EQ(authority_fingerprint(scheduler) + worker_fingerprint(scheduler.workers()), before);
    TOS_CHECK_MSG(!scheduler.domains().find(record.id).has_value(),
                  "a foreign publication must not register a domain");
  }

  // (b) A matching record is published, then load is claimed for a domain this
  // session never published.
  ExecutionDomainRecord owned = synthetic_record(ExecutionDomainId(9102),
                                                 ExecutionDomainType::kDpu,
                                                 "synthetic.dpu.owned");
  TOS_REQUIRE(owned.id.valid());
  owned.worker = worker;
  owned.worker_boot = boot;
  {
    dist::DomainPublishMessage publish;
    publish.record = owned;
    TOS_CHECK(connection.send(frame_bytes(dist::MessageType::kDomainPublish, dist::encode(publish)).value));
    tos_test::wait_until([&scheduler, &owned] {
      ExecutionDomainRecord stored;
      return scheduler.domains().get(owned.id, stored);
    });
  }
  {
    const std::string before =
        authority_fingerprint(scheduler) + worker_fingerprint(scheduler.workers());
    dist::LoadUpdateMessage load;
    load.domain = ExecutionDomainId(9103);
    load.load = owned.load;
    TOS_CHECK(connection.send(frame_bytes(dist::MessageType::kLoadUpdate, dist::encode(load)).value));
    const dist::ErrorMessage error = connection.expect_error();
    TOS_CHECK_EQ(error.code, std::string("session.domain_not_owned"));
    TOS_CHECK_EQ(authority_fingerprint(scheduler) + worker_fingerprint(scheduler.workers()), before);
    TOS_CHECK(!scheduler.domains().find(load.domain).has_value());
  }

  expect_legitimate_client_hello(port, coordinator);
  TOS_REQUIRE(coordinator.stop().ok);
}

// ---------------------------------------------------------------------------
// Completion replay: stale epoch and unknown identity must not commit.
// ---------------------------------------------------------------------------

TOS_TEST(protocol_completion_replay_attacks_are_rejected) {
  dist::CoordinatorConfig config;
  config.bind_host = "127.0.0.1";
  config.port = 0;
  dist::OffloadCoordinator coordinator(std::move(config));
  TOS_REQUIRE(coordinator.start().ok);
  const std::uint16_t port = coordinator.port();
  Scheduler& scheduler = coordinator.scheduler();
  const CoordinatorEpoch epoch = scheduler.coordinator_epoch();

  const WorkerId worker(0x7171);
  const WorkerBootId boot(0x8181);
  RawConnection connection(port);
  TOS_REQUIRE(connection.valid());
  TOS_CHECK(connection.send(
      frame_bytes(dist::MessageType::kHello,
                  hello_frame(dist::PeerRole::kWorker, "completion.worker", worker, boot))
          .value));
  TOS_REQUIRE(connection.expect_hello_ack().accepted);

  ExecutionDomainRecord record = synthetic_record(ExecutionDomainId(9201),
                                                  ExecutionDomainType::kDpu,
                                                  "synthetic.dpu.completion");
  TOS_REQUIRE(record.id.valid());
  record.worker = worker;
  record.worker_boot = boot;
  {
    // The publication sequence a real worker uses: structural record, capability,
    // then volatile load evidence. DOMAIN_PUBLISH deliberately carries no load.
    dist::DomainPublishMessage publish;
    publish.record = record;
    TOS_CHECK(connection.send(frame_bytes(dist::MessageType::kDomainPublish, dist::encode(publish)).value));
    dist::CapabilityPublishMessage capability;
    capability.capability = record.capability;
    TOS_CHECK(connection.send(
        frame_bytes(dist::MessageType::kCapabilityPublish, dist::encode(capability)).value));
    dist::LoadUpdateMessage load;
    load.domain = record.id;
    load.load = record.load;
    TOS_CHECK(connection.send(frame_bytes(dist::MessageType::kLoadUpdate, dist::encode(load)).value));
    tos_test::wait_until([&scheduler, &record] {
      ExecutionDomainRecord stored;
      return scheduler.domains().get(record.id, stored) && stored.load.published();
    });
  }

  // Dispatch a real attempt to the session, so every completion attack names an
  // attempt that genuinely exists.
  const std::vector<std::uint8_t> payload = tos_test::make_payload(512, 21);
  OperationRequest request = tos_test::make_request(opclass::checksum_crc32c(), payload.size());
  const DispatchOutcome dispatched = scheduler.plan_reserve_dispatch(
      request, std::span<const std::uint8_t>(payload.data(), payload.size()));
  std::string rejection_detail = dispatched.plan_result.status.code + " / " +
                                std::string(to_string(dispatched.plan_result.explanation.outcome));
  for (const CandidateEvaluation& candidate : dispatched.plan_result.explanation.candidates) {
    rejection_detail += " / candidate " + format_id(candidate.domain.value()) + " " +
                        std::string(to_string(candidate.reason)) + " " + candidate.detail;
  }
  TOS_CHECK_MSG(dispatched.plan_result.planned,
                "a domain published by a live worker session must be plannable: " +
                    rejection_detail);
  TOS_REQUIRE(dispatched.plan_result.planned);
  TOS_REQUIRE(dispatched.dispatch_result.dispatched);
  const ExecutionAttemptId attempt = dispatched.plan_result.plan.attempt;
  const dist::DispatchRequestMessage envelope = connection.expect_dispatch_request();
  TOS_CHECK_EQ(envelope.envelope.attempt, attempt);

  dist::CompletionMessage completion;
  completion.submission.attempt = envelope.envelope.attempt;
  completion.submission.generation = envelope.envelope.attempt_generation;
  completion.submission.dispatch = envelope.envelope.dispatch;
  completion.submission.completion = CompletionId(0x5150);
  completion.submission.worker = envelope.envelope.worker;
  completion.submission.worker_boot = envelope.envelope.worker_boot;
  completion.submission.coordinator_epoch = envelope.envelope.coordinator_epoch;
  completion.submission.domain_generation = envelope.envelope.domain_generation;
  completion.submission.capability_generation = envelope.envelope.capability_generation;
  completion.submission.operation = envelope.envelope.operation;
  completion.submission.provenance = envelope.envelope.provenance;
  completion.submission.result.success = true;
  completion.submission.result.result_digest = crc32c(payload.data(), payload.size());
  completion.submission.result.bytes_processed = payload.size();
  completion.submission.result.duration_ns = 1;
  completion.submission.result.backend_detail = "adversarial probe";

  // (a) Completion carrying a stale (future) coordinator epoch.
  {
    const std::string before = authority_fingerprint(scheduler);
    dist::CompletionMessage stale = completion;
    stale.submission.coordinator_epoch = CoordinatorEpoch(epoch.value() + 1);
    TOS_CHECK(connection.send(frame_bytes(dist::MessageType::kCompletion, dist::encode(stale)).value));
    const dist::CompletionAckMessage ack = connection.expect_completion_ack();
    TOS_CHECK_EQ(ack.attempt, attempt);
    TOS_CHECK_EQ(ack.rejection, CompletionRejection::kStaleCoordinatorEpoch);
    TOS_CHECK_MSG(!ack.committed, "a stale epoch must never commit");
    TOS_CHECK_EQ(authority_fingerprint(scheduler), before);
  }

  // (b) Completion for an attempt that does not exist, with the current epoch.
  {
    const std::string before = authority_fingerprint(scheduler);
    dist::CompletionMessage unknown = completion;
    unknown.submission.attempt = ExecutionAttemptId(0x5EEDULL);
    TOS_CHECK(connection.send(frame_bytes(dist::MessageType::kCompletion, dist::encode(unknown)).value));
    const dist::CompletionAckMessage ack = connection.expect_completion_ack();
    TOS_CHECK_EQ(ack.attempt, ExecutionAttemptId(0x5EEDULL));
    TOS_CHECK_EQ(ack.rejection, CompletionRejection::kUnknownAttempt);
    TOS_CHECK_MSG(!ack.committed, "an unknown attempt must never commit");
    TOS_CHECK_EQ(authority_fingerprint(scheduler), before);
  }

  // (c) Completion claiming a foreign worker incarnation is refused before the
  // completion authority is consulted at all.
  {
    const std::string before =
        authority_fingerprint(scheduler) + worker_fingerprint(scheduler.workers());
    dist::CompletionMessage foreign = completion;
    foreign.submission.worker_boot = WorkerBootId(0xFACE);
    TOS_CHECK(connection.send(frame_bytes(dist::MessageType::kCompletion, dist::encode(foreign)).value));
    const dist::ErrorMessage error = connection.expect_error();
    TOS_CHECK_EQ(error.code, std::string("session.identity_mismatch"));
    TOS_CHECK_EQ(authority_fingerprint(scheduler) + worker_fingerprint(scheduler.workers()), before);
  }

  // Control: with the attacks rejected, the identical legitimate completion is
  // still authoritative exactly once.
  {
    TOS_CHECK(connection.send(frame_bytes(dist::MessageType::kCompletion, dist::encode(completion)).value));
    const dist::CompletionAckMessage ack = connection.expect_completion_ack();
    TOS_CHECK_EQ(ack.attempt, attempt);
    TOS_CHECK_EQ(ack.rejection, CompletionRejection::kAccepted);
    TOS_CHECK_MSG(ack.committed, "the legitimate completion must commit");
  }
  ExecutionAttempt stored;
  TOS_REQUIRE(scheduler.attempts().get(attempt, stored));
  TOS_CHECK_EQ(stored.state, AttemptState::kCompleted);
  TOS_CHECK(stored.completion_committed);
  TOS_CHECK_EQ(stored.result.result_digest,
               static_cast<std::uint64_t>(crc32c(payload.data(), payload.size())));

  expect_legitimate_client_hello(port, coordinator);
  TOS_REQUIRE(coordinator.stop().ok);
}
