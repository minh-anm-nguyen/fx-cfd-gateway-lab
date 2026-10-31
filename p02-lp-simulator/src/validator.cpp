#include "lp/validator.hpp"

#include <chrono>

namespace lpsim {

std::uint64_t canonical_hash64(std::string_view s) {
    std::uint64_t h = 1469598103934665603ull;  // FNV-1a offset basis
    for (char c : s) {
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ull;  // FNV prime
    }
    return h;
}

const SymbolProfile* AppValidator::find_symbol(std::string_view symbol) const {
    for (const SymbolProfile& s : symbols_) {
        if (s.symbol == symbol) return &s;
    }
    return nullptr;
}

namespace {

// Mandatory business tags, by message type. Sending any of them twice makes the
// payload ambiguous; the design doc forbids picking one occurrence (B-18).
const int kNewOrderUniqueTags[] = {tag::ClOrdID, tag::Symbol,  tag::Side,
                                   tag::OrderQty, tag::OrdType, tag::Price,
                                   tag::TimeInForce, tag::TransactTime, tag::Account};
const int kCancelUniqueTags[] = {tag::ClOrdID, tag::OrigClOrdID, tag::Symbol, tag::Side,
                                 tag::OrderQty, tag::TransactTime};

BizReject check_unique(const DecodedMessage& m, const int* tags, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        if (m.count(tags[i]) > 1) return BizReject::AmbiguousDuplicateTag;
    }
    return BizReject::None;
}

// Map a numeric parse failure onto the business reject the matrix expects.
BizReject price_reject(NumParse e) {
    switch (e) {
        case NumParse::TooManyDecimals: return BizReject::OffTick;  // B-08
        case NumParse::Ok: return BizReject::None;
        default: return BizReject::BadPrice;                        // B-07
    }
}

BizReject qty_reject(NumParse e) {
    switch (e) {
        case NumParse::Ok: return BizReject::None;
        default: return BizReject::BadQuantity;  // B-06, B-09
    }
}

std::string build_canonical(std::string_view type, const AppCommand& c) {
    std::string s;
    s.reserve(128);
    s.append(type).push_back('|');
    s.append(c.key.firm).push_back('|');
    s.append(c.key.business_day).push_back('|');
    s.append(c.key.cl_ord_id).push_back('|');
    s.append(c.orig_cl_ord_id).push_back('|');
    s.append(c.symbol).push_back('|');
    s.append(to_string(c.side)).push_back('|');
    s.append(to_string(c.ord_type)).push_back('|');
    s.append(to_string(c.tif)).push_back('|');
    s.append(format_qty(c.qty)).push_back('|');
    s.append(format_qty(c.limit_ticks)).push_back('|');
    // TransactTime is deliberately NOT part of the canonical payload: an
    // application resend legitimately carries a fresh 60 while meaning the
    // same business request (C-03, I-03). Including it would turn every resend
    // into a conflict.
    return s;
}

}  // namespace

BizReject AppValidator::validate(const DecodedMessage& m, const InboundIdentity& id,
                                 std::string_view firm, std::string_view business_day,
                                 WallTime now, AppCommand* out) const {
    *out = AppCommand{};
    out->inbound = id;
    out->key.firm = std::string(firm);
    out->key.business_day = std::string(business_day);

    const std::string_view mt = m.msg_type;
    if (mt == msg_type::NewOrderSingle) out->type = RequestType::New;
    else if (mt == msg_type::OrderCancelRequest) out->type = RequestType::Cancel;
    else if (mt == msg_type::OrderCancelReplaceRequest) out->type = RequestType::Replace;
    else return BizReject::UnsupportedMsgType;  // answered with 35=j (B-17)

    // --- Ambiguity ---------------------------------------------------------
    const BizReject dup =
        (out->type == RequestType::Cancel)
            ? check_unique(m, kCancelUniqueTags, sizeof(kCancelUniqueTags) / sizeof(int))
            : check_unique(m, kNewOrderUniqueTags, sizeof(kNewOrderUniqueTags) / sizeof(int));
    if (dup != BizReject::None) return dup;

    // --- ClOrdID(11) -------------------------------------------------------
    const auto cl = m.find(tag::ClOrdID);
    if (!cl) return BizReject::MissingRequiredField;  // B-01
    if (cl->size() > policy_.max_cl_ord_id_len) return BizReject::IdTooLong;  // B-19
    out->key.cl_ord_id = std::string(*cl);
    out->has_cl_ord_id = true;

    // --- OrigClOrdID(41) ---------------------------------------------------
    if (out->type != RequestType::New) {
        const auto orig = m.find(tag::OrigClOrdID);
        // Without 41 there is nothing to cancel or replace, and guessing the
        // target would be worse than refusing (B-13, B-14).
        if (!orig) return BizReject::MissingRequiredField;
        if (orig->size() > policy_.max_cl_ord_id_len) return BizReject::IdTooLong;
        out->orig_cl_ord_id = std::string(*orig);
        out->has_orig_cl_ord_id = true;
    }

    // --- Account(1) --------------------------------------------------------
    if (const auto acc = m.find(tag::Account)) out->account = std::string(*acc);
    if (policy_.require_account || !policy_.allowed_accounts.empty()) {
        if (out->account.empty()) return BizReject::UnknownAccount;
        bool found = policy_.allowed_accounts.empty();
        for (const std::string& a : policy_.allowed_accounts) {
            if (a == out->account) {
                found = true;
                break;
            }
        }
        if (!found) return BizReject::UnknownAccount;  // C-16
    }

    // --- Symbol(55) --------------------------------------------------------
    const auto sym = m.find(tag::Symbol);
    if (!sym) return BizReject::MissingRequiredField;  // B-02
    out->symbol = std::string(*sym);
    out->has_symbol = true;
    const SymbolProfile* profile = find_symbol(out->symbol);
    if (profile == nullptr) return BizReject::UnknownSymbol;  // B-10
    if (!profile->enabled) return BizReject::DisabledSymbol;

    // --- Side(54) ----------------------------------------------------------
    const auto side_v = m.find(tag::Side);
    if (!side_v) return BizReject::MissingRequiredField;  // B-03
    if (!parse_side(*side_v, &out->side)) return BizReject::UnsupportedOrdType;
    out->has_side = true;

    // --- TransactTime(60) --------------------------------------------------
    const auto tt = m.find(tag::TransactTime);
    if (!tt) return BizReject::MissingRequiredField;
    if (!parse_utc_timestamp(*tt, &out->transact_time)) return BizReject::MissingRequiredField;
    {
        const auto delta =
            std::chrono::duration_cast<std::chrono::seconds>(out->transact_time - now).count();
        if (delta > policy_.transact_time_future_tolerance_s)
            return BizReject::FutureTransactTime;  // C-18
        if (-delta > policy_.transact_time_past_tolerance_s)
            return BizReject::StaleTransactTime;  // C-17
    }

    // --- Cancel stops here -------------------------------------------------
    if (out->type == RequestType::Cancel) {
        // OrderQty(38) is optional on a cancel in this profile. When present it
        // must parse and is checked against the chain later (F-14); a malformed
        // value is refused now rather than ignored.
        if (const auto q = m.find(tag::OrderQty)) {
            const NumParse e = parse_qty_units(*q, &out->qty);
            if (e != NumParse::Ok) return qty_reject(e);
            out->has_qty = true;
        }
        out->canonical_hash = canonical_hash64(build_canonical("F", *out));
        return BizReject::None;
    }

    // --- OrdType(40) -------------------------------------------------------
    const auto ot = m.find(tag::OrdType);
    if (!ot) return BizReject::MissingRequiredField;  // B-04
    if (!parse_ord_type(*ot, &out->ord_type)) return BizReject::UnsupportedOrdType;
    if (!profile->allows(out->ord_type)) return BizReject::UnsupportedOrdType;  // B-11

    // --- TimeInForce(59) ---------------------------------------------------
    // An absent tag 59 means Day per the FIX 4.4 dictionary, which is a
    // documented default rather than an arbitrary one (B-12).
    if (const auto tif_v = m.find(tag::TimeInForce)) {
        if (!parse_tif(*tif_v, &out->tif)) return BizReject::UnsupportedTimeInForce;
    } else {
        out->tif = Tif::Day;
    }
    if (!profile->allows(out->tif)) return BizReject::UnsupportedTimeInForce;

    // --- OrderQty(38) ------------------------------------------------------
    const auto q = m.find(tag::OrderQty);
    if (!q) return BizReject::MissingRequiredField;
    {
        const NumParse e = parse_qty_units(*q, &out->qty);
        if (e != NumParse::Ok) return qty_reject(e);
    }
    out->has_qty = true;
    if (!on_lot_step(out->qty, profile->lot_step)) return BizReject::OffLotStep;

    // --- Price(44) ---------------------------------------------------------
    // A Limit order without a price has no admissible interpretation (B-05).
    const auto px = m.find(tag::Price);
    if (!px) return BizReject::MissingRequiredField;
    {
        const NumParse e = parse_price_ticks(*px, profile->price, &out->limit_ticks);
        if (e != NumParse::Ok) return price_reject(e);
    }
    if (!on_tick(out->limit_ticks, profile->price)) return BizReject::OffTick;  // B-08

    out->canonical_hash =
        canonical_hash64(build_canonical(out->type == RequestType::New ? "D" : "G", *out));
    return BizReject::None;
}

}  // namespace lpsim
