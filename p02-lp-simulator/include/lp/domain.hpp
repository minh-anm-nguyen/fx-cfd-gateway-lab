// LP business domain: order chains, request registry, fill ledger and the
// canonical report payloads that feed the durable outbox.
//
// Reference: design doc 6 (domain model and identity) and 6.3 (state machine).
//
// A note on the state representation. The design doc draws a single state
// machine that includes PENDING_CANCEL and PENDING_REPLACE, but FIX 4.4 treats
// a pending request as a status that takes PRECEDENCE over the underlying
// order state rather than replacing it (design doc 6.3 and 9.2). Modelling that
// as one flat enum forces a combinatorial explosion and makes "a fill arrives
// while pending cancel" ambiguous. So the chain carries two orthogonal fields:
//
//   base    - where the order actually is: New, PartiallyFilled, Filled, ...
//   pending - which request is in flight: None, Cancel, Replace
//
// The reported OrdStatus(39) is derived from both by ord_status_for(), which is
// the single place the precedence rule lives. Scenarios E-20, F-09, F-17 and
// G-13 all assert against that derivation.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lp/fixed_point.hpp"
#include "lp/result.hpp"
#include "lp/wire.hpp"

namespace lpsim {

// ---------------------------------------------------------------------------
// Enumerations
// ---------------------------------------------------------------------------

enum class Side { Buy, Sell };

std::string_view to_string(Side s);
std::string_view fix_side(Side s);
bool parse_side(std::string_view v, Side* out);

// v1 supports Limit only. Market, Stop and StopLimit exist so that an
// unsupported order type is rejected for a named reason (B-11).
enum class OrdType { Limit, Market, Stop, StopLimit };

std::string_view to_string(OrdType t);
bool parse_ord_type(std::string_view v, OrdType* out);

// v1 supports Day only. An absent tag 59 defaults to Day per the FIX 4.4
// dictionary; IOC/FOK/GTC are parsed so that B-12 and M-19 can assert an
// explicit refusal rather than a silent acceptance.
enum class Tif { Day, GoodTillCancel, AtTheOpening, ImmediateOrCancel, FillOrKill };

std::string_view to_string(Tif t);
bool parse_tif(std::string_view v, Tif* out);

// Where the order actually is.
enum class BaseState { New, PartiallyFilled, Filled, Canceled, Expired, Rejected };

std::string_view to_string(BaseState s);
bool is_terminal(BaseState s);
inline bool is_active(BaseState s) { return !is_terminal(s); }

// Which request is in flight against the chain.
enum class Pending { None, Cancel, Replace };

std::string_view to_string(Pending p);

// Reported OrdStatus(39): a pending request takes precedence over the base
// state while the order is still active. A terminal order reports its terminal
// status even if a request was in flight, because the terminal transition has
// already resolved that request (F-10, G-14).
std::string_view ord_status_for(BaseState base, Pending pending);

enum class RequestType { New, Cancel, Replace };

std::string_view to_string(RequestType t);

// What the registry recorded for a business request. A request that was
// rejected keeps its outcome so that resending it returns the same decision
// rather than being treated as fresh (C-12).
enum class RequestOutcome { Accepted, Rejected, Pending };

std::string_view to_string(RequestOutcome o);

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------

// Business request identity. Design doc 3.3 pins the v1 namespace to
// (sender_firm, business_day, ClOrdID), shared across D/F/G and independent of
// the FIX session epoch, so a coordinated sequence reset cannot release an ID
// (I-16, C-07).
struct BusinessKey {
    std::string firm;
    std::string business_day;
    std::string cl_ord_id;

    bool operator==(const BusinessKey& o) const {
        return firm == o.firm && business_day == o.business_day && cl_ord_id == o.cl_ord_id;
    }
};

std::string to_string(const BusinessKey& k);

// FIX inbound identity, the other independent de-duplication layer (I-16).
struct InboundIdentity {
    std::string session_id;
    std::uint64_t epoch = 0;
    std::uint64_t msg_seq_num = 0;
    bool poss_dup = false;     // tag 43: session retransmission
    bool poss_resend = false;  // tag 97: application resend, possibly a new seq
};

// ---------------------------------------------------------------------------
// Symbol profile
// ---------------------------------------------------------------------------

struct SymbolProfile {
    std::string symbol;
    PriceFormat price;
    Units lot_step = 1000;
    bool enabled = true;
    // Quote freshness. A quote older than this cannot back a fill (D-06).
    std::int64_t quote_stale_after_ms = 2000;
    // Allowed order characteristics. Anything outside these lists is rejected
    // with a named reason rather than coerced (B-11, B-12).
    std::vector<OrdType> allowed_ord_types{OrdType::Limit};
    std::vector<Tif> allowed_tifs{Tif::Day};

    bool allows(OrdType t) const;
    bool allows(Tif t) const;
};

// ---------------------------------------------------------------------------
// Order chain
// ---------------------------------------------------------------------------

struct OrderChain {
    std::string order_id;  // LP-generated, stable across replace (I-08)
    std::string firm;
    std::string business_day;

    std::string original_cl_ord_id;
    std::string current_cl_ord_id;  // last ACCEPTED version (I-08)

    // The in-flight request, if any. pending_cl_ord_id is not yet the current
    // version: a fill that lands while a replace is pending must report the
    // old accepted parameters (G-13, G-18).
    Pending pending = Pending::None;
    std::string pending_cl_ord_id;
    Units pending_qty = 0;
    Ticks pending_limit_ticks = 0;

    std::string symbol;
    Side side = Side::Buy;
    OrdType ord_type = OrdType::Limit;
    Tif tif = Tif::Day;

    Units original_qty = 0;
    Units effective_qty = 0;  // current accepted OrderQty, after any replace
    Ticks limit_ticks = 0;    // current accepted limit price

    Units cum_qty = 0;
    Units leaves_qty = 0;
    Ticks avg_px_ticks = 0;

    BaseState base = BaseState::New;
    std::uint64_t version = 1;

    std::string accepted_at_utc;
    std::string terminal_at_utc;

    std::string_view fix_ord_status() const { return ord_status_for(base, pending); }

    // LeavesQty is OrderQty - CumQty while active and exactly zero once
    // terminal, including a cancel after a partial fill (invariant I-04). The
    // formula OrderQty = CumQty + LeavesQty therefore does NOT hold in every
    // terminal state, which is why it is computed here rather than asserted.
    Units expected_leaves() const {
        if (is_terminal(base)) return 0;
        const Units remaining = effective_qty - cum_qty;
        return remaining > 0 ? remaining : 0;
    }
};

// ---------------------------------------------------------------------------
// Fill ledger
// ---------------------------------------------------------------------------

struct Fill {
    std::string fill_id;  // ledger primary key
    std::string exec_id;  // unique per logical report (I-05)
    std::string order_id;
    std::uint64_t quote_version = 0;
    std::string liquidity_slice;  // which slice was consumed (I-12)
    Units qty = 0;
    Ticks px_ticks = 0;
    std::string event_id;  // unique, ties the fill to exactly one outbox row
    std::string created_at_utc;
};

// ---------------------------------------------------------------------------
// Canonical report payload
// ---------------------------------------------------------------------------

// The business body of an outbound message: everything except the session
// header and trailer. Design doc 7.3 forbids a business actor from writing
// MsgSeqNum, so the body is stored without it and the session layer owns
// sequencing. The stored form survives restart and is replayed byte-identically.
using CanonicalBody = std::vector<std::pair<int, std::string>>;

std::string serialize_body(const CanonicalBody& body);
bool deserialize_body(std::string_view s, CanonicalBody* out);

enum class ReportType {
    ExecNew,
    ExecTrade,
    ExecCanceled,
    ExecReplaced,
    ExecPendingCancel,
    ExecPendingReplace,
    ExecRejected,
    ExecExpired,
    CancelReject,
    BusinessReject,
};

std::string_view to_string(ReportType t);

// Which MsgType(35) carries this report. A cancel or replace refusal uses
// 35=9, never 35=8 with ExecType=8: design doc 7.1 and scenario F-16 make
// getting this wrong a release blocker.
std::string_view msg_type_for(ReportType t);

// Dispatch state of an outbox row. The transition Pending -> SeqAssigned is
// TX-C and is the point at which the row is permanently bound to one outbound
// sequence number (invariant I-15).
enum class SendState { Pending, SeqAssigned, Sent };

std::string_view to_string(SendState s);

struct OutboxEntry {
    std::string event_id;  // immutable, stable across restart
    std::string order_id;
    ReportType type = ReportType::ExecNew;
    CanonicalBody body;
    std::string exec_id;  // empty unless the report carries one
    SendState send_state = SendState::Pending;
    std::uint64_t outbound_seq = 0;  // 0 until TX-C has committed
    std::uint64_t session_epoch = 0;
    std::string created_at_utc;
};

}  // namespace lpsim
