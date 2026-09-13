// Portable TCP sockets with explicit ownership and no timeouts.
//
// Liveness is achieved by shutting a socket down from the controlling thread,
// which unblocks any reader. Nothing in the runtime relies on a watchdog.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#ifndef TOS_UTIL_NET_HPP
#define TOS_UTIL_NET_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "tos/util/status.hpp"

namespace tos {

/// Process-wide socket subsystem lifetime (Winsock startup on Windows).
class SocketRuntime {
 public:
  SocketRuntime();
  ~SocketRuntime();
  SocketRuntime(const SocketRuntime&) = delete;
  SocketRuntime& operator=(const SocketRuntime&) = delete;
  [[nodiscard]] bool valid() const noexcept;
};

class TcpSocket {
 public:
  TcpSocket() = default;
  ~TcpSocket();
  TcpSocket(TcpSocket&& other) noexcept;
  TcpSocket& operator=(TcpSocket&& other) noexcept;
  TcpSocket(const TcpSocket&) = delete;
  TcpSocket& operator=(const TcpSocket&) = delete;

  [[nodiscard]] bool valid() const noexcept;
  void close() noexcept;
  /// Half-close the connection. A blocked reader on another thread returns.
  void shutdown_both() noexcept;

  [[nodiscard]] Status send_all(const void* data, std::size_t length) noexcept;
  [[nodiscard]] Checked<std::size_t> recv_some(void* buffer, std::size_t capacity) noexcept;
  /// Read exactly length bytes. Returns "net.closed" when the peer closed first.
  [[nodiscard]] Status recv_exact(void* buffer, std::size_t length) noexcept;

  void set_nodelay(bool enabled) noexcept;
  [[nodiscard]] std::string peer_text() const;

  [[nodiscard]] std::uintptr_t native_handle() const noexcept;

 private:
  friend class TcpListener;
  friend Checked<TcpSocket> connect_tcp(const std::string&, std::uint16_t);
  explicit TcpSocket(std::uintptr_t handle) noexcept : handle_(handle) {}
  std::uintptr_t handle_{static_cast<std::uintptr_t>(~static_cast<std::uintptr_t>(0))};
};

class TcpListener {
 public:
  TcpListener() = default;
  ~TcpListener();
  TcpListener(TcpListener&& other) noexcept;
  TcpListener& operator=(TcpListener&& other) noexcept;
  TcpListener(const TcpListener&) = delete;
  TcpListener& operator=(const TcpListener&) = delete;

  /// Bind and listen. Port 0 selects an ephemeral port, readable via port().
  [[nodiscard]] Status listen_on(const std::string& host, std::uint16_t port, int backlog = 64);
  [[nodiscard]] Checked<TcpSocket> accept_one() noexcept;
  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] bool valid() const noexcept;
  /// Close the listener. A blocked accept() returns.
  void close() noexcept;

 private:
  std::uintptr_t handle_{static_cast<std::uintptr_t>(~static_cast<std::uintptr_t>(0))};
  std::uint16_t port_{0};
};

/// Connect with an explicit connect attempt count. Each attempt is a separate
/// connect() call; there is no sleep-based retry loop.
[[nodiscard]] Checked<TcpSocket> connect_tcp(const std::string& host, std::uint16_t port);
[[nodiscard]] Checked<TcpSocket> connect_tcp_retry(const std::string& host, std::uint16_t port,
                                                   int attempts);

}  // namespace tos

#endif  // TOS_UTIL_NET_HPP
