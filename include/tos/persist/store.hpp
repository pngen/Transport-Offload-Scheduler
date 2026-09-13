// Versioned, integrity-checked, atomically replaced durable state.
//
// Only state that remains meaningful across a restart is persisted. Volatile
// queue/load/health evidence is explicitly excluded: a recovered runtime must not
// resurrect operational truth.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#ifndef TOS_PERSIST_STORE_HPP
#define TOS_PERSIST_STORE_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "tos/core/attempt.hpp"
#include "tos/core/domain.hpp"
#include "tos/core/policy.hpp"
#include "tos/util/status.hpp"

namespace tos {

/// Hard bound on a persisted state file.
inline constexpr std::size_t kMaxPersistedBytes = 64 * 1024 * 1024;
inline constexpr std::size_t kMaxPersistedDomains = 65536;
inline constexpr std::size_t kMaxPersistedAttempts = 200000;

/// Serialized counters. Persisted so that identities never collide across restarts.
enum PersistedCounter : int {
  kCounterOperation = 0,
  kCounterPlanSequence = 1,
  kCounterDispatch = 2,
  kCounterSnapshot = 3,
  kCounterDomain = 4,
  kCounterCount = 5,
};

struct PersistedState {
  PersistenceGeneration generation;
  CoordinatorEpoch last_epoch;
  PolicyGeneration policy_generation;
  bool has_policy{false};
  SchedulerPolicy policy;
  std::vector<ExecutionDomainRecord> domains;  ///< durable structural records only
  std::vector<ExecutionAttempt> attempts;      ///< terminal records, conservatively classified
  std::vector<WorkerBootId> fenced_boots;
  std::uint64_t counters[kCounterCount] = {0, 0, 0, 0, 0};
};

class StateStore {
 public:
  explicit StateStore(std::string path);

  [[nodiscard]] const std::string& path() const noexcept { return path_; }

  /// Atomic replace: a temporary sibling is written and flushed, then renamed over
  /// the destination. An interrupted save can never leave a partially valid file.
  [[nodiscard]] Status save(const PersistedState& state) const;
  [[nodiscard]] Checked<PersistedState> load() const;
  [[nodiscard]] Status remove() const;
  [[nodiscard]] bool exists() const noexcept;

  /// Canonical encoding. Identical logical state always produces identical bytes.
  [[nodiscard]] static std::vector<std::uint8_t> encode(const PersistedState& state);
  /// Full validation before any value is returned. Truncation, corruption, bad
  /// versions, duplicate identities, generation rollback and impossible lifecycles
  /// are all rejected here rather than partially applied.
  [[nodiscard]] static Checked<PersistedState> decode(const std::uint8_t* data, std::size_t size);
  [[nodiscard]] static Checked<PersistedState> decode(const std::vector<std::uint8_t>& bytes);

  [[nodiscard]] static std::uint32_t payload_crc(const PersistedState& state);

 private:
  std::string path_;
};

}  // namespace tos

#endif  // TOS_PERSIST_STORE_HPP
