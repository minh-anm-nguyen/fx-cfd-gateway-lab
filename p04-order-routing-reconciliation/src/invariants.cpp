#include "router/invariants.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace router {
namespace {

void add(std::vector<Violation>& out, const char* id, std::string detail, std::string subject) {
    out.push_back(Violation{id, std::move(detail), std::move(subject)});
}

// Stands in for the command table when the caller asked to skip it, so the
// loops below read the same either way.
const std::map<std::string, CommandIntent> kNoIntents{};

}  // namespace

std::vector<Violation> check_invariants(const InvariantInputs& in) {
    std::vector<Violation> v;
    const StoreImage& img = in.store;
    const bool scan_commands = !in.skip_command_tables;

    // R-I01 command identity: one canonical intent per (tenant, command_id).
    // The map key guarantees uniqueness, so what is checked here is that the
    // key actually matches the record it holds.
    for (const auto& [key, intent] : (scan_commands ? img.intents : kNoIntents)) {
        if (key != intent.key())
            add(v, "R-I01", "intent stored under a key that is not its own", key);
        if (intent.request_hash.is_zero() && intent.kind != CommandKind::StatusQuery)
            add(v, "R-I01", "admitted command without a canonical request digest", key);
    }

    // R-I02 single route: every intent on a chain names the same LP, and the
    // chain agrees.
    {
        std::map<std::string, std::string> chain_lp;
        for (const auto& [key, intent] : (scan_commands ? img.intents : kNoIntents)) {
            auto it = chain_lp.find(intent.chain_id.str());
            if (it == chain_lp.end())
                chain_lp[intent.chain_id.str()] = intent.lp.str();
            else if (it->second != intent.lp.str())
                add(v, "R-I02", "two LPs for one chain", key);
        }
        for (const auto& [id, chain] : img.chains) {
            auto it = chain_lp.find(id);
            if (it != chain_lp.end() && it->second != chain.lp.str())
                add(v, "R-I02", "chain projection names a different LP than its commands", id);
        }
    }

    // R-I03 no implicit success: a business outcome only exists where a business
    // report has actually been applied to the chain.
    for (const auto& [key, intent] : (scan_commands ? img.intents : kNoIntents)) {
        if (intent.business == BusinessOutcome::Unknown) continue;
        auto chain = img.chains.find(intent.chain_id.str());
        if (chain == img.chains.end()) {
            add(v, "R-I03", "business outcome without a chain projection", key);
            continue;
        }
        if (chain->second.local_only)
            add(v, "R-I03", "business outcome claimed with no LP evidence at all", key);
    }

    // R-I04 stable ClOrdID: unique per (lp, ClOrdID) and never reused for a
    // different intent, and a request never reuses the id it refers to.
    {
        std::map<std::string, std::string> seen;
        for (const auto& [key, intent] : (scan_commands ? img.intents : kNoIntents)) {
            if (intent.kind == CommandKind::StatusQuery) continue;
            const std::string k = intent.lp.str() + "|" + intent.cl_ord_id.str();
            auto it = seen.find(k);
            if (it != seen.end() && it->second != key)
                add(v, "R-I04", "ClOrdID shared by two intents", k);
            seen[k] = key;
            if (intent.orig_cl_ord_id && *intent.orig_cl_ord_id == intent.cl_ord_id)
                add(v, "R-I04", "request reuses the ClOrdID it refers to", key);
        }
    }

    // R-I05 accepted lineage: a pending replace is never already the accepted
    // version, and a pending cancel never is either.
    for (const auto& [id, chain] : img.chains) {
        if (chain.pending_replace && *chain.pending_replace == chain.accepted_cl_ord_id)
            add(v, "R-I05", "pending replace id is already the accepted version", id);
        if (chain.pending_cancel && *chain.pending_cancel == chain.accepted_cl_ord_id)
            add(v, "R-I05", "pending cancel id is the accepted version", id);
    }

    // R-I06 no double original send: one binding per command, and no two
    // commands bound to the same original sequence.
    {
        // Scoped by epoch: a bilateral sequence reset makes MsgSeqNum repeat
        // legitimately, so only a collision inside one epoch is a double send.
        std::map<std::string, std::string> by_seq;
        for (const auto& [cmd, binding] : in.session.bindings) {
            const std::string k =
                std::to_string(binding.epoch) + "|" + std::to_string(binding.original_seq);
            auto it = by_seq.find(k);
            if (it != by_seq.end() && it->second != cmd)
                add(v, "R-I06", "two commands share one original sequence in one epoch", cmd);
            by_seq[k] = cmd;
        }
        // A chain must never have two New commands: that would be a business
        // resubmit, which is exactly what R-I06 and R-I16 forbid.
        std::map<std::string, int> new_per_chain;
        for (const auto& [key, intent] : (scan_commands ? img.intents : kNoIntents)) {
            (void)key;
            if (intent.kind == CommandKind::New) ++new_per_chain[intent.chain_id.str()];
        }
        for (const auto& [chain, n] : new_per_chain)
            if (n > 1) add(v, "R-I06", "more than one NewOrderSingle intent for a chain", chain);
    }

    // R-I07 dedup: a fill appears once per chain, and every applied fill has a
    // business dedup key behind it.
    for (const auto& [id, chain] : img.chains) {
        std::set<std::string> exec_ids;
        for (const p03::Fill& f : chain.fills) {
            if (!exec_ids.insert(f.exec_id.str()).second)
                add(v, "R-I07", "the same ExecID appears twice in one fill ledger", id);
            const std::string k = chain.lp.str() + "|" + f.exec_id.str();
            if (img.exec_keys.find(k) == img.exec_keys.end())
                add(v, "R-I07", "applied fill without a persisted dedup key", k);
        }
    }

    // R-I08 ExecID policy: the status snapshot sentinel never enters the
    // business uniqueness index.
    for (const auto& [key, ek] : img.exec_keys) {
        if (ek.semantic_exec_id == "0" || ek.semantic_exec_id.empty())
            add(v, "R-I08", "status snapshot ExecID used as a business dedup key", key);
        if (ek.payload_fingerprint.is_zero())
            add(v, "R-I08", "dedup key without a content fingerprint", key);
    }

    // R-I09 no synthetic fill: every fill has a positive quantity, a price and
    // an execution identity. A fill with no ExecID could only have been
    // invented.
    for (const auto& [id, chain] : img.chains) {
        for (const p03::Fill& f : chain.fills) {
            if (f.exec_id.empty()) add(v, "R-I09", "fill without an ExecID", id);
            if (f.qty <= 0) add(v, "R-I09", "fill with a non-positive quantity", id);
            if (f.px <= 0) add(v, "R-I09", "fill with a non-positive price", id);
        }
    }

    // R-I10 no overfill by the router: the ledger arithmetic has to close, and
    // an overfilled chain must be held rather than clamped.
    for (const auto& [id, chain] : img.chains) {
        std::int64_t sum = 0;
        for (const p03::Fill& f : chain.fills) sum += f.qty;
        if (sum != chain.cum_qty)
            add(v, "R-I10", "CumQty does not equal the sum of the fill ledger", id);
        const std::int64_t expected_leaves =
            chain.accepted_qty > chain.cum_qty ? chain.accepted_qty - chain.cum_qty : 0;
        if (!chain.terminal() && chain.leaves_qty != expected_leaves)
            add(v, "R-I10", "LeavesQty is not accepted quantity minus CumQty", id);
        if (chain.cum_qty > chain.accepted_qty && !chain.hold.has_value())
            add(v, "R-I10", "CumQty exceeds the accepted quantity with no hold", id);
    }

    // R-I11 no false cancel: a cancelled chain has an actual Canceled report
    // behind it in the journal.
    for (const auto& [id, chain] : img.chains) {
        if (chain.status != OrdStatus::Canceled) continue;
        if (!chain.cancel_proven)
            add(v, "R-I11", "chain marked Canceled with no Canceled report applied", id);
        if (chain.leaves_qty != 0)
            add(v, "R-I11", "cancelled chain still reports remaining quantity", id);
    }

    // R-I12 no false replace: while a replace is pending, the accepted version
    // is still the one in force. Comparing the parameters is not the test --
    // a replace may legitimately restate the same price and quantity, and the
    // random walk does exactly that -- so what is checked is observable: the
    // pending identifier has not become the accepted one, and the remainder is
    // still derived from the accepted quantity rather than the proposed one.
    for (const auto& [id, chain] : img.chains) {
        if (!chain.pending_replace) continue;
        if (*chain.pending_replace == chain.accepted_cl_ord_id)
            add(v, "R-I12", "pending replace identifier became the accepted version", id);
        if (chain.terminal()) continue;
        if (chain.pending_replace_qty == chain.accepted_qty) continue;
        const std::int64_t from_accepted =
            chain.accepted_qty > chain.cum_qty ? chain.accepted_qty - chain.cum_qty : 0;
        if (chain.leaves_qty != from_accepted)
            add(v, "R-I12",
                "remainder follows the proposed replace quantity, not the accepted one", id);
    }

    // R-I13 order identity consistency: one LP OrderID belongs to one chain.
    {
        std::map<std::string, std::string> by_order_id;
        for (const auto& [id, chain] : img.chains) {
            if (!chain.lp_order_id) continue;
            const std::string k = chain.lp.str() + "|" + chain.lp_order_id->str();
            auto it = by_order_id.find(k);
            if (it != by_order_id.end() && it->second != id)
                add(v, "R-I13", "one OrderID bound to two chains", k);
            by_order_id[k] = id;
            if (chain.lp_order_id->str() == "NONE")
                add(v, "R-I13", "placeholder NONE bound as a real OrderID", id);
        }
    }

    // R-I14 input provenance: every applied event kept its origin.
    for (std::size_t ji = std::min(in.journal_from, img.journal.size());
         ji < img.journal.size(); ++ji) {
        const JournalRecord& rec = img.journal[ji];
        if (rec.kind != JournalRecord::Kind::ReportApplied || !rec.report) continue;
        const NormalizedReport& r = *rec.report;
        if (r.session_key.empty()) add(v, "R-I14", "applied event without a session identity",
                                       r.chain_id.str());
        if (r.raw_frame_hash.is_zero())
            add(v, "R-I14", "applied event without a raw frame digest", r.chain_id.str());
        if (r.source_receipt == 0)
            add(v, "R-I14", "applied event without a durable receipt id", r.chain_id.str());
        if (r.parser_version.empty() || r.profile_version.empty())
            add(v, "R-I14", "applied event without parser or profile version", r.chain_id.str());
    }

    // R-I15 atomic apply boundary: a dedup key never exists without the chain it
    // belongs to, and the audit log is intact.
    for (const auto& [key, ek] : img.exec_keys) {
        if (img.chains.find(ek.chain_id.str()) == img.chains.end())
            add(v, "R-I15", "dedup key for a chain that does not exist", key);
    }
    if (!img.audit.verify_from(in.audit_from))
        add(v, "R-I15", "audit hash chain does not verify", "audit");

    // R-I16 ambiguity explicit: an unknown command keeps its identifier and has
    // a case open; nothing was re-minted.
    for (const auto& [key, intent] : (scan_commands ? img.intents : kNoIntents)) {
        if (intent.delivery != DeliveryState::UnknownTransport) continue;
        bool has_case = false;
        for (const auto& [k, c] : img.recon_cases) {
            (void)k;
            if (c.chain_id == intent.chain_id) has_case = true;
        }
        if (!has_case)
            add(v, "R-I16", "unknown transport outcome with no reconciliation case", key);
    }

    // R-I17 evidence based reconciliation: a resolved case names what resolved
    // it and carries the digest of the evidence.
    for (const auto& [key, c] : img.recon_cases) {
        if (c.state != ReconState::Reconciled) continue;
        if (c.resolved_by.empty())
            add(v, "R-I17", "case resolved without naming who or what resolved it", key);
        if (c.last_evidence_hash.is_zero())
            add(v, "R-I17", "case resolved without an evidence digest", key);
    }

    // R-I18 no cross tenant leak: a chain's account belongs to the tenant that
    // owns the commands on it.
    for (const auto& [id, chain] : img.chains) {
        if (!in.cfg.account_belongs_to(chain.tenant, chain.account))
            add(v, "R-I18", "chain account is not owned by its tenant", id);
    }

    // R-I19 bounded failure: no bound is exceeded.
    {
        std::size_t pending = 0;
        for (const auto& [id, row] : img.outbox) {
            (void)id;
            if (row.state == OutboxState::Pending || row.state == OutboxState::Claimed) ++pending;
        }
        if (pending > in.cfg.limits.global_command_queue)
            add(v, "R-I19", "outbox above the configured global bound", std::to_string(pending));
        std::size_t unresolved = 0;
        for (const QuarantineEntry& q : img.quarantine)
            if (!q.resolved) ++unresolved;
        if (unresolved > in.cfg.limits.orphan_quota)
            add(v, "R-I19", "quarantine above the configured quota", std::to_string(unresolved));
        if (in.status_probes_in_flight > in.cfg.limits.status_queries_in_flight)
            add(v, "R-I19", "more status probes in flight than permitted",
                std::to_string(in.status_probes_in_flight));
    }

    // R-I20 fail closed on durability: nothing is accepted-durable without the
    // bytes that make it replayable.
    for (const auto& [key, intent] : (scan_commands ? img.intents : kNoIntents)) {
        if (intent.kind == CommandKind::StatusQuery) continue;
        const bool past_admission = intent.delivery != DeliveryState::Received &&
                                    intent.delivery != DeliveryState::RejectedLocal;
        if (past_admission && (intent.fix_body.empty() || intent.payload_hash.is_zero()))
            add(v, "R-I20", "durable command without an immutable payload", key);
    }

    // R-I21 source order preservation: the fill ledger is in application order
    // with no gaps.
    for (const auto& [id, chain] : img.chains) {
        std::uint64_t expected = 1;
        for (const p03::Fill& f : chain.fills) {
            if (f.applied_seq != expected)
                add(v, "R-I21", "fill ledger is not in application order", id);
            ++expected;
        }
    }

    // R-I22 operator changes auditable.
    for (std::size_t ai = std::min(in.audit_from, img.audit.records().size());
         ai < img.audit.records().size(); ++ai) {
        const AuditRecord& rec = img.audit.records()[ai];
        if (rec.kind != AuditKind::OperatorAction) continue;
        if (rec.actor.empty() || rec.actor == "system")
            add(v, "R-I22", "operator action without an identified actor",
                std::to_string(rec.seq));
        if (rec.reason.empty())
            add(v, "R-I22", "operator action without a reason", std::to_string(rec.seq));
    }

    // R-I23 consistent time semantics: every record carries both clocks.
    for (const auto& [key, intent] : (scan_commands ? img.intents : kNoIntents)) {
        if (intent.created_wall == 0)
            add(v, "R-I23", "command without a UTC creation timestamp", key);
    }

    // R-I24 no irreversible automation on contradiction: a held chain has a case
    // or a quarantine row attached.
    for (const auto& [id, chain] : img.chains) {
        if (!chain.hold) continue;
        bool tracked = false;
        for (const auto& [k, c] : img.recon_cases) {
            (void)k;
            if (c.chain_id.str() == id) tracked = true;
        }
        for (const QuarantineEntry& q : img.quarantine)
            if (q.chain_hint && q.chain_hint->str() == id) tracked = true;
        if (!tracked) add(v, "R-I24", "held chain with no case and no quarantine record", id);
    }

    // R-I25 outbox and inbox non-loss: every quarantined event kept its digest
    // and is marked consumed exactly once.
    {
        std::set<std::string> receipts;
        for (std::size_t qi = std::min(in.quarantine_from, img.quarantine.size());
             qi < img.quarantine.size(); ++qi) {
            const QuarantineEntry& q = img.quarantine[qi];
            if (q.raw_hash.is_zero())
                add(v, "R-I25", "quarantined event without a raw digest",
                    std::to_string(q.receipt_id));
            const std::string k =
                q.session_key + "|" + std::to_string(q.epoch) + "|" + std::to_string(q.receipt_id);
            if (!receipts.insert(k).second)
                add(v, "R-I25", "the same receipt quarantined twice", k);
            if (img.applied_receipts.find(k) == img.applied_receipts.end())
                add(v, "R-I25", "quarantined event never marked consumed", k);
        }
    }

    // R-I26 capacity isolation: one chain cannot monopolise the outbox.
    {
        std::map<std::string, std::size_t> per_chain;
        for (const auto& [id, row] : img.outbox) {
            (void)id;
            if (row.state != OutboxState::Pending && row.state != OutboxState::Claimed) continue;
            const std::string key = row.tenant.str() + "|" + row.command_id.str();
            auto it = img.intents.find(key);
            if (it == img.intents.end()) continue;
            ++per_chain[it->second.chain_id.str()];
        }
        for (const auto& [chain, n] : per_chain)
            if (n > in.cfg.limits.per_chain_mailbox)
                add(v, "R-I26", "per-chain mailbox bound exceeded", chain);
    }

    return v;
}

std::string render_violations(const std::vector<Violation>& v) {
    std::string out;
    for (const Violation& x : v) {
        if (!out.empty()) out += "; ";
        out += x.invariant + " " + x.detail + " [" + x.subject + "]";
    }
    return out;
}

}  // namespace router
