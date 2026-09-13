// Canonical codecs shared by the durable format and the wire protocol.
//
// One implementation of each record codec means the protocol validates exactly what
// persistence validates, and a corrupt or impossible record is rejected identically
// on both paths.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#ifndef TOS_SRC_PERSIST_CODEC_HPP
#define TOS_SRC_PERSIST_CODEC_HPP

#include "tos/core/attempt.hpp"
#include "tos/core/domain.hpp"
#include "tos/core/policy.hpp"
#include "tos/util/bytes.hpp"

namespace tos {
namespace codec {

void write_capability(ByteWriter& writer, const CapabilitySet& capability);
bool read_capability(ByteReader& reader, CapabilitySet& capability);

void write_domain(ByteWriter& writer, const ExecutionDomainRecord& domain);
bool read_domain(ByteReader& reader, ExecutionDomainRecord& domain);

void write_attempt(ByteWriter& writer, const ExecutionAttempt& attempt);
bool read_attempt(ByteReader& reader, ExecutionAttempt& attempt);

void write_policy(ByteWriter& writer, const SchedulerPolicy& policy);
bool read_policy(ByteReader& reader, SchedulerPolicy& policy);

}  // namespace codec
}  // namespace tos

#endif  // TOS_SRC_PERSIST_CODEC_HPP
