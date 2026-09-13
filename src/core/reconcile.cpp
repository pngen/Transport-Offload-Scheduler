// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "tos/core/reconcile.hpp"

#include <algorithm>
#include <string>

namespace tos {

std::uint64_t ReconciliationReport::count_of(DiscrepancyKind kind) const noexcept {
  return static_cast<std::uint64_t>(
      std::count_if(items.begin(), items.end(),
                    [kind](const ReconciliationItem& item) { return item.kind == kind; }));
}

std::string ReconciliationReport::render_text() const {
  std::string out;
  out += "reconciliation epoch=" + std::to_string(coordinator_epoch.value()) +
         " compared=" + std::to_string(domains_compared) +
         " matched=" + std::to_string(domains_matched) +
         " discrepancies=" + std::to_string(discrepancies) +
         " attempts_classified=" + std::to_string(attempts_classified) +
         " conservative=" + (conservative ? "yes" : "no") + "\n";
  for (const ReconciliationItem& item : items) {
    out += "  discrepancy " + std::string(to_string(item.kind)) + " domain " +
           std::string(item.domain_name.empty() ? format_id(item.domain.value()) : item.domain_name) +
           " expected_gen=" + std::to_string(item.expected_generation.value()) +
           " observed_gen=" + std::to_string(item.observed_generation.value()) +
           " expected_boot=" + format_id(item.expected_boot.value()) +
           " observed_boot=" + format_id(item.observed_boot.value()) +
           (item.attempt.valid() ? " attempt=" + format_id(item.attempt.value()) : "") +
           " detail=" + item.detail + "\n";
  }
  return out;
}

}  // namespace tos
