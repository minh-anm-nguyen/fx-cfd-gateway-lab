// Audit record construction and chain verification (§18.2, RI-12/RI-30).
//
// Every committed mutation produces exactly one audit event carrying the state
// hash before and after. Chained together, those hashes make the audit trail
// self-verifying: a missing or reordered event breaks the chain, so "100% of
// mutations are audited" becomes a checkable property rather than a claim.
#pragma once

#include <string>
#include <vector>

#include "risk/store.hpp"

namespace risk {

class AuditBuilder {
 public:
  AuditBuilder(TenantId tenant, DomainId domain, std::string component_version);

  AuditEvent begin(std::string event_type, std::string old_state_hash);

  // Fills in the fields a decision always has. Returns a copy so callers can
  // keep building without a mutable shared object.
  AuditEvent& with_command(AuditEvent& e, const CommandId& command_id,
                           const AccountId& account, const SymbolId& symbol);
  AuditEvent& with_reservation(AuditEvent& e, const ReservationId& id);
  AuditEvent& with_reason(AuditEvent& e, Reason reason, std::string note);

 private:
  TenantId tenant_;
  DomainId domain_;
  std::string component_version_;
};

struct AuditChainReport {
  bool ok = true;
  std::string detail;
  std::uint64_t events = 0;
  std::uint64_t mutations = 0;  // events whose state hash actually changed

  // Revisions seen, in order. Must be non-decreasing and gap-free from the
  // first observed revision.
  std::vector<Revision> revisions;
};

// Checks: ordinals start at 0 and increase within a revision; revisions never
// go backwards; and each event's `old_state_hash` equals the previous event's
// `new_state_hash`.
AuditChainReport verify_audit_chain(const std::vector<AuditEvent>& events);

// Every reservation that holds budget must be traceable to at least one audit
// event (RI-12: all transitions traceable).
Status verify_every_reservation_audited(const std::vector<Reservation>& reservations,
                                        const std::vector<AuditEvent>& events);

}  // namespace risk
