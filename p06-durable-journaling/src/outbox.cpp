#include "jrnl/outbox.hpp"

#include "jrnl/failpoint.hpp"

namespace jrnl {

DispatchStats OutboxDispatcher::dispatch_once(const TenantId& tenant, FenceToken token) {
    DispatchStats st;

    int released = 0;
    store_.release_expired_claims(tenant, store_.clock().now_ms(), &released);
    st.released_expired = static_cast<std::size_t>(released);

    std::vector<OutboxRow> claimed;
    if (store_.claim_outbox(tenant, opt_.owner, token, opt_.claim_limit, opt_.lease_ms,
                            &claimed) != StoreError::None) {
        return st;
    }
    st.claimed = claimed.size();

    for (const OutboxRow& row : claimed) {
        // Record the intent to call P01, in its own committed write, before
        // the call. Everything the recovery policy can say about a crash
        // after this point depends on this row being durable first.
        if (store_.mark_send_attempted(tenant, row.outbox_id, token) != StoreError::None) {
            ++st.skipped_not_owner;
            continue;
        }

        if (JRNL_FP(fp::point::kBeforeStableSend) == fp::Action::ReturnError) {
            store_.set_outbox_state(tenant, row.outbox_id, "UNKNOWN", token, std::nullopt,
                                    "dispatcher failed before calling the session");
            ++st.unknown;
            continue;
        }

        const SendBinding b =
            link_.send_once(row.session_key, row.command_id, row.payload_hash, row.payload);

        if (b.conflict) {
            // The key is bound to different bytes at P01. Sending anything
            // now would be issuing a second, different order.
            store_.set_outbox_state(tenant, row.outbox_id, "HELD", token, std::nullopt,
                                    "stable send key conflict: " + b.detail);
            ++st.held;
            continue;
        }

        if (b.ambiguous) {
            // JI-20. There is no automatic path from here to a resend: a new
            // ClOrdID would be a second order and a repeat of the same one
            // cannot be shown to be safe without the peer's own evidence.
            const char* next = opt_.safe_retry_requires_stable_send ? "UNKNOWN" : "READY";
            store_.set_outbox_state(tenant, row.outbox_id, next, token,
                                    b.bound ? std::optional<std::uint64_t>(b.original_seq)
                                            : std::nullopt,
                                    "ambiguous send: " + b.detail);
            ++st.unknown;
            continue;
        }

        if (!b.bound) {
            store_.set_outbox_state(tenant, row.outbox_id, "UNKNOWN", token, std::nullopt,
                                    "session did not bind a sequence");
            ++st.unknown;
            continue;
        }

        if (JRNL_FP(fp::point::kBeforeBindingRecorded) == fp::Action::ReturnError) {
            // The sequence is durable at P01 but our row still says CLAIMED.
            // Recovery must read P01's mapping, not guess.
            ++st.unknown;
            continue;
        }

        // BOUND means one thing only: P01 has durably bound this command to
        // an original sequence number. JI-16 and JI-17 forbid reading it as
        // delivery or as acceptance by the liquidity provider.
        const StoreError e = store_.set_outbox_state(tenant, row.outbox_id, "BOUND", token,
                                                     b.original_seq, "");
        if (e == StoreError::NotOwner) {
            ++st.skipped_not_owner;
            continue;
        }
        ++st.bound;
    }
    return st;
}

std::vector<OutboxRow> OutboxDispatcher::unresolved(const TenantId& tenant) const {
    std::vector<OutboxRow> out;
    for (const char* state : {"UNKNOWN", "HELD"}) {
        for (OutboxRow& r : store_.outbox_rows(tenant, state)) out.push_back(std::move(r));
    }
    return out;
}

std::size_t OutboxDispatcher::reconcile_unknown(const TenantId& tenant, StableSendStore& link) {
    std::size_t resolved = 0;
    for (const OutboxRow& r : store_.outbox_rows(tenant, "UNKNOWN")) {
        std::uint64_t seq = 0;
        Digest hash{};
        if (!link.bound_sequence(r.session_key, r.command_id, &seq, &hash)) {
            // No sequence was ever bound, so nothing can have gone out under
            // this key. The row is safe to dispatch again with the same key.
            store_.set_outbox_state(tenant, r.outbox_id, "READY", 0, std::nullopt,
                                    "reconciled: no original sequence was ever bound");
            ++resolved;
            continue;
        }
        if (!(hash == r.payload_hash)) {
            store_.set_outbox_state(tenant, r.outbox_id, "HELD", 0, seq,
                                    "reconciled: bound sequence carries a different payload");
            ++resolved;
            continue;
        }
        // A sequence exists. Whether the peer received or acted on it is not
        // knowable from our side, so this becomes a held case for
        // reconciliation against the counterparty's own history (JI-40).
        store_.set_outbox_state(tenant, r.outbox_id, "HELD", 0, seq,
                                "reconciled: original sequence " + std::to_string(seq) +
                                    " is bound; counterparty evidence required");
        ++resolved;
    }
    return resolved;
}

}  // namespace jrnl
