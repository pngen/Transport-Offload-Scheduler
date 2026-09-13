// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "tos/util/net.hpp"

#include <atomic>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace tos {
namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;
#endif

std::atomic<int> g_socket_users{0};
std::mutex g_socket_mutex;

NativeSocket to_native(std::uintptr_t handle) noexcept {
  return static_cast<NativeSocket>(handle);
}

std::uintptr_t from_native(NativeSocket socket) noexcept {
  return static_cast<std::uintptr_t>(socket);
}

constexpr std::uintptr_t kInvalidHandle = static_cast<std::uintptr_t>(~static_cast<std::uintptr_t>(0));

void close_native(NativeSocket socket) noexcept {
#ifdef _WIN32
  ::closesocket(socket);
#else
  ::close(socket);
#endif
}

bool would_block() noexcept {
#ifdef _WIN32
  return ::WSAGetLastError() == WSAEWOULDBLOCK;
#else
  return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

std::string last_socket_error() {
#ifdef _WIN32
  return "wsa:" + std::to_string(::WSAGetLastError());
#else
  return std::string(std::strerror(errno));
#endif
}

}  // namespace

SocketRuntime::SocketRuntime() {
  std::lock_guard<std::mutex> lock(g_socket_mutex);
  if (g_socket_users.fetch_add(1) == 0) {
#ifdef _WIN32
    WSADATA data{};
    if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) {
      g_socket_users.fetch_sub(1);
    }
#endif
  }
}

SocketRuntime::~SocketRuntime() {
  std::lock_guard<std::mutex> lock(g_socket_mutex);
  if (g_socket_users.fetch_sub(1) == 1) {
#ifdef _WIN32
    ::WSACleanup();
#endif
  }
}

bool SocketRuntime::valid() const noexcept { return g_socket_users.load() > 0; }

TcpSocket::TcpSocket(TcpSocket&& other) noexcept : handle_(other.handle_) {
  other.handle_ = kInvalidHandle;
}

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    other.handle_ = kInvalidHandle;
  }
  return *this;
}

TcpSocket::~TcpSocket() { close(); }

bool TcpSocket::valid() const noexcept {
  return handle_ != kInvalidHandle && handle_ != from_native(kInvalidSocket);
}

void TcpSocket::close() noexcept {
  if (valid()) {
    close_native(to_native(handle_));
    handle_ = kInvalidHandle;
  }
}

void TcpSocket::shutdown_both() noexcept {
  if (valid()) {
#ifdef _WIN32
    ::shutdown(to_native(handle_), SD_BOTH);
#else
    ::shutdown(to_native(handle_), SHUT_RDWR);
#endif
  }
}

Status TcpSocket::send_all(const void* data, std::size_t length) noexcept {
  if (!valid()) return Status::failure("net.invalid_socket");
  const auto* bytes = static_cast<const char*>(data);
  std::size_t sent = 0;
  while (sent < length) {
    const std::size_t chunk = length - sent > 1U << 20 ? 1U << 20 : length - sent;
    const int result = ::send(to_native(handle_), bytes + sent, static_cast<int>(chunk), 0);
    if (result <= 0) return Status::failure("net.send_failed", last_socket_error());
    sent += static_cast<std::size_t>(result);
  }
  return Status::success();
}

Checked<std::size_t> TcpSocket::recv_some(void* buffer, std::size_t capacity) noexcept {
  if (!valid()) return Checked<std::size_t>::bad("net.invalid_socket");
  const int result = ::recv(to_native(handle_), static_cast<char*>(buffer),
                            static_cast<int>(capacity), 0);
  if (result < 0) {
    if (would_block()) return Checked<std::size_t>::bad("net.would_block");
    return Checked<std::size_t>::bad("net.recv_failed", last_socket_error());
  }
  return Checked<std::size_t>::good(static_cast<std::size_t>(result));
}

Status TcpSocket::recv_exact(void* buffer, std::size_t length) noexcept {
  auto* bytes = static_cast<char*>(buffer);
  std::size_t received = 0;
  while (received < length) {
    const int result = ::recv(to_native(handle_), bytes + received,
                              static_cast<int>(length - received), 0);
    if (result == 0) return Status::failure("net.closed");
    if (result < 0) {
      if (would_block()) continue;
      return Status::failure("net.recv_failed", last_socket_error());
    }
    received += static_cast<std::size_t>(result);
  }
  return Status::success();
}

void TcpSocket::set_nodelay(bool enabled) noexcept {
  if (!valid()) return;
  const int value = enabled ? 1 : 0;
  ::setsockopt(to_native(handle_), IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char*>(&value), sizeof(value));
}

std::string TcpSocket::peer_text() const {
  if (!valid()) return "none";
  sockaddr_storage storage{};
  socklen_t length = sizeof(storage);
  if (::getpeername(to_native(handle_), reinterpret_cast<sockaddr*>(&storage), &length) != 0) {
    return "unknown";
  }
  char host[64] = {0};
  char service[16] = {0};
  if (::getnameinfo(reinterpret_cast<sockaddr*>(&storage), length, host, sizeof(host), service,
                    sizeof(service), NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
    return "unknown";
  }
  return std::string(host) + ":" + service;
}

std::uintptr_t TcpSocket::native_handle() const noexcept { return handle_; }

TcpListener::TcpListener(TcpListener&& other) noexcept
    : handle_(other.handle_), port_(other.port_) {
  other.handle_ = kInvalidHandle;
  other.port_ = 0;
}

TcpListener& TcpListener::operator=(TcpListener&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    port_ = other.port_;
    other.handle_ = kInvalidHandle;
    other.port_ = 0;
  }
  return *this;
}

TcpListener::~TcpListener() { close(); }

bool TcpListener::valid() const noexcept {
  return handle_ != kInvalidHandle && handle_ != from_native(kInvalidSocket);
}

void TcpListener::close() noexcept {
  if (valid()) {
    close_native(to_native(handle_));
    handle_ = kInvalidHandle;
  }
}

Status TcpListener::listen_on(const std::string& host, std::uint16_t port, int backlog) {
  close();
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = AI_PASSIVE;
  addrinfo* results = nullptr;
  const std::string service = std::to_string(port);
  const int resolved = ::getaddrinfo(host.empty() ? nullptr : host.c_str(), service.c_str(), &hints,
                                     &results);
  if (resolved != 0 || results == nullptr) {
    return Status::failure("net.resolve_failed", host + ":" + service);
  }
  NativeSocket socket = ::socket(results->ai_family, results->ai_socktype, results->ai_protocol);
  if (socket == kInvalidSocket) {
    ::freeaddrinfo(results);
    return Status::failure("net.socket_failed", last_socket_error());
  }
  const int reuse = 1;
  ::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
               sizeof(reuse));
  if (::bind(socket, results->ai_addr, static_cast<int>(results->ai_addrlen)) != 0) {
    ::freeaddrinfo(results);
    close_native(socket);
    return Status::failure("net.bind_failed", host + ":" + service + " " + last_socket_error());
  }
  ::freeaddrinfo(results);
  if (::listen(socket, backlog) != 0) {
    close_native(socket);
    return Status::failure("net.listen_failed", last_socket_error());
  }
  sockaddr_in bound{};
  socklen_t bound_length = sizeof(bound);
  if (::getsockname(socket, reinterpret_cast<sockaddr*>(&bound), &bound_length) == 0) {
    port_ = ntohs(bound.sin_port);
  } else {
    port_ = port;
  }
  handle_ = from_native(socket);
  return Status::success();
}

Checked<TcpSocket> TcpListener::accept_one() noexcept {
  if (!valid()) return Checked<TcpSocket>::bad("net.listener_closed");
  sockaddr_storage storage{};
  socklen_t length = sizeof(storage);
  const NativeSocket accepted =
      ::accept(to_native(handle_), reinterpret_cast<sockaddr*>(&storage), &length);
  if (accepted == kInvalidSocket) {
    return Checked<TcpSocket>::bad("net.accept_failed", last_socket_error());
  }
  return Checked<TcpSocket>::good(TcpSocket(from_native(accepted)));
}

Checked<TcpSocket> connect_tcp(const std::string& host, std::uint16_t port) {
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* results = nullptr;
  const std::string service = std::to_string(port);
  if (::getaddrinfo(host.c_str(), service.c_str(), &hints, &results) != 0 || results == nullptr) {
    return Checked<TcpSocket>::bad("net.resolve_failed", host + ":" + service);
  }
  NativeSocket socket = ::socket(results->ai_family, results->ai_socktype, results->ai_protocol);
  if (socket == kInvalidSocket) {
    ::freeaddrinfo(results);
    return Checked<TcpSocket>::bad("net.socket_failed", last_socket_error());
  }
  if (::connect(socket, results->ai_addr, static_cast<int>(results->ai_addrlen)) != 0) {
    const std::string detail = last_socket_error();
    ::freeaddrinfo(results);
    close_native(socket);
    return Checked<TcpSocket>::bad("net.connect_failed", host + ":" + service + " " + detail);
  }
  ::freeaddrinfo(results);
  TcpSocket wrapper(from_native(socket));
  wrapper.set_nodelay(true);
  return Checked<TcpSocket>::good(std::move(wrapper));
}

Checked<TcpSocket> connect_tcp_retry(const std::string& host, std::uint16_t port, int attempts) {
  Status last = Status::failure("net.connect_failed");
  const int count = attempts < 1 ? 1 : attempts;
  for (int i = 0; i < count; ++i) {
    auto result = connect_tcp(host, port);
    if (result.ok()) return result;
    last = result.status;
  }
  return Checked<TcpSocket>::bad(last.code, last.message);
}

}  // namespace tos
