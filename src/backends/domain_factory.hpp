// Internal helpers shared by the built-in backends.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#ifndef TOS_SRC_BACKENDS_DOMAIN_FACTORY_HPP
#define TOS_SRC_BACKENDS_DOMAIN_FACTORY_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "tos/core/capability.hpp"
#include "tos/core/domain.hpp"
#include "tos/core/operation.hpp"

namespace tos {
namespace detail {

/// Operation classes the in-process host executor actually implements. The list is
/// derived from the executor, never from a vendor claim.
[[nodiscard]] std::vector<OperationClassId> host_executor_operations();

[[nodiscard]] CapabilitySet make_host_executor_capability(std::uint64_t max_payload_bytes,
                                                          std::uint32_t queue_capacity,
                                                          std::uint32_t max_concurrency,
                                                          std::string backend_family,
                                                          std::uint32_t memory_domain_mask,
                                                          std::uint32_t driver_backend_version);

/// Stable, bounded host identity of the running machine.
[[nodiscard]] std::string host_name();
/// Number of NUMA nodes reported by the operating system, at least 1.
[[nodiscard]] std::uint32_t host_numa_nodes();
/// Physical memory of the machine in bytes, 0 when unknown.
[[nodiscard]] std::uint64_t physical_memory_bytes();

}  // namespace detail
}  // namespace tos

#endif  // TOS_SRC_BACKENDS_DOMAIN_FACTORY_HPP
