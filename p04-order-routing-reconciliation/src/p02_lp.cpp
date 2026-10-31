#include "deps/p02_lp.hpp"

#include <cstdio>

namespace router::p02 {
namespace {

constexpr char kSoh = '\x01';

// The simulator's own decimal renderer. Not shared with router::format_decimal_ticks
// on purpose: if both sides had the same formatting bug, the golden fixture
// tests would agree with each other and still be wrong (design doc 22.1).
std::string decimal(std::int64_t ticks, int scale) {
    char buf[64];
    if (scale <= 0) {
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(ticks));
        return buf;
    }
    const bool neg = ticks < 0;
    unsigned long long mag =
        neg ? (~static_cast<unsigned long long>(ticks) + 1ull) : static_cast<unsigned long long>(ticks);
    unsigned long long div = 1;
    for (int i = 0; i < scale; ++i) div *= 10;
    std::snprintf(buf, sizeof(buf), "%s%llu.%0*llu", neg ? "-" : "", mag / div, scale,
                  mag % div);
    return buf;
}

std::string exec_type_code(ExecType t) {
    switch (t) {
        case ExecType::New: return "0";
        case ExecType::DoneForDay: return "3";
        case ExecType::Canceled: return "4";
        case ExecType::Replaced: return "5";
        case ExecType::PendingCancel: return "6";
        case ExecType::Stopped: return "7";
        case ExecType::Rejected: return "8";
        case ExecType::Suspended: return "9";
        case ExecType::PendingNew: return "A";
        case ExecType::Calculated: return "B";
        case ExecType::Expired: return "C";
        case ExecType::Restated: return "D";
        case ExecType::PendingReplace: return "E";
        case ExecType::Trade: return "F";
        case ExecType::TradeCorrect: return "G";
        case ExecType::TradeCancel: return "H";
        case ExecType::OrderStatus: return "I";
    }
    return "?";
}

std::string ord_status_code(OrdStatus s) {
    switch (s) {
        case OrdStatus::New: return "0";
        case OrdStatus::PartiallyFilled: return "1";
        case OrdStatus::Filled: return "2";
        case OrdStatus::DoneForDay: return "3";
        case OrdStatus::Canceled: return "4";
        case OrdStatus::PendingCancel: return "6";
        case OrdStatus::Stopped: return "7";
        case OrdStatus::Rejected: return "8";
        case OrdStatus::Suspended: return "9";
        case OrdStatus::PendingNew: return "A";
        case OrdStatus::Calculated: return "B";
        case OrdStatus::Expired: return "C";
        case OrdStatus::AcceptedForBidding: return "D";
        case OrdStatus::PendingReplace: return "E";
    }
    return "?";
}

}  // namespace

std::string ReportSpec::encode() const {
    std::string out;
    const auto put = [&](int tag, const std::string& value) {
        if (omit.count(tag) != 0) return;
        auto it = raw.find(tag);
        const std::string& v = it != raw.end() ? it->second : value;
        out += std::to_string(tag);
        out.push_back('=');
        out += v;
        out.push_back(kSoh);
    };

    put(35, std::string(1, msg_type));
    if (msg_type == '8') {
        put(37, order_id);
        put(17, exec_id);
        if (!exec_ref_id.empty()) put(19, exec_ref_id);
        put(150, exec_type_code(exec_type));
        put(39, ord_status_code(ord_status));
        put(11, cl_ord_id);
        if (!orig_cl_ord_id.empty()) put(41, orig_cl_ord_id);
        put(1, account);
        put(55, symbol);
        put(54, side == Side::Buy ? "1" : "2");
        if (order_qty) put(38, decimal(*order_qty, 0));
        if (last_qty) put(32, decimal(*last_qty, 0));
        if (last_px) put(31, decimal(*last_px, price_scale));
        put(14, decimal(cum_qty, 0));
        put(151, decimal(leaves_qty, 0));
        put(6, decimal(avg_px, price_scale));
        if (ord_status_req_id) put(790, *ord_status_req_id);
    } else {
        // 35=9 OrderCancelReject
        put(37, order_id);
        put(11, cl_ord_id);
        put(41, orig_cl_ord_id);
        put(39, ord_status_code(ord_status));
        if (cxl_rej_response_to)
            put(434, *cxl_rej_response_to == CxlRejResponseTo::OrderCancelRequest ? "1" : "2");
        if (cxl_rej_reason) put(102, std::to_string(*cxl_rej_reason));
    }
    if (poss_dup) put(43, "Y");
    if (poss_resend) put(97, "Y");
    if (!text.empty()) put(58, text);
    for (const auto& [tag, value] : extra) {
        out += std::to_string(tag);
        out.push_back('=');
        out += value;
        out.push_back(kSoh);
    }
    return out;
}

// ---------------------------------------------------------------------------

LpSimulator::LpSimulator(LpImage& image, p01::SessionEngine& session)
    : image_(image), session_(session) {
    session_.attach_peer(this);
}

std::map<int, std::string> LpSimulator::split(const std::string& frame) {
    std::map<int, std::string> out;
    std::size_t i = 0;
    while (i < frame.size()) {
        const std::size_t soh = frame.find(kSoh, i);
        const std::size_t end = soh == std::string::npos ? frame.size() : soh;
        const std::string field = frame.substr(i, end - i);
        const std::size_t eq = field.find('=');
        if (eq != std::string::npos && eq > 0) {
            bool numeric = true;
            int tag = 0;
            for (std::size_t k = 0; k < eq; ++k) {
                if (field[k] < '0' || field[k] > '9') {
                    numeric = false;
                    break;
                }
                tag = tag * 10 + (field[k] - '0');
            }
            if (numeric) out[tag] = field.substr(eq + 1);
        }
        if (soh == std::string::npos) break;
        i = soh + 1;
    }
    return out;
}

namespace {

std::int64_t to_int(const std::string& s) {
    std::int64_t v = 0;
    bool neg = false;
    std::size_t i = 0;
    if (i < s.size() && s[i] == '-') {
        neg = true;
        ++i;
    }
    for (; i < s.size(); ++i) {
        if (s[i] == '.') break;
        if (s[i] < '0' || s[i] > '9') return 0;
        v = v * 10 + (s[i] - '0');
    }
    return neg ? -v : v;
}

std::int64_t to_ticks(const std::string& s, int scale) {
    std::int64_t whole = 0;
    std::int64_t frac = 0;
    int frac_digits = 0;
    bool in_frac = false;
    for (char c : s) {
        if (c == '.') {
            in_frac = true;
            continue;
        }
        if (c < '0' || c > '9') continue;
        if (in_frac) {
            if (frac_digits < scale) {
                frac = frac * 10 + (c - '0');
                ++frac_digits;
            }
        } else {
            whole = whole * 10 + (c - '0');
        }
    }
    while (frac_digits < scale) {
        frac *= 10;
        ++frac_digits;
    }
    std::int64_t mul = 1;
    for (int i = 0; i < scale; ++i) mul *= 10;
    return whole * mul + frac;
}

}  // namespace

void LpSimulator::on_app_frame(const std::string& frame) {
    image_.received_frames.push_back(frame);

    // Framing check before anything else. A FIX field block is a sequence of
    // SOH terminated fields, so a block that does not end in SOH was truncated
    // and a real acceptor would never hand it to the application layer
    // (OR-E-08, OR-O-08).
    if (frame.empty() || frame.back() != kSoh) {
        image_.rejected_frames.push_back(frame);
        return;
    }

    const auto f = split(frame);

    auto mt = f.find(35);
    if (mt == f.end() || mt->second.size() != 1) {
        image_.rejected_frames.push_back(frame);
        return;
    }

    // A truncated frame (CW-08) has no usable ClOrdID; the peer discards it the
    // way a FIX parser would and does not create an order.
    auto cl = f.find(11);
    if (cl == f.end() || cl->second.empty()) {
        image_.rejected_frames.push_back(frame);
        return;
    }
    const std::string cl_ord_id = cl->second;

    switch (mt->second[0]) {
        case 'D': {
            if (image_.id_to_order.count(cl_ord_id) != 0) {
                // A duplicate ClOrdID is a protocol error for the peer too.
                image_.rejected_frames.push_back(frame);
                return;
            }
            LpOrder o;
            o.cl_ord_id = cl_ord_id;
            o.first_cl_ord_id = cl_ord_id;
            o.order_id = "LP-" + std::to_string(image_.next_order_id++);
            o.account = f.count(1) ? f.at(1) : "";
            o.symbol = f.count(55) ? f.at(55) : "";
            o.side = (f.count(54) && f.at(54) == "2") ? Side::Sell : Side::Buy;
            o.qty = f.count(38) ? to_int(f.at(38)) : 0;
            o.price = f.count(44) ? to_ticks(f.at(44), 5) : 0;
            o.leaves_qty = o.qty;
            o.status = "New";
            image_.book[cl_ord_id] = o;
            image_.id_to_order[cl_ord_id] = cl_ord_id;
            if (image_.auto_ack && !image_.suppress_reports) send(new_ack(cl_ord_id));
            return;
        }
        case 'F': {
            const std::string orig = f.count(41) ? f.at(41) : "";
            auto key = image_.id_to_order.find(orig);
            if (key == image_.id_to_order.end()) {
                // Unknown order: a real peer answers with 35=9 reason 1.
                if (!image_.suppress_reports) {
                    ReportSpec rej;
                    rej.msg_type = '9';
                    rej.cl_ord_id = cl_ord_id;
                    rej.orig_cl_ord_id = orig;
                    rej.order_id = "NONE";
                    rej.ord_status = OrdStatus::Rejected;
                    rej.cxl_rej_response_to = CxlRejResponseTo::OrderCancelRequest;
                    rej.cxl_rej_reason = 1;
                    send(rej);
                }
                return;
            }
            LpOrder& o = image_.book[key->second];
            o.pending_cancel = true;
            image_.id_to_order[cl_ord_id] = key->second;
            if (image_.auto_pending_cancel && !image_.suppress_reports)
                send(pending_cancel(cl_ord_id, orig));
            return;
        }
        case 'G': {
            const std::string orig = f.count(41) ? f.at(41) : "";
            auto key = image_.id_to_order.find(orig);
            if (key == image_.id_to_order.end()) {
                if (!image_.suppress_reports) {
                    ReportSpec rej;
                    rej.msg_type = '9';
                    rej.cl_ord_id = cl_ord_id;
                    rej.orig_cl_ord_id = orig;
                    rej.order_id = "NONE";
                    rej.ord_status = OrdStatus::Rejected;
                    rej.cxl_rej_response_to = CxlRejResponseTo::OrderCancelReplaceRequest;
                    rej.cxl_rej_reason = 1;
                    send(rej);
                }
                return;
            }
            LpOrder& o = image_.book[key->second];
            o.pending_replace = true;
            o.pending_replace_id = cl_ord_id;
            o.pending_qty = f.count(38) ? to_int(f.at(38)) : o.qty;
            o.pending_price = f.count(44) ? to_ticks(f.at(44), 5) : o.price;
            image_.id_to_order[cl_ord_id] = key->second;
            if (image_.auto_pending_replace && !image_.suppress_reports)
                send(pending_replace(cl_ord_id, orig));
            return;
        }
        case 'H': {
            ++image_.status_requests_seen;
            if (!image_.capability_status_request) {
                // The peer never agreed to this. A router that sends it anyway is
                // the bug OR-N-01 looks for, so the frame is recorded as refused.
                image_.rejected_frames.push_back(frame);
                return;
            }
            if (image_.reject_status_requests) {
                image_.rejected_frames.push_back(frame);
                return;
            }
            std::optional<std::string> qid;
            if (f.count(790) != 0) qid = f.at(790);
            send(status_snapshot(cl_ord_id, qid));
            return;
        }
        default:
            image_.rejected_frames.push_back(frame);
            return;
    }
}

DurableReceiptId LpSimulator::send(const ReportSpec& spec) {
    return session_.deliver_inbound(spec.encode());
}

const LpOrder* LpSimulator::find(const std::string& any_cl_ord_id) const {
    auto it = image_.id_to_order.find(any_cl_ord_id);
    if (it == image_.id_to_order.end()) return nullptr;
    auto jt = image_.book.find(it->second);
    return jt == image_.book.end() ? nullptr : &jt->second;
}

namespace {

void fill_from_order(ReportSpec& s, const LpOrder& o) {
    s.order_id = o.order_id;
    s.account = o.account;
    s.symbol = o.symbol;
    s.side = o.side;
    s.cum_qty = o.cum_qty;
    s.leaves_qty = o.leaves_qty;
    s.avg_px = o.cum_qty > 0 ? o.weighted_px / o.cum_qty : 0;
    s.order_qty = o.qty;
}

}  // namespace

ReportSpec LpSimulator::new_ack(const std::string& cl_ord_id) const {
    ReportSpec s;
    s.exec_type = ExecType::New;
    s.ord_status = OrdStatus::New;
    s.cl_ord_id = cl_ord_id;
    s.exec_id = "EXA-" + cl_ord_id;
    if (const LpOrder* o = find(cl_ord_id)) fill_from_order(s, *o);
    return s;
}

ReportSpec LpSimulator::trade(const std::string& cl_ord_id, const std::string& exec_id,
                              std::int64_t qty, std::int64_t px) const {
    ReportSpec s;
    s.exec_type = ExecType::Trade;
    s.cl_ord_id = cl_ord_id;
    s.exec_id = exec_id;
    s.last_qty = qty;
    s.last_px = px;
    if (const LpOrder* o = find(cl_ord_id)) {
        fill_from_order(s, *o);
        // The projected book state after this fill, computed by the simulator's
        // own arithmetic.
        s.cum_qty = o->cum_qty + qty;
        s.leaves_qty = o->qty > s.cum_qty ? o->qty - s.cum_qty : 0;
        s.avg_px = s.cum_qty > 0 ? (o->weighted_px + qty * px) / s.cum_qty : 0;
        s.ord_status = s.leaves_qty == 0 ? OrdStatus::Filled : OrdStatus::PartiallyFilled;
    } else {
        s.cum_qty = qty;
        s.ord_status = OrdStatus::PartiallyFilled;
    }
    return s;
}

ReportSpec LpSimulator::pending_cancel(const std::string& cancel_id,
                                       const std::string& orig) const {
    ReportSpec s;
    s.exec_type = ExecType::PendingCancel;
    s.ord_status = OrdStatus::PendingCancel;
    s.cl_ord_id = cancel_id;
    s.orig_cl_ord_id = orig;
    s.exec_id = "EXPC-" + cancel_id;
    if (const LpOrder* o = find(orig)) fill_from_order(s, *o);
    return s;
}

ReportSpec LpSimulator::canceled(const std::string& cancel_id, const std::string& orig) const {
    ReportSpec s;
    s.exec_type = ExecType::Canceled;
    s.ord_status = OrdStatus::Canceled;
    s.cl_ord_id = cancel_id;
    s.orig_cl_ord_id = orig;
    s.exec_id = "EXC-" + cancel_id;
    if (const LpOrder* o = find(orig)) {
        fill_from_order(s, *o);
        s.leaves_qty = 0;
    }
    return s;
}

ReportSpec LpSimulator::pending_replace(const std::string& replace_id,
                                         const std::string& orig) const {
    ReportSpec s;
    s.exec_type = ExecType::PendingReplace;
    s.ord_status = OrdStatus::PendingReplace;
    s.cl_ord_id = replace_id;
    s.orig_cl_ord_id = orig;
    s.exec_id = "EXPR-" + replace_id;
    if (const LpOrder* o = find(orig)) fill_from_order(s, *o);
    return s;
}

ReportSpec LpSimulator::replaced(const std::string& replace_id, const std::string& orig,
                                 std::int64_t new_qty, std::int64_t new_px) const {
    ReportSpec s;
    s.exec_type = ExecType::Replaced;
    s.cl_ord_id = replace_id;
    s.orig_cl_ord_id = orig;
    s.exec_id = "EXR-" + replace_id;
    if (const LpOrder* o = find(orig)) {
        fill_from_order(s, *o);
        s.order_qty = new_qty;
        s.leaves_qty = new_qty > o->cum_qty ? new_qty - o->cum_qty : 0;
        s.ord_status = o->cum_qty > 0 ? OrdStatus::PartiallyFilled : OrdStatus::New;
        if (s.leaves_qty == 0) s.ord_status = OrdStatus::Filled;
    } else {
        s.order_qty = new_qty;
        s.leaves_qty = new_qty;
        s.ord_status = OrdStatus::New;
    }
    (void)new_px;
    return s;
}

ReportSpec LpSimulator::rejected(const std::string& cl_ord_id, const std::string& reason) const {
    ReportSpec s;
    s.exec_type = ExecType::Rejected;
    s.ord_status = OrdStatus::Rejected;
    s.cl_ord_id = cl_ord_id;
    s.exec_id = "EXJ-" + cl_ord_id;
    s.text = reason;
    if (const LpOrder* o = find(cl_ord_id)) {
        fill_from_order(s, *o);
        s.leaves_qty = 0;
    } else {
        s.order_id = "NONE";
    }
    return s;
}

ReportSpec LpSimulator::cancel_reject(const std::string& cl_ord_id, const std::string& orig,
                                      CxlRejResponseTo kind, int reason) const {
    ReportSpec s;
    s.msg_type = '9';
    s.cl_ord_id = cl_ord_id;
    s.orig_cl_ord_id = orig;
    s.cxl_rej_response_to = kind;
    s.cxl_rej_reason = reason;
    const LpOrder* o = find(orig);
    s.order_id = o != nullptr ? o->order_id : "NONE";
    if (o != nullptr) {
        if (o->leaves_qty == 0 && o->cum_qty >= o->qty)
            s.ord_status = OrdStatus::Filled;
        else if (o->cum_qty > 0)
            s.ord_status = OrdStatus::PartiallyFilled;
        else
            s.ord_status = OrdStatus::New;
    } else {
        s.ord_status = OrdStatus::Rejected;
    }
    return s;
}

ReportSpec LpSimulator::status_snapshot(const std::string& cl_ord_id,
                                        const std::optional<std::string>& query_id) const {
    ReportSpec s;
    s.exec_type = ExecType::OrderStatus;
    s.cl_ord_id = cl_ord_id;
    // FIX 4.4 allows ExecID=0 on a status snapshot. R-I08 and OR-K-09 both turn
    // on the router not treating that as a business execution identity.
    s.exec_id = "0";
    if (const LpOrder* o = find(cl_ord_id)) {
        fill_from_order(s, *o);
        if (o->status == "Canceled")
            s.ord_status = OrdStatus::Canceled;
        else if (o->leaves_qty == 0 && o->cum_qty >= o->qty)
            s.ord_status = OrdStatus::Filled;
        else if (o->cum_qty > 0)
            s.ord_status = OrdStatus::PartiallyFilled;
        else
            s.ord_status = OrdStatus::New;
    } else {
        s.ord_status = OrdStatus::Rejected;
        s.order_id = "NONE";
    }
    if (query_id && image_.capability_status_request) s.ord_status_req_id = query_id;
    return s;
}

DurableReceiptId LpSimulator::execute(const std::string& cl_ord_id, std::int64_t qty,
                                      std::int64_t px) {
    auto key = image_.id_to_order.find(cl_ord_id);
    if (key == image_.id_to_order.end()) return 0;
    LpOrder& o = image_.book[key->second];
    const ReportSpec s = trade(o.cl_ord_id, "EX-" + std::to_string(image_.next_exec_id++), qty, px);
    o.cum_qty += qty;
    o.weighted_px += qty * px;
    o.leaves_qty = o.qty > o.cum_qty ? o.qty - o.cum_qty : 0;
    if (o.leaves_qty == 0) o.status = "Filled";
    if (image_.suppress_reports) return 0;
    return send(s);
}

DurableReceiptId LpSimulator::accept_cancel(const std::string& cancel_id) {
    auto key = image_.id_to_order.find(cancel_id);
    if (key == image_.id_to_order.end()) return 0;
    LpOrder& o = image_.book[key->second];
    const std::string orig = o.cl_ord_id;
    o.pending_cancel = false;
    o.leaves_qty = 0;
    o.status = "Canceled";
    if (image_.suppress_reports) return 0;
    return send(canceled(cancel_id, orig));
}

DurableReceiptId LpSimulator::accept_replace(const std::string& replace_id) {
    auto key = image_.id_to_order.find(replace_id);
    if (key == image_.id_to_order.end()) return 0;
    LpOrder& o = image_.book[key->second];
    const std::string orig = o.cl_ord_id;
    const ReportSpec s = replaced(replace_id, orig, o.pending_qty, o.pending_price);
    o.cl_ord_id = replace_id;
    o.qty = o.pending_qty;
    o.price = o.pending_price;
    o.leaves_qty = o.qty > o.cum_qty ? o.qty - o.cum_qty : 0;
    o.pending_replace = false;
    o.pending_replace_id.clear();
    image_.id_to_order[replace_id] = key->second;
    if (image_.suppress_reports) return 0;
    return send(s);
}

}  // namespace router::p02
