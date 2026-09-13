// Coordinator client used by tooling, examples and process-level proofs.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#ifndef TOS_DIST_CLIENT_HPP
#define TOS_DIST_CLIENT_HPP

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "tos/core/operation.hpp"
#include "tos/dist/protocol.hpp"
#include "tos/util/net.hpp"

namespace tos {
namespace dist {

struct ClientConfig {
  std::string host{"127.0.0.1"};
  std::uint16_t port{0};
  std::string name{"client"};
  std::size_t max_frame_bytes{kMaxFrameBytes};
  int connect_attempts{20};
};

class CoordinatorClient {
 public:
  explicit CoordinatorClient(ClientConfig config);
  ~CoordinatorClient();
  CoordinatorClient(const CoordinatorClient&) = delete;
  CoordinatorClient& operator=(const CoordinatorClient&) = delete;

  [[nodiscard]] Status connect_and_hello();
  [[nodiscard]] Status close();
  [[nodiscard]] bool connected() const noexcept;
  [[nodiscard]] CoordinatorEpoch epoch() const noexcept;

  [[nodiscard]] Checked<SubmitResponseMessage> submit(
      const OperationRequest& request, std::span<const std::uint8_t> payload, bool dispatch_now,
      bool reserve_first = true);
  [[nodiscard]] Checked<QueryResponseMessage> query(QueryKind kind, std::uint64_t id = 0);
  [[nodiscard]] Checked<AdminResponseMessage> admin(AdminAction action, std::uint64_t id,
                                                     std::string reason, std::uint64_t value = 0);

  /// Send one request frame and read exactly one response frame.
  [[nodiscard]] Checked<Frame> exchange(MessageType type, const std::vector<std::uint8_t>& payload);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace dist
}  // namespace tos

#endif  // TOS_DIST_CLIENT_HPP
