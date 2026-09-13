// Deterministic interleaving hooks.
//
// Random thread timing is not a proof. These hooks let a caller force a specific
// ordering at every authority boundary so that races can be reproduced exactly.
// Hooks are invoked with no internal lock held; an implementation must not assume
// otherwise and must not call back into the scheduler on the owning thread in a
// way that requires a lock the scheduler already holds (it never holds one).
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#ifndef TOS_CORE_HOOKS_HPP
#define TOS_CORE_HOOKS_HPP

#include <cstdint>
#include <string_view>

#include "tos/core/enums.hpp"
#include "tos/core/identities.hpp"
#include "tos/core/policy.hpp"

namespace tos {

enum class HookPoint : std::uint8_t {
  kAfterEligibility = 0,
  kAfterRanking = 1,
  kBeforeReserve = 2,
  kAfterReserve = 3,
  kBeforeRevalidate = 4,
  kAfterRevalidate = 5,
  kBeforeAttemptRegistration = 6,
  kAfterAttemptRegistration = 7,
  kBeforeChannelSend = 8,
  kAfterChannelSend = 9,
  kBeforeCompletionCommit = 10,
  kAfterCompletionCommit = 11,
  kBeforeFallback = 12,
  kAfterFallbackSelection = 13,
  kDuringCancellation = 14,
  kBeforeShutdownFence = 15,
  kAfterShutdownFence = 16,
  kBeforeReservationCommit = 17,
  kAfterReservationCommit = 18,
};
inline constexpr std::size_t kHookPointCount = 19;

[[nodiscard]] std::string_view to_string(HookPoint value) noexcept;
[[nodiscard]] bool parse_enum(std::string_view text, HookPoint& out) noexcept;

struct HookContext {
  HookPoint point{HookPoint::kAfterEligibility};
  TransportOperationId operation;
  ExecutionAttemptId attempt;
  ExecutionDomainId domain;
  ExecutionDomainType domain_type{ExecutionDomainType::kCpu};
  std::uint32_t retry_index{0};
  std::uint32_t fallback_depth{0};
  FailureKind failure{FailureKind::kNone};
};

class IInterleavingHook {
 public:
  virtual ~IInterleavingHook() = default;
  /// Invoked at a named boundary. Must not throw; a throwing hook is a programming error.
  virtual void at(const HookContext& context) = 0;
};

}  // namespace tos

#endif  // TOS_CORE_HOOKS_HPP
