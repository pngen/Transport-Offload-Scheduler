// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "domain_factory.hpp"

#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace tos {
namespace detail {
namespace {

std::string sanitize(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text) {
    const auto uc = static_cast<unsigned char>(c);
    if ((uc >= 'a' && uc <= 'z') || (uc >= 'A' && uc <= 'Z') || (uc >= '0' && uc <= '9') ||
        c == '.' || c == '-' || c == '_') {
      out.push_back(c);
    } else if (c == ' ' || c == ':' || c == '/') {
      out.push_back('.');
    }
  }
  while (!out.empty() && out.back() == '.') out.pop_back();
  if (out.size() > kMaxNameLength) out.resize(kMaxNameLength);
  if (out.empty()) out = "host";
  return out;
}

}  // namespace

std::vector<OperationClassId> host_executor_operations() {
  return {opclass::checksum_crc32c(), opclass::compress_rle(),   opclass::decompress_rle(),
          opclass::copy_stage(),     opclass::segment_split(),  opclass::reassemble_join(),
          opclass::validate_integrity(), opclass::side_effect_emit(), opclass::noop_probe()};
}

CapabilitySet make_host_executor_capability(std::uint64_t max_payload_bytes,
                                            std::uint32_t queue_capacity,
                                            std::uint32_t max_concurrency,
                                            std::string backend_family,
                                            std::uint32_t memory_domain_mask,
                                            std::uint32_t driver_backend_version) {
  CapabilitySet capability;
  capability.operations = host_executor_operations();
  capability.flags = flag_bit(CapabilityFlag::kChecksum) | flag_bit(CapabilityFlag::kCompression) |
                     flag_bit(CapabilityFlag::kDecompression) |
                     flag_bit(CapabilityFlag::kIntegrityVerify) |
                     flag_bit(CapabilityFlag::kSegmentation) | flag_bit(CapabilityFlag::kReassembly) |
                     flag_bit(CapabilityFlag::kStagingCopy) | flag_bit(CapabilityFlag::kSideEffectSink) |
                     flag_bit(CapabilityFlag::kPacketProcessing);
  capability.unproven_flags = flag_bit(CapabilityFlag::kEncryption) |
                              flag_bit(CapabilityFlag::kDecryption) | flag_bit(CapabilityFlag::kRdma) |
                              flag_bit(CapabilityFlag::kGpudirectAddressable) |
                              flag_bit(CapabilityFlag::kDma) | flag_bit(CapabilityFlag::kScatterGather) |
                              flag_bit(CapabilityFlag::kHeaderProcessing) |
                              flag_bit(CapabilityFlag::kProtocolTransform);
  capability.memory_domains = memory_domain_mask;
  capability.min_payload_bytes = 0;
  capability.max_payload_bytes = max_payload_bytes;
  capability.alignment_bytes = 8;
  capability.max_concurrency = max_concurrency == 0 ? 1 : max_concurrency;
  capability.queue_capacity = queue_capacity;
  capability.transport_classes = transport_bit(TransportClass::kRawFrames) |
                                 transport_bit(TransportClass::kStreamBytes) |
                                 transport_bit(TransportClass::kDatagramBytes) |
                                 transport_bit(TransportClass::kControlPlane);
  capability.payload_classes = payload_bit(PayloadClass::kOpaqueBytes) |
                               payload_bit(PayloadClass::kStructuredHeader) |
                               payload_bit(PayloadClass::kSegmentedFrames) |
                               payload_bit(PayloadClass::kScatterGatherList);
  capability.protocol_version = 1;
  capability.driver_backend_version = driver_backend_version;
  capability.firmware_generation = 0;
  capability.accelerator_arch = 0;
  capability.backend_family = sanitize(backend_family);
  capability.canonicalize();
  return capability;
}

std::string host_name() {
#ifdef _WIN32
  wchar_t buffer[256] = {0};
  DWORD size = 256;
  if (::GetComputerNameW(buffer, &size)) {
    const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(size), nullptr, 0,
                                           nullptr, nullptr);
    std::string out(static_cast<std::size_t>(bytes), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(size), out.data(), bytes, nullptr,
                          nullptr);
    return sanitize(out);
  }
  return "host";
#else
  char buffer[256] = {0};
  if (::gethostname(buffer, sizeof(buffer) - 1) == 0) return sanitize(buffer);
  return "host";
#endif
}

std::uint32_t host_numa_nodes() {
#ifdef _WIN32
  ULONG highest = 0;
  if (::GetNumaHighestNodeNumber(&highest)) return static_cast<std::uint32_t>(highest) + 1;
  return 1;
#else
  return 1;
#endif
}

std::uint64_t physical_memory_bytes() {
#ifdef _WIN32
  MEMORYSTATUSEX status{};
  status.dwLength = sizeof(status);
  if (::GlobalMemoryStatusEx(&status)) return static_cast<std::uint64_t>(status.ullTotalPhys);
  return 0;
#else
  const long pages = ::sysconf(_SC_PHYS_PAGES);
  const long page_size = ::sysconf(_SC_PAGE_SIZE);
  if (pages > 0 && page_size > 0) {
    return static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(page_size);
  }
  return 0;
#endif
}

}  // namespace detail
}  // namespace tos
