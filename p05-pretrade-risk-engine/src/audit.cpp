#include "risk/audit.hpp"

#include <set>

namespace risk {

AuditBuilder::AuditBuilder(TenantId tenant, DomainId domain,
                           std::string component_version)
    : tenant_(std::move(tenant)),
      domain_(std::move(domain)),
      component_version_(std::move(component_version)) {}

AuditEvent AuditBuilder::begin(std::string event_type, std::string old_state_hash) {
  AuditEvent e;
  e.tenant_id = tenant_;
  e.domain_id = domain_;
  e.event_type = std::move(event_type);
  e.old_state_hash = std::move(old_state_hash);
  e.component_version = component_version_;
  return e;
}

AuditEvent& AuditBuilder::with_command(AuditEvent& e, const CommandId& command_id,
                                       const AccountId& account,
                                       const SymbolId& symbol) {
  e.command_id = command_id;
  e.account_id = account;
  e.symbol = symbol;
  return e;
}

AuditEvent& AuditBuilder::with_reservation(AuditEvent& e, const ReservationId& id) {
  e.reservation_id = id;
  return e;
}

AuditEvent& AuditBuilder::with_reason(AuditEvent& e, Reason reason, std::string note) {
  e.reason = reason;
  e.note = std::move(note);
  return e;
}

AuditChainReport verify_audit_chain(const std::vector<AuditEvent>& events) {
  AuditChainReport report;
  report.events = events.size();
  if (events.empty()) return report;

  Revision last_revision = events.front().commit_revision;
  int last_ordinal = -1;
  std::string expected_old;
  bool have_expected = false;

  for (std::size_t i = 0; i < events.size(); ++i) {
    const AuditEvent& e = events[i];
    report.revisions.push_back(e.commit_revision);

    if (e.commit_revision < last_revision) {
      report.ok = false;
      report.detail = "revision went backwards at index " + std::to_string(i);
      return report;
    }
    if (e.commit_revision != last_revision) {
      last_revision = e.commit_revision;
      last_ordinal = -1;
    }
    if (e.event_ordinal != last_ordinal + 1) {
      report.ok = false;
      report.detail = "ordinal gap at index " + std::to_string(i) + " (revision " +
                      std::to_string(e.commit_revision) + ", ordinal " +
                      std::to_string(e.event_ordinal) + ")";
      return report;
    }
    last_ordinal = e.event_ordinal;

    if (have_expected && e.old_state_hash != expected_old) {
      report.ok = false;
      report.detail = "state hash chain broken at index " + std::to_string(i);
      return report;
    }
    if (e.event_type.empty()) {
      report.ok = false;
      report.detail = "event at index " + std::to_string(i) + " has no type";
      return report;
    }
    if (e.policy_version == 0) {
      // RI-30: a decision with no pinned policy version is not auditable.
      report.ok = false;
      report.detail = "event at index " + std::to_string(i) +
                      " has no policy version";
      return report;
    }
    if (e.new_state_hash != e.old_state_hash) ++report.mutations;

    expected_old = e.new_state_hash;
    have_expected = true;
  }

  return report;
}

Status verify_every_reservation_audited(const std::vector<Reservation>& reservations,
                                        const std::vector<AuditEvent>& events) {
  std::set<ReservationId> audited;
  for (const AuditEvent& e : events) {
    if (!e.reservation_id.empty()) audited.insert(e.reservation_id);
  }
  for (const Reservation& r : reservations) {
    if (audited.count(r.reservation_id) == 0) {
      return Status::fail(Reason::AuditUnavailable,
                          "reservation " + r.reservation_id +
                              " has no audit event");
    }
  }
  return Status::success();
}

}  // namespace risk
