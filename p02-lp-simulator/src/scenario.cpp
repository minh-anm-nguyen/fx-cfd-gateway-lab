#include "lp/scenario.hpp"

#include <algorithm>
#include <cstdlib>

#include "lp/validator.hpp"

namespace lpsim {

std::string_view to_string(ScenarioAction a) {
    switch (a) {
        case ScenarioAction::ExpectLogon: return "expect_logon";
        case ScenarioAction::ExpectNew: return "expect_new";
        case ScenarioAction::ExpectCancel: return "expect_cancel";
        case ScenarioAction::ExpectReplace: return "expect_replace";
        case ScenarioAction::InjectQuote: return "inject_quote";
        case ScenarioAction::InjectFill: return "inject_fill";
        case ScenarioAction::FinalizeCancel: return "finalize_cancel";
        case ScenarioAction::FinalizeReplace: return "finalize_replace";
        case ScenarioAction::Expire: return "expire";
        case ScenarioAction::Disconnect: return "disconnect";
        case ScenarioAction::Reconnect: return "reconnect";
        case ScenarioAction::ResendRequest: return "resend_request";
        case ScenarioAction::Advance: return "advance";
        case ScenarioAction::Assert: return "assert";
    }
    return "?";
}

namespace {

bool action_from(std::string_view s, ScenarioAction* out) {
    static const std::pair<const char*, ScenarioAction> kMap[] = {
        {"expect_logon", ScenarioAction::ExpectLogon},
        {"expect_new", ScenarioAction::ExpectNew},
        {"expect_cancel", ScenarioAction::ExpectCancel},
        {"expect_replace", ScenarioAction::ExpectReplace},
        {"inject_quote", ScenarioAction::InjectQuote},
        {"inject_fill", ScenarioAction::InjectFill},
        {"finalize_cancel", ScenarioAction::FinalizeCancel},
        {"finalize_replace", ScenarioAction::FinalizeReplace},
        {"expire", ScenarioAction::Expire},
        {"disconnect", ScenarioAction::Disconnect},
        {"reconnect", ScenarioAction::Reconnect},
        {"resend_request", ScenarioAction::ResendRequest},
        {"advance", ScenarioAction::Advance},
        {"assert", ScenarioAction::Assert},
    };
    for (const auto& kv : kMap) {
        if (s == kv.first) {
            *out = kv.second;
            return true;
        }
    }
    return false;
}

std::string scalar(const ConfNode& n, std::string_view key) {
    const ConfNode* c = n.find(key);
    return (c != nullptr && c->is_scalar()) ? c->scalar : std::string();
}

long long integer(const ConfNode& n, std::string_view key, long long def) {
    const std::string s = scalar(n, key);
    if (s.empty()) return def;
    char* end = nullptr;
    const long long v = std::strtoll(s.c_str(), &end, 10);
    return (end != nullptr && *end == '\0') ? v : def;
}

bool boolean(const ConfNode& n, std::string_view key, bool def) {
    const std::string s = scalar(n, key);
    if (s == "true" || s == "yes" || s == "1") return true;
    if (s == "false" || s == "no" || s == "0") return false;
    return def;
}

std::string trim_sv(std::string_view s) {
    std::size_t b = 0;
    while (b < s.size() && s[b] == ' ') ++b;
    std::size_t e = s.size();
    while (e > b && s[e - 1] == ' ') --e;
    return std::string(s.substr(b, e - b));
}

}  // namespace

bool parse_scenario(const ConfNode& doc, ScenarioSpec* out, ConfError* err) {
    *out = ScenarioSpec{};
    err->ok = true;
    auto bad = [&](std::string msg) {
        err->ok = false;
        err->message = std::move(msg);
        return false;
    };

    out->scenario_id = scalar(doc, "scenario_id");
    if (out->scenario_id.empty()) return bad("scenario_id is required");

    // A scenario without a pinned seed is refused: an unreproducible run cannot
    // be cited as evidence (M-12).
    if (const ConfNode* seed = doc.find("seed")) {
        if (!seed->is_scalar() || seed->scalar.empty()) return bad("seed must be a number");
        char* end = nullptr;
        out->seed = std::strtoull(seed->scalar.c_str(), &end, 10);
        if (end == nullptr || *end != '\0') return bad("seed must be a number");
        out->seed_present = true;
    } else {
        return bad("seed is required so the run can be reproduced");
    }

    const std::string clock = scalar(doc, "clock");
    if (clock == "virtual" || clock.empty()) out->clock = ClockMode::Virtual;
    else if (clock == "wall") out->clock = ClockMode::Wall;
    else return bad("clock must be virtual or wall");

    const std::string trigger = scalar(doc, "fill_trigger_policy");
    if (trigger.empty() || trigger == "scripted") out->fill_trigger = FillTrigger::Scripted;
    else if (trigger == "auto_on_new_or_quote")
        out->fill_trigger = FillTrigger::AutoOnNewOrQuote;
    else return bad("fill_trigger_policy must be scripted or auto_on_new_or_quote");

    out->emit_pending_cancel = boolean(doc, "emit_pending_cancel", false);
    out->emit_pending_replace = boolean(doc, "emit_pending_replace", false);

    // initial_quote is sugar for an inject_quote at t=0.
    if (const ConfNode* q = doc.find("initial_quote")) {
        ScenarioStep s;
        s.at_ms = 0;
        s.action = ScenarioAction::InjectQuote;
        s.symbol = scalar(*q, "symbol");
        if (s.symbol.empty()) s.symbol = "EURUSD";
        s.bid = scalar(*q, "bid");
        s.ask = scalar(*q, "ask");
        s.bid_available = scalar(*q, "bid_available");
        s.ask_available = scalar(*q, "ask_available");
        s.quote_version = static_cast<std::uint64_t>(integer(*q, "version", 1));
        s.ttl_ms = integer(*q, "ttl_ms", 2000);
        if (s.bid.empty() || s.ask.empty()) return bad("initial_quote needs bid and ask");
        out->steps.push_back(std::move(s));
    }

    const ConfNode* events = doc.find("events");
    if (events != nullptr) {
        if (!events->is_seq()) return bad("events must be a sequence");
        if (events->seq.size() > out->max_steps) return bad("too many events");

        std::uint64_t auto_version = 1;
        for (const ConfNode& e : events->seq) {
            if (!e.is_map()) return bad("each event must be a mapping");
            ScenarioStep s;
            s.at_ms = integer(e, "at_ms", 0);
            if (s.at_ms < 0) return bad("at_ms must not be negative");

            const std::string act = scalar(e, "action");
            if (!action_from(act, &s.action)) return bad("unknown action '" + act + "'");

            s.cl_ord_id = scalar(e, "cl_ord_id");
            s.orig_cl_ord_id = scalar(e, "orig_cl_ord_id");
            s.order_ref = scalar(e, "order");
            if (s.order_ref.empty()) s.order_ref = scalar(e, "request");
            if (!scalar(e, "symbol").empty()) s.symbol = scalar(e, "symbol");
            if (!scalar(e, "side").empty()) s.side = scalar(e, "side");
            s.qty = scalar(e, "qty");
            s.price = scalar(e, "price");
            if (s.price.empty()) s.price = scalar(e, "limit");
            s.bid = scalar(e, "bid");
            s.ask = scalar(e, "ask");
            s.bid_available = scalar(e, "bid_available");
            s.ask_available = scalar(e, "ask_available");
            s.begin_seq = static_cast<std::uint64_t>(integer(e, "begin_seq", 0));
            s.end_seq = static_cast<std::uint64_t>(integer(e, "end_seq", 0));
            s.expect_reject = boolean(e, "expect_reject", false);
            s.assertion = scalar(e, "assert");
            s.ttl_ms = integer(e, "ttl_ms", 2000);
            s.quote_version = static_cast<std::uint64_t>(integer(e, "version", 0));

            // A quote update without an explicit version gets the next one, so
            // the monotonic rule holds without the author restating it.
            if (s.action == ScenarioAction::InjectQuote && s.quote_version == 0) {
                s.quote_version = ++auto_version;
            }

            // Per-action required fields, checked now rather than at run time.
            switch (s.action) {
                case ScenarioAction::ExpectNew:
                    if (s.cl_ord_id.empty() || s.qty.empty() || s.price.empty())
                        return bad("expect_new needs cl_ord_id, qty and limit");
                    break;
                case ScenarioAction::ExpectCancel:
                case ScenarioAction::ExpectReplace:
                    if (s.cl_ord_id.empty() || s.orig_cl_ord_id.empty())
                        return bad(act + " needs cl_ord_id and orig_cl_ord_id");
                    break;
                case ScenarioAction::InjectFill:
                    if (s.order_ref.empty() || s.qty.empty() || s.price.empty())
                        return bad("inject_fill needs order, qty and price");
                    break;
                case ScenarioAction::FinalizeCancel:
                case ScenarioAction::FinalizeReplace:
                    if (s.order_ref.empty()) return bad(act + " needs a request id");
                    break;
                case ScenarioAction::InjectQuote:
                    if (s.bid.empty() && s.ask.empty())
                        return bad("inject_quote needs at least one side");
                    break;
                case ScenarioAction::Assert:
                    if (s.assertion.empty()) return bad("assert needs an expression");
                    break;
                default:
                    break;
            }
            out->steps.push_back(std::move(s));
        }
    }

    // Steps run in (at_ms, declaration order). A stable sort preserves the
    // declaration order for equal timestamps, which is exactly the swap the
    // FILL_BEFORE_CANCEL and CANCEL_BEFORE_FILL fixtures rely on (H-01).
    std::stable_sort(out->steps.begin(), out->steps.end(),
                     [](const ScenarioStep& a, const ScenarioStep& b) {
                         return a.at_ms < b.at_ms;
                     });

    if (const ConfNode* asserts = doc.find("assert")) {
        if (!asserts->is_seq()) return bad("assert must be a sequence of expressions");
        for (const ConfNode& a : asserts->seq) {
            if (!a.is_scalar() || a.scalar.empty()) return bad("empty assertion");
            out->assertions.push_back(a.scalar);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Assertions
// ---------------------------------------------------------------------------

bool evaluate_assertion(std::string_view text, const OrderActor& actor,
                        const std::map<std::string, std::string>& order_ids,
                        std::string* why) {
    // Grammar: <fn>(<ref>) <op> <value>, plus the bare predicate
    // session_and_business_invariants_hold == true.
    const std::string s = trim_sv(text);

    if (s.rfind("session_and_business_invariants_hold", 0) == 0) {
        std::string reason;
        if (!actor.check_invariants(&reason)) {
            *why = "invariants do not hold: " + reason;
            return false;
        }
        return true;
    }

    const std::size_t open = s.find('(');
    const std::size_t close = s.find(')', open == std::string::npos ? 0 : open);
    if (open == std::string::npos || close == std::string::npos) {
        *why = "cannot parse assertion '" + s + "'";
        return false;
    }
    const std::string fn = trim_sv(std::string_view(s).substr(0, open));
    const std::string ref = trim_sv(std::string_view(s).substr(open + 1, close - open - 1));

    std::string rest = trim_sv(std::string_view(s).substr(close + 1));
    std::string op;
    if (rest.rfind("==", 0) == 0) op = "==";
    else if (rest.rfind("!=", 0) == 0) op = "!=";
    else if (rest.rfind("<=", 0) == 0) op = "<=";
    else if (rest.rfind(">=", 0) == 0) op = ">=";
    else if (!rest.empty() && (rest[0] == '<' || rest[0] == '>')) op = rest.substr(0, 1);
    if (op.empty()) {
        *why = "cannot parse comparison in '" + s + "'";
        return false;
    }
    const std::string expected = trim_sv(std::string_view(rest).substr(op.size()));

    // Resolve the reference: a ClOrdID the scenario used, or an OrderID.
    const OrderChain* chain = nullptr;
    const auto it = order_ids.find(ref);
    if (it != order_ids.end()) chain = actor.find_order(it->second);
    if (chain == nullptr) chain = actor.find_by_cl_ord_id(ref);
    if (chain == nullptr) chain = actor.find_order(ref);

    if (fn == "count_distinct_fills") {
        if (chain == nullptr) {
            *why = "unknown order '" + ref + "'";
            return false;
        }
        // Counted from the ledger mirror for THIS chain rather than from the
        // reports, so a test cannot pass on the wire while the ledger
        // disagrees (M-10).
        const std::size_t n = actor.fill_count_for(chain->order_id);
        const long long want = std::strtoll(expected.c_str(), nullptr, 10);
        const bool ok = (op == "==") ? (static_cast<long long>(n) == want)
                                     : (static_cast<long long>(n) != want);
        if (!ok) {
            *why = "count_distinct_fills(" + ref + ") is " + std::to_string(n) + ", expected " +
                   op + " " + expected;
            return false;
        }
        return true;
    }

    if (chain == nullptr) {
        *why = "unknown order '" + ref + "' in assertion '" + s + "'";
        return false;
    }

    std::string actual;
    bool numeric = false;
    long long actual_n = 0;
    if (fn == "CumQty") {
        actual_n = chain->cum_qty;
        numeric = true;
    } else if (fn == "LeavesQty") {
        actual_n = chain->leaves_qty;
        numeric = true;
    } else if (fn == "OrderQty") {
        actual_n = chain->effective_qty;
        numeric = true;
    } else if (fn == "AvgPx") {
        actual_n = chain->avg_px_ticks;
        numeric = true;
    } else if (fn == "Version") {
        actual_n = static_cast<long long>(chain->version);
        numeric = true;
    } else if (fn == "OrdStatus") {
        actual = std::string(to_string(chain->base));
    } else if (fn == "Pending") {
        actual = std::string(to_string(chain->pending));
    } else if (fn == "OrderID") {
        actual = chain->order_id;
    } else if (fn == "CurrentClOrdID") {
        actual = chain->current_cl_ord_id;
    } else {
        *why = "unknown assertion function '" + fn + "'";
        return false;
    }

    bool ok = false;
    if (numeric) {
        char* end = nullptr;
        const long long want = std::strtoll(expected.c_str(), &end, 10);
        if (end == nullptr || *end != '\0') {
            *why = "expected a number in '" + s + "'";
            return false;
        }
        if (op == "==") ok = actual_n == want;
        else if (op == "!=") ok = actual_n != want;
        else if (op == "<=") ok = actual_n <= want;
        else if (op == ">=") ok = actual_n >= want;
        else if (op == "<") ok = actual_n < want;
        else ok = actual_n > want;
        actual = std::to_string(actual_n);
    } else {
        if (op == "==") ok = actual == expected;
        else if (op == "!=") ok = actual != expected;
        else {
            *why = "ordering comparison is not defined for '" + fn + "'";
            return false;
        }
    }
    if (!ok) {
        *why = fn + "(" + ref + ") is " + actual + ", expected " + op + " " + expected;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Runner
// ---------------------------------------------------------------------------

std::vector<std::string> RecordingTransport::frames() const {
    std::vector<std::string> out;
    FixFramer f(1 << 20, 1 << 21);
    f.append(out_);
    while (true) {
        std::string_view fr;
        WireError e = WireError::None;
        if (!f.next(&fr, &e)) break;
        out.emplace_back(fr);
    }
    return out;
}

ScenarioRunner::ScenarioRunner(const ScenarioSpec& spec, VirtualClock& clock,
                               OrderActor& actor, AcceptorSession& session, QuoteBook& book,
                               std::vector<SymbolProfile> symbols,
                               RecordingTransport& transport)
    : spec_(spec),
      clock_(clock),
      actor_(actor),
      session_(session),
      book_(book),
      symbols_(std::move(symbols)),
      transport_(transport) {}

void ScenarioRunner::note(std::string line) { trace_.push_back(std::move(line)); }

const SymbolProfile* ScenarioRunner::symbol_of(std::string_view s) const {
    for (const SymbolProfile& p : symbols_) {
        if (p.symbol == s) return &p;
    }
    return nullptr;
}

namespace {

OutboundFields client_header(std::string_view mt, std::uint64_t seq, WallTime now) {
    OutboundFields f;
    f.msg_type = std::string(mt);
    f.seq_num = seq;
    f.sender_comp_id = "TRADER01";
    f.target_comp_id = "SIMLP01";
    f.sending_time = now;
    return f;
}

}  // namespace

std::string ScenarioRunner::build_logon() {
    OutboundFields f = client_header(msg_type::Logon, client_seq_++, clock_.wall_now());
    f.body.emplace_back(tag::EncryptMethod, "0");
    f.body.emplace_back(tag::HeartBtInt, "30");
    return encode(f);
}

std::string ScenarioRunner::build_new(const ScenarioStep& s) {
    OutboundFields f =
        client_header(msg_type::NewOrderSingle, client_seq_++, clock_.wall_now());
    f.body.emplace_back(tag::ClOrdID, s.cl_ord_id);
    f.body.emplace_back(tag::Symbol, s.symbol);
    f.body.emplace_back(tag::Side, s.side == "SELL" ? std::string(side_code::Sell)
                                                    : std::string(side_code::Buy));
    f.body.emplace_back(tag::OrderQty, s.qty);
    f.body.emplace_back(tag::OrdType, std::string(ord_type::Limit));
    f.body.emplace_back(tag::Price, s.price);
    f.body.emplace_back(tag::TimeInForce, std::string(tif_code::Day));
    f.body.emplace_back(tag::TransactTime, format_utc_timestamp(clock_.wall_now()));
    return encode(f);
}

std::string ScenarioRunner::build_cancel(const ScenarioStep& s) {
    OutboundFields f =
        client_header(msg_type::OrderCancelRequest, client_seq_++, clock_.wall_now());
    f.body.emplace_back(tag::ClOrdID, s.cl_ord_id);
    f.body.emplace_back(tag::OrigClOrdID, s.orig_cl_ord_id);
    f.body.emplace_back(tag::Symbol, s.symbol);
    f.body.emplace_back(tag::Side, s.side == "SELL" ? std::string(side_code::Sell)
                                                    : std::string(side_code::Buy));
    f.body.emplace_back(tag::TransactTime, format_utc_timestamp(clock_.wall_now()));
    return encode(f);
}

std::string ScenarioRunner::build_replace(const ScenarioStep& s) {
    OutboundFields f = client_header(msg_type::OrderCancelReplaceRequest, client_seq_++,
                                     clock_.wall_now());
    f.body.emplace_back(tag::ClOrdID, s.cl_ord_id);
    f.body.emplace_back(tag::OrigClOrdID, s.orig_cl_ord_id);
    f.body.emplace_back(tag::Symbol, s.symbol);
    f.body.emplace_back(tag::Side, s.side == "SELL" ? std::string(side_code::Sell)
                                                    : std::string(side_code::Buy));
    f.body.emplace_back(tag::OrderQty, s.qty);
    f.body.emplace_back(tag::OrdType, std::string(ord_type::Limit));
    f.body.emplace_back(tag::Price, s.price);
    f.body.emplace_back(tag::TimeInForce, std::string(tif_code::Day));
    f.body.emplace_back(tag::TransactTime, format_utc_timestamp(clock_.wall_now()));
    return encode(f);
}

std::string ScenarioRunner::build_resend(const ScenarioStep& s) {
    OutboundFields f =
        client_header(msg_type::ResendRequest, client_seq_++, clock_.wall_now());
    f.body.emplace_back(tag::BeginSeqNo, std::to_string(s.begin_seq));
    f.body.emplace_back(tag::EndSeqNo, std::to_string(s.end_seq));
    return encode(f);
}

bool ScenarioRunner::resolve_order(const std::string& ref, std::string* order_id,
                                   std::string* why) {
    const auto it = order_ids_.find(ref);
    if (it != order_ids_.end()) {
        *order_id = it->second;
        return true;
    }
    if (const OrderChain* c = actor_.find_by_cl_ord_id(ref)) {
        *order_id = c->order_id;
        return true;
    }
    if (const OrderChain* c = actor_.find_order(ref)) {
        *order_id = c->order_id;
        return true;
    }
    *why = "cannot resolve order reference '" + ref + "'";
    return false;
}

ScenarioResult ScenarioRunner::run() {
    ScenarioResult res;
    auto fail = [&](const ScenarioStep& s, std::string msg) {
        res.ok = false;
        res.failure = "step at_ms=" + std::to_string(s.at_ms) + " " +
                      std::string(to_string(s.action)) + ": " + std::move(msg);
        res.trace = trace_;
        res.trace_hash = trace_hash(trace_);
        res.order_ids = order_ids_;
        return res;
    };

    for (const ScenarioStep& s : spec_.steps) {
        // Step the virtual clock to the step's instant, then let every event due
        // at or before it run in canonical order.
        clock_.set_virtual_us(s.at_ms * 1000);
        actor_.run_due(clock_.virtual_now_us());
        session_.dispatch_outbox();

        switch (s.action) {
            case ScenarioAction::ExpectLogon: {
                transport_.reopen();
                session_.on_connected(++generation_);
                session_.on_bytes(build_logon(), generation_);
                if (session_.phase() != SessionPhase::Active)
                    return fail(s, "logon did not reach ACTIVE");
                note("LOGON");
                break;
            }
            case ScenarioAction::ExpectNew: {
                session_.on_bytes(build_new(s), generation_);
                const OrderChain* c = actor_.find_by_cl_ord_id(s.cl_ord_id);
                if (s.expect_reject) {
                    if (c != nullptr) return fail(s, "expected a reject but the order exists");
                    ++res.refusals;
                    note("NEW_REJECTED " + s.cl_ord_id);
                } else {
                    if (c == nullptr) return fail(s, "order was not accepted");
                    order_ids_[s.cl_ord_id] = c->order_id;
                    note("NEW " + s.cl_ord_id + " -> " + c->order_id + " qty=" + s.qty);
                }
                break;
            }
            case ScenarioAction::ExpectCancel: {
                session_.on_bytes(build_cancel(s), generation_);
                note("CANCEL " + s.cl_ord_id + " orig=" + s.orig_cl_ord_id);
                break;
            }
            case ScenarioAction::ExpectReplace: {
                session_.on_bytes(build_replace(s), generation_);
                note("REPLACE " + s.cl_ord_id + " orig=" + s.orig_cl_ord_id);
                break;
            }
            case ScenarioAction::InjectQuote: {
                const SymbolProfile* p = symbol_of(s.symbol);
                if (p == nullptr) return fail(s, "unknown symbol " + s.symbol);
                QuoteSlice q;
                q.symbol = s.symbol;
                q.version = s.quote_version;
                const QuoteSlice* cur = book_.current(s.symbol);
                // A side the step leaves out keeps its current value, so a
                // scenario can move one side of the book at a time.
                q.bid_ticks = cur != nullptr ? cur->bid_ticks : 0;
                q.ask_ticks = cur != nullptr ? cur->ask_ticks : 0;
                q.bid_available = cur != nullptr ? cur->bid_available : 0;
                q.ask_available = cur != nullptr ? cur->ask_available : 0;
                if (!s.bid.empty() &&
                    parse_price_ticks(s.bid, p->price, &q.bid_ticks) != NumParse::Ok)
                    return fail(s, "bid is not a valid price");
                if (!s.ask.empty() &&
                    parse_price_ticks(s.ask, p->price, &q.ask_ticks) != NumParse::Ok)
                    return fail(s, "ask is not a valid price");
                if (!s.bid_available.empty() &&
                    parse_qty_units(s.bid_available, &q.bid_available) != NumParse::Ok)
                    return fail(s, "bid_available is not a valid quantity");
                if (!s.ask_available.empty() &&
                    parse_qty_units(s.ask_available, &q.ask_available) != NumParse::Ok)
                    return fail(s, "ask_available is not a valid quantity");
                q.valid_from_ms = s.at_ms;
                q.expires_at_ms = s.at_ms + s.ttl_ms;
                q.source_fixture_id = spec_.scenario_id;

                ScheduledEvent ev;
                ev.event_id = "QV-" + s.symbol + "-" + std::to_string(s.quote_version);
                ev.due_us = s.at_ms * 1000;
                ev.kind = EventKind::QuoteUpdate;
                ev.quote = q;
                if (actor_.schedule(ev) != EnqueueResult::Ok)
                    return fail(s, "quote event was refused by the scheduler");
                actor_.run_due(clock_.virtual_now_us());
                const QuoteSlice* now = book_.current(s.symbol);
                if (s.expect_reject) {
                    if (now != nullptr && now->version == s.quote_version)
                        return fail(s, "expected the quote to be refused");
                    ++res.refusals;
                    note("QUOTE_REFUSED v" + std::to_string(s.quote_version));
                } else {
                    if (now == nullptr || now->version != s.quote_version)
                        return fail(s, "quote was not installed");
                    note("QUOTE v" + std::to_string(s.quote_version) + " ask=" +
                         format_price(now->ask_ticks, p->price.scale) + " ask_avail=" +
                         format_qty(now->ask_available));
                }
                break;
            }
            case ScenarioAction::InjectFill: {
                std::string order_id, why;
                if (!resolve_order(s.order_ref, &order_id, &why)) return fail(s, why);
                const SymbolProfile* p = symbol_of(s.symbol);
                if (p == nullptr) return fail(s, "unknown symbol " + s.symbol);

                ScheduledEvent ev;
                ev.event_id = "IF-" + order_id + "-" + std::to_string(++event_counter_);
                ev.due_us = s.at_ms * 1000;
                ev.kind = EventKind::InjectFill;
                ev.order_id = order_id;
                if (parse_qty_units(s.qty, &ev.qty) != NumParse::Ok)
                    return fail(s, "fill qty is not a valid quantity");
                if (parse_price_ticks(s.price, p->price, &ev.px_ticks) != NumParse::Ok)
                    return fail(s, "fill price is not a valid price");
                if (actor_.schedule(ev) != EnqueueResult::Ok)
                    return fail(s, "fill event was refused by the scheduler");

                const std::size_t before = actor_.fill_count_for(order_id);
                actor_.run_due(clock_.virtual_now_us());
                session_.dispatch_outbox();
                const std::size_t after = actor_.fill_count_for(order_id);

                if (s.expect_reject) {
                    if (after != before) return fail(s, "expected the fill to be refused");
                    ++res.refusals;
                    note("FILL_REFUSED " + order_id + " qty=" + s.qty);
                } else {
                    if (after == before) return fail(s, "fill did not happen");
                    ++res.fills;
                    note("FILL " + order_id + " qty=" + s.qty + " px=" + s.price);
                }
                break;
            }
            case ScenarioAction::FinalizeCancel:
            case ScenarioAction::FinalizeReplace: {
                // The request id names the pending request; the chain it belongs
                // to is found through the chain that carries it.
                std::string order_id;
                for (const OrderChain* c : actor_.chains()) {
                    if (c->pending_cl_ord_id == s.order_ref) order_id = c->order_id;
                }
                if (order_id.empty())
                    return fail(s, "no chain has '" + s.order_ref + "' pending");
                ScheduledEvent ev;
                ev.event_id = "FIN-" + s.order_ref;
                ev.due_us = s.at_ms * 1000;
                ev.kind = s.action == ScenarioAction::FinalizeCancel
                              ? EventKind::FinalizeCancel
                              : EventKind::FinalizeReplace;
                ev.order_id = order_id;
                ev.request_id = s.order_ref;
                if (actor_.schedule(ev) != EnqueueResult::Ok)
                    return fail(s, "finalisation event was refused by the scheduler");
                actor_.run_due(clock_.virtual_now_us());
                session_.dispatch_outbox();
                note(std::string(to_string(s.action)) + " " + s.order_ref);
                break;
            }
            case ScenarioAction::Expire: {
                std::string order_id, why;
                if (!resolve_order(s.order_ref, &order_id, &why)) return fail(s, why);
                ScheduledEvent ev;
                ev.event_id = "EXP-" + order_id;
                ev.due_us = s.at_ms * 1000;
                ev.kind = EventKind::Expire;
                ev.order_id = order_id;
                if (actor_.schedule(ev) != EnqueueResult::Ok)
                    return fail(s, "expiry event was refused by the scheduler");
                actor_.run_due(clock_.virtual_now_us());
                session_.dispatch_outbox();
                note("EXPIRE " + order_id);
                break;
            }
            case ScenarioAction::Disconnect: {
                transport_.close();
                session_.on_disconnect(generation_);
                note("DISCONNECT");
                break;
            }
            case ScenarioAction::Reconnect: {
                transport_.reopen();
                session_.on_connected(++generation_);
                session_.on_bytes(build_logon(), generation_);
                note("RECONNECT");
                break;
            }
            case ScenarioAction::ResendRequest: {
                session_.on_bytes(build_resend(s), generation_);
                note("RESEND " + std::to_string(s.begin_seq) + ".." +
                     std::to_string(s.end_seq));
                break;
            }
            case ScenarioAction::Advance: {
                note("ADVANCE " + std::to_string(s.at_ms));
                break;
            }
            case ScenarioAction::Assert: {
                std::string why;
                if (!evaluate_assertion(s.assertion, actor_, order_ids_, &why))
                    return fail(s, why);
                note("ASSERT " + s.assertion);
                break;
            }
        }

        // An invariant break anywhere in the timeline fails the scenario at the
        // step that caused it, not at the end where the cause is lost.
        std::string why;
        if (!actor_.check_invariants(&why)) return fail(s, "invariant violated: " + why);
        if (actor_.state() == ActorState::Halted)
            return fail(s, "actor halted: " + actor_.halt_detail());
    }

    // Final assertions from the scenario's assert block.
    for (const std::string& a : spec_.assertions) {
        std::string why;
        if (!evaluate_assertion(a, actor_, order_ids_, &why)) {
            res.ok = false;
            res.failure = "final assertion failed: " + why;
            res.trace = trace_;
            res.trace_hash = trace_hash(trace_);
            res.order_ids = order_ids_;
            return res;
        }
    }

    res.ok = true;
    res.trace = trace_;
    res.trace_hash = trace_hash(trace_);
    res.order_ids = order_ids_;
    res.reports = transport_.frames().size();
    return res;
}

std::uint64_t trace_hash(const std::vector<std::string>& trace) {
    std::string joined;
    joined.reserve(trace.size() * 32);
    for (const std::string& line : trace) {
        joined.append(line);
        joined.push_back('\n');
    }
    return canonical_hash64(joined);
}

}  // namespace lpsim
