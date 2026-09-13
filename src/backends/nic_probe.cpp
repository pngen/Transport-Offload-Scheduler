// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "tos/backends/nic_probe.hpp"

#include <array>
#include <cstdio>

#include "domain_factory.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#endif

namespace tos {
namespace {

std::string sanitize_name(std::string_view text) {
  std::string out;
  for (char c : text) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '.' || c == '-' || c == '_';
    out.push_back(ok ? c : '.');
  }
  if (out.size() > kMaxNameLength) out.resize(kMaxNameLength);
  if (out.empty()) out = "nic";
  return out;
}

}  // namespace

std::vector<NicInfo> enumerate_nics() {
  std::vector<NicInfo> nics;
#ifdef _WIN32
  ULONG size = 16 * 1024;
  std::vector<std::uint8_t> buffer(size);
  ULONG result = ::GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr,
                                        reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()),
                                        &size);
  if (result == ERROR_BUFFER_OVERFLOW) {
    buffer.resize(size);
    result = ::GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr,
                                    reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
  }
  if (result != NO_ERROR) return nics;

  const auto* current = reinterpret_cast<const IP_ADAPTER_ADDRESSES*>(buffer.data());
  std::uint32_t index = 0;
  while (current != nullptr && nics.size() < 256) {
    NicInfo info;
    info.index = current->IfIndex != 0 ? current->IfIndex : ++index;
    if (current->FriendlyName != nullptr) {
      const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, current->FriendlyName, -1, nullptr, 0,
                                             nullptr, nullptr);
      if (bytes > 1) {
        std::string name(static_cast<std::size_t>(bytes - 1), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, current->FriendlyName, -1, name.data(), bytes, nullptr,
                              nullptr);
        info.name = sanitize_name(name);
      }
    }
    if (info.name.empty()) info.name = "nic." + std::to_string(current->IfIndex);
    if (current->Description != nullptr) {
      const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, current->Description, -1, nullptr, 0,
                                             nullptr, nullptr);
      if (bytes > 1) {
        std::string description(static_cast<std::size_t>(bytes - 1), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, current->Description, -1, description.data(), bytes,
                              nullptr, nullptr);
        info.description = bounded_text(description, kMaxNameLength);
      }
    }
    // The OS reports an unreported speed as an all-ones value; publish 0 for unknown.
    info.link_speed_bps = current->TransmitLinkSpeed == 0xFFFFFFFFFFFFFFFFULL
                              ? 0ULL
                              : static_cast<std::uint64_t>(current->TransmitLinkSpeed);
    info.up = current->OperStatus == IfOperStatusUp;
    info.loopback = current->IfType == IF_TYPE_SOFTWARE_LOOPBACK;
    if (current->PhysicalAddressLength == 6) {
      char mac[18] = {0};
      std::snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", current->PhysicalAddress[0],
                    current->PhysicalAddress[1], current->PhysicalAddress[2],
                    current->PhysicalAddress[3], current->PhysicalAddress[4],
                    current->PhysicalAddress[5]);
      info.mac = mac;
    }
    if (current->IfType != IF_TYPE_SOFTWARE_LOOPBACK) nics.push_back(std::move(info));
    current = current->Next;
    ++index;
  }
#endif
  return nics;
}

ExecutionDomainRecord make_nic_domain(const NicInfo& nic, ExecutionDomainId id,
                                      std::string parent_host, WorkerId worker,
                                      WorkerBootId worker_boot, std::uint64_t max_payload_bytes) {
  ExecutionDomainRecord record;
  record.id = id;
  record.generation = ExecutionDomainGeneration::first();
  record.type = ExecutionDomainType::kNic;
  record.name = "nic." + nic.name;
  if (record.name.size() > kMaxNameLength) record.name.resize(kMaxNameLength);
  record.parent_device = nic.description.empty() ? nic.name : nic.description;
  record.parent_host = parent_host.empty() ? detail::host_name() : std::move(parent_host);
  record.worker = worker;
  record.worker_boot = worker_boot;
  record.provenance = Provenance::kReal;  // the adapter is real and was discovered
  record.registration_sequence = id.value();
  record.capability.domain = id;
  record.capability.generation = CapabilityGeneration::first();
  record.capability.evidence = EvidenceGeneration::first();
  record.capability.provenance = Provenance::kReal;
  // Capability is deliberately not authoritative: discovery proves the adapter
  // exists, not that it offloads a given operation class.
  record.capability.authoritative = false;
  record.capability.capability.operations.clear();
  record.capability.capability.flags = 0;
  record.capability.capability.unproven_flags =
      flag_bit(CapabilityFlag::kChecksum) | flag_bit(CapabilityFlag::kCompression) |
      flag_bit(CapabilityFlag::kDecompression) | flag_bit(CapabilityFlag::kEncryption) |
      flag_bit(CapabilityFlag::kDecryption) | flag_bit(CapabilityFlag::kDma) |
      flag_bit(CapabilityFlag::kScatterGather) | flag_bit(CapabilityFlag::kRdma) |
      flag_bit(CapabilityFlag::kGpudirectAddressable) | flag_bit(CapabilityFlag::kPacketProcessing) |
      flag_bit(CapabilityFlag::kHeaderProcessing) | flag_bit(CapabilityFlag::kProtocolTransform) |
      flag_bit(CapabilityFlag::kIntegrityVerify) | flag_bit(CapabilityFlag::kSegmentation) |
      flag_bit(CapabilityFlag::kReassembly);
  record.capability.capability.memory_domains = memory_bit(MemoryDomain::kHost);
  record.capability.capability.min_payload_bytes = 0;
  record.capability.capability.max_payload_bytes = max_payload_bytes;
  record.capability.capability.alignment_bytes = 64;
  record.capability.capability.max_concurrency = 1;
  record.capability.capability.queue_capacity = 0;
  record.capability.capability.transport_classes =
      transport_bit(TransportClass::kRawFrames) | transport_bit(TransportClass::kStreamBytes);
  record.capability.capability.payload_classes = payload_bit(PayloadClass::kOpaqueBytes);
  record.capability.capability.protocol_version = 1;
  record.capability.capability.backend_family = "nic.discovery";
  record.capability.capability.canonicalize();
  record.load.load_generation = LoadGeneration::first();
  record.load.queue_generation = QueueGeneration::first();
  record.load.health_generation = HealthGeneration::first();
  record.load.evidence = EvidenceGeneration::first();
  record.load.provenance = Provenance::kReal;
  record.load.healthy = nic.up;
  record.load.ready = nic.up;
  record.load.accepting = nic.up;
  record.locality.generation = LocalityGeneration::first();
  record.locality.class_to_payload = LocalityClass::kSameHost;
  record.locality.host_index = nic.index;
  record.locality.numa_node = 0;
  record.locality.pcie_root_complex = 0;
  record.locality.pcie_switch = 0;
  record.locality.has_local_nic = true;
  record.topology.generation = TopologyGeneration::first();
  record.topology.host_node = record.parent_host;
  record.compatibility.generation = CompatibilityGeneration::first();
  record.compatibility.driver_backend_version = 1;
  record.compatibility.protocol_version = 1;
  record.compatibility.backend_family = "nic.discovery";
  record.isolation = IsolationClass::kUnknown;
  record.capacity[static_cast<std::size_t>(ResourceKind::kExecutionSlot)] = 0;
  record.backend_generation = BackendGeneration::first();
  return record;
}

std::string describe_nic_inventory(const std::vector<NicInfo>& nics) {
  std::string out;
  for (const NicInfo& nic : nics) {
    out += "  nic." + nic.name + " speed=" + std::to_string(nic.link_speed_bps) +
           "bps up=" + (nic.up ? "yes" : "no") + " mac=" + (nic.mac.empty() ? "unknown" : nic.mac) +
           " desc=" + nic.description + "\n";
  }
  if (out.empty()) out = "  no network adapters discovered\n";
  return out;
}

}  // namespace tos
