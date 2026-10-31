#include "gw/session_adapter.hpp"

#include <utility>
#include <vector>

#include "jrnl/sql.hpp"

namespace gw {
namespace {

// The highest receipt id already used for this session and epoch.
//
// The counter cannot simply start at 1 on every construction. persist_raw()
// inserts with ON CONFLICT(session_key,session_epoch,receipt_id) DO NOTHING, and
// undelivered() filters on acked=0, so an acknowledged receipt is invisible
// through the store's own API while its row is still there. Reusing its id
// would make the insert a silent no-op and lose an inbound message -- the exact
// shape of failure EI-45 is about, where an accepted event simply stops
// existing. Hence the direct read: P06 exposes db(), and this is what for.
std::uint64_t highest_receipt_id(jrnl::StableSendStore& store, const jrnl::SessionKey& session,
                                 std::uint64_t epoch) {
    jrnl::Stmt s;
    if (store.db().prepare("SELECT IFNULL(MAX(receipt_id),0) FROM inbound_raw WHERE "
                           "session_key=?1 AND session_epoch=?2;",
                           &s) != jrnl::StoreError::None) {
        return 0;
    }
    s.bind_text(1, session);
    s.bind_u64(2, epoch);
    bool row = false;
    if (s.step(&row) != jrnl::StoreError::None || !row) return 0;
    return s.col_u64(0);
}

}  // namespace

jrnl::Digest to_digest(const router::Hash& h) { return h.bytes; }

router::Hash to_router_hash(const jrnl::Digest& d) {
    router::Hash h;
    h.bytes = d;
    return h;
}

SessionAdapter::SessionAdapter(router::p01::SessionImage& image, router::FailpointRegistry& fp,
                               jrnl::StableSendStore& store, Options options)
    : router::p01::SessionEngine(image, fp), store_(store), options_(std::move(options)) {
    image_.epoch = store_.current_epoch(options_.session_key);
    image_.next_receipt = highest_receipt_id(store_, options_.session_key, image_.epoch) + 1;
}

router::p01::StableSendResult SessionAdapter::send_once(const router::CommandId& command_id,
                                                       const router::Hash& payload_hash,
                                                       const std::string& app_frame) {
    using State = router::p01::StableSendResult::State;
    router::p01::StableSendResult out;

    // NOT_READY has to mean "nothing was bound", so the question is asked
    // before the store is touched. Minting a sequence while the session is down
    // would spend an original sequence number on a message that cannot leave.
    if (!ready_for_app()) {
        out.state = State::NotReady;
        return out;
    }

    // Asked first, so the fresh-versus-repeat distinction is observed rather
    // than inferred from the binding that comes back: P06 returns the same
    // SendBinding either way, which is the whole point of issue-once.
    std::uint64_t prior_seq = 0;
    jrnl::Digest prior_hash{};
    const bool already_bound =
        store_.bound_sequence(options_.session_key, command_id.str(), &prior_seq, &prior_hash);

    const jrnl::SendBinding b = store_.send_once(options_.session_key, command_id.str(),
                                                 to_digest(payload_hash), app_frame);

    if (b.conflict) {
        // Same command id, different content. P04 calls this Fatal and it is:
        // one accepted command id yields one original intent (EI-09), so the
        // two payloads cannot both be it.
        out.state = State::Fatal;
        return out;
    }
    if (b.ambiguous) {
        // The sequence is durable; whether the bytes left is unknown. P06's
        // header is explicit that HOLD and reconciliation are the only safe
        // responses (JI-20), and P04's Unknown means the same thing.
        out.state = State::Unknown;
        if (b.original_seq != 0) out.original_seq = b.original_seq;
        return out;
    }
    if (!b.bound) {
        out.state = State::NotReady;
        return out;
    }

    out.original_seq = b.original_seq;
    out.state = already_bound ? State::AlreadyAssigned : State::AssignedOriginalSeq;

    // Keep the image coherent for the parts of P04 that read it. `written` is
    // set because the store called its wire callback; see the note in
    // lookup_binding() for why that is the conservative value.
    router::p01::SessionImage::Binding& binding = image_.bindings[command_id.str()];
    binding.original_seq = b.original_seq;
    binding.epoch = image_.epoch;
    binding.payload_hash = payload_hash;
    binding.frame = app_frame;
    binding.written = true;
    if (b.original_seq >= image_.next_out) image_.next_out = b.original_seq + 1;
    return out;
}

std::optional<router::p01::SessionImage::Binding> SessionAdapter::lookup_binding(
    const router::CommandId& command_id) const {
    std::uint64_t seq = 0;
    jrnl::Digest hash{};
    if (!store_.bound_sequence(options_.session_key, command_id.str(), &seq, &hash)) {
        return std::nullopt;
    }

    router::p01::SessionImage::Binding b;
    b.original_seq = seq;
    b.epoch = store_.current_epoch(options_.session_key);
    b.payload_hash = to_router_hash(hash);
    // P06 answers which sequence is bound, not which bytes were stored, so
    // there is no frame to hand back and no digest of one.
    b.frame.clear();

    // `written` is the load-bearing field: P04 reads it as UnknownTransport when
    // true and AcceptedDurable -- still safe to dispatch -- when false. P06 can
    // count unconfirmed sends for a session but cannot say whether *this* key's
    // bytes were confirmed, so the honest answer is "assume they were". A bound
    // sequence whose write status is unknown is precisely UNKNOWN_OUTCOME, and
    // EI-34 forbids resending it; returning false here would invite the second
    // original send that EI-09 rules out.
    b.written = true;
    return b;
}

std::optional<router::p01::DurableFixInbound> SessionAdapter::peek_inbound() const {
    const std::vector<jrnl::RawReceipt> pending = store_.undelivered(options_.session_key, 1);
    if (pending.empty()) return std::nullopt;
    const jrnl::RawReceipt& r = pending.front();

    router::p01::DurableFixInbound out;
    out.session_key = r.session_key;
    out.epoch = r.session_epoch;
    out.durable_inbox_id = r.receipt_id;
    out.frame = r.raw;
    out.raw_sha256 = to_router_hash(r.raw_hash);
    // P06's receipt carries no FIX sequence number: the sequence belongs to
    // P01's own store, and P06 persists the raw bytes before anyone has parsed
    // them. P04 does not read this field on the inbound path -- it correlates on
    // durable_inbox_id -- so it is left at zero rather than filled with
    // something that would look like a sequence and not be one.
    out.msg_seq_num = 0;
    return out;
}

void SessionAdapter::ack_inbound_after_commit(router::DurableReceiptId id) {
    // Only reached after P04 committed its own transaction, which is the
    // ordering the name states and the reason undelivered() filters on acked
    // rather than on delivered.
    store_.ack_delivery(options_.session_key, image_.epoch, id);
}

std::size_t SessionAdapter::pending_inbound() const {
    return store_.unacked_count(options_.session_key);
}

router::DurableReceiptId SessionAdapter::deliver_inbound(const std::string& frame) {
    jrnl::RawReceipt r;
    r.session_key = options_.session_key;
    r.session_epoch = image_.epoch;
    r.receipt_id = image_.next_receipt++;
    r.raw = frame;
    r.raw_hash = jrnl::sha256(frame);
    if (store_.persist_raw(r) != jrnl::StoreError::None) {
        // Nothing durable, so nothing to hand out. Returning an id for a receipt
        // that does not exist would have P04 acknowledge a message the store
        // never took.
        --image_.next_receipt;
        return 0;
    }
    return r.receipt_id;
}

void SessionAdapter::redeliver(router::DurableReceiptId id) {
    // Nothing to do, and that is the semantics rather than a gap: undelivered()
    // keeps returning every receipt whose acked flag is clear, so an
    // unacknowledged message is already redelivered on each peek. P04's in-tree
    // engine needs an explicit nudge because it tracks delivery itself.
    (void)id;
}

void SessionAdapter::disconnect() {
    image_.state = router::p01::SessionState::Disconnected;
    // The epoch is untouched. A disconnect is not evidence about anything in
    // flight (EI-04, EI-25), and bumping the epoch here would orphan the
    // receipts of the session that just dropped.
}

void SessionAdapter::reconnect(bool reset_sequences) {
    if (reset_sequences) {
        std::uint64_t now = 0;
        if (store_.bump_epoch(options_.session_key, &now) == jrnl::StoreError::None) {
            image_.epoch = now;
            // Receipt ids are unique within an epoch, so the counter restarts
            // with it -- from whatever the new epoch already holds, which is
            // nothing on a fresh bump but is read rather than assumed.
            image_.next_receipt = highest_receipt_id(store_, options_.session_key, now) + 1;
        }
        image_.next_out = 1;
        image_.next_in = 1;
        image_.bindings.clear();
    }
    image_.state = router::p01::SessionState::Active;
    image_.owner_generation = generation_;
}

std::size_t SessionAdapter::unconfirmed_sends() const {
    return store_.unconfirmed_sends(options_.session_key);
}

std::uint64_t SessionAdapter::allocated_sequences() const {
    return store_.allocated_sequences(options_.session_key);
}

}  // namespace gw
