// Group A -- New order, first acknowledgement and reject (design doc 18.A).
#include "../support/lab.hpp"

using namespace lab;

SCENARIO("A-01", "P0", "a first valid New creates one prepared chain, nothing accepted") {
    Lab L;
    const EngineResult r = L.submit(cmd_new("O1", "100000", "1.08500"));
    CHECK(r.ok());
    CHECK_EQ(L.engine().chain_count(), size_t{1});
    const OrderAggregate& a = L.state(r.id);
    CHECK_EQ(a.lifecycle(), Lifecycle::LocalPreparing);
    // Normalized, not rounded: 1.08500 at a 0.00001 tick is exactly 108500.
    CHECK(a.pending.has_value());
    CHECK_EQ(a.pending->desired_qty_units, int64_t{100000});
    CHECK_EQ(a.pending->desired_price_ticks, int64_t{108500});
    // Nothing the counterparty has said yet.
    CHECK(!a.has_accepted());
    CHECK_EQ(a.cum_qty_units, int64_t{0});
    CHECK_EQ(a.pending->phase, PendingRequest::Phase::Prepared);
}

SCENARIO("A-02", "P0", "the same New submitted twice before dispatch is one request") {
    Lab L;
    const EngineResult first = L.submit(cmd_new("O1"));
    CHECK(first.ok());
    const EngineResult second = L.submit(cmd_new("O1"));
    expect(second, Decision::DuplicateNoop, Reason::DuplicateSamePayload);
    CHECK_EQ(L.engine().chain_count(), size_t{1});
    CHECK_EQ(second.id.value, first.id.value);
    // Exactly one routing effect left the box.
    CHECK_EQ(L.engine().sink().count_key("route-new|FIRM|2026-10-30|O1"), size_t{1});
}

SCENARIO("A-03", "P0", "the same ClOrdID with a different quantity is a collision") {
    Lab L;
    CHECK(L.submit(cmd_new("O1", "100000")).ok());
    expect_reject(L.submit(cmd_new("O1", "200000")), Reason::IdCollision);
    CHECK_EQ(L.engine().chain_count(), size_t{1});
}

SCENARIO("A-04", "P0", "New with quantity zero is refused") {
    Lab L;
    expect_reject(L.submit(cmd_new("O1", "0")), Reason::InvalidQty);
    CHECK_EQ(L.engine().chain_count(), size_t{0});
    CHECK_EQ(L.engine().sink().dispatched().size(), size_t{0});
}

SCENARIO("A-05", "P0", "New with a negative quantity is refused, not cast to unsigned") {
    Lab L;
    expect_reject(L.submit(cmd_new("O1", "-100000")), Reason::InvalidQty);
    CHECK_EQ(L.engine().chain_count(), size_t{0});
}

SCENARIO("A-06", "P0", "New with price zero is refused under a positive-price profile") {
    Lab L;
    expect_reject(L.submit(cmd_new("O1", "100000", "0")), Reason::InvalidPrice);
    CHECK_EQ(L.engine().chain_count(), size_t{0});
}

SCENARIO("A-07", "P0", "New with a side outside buy/sell is refused") {
    Lab L;
    LocalCommand c = cmd_new("O1");
    c.side_raw = "7";
    expect_reject(L.submit(c), Reason::InvalidSide);
}

SCENARIO("A-08", "P0", "New missing symbol, firm or account is refused with the field named") {
    Lab L;
    LocalCommand no_symbol = cmd_new("O1");
    no_symbol.symbol.clear();
    expect_reject(L.submit(no_symbol), Reason::MissingField);

    LocalCommand no_account = cmd_new("O2");
    no_account.account.clear();
    expect_reject(L.submit(no_account), Reason::MissingField);

    LocalCommand no_firm = cmd_new("O3");
    no_firm.firm.clear();
    expect_reject(L.submit(no_firm), Reason::MissingField);
    CHECK_EQ(L.engine().chain_count(), size_t{0});
}

SCENARIO("A-09", "P0", "New on a symbol with no pinned scale is refused, no tick is invented") {
    Lab L;
    expect_reject(L.submit(cmd_new("O1", "100000", "1.08500", "GBPCHF")), Reason::UnknownSymbol);
}

SCENARIO("A-10", "P0", "a committed dispatch intent reaches sent-unknown, never working") {
    Lab L;
    const EngineResult r = L.submit(cmd_new("O1"));
    CHECK(L.submit(sys(SystemKind::Dispatched, "O1")).ok());
    const OrderAggregate& a = L.state(r.id);
    CHECK_EQ(a.lifecycle(), Lifecycle::NewSentUnknown);
    CHECK_EQ(a.base, BaseState::SentUnknown);
    CHECK(!a.has_accepted());
}

SCENARIO("A-11", "P0", "a valid New Ack installs accepted version zero with no fill") {
    Lab L;
    const EngineResult r = L.submit(cmd_new("O1", "100000", "1.08500"));
    CHECK(L.submit(sys(SystemKind::Dispatched, "O1")).ok());
    CHECK(L.submit(new_ack("O1")).ok());

    const OrderAggregate& a = L.state(r.id);
    CHECK_EQ(a.lifecycle(), Lifecycle::Working);
    CHECK_EQ(a.accepted_versions.size(), size_t{1});
    CHECK_EQ(a.accepted_versions[0].version, uint32_t{0});
    CHECK_EQ(a.accepted_cl_ord_id(), std::string("O1"));
    CHECK_EQ(a.accepted_qty_units(), int64_t{100000});
    CHECK_EQ(a.accepted_price_ticks(), int64_t{108500});
    CHECK_EQ(a.cum_qty_units, int64_t{0});
    CHECK_EQ(a.leaves_qty_units(), int64_t{100000});
    CHECK(!a.pending.has_value());
}

SCENARIO("A-12", "P0", "an Ack arriving before the dispatch callback correlates by identity") {
    Lab L;
    const EngineResult r = L.submit(cmd_new("O1"));
    // No Dispatched notice yet: the outbox intent is durable, the callback is
    // simply not back. The Ack must still land on the same chain.
    CHECK(L.submit(new_ack("O1")).ok());
    CHECK_EQ(L.engine().chain_count(), size_t{1});
    CHECK_EQ(L.state(r.id).accepted_versions.size(), size_t{1});
}

SCENARIO("A-13", "P0", "an Ack naming a different account is quarantined") {
    Lab L;
    const EngineResult r = L.submit(cmd_new("O1"));
    expect_quarantine(L.submit(new_ack("O1").acct("ACC9")), Reason::AccountMismatch);
    const OrderAggregate& a = L.state(r.id);
    CHECK(!a.has_accepted());
    CHECK_EQ(a.account, std::string("ACC1"));
    CHECK(a.quarantined);
}

SCENARIO("A-14", "P0", "an Ack naming a different symbol is quarantined") {
    Lab L;
    const EngineResult r = L.submit(cmd_new("O1"));
    expect_quarantine(L.submit(new_ack("O1").sym("USDJPY")), Reason::SymbolMismatch);
    CHECK_EQ(L.state(r.id).symbol, std::string("EURUSD"));
    CHECK(!L.state(r.id).has_accepted());
}

SCENARIO("A-15", "P0", "an Ack without OrderID(37) is quarantined, not accepted") {
    Lab L;
    const EngineResult r = L.submit(cmd_new("O1"));
    expect_quarantine(L.submit(new_ack("O1").no_oid()), Reason::IdentityInsufficient);
    CHECK(!L.state(r.id).has_accepted());
}

SCENARIO("A-16", "P0", "a Reject before any Ack is terminal with zero leaves") {
    Lab L;
    const EngineResult r = L.submit(cmd_new("O1"));
    CHECK(L.submit(sys(SystemKind::Dispatched, "O1")).ok());
    CHECK(L.submit(rejected("O1").text("limit too far")).ok());

    const OrderAggregate& a = L.state(r.id);
    CHECK_EQ(a.lifecycle(), Lifecycle::Rejected);
    CHECK_EQ(a.leaves_qty_units(), int64_t{0});
    CHECK_EQ(a.cum_qty_units, int64_t{0});
    CHECK(a.terminal());
}

SCENARIO("A-17", "P0", "a Reject after a recorded fill is quarantined and the ledger survives") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "40000");
    expect_quarantine(L.submit(rejected("O1", "E-REJ2")), Reason::ContradictoryResponse);

    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.cum_qty_units, int64_t{40000});
    CHECK_EQ(a.ledger.size(), size_t{1});
    CHECK(a.done_reason != DoneReason::Rejected);
    CHECK_EQ(oracle(L.store(), id).cum, int64_t{40000});
}

SCENARIO("A-18", "P0", "a New timeout yields unknown outcome and a single status query") {
    Lab L;
    const EngineResult r = L.submit(cmd_new("O1"));
    CHECK(L.submit(sys(SystemKind::Dispatched, "O1")).ok());
    const EngineResult t = L.submit(sys(SystemKind::AckTimeout, "O1"));
    CHECK(t.ok());
    CHECK_EQ(t.reason, Reason::UnknownOutcome);

    const OrderAggregate& a = L.state(r.id);
    CHECK_EQ(a.lifecycle(), Lifecycle::ReconciliationRequired);
    CHECK_EQ(a.confidence, Confidence::Unknown);
    CHECK(a.reconciliation_required);
    CHECK(a.pending.has_value());
    CHECK(a.pending->outcome_unknown);
    // A status query, not a second order.
    CHECK_EQ(L.engine().sink().count_key("query-status|1|O1"), size_t{1});
    CHECK_EQ(L.engine().sink().count_key("route-new|FIRM|2026-10-30|O1"), size_t{1});
    // And a retry under a fresh ClOrdID is refused while the outcome is unknown.
    expect_reject(L.submit(cmd_cancel("C1", "O1")), Reason::UnsafeDependentCommand);
}

SCENARIO("A-19", "P0", "an Ack arriving after the timeout resolves the uncertainty exactly once") {
    Lab L;
    const EngineResult r = L.submit(cmd_new("O1"));
    CHECK(L.submit(sys(SystemKind::Dispatched, "O1")).ok());
    CHECK(L.submit(sys(SystemKind::AckTimeout, "O1")).ok());
    CHECK(L.submit(new_ack("O1")).ok());

    const OrderAggregate& a = L.state(r.id);
    CHECK_EQ(a.lifecycle(), Lifecycle::Working);
    CHECK_EQ(a.confidence, Confidence::Confirmed);
    CHECK(!a.reconciliation_required);
    CHECK_EQ(a.accepted_versions.size(), size_t{1});

    // Delivered a second time it is a no-op, not a second accepted version.
    expect_decision(L.submit(new_ack("O1")), Decision::DuplicateNoop);
    CHECK_EQ(L.state(r.id).accepted_versions.size(), size_t{1});
}

SCENARIO("A-20", "P0", "an Ack claiming CumQty with no execution detail is quarantined") {
    Lab L;
    const EngineResult r = L.submit(cmd_new("O1"));
    expect_quarantine(L.submit(new_ack("O1").cum("40000")), Reason::MissingFillEvidence);

    const OrderAggregate& a = L.state(r.id);
    CHECK_EQ(a.cum_qty_units, int64_t{0});
    CHECK_EQ(a.ledger.size(), size_t{0});
    CHECK(!a.has_accepted());
}
