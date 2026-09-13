// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "tos/dist/client.hpp"

#include <utility>

#include "tos/util/log.hpp"

namespace tos {
namespace dist {

struct CoordinatorClient::Impl {
  ClientConfig config;
  SocketRuntime sockets;
  TcpSocket socket;
  CoordinatorEpoch epoch;
  bool connected{false};

  [[nodiscard]] Checked<Frame> exchange(MessageType type, const std::vector<std::uint8_t>& payload) {
    auto encoded = encode_frame(type, payload, config.max_frame_bytes);
    if (!encoded.ok()) return Checked<Frame>::bad(encoded.status.code, encoded.status.message);
    const Status sent = socket.send_all(encoded.value.data(), encoded.value.size());
    if (!sent) return Checked<Frame>::bad(sent.code, sent.message);

    std::vector<std::uint8_t> header(kFrameHeaderBytes);
    const Status read_header = socket.recv_exact(header.data(), kFrameHeaderBytes);
    if (!read_header) return Checked<Frame>::bad(read_header.code, read_header.message);
    FrameHeader parsed;
    const DecodeError error = decode_header(header.data(), header.size(), config.max_frame_bytes, parsed);
    if (error != DecodeError::kOk) {
      return Checked<Frame>::bad(std::string("protocol.") + std::string(to_string(error)));
    }
    std::vector<std::uint8_t> buffer(kFrameHeaderBytes + parsed.length, 0);
    std::copy(header.begin(), header.end(), buffer.begin());
    if (parsed.length > 0) {
      const Status body = socket.recv_exact(buffer.data() + kFrameHeaderBytes, parsed.length);
      if (!body) return Checked<Frame>::bad(body.code, body.message);
    }
    return decode_frame(buffer.data(), buffer.size(), config.max_frame_bytes);
  }
};

CoordinatorClient::CoordinatorClient(ClientConfig config) : impl_(std::make_unique<Impl>()) {
  impl_->config = std::move(config);
}

CoordinatorClient::~CoordinatorClient() {
  const Status closed = close();
  (void)closed;
}

Status CoordinatorClient::connect_and_hello() {
  auto connected = connect_tcp_retry(impl_->config.host, impl_->config.port,
                                     impl_->config.connect_attempts);
  if (!connected.ok()) return connected.status;
  impl_->socket = std::move(connected.value);
  impl_->socket.set_nodelay(true);

  HelloMessage hello;
  hello.role = PeerRole::kClient;
  hello.name = impl_->config.name;
  hello.host = impl_->config.host;
  auto frame = impl_->exchange(MessageType::kHello, encode(hello));
  if (!frame.ok()) return frame.status;
  if (frame.value.type != MessageType::kHelloAck) {
    ErrorMessage error;
    if (frame.value.type == MessageType::kError && decode(frame.value.payload, error)) {
      return Status::failure(error.code, error.detail);
    }
    return Status::failure("client.unexpected_handshake");
  }
  HelloAckMessage ack;
  if (!decode(frame.value.payload, ack)) return Status::failure("client.malformed_hello_ack");
  if (!ack.accepted) return Status::failure(ack.code, ack.detail);
  impl_->epoch = ack.epoch;
  impl_->connected = true;
  return Status::success();
}

Status CoordinatorClient::close() {
  if (!impl_->connected) return Status::success();
  impl_->connected = false;
  GoodbyeMessage goodbye;
  goodbye.reason = "client closing";
  auto encoded = encode_frame(MessageType::kGoodbye, encode(goodbye), impl_->config.max_frame_bytes);
  if (encoded.ok()) {
    const Status sent = impl_->socket.send_all(encoded.value.data(), encoded.value.size());
    (void)sent;
  }
  impl_->socket.close();
  return Status::success();
}

bool CoordinatorClient::connected() const noexcept { return impl_->connected; }
CoordinatorEpoch CoordinatorClient::epoch() const noexcept { return impl_->epoch; }

Checked<Frame> CoordinatorClient::exchange(MessageType type,
                                           const std::vector<std::uint8_t>& payload) {
  return impl_->exchange(type, payload);
}

Checked<SubmitResponseMessage> CoordinatorClient::submit(const OperationRequest& request,
                                                         std::span<const std::uint8_t> payload,
                                                         bool dispatch_now, bool reserve_first) {
  SubmitRequestMessage message;
  message.request = request;
  message.payload.assign(payload.begin(), payload.end());
  message.dispatch_now = dispatch_now;
  message.reserve_first = reserve_first;
  auto frame = impl_->exchange(MessageType::kSubmitRequest, encode(message));
  if (!frame.ok()) return Checked<SubmitResponseMessage>::bad(frame.status.code, frame.status.message);
  if (frame.value.type != MessageType::kSubmitResponse) {
    ErrorMessage error;
    if (frame.value.type == MessageType::kError && decode(frame.value.payload, error)) {
      return Checked<SubmitResponseMessage>::bad(error.code, error.detail);
    }
    return Checked<SubmitResponseMessage>::bad("client.unexpected_response");
  }
  SubmitResponseMessage response;
  if (!decode(frame.value.payload, response)) {
    return Checked<SubmitResponseMessage>::bad("client.malformed_submit_response");
  }
  return Checked<SubmitResponseMessage>::good(std::move(response));
}

Checked<QueryResponseMessage> CoordinatorClient::query(QueryKind kind, std::uint64_t id) {
  QueryRequestMessage message;
  message.kind = kind;
  message.id = id;
  auto frame = impl_->exchange(MessageType::kQueryRequest, encode(message));
  if (!frame.ok()) return Checked<QueryResponseMessage>::bad(frame.status.code, frame.status.message);
  if (frame.value.type != MessageType::kQueryResponse) {
    return Checked<QueryResponseMessage>::bad("client.unexpected_response");
  }
  QueryResponseMessage response;
  if (!decode(frame.value.payload, response)) {
    return Checked<QueryResponseMessage>::bad("client.malformed_query_response");
  }
  return Checked<QueryResponseMessage>::good(std::move(response));
}

Checked<AdminResponseMessage> CoordinatorClient::admin(AdminAction action, std::uint64_t id,
                                                       std::string reason, std::uint64_t value) {
  AdminRequestMessage message;
  message.action = action;
  message.id = id;
  message.reason = std::move(reason);
  message.value = value;
  auto frame = impl_->exchange(MessageType::kAdminRequest, encode(message));
  if (!frame.ok()) return Checked<AdminResponseMessage>::bad(frame.status.code, frame.status.message);
  if (frame.value.type != MessageType::kAdminResponse) {
    return Checked<AdminResponseMessage>::bad("client.unexpected_response");
  }
  AdminResponseMessage response;
  if (!decode(frame.value.payload, response)) {
    return Checked<AdminResponseMessage>::bad("client.malformed_admin_response");
  }
  return Checked<AdminResponseMessage>::good(std::move(response));
}

}  // namespace dist
}  // namespace tos