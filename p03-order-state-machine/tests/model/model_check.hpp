// Bounded exhaustive state exploration (design doc 20.2).
//
// This explores the reachable states of the *pure reducer*, which is what gate
// G1 is about. Working on the reducer rather than the engine is what makes the
// search affordable: an OrderAggregate is a value, so a state can be copied,
// queued and compared, and the two commit outcomes a crash can produce are
// just "take the proposed state" and "do not take it".
//
// The bound is small on purpose: quantities of one to three, at most three
// fills, at most one pending mutation. Inside that bound every interleaving is
// visited, and the twenty-four invariants are asserted on every state that the
// reducer would have committed. Outside it, nothing is proved -- which is why
// the property tests and the scenario catalog exist as well.
#pragma once

#include <deque>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "order_core/reducer.hpp"

namespace model {

using namespace order_core;

struct Bounds {
    int64_t order_qty = 3;
    int64_t max_fill = 2;
    int64_t replace_qty = 3;
    size_t max_states = 200000;
    size_t max_depth = 8;
};

struct Report {
    size_t states_visited = 0;
    size_t transitions = 0;
    size_t committed = 0;
    size_t quarantined = 0;
    size_t duplicates = 0;
    size_t rejected = 0;
    size_t held = 0;
    size_t deepest = 0;
    bool hit_state_limit = false;
    // Populated only on failure.
    std::vector<InvariantViolation> violations;
    std::string failing_trace;
    [[nodiscard]] bool ok() const { return violations.empty(); }
};

// The policy the model runs under: single-unit granularity so quantities of
// one, two and three are admissible.
inline Policy model_policy() {
    Policy p;
    SymbolSpec unit;
    unit.tick_numerator = 1;
    unit.tick_denominator = 100;
    unit.qty_step = 1;
    unit.min_qty = 1;
    unit.max_qty = 1000;
    unit.scale_id = 1;
    p.symbols["UNITFX"] = unit;
    p.business_namespace = "MODEL";
    p.peer_id = "LP1";
    return p;
}

namespace detail {

inline LocalCommand base_command() {
    LocalCommand c;
    c.firm = "FIRM";
    c.business_day = "D1";
    c.account = "ACC1";
    c.symbol = "UNITFX";
    c.side_raw = "1";
    return c;
}

inline PeerReport base_report() {
    PeerReport r;
    r.msg_type = '8';
    r.account = "ACC1";
    r.symbol = "UNITFX";
    r.side_raw = "1";
    r.peer_id = "LP1";
    r.session_id = "S1";
    r.has_source_cursor = true;
    r.source_seq = 1;
    return r;
}

// Identifiers are derived from the state so the alphabet stays finite: the
// same logical step always produces the same identifier, which keeps the
// reachable state set closed.
inline std::string accepted_id(const OrderAggregate& a) {
    return a.has_accepted() ? a.accepted_cl_ord_id() : std::string("O1");
}
inline std::string next_request_id(const OrderAggregate& a, char kind) {
    return std::string(1, kind) + std::to_string(a.accepted_versions.size());
}
inline std::string next_exec_id(const OrderAggregate& a) {
    return "X" + std::to_string(a.ledger.size() + 1);
}
inline std::string last_exec_id(const OrderAggregate& a) {
    return a.ledger.empty() ? std::string("X1") : a.ledger.back().exec_id;
}

// The memo key is the business state plus the dedup history, so two states
// that differ only in what they have already seen are explored separately and
// a dedup bug cannot hide behind an equal business state.
inline std::string memo_key(const OrderAggregate& a) {
    std::string key = a.canonical_text();
    key += "#seen:";
    for (const SeenReport& s : a.seen_reports) {
        key += s.exec_id;
        key.push_back(',');
    }
    return key;
}

}  // namespace detail

// One labelled transition the model can attempt from a given state.
struct Step {
    std::string label;
    OrderEvent event;
};

// Everything worth trying next from this state. Events that make no sense are
// still offered where the catalog cares about the answer (a cancel on a
// terminal order, a trade after a fill), because "refused" is a result the
// model should see.
inline std::vector<Step> alphabet(const OrderAggregate& a, const Bounds& b) {
    using namespace detail;
    std::vector<Step> steps;

    if (!a.has_accepted() && !a.pending.has_value()) {
        LocalCommand c = base_command();
        c.kind = CommandKind::New;
        c.cl_ord_id = "O1";
        c.qty_text = std::to_string(b.order_qty);
        c.price_text = "1.00";
        steps.push_back({"new", OrderEvent{c}});
        return steps;
    }

    if (a.pending.has_value() && a.pending->kind == CommandKind::New) {
        SystemEvent d;
        d.kind = SystemKind::Dispatched;
        d.target_cl_ord_id = a.pending->cl_ord_id;
        steps.push_back({"dispatch", OrderEvent{d}});

        PeerReport ack = base_report();
        ack.exec_type = exec_type::kNew;
        ack.ord_status = ord_status::kNew;
        ack.exec_id = "A1";
        ack.cl_ord_id = a.pending->cl_ord_id;
        ack.order_id = "LPO1";
        steps.push_back({"ack", OrderEvent{ack}});

        PeerReport rej = base_report();
        rej.exec_type = exec_type::kRejected;
        rej.ord_status = ord_status::kRejected;
        rej.exec_id = "J1";
        rej.cl_ord_id = a.pending->cl_ord_id;
        steps.push_back({"reject", OrderEvent{rej}});
    }

    // Fills, including one that would overfill.
    for (int64_t q = 1; q <= b.max_fill; ++q) {
        PeerReport t = base_report();
        t.exec_type = exec_type::kTrade;
        t.ord_status = ord_status::kPartiallyFilled;
        t.exec_id = next_exec_id(a);
        t.cl_ord_id = accepted_id(a);
        t.last_qty = std::to_string(q);
        t.last_px = "0.99";
        steps.push_back({"fill" + std::to_string(q), OrderEvent{t}});
    }
    // A redelivery of the most recent execution.
    if (!a.ledger.empty()) {
        PeerReport t = base_report();
        t.exec_type = exec_type::kTrade;
        t.ord_status = ord_status::kPartiallyFilled;
        t.exec_id = last_exec_id(a);
        t.cl_ord_id = a.ledger.back().cl_ord_id;
        t.last_qty = std::to_string(a.ledger.back().qty_units);
        t.last_px = "0.99";
        steps.push_back({"dup-fill", OrderEvent{t}});
    }

    // Local mutations.
    if (!a.pending.has_value()) {
        LocalCommand c = base_command();
        c.kind = CommandKind::Cancel;
        c.cl_ord_id = next_request_id(a, 'C');
        c.orig_cl_ord_id = accepted_id(a);
        steps.push_back({"cancel-cmd", OrderEvent{c}});

        LocalCommand r = base_command();
        r.kind = CommandKind::Replace;
        r.cl_ord_id = next_request_id(a, 'R');
        r.orig_cl_ord_id = accepted_id(a);
        r.qty_text = std::to_string(b.replace_qty);
        steps.push_back({"replace-cmd", OrderEvent{r}});
    }

    // Peer answers to an outstanding mutation.
    if (a.pending.has_value() && a.pending->kind == CommandKind::Cancel) {
        PeerReport pc = base_report();
        pc.exec_type = exec_type::kPendingCancel;
        pc.ord_status = ord_status::kPendingCancel;
        pc.exec_id = "PC" + a.pending->cl_ord_id;
        pc.cl_ord_id = a.pending->cl_ord_id;
        steps.push_back({"pending-cancel", OrderEvent{pc}});

        PeerReport ca = base_report();
        ca.exec_type = exec_type::kCanceled;
        ca.ord_status = ord_status::kCanceled;
        ca.exec_id = "CA" + a.pending->cl_ord_id;
        ca.cl_ord_id = a.pending->cl_ord_id;
        ca.leaves_qty = "0";
        steps.push_back({"cancel-ack", OrderEvent{ca}});

        PeerReport cr = base_report();
        cr.msg_type = '9';
        cr.cl_ord_id = a.pending->cl_ord_id;
        cr.orig_cl_ord_id = accepted_id(a);
        cr.cxl_rej_response_to = "1";
        steps.push_back({"cancel-reject", OrderEvent{cr}});
    }
    if (a.pending.has_value() && a.pending->kind == CommandKind::Replace) {
        PeerReport pr = base_report();
        pr.exec_type = exec_type::kPendingReplace;
        pr.ord_status = ord_status::kPendingReplace;
        pr.exec_id = "PR" + a.pending->cl_ord_id;
        pr.cl_ord_id = a.pending->cl_ord_id;
        steps.push_back({"pending-replace", OrderEvent{pr}});

        PeerReport ra = base_report();
        ra.exec_type = exec_type::kReplaced;
        ra.ord_status = ord_status::kNew;
        ra.exec_id = "RA" + a.pending->cl_ord_id;
        ra.cl_ord_id = a.pending->cl_ord_id;
        ra.orig_cl_ord_id = accepted_id(a);
        ra.order_qty = std::to_string(a.pending->desired_qty_units);
        steps.push_back({"replace-ack", OrderEvent{ra}});

        PeerReport rr = base_report();
        rr.msg_type = '9';
        rr.cl_ord_id = a.pending->cl_ord_id;
        rr.orig_cl_ord_id = accepted_id(a);
        rr.cxl_rej_response_to = "2";
        steps.push_back({"replace-reject", OrderEvent{rr}});
    }

    // Uncertainty and reconciliation.
    if (a.pending.has_value()) {
        SystemEvent t;
        t.kind = SystemKind::AckTimeout;
        t.target_cl_ord_id = a.pending->cl_ord_id;
        steps.push_back({"timeout", OrderEvent{t}});
    }
    if (a.has_accepted()) {
        PeerReport st = base_report();
        st.exec_type = exec_type::kOrderStatus;
        st.ord_status = a.cum_qty_units > 0 ? ord_status::kPartiallyFilled : ord_status::kNew;
        st.exec_id = "ST" + std::to_string(a.cum_qty_units);
        st.cl_ord_id = accepted_id(a);
        st.order_id = "LPO1";
        st.cum_qty = std::to_string(a.cum_qty_units);
        steps.push_back({"status", OrderEvent{st}});
    }

    return steps;
}

// Breadth-first over the reachable state graph, asserting the invariant set on
// every state the reducer would have committed. Each transition is explored
// under both commit outcomes a crash can produce, which is what makes the
// "either before or after, never partial" claim hold by construction here.
inline Report explore(const Bounds& b = Bounds{}) {
    const Policy policy = model_policy();
    Report report;

    struct Node {
        OrderAggregate state;
        size_t depth = 0;
        std::string trace;
    };

    std::set<std::string> seen;
    std::deque<Node> queue;

    OrderAggregate start;
    start.id = OrderId{1};
    queue.push_back({start, 0, ""});
    seen.insert(detail::memo_key(start));

    while (!queue.empty()) {
        const Node node = queue.front();
        queue.pop_front();
        ++report.states_visited;
        report.deepest = std::max(report.deepest, node.depth);
        if (node.depth >= b.max_depth) continue;
        if (report.states_visited > b.max_states) {
            report.hit_state_limit = true;
            break;
        }

        for (const Step& step : alphabet(node.state, b)) {
            ++report.transitions;
            const Reduction r = reduce(node.state, step.event, policy,
                                      static_cast<uint64_t>(node.depth) + 1);
            switch (r.decision) {
                case Decision::Applied: ++report.committed; break;
                case Decision::Quarantined: ++report.quarantined; break;
                case Decision::DuplicateNoop: ++report.duplicates; break;
                case Decision::RejectLocal: ++report.rejected; break;
                case Decision::HeldForCausality: ++report.held; break;
                case Decision::RetryCommit: break;
            }

            const bool commits =
                r.decision == Decision::Applied || r.decision == Decision::Quarantined;
            if (!commits) continue;  // this is the "crash before commit" branch

            // Every committed state must satisfy the whole safety set.
            const std::vector<InvariantViolation> v = check_invariants(r.next, policy);
            if (!v.empty()) {
                report.violations = v;
                report.failing_trace = node.trace + " -> " + step.label;
                return report;
            }
            // And the reducer must never have produced a violation it then
            // committed anyway.
            if (r.decision == Decision::Applied && !r.violations.empty()) {
                report.violations = r.violations;
                report.failing_trace = node.trace + " -> " + step.label + " (committed dirty)";
                return report;
            }

            const std::string key = detail::memo_key(r.next);
            if (seen.insert(key).second) {
                queue.push_back({r.next, node.depth + 1,
                                 node.trace.empty() ? step.label : node.trace + " -> " + step.label});
            }
        }
    }
    return report;
}

}  // namespace model
