#include "risk/store.hpp"

#include "risk/hash.hpp"

namespace risk {

std::string_view commit_outcome_name(CommitOutcome o) noexcept {
  switch (o) {
    case CommitOutcome::Committed: return "COMMITTED";
    case CommitOutcome::RolledBack: return "ROLLED_BACK";
    case CommitOutcome::UnknownButDurable: return "UNKNOWN_BUT_DURABLE";
    case CommitOutcome::Fenced: return "FENCED";
  }
  return "UNKNOWN_OUTCOME";
}

std::string RiskDomainState::digest() const {
  Hasher h;
  h.named("tenant", tenant_id)
      .named("domain", domain_id)
      .named("revision", revision)
      .named("fencing_token", fencing_token)
      .named("policy_version", policy_version)
      .named("mode", domain_mode_name(mode))
      .named("quarantined", quarantined ? std::string("1") : std::string("0"))
      .named("admission_open", admission_open ? std::string("1") : std::string("0"));
  return h.hex();
}

std::string AuditEvent::digest() const {
  Hasher h;
  h.named("tenant", tenant_id)
      .named("domain", domain_id)
      .named("commit_revision", commit_revision)
      .named("event_ordinal", static_cast<std::int64_t>(event_ordinal))
      .named("event_type", event_type)
      .named("command_id", command_id)
      .named("reservation_id", reservation_id)
      .named("account_id", account_id)
      .named("symbol", symbol)
      .named("source_event_id", source_event_id)
      .named("policy_version", policy_version)
      .named("snapshots", snapshots.digest())
      .named("payload_hash", payload_hash)
      .named("old_state_hash", old_state_hash)
      .named("new_state_hash", new_state_hash)
      .named("reason", reason_name(reason))
      .named("actor", actor)
      .named("note", note);
  return h.hex();
}

bool TransactionData::empty() const {
  return commands.empty() && reservations.empty() && applied_events.empty() &&
         audit.empty() && outbox_inserts.empty() && outbox_updates.empty() &&
         !exposure.has_value() && !policy.has_value() && !mode.has_value() &&
         !quarantined.has_value() && !admission_open.has_value() &&
         chain_index.empty() && account_quarantine.empty() &&
         !tenant_id.has_value() && !domain_id.has_value();
}

std::string chain_key(const TenantId& tenant, const AccountId& account,
                      const OrderChainId& chain) {
  Hasher h;
  h.field(tenant).field(account).field(chain);
  return h.hex();
}

std::string command_key(const TenantId& tenant, const AccountId& account,
                        const CommandId& command) {
  Hasher h;
  h.field(tenant).field(account).field(command);
  return h.hex();
}

std::string event_key(const TenantId& tenant, const std::string& ns,
                      const EventId& event) {
  Hasher h;
  h.field(tenant).field(ns).field(event);
  return h.hex();
}

std::string RiskStore::state_hash() const {
  Hasher h;
  h.field("risk_store_state_v1");
  h.field(domain_state().digest());
  h.field(exposure().state_hash());
  h.field(policy().hash());

  // Reservations are hashed in a deterministic order; the store returns them
  // sorted by id so an in-memory and a replayed store agree.
  h.field("reservations");
  for (const Reservation& r : reservations()) h.field(r.digest());

  h.field("quarantined_accounts");
  for (const AccountId& a : quarantined_accounts()) h.field(a);

  h.field("outbox");
  for (const OutboxIntent& o : outbox()) {
    Hasher oh;
    oh.named("intent_id", o.intent_id)
        .named("tenant", o.tenant_id)
        .named("account", o.account_id)
        .named("chain", o.order_chain_id)
        .named("command", o.command_id)
        .named("reservation", o.reservation_id)
        .named("kind", o.kind)
        .named("symbol", o.symbol)
        .named("side", side_name(o.side))
        .named("total_units", o.total_units)
        .named("limit_price_ticks", o.limit_price_ticks)
        .named("commit_revision", o.commit_revision)
        .named("canonical_hash", o.canonical_hash)
        .named("claimed", o.claimed ? std::string("1") : std::string("0"))
        .named("dispatched", o.dispatched ? std::string("1") : std::string("0"));
    h.field(oh.hex());
  }

  return h.hex();
}

}  // namespace risk
