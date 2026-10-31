#include "llx/dispatch.hpp"

#include <sstream>

#include "llx/failpoint.hpp"

namespace llx {

std::string_view to_string(SendResult r) {
    switch (r) {
        case SendResult::Acked: return "ACKED";
        case SendResult::Failed: return "FAILED";
        case SendResult::Unknown: return "UNKNOWN";
    }
    return "UNKNOWN";
}

SendResult RecordingSink::send(const std::string& frame) {
    ++attempts_;
    LLX_FP(fp::point::kDispatchAfterWrite);
    const auto it = programmed_.find(attempts_);
    const SendResult r = (it != programmed_.end()) ? it->second : default_;
    // A frame that was partially written still reached the wire in part, so it
    // is recorded: the LP may have seen it. Not recording it would make the
    // test oracle weaker than reality.
    if (r != SendResult::Failed) frames_.push_back(frame);
    return r;
}

std::string Dispatcher::encode(const OutboxRecord& ob) {
    const char* msg_type = "D";
    if (ob.kind == 2) msg_type = "F";
    else if (ob.kind == 3) msg_type = "G";
    std::ostringstream o;
    o << "35=" << msg_type << "|11=" << ob.cl_ord_id;
    if (ob.orig_cl_ord_id != 0) o << "|41=" << ob.orig_cl_ord_id;
    o << "|55=" << ob.symbol << "|54=" << (ob.side == Side::Buy ? 1 : 2)
      << "|38=" << ob.qty << "|44=" << ob.px << "|outbox=" << ob.id;
    return o.str();
}

std::size_t Dispatcher::pump(std::uint32_t owner_epoch, std::size_t max) {
    std::size_t done = 0;
    while (done < max) {
        OutboxRecord ob;
        if (!store_.claim_next(owner_epoch, ob)) {
            // Either nothing is ready, or this dispatcher is fenced out. The
            // two are distinguished so a stale dispatcher is visible rather
            // than looking idle.
            if (owner_epoch < store_.fencing_epoch()) ++stats_.refused_stale_epoch;
            break;
        }
        ++stats_.claimed;

        // Exactly one original per ClOrdID. A record whose attempts already
        // exceed one arrived here after a restart or a requeue, and sending it
        // again as an original is precisely what LI-18 forbids.
        auto& already = originals_[ob.cl_ord_id];
        if (already > 0 || ob.attempts > 1) {
            ++stats_.duplicate_original_attempts;
            ++stats_.refused_ambiguous_on_restart;
            store_.mark_ambiguous(ob.id);
            ++stats_.ambiguous;
            ++done;
            continue;
        }

        LLX_FP(fp::point::kDispatchBeforeEncode);
        const std::string frame = encode(ob);
        LLX_FP(fp::point::kDispatchAfterEncode);

        // A partial write is indistinguishable from a complete one from here,
        // which is the whole of CW-22.
        SendResult r;
        if (LLX_FP(fp::point::kDispatchPartialWrite) == fp::Action::ReturnError) {
            r = SendResult::Unknown;
        } else {
            r = sink_.send(frame);
        }

        switch (r) {
            case SendResult::Acked:
                ++already;
                store_.mark_sent(ob.id);
                ++stats_.sent;
                break;
            case SendResult::Failed:
                // Definitively not sent. The record stays claimed and is not
                // marked sent; reconciliation decides whether to release it.
                // It is NOT returned to Ready here, because "definitively not
                // sent" is a claim about the local write, not about what the
                // LP saw, and only the reconciliation path can close that gap.
                store_.mark_ambiguous(ob.id);
                ++stats_.failed;
                ++stats_.ambiguous;
                break;
            case SendResult::Unknown:
                ++already;  // an original may be out there; never send a second
                store_.mark_ambiguous(ob.id);
                ++stats_.ambiguous;
                break;
        }
        ++done;
    }
    return done;
}

bool Dispatcher::reconcile(std::uint64_t outbox_id, bool lp_has_order) {
    for (const OutboxRecord& ob : store_.outbox_snapshot()) {
        if (ob.id != outbox_id) continue;
        if (ob.state != OutboxState::Ambiguous) return false;
        if (lp_has_order) {
            // Evidence says the LP has it: the original was effective, so the
            // row is settled as sent.
            return store_.mark_sent(outbox_id);
        }
        // Evidence says the LP never got it. Even here the dispatcher does not
        // mint a new command: re-offering the *same* durable record under the
        // same ClOrdID is the only safe retry, and that is a decision for the
        // operator-authorized path, not for this function.
        return false;
    }
    return false;
}

std::vector<OutboxRecord> Dispatcher::ambiguous_records() const {
    std::vector<OutboxRecord> out;
    for (const OutboxRecord& ob : store_.outbox_snapshot())
        if (ob.state == OutboxState::Ambiguous) out.push_back(ob);
    return out;
}

}  // namespace llx
