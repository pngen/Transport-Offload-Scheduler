// Real NIC discovery and locality evidence.
//
// This module reports what the operating system can actually tell us about the
// network adapters present on the machine: identity, link speed and operational
// state. It does not claim hardware offload capability, because the runtime has no
// vendor backend that could prove it. Offload capabilities on a discovered NIC are
// therefore published as UNPROVEN, which keeps the domain ineligible whenever
// policy requires positive evidence.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#ifndef TOS_BACKENDS_NIC_PROBE_HPP
#define TOS_BACKENDS_NIC_PROBE_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "tos/backends/backend.hpp"
#include "tos/core/domain.hpp"
#include "tos/core/identities.hpp"

namespace tos {

struct NicInfo {
  std::uint32_t index{0};           ///< interface index
  std::string name;                 ///< stable adapter name produced by the OS
  std::string description;          ///< adapter description
  std::uint64_t link_speed_bps{0};  ///< 0 when the OS does not report a speed
  std::string mac;                  ///< colon-separated, empty when unknown
  bool up{false};
  bool loopback{false};
};

/// Real enumeration of the host network interfaces.
[[nodiscard]] std::vector<NicInfo> enumerate_nics();

/// Build an execution domain record for a discovered NIC. The capability record is
/// deliberately not authoritative: the runtime can prove the adapter exists, but
/// not that it implements transport offload for a given operation class.
[[nodiscard]] ExecutionDomainRecord make_nic_domain(const NicInfo& nic, ExecutionDomainId id,
                                                   std::string parent_host, WorkerId worker,
                                                   WorkerBootId worker_boot,
                                                   std::uint64_t max_payload_bytes = kMaxDispatchPayloadBytes);

[[nodiscard]] std::string describe_nic_inventory(const std::vector<NicInfo>& nics);

}  // namespace tos

#endif  // TOS_BACKENDS_NIC_PROBE_HPP
