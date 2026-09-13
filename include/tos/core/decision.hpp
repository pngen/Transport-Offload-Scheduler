// Structured, machine-readable decisions.
//
// A machine caller reads outcome codes and named reasons. Human-readable prose is
// carried alongside but is never the interface.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#ifndef TOS_CORE_DECISION_HPP
#define TOS_CORE_DECISION_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "tos/core/enums.hpp"
#include "tos/core/identities.hpp"
#include "tos/core/operation.hpp"
#include "tos/core/ranking.hpp"

namespace tos {

/// Evaluation of one candidate domain during hard eligibility.
struct CandidateEvaluation {
  ExecutionDomainId domain;
  ExecutionDomainType domain_type{ExecutionDomainType::kCpu};
  Provenance provenance{Provenance::kUnsupported};
  bool eligible{false};
  IneligibilityReason reason{IneligibilityReason::kNone};
  std::string detail;
};

/// Named reason code attached to a decision, e.g. "fallback.from.DPU".
struct DecisionReason {
  std::string code;   ///< stable machine token
  std::string detail; ///< bounded human-readable detail
};

struct DecisionExplanation {
  SelectionOutcome outcome{SelectionOutcome::kNoEligibleDomain};
  TransportOperationId operation;
  std::string operation_class_name;
  ExecutionDomainId selected_domain;
  ExecutionDomainType selected_domain_type{ExecutionDomainType::kCpu};
  Provenance selected_provenance{Provenance::kUnsupported};
  bool selected_is_offload{false};
  bool fallback_used{false};
  std::uint32_t fallback_depth{0};
  ExecutionDomainId fallback_from;
  std::vector<CandidateEvaluation> candidates;
  std::vector<RankedCandidate> ranking;
  std::vector<DecisionReason> reasons;
  std::vector<ExecutionDomainType> fallback_chain_tried;
  SnapshotGeneration snapshot;
  PolicyGeneration policy_generation;
  std::uint64_t evaluated_candidates{0};
  std::uint64_t rejected_candidates{0};

  [[nodiscard]] bool has_reason(std::string_view code) const;
  [[nodiscard]] std::string_view primary_reason_code() const;
};

/// Structured rejection of a candidate, used by tests and inspection tooling.
[[nodiscard]] CandidateEvaluation make_rejection(ExecutionDomainId domain,
                                                 ExecutionDomainType type,
                                                 Provenance provenance,
                                                 IneligibilityReason reason,
                                                 std::string detail);

/// Render a decision explanation as deterministic JSON. Machine callers should read
/// the structured fields; this rendering exists for inspection tooling and logs.
[[nodiscard]] std::string render_explanation_json(const DecisionExplanation& explanation);

}  // namespace tos

#endif  // TOS_CORE_DECISION_HPP
