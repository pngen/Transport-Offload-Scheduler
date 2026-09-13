// Framed, versioned, integrity-checked protocol.
//
// Every frame is bounded and every decoder validates before use: wrong magic,
// unsupported version, unknown type, oversized length, truncation and bad integrity
// are all rejected. Nothing in the runtime trusts a declared size.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#ifndef TOS_DIST_PROTOCOL_HPP
#define TOS_DIST_PROTOCOL_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "tos/core/attempt.hpp"
#include "tos/core/dispatch.hpp"
#include "tos/core/domain.hpp"
#include "tos/core/operation.hpp"
#include "tos/util/bytes.hpp"
#include "tos/util/status.hpp"

namespace tos {
namespace dist {

inline constexpr std::uint32_t kFrameMagic = 0x31534F54U;  // "TOS1" little endian
/// Frame header layout, little endian:
///   magic u32 | version u16 | type u16 | flags u16 | reserved u16 | length u32 | crc32c u32
inline constexpr std::size_t kFrameHeaderBytes = 20;
inline constexpr std::size_t kMinFrameBytes = kFrameHeaderBytes;
inline constexpr std::size_t kMaxFrameBytes = 1024 * 1024;
inline constexpr std::size_t kMaxProtocolPayloadBytes = 512 * 1024;

enum class MessageType : std::uint16_t {
  kHello = 1,
  kHelloAck = 2,
  kError = 3,
  kGoodbye = 4,
  kDomainPublish = 10,
  kCapabilityPublish = 11,
  kLoadUpdate = 12,
  kHeartbeat = 13,
  kDispatchRequest = 20,
  kDispatchAccepted = 21,
  kDispatchRejected = 22,
  kCompletion = 23,
  kFailureReport = 24,
  kCancelRequest = 25,
  kCancelResult = 26,
  kCompletionAck = 27,
  kSubmitRequest = 30,
  kSubmitResponse = 31,
  kQueryRequest = 40,
  kQueryResponse = 41,
  kAdminRequest = 50,
  kAdminResponse = 51,
  kShutdownRequest = 60,
  kFence = 61,
};
inline constexpr std::uint16_t kMaxMessageType = 61;

enum class PeerRole : std::uint8_t { kWorker = 1, kClient = 2, kCoordinator = 3 };

enum class QueryKind : std::uint8_t {
  kSnapshot = 0,
  kReconcile = 1,
  kVersion = 2,
  kAccounting = 3,
  kAttempt = 4,
  kOperation = 5,
};

enum class AdminAction : std::uint8_t {
  kFenceDomain = 0,
  kFenceBoot = 1,
  kReconcile = 2,
  kSaveState = 3,
  kSetOffloadRequirement = 4,
  kShutdownCoordinator = 5,
};

/// Structural description of a frame once its header has been validated.
struct FrameHeader {
  MessageType type{MessageType::kError};
  std::uint16_t flags{0};
  std::uint32_t length{0};
};

enum class DecodeError : std::uint8_t {
  kOk = 0,
  kEmpty = 1,
  kTruncatedHeader = 2,
  kBadMagic = 3,
  kBadVersion = 4,
  kUnknownType = 5,
  kOversized = 6,
  kLengthMismatch = 7,
  kIntegrityFailure = 8,
  kMalformedPayload = 9,
  kUnsupportedFlags = 10,
};

[[nodiscard]] std::string_view to_string(DecodeError value) noexcept;

/// Validate a frame header that has already been read from a stream.
[[nodiscard]] DecodeError decode_header(const std::uint8_t* header, std::size_t available,
                                        std::size_t max_frame, FrameHeader& out) noexcept;

/// Encode a frame. Payloads larger than the bound are refused rather than truncated.
[[nodiscard]] Checked<std::vector<std::uint8_t>> encode_frame(MessageType type,
                                                              const std::vector<std::uint8_t>& payload,
                                                              std::size_t max_frame = kMaxFrameBytes);

struct Frame {
  MessageType type{MessageType::kError};
  std::uint16_t flags{0};
  std::vector<std::uint8_t> payload;
};

/// Fully validate and decode one complete frame.
[[nodiscard]] Checked<Frame> decode_frame(const std::uint8_t* data, std::size_t size,
                                          std::size_t max_frame = kMaxFrameBytes);

// ---- messages --------------------------------------------------------------

struct HelloMessage {
  PeerRole role{PeerRole::kWorker};
  std::string name;
  WorkerId worker;
  WorkerBootId boot;
  std::string host;
  std::uint64_t pid{0};
  std::string version;
};

struct HelloAckMessage {
  bool accepted{false};
  CoordinatorEpoch epoch;
  std::string code;
  std::string detail;
};

struct ErrorMessage {
  std::string code;
  std::string detail;
};

struct DomainPublishMessage {
  ExecutionDomainRecord record;
};

struct CapabilityPublishMessage {
  CapabilityRecord capability;
};

struct LoadUpdateMessage {
  ExecutionDomainId domain;
  DomainLoadEvidence load;
};

struct HeartbeatMessage {
  WorkerId worker;
  WorkerBootId boot;
  CoordinatorEpoch epoch;
  std::uint64_t sequence{0};
};

struct DispatchRequestMessage {
  DispatchEnvelope envelope;
};

struct DispatchAcceptedMessage {
  ExecutionAttemptId attempt;
  ExecutionAttemptGeneration generation;
  DispatchId dispatch;
  WorkerBootId boot;
};

struct DispatchRejectedMessage {
  ExecutionAttemptId attempt;
  DispatchId dispatch;
  std::string code;
  std::string detail;
};

struct CompletionMessage {
  CompletionSubmission submission;
};

struct FailureReportMessage {
  ExecutionAttemptId attempt;
  ExecutionAttemptGeneration generation;
  DispatchId dispatch;
  WorkerBootId boot;
  CoordinatorEpoch epoch;
  FailureKind kind{FailureKind::kNone};
  std::string detail;
};

struct CompletionAckMessage {
  ExecutionAttemptId attempt;
  CompletionRejection rejection{CompletionRejection::kAccepted};
  bool committed{false};
  bool idempotent{false};
};

struct CancelRequestMessage {
  CancelEnvelope envelope;
};

struct CancelResultMessage {
  ExecutionAttemptId attempt;
  bool cancelled{false};
  bool may_have_executed{false};
  std::string detail;
};

struct SubmitRequestMessage {
  OperationRequest request;
  std::vector<std::uint8_t> payload;
  bool dispatch_now{true};
  bool reserve_first{true};
};

struct SubmitResponseMessage {
  bool planned{false};
  bool dispatched{false};
  SelectionOutcome outcome{SelectionOutcome::kNoEligibleDomain};
  std::uint64_t operation{0};
  std::uint64_t domain{0};
  ExecutionDomainType domain_type{ExecutionDomainType::kCpu};
  Provenance provenance{Provenance::kUnsupported};
  std::uint64_t attempt{0};
  ExecutionAttemptGeneration attempt_generation;
  AttemptState attempt_state{AttemptState::kPlanned};
  DispatchRejection dispatch_rejection{DispatchRejection::kNone};
  std::string code;
  std::string detail;
  std::string explanation_json;
};

struct QueryRequestMessage {
  QueryKind kind{QueryKind::kSnapshot};
  std::uint64_t id{0};
};

struct QueryResponseMessage {
  bool ok{false};
  std::string code;
  std::string body;
};

struct AdminRequestMessage {
  AdminAction action{AdminAction::kReconcile};
  std::uint64_t id{0};
  std::string reason;
  std::uint64_t value{0};
};

struct AdminResponseMessage {
  bool ok{false};
  std::string code;
  std::string detail;
  std::string body;
};

struct ShutdownRequestMessage {
  std::string reason;
};

struct FenceMessage {
  ExecutionDomainId domain;
  WorkerBootId boot;
  std::string reason;
};

struct GoodbyeMessage {
  std::string reason;
};

// ---- codecs ----------------------------------------------------------------

[[nodiscard]] std::vector<std::uint8_t> encode(const HelloMessage& message);
[[nodiscard]] bool decode(const std::vector<std::uint8_t>& payload, HelloMessage& message);
[[nodiscard]] std::vector<std::uint8_t> encode(const HelloAckMessage& message);
[[nodiscard]] bool decode(const std::vector<std::uint8_t>& payload, HelloAckMessage& message);
[[nodiscard]] std::vector<std::uint8_t> encode(const ErrorMessage& message);
[[nodiscard]] bool decode(const std::vector<std::uint8_t>& payload, ErrorMessage& message);
[[nodiscard]] std::vector<std::uint8_t> encode(const DomainPublishMessage& message);
[[nodiscard]] bool decode(const std::vector<std::uint8_t>& payload, DomainPublishMessage& message);
[[nodiscard]] std::vector<std::uint8_t> encode(const CapabilityPublishMessage& message);
[[nodiscard]] bool decode(const std::vector<std::uint8_t>& payload, CapabilityPublishMessage& message);
[[nodiscard]] std::vector<std::uint8_t> encode(const LoadUpdateMessage& message);
[[nodiscard]] bool decode(const std::vector<std::uint8_t>& payload, LoadUpdateMessage& message);
[[nodiscard]] std::vector<std::uint8_t> encode(const HeartbeatMessage& message);
[[nodiscard]] bool decode(const std::vector<std::uint8_t>& payload, HeartbeatMessage& message);
[[nodiscard]] std::vector<std::uint8_t> encode(const DispatchRequestMessage& message);
[[nodiscard]] bool decode(const std::vector<std::uint8_t>& payload, DispatchRequestMessage& message);
[[nodiscard]] std::vector<std::uint8_t> encode(const DispatchAcceptedMessage& message);
[[nodiscard]] bool decode(const std::vector<std::uint8_t>& payload, DispatchAcceptedMessage& message);
[[nodiscard]] std::vector<std::uint8_t> encode(const DispatchRejectedMessage& message);
[[nodiscard]] bool decode(const std::vector<std::uint8_t>& payload, DispatchRejectedMessage& message);
[[nodiscard]] std::vector<std::uint8_t> encode(const CompletionMessage& message);
[[nodiscard]] bool decode(const std::vector<std::uint8_t>& payload, CompletionMessage& message);
[[nodiscard]] std::vector<std::uint8_t> encode(const FailureReportMessage& message);
[[nodiscard]] bool decode(const std::vector<std::uint8_t>& payload, FailureReportMessage& message);
[[nodiscard]] std::vector<std::uint8_t> encode(const CompletionAckMessage& message);
[[nodiscard]] bool decode(const std::vector<std::uint8_t>& payload, CompletionAckMessage& message);
[[nodiscard]] std::vector<std::uint8_t> encode(const CancelRequestMessage& message);
[[nodiscard]] bool decode(const std::vector<std::uint8_t>& payload, CancelRequestMessage& message);
[[nodiscard]] std::vector<std::uint8_t> encode(const CancelResultMessage& message);
[[nodiscard]] bool decode(const std::vector<std::uint8_t>& payload, CancelResultMessage& message);
[[nodiscard]] std::vector<std::uint8_t> encode(const SubmitRequestMessage& message);
[[nodiscard]] bool decode(const std::vector<std::uint8_t>& payload, SubmitRequestMessage& message);
[[nodiscard]] std::vector<std::uint8_t> encode(const SubmitResponseMessage& message);
[[nodiscard]] bool decode(const std::vector<std::uint8_t>& payload, SubmitResponseMessage& message);
[[nodiscard]] std::vector<std::uint8_t> encode(const QueryRequestMessage& message);
[[nodiscard]] bool decode(const std::vector<std::uint8_t>& payload, QueryRequestMessage& message);
[[nodiscard]] std::vector<std::uint8_t> encode(const QueryResponseMessage& message);
[[nodiscard]] bool decode(const std::vector<std::uint8_t>& payload, QueryResponseMessage& message);
[[nodiscard]] std::vector<std::uint8_t> encode(const AdminRequestMessage& message);
[[nodiscard]] bool decode(const std::vector<std::uint8_t>& payload, AdminRequestMessage& message);
[[nodiscard]] std::vector<std::uint8_t> encode(const AdminResponseMessage& message);
[[nodiscard]] bool decode(const std::vector<std::uint8_t>& payload, AdminResponseMessage& message);
[[nodiscard]] std::vector<std::uint8_t> encode(const ShutdownRequestMessage& message);
[[nodiscard]] bool decode(const std::vector<std::uint8_t>& payload, ShutdownRequestMessage& message);
[[nodiscard]] std::vector<std::uint8_t> encode(const FenceMessage& message);
[[nodiscard]] bool decode(const std::vector<std::uint8_t>& payload, FenceMessage& message);
[[nodiscard]] std::vector<std::uint8_t> encode(const GoodbyeMessage& message);
[[nodiscard]] bool decode(const std::vector<std::uint8_t>& payload, GoodbyeMessage& message);

/// Encode any message as a frame in one step.
template <class Message>
[[nodiscard]] Checked<std::vector<std::uint8_t>> frame_of(MessageType type, const Message& message,
                                                          std::size_t max_frame = kMaxFrameBytes) {
  return encode_frame(type, encode(message), max_frame);
}

}  // namespace dist
}  // namespace tos

#endif  // TOS_DIST_PROTOCOL_HPP
