// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "tos/dist/protocol.hpp"

#include <algorithm>
#include <cstring>

#include "persist/codec.hpp"
#include "tos/util/crc32c.hpp"
#include "tos/version.hpp"

namespace tos {
namespace dist {
namespace {

using codec::read_attempt;
using codec::read_capability;
using codec::read_domain;
using codec::write_attempt;
using codec::write_capability;
using codec::write_domain;

void put_u64(ByteWriter& writer, std::uint64_t value) { writer.u64(value); }

bool take_u64(ByteReader& reader, std::uint64_t& value) { return reader.u64(value); }

template <class IdType>
void put_id(ByteWriter& writer, IdType id) {
  writer.u64(id.value());
}

template <class IdType>
bool take_id(ByteReader& reader, IdType& id) {
  std::uint64_t value = 0;
  if (!reader.u64(value)) return false;
  id = IdType(value);
  return true;
}

template <class GenerationType>
void put_generation(ByteWriter& writer, GenerationType generation) {
  writer.u64(generation.value());
}

template <class GenerationType>
bool take_generation(ByteReader& reader, GenerationType& generation) {
  std::uint64_t value = 0;
  if (!reader.u64(value)) return false;
  generation = GenerationType(value);
  return true;
}

template <class EnumType>
void put_enum(ByteWriter& writer, EnumType value, std::uint8_t /*max*/) {
  writer.u8(static_cast<std::uint8_t>(value));
}

template <class EnumType>
bool take_enum(ByteReader& reader, EnumType& value, std::uint8_t max_value) {
  std::uint8_t raw = 0;
  if (!reader.u8(raw)) return false;
  if (raw > max_value) return reader.reject("protocol.enum_out_of_range");
  value = static_cast<EnumType>(raw);
  return true;
}

void put_payload_descriptor(ByteWriter& writer, const PayloadDescriptor& payload) {
  writer.u64(payload.size_bytes);
  put_enum(writer, payload.source_memory, 0);
  put_enum(writer, payload.destination_memory, 0);
  writer.u64(payload.source_handle);
  writer.u64(payload.destination_handle);
  writer.u32(payload.alignment_bytes);
  writer.u32(payload.segment_count);
  put_enum(writer, payload.payload_class, 0);
  put_enum(writer, payload.transport_class, 0);
  writer.u8(payload.scatter_gather ? 1 : 0);
}

bool take_payload_descriptor(ByteReader& reader, PayloadDescriptor& payload) {
  if (!reader.u64(payload.size_bytes)) return false;
  if (!take_enum(reader, payload.source_memory, kMemoryDomainCount - 1)) return false;
  if (!take_enum(reader, payload.destination_memory, kMemoryDomainCount - 1)) return false;
  if (!reader.u64(payload.source_handle)) return false;
  if (!reader.u64(payload.destination_handle)) return false;
  if (!reader.u32(payload.alignment_bytes)) return false;
  if (!reader.u32(payload.segment_count)) return false;
  if (!take_enum(reader, payload.payload_class, 3)) return false;
  if (!take_enum(reader, payload.transport_class, kTransportClassCount - 1)) return false;
  std::uint8_t scatter = 0;
  if (!reader.u8(scatter)) return false;
  payload.scatter_gather = scatter != 0;
  if (payload.alignment_bytes == 0 || payload.segment_count == 0) {
    return reader.reject("protocol.invalid_payload_descriptor");
  }
  return true;
}

void put_operation_request(ByteWriter& writer, const OperationRequest& request) {
  put_id(writer, request.operation_class);
  put_payload_descriptor(writer, request.payload);
  writer.u32(static_cast<std::uint32_t>(request.allowed_domains.size()));
  for (ExecutionDomainType type : request.allowed_domains) put_enum(writer, type, 0);
  writer.u32(static_cast<std::uint32_t>(request.forbidden_domains.size()));
  for (ExecutionDomainType type : request.forbidden_domains) put_enum(writer, type, 0);
  writer.u32(static_cast<std::uint32_t>(request.allowed_domain_ids.size()));
  for (ExecutionDomainId id : request.allowed_domain_ids) put_id(writer, id);
  writer.u32(request.required_capability_flags);
  put_enum(writer, request.required_isolation, 0);
  writer.u8(request.require_dma ? 1 : 0);
  writer.u8(request.require_scatter_gather ? 1 : 0);
  put_enum(writer, request.locality.minimum_class, 0);
  writer.u32(request.locality.required_numa_node);
  writer.u8(request.locality.require_same_host ? 1 : 0);
  writer.u8(request.locality.require_local_nic ? 1 : 0);
  writer.u32(request.compatibility.minimum_driver_backend_version);
  writer.u32(request.compatibility.minimum_firmware_generation);
  writer.u32(request.compatibility.required_protocol_version);
  writer.u32(request.compatibility.required_accelerator_arch);
  writer.text(request.compatibility.required_backend_family);
  writer.u8(request.economics.has_value() ? 1 : 0);
  if (request.economics.has_value()) {
    writer.u64(request.economics->setup_cost_units);
    writer.u64(request.economics->per_byte_cost_micro_units);
    writer.u64(request.economics->maximum_total_cost_units);
    writer.u64(request.economics->estimated_transfer_bytes);
  }
  writer.u8(request.latency_slo_ns.has_value() ? 1 : 0);
  if (request.latency_slo_ns.has_value()) writer.u64(request.latency_slo_ns.value());
  writer.u8(request.retry.allowed ? 1 : 0);
  writer.u32(request.retry.max_attempts);
  writer.u8(request.fallback_allowed ? 1 : 0);
  put_enum(writer, request.side_effect_override, 0);
  writer.u8(request.completion_may_produce_side_effects ? 1 : 0);
  put_id(writer, request.operation_id);
  writer.text(request.correlation_label);
  put_enum(writer, request.provenance_hint, 0);
}

bool take_operation_request(ByteReader& reader, OperationRequest& request) {
  if (!take_id(reader, request.operation_class)) return false;
  if (!take_payload_descriptor(reader, request.payload)) return false;
  std::uint32_t count = 0;
  if (!reader.u32(count) || count > kExecutionDomainTypeCount) return false;
  request.allowed_domains.clear();
  for (std::uint32_t i = 0; i < count; ++i) {
    ExecutionDomainType type{};
    if (!take_enum(reader, type, kExecutionDomainTypeCount - 1)) return false;
    request.allowed_domains.push_back(type);
  }
  if (!reader.u32(count) || count > kExecutionDomainTypeCount) return false;
  request.forbidden_domains.clear();
  for (std::uint32_t i = 0; i < count; ++i) {
    ExecutionDomainType type{};
    if (!take_enum(reader, type, kExecutionDomainTypeCount - 1)) return false;
    request.forbidden_domains.push_back(type);
  }
  if (!reader.u32(count) || count > kMaxExecutionDomains) return false;
  request.allowed_domain_ids.clear();
  for (std::uint32_t i = 0; i < count; ++i) {
    ExecutionDomainId id;
    if (!take_id(reader, id)) return false;
    request.allowed_domain_ids.push_back(id);
  }
  if (!reader.u32(request.required_capability_flags)) return false;
  if (!take_enum(reader, request.required_isolation, static_cast<std::uint8_t>(IsolationClass::kDedicatedDeviceService))) {
    return false;
  }
  std::uint8_t flag = 0;
  if (!reader.u8(flag)) return false;
  request.require_dma = flag != 0;
  if (!reader.u8(flag)) return false;
  request.require_scatter_gather = flag != 0;
  if (!take_enum(reader, request.locality.minimum_class, static_cast<std::uint8_t>(LocalityClass::kDomainLocal))) {
    return false;
  }
  if (!reader.u32(request.locality.required_numa_node)) return false;
  if (!reader.u8(flag)) return false;
  request.locality.require_same_host = flag != 0;
  if (!reader.u8(flag)) return false;
  request.locality.require_local_nic = flag != 0;
  if (!reader.u32(request.compatibility.minimum_driver_backend_version)) return false;
  if (!reader.u32(request.compatibility.minimum_firmware_generation)) return false;
  if (!reader.u32(request.compatibility.required_protocol_version)) return false;
  if (!reader.u32(request.compatibility.required_accelerator_arch)) return false;
  if (!reader.text(request.compatibility.required_backend_family, kMaxNameLength)) return false;
  std::uint8_t has_economics = 0;
  if (!reader.u8(has_economics)) return false;
  if (has_economics != 0) {
    ExecutionEconomics economics;
    if (!reader.u64(economics.setup_cost_units)) return false;
    if (!reader.u64(economics.per_byte_cost_micro_units)) return false;
    if (!reader.u64(economics.maximum_total_cost_units)) return false;
    if (!reader.u64(economics.estimated_transfer_bytes)) return false;
    request.economics = economics;
  } else {
    request.economics.reset();
  }
  std::uint8_t has_slo = 0;
  if (!reader.u8(has_slo)) return false;
  if (has_slo != 0) {
    std::uint64_t slo = 0;
    if (!reader.u64(slo)) return false;
    request.latency_slo_ns = slo;
  } else {
    request.latency_slo_ns.reset();
  }
  if (!reader.u8(flag)) return false;
  request.retry.allowed = flag != 0;
  if (!reader.u32(request.retry.max_attempts)) return false;
  if (!reader.u8(flag)) return false;
  request.fallback_allowed = flag != 0;
  if (!take_enum(reader, request.side_effect_override, static_cast<std::uint8_t>(SideEffectClass::kUnknown))) {
    return false;
  }
  if (!reader.u8(flag)) return false;
  request.completion_may_produce_side_effects = flag != 0;
  if (!take_id(reader, request.operation_id)) return false;
  if (!reader.text(request.correlation_label, kMaxLabelLength)) return false;
  if (!take_enum(reader, request.provenance_hint, static_cast<std::uint8_t>(Provenance::kUnsupported))) {
    return false;
  }
  if (request.retry.max_attempts == 0 || request.retry.max_attempts > kMaxRetryAttempts) {
    return reader.reject("protocol.invalid_retry_bound");
  }
  return true;
}

void put_dispatch_envelope(ByteWriter& writer, const DispatchEnvelope& envelope) {
  put_id(writer, envelope.attempt);
  put_generation(writer, envelope.attempt_generation);
  put_id(writer, envelope.dispatch);
  put_generation(writer, envelope.coordinator_epoch);
  put_id(writer, envelope.worker);
  put_id(writer, envelope.worker_boot);
  put_id(writer, envelope.domain);
  put_generation(writer, envelope.domain_generation);
  put_generation(writer, envelope.capability_generation);
  put_generation(writer, envelope.policy_generation);
  put_id(writer, envelope.operation);
  put_id(writer, envelope.operation_class);
  put_enum(writer, envelope.side_effect, 0);
  put_enum(writer, envelope.provenance, 0);
  put_payload_descriptor(writer, envelope.payload);
  writer.blob(envelope.bytes);
  writer.u8(envelope.fallback ? 1 : 0);
  writer.u32(envelope.fallback_depth);
  writer.u32(envelope.retry_index);
}

bool take_dispatch_envelope(ByteReader& reader, DispatchEnvelope& envelope) {
  if (!take_id(reader, envelope.attempt)) return false;
  if (!take_generation(reader, envelope.attempt_generation)) return false;
  if (!take_id(reader, envelope.dispatch)) return false;
  if (!take_generation(reader, envelope.coordinator_epoch)) return false;
  if (!take_id(reader, envelope.worker)) return false;
  if (!take_id(reader, envelope.worker_boot)) return false;
  if (!take_id(reader, envelope.domain)) return false;
  if (!take_generation(reader, envelope.domain_generation)) return false;
  if (!take_generation(reader, envelope.capability_generation)) return false;
  if (!take_generation(reader, envelope.policy_generation)) return false;
  if (!take_id(reader, envelope.operation)) return false;
  if (!take_id(reader, envelope.operation_class)) return false;
  if (!take_enum(reader, envelope.side_effect, static_cast<std::uint8_t>(SideEffectClass::kUnknown))) {
    return false;
  }
  if (!take_enum(reader, envelope.provenance, static_cast<std::uint8_t>(Provenance::kUnsupported))) {
    return false;
  }
  if (!take_payload_descriptor(reader, envelope.payload)) return false;
  if (!reader.blob(envelope.bytes, kMaxProtocolPayloadBytes)) return false;
  std::uint8_t fallback = 0;
  if (!reader.u8(fallback)) return false;
  envelope.fallback = fallback != 0;
  if (!reader.u32(envelope.fallback_depth)) return false;
  if (!reader.u32(envelope.retry_index)) return false;
  if (!envelope.attempt.valid() || !envelope.operation.valid()) {
    return reader.reject("protocol.invalid_dispatch_identity");
  }
  return true;
}

void put_completion_submission(ByteWriter& writer, const CompletionSubmission& submission) {
  put_id(writer, submission.attempt);
  put_generation(writer, submission.generation);
  put_id(writer, submission.dispatch);
  put_id(writer, submission.completion);
  put_id(writer, submission.worker);
  put_id(writer, submission.worker_boot);
  put_generation(writer, submission.coordinator_epoch);
  put_generation(writer, submission.domain_generation);
  put_generation(writer, submission.capability_generation);
  put_id(writer, submission.operation);
  put_enum(writer, submission.provenance, 0);
  writer.u8(submission.result.success ? 1 : 0);
  writer.u64(submission.result.result_digest);
  writer.u64(submission.result.bytes_processed);
  writer.u64(submission.result.duration_ns);
  put_enum(writer, submission.result.failure, 0);
  writer.text(submission.result.backend_detail);
}

bool take_completion_submission(ByteReader& reader, CompletionSubmission& submission) {
  if (!take_id(reader, submission.attempt)) return false;
  if (!take_generation(reader, submission.generation)) return false;
  if (!take_id(reader, submission.dispatch)) return false;
  if (!take_id(reader, submission.completion)) return false;
  if (!take_id(reader, submission.worker)) return false;
  if (!take_id(reader, submission.worker_boot)) return false;
  if (!take_generation(reader, submission.coordinator_epoch)) return false;
  if (!take_generation(reader, submission.domain_generation)) return false;
  if (!take_generation(reader, submission.capability_generation)) return false;
  if (!take_id(reader, submission.operation)) return false;
  if (!take_enum(reader, submission.provenance, static_cast<std::uint8_t>(Provenance::kUnsupported))) {
    return false;
  }
  std::uint8_t success = 0;
  if (!reader.u8(success)) return false;
  submission.result.success = success != 0;
  if (!reader.u64(submission.result.result_digest)) return false;
  if (!reader.u64(submission.result.bytes_processed)) return false;
  if (!reader.u64(submission.result.duration_ns)) return false;
  if (!take_enum(reader, submission.result.failure,
                 static_cast<std::uint8_t>(kFailureKindCount - 1))) {
    return false;
  }
  if (!reader.text(submission.result.backend_detail)) return false;
  submission.result.backend_detail = bounded_text(submission.result.backend_detail);
  if (!submission.attempt.valid()) return reader.reject("protocol.invalid_attempt_identity");
  return true;
}

}  // namespace

std::string_view to_string(DecodeError value) noexcept {
  switch (value) {
    case DecodeError::kOk: return "ok";
    case DecodeError::kEmpty: return "empty";
    case DecodeError::kTruncatedHeader: return "truncated_header";
    case DecodeError::kBadMagic: return "bad_magic";
    case DecodeError::kBadVersion: return "bad_version";
    case DecodeError::kUnknownType: return "unknown_type";
    case DecodeError::kOversized: return "oversized";
    case DecodeError::kLengthMismatch: return "length_mismatch";
    case DecodeError::kIntegrityFailure: return "integrity_failure";
    case DecodeError::kMalformedPayload: return "malformed_payload";
    case DecodeError::kUnsupportedFlags: return "unsupported_flags";
  }
  return "empty";
}

DecodeError decode_header(const std::uint8_t* header, std::size_t available, std::size_t max_frame,
                          FrameHeader& out) noexcept {
  if (header == nullptr || available < kFrameHeaderBytes) return DecodeError::kTruncatedHeader;
  if (max_frame < kMinFrameBytes) return DecodeError::kOversized;
  std::uint32_t magic = 0;
  std::uint16_t version = 0;
  std::uint16_t type = 0;
  std::uint16_t flags = 0;
  std::uint16_t reserved = 0;
  std::uint32_t length = 0;
  std::memcpy(&magic, header + 0, 4);
  std::memcpy(&version, header + 4, 2);
  std::memcpy(&type, header + 6, 2);
  std::memcpy(&flags, header + 8, 2);
  std::memcpy(&reserved, header + 10, 2);
  std::memcpy(&length, header + 12, 4);
  if (magic != kFrameMagic) return DecodeError::kBadMagic;
  if (version != kProtocolVersion) return DecodeError::kBadVersion;
  if (type == 0 || type > kMaxMessageType) return DecodeError::kUnknownType;
  if (flags != 0 || reserved != 0) return DecodeError::kUnsupportedFlags;
  if (length > max_frame - kFrameHeaderBytes) return DecodeError::kOversized;
  out.type = static_cast<MessageType>(type);
  out.flags = flags;
  out.length = length;
  return DecodeError::kOk;
}

Checked<std::vector<std::uint8_t>> encode_frame(MessageType type,
                                                const std::vector<std::uint8_t>& payload,
                                                std::size_t max_frame) {
  if (max_frame < kMinFrameBytes) return Checked<std::vector<std::uint8_t>>::bad("protocol.bad_bound");
  if (payload.size() > max_frame - kFrameHeaderBytes) {
    return Checked<std::vector<std::uint8_t>>::bad("protocol.oversized_payload");
  }
  ByteWriter writer(max_frame);
  writer.u32(kFrameMagic);
  writer.u16(kProtocolVersion);
  writer.u16(static_cast<std::uint16_t>(type));
  writer.u16(0);
  writer.u16(0);
  writer.u32(static_cast<std::uint32_t>(payload.size()));
  writer.u32(crc32c(payload.data(), payload.size()));
  writer.raw(payload.data(), payload.size());
  if (!writer.ok()) return Checked<std::vector<std::uint8_t>>::bad(writer.status().code);
  return Checked<std::vector<std::uint8_t>>::good(writer.take());
}

Checked<Frame> decode_frame(const std::uint8_t* data, std::size_t size, std::size_t max_frame) {
  if (data == nullptr || size == 0) return Checked<Frame>::bad("protocol.empty");
  FrameHeader header;
  const DecodeError header_error = decode_header(data, size, max_frame, header);
  if (header_error != DecodeError::kOk) {
    return Checked<Frame>::bad(std::string("protocol.") + std::string(to_string(header_error)));
  }
  if (size != kFrameHeaderBytes + header.length) {
    return Checked<Frame>::bad("protocol.length_mismatch");
  }
  std::uint32_t declared_crc = 0;
  std::memcpy(&declared_crc, data + 16, 4);
  const std::uint8_t* payload = data + kFrameHeaderBytes;
  if (crc32c(payload, header.length) != declared_crc) {
    return Checked<Frame>::bad("protocol.integrity_failure");
  }
  Frame frame;
  frame.type = header.type;
  frame.flags = header.flags;
  frame.payload.assign(payload, payload + header.length);
  return Checked<Frame>::good(std::move(frame));
}

// ---- message codecs --------------------------------------------------------

std::vector<std::uint8_t> encode(const HelloMessage& message) {
  ByteWriter writer;
  put_enum(writer, message.role, 0);
  writer.text(message.name);
  put_id(writer, message.worker);
  put_id(writer, message.boot);
  writer.text(message.host);
  put_u64(writer, message.pid);
  writer.text(message.version);
  return writer.take();
}

bool decode(const std::vector<std::uint8_t>& payload, HelloMessage& message) {
  ByteReader reader(payload);
  if (!take_enum(reader, message.role, 3)) return false;
  if (!reader.text(message.name, kMaxNameLength)) return false;
  if (!take_id(reader, message.worker)) return false;
  if (!take_id(reader, message.boot)) return false;
  if (!reader.text(message.host, kMaxLabelLength)) return false;
  if (!reader.u64(message.pid)) return false;
  if (!reader.text(message.version, kMaxNameLength)) return false;
  return reader.at_end();
}

std::vector<std::uint8_t> encode(const HelloAckMessage& message) {
  ByteWriter writer;
  writer.u8(message.accepted ? 1 : 0);
  put_generation(writer, message.epoch);
  writer.text(message.code);
  writer.text(message.detail);
  return writer.take();
}

bool decode(const std::vector<std::uint8_t>& payload, HelloAckMessage& message) {
  ByteReader reader(payload);
  std::uint8_t accepted = 0;
  if (!reader.u8(accepted)) return false;
  message.accepted = accepted != 0;
  if (!take_generation(reader, message.epoch)) return false;
  if (!reader.text(message.code, kMaxNameLength)) return false;
  if (!reader.text(message.detail)) return false;
  return reader.at_end();
}

std::vector<std::uint8_t> encode(const ErrorMessage& message) {
  ByteWriter writer;
  writer.text(message.code);
  writer.text(message.detail);
  return writer.take();
}

bool decode(const std::vector<std::uint8_t>& payload, ErrorMessage& message) {
  ByteReader reader(payload);
  if (!reader.text(message.code, kMaxNameLength)) return false;
  if (!reader.text(message.detail)) return false;
  return reader.at_end();
}

std::vector<std::uint8_t> encode(const DomainPublishMessage& message) {
  ByteWriter writer;
  write_domain(writer, message.record);
  return writer.take();
}

bool decode(const std::vector<std::uint8_t>& payload, DomainPublishMessage& message) {
  ByteReader reader(payload);
  if (!read_domain(reader, message.record)) return false;
  return reader.at_end();
}

std::vector<std::uint8_t> encode(const CapabilityPublishMessage& message) {
  ByteWriter writer;
  put_id(writer, message.capability.domain);
  put_generation(writer, message.capability.generation);
  put_generation(writer, message.capability.evidence);
  put_enum(writer, message.capability.provenance, 0);
  writer.u8(message.capability.authoritative ? 1 : 0);
  put_id(writer, message.capability.publisher);
  write_capability(writer, message.capability.capability);
  return writer.take();
}

bool decode(const std::vector<std::uint8_t>& payload, CapabilityPublishMessage& message) {
  ByteReader reader(payload);
  if (!take_id(reader, message.capability.domain)) return false;
  if (!take_generation(reader, message.capability.generation)) return false;
  if (!take_generation(reader, message.capability.evidence)) return false;
  if (!take_enum(reader, message.capability.provenance,
                 static_cast<std::uint8_t>(Provenance::kUnsupported))) {
    return false;
  }
  std::uint8_t authoritative = 0;
  if (!reader.u8(authoritative)) return false;
  message.capability.authoritative = authoritative != 0;
  if (!take_id(reader, message.capability.publisher)) return false;
  if (!read_capability(reader, message.capability.capability)) return false;
  return reader.at_end();
}

std::vector<std::uint8_t> encode(const LoadUpdateMessage& message) {
  ByteWriter writer;
  put_id(writer, message.domain);
  const DomainLoadEvidence& load = message.load;
  put_generation(writer, load.load_generation);
  put_generation(writer, load.queue_generation);
  put_generation(writer, load.health_generation);
  put_generation(writer, load.evidence);
  put_enum(writer, load.provenance, 0);
  writer.u32(load.utilization_percent);
  writer.u32(load.congestion_percent);
  writer.u32(load.queue_depth);
  writer.u32(load.in_flight);
  put_u64(writer, load.observed_latency_ns);
  put_u64(writer, load.throughput_bytes_per_second);
  writer.u8(load.healthy ? 1 : 0);
  writer.u8(load.ready ? 1 : 0);
  writer.u8(load.accepting ? 1 : 0);
  return writer.take();
}

bool decode(const std::vector<std::uint8_t>& payload, LoadUpdateMessage& message) {
  ByteReader reader(payload);
  if (!take_id(reader, message.domain)) return false;
  DomainLoadEvidence& load = message.load;
  if (!take_generation(reader, load.load_generation)) return false;
  if (!take_generation(reader, load.queue_generation)) return false;
  if (!take_generation(reader, load.health_generation)) return false;
  if (!take_generation(reader, load.evidence)) return false;
  if (!take_enum(reader, load.provenance, static_cast<std::uint8_t>(Provenance::kUnsupported))) {
    return false;
  }
  if (!reader.u32(load.utilization_percent)) return false;
  if (!reader.u32(load.congestion_percent)) return false;
  if (!reader.u32(load.queue_depth)) return false;
  if (!reader.u32(load.in_flight)) return false;
  if (!reader.u64(load.observed_latency_ns)) return false;
  if (!reader.u64(load.throughput_bytes_per_second)) return false;
  std::uint8_t flag = 0;
  if (!reader.u8(flag)) return false;
  load.healthy = flag != 0;
  if (!reader.u8(flag)) return false;
  load.ready = flag != 0;
  if (!reader.u8(flag)) return false;
  load.accepting = flag != 0;
  if (load.utilization_percent > 100 || load.congestion_percent > 100) {
    return reader.reject("protocol.invalid_load_percent");
  }
  return reader.at_end();
}

std::vector<std::uint8_t> encode(const HeartbeatMessage& message) {
  ByteWriter writer;
  put_id(writer, message.worker);
  put_id(writer, message.boot);
  put_generation(writer, message.epoch);
  put_u64(writer, message.sequence);
  return writer.take();
}

bool decode(const std::vector<std::uint8_t>& payload, HeartbeatMessage& message) {
  ByteReader reader(payload);
  if (!take_id(reader, message.worker)) return false;
  if (!take_id(reader, message.boot)) return false;
  if (!take_generation(reader, message.epoch)) return false;
  if (!reader.u64(message.sequence)) return false;
  return reader.at_end();
}

std::vector<std::uint8_t> encode(const DispatchRequestMessage& message) {
  ByteWriter writer;
  put_dispatch_envelope(writer, message.envelope);
  return writer.take();
}

bool decode(const std::vector<std::uint8_t>& payload, DispatchRequestMessage& message) {
  ByteReader reader(payload);
  if (!take_dispatch_envelope(reader, message.envelope)) return false;
  return reader.at_end();
}

std::vector<std::uint8_t> encode(const DispatchAcceptedMessage& message) {
  ByteWriter writer;
  put_id(writer, message.attempt);
  put_generation(writer, message.generation);
  put_id(writer, message.dispatch);
  put_id(writer, message.boot);
  return writer.take();
}

bool decode(const std::vector<std::uint8_t>& payload, DispatchAcceptedMessage& message) {
  ByteReader reader(payload);
  if (!take_id(reader, message.attempt)) return false;
  if (!take_generation(reader, message.generation)) return false;
  if (!take_id(reader, message.dispatch)) return false;
  if (!take_id(reader, message.boot)) return false;
  return reader.at_end();
}

std::vector<std::uint8_t> encode(const DispatchRejectedMessage& message) {
  ByteWriter writer;
  put_id(writer, message.attempt);
  put_id(writer, message.dispatch);
  writer.text(message.code);
  writer.text(message.detail);
  return writer.take();
}

bool decode(const std::vector<std::uint8_t>& payload, DispatchRejectedMessage& message) {
  ByteReader reader(payload);
  if (!take_id(reader, message.attempt)) return false;
  if (!take_id(reader, message.dispatch)) return false;
  if (!reader.text(message.code, kMaxNameLength)) return false;
  if (!reader.text(message.detail)) return false;
  return reader.at_end();
}

std::vector<std::uint8_t> encode(const CompletionMessage& message) {
  ByteWriter writer;
  put_completion_submission(writer, message.submission);
  return writer.take();
}

bool decode(const std::vector<std::uint8_t>& payload, CompletionMessage& message) {
  ByteReader reader(payload);
  if (!take_completion_submission(reader, message.submission)) return false;
  return reader.at_end();
}

std::vector<std::uint8_t> encode(const FailureReportMessage& message) {
  ByteWriter writer;
  put_id(writer, message.attempt);
  put_generation(writer, message.generation);
  put_id(writer, message.dispatch);
  put_id(writer, message.boot);
  put_generation(writer, message.epoch);
  put_enum(writer, message.kind, 0);
  writer.text(message.detail);
  return writer.take();
}

bool decode(const std::vector<std::uint8_t>& payload, FailureReportMessage& message) {
  ByteReader reader(payload);
  if (!take_id(reader, message.attempt)) return false;
  if (!take_generation(reader, message.generation)) return false;
  if (!take_id(reader, message.dispatch)) return false;
  if (!take_id(reader, message.boot)) return false;
  if (!take_generation(reader, message.epoch)) return false;
  if (!take_enum(reader, message.kind, static_cast<std::uint8_t>(kFailureKindCount - 1))) return false;
  if (!reader.text(message.detail)) return false;
  return reader.at_end();
}

std::vector<std::uint8_t> encode(const CompletionAckMessage& message) {
  ByteWriter writer;
  put_id(writer, message.attempt);
  put_enum(writer, message.rejection, 0);
  writer.u8(message.committed ? 1 : 0);
  writer.u8(message.idempotent ? 1 : 0);
  return writer.take();
}

bool decode(const std::vector<std::uint8_t>& payload, CompletionAckMessage& message) {
  ByteReader reader(payload);
  if (!take_id(reader, message.attempt)) return false;
  if (!take_enum(reader, message.rejection, static_cast<std::uint8_t>(CompletionRejection::kProvenanceMismatch))) {
    return false;
  }
  std::uint8_t flag = 0;
  if (!reader.u8(flag)) return false;
  message.committed = flag != 0;
  if (!reader.u8(flag)) return false;
  message.idempotent = flag != 0;
  return reader.at_end();
}

std::vector<std::uint8_t> encode(const CancelRequestMessage& message) {
  ByteWriter writer;
  const CancelEnvelope& envelope = message.envelope;
  put_id(writer, envelope.attempt);
  put_generation(writer, envelope.attempt_generation);
  put_id(writer, envelope.dispatch);
  put_generation(writer, envelope.coordinator_epoch);
  put_id(writer, envelope.worker_boot);
  put_id(writer, envelope.domain);
  writer.u8(envelope.may_have_executed ? 1 : 0);
  return writer.take();
}

bool decode(const std::vector<std::uint8_t>& payload, CancelRequestMessage& message) {
  ByteReader reader(payload);
  CancelEnvelope& envelope = message.envelope;
  if (!take_id(reader, envelope.attempt)) return false;
  if (!take_generation(reader, envelope.attempt_generation)) return false;
  if (!take_id(reader, envelope.dispatch)) return false;
  if (!take_generation(reader, envelope.coordinator_epoch)) return false;
  if (!take_id(reader, envelope.worker_boot)) return false;
  if (!take_id(reader, envelope.domain)) return false;
  std::uint8_t flag = 0;
  if (!reader.u8(flag)) return false;
  envelope.may_have_executed = flag != 0;
  return reader.at_end();
}

std::vector<std::uint8_t> encode(const CancelResultMessage& message) {
  ByteWriter writer;
  put_id(writer, message.attempt);
  writer.u8(message.cancelled ? 1 : 0);
  writer.u8(message.may_have_executed ? 1 : 0);
  writer.text(message.detail);
  return writer.take();
}

bool decode(const std::vector<std::uint8_t>& payload, CancelResultMessage& message) {
  ByteReader reader(payload);
  if (!take_id(reader, message.attempt)) return false;
  std::uint8_t flag = 0;
  if (!reader.u8(flag)) return false;
  message.cancelled = flag != 0;
  if (!reader.u8(flag)) return false;
  message.may_have_executed = flag != 0;
  if (!reader.text(message.detail)) return false;
  return reader.at_end();
}

std::vector<std::uint8_t> encode(const SubmitRequestMessage& message) {
  ByteWriter writer;
  put_operation_request(writer, message.request);
  writer.blob(message.payload);
  writer.u8(message.dispatch_now ? 1 : 0);
  writer.u8(message.reserve_first ? 1 : 0);
  return writer.take();
}

bool decode(const std::vector<std::uint8_t>& payload, SubmitRequestMessage& message) {
  ByteReader reader(payload);
  if (!take_operation_request(reader, message.request)) return false;
  if (!reader.blob(message.payload, kMaxProtocolPayloadBytes)) return false;
  std::uint8_t flag = 0;
  if (!reader.u8(flag)) return false;
  message.dispatch_now = flag != 0;
  if (!reader.u8(flag)) return false;
  message.reserve_first = flag != 0;
  return reader.at_end();
}

std::vector<std::uint8_t> encode(const SubmitResponseMessage& message) {
  ByteWriter writer;
  writer.u8(message.planned ? 1 : 0);
  writer.u8(message.dispatched ? 1 : 0);
  put_enum(writer, message.outcome, 0);
  put_u64(writer, message.operation);
  put_u64(writer, message.domain);
  put_enum(writer, message.domain_type, 0);
  put_enum(writer, message.provenance, 0);
  put_u64(writer, message.attempt);
  put_generation(writer, message.attempt_generation);
  put_enum(writer, message.attempt_state, 0);
  put_enum(writer, message.dispatch_rejection, 0);
  writer.text(message.code);
  writer.text(message.detail);
  // The explanation is a document, not a label: it is carried as a bounded blob so
  // that a large decision record can never be silently truncated.
  writer.blob(message.explanation_json.data(), message.explanation_json.size());
  return writer.take();
}

bool decode(const std::vector<std::uint8_t>& payload, SubmitResponseMessage& message) {
  ByteReader reader(payload);
  std::uint8_t flag = 0;
  if (!reader.u8(flag)) return false;
  message.planned = flag != 0;
  if (!reader.u8(flag)) return false;
  message.dispatched = flag != 0;
  if (!take_enum(reader, message.outcome, static_cast<std::uint8_t>(SelectionOutcome::kCancelled))) {
    return false;
  }
  if (!reader.u64(message.operation)) return false;
  if (!reader.u64(message.domain)) return false;
  if (!take_enum(reader, message.domain_type, kExecutionDomainTypeCount - 1)) return false;
  if (!take_enum(reader, message.provenance, static_cast<std::uint8_t>(Provenance::kUnsupported))) {
    return false;
  }
  if (!reader.u64(message.attempt)) return false;
  if (!take_generation(reader, message.attempt_generation)) return false;
  if (!take_enum(reader, message.attempt_state, static_cast<std::uint8_t>(AttemptState::kFenced))) {
    return false;
  }
  if (!take_enum(reader, message.dispatch_rejection,
                 static_cast<std::uint8_t>(DispatchRejection::kCapacityExhausted))) {
    return false;
  }
  if (!reader.text(message.code, kMaxNameLength)) return false;
  if (!reader.text(message.detail)) return false;
  std::vector<std::uint8_t> explanation;
  if (!reader.blob(explanation, kMaxBlobBytes)) return false;
  message.explanation_json.assign(explanation.begin(), explanation.end());
  return reader.at_end();
}

std::vector<std::uint8_t> encode(const QueryRequestMessage& message) {
  ByteWriter writer;
  put_enum(writer, message.kind, 0);
  put_u64(writer, message.id);
  return writer.take();
}

bool decode(const std::vector<std::uint8_t>& payload, QueryRequestMessage& message) {
  ByteReader reader(payload);
  if (!take_enum(reader, message.kind, static_cast<std::uint8_t>(QueryKind::kOperation))) return false;
  if (!reader.u64(message.id)) return false;
  return reader.at_end();
}

std::vector<std::uint8_t> encode(const QueryResponseMessage& message) {
  ByteWriter writer;
  writer.u8(message.ok ? 1 : 0);
  writer.text(message.code);
  writer.blob(message.body.data(), message.body.size());
  return writer.take();
}

bool decode(const std::vector<std::uint8_t>& payload, QueryResponseMessage& message) {
  ByteReader reader(payload);
  std::uint8_t flag = 0;
  if (!reader.u8(flag)) return false;
  message.ok = flag != 0;
  if (!reader.text(message.code, kMaxNameLength)) return false;
  std::vector<std::uint8_t> body;
  if (!reader.blob(body, kMaxBlobBytes)) return false;
  message.body.assign(body.begin(), body.end());
  return reader.at_end();
}

std::vector<std::uint8_t> encode(const AdminRequestMessage& message) {
  ByteWriter writer;
  put_enum(writer, message.action, 0);
  put_u64(writer, message.id);
  writer.text(message.reason);
  put_u64(writer, message.value);
  return writer.take();
}

bool decode(const std::vector<std::uint8_t>& payload, AdminRequestMessage& message) {
  ByteReader reader(payload);
  if (!take_enum(reader, message.action, static_cast<std::uint8_t>(AdminAction::kShutdownCoordinator))) {
    return false;
  }
  if (!reader.u64(message.id)) return false;
  if (!reader.text(message.reason)) return false;
  if (!reader.u64(message.value)) return false;
  return reader.at_end();
}

std::vector<std::uint8_t> encode(const AdminResponseMessage& message) {
  ByteWriter writer;
  writer.u8(message.ok ? 1 : 0);
  writer.text(message.code);
  writer.text(message.detail);
  writer.blob(message.body.data(), message.body.size());
  return writer.take();
}

bool decode(const std::vector<std::uint8_t>& payload, AdminResponseMessage& message) {
  ByteReader reader(payload);
  std::uint8_t flag = 0;
  if (!reader.u8(flag)) return false;
  message.ok = flag != 0;
  if (!reader.text(message.code, kMaxNameLength)) return false;
  if (!reader.text(message.detail)) return false;
  std::vector<std::uint8_t> body;
  if (!reader.blob(body, kMaxBlobBytes)) return false;
  message.body.assign(body.begin(), body.end());
  return reader.at_end();
}

std::vector<std::uint8_t> encode(const ShutdownRequestMessage& message) {
  ByteWriter writer;
  writer.text(message.reason);
  return writer.take();
}

bool decode(const std::vector<std::uint8_t>& payload, ShutdownRequestMessage& message) {
  ByteReader reader(payload);
  if (!reader.text(message.reason)) return false;
  return reader.at_end();
}

std::vector<std::uint8_t> encode(const FenceMessage& message) {
  ByteWriter writer;
  put_id(writer, message.domain);
  put_id(writer, message.boot);
  writer.text(message.reason);
  return writer.take();
}

bool decode(const std::vector<std::uint8_t>& payload, FenceMessage& message) {
  ByteReader reader(payload);
  if (!take_id(reader, message.domain)) return false;
  if (!take_id(reader, message.boot)) return false;
  if (!reader.text(message.reason)) return false;
  return reader.at_end();
}

std::vector<std::uint8_t> encode(const GoodbyeMessage& message) {
  ByteWriter writer;
  writer.text(message.reason);
  return writer.take();
}

bool decode(const std::vector<std::uint8_t>& payload, GoodbyeMessage& message) {
  ByteReader reader(payload);
  if (!reader.text(message.reason)) return false;
  return reader.at_end();
}

}  // namespace dist
}  // namespace tos
