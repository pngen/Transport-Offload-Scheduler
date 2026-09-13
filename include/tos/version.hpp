// Transport Offload Scheduler - version and provenance constants.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#ifndef TOS_VERSION_HPP
#define TOS_VERSION_HPP

#include <cstdint>
#include <string_view>

namespace tos {

inline constexpr int kVersionMajor = 1;
inline constexpr int kVersionMinor = 0;
inline constexpr int kVersionPatch = 0;
inline constexpr std::string_view kVersionString = "1.0.0";
inline constexpr std::string_view kProjectName = "Transport Offload Scheduler";
inline constexpr std::string_view kCopyrightNotice = "Copyright 2026 Summon Software Labs.";

/// Wire protocol version understood by this build. Bumped on incompatible framing changes.
inline constexpr std::uint16_t kProtocolVersion = 1;
/// Persistence format version understood by this build.
inline constexpr std::uint16_t kPersistenceFormatVersion = 1;

}  // namespace tos

#endif  // TOS_VERSION_HPP
