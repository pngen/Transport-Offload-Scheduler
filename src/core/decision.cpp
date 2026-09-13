// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "tos/core/decision.hpp"

#include <algorithm>

namespace tos {

bool DecisionExplanation::has_reason(std::string_view code) const {
  return std::any_of(reasons.begin(), reasons.end(),
                     [code](const DecisionReason& reason) { return reason.code == code; });
}

std::string_view DecisionExplanation::primary_reason_code() const {
  if (reasons.empty()) return "none";
  return reasons.front().code;
}

CandidateEvaluation make_rejection(ExecutionDomainId domain, ExecutionDomainType type,
                                   Provenance provenance, IneligibilityReason reason,
                                   std::string detail) {
  CandidateEvaluation evaluation;
  evaluation.domain = domain;
  evaluation.domain_type = type;
  evaluation.provenance = provenance;
  evaluation.eligible = false;
  evaluation.reason = reason;
  evaluation.detail = bounded_text(detail);
  return evaluation;
}

namespace {

std::string quote_json(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 2);
  out.push_back('"');
  for (char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          out += "?";
        } else {
          out.push_back(c);
        }
    }
  }
  out.push_back('"');
  return out;
}

void field(std::string& out, bool& first, std::string_view name, std::string value) {
  if (!first) out.push_back(',');
  first = false;
  out += quote_json(name);
  out.push_back(':');
  out += std::move(value);
}

}  // namespace

std::string render_explanation_json(const DecisionExplanation& explanation) {
  std::string out = "{";
  bool first = true;
  field(out, first, "outcome", quote_json(to_string(explanation.outcome)));
  field(out, first, "operation", quote_json(format_id(explanation.operation.value())));
  field(out, first, "operation_class", quote_json(explanation.operation_class_name));
  field(out, first, "selected_domain", quote_json(format_id(explanation.selected_domain.value())));
  field(out, first, "selected_domain_type", quote_json(to_string(explanation.selected_domain_type)));
  field(out, first, "selected_provenance", quote_json(to_string(explanation.selected_provenance)));
  field(out, first, "selected_is_offload", explanation.selected_is_offload ? "true" : "false");
  field(out, first, "fallback_used", explanation.fallback_used ? "true" : "false");
  field(out, first, "fallback_depth", std::to_string(explanation.fallback_depth));
  field(out, first, "fallback_from", quote_json(format_id(explanation.fallback_from.value())));
  field(out, first, "evaluated_candidates", std::to_string(explanation.evaluated_candidates));
  field(out, first, "rejected_candidates", std::to_string(explanation.rejected_candidates));
  field(out, first, "policy_generation", std::to_string(explanation.policy_generation.value()));
  field(out, first, "snapshot_generation", std::to_string(explanation.snapshot.value()));

  {
    std::string reasons = "[";
    for (std::size_t i = 0; i < explanation.reasons.size(); ++i) {
      if (i > 0) reasons.push_back(',');
      std::string entry = "{";
      bool entry_first = true;
      field(entry, entry_first, "code", quote_json(explanation.reasons[i].code));
      field(entry, entry_first, "detail", quote_json(explanation.reasons[i].detail));
      entry += "}";
      reasons += entry;
    }
    reasons += "]";
    field(out, first, "reasons", reasons);
  }
  {
    std::string candidates = "[";
    for (std::size_t i = 0; i < explanation.candidates.size(); ++i) {
      const CandidateEvaluation& candidate = explanation.candidates[i];
      if (i > 0) candidates.push_back(',');
      std::string entry = "{";
      bool entry_first = true;
      field(entry, entry_first, "domain", quote_json(format_id(candidate.domain.value())));
      field(entry, entry_first, "domain_type", quote_json(to_string(candidate.domain_type)));
      field(entry, entry_first, "provenance", quote_json(to_string(candidate.provenance)));
      field(entry, entry_first, "eligible", candidate.eligible ? "true" : "false");
      field(entry, entry_first, "reason", quote_json(to_string(candidate.reason)));
      field(entry, entry_first, "detail", quote_json(candidate.detail));
      entry += "}";
      candidates += entry;
    }
    candidates += "]";
    field(out, first, "candidates", candidates);
  }
  {
    std::string ranking = "[";
    for (std::size_t i = 0; i < explanation.ranking.size(); ++i) {
      const RankedCandidate& candidate = explanation.ranking[i];
      if (i > 0) ranking.push_back(',');
      std::string entry = "{";
      bool entry_first = true;
      field(entry, entry_first, "domain", quote_json(format_id(candidate.domain_id)));
      field(entry, entry_first, "rank", std::to_string(candidate.rank));
      field(entry, entry_first, "score", std::to_string(candidate.weighted_score));
      field(entry, entry_first, "total_weight", std::to_string(candidate.total_weight));
      std::string factors = "{";
      bool factors_first = true;
      for (std::size_t k = 0; k < kRankingFactorCount; ++k) {
        field(factors, factors_first, to_string(static_cast<RankingFactor>(k)),
              std::to_string(candidate.factors[k]));
      }
      factors += "}";
      field(entry, entry_first, "factors", factors);
      entry += "}";
      ranking += entry;
    }
    ranking += "]";
    field(out, first, "ranking", ranking);
  }
  {
    std::string chain = "[";
    for (std::size_t i = 0; i < explanation.fallback_chain_tried.size(); ++i) {
      if (i > 0) chain.push_back(',');
      chain += quote_json(to_string(explanation.fallback_chain_tried[i]));
    }
    chain += "]";
    field(out, first, "fallback_chain_tried", chain);
  }
  out += "}";
  return out;
}

}  // namespace tos