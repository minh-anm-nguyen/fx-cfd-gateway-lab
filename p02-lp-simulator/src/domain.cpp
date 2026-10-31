#include "lp/domain.hpp"

#include <algorithm>

namespace lpsim {

std::string_view to_string(Side s) {
    switch (s) {
        case Side::Buy: return "BUY";
        case Side::Sell: return "SELL";
    }
    return "?";
}

std::string_view fix_side(Side s) {
    return s == Side::Buy ? side_code::Buy : side_code::Sell;
}

bool parse_side(std::string_view v, Side* out) {
    if (v == side_code::Buy) {
        *out = Side::Buy;
        return true;
    }
    if (v == side_code::Sell) {
        *out = Side::Sell;
        return true;
    }
    return false;
}

std::string_view to_string(OrdType t) {
    switch (t) {
        case OrdType::Limit: return "LIMIT";
        case OrdType::Market: return "MARKET";
        case OrdType::Stop: return "STOP";
        case OrdType::StopLimit: return "STOP_LIMIT";
    }
    return "?";
}

bool parse_ord_type(std::string_view v, OrdType* out) {
    if (v == ord_type::Limit) { *out = OrdType::Limit; return true; }
    if (v == ord_type::Market) { *out = OrdType::Market; return true; }
    if (v == ord_type::Stop) { *out = OrdType::Stop; return true; }
    if (v == ord_type::StopLimit) { *out = OrdType::StopLimit; return true; }
    return false;
}

std::string_view to_string(Tif t) {
    switch (t) {
        case Tif::Day: return "DAY";
        case Tif::GoodTillCancel: return "GTC";
        case Tif::AtTheOpening: return "OPG";
        case Tif::ImmediateOrCancel: return "IOC";
        case Tif::FillOrKill: return "FOK";
    }
    return "?";
}

bool parse_tif(std::string_view v, Tif* out) {
    if (v == tif_code::Day) { *out = Tif::Day; return true; }
    if (v == tif_code::GoodTillCancel) { *out = Tif::GoodTillCancel; return true; }
    if (v == tif_code::AtTheOpening) { *out = Tif::AtTheOpening; return true; }
    if (v == tif_code::ImmediateOrCancel) { *out = Tif::ImmediateOrCancel; return true; }
    if (v == tif_code::FillOrKill) { *out = Tif::FillOrKill; return true; }
    return false;
}

std::string_view to_string(BaseState s) {
    switch (s) {
        case BaseState::New: return "NEW";
        case BaseState::PartiallyFilled: return "PARTIALLY_FILLED";
        case BaseState::Filled: return "FILLED";
        case BaseState::Canceled: return "CANCELED";
        case BaseState::Expired: return "EXPIRED";
        case BaseState::Rejected: return "REJECTED";
    }
    return "?";
}

bool is_terminal(BaseState s) {
    switch (s) {
        case BaseState::Filled:
        case BaseState::Canceled:
        case BaseState::Expired:
        case BaseState::Rejected:
            return true;
        case BaseState::New:
        case BaseState::PartiallyFilled:
            return false;
    }
    return false;
}

std::string_view to_string(Pending p) {
    switch (p) {
        case Pending::None: return "NONE";
        case Pending::Cancel: return "PENDING_CANCEL";
        case Pending::Replace: return "PENDING_REPLACE";
    }
    return "?";
}

std::string_view ord_status_for(BaseState base, Pending pending) {
    // A terminal order has already resolved any in-flight request, so the
    // terminal status wins. This is what stops a Filled order from reporting
    // OrdStatus=6 after a late cancel request (F-10).
    if (!is_terminal(base)) {
        if (pending == Pending::Cancel) return ord_status::PendingCancel;
        if (pending == Pending::Replace) return ord_status::PendingReplace;
    }
    switch (base) {
        case BaseState::New: return ord_status::New;
        case BaseState::PartiallyFilled: return ord_status::PartiallyFilled;
        case BaseState::Filled: return ord_status::Filled;
        case BaseState::Canceled: return ord_status::Canceled;
        case BaseState::Expired: return ord_status::Expired;
        case BaseState::Rejected: return ord_status::Rejected;
    }
    return ord_status::New;
}

std::string_view to_string(RequestType t) {
    switch (t) {
        case RequestType::New: return "NEW";
        case RequestType::Cancel: return "CANCEL";
        case RequestType::Replace: return "REPLACE";
    }
    return "?";
}

std::string_view to_string(RequestOutcome o) {
    switch (o) {
        case RequestOutcome::Accepted: return "ACCEPTED";
        case RequestOutcome::Rejected: return "REJECTED";
        case RequestOutcome::Pending: return "PENDING";
    }
    return "?";
}

std::string to_string(const BusinessKey& k) {
    return k.firm + "/" + k.business_day + "/" + k.cl_ord_id;
}

bool SymbolProfile::allows(OrdType t) const {
    return std::find(allowed_ord_types.begin(), allowed_ord_types.end(), t) !=
           allowed_ord_types.end();
}

bool SymbolProfile::allows(Tif t) const {
    return std::find(allowed_tifs.begin(), allowed_tifs.end(), t) != allowed_tifs.end();
}

// ---------------------------------------------------------------------------
// Canonical body serialisation
// ---------------------------------------------------------------------------

std::string serialize_body(const CanonicalBody& body) {
    std::string out;
    for (const auto& kv : body) {
        out.append(std::to_string(kv.first));
        out.push_back('=');
        out.append(kv.second);
        out.push_back(kSOH);
    }
    return out;
}

bool deserialize_body(std::string_view s, CanonicalBody* out) {
    out->clear();
    std::size_t pos = 0;
    while (pos < s.size()) {
        const std::size_t eq = s.find('=', pos);
        if (eq == std::string_view::npos || eq == pos) return false;
        const std::size_t soh = s.find(kSOH, eq + 1);
        if (soh == std::string_view::npos) return false;

        int tag_num = 0;
        for (std::size_t i = pos; i < eq; ++i) {
            const char c = s[i];
            if (c < '0' || c > '9') return false;
            tag_num = tag_num * 10 + (c - '0');
            if (tag_num > 999999) return false;
        }
        if (tag_num == 0) return false;
        if (soh == eq + 1) return false;  // an empty value never round-trips

        out->emplace_back(tag_num, std::string(s.substr(eq + 1, soh - eq - 1)));
        pos = soh + 1;
    }
    return true;
}

std::string_view to_string(ReportType t) {
    switch (t) {
        case ReportType::ExecNew: return "EXEC_NEW";
        case ReportType::ExecTrade: return "EXEC_TRADE";
        case ReportType::ExecCanceled: return "EXEC_CANCELED";
        case ReportType::ExecReplaced: return "EXEC_REPLACED";
        case ReportType::ExecPendingCancel: return "EXEC_PENDING_CANCEL";
        case ReportType::ExecPendingReplace: return "EXEC_PENDING_REPLACE";
        case ReportType::ExecRejected: return "EXEC_REJECTED";
        case ReportType::ExecExpired: return "EXEC_EXPIRED";
        case ReportType::CancelReject: return "CANCEL_REJECT";
        case ReportType::BusinessReject: return "BUSINESS_REJECT";
    }
    return "?";
}

std::string_view msg_type_for(ReportType t) {
    switch (t) {
        case ReportType::CancelReject: return msg_type::OrderCancelReject;        // 35=9
        case ReportType::BusinessReject: return msg_type::BusinessMessageReject;  // 35=j
        default: return msg_type::ExecutionReport;                                // 35=8
    }
}

std::string_view to_string(SendState s) {
    switch (s) {
        case SendState::Pending: return "PENDING";
        case SendState::SeqAssigned: return "SEQ_ASSIGNED";
        case SendState::Sent: return "SENT";
    }
    return "?";
}

}  // namespace lpsim
