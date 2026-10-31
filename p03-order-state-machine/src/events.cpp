#include "order_core/events.hpp"

namespace order_core {

const char* to_string(Side s) {
    switch (s) {
        case Side::Unspecified: return "UNSPECIFIED";
        case Side::Buy: return "BUY";
        case Side::Sell: return "SELL";
    }
    return "UNSPECIFIED";
}

Side side_from_fix(const std::string& raw) {
    if (raw == "1") return Side::Buy;
    if (raw == "2") return Side::Sell;
    return Side::Unspecified;
}

const char* to_string(CommandKind k) {
    switch (k) {
        case CommandKind::New: return "NEW";
        case CommandKind::Cancel: return "CANCEL";
        case CommandKind::Replace: return "REPLACE";
    }
    return "UNKNOWN";
}

const char* to_string(SystemKind k) {
    switch (k) {
        case SystemKind::Dispatched: return "DISPATCHED";
        case SystemKind::AckTimeout: return "ACK_TIMEOUT";
        case SystemKind::TransportDisconnected: return "TRANSPORT_DISCONNECTED";
        case SystemKind::ReconcileStarted: return "RECONCILE_STARTED";
        case SystemKind::ReconcileResolved: return "RECONCILE_RESOLVED";
        case SystemKind::StorageFailed: return "STORAGE_FAILED";
        case SystemKind::ManualOverride: return "MANUAL_OVERRIDE";
    }
    return "UNKNOWN";
}

int ord_status_precedence(const std::string& status) {
    if (status == ord_status::kPendingCancel) return 11;
    if (status == ord_status::kPendingReplace) return 10;
    if (status == ord_status::kDoneForDay) return 9;
    if (status == ord_status::kCalculated) return 8;
    if (status == ord_status::kFilled) return 7;
    if (status == ord_status::kStopped) return 6;
    if (status == ord_status::kSuspended) return 5;
    if (status == ord_status::kCanceled || status == ord_status::kExpired) return 4;
    if (status == ord_status::kPartiallyFilled) return 3;
    if (status == ord_status::kNew || status == ord_status::kRejected ||
        status == ord_status::kPendingNew) {
        return 2;
    }
    if (status == ord_status::kAcceptedForBidding) return 1;
    return 0;  // unknown or obsolete: no precedence, handled by policy
}

EventClass classify(const OrderEvent& e) {
    if (std::holds_alternative<LocalCommand>(e)) return EventClass::LocalCommand;
    if (std::holds_alternative<PeerReport>(e)) return EventClass::PeerReport;
    return EventClass::SystemEvent;
}

namespace {

// Length-prefixed so "AB"+"C" and "A"+"BC" cannot collide, and so an absent
// optional is distinguishable from an empty string.
void feed(std::string& buf, const std::string& field) {
    buf += std::to_string(field.size());
    buf.push_back(':');
    buf += field;
    buf.push_back('\x1f');
}

void feed(std::string& buf, const std::optional<std::string>& field) {
    if (!field.has_value()) {
        buf += "-\x1f";
        return;
    }
    feed(buf, *field);
}

}  // namespace

uint64_t business_fingerprint(const PeerReport& r) {
    std::string buf;
    buf.push_back(r.msg_type);
    buf.push_back('\x1f');
    feed(buf, r.exec_type);
    feed(buf, r.ord_status);
    feed(buf, r.exec_id);
    feed(buf, r.order_id);
    feed(buf, r.cl_ord_id);
    feed(buf, r.orig_cl_ord_id);
    feed(buf, r.exec_ref_id);
    feed(buf, r.account);
    feed(buf, r.symbol);
    feed(buf, r.side_raw);
    feed(buf, r.last_qty);
    feed(buf, r.last_px);
    feed(buf, r.cum_qty);
    feed(buf, r.leaves_qty);
    feed(buf, r.order_qty);
    feed(buf, r.price);
    feed(buf, r.avg_px);
    feed(buf, r.cxl_rej_response_to);
    feed(buf, r.cxl_rej_reason);
    feed(buf, r.peer_id);
    // Deliberately excluded: session_id, session_epoch, source_seq, poss_dup,
    // poss_resend, sending_time, text, reconciliation_generation. These are
    // transport or observational, and a resend changes them while describing
    // the same execution.
    return fnv1a(buf);
}

uint64_t intent_fingerprint(const LocalCommand& c) {
    std::string buf;
    feed(buf, c.firm);
    feed(buf, c.account);
    feed(buf, c.symbol);
    feed(buf, c.side_raw);
    feed(buf, c.qty_text);
    feed(buf, c.price_text);
    feed(buf, c.tif);
    // business_day, ClOrdID and OrigClOrdID are deliberately absent.
    return fnv1a(buf);
}

uint64_t business_fingerprint(const LocalCommand& c) {
    std::string buf;
    buf += to_string(c.kind);
    buf.push_back('\x1f');
    feed(buf, c.firm);
    feed(buf, c.business_day);
    feed(buf, c.account);
    feed(buf, c.symbol);
    feed(buf, c.side_raw);
    feed(buf, c.cl_ord_id);
    feed(buf, c.orig_cl_ord_id);
    feed(buf, c.qty_text);
    feed(buf, c.price_text);
    feed(buf, c.tif);
    // command_seq excluded: a producer retrying the same intent must land on
    // the same fingerprint.
    return fnv1a(buf);
}

}  // namespace order_core
