#include "router/normalizer.hpp"

#include "router/numeric.hpp"

namespace router {
namespace {

NormalizeResult invalid(ValidationError e, std::string note) {
    NormalizeResult r;
    r.outcome = NormalizeOutcome::Invalid;
    r.validation_error = e;
    r.reason = HoldReason::SchemaViolation;
    r.note = std::move(note);
    return r;
}

NormalizeResult quarantine(HoldReason reason, std::string note,
                           std::optional<OrderChainId> hint = std::nullopt) {
    NormalizeResult r;
    r.outcome = NormalizeOutcome::Quarantine;
    r.reason = reason;
    r.note = std::move(note);
    r.chain_hint = hint;
    return r;
}

NormalizeResult orphan(std::string note) {
    NormalizeResult r;
    r.outcome = NormalizeOutcome::Orphan;
    r.reason = HoldReason::OrphanReport;
    r.note = std::move(note);
    return r;
}

}  // namespace

Hash report_fingerprint(const NormalizedReport& r) {
    // Only business-meaningful content. Session sequence numbers and receipt ids
    // are deliberately excluded: the same execution replayed on a new sequence
    // must fingerprint identically (OR-F-10, OR-K-02).
    Sha256 s;
    s.field(r.lp.str());
    s.field(r.account.str());
    s.field(r.cl_ord_id.str());
    s.field(r.orig_cl_ord_id ? r.orig_cl_ord_id->str() : "");
    s.field(r.order_id ? r.order_id->str() : "");
    s.field(r.exec_id ? r.exec_id->str() : "");
    s.field(to_string(r.exec_type));
    s.field(to_string(r.ord_status));
    s.field(r.wire_symbol);
    s.update(static_cast<std::uint64_t>(r.side == Side::Buy ? 1 : 2));
    s.update(static_cast<std::uint64_t>(r.last_qty.value_or(-1)));
    s.update(static_cast<std::uint64_t>(r.last_px.value_or(-1)));
    s.update(static_cast<std::uint64_t>(r.cum_qty));
    s.update(static_cast<std::uint64_t>(r.leaves_qty));
    s.update(static_cast<std::uint64_t>(r.order_qty.value_or(-1)));
    s.field(r.cxl_rej_response_to ? to_string(*r.cxl_rej_response_to) : "");
    s.update(static_cast<std::uint64_t>(r.cxl_rej_reason));
    return s.finish();
}

NormalizeResult Normalizer::parse_and_correlate(const p01::DurableFixInbound& inbound,
                                                const Route& route) const {
    // Step 1: the session this arrived on must be the session the route owns.
    // A frame from an unexpected session is P01's problem to reject, and if one
    // reaches us anyway it must not touch business state (OR-F-14, OR-Q-02).
    const std::string expected = route.sender_comp_id + ":" + route.target_comp_id + ":" +
                                 route.session_qualifier;
    if (inbound.session_key != expected)
        return quarantine(HoldReason::SecurityQuarantine,
                          "inbound session identity does not match the route");

    fix::ParseLimits lim;
    lim.max_frame_bytes = cfg_.limits.max_payload_bytes;
    lim.max_field_bytes = cfg_.limits.max_field_bytes;
    lim.max_fields = cfg_.limits.max_fields_per_message;
    lim.tolerated_custom_tags = cfg_.order_profile.tolerated_custom_tags;

    auto parsed = fix::parse_app_frame(inbound.frame, lim);
    if (parsed.is_err())
        return invalid(parsed.error(), "application frame failed the structural rules");
    const fix::Message& m = parsed.value();

    if (m.msg_type() == '8') {
        auto v = fix::validate_execution_report(m, cfg_);
        if (v.is_err()) return invalid(v.error(), "execution report failed the dictionary");
    } else if (m.msg_type() == '9') {
        auto v = fix::validate_cancel_reject(m);
        if (v.is_err()) return invalid(v.error(), "cancel reject failed the dictionary");
    } else {
        return invalid(ValidationError::UnsupportedMsgType, "unsupported application MsgType");
    }

    return correlate(m, inbound, route);
}

NormalizeResult Normalizer::correlate(const fix::Message& m, const p01::DurableFixInbound& inbound,
                                      const Route& route) const {
    NormalizedReport r;
    r.session_key = inbound.session_key;
    r.epoch = inbound.epoch;
    r.source_receipt = inbound.durable_inbox_id;
    r.msg_seq_num = inbound.msg_seq_num;
    r.raw_frame_hash = inbound.raw_sha256;
    r.profile_version = std::to_string(cfg_.profile_version);
    r.lp = route.lp;
    r.kind = m.msg_type() == '9' ? ReportKind::CancelReject : ReportKind::ExecutionReport;

    r.cl_ord_id = ClientOrderId(m.get_or(fix::tag::kClOrdID));
    if (m.has(fix::tag::kOrigClOrdID))
        r.orig_cl_ord_id = ClientOrderId(m.get_or(fix::tag::kOrigClOrdID));
    r.wire_symbol = m.get_or(fix::tag::kSymbol);
    if (auto side = fix::side_from_wire(m.get_or(fix::tag::kSide))) r.side = *side;
    r.account = AccountId(m.get_or(fix::tag::kAccount));
    r.poss_dup = m.get_or(fix::tag::kPossDupFlag) == "Y";
    r.poss_resend = m.get_or(fix::tag::kPossResend) == "Y";
    if (m.has(fix::tag::kOrdStatusReqID))
        r.ord_status_req_id = QueryId(m.get_or(fix::tag::kOrdStatusReqID));

    const std::string order_id_raw = m.get_or(fix::tag::kOrderID);
    // R-I13 and OR-C-20: "NONE" is a placeholder the ROE allows on an
    // unknown-order reject. It is never stored as a real LP OrderID.
    const bool order_id_is_none = order_id_raw.empty() || order_id_raw == "NONE";
    if (!order_id_is_none) r.order_id = OrderId(order_id_raw);

    if (r.kind == ReportKind::ExecutionReport) {
        auto et = fix::exec_type_from_wire(m.get_or(fix::tag::kExecType));
        auto os = fix::ord_status_from_wire(m.get_or(fix::tag::kOrdStatus));
        if (!et || !os) return invalid(ValidationError::UnknownExecType, "bad ExecType/OrdStatus");
        r.exec_type = *et;
        r.ord_status = *os;
        const std::string exec_raw = m.get_or(fix::tag::kExecID);
        if (!exec_raw.empty()) r.exec_id = ExecutionId(exec_raw);
        if (m.has(fix::tag::kExecRefID)) r.exec_ref_id = ExecutionId(m.get_or(fix::tag::kExecRefID));

        const SymbolProfile* prof = cfg_.symbol(r.wire_symbol);
        const int scale = prof ? prof->price_scale : 5;
        if (auto q = parse_decimal_ticks(m.get_or(fix::tag::kCumQty), 0); q.is_ok())
            r.cum_qty = q.value();
        if (auto q = parse_decimal_ticks(m.get_or(fix::tag::kLeavesQty), 0); q.is_ok())
            r.leaves_qty = q.value();
        if (auto q = parse_decimal_ticks(m.get_or(fix::tag::kAvgPx), scale); q.is_ok())
            r.avg_px = q.value();
        if (m.has(fix::tag::kOrderQty)) {
            if (auto q = parse_decimal_ticks(m.get_or(fix::tag::kOrderQty), 0); q.is_ok())
                r.order_qty = q.value();
        }
        if (m.has(fix::tag::kLastQty)) {
            if (auto q = parse_decimal_ticks(m.get_or(fix::tag::kLastQty), 0); q.is_ok())
                r.last_qty = q.value();
        }
        if (m.has(fix::tag::kLastPx)) {
            if (auto q = parse_decimal_ticks(m.get_or(fix::tag::kLastPx), scale); q.is_ok())
                r.last_px = q.value();
        }

        // The ExecType to OrdStatus pairings that cannot both be true. A peer
        // claiming Trade while reporting the order as Rejected is contradicting
        // itself, and guessing which half to believe is not an option.
        if (r.exec_type == ExecType::Trade &&
            (r.ord_status == OrdStatus::Rejected || r.ord_status == OrdStatus::Expired))
            return quarantine(HoldReason::SemanticConflict,
                              "Trade reported with a terminal non-fill OrdStatus");
    } else {
        auto os = fix::ord_status_from_wire(m.get_or(fix::tag::kOrdStatus));
        if (!os) return invalid(ValidationError::UnknownOrdStatus, "bad OrdStatus on 35=9");
        r.ord_status = *os;
        r.cxl_rej_response_to = fix::cxl_rej_response_to_from_wire(
            m.get_or(fix::tag::kCxlRejResponseTo));
        if (m.has(fix::tag::kCxlRejReason)) {
            if (auto v = parse_u63(m.get_or(fix::tag::kCxlRejReason), 6); v.is_ok())
                r.cxl_rej_reason = static_cast<int>(v.value());
        }
    }

    // Step 2: the request lineage. Tag 11 names the request this report answers.
    const CommandIntent* by_cl = store_.find_intent_by_cl_ord_id(route.lp, r.cl_ord_id);
    const CommandIntent* by_orig =
        r.orig_cl_ord_id ? store_.find_intent_by_cl_ord_id(route.lp, *r.orig_cl_ord_id) : nullptr;

    const CommandIntent* anchor = by_cl != nullptr ? by_cl : by_orig;
    if (anchor == nullptr) {
        // Nothing we issued carries either identifier. OR-J-11 and OR-L-07 both
        // require the evidence to be kept and reviewed, not dropped.
        return orphan("neither ClOrdID nor OrigClOrdID matches a request we issued");
    }
    r.chain_id = anchor->chain_id;

    const p03::ChainState* chain = store_.find_chain(r.chain_id);
    if (chain == nullptr)
        return orphan("request is known but its chain projection is not committed yet");

    // Step 1b, now that we know the chain: the account on the report must be the
    // account that owns the chain. A valid session carrying another account's
    // identifier is a security event, not a business event (OR-C-18, OR-J-15,
    // OR-Q-03).
    if (!r.account.empty() && r.account != chain->account)
        return quarantine(HoldReason::SecurityQuarantine,
                          "report Account does not own this chain", r.chain_id);
    if (r.account.empty()) r.account = chain->account;
    if (chain->lp != route.lp)
        return quarantine(HoldReason::SecurityQuarantine, "chain is routed to another LP",
                          r.chain_id);

    // Step 2b: an execution we have already applied, byte for byte, is the same
    // execution. Re-validating its lineage against a chain that has since moved
    // on would turn a harmless replay into a contradiction -- a duplicated
    // ExecType=5 would be judged against the version it itself established.
    // R-I07 wants the replay to be a no-op, so it short circuits here and the
    // router's dedup turns it into one (OR-I-09, OR-K-19, OR-K-20).
    if (r.exec_id && !r.exec_id->str().empty() && r.exec_id->str() != "0" &&
        r.exec_type != ExecType::OrderStatus) {
        const BusinessExecKey* applied =
            store_.find_exec_key(route.lp.str(), r.exec_id->str());
        if (applied != nullptr && applied->chain_id == r.chain_id) {
            r.fingerprint = report_fingerprint(r);
            if (applied->payload_fingerprint == r.fingerprint) {
                NormalizeResult dup;
                dup.outcome = NormalizeOutcome::Ok;
                dup.report = r;
                dup.chain_hint = r.chain_id;
                return dup;
            }
        }
    }

    // Step 3: OrderID. Once bound it must not change; two LPs reusing the same
    // value stay separate because the key includes the LP (OR-C-11, OR-C-12).
    if (r.order_id && chain->lp_order_id && *chain->lp_order_id != *r.order_id)
        return quarantine(HoldReason::IdentityConflict,
                          "OrderID contradicts the binding already made for this chain",
                          r.chain_id);
    if (r.order_id && !chain->lp_order_id) {
        // A new OrderID must not already belong to a different chain.
        for (const auto& [id, other] : store_.image().chains) {
            (void)id;
            if (other.chain_id == chain->chain_id) continue;
            if (other.lp == route.lp && other.lp_order_id && *other.lp_order_id == *r.order_id)
                return quarantine(HoldReason::IdentityConflict,
                                  "OrderID is already bound to another chain", r.chain_id);
        }
    }

    // Step 4: OrigClOrdID against the last *accepted* version, strict v1
    // (R-I05). A cancel acknowledgement that names a superseded or never
    // accepted version is a lineage contradiction (OR-C-14, OR-C-19).
    if (r.kind == ReportKind::ExecutionReport) {
        const bool lineage_report =
            r.exec_type == ExecType::Canceled || r.exec_type == ExecType::PendingCancel ||
            r.exec_type == ExecType::Replaced || r.exec_type == ExecType::PendingReplace;
        if (lineage_report && r.orig_cl_ord_id &&
            *r.orig_cl_ord_id != chain->accepted_cl_ord_id) {
            return quarantine(HoldReason::IdentityConflict,
                              "OrigClOrdID is not the last accepted version", r.chain_id);
        }
    } else {
        // 35=9: tag 11 must be the pending request of the kind 434 names.
        if (!r.cxl_rej_response_to)
            return invalid(ValidationError::MissingCancelRejectResponseTo, "35=9 without 434");
        const bool to_cancel = *r.cxl_rej_response_to == CxlRejResponseTo::OrderCancelRequest;
        const std::optional<ClientOrderId>& pending =
            to_cancel ? chain->pending_cancel : chain->pending_replace;
        if (!pending)
            // OR-C-15: 434 names a request kind that has nothing outstanding.
            return quarantine(HoldReason::IdentityConflict,
                              "cancel reject names a request kind with nothing pending",
                              r.chain_id);
        if (r.cl_ord_id != *pending)
            // OR-C-16, OR-H-18: the echo does not match the pending request.
            return quarantine(HoldReason::IdentityConflict,
                              "cancel reject echoes a ClOrdID that is not pending", r.chain_id);
        // OR-C-20: OrderID NONE is only acceptable for the unknown-order reason.
        if (order_id_is_none && r.cxl_rej_reason != 1)
            return quarantine(HoldReason::IdentityConflict,
                              "OrderID NONE outside the agreed unknown-order case", r.chain_id);
    }

    // Step 4b: an execution report that is not about a cancel or replace
    // request must name the accepted version. A Trade quoting a ClOrdID that is
    // still only a pending request would mean the peer is working a version it
    // has never told us it accepted, and crediting it would be the silent
    // version switch R-I12 forbids (OR-M-06).
    if (r.kind == ReportKind::ExecutionReport) {
        const bool lineage_report =
            r.exec_type == ExecType::Canceled || r.exec_type == ExecType::PendingCancel ||
            r.exec_type == ExecType::Replaced || r.exec_type == ExecType::PendingReplace;
        if (!lineage_report && r.cl_ord_id != chain->accepted_cl_ord_id) {
            const bool names_pending =
                (chain->pending_replace && r.cl_ord_id == *chain->pending_replace) ||
                (chain->pending_cancel && r.cl_ord_id == *chain->pending_cancel);
            if (names_pending)
                return quarantine(HoldReason::IdentityConflict,
                                  "execution names a request that is still pending",
                                  r.chain_id);
        }
    }

    // Step 5: the ExecID namespace. A Trade with the status-snapshot sentinel is
    // outside the agreed scope and must not be used as a trade identity
    // (R-I08, OR-K-10).
    if (r.kind == ReportKind::ExecutionReport && r.exec_type == ExecType::Trade && r.exec_id &&
        r.exec_id->str() == "0")
        return quarantine(HoldReason::ExecIdConflict,
                          "Trade carrying ExecID=0 outside the agreed namespace", r.chain_id);

    // A status snapshot that answers a query we never issued cannot serve as
    // evidence for this case (OR-N-06).
    if (r.ord_status_req_id && cfg_.capabilities.echo_ord_status_req_id) {
        bool known = false;
        for (const auto& [k, c] : store_.image().recon_cases) {
            (void)k;
            for (const std::string& q : c.query_ids)
                if (q == r.ord_status_req_id->str()) known = true;
        }
        if (!known)
            return quarantine(HoldReason::IdentityConflict,
                              "status response echoes an unknown OrdStatusReqID", r.chain_id);
    }

    r.source_wall = 0;
    r.fingerprint = report_fingerprint(r);

    NormalizeResult out;
    out.outcome = NormalizeOutcome::Ok;
    out.report = r;
    out.chain_hint = r.chain_id;
    return out;
}

}  // namespace router
