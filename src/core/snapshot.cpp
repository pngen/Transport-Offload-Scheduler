// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "tos/core/snapshot.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

namespace tos {
namespace {

std::string quote(std::string_view text) {
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
          char buffer[8] = {0};
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(c));
          out += buffer;
        } else {
          out.push_back(c);
        }
    }
  }
  out.push_back('"');
  return out;
}

std::string number(std::uint64_t value) { return std::to_string(value); }

std::string id_text(std::uint64_t value) { return format_id(value); }

void append_field(std::string& out, bool& first, std::string_view name, std::string value) {
  if (!first) out.push_back(',');
  first = false;
  out += quote(name);
  out.push_back(':');
  out += std::move(value);
}

}  // namespace

const ExecutionDomainRecord* SchedulerSnapshot::find_domain(ExecutionDomainId id) const {
  const auto found = std::find_if(domains.begin(), domains.end(),
                                  [id](const ExecutionDomainRecord& d) { return d.id == id; });
  return found == domains.end() ? nullptr : &*found;
}

std::string SchedulerSnapshot::render_json() const {
  std::string out = "{";
  bool first = true;
  append_field(out, first, "snapshot_generation", number(generation.value()));
  append_field(out, first, "coordinator_epoch", number(coordinator_epoch.value()));
  append_field(out, first, "policy_generation", number(policy_generation.value()));
  append_field(out, first, "running", running ? "true" : "false");
  append_field(out, first, "mutation_sequence", number(mutation_sequence));

  std::string totals_json = "{";
  bool tfirst = true;
  append_field(totals_json, tfirst, "domains", number(totals.domains));
  append_field(totals_json, tfirst, "domains_fenced", number(totals.domains_fenced));
  append_field(totals_json, tfirst, "domains_with_current_evidence",
               number(totals.domains_with_current_evidence));
  append_field(totals_json, tfirst, "live_attempts", number(totals.live_attempts));
  append_field(totals_json, tfirst, "completed_attempts", number(totals.completed_attempts));
  append_field(totals_json, tfirst, "ambiguous_attempts", number(totals.ambiguous_attempts));
  append_field(totals_json, tfirst, "fenced_attempts", number(totals.fenced_attempts));
  append_field(totals_json, tfirst, "outstanding_reservations",
               number(totals.outstanding_reservations));
  append_field(totals_json, tfirst, "plans_created", number(totals.plans_created));
  append_field(totals_json, tfirst, "plans_dispatched", number(totals.plans_dispatched));
  append_field(totals_json, tfirst, "dispatch_rejections", number(totals.dispatch_rejections));
  append_field(totals_json, tfirst, "completion_rejections", number(totals.completion_rejections));
  append_field(totals_json, tfirst, "fallbacks", number(totals.fallbacks));
  append_field(totals_json, tfirst, "retries", number(totals.retries));
  totals_json += "}";
  append_field(out, first, "totals", totals_json);

  {
    std::string list = "[";
    for (std::size_t i = 0; i < domains.size(); ++i) {
      const ExecutionDomainRecord& domain = domains[i];
      if (i > 0) list.push_back(',');
      std::string entry = "{";
      bool efirst = true;
      append_field(entry, efirst, "id", quote(id_text(domain.id.value())));
      append_field(entry, efirst, "name", quote(domain.name));
      append_field(entry, efirst, "type", quote(to_string(domain.type)));
      append_field(entry, efirst, "provenance", quote(to_string(domain.provenance)));
      append_field(entry, efirst, "domain_generation", number(domain.generation.value()));
      append_field(entry, efirst, "capability_generation",
                   number(domain.capability.generation.value()));
      append_field(entry, efirst, "worker", quote(id_text(domain.worker.value())));
      append_field(entry, efirst, "worker_boot", quote(id_text(domain.worker_boot.value())));
      append_field(entry, efirst, "fenced", domain.fenced ? "true" : "false");
      append_field(entry, efirst, "isolation", quote(to_string(domain.isolation)));
      append_field(entry, efirst, "topology_generation", number(domain.topology.generation.value()));
      append_field(entry, efirst, "locality_generation", number(domain.locality.generation.value()));
      append_field(entry, efirst, "health_generation", number(domain.load.health_generation.value()));
      append_field(entry, efirst, "queue_generation", number(domain.load.queue_generation.value()));
      append_field(entry, efirst, "load_generation", number(domain.load.load_generation.value()));
      append_field(entry, efirst, "compatibility_generation",
                   number(domain.compatibility.generation.value()));
      append_field(entry, efirst, "evidence_generation", number(domain.capability.evidence.value()));
      append_field(entry, efirst, "evidence_current",
                   domain.load.published() ? "true" : "false");
      append_field(entry, efirst, "healthy", domain.load.healthy ? "true" : "false");
      append_field(entry, efirst, "ready", domain.load.ready ? "true" : "false");
      append_field(entry, efirst, "utilization_percent", number(domain.load.utilization_percent));
      append_field(entry, efirst, "queue_depth", number(domain.load.queue_depth));
      append_field(entry, efirst, "in_flight", number(domain.load.in_flight));
      append_field(entry, efirst, "congestion_percent", number(domain.load.congestion_percent));
      append_field(entry, efirst, "locality_class",
                   quote(to_string(domain.locality.class_to_payload)));
      append_field(entry, efirst, "parent_host", quote(domain.parent_host));
      append_field(entry, efirst, "parent_device", quote(domain.parent_device));
      append_field(entry, efirst, "backend_family",
                   quote(domain.capability.capability.backend_family));
      append_field(entry, efirst, "driver_backend_version",
                   number(domain.compatibility.driver_backend_version));
      append_field(entry, efirst, "firmware_generation",
                   number(domain.compatibility.firmware_generation));
      append_field(entry, efirst, "backend_generation", number(domain.backend_generation.value()));
      entry += "}";
      list += entry;
    }
    list += "]";
    append_field(out, first, "domains", list);
  }

  {
    std::string list = "[";
    for (std::size_t i = 0; i < workers.size(); ++i) {
      const WorkerBootView& worker = workers[i];
      if (i > 0) list.push_back(',');
      std::string entry = "{";
      bool efirst = true;
      append_field(entry, efirst, "worker", quote(id_text(worker.worker.value())));
      append_field(entry, efirst, "boot", quote(id_text(worker.boot.value())));
      append_field(entry, efirst, "current", worker.current ? "true" : "false");
      append_field(entry, efirst, "fenced", worker.fenced ? "true" : "false");
      entry += "}";
      list += entry;
    }
    list += "]";
    append_field(out, first, "workers", list);
  }

  {
    std::string list = "[";
    for (std::size_t i = 0; i < reservations.size(); ++i) {
      const Reservation& reservation = reservations[i];
      if (i > 0) list.push_back(',');
      std::string entry = "{";
      bool efirst = true;
      append_field(entry, efirst, "id", quote(id_text(reservation.id.value())));
      append_field(entry, efirst, "domain", quote(id_text(reservation.domain.value())));
      append_field(entry, efirst, "state", quote(to_string(reservation.state)));
      append_field(entry, efirst, "domain_generation", number(reservation.domain_generation.value()));
      append_field(entry, efirst, "capability_generation",
                   number(reservation.capability_generation.value()));
      append_field(entry, efirst, "policy_generation", number(reservation.policy_generation.value()));
      append_field(entry, efirst, "operation", quote(id_text(reservation.operation.value())));
      entry += "}";
      list += entry;
    }
    list += "]";
    append_field(out, first, "reservations", list);
  }

  {
    std::string list = "[";
    for (std::size_t i = 0; i < attempts.size(); ++i) {
      const ExecutionAttempt& attempt = attempts[i];
      if (i > 0) list.push_back(',');
      std::string entry = "{";
      bool efirst = true;
      append_field(entry, efirst, "id", quote(id_text(attempt.id.value())));
      append_field(entry, efirst, "generation", number(attempt.generation.value()));
      append_field(entry, efirst, "operation", quote(id_text(attempt.operation.value())));
      append_field(entry, efirst, "domain", quote(id_text(attempt.domain.value())));
      append_field(entry, efirst, "domain_type", quote(to_string(attempt.domain_type)));
      append_field(entry, efirst, "state", quote(to_string(attempt.state)));
      append_field(entry, efirst, "resolution", quote(to_string(attempt.resolution)));
      append_field(entry, efirst, "side_effect", quote(to_string(attempt.side_effect)));
      append_field(entry, efirst, "failure", quote(to_string(attempt.failure)));
      append_field(entry, efirst, "fallback", attempt.fallback ? "true" : "false");
      append_field(entry, efirst, "retry_index", number(attempt.retry_index));
      append_field(entry, efirst, "completion_committed",
                   attempt.completion_committed ? "true" : "false");
      append_field(entry, efirst, "worker_boot", quote(id_text(attempt.worker_boot.value())));
      append_field(entry, efirst, "coordinator_epoch", number(attempt.coordinator_epoch.value()));
      append_field(entry, efirst, "provenance", quote(to_string(attempt.provenance)));
      append_field(entry, efirst, "result_digest", number(attempt.result.result_digest));
      append_field(entry, efirst, "bytes_processed", number(attempt.result.bytes_processed));
      append_field(entry, efirst, "detail", quote(attempt.close_reason));
      entry += "}";
      list += entry;
    }
    list += "]";
    append_field(out, first, "attempts", list);
  }

  {
    std::string list = "[";
    for (std::size_t i = 0; i < reconciliations.size(); ++i) {
      const ReconciliationReport& report = reconciliations[i];
      if (i > 0) list.push_back(',');
      std::string entry = "{";
      bool efirst = true;
      append_field(entry, efirst, "coordinator_epoch", number(report.coordinator_epoch.value()));
      append_field(entry, efirst, "domains_compared", number(report.domains_compared));
      append_field(entry, efirst, "domains_matched", number(report.domains_matched));
      append_field(entry, efirst, "discrepancies", number(report.discrepancies));
      std::string items = "[";
      for (std::size_t k = 0; k < report.items.size(); ++k) {
        const ReconciliationItem& item = report.items[k];
        if (k > 0) items.push_back(',');
        std::string item_text = "{";
        bool ifirst = true;
        append_field(item_text, ifirst, "kind", quote(to_string(item.kind)));
        append_field(item_text, ifirst, "domain", quote(id_text(item.domain.value())));
        append_field(item_text, ifirst, "domain_name", quote(item.domain_name));
        append_field(item_text, ifirst, "expected_generation", number(item.expected_generation.value()));
        append_field(item_text, ifirst, "observed_generation", number(item.observed_generation.value()));
        append_field(item_text, ifirst, "expected_boot", quote(id_text(item.expected_boot.value())));
        append_field(item_text, ifirst, "observed_boot", quote(id_text(item.observed_boot.value())));
        append_field(item_text, ifirst, "attempt", quote(id_text(item.attempt.value())));
        append_field(item_text, ifirst, "detail", quote(item.detail));
        item_text += "}";
        items += item_text;
      }
      items += "]";
      append_field(entry, efirst, "items", items);
      entry += "}";
      list += entry;
    }
    list += "]";
    append_field(out, first, "reconciliations", list);
  }
  out += "}";
  return out;
}

std::string SchedulerSnapshot::render_text() const {
  std::string out;
  out += "snapshot generation=" + id_text(generation.value()) +
         " coordinator_epoch=" + std::to_string(coordinator_epoch.value()) +
         " policy_generation=" + id_text(policy_generation.value()) +
         " running=" + (running ? "yes" : "no") + "\n";
  out += "domains=" + std::to_string(totals.domains) +
         " fenced=" + std::to_string(totals.domains_fenced) +
         " current_evidence=" + std::to_string(totals.domains_with_current_evidence) +
         " live_attempts=" + std::to_string(totals.live_attempts) +
         " completed=" + std::to_string(totals.completed_attempts) +
         " ambiguous=" + std::to_string(totals.ambiguous_attempts) +
         " fenced_attempts=" + std::to_string(totals.fenced_attempts) +
         " reservations=" + std::to_string(totals.outstanding_reservations) + "\n";
  out += "plans=" + std::to_string(totals.plans_created) +
         " dispatched=" + std::to_string(totals.plans_dispatched) +
         " dispatch_rejections=" + std::to_string(totals.dispatch_rejections) +
         " completion_rejections=" + std::to_string(totals.completion_rejections) +
         " fallbacks=" + std::to_string(totals.fallbacks) +
         " retries=" + std::to_string(totals.retries) + "\n";
  for (const ExecutionDomainRecord& domain : domains) {
    out += "  domain " + id_text(domain.id.value()) + " " + domain.name + " [" +
           std::string(to_string(domain.type)) + "/" +
           std::string(to_string(domain.provenance)) + "]" +
           " gen=" + std::to_string(domain.generation.value()) +
           " cap=" + std::to_string(domain.capability.generation.value()) +
           " boot=" + id_text(domain.worker_boot.value()) +
           (domain.fenced ? " FENCED" : "") +
           (domain.load.published() ? "" : " evidence=stale") +
           " isolation=" + std::string(to_string(domain.isolation)) + "\n";
  }
  for (const WorkerBootView& worker : workers) {
    out += "  worker " + id_text(worker.worker.value()) + " boot " + id_text(worker.boot.value()) +
           (worker.current ? " current" : "") + (worker.fenced ? " fenced" : "") + "\n";
  }
  for (const Reservation& reservation : reservations) {
    out += "  reservation " + id_text(reservation.id.value()) + " domain " +
           id_text(reservation.domain.value()) + " state " +
           std::string(to_string(reservation.state)) + "\n";
  }
  for (const ExecutionAttempt& attempt : attempts) {
    out += "  attempt " + id_text(attempt.id.value()) + " gen " +
           std::to_string(attempt.generation.value()) + " operation " +
           id_text(attempt.operation.value()) + " domain " + id_text(attempt.domain.value()) + " " +
           std::string(to_string(attempt.state)) + "/" +
           std::string(to_string(attempt.resolution)) + " side_effect " +
           std::string(to_string(attempt.side_effect)) + " origin " +
           std::string(to_string(attempt.provenance)) + "\n";
  }
  for (const ReconciliationReport& report : reconciliations) {
    out += report.render_text();
  }
  return out;
}

}  // namespace tos
