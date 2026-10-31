// G05 — Client commit acknowledgements, retries and idempotency.
//
// The distinction section 9.3 insists on runs through all of these: an
// aborted transaction left nothing and may be retried; an unknown commit
// may have left everything and must be resolved by key first. Treating the
// second as the first is how one command becomes two orders.
#include "jrnl/failpoint.hpp"
#include "lab.hpp"

using namespace lab;

namespace {

SCENARIO("G05-01", "P0", "an accepted command returns ACCEPTED_DURABLE once") {
    Lab l("g05-01");
    l.start();
    const CommitOutcome o = l.service().submit_new(l.new_order("K1", "O1", 1000), l.token());
    CHECK_EQ(o.kind, CommitKind::Committed);
    CHECK(o.durable());
    CHECK(!o.tx.empty());
    expect_consistent(l, "G05-01");
}

SCENARIO("G05-02", "P0", "a retry with the same key returns the earlier durable result") {
    Lab l("g05-02");
    l.start();
    const auto req = l.new_order("K1", "O1", 1000);
    const CommitOutcome first = l.service().submit_new(req, l.token());
    const CommitOutcome second = l.service().submit_new(req, l.token());

    CHECK_EQ(second.kind, CommitKind::Replayed);
    CHECK(second.durable());
    CHECK_EQ(second.tx, first.tx);
    CHECK_EQ(l.row_count("orders"), std::size_t(1));
    CHECK_EQ(l.row_count("command_outbox"), std::size_t(1));
    expect_consistent(l, "G05-02");
}

SCENARIO("G05-03", "P0", "the same key with different content is a conflict") {
    Lab l("g05-03");
    l.start();
    l.service().submit_new(l.new_order("K1", "O1", 1000), l.token());
    const CommitOutcome o = l.service().submit_new(l.new_order("K1", "O1", 2000), l.token());
    CHECK_EQ(o.kind, CommitKind::Conflict);
    CHECK(!o.durable());
    CHECK_EQ(l.order("O1").order_qty, std::int64_t(1000));
    expect_consistent(l, "G05-03");
}

SCENARIO("G05-04", "P0", "COMMIT_UNKNOWN is neither a rollback nor an acceptance") {
    // W11 and JI-15. The data is durable and the caller does not know.
    Lab l("g05-04");
    l.start();

    fp::disarm_all();
    fp::arm(std::string(fp::point::kDuringCommit), fp::Action::ReturnError);
    const CommitOutcome o = l.service().submit_new(l.new_order("K1", "O1", 1000), l.token());
    fp::disarm_all();

    CHECK_EQ(o.kind, CommitKind::Unknown);
    CHECK_EQ(o.error, StoreError::CommitUnknown);
    CHECK(!o.durable());
    CHECK(is_ambiguous(o.error));
    // Not retryable: section 9.3 separates it from an abort precisely so a
    // caller cannot re-enter as if nothing had happened.
    CHECK(!is_retryable(o.error));
    CHECK_EQ(l.metrics().get(metric::kDbCommitUnknown), std::int64_t(1));
    expect_consistent(l, "G05-04");
}

SCENARIO("G05-05", "P0", "an unknown commit is resolved by looking the key up") {
    Lab l("g05-05");
    l.start();
    fp::disarm_all();
    fp::arm(std::string(fp::point::kDuringCommit), fp::Action::ReturnError);
    const CommitOutcome o = l.service().submit_new(l.new_order("K1", "O1", 1000), l.token());
    fp::disarm_all();
    CHECK_EQ(o.kind, CommitKind::Unknown);

    // The prescribed next step: query by the idempotency key before any
    // decision about retrying.
    const CommandRecord rec = l.store().lookup(Lab::kTenant, Lab::kAccount, "K1");
    CHECK(rec.found);
    CHECK_EQ(rec.decision, std::string("ACCEPTED"));
    CHECK_EQ(rec.chain, std::string("O1"));

    // Resubmitting the same key now correctly reports the existing result
    // rather than placing a second order.
    CHECK_EQ(l.service().submit_new(l.new_order("K1", "O1", 1000), l.token()).kind,
             CommitKind::Replayed);
    CHECK_EQ(l.row_count("orders"), std::size_t(1));
    expect_consistent(l, "G05-05");
}

SCENARIO("G05-06", "P0", "an unknown commit survives a restart and is still one order") {
    // W12: the process died between COMMIT and the reply to the client.
    Lab l("g05-06");
    l.start();
    fp::disarm_all();
    fp::arm(std::string(fp::point::kDuringCommit), fp::Action::ReturnError);
    l.service().submit_new(l.new_order("K1", "O1", 1000), l.token());
    fp::disarm_all();

    l.restart();
    l.start();
    const CommandRecord rec = l.store().lookup(Lab::kTenant, Lab::kAccount, "K1");
    CHECK(rec.found);
    CHECK_EQ(l.row_count("orders"), std::size_t(1));
    CHECK_EQ(l.row_count("command_outbox"), std::size_t(1));
    expect_consistent(l, "G05-06");
}

SCENARIO("G05-07", "P0", "a rejected command leaves no trace to look up") {
    Lab l("g05-07", /*limit=*/500);
    l.start();
    const CommitOutcome o = l.service().submit_new(l.new_order("K1", "O1", 5000), l.token());
    CHECK_EQ(o.kind, CommitKind::Rejected);
    CHECK(!l.store().lookup(Lab::kTenant, Lab::kAccount, "K1").found);
    CHECK_EQ(l.row_count("orders"), std::size_t(0));
    expect_consistent(l, "G05-07");
}

SCENARIO("G05-08", "P0", "a retry derives the same transaction id") {
    // JI-27: the retry is the same transaction, not a new one that happens
    // to look similar. A different id would create a second manifest.
    Lab l("g05-08");
    l.start();
    const auto req = l.new_order("K1", "O1", 1000);
    const Digest key = GatewayService::canonical_hash(req);
    const TxId expected = derive_tx_id(Lab::kTenant, "new", key);

    const CommitOutcome o = l.service().submit_new(req, l.token());
    CHECK_EQ(o.tx, expected);
    CHECK_EQ(l.row_count("journal_transactions"), std::size_t(2));  // limit + new order
    CHECK_EQ(l.service().submit_new(req, l.token()).tx, expected);
    CHECK_EQ(l.row_count("journal_transactions"), std::size_t(2));
    expect_consistent(l, "G05-08");
}

SCENARIO("G05-09", "P0", "the canonical hash covers every field that changes the order") {
    const Lab l("g05-09");
    const auto base = l.new_order("K1", "O1", 1000, 10000);
    const Digest h = GatewayService::canonical_hash(base);

    auto qty = base;
    qty.order_qty = 1001;
    CHECK_NE(to_hex(GatewayService::canonical_hash(qty)), to_hex(h));

    auto px = base;
    px.limit_px = 10001;
    CHECK_NE(to_hex(GatewayService::canonical_hash(px)), to_hex(h));

    auto side = base;
    side.side = "SELL";
    CHECK_NE(to_hex(GatewayService::canonical_hash(side)), to_hex(h));

    auto sym = base;
    sym.symbol = "GBPUSD";
    CHECK_NE(to_hex(GatewayService::canonical_hash(sym)), to_hex(h));

    auto domain = base;
    domain.risk_domain = "D2";
    CHECK_NE(to_hex(GatewayService::canonical_hash(domain)), to_hex(h));

    // The session is transport, not business: it is deliberately outside
    // the business identity of the command.
    auto session = base;
    session.session_key = "S2";
    CHECK_EQ(to_hex(GatewayService::canonical_hash(session)), to_hex(h));
}

SCENARIO("G05-10", "P0", "two accounts may use the same command id") {
    Lab l("g05-10");
    l.start();
    CHECK_EQ(l.service().submit_new(l.new_order("K1", "O1", 1000), l.token()).kind,
             CommitKind::Committed);

    auto other = l.new_order("K1", "O2", 1000);
    other.account = "A2";
    CHECK_EQ(l.service().submit_new(other, l.token()).kind, CommitKind::Committed);

    // The key is (tenant, account, command), so these do not collide.
    CHECK_EQ(l.row_count("command_intents"), std::size_t(2));
    CHECK_EQ(l.row_count("orders"), std::size_t(2));
    expect_consistent(l, "G05-10");
}

SCENARIO("G05-11", "P0", "a cancel and a new order cannot share a command id") {
    Lab l("g05-11");
    l.start();
    l.service().submit_new(l.new_order("K1", "O1", 1000), l.token());
    // Reusing K1 for a cancel: different canonical content under the same
    // key, which is a conflict rather than a second command.
    const CommitOutcome o = l.service().submit_cancel(l.cancel("K1", "O1"), l.token());
    CHECK_EQ(o.kind, CommitKind::Conflict);
    CHECK(!l.order("O1").pending_cancel);
    expect_consistent(l, "G05-11");
}

SCENARIO("G05-12", "P0", "the retry budget is bounded") {
    // Section 19 and JI-36: a hot row must not be able to spin for ever.
    Lab l("g05-12");
    l.start();
    fp::disarm_all();
    // Fail every attempt at the same boundary. The store gives up with a
    // typed error instead of looping.
    fp::arm(std::string(fp::point::kAfterDedupInsert), fp::Action::ReturnError, 0, 1);
    const CommitOutcome o = l.service().submit_new(l.new_order("K1", "O1", 1000), l.token());
    fp::disarm_all();
    CHECK_NE(o.kind, CommitKind::Committed);
    CHECK_EQ(l.row_count("orders"), std::size_t(0));
    expect_consistent(l, "G05-12");
}

SCENARIO("G05-13", "P0", "a durable result reports the decision that was recorded") {
    Lab l("g05-13");
    l.start();
    l.service().submit_new(l.new_order("K1", "O1", 1000), l.token());
    const CommandRecord rec = l.store().lookup(Lab::kTenant, Lab::kAccount, "K1");
    CHECK(rec.found);
    CHECK_EQ(rec.decision, std::string("ACCEPTED"));
    CHECK_EQ(rec.reservation, GatewayService::reservation_for("O1"));
    CHECK_EQ(to_hex(rec.canonical_hash),
             to_hex(GatewayService::canonical_hash(l.new_order("K1", "O1", 1000))));
}

SCENARIO("G05-14", "P1", "looking up an unknown key is not an error") {
    Lab l("g05-14");
    l.start();
    const CommandRecord rec = l.store().lookup(Lab::kTenant, Lab::kAccount, "never-sent");
    CHECK(!rec.found);
    CHECK(rec.decision.empty());
}

SCENARIO_BLOCKED("G05-15", "P0", "a client reuses an expired idempotency key after its TTL",
                 "the tombstone policy section 18 asks for is implemented as never pruning "
                 "command_intents, which G19 asserts. Exercising an actual expiry needs a TTL "
                 "the design deliberately does not have in v1.")

}  // namespace

int main(int argc, char** argv) { return tt::run_all(argc, argv, "test_command_idempotency"); }
