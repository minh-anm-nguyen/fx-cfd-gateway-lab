// G03 — Inbox, FIX raw receipt and the application ACK boundary.
//
// The boundary between P01 and P06 is at-least-once delivery across two
// databases (section 6's open blocker). These cases are about what that
// costs and what makes it safe: dedup on the transport key, an ACK that
// happens only after the business commit, and redelivery that is a no-op
// rather than a second effect.
#include "jrnl/failpoint.hpp"
#include "lab.hpp"

using namespace lab;

namespace {

// Lab owns a mutex, so it is neither copyable nor movable; the fixture is
// built in place and seeded here.
void seed_order(Lab& l, std::int64_t qty = 100000) {
    l.start();
    const CommitOutcome o = l.service().submit_new(l.new_order("K1", "O1", qty), l.token());
    if (o.kind != CommitKind::Committed) TT_FAIL("setup order did not commit: " + o.detail);
}

SCENARIO("G03-01", "P0", "a receipt applied once moves the order once") {
    Lab l("g03-01");
    seed_order(l);
    const auto r = l.fill("O1", "E100", 40000, 10000, "K1");
    CHECK_EQ(l.deliver(r).kind, CommitKind::Committed);

    CHECK_EQ(l.order("O1").cum_qty, std::int64_t(40000));
    CHECK_EQ(l.row_count("inbox_receipts"), std::size_t(1));
    CHECK_EQ(l.row_count("fills"), std::size_t(1));
    expect_consistent(l, "G03-01");
}

SCENARIO("G03-02", "P0", "the same receipt delivered twice applies once") {
    Lab l("g03-02");
    seed_order(l);
    const auto r = l.fill("O1", "E100", 40000, 10000, "K1");

    CHECK_EQ(l.deliver(r).kind, CommitKind::Committed);
    const CommitOutcome again = l.deliver(r);
    CHECK_EQ(again.kind, CommitKind::Replayed);

    CHECK_EQ(l.order("O1").cum_qty, std::int64_t(40000));
    CHECK_EQ(l.row_count("fills"), std::size_t(1));
    // And the independent counterparty ledger agrees there was one trade.
    CHECK_EQ(l.lp().economic_trade_count("E100"), std::size_t(1));
    expect_consistent(l, "G03-02");
}

SCENARIO("G03-03", "P0", "a receipt redelivered with different bytes is quarantined") {
    Lab l("g03-03");
    seed_order(l);
    auto r = l.fill("O1", "E100", 40000, 10000, "K1");
    CHECK_EQ(l.deliver(r).kind, CommitKind::Committed);

    // Same transport identity, different content. JI-07: neither version is
    // chosen, because choosing would be guessing about a trade.
    auto forged = r;
    forged.raw = r.raw + "|tampered";
    forged.last_qty = 90000;
    const CommitOutcome o = l.deliver(forged);
    CHECK_EQ(o.kind, CommitKind::Quarantined);
    CHECK_EQ(l.order("O1").cum_qty, std::int64_t(40000));
    expect_consistent(l, "G03-03");
}

SCENARIO("G03-04", "P0", "P01 holds the raw message until P06 acknowledges") {
    Lab l("g03-04");
    seed_order(l);
    const auto r = l.fill("O1", "E100", 40000, 10000, "K1");

    RawReceipt raw;
    raw.session_key = r.session_key;
    raw.session_epoch = r.session_epoch;
    raw.receipt_id = r.receipt_id;
    raw.raw = r.raw;
    raw.raw_hash = sha256(r.raw);
    CHECK_EQ(l.link().persist_raw(raw), StoreError::None);
    CHECK_EQ(l.link().unacked_count(Lab::kSession), std::size_t(1));

    // Handed over but not yet acknowledged: still redeliverable.
    l.link().mark_delivered(r.session_key, r.session_epoch, r.receipt_id);
    CHECK_EQ(l.link().undelivered(Lab::kSession, 10).size(), std::size_t(1));

    CHECK_EQ(l.service().apply_report(r, l.token()).kind, CommitKind::Committed);
    CHECK_EQ(l.service().ack_delivery(r), StoreError::None);
    CHECK_EQ(l.link().unacked_count(Lab::kSession), std::size_t(0));
    expect_consistent(l, "G03-04");
}

SCENARIO("G03-05", "P0", "a lost ACK causes redelivery and a no-op, not a second fill") {
    // W21 and JI-26 together. The business transaction committed; the
    // acknowledgement did not reach P01.
    Lab l("g03-05");
    seed_order(l);
    const auto r = l.fill("O1", "E100", 40000, 10000, "K1");

    RawReceipt raw;
    raw.session_key = r.session_key;
    raw.session_epoch = r.session_epoch;
    raw.receipt_id = r.receipt_id;
    raw.raw = r.raw;
    raw.raw_hash = sha256(r.raw);
    l.link().persist_raw(raw);
    l.lp_executes(r);

    CHECK_EQ(l.service().apply_report(r, l.token()).kind, CommitKind::Committed);

    fp::disarm_all();
    fp::arm(std::string(fp::point::kBeforeDeliveryAck), fp::Action::ReturnError);
    CHECK_NE(l.service().ack_delivery(r), StoreError::None);
    fp::disarm_all();

    // P01 still owes it, so it comes round again.
    CHECK_EQ(l.link().unacked_count(Lab::kSession), std::size_t(1));
    const auto pending = l.link().undelivered(Lab::kSession, 10);
    CHECK_EQ(pending.size(), std::size_t(1));

    CHECK_EQ(l.service().apply_report(r, l.token()).kind, CommitKind::Replayed);
    CHECK_EQ(l.order("O1").cum_qty, std::int64_t(40000));
    CHECK_EQ(l.row_count("fills"), std::size_t(1));
    expect_consistent(l, "G03-05");
}

SCENARIO("G03-06", "P0", "a receipt id is only unique within its session epoch") {
    Lab l("g03-06");
    seed_order(l);
    auto first = l.fill("O1", "E100", 10000, 10000, "K1");
    first.receipt_id = 7;
    CHECK_EQ(l.deliver(first).kind, CommitKind::Committed);

    // After a reconnect the counterparty numbers from scratch. The same
    // receipt id in a new epoch is a different message, and treating it as
    // a duplicate would silently drop a real fill.
    std::uint64_t epoch = 0;
    CHECK_EQ(l.link().bump_epoch(Lab::kSession, &epoch), StoreError::None);
    CHECK_EQ(epoch, std::uint64_t(2));

    auto second = l.fill("O1", "E200", 20000, 10000, "K1");
    second.receipt_id = 7;
    second.session_epoch = epoch;
    CHECK_EQ(l.deliver(second).kind, CommitKind::Committed);

    CHECK_EQ(l.order("O1").cum_qty, std::int64_t(30000));
    CHECK_EQ(l.row_count("inbox_receipts"), std::size_t(2));
    expect_consistent(l, "G03-06");
}

SCENARIO("G03-07", "P0", "the inbox insert and the business apply commit together") {
    // W19: a crash between the inbox row and the projection must leave
    // neither, or P06 would believe it had applied something it had not.
    Lab l("g03-07");
    seed_order(l);
    const auto r = l.fill("O1", "E100", 40000, 10000, "K1");

    fp::disarm_all();
    fp::arm(std::string(fp::point::kAfterOrderProjection), fp::Action::ReturnError);
    const CommitOutcome o = l.deliver(r, /*ack=*/false);
    fp::disarm_all();

    CHECK_NE(o.kind, CommitKind::Committed);
    CHECK_EQ(l.row_count("inbox_receipts"), std::size_t(0));
    CHECK_EQ(l.order("O1").cum_qty, std::int64_t(0));
    // P01 still holds it, so it will be redelivered.
    CHECK_EQ(l.link().unacked_count(Lab::kSession), std::size_t(1));
    expect_consistent(l, "G03-07");
}

SCENARIO("G03-08", "P0", "a report for an unknown chain is quarantined, not dropped") {
    Lab l("g03-08");
    seed_order(l);
    const auto r = l.fill("O-nonexistent", "E999", 1000, 10000, "");
    const CommitOutcome o = l.deliver(r);

    CHECK_EQ(o.kind, CommitKind::Quarantined);
    // The evidence is kept: a receipt row marked QUARANTINED and an open case.
    CHECK_EQ(l.scalar("SELECT COUNT(*) FROM inbox_receipts WHERE apply_state='QUARANTINED';"),
             std::int64_t(1));
    CHECK_EQ(l.store().open_cases(Lab::kTenant).size(), std::size_t(1));
    expect_consistent(l, "G03-08");
}

SCENARIO("G03-09", "P0", "a quarantined receipt is not reprocessed on redelivery") {
    Lab l("g03-09");
    seed_order(l);
    const auto r = l.fill("O-nonexistent", "E999", 1000, 10000, "");
    CHECK_EQ(l.deliver(r).kind, CommitKind::Quarantined);
    const std::size_t cases = l.store().open_cases(Lab::kTenant).size();

    CHECK_EQ(l.deliver(r).kind, CommitKind::Replayed);
    CHECK_EQ(l.store().open_cases(Lab::kTenant).size(), cases);
    CHECK_EQ(l.row_count("inbox_receipts"), std::size_t(1));
    expect_consistent(l, "G03-09");
}

SCENARIO("G03-10", "P0", "a PossDup redelivery of a business event still applies once") {
    // Section 11: PossDupFlag=Y does not mean the business event was
    // applied. Dedup on the business execution key is what decides.
    Lab l("g03-10");
    seed_order(l);
    auto first = l.fill("O1", "E100", 40000, 10000, "K1");
    CHECK_EQ(l.deliver(first).kind, CommitKind::Committed);

    // A new transport identity carrying the same execution.
    auto possdup = l.fill("O1", "E100", 40000, 10000, "K1");
    possdup.raw = first.raw + "|43=Y";
    const CommitOutcome o = l.deliver(possdup);
    CHECK_EQ(o.kind, CommitKind::Replayed);

    CHECK_EQ(l.order("O1").cum_qty, std::int64_t(40000));
    CHECK_EQ(l.row_count("fills"), std::size_t(1));
    CHECK_EQ(l.lp().economic_trade_count("E100"), std::size_t(1));
    expect_consistent(l, "G03-10");
}

SCENARIO("G03-11", "P1", "many unacknowledged receipts stay bounded and ordered") {
    Lab l("g03-11");
    seed_order(l, 1000000);
    for (int i = 0; i < 50; ++i) {
        RawReceipt raw;
        raw.session_key = Lab::kSession;
        raw.session_epoch = 1;
        raw.receipt_id = 1000 + static_cast<std::uint64_t>(i);
        raw.raw = "35=8|n=" + std::to_string(i);
        raw.raw_hash = sha256(raw.raw);
        CHECK_EQ(l.link().persist_raw(raw), StoreError::None);
    }
    CHECK_EQ(l.link().unacked_count(Lab::kSession), std::size_t(50));

    // The reader is bounded and returns them in order, so a backlog is
    // drained predictably rather than all at once (section 19).
    const auto page = l.link().undelivered(Lab::kSession, 10);
    CHECK_EQ(page.size(), std::size_t(10));
    CHECK_EQ(page.front().receipt_id, std::uint64_t(1000));
    CHECK_EQ(page.back().receipt_id, std::uint64_t(1009));
}

SCENARIO("G03-12", "P0", "persisting the raw message twice keeps one copy") {
    Lab l("g03-12");
    seed_order(l);
    RawReceipt raw;
    raw.session_key = Lab::kSession;
    raw.session_epoch = 1;
    raw.receipt_id = 55;
    raw.raw = "35=8|x=1";
    raw.raw_hash = sha256(raw.raw);
    CHECK_EQ(l.link().persist_raw(raw), StoreError::None);
    CHECK_EQ(l.link().persist_raw(raw), StoreError::None);
    CHECK_EQ(l.link().unacked_count(Lab::kSession), std::size_t(1));
}

SCENARIO("G03-13", "P0", "an acknowledged receipt is not redelivered") {
    Lab l("g03-13");
    seed_order(l);
    const auto r = l.fill("O1", "E100", 40000, 10000, "K1");
    CHECK_EQ(l.deliver(r).kind, CommitKind::Committed);
    CHECK_EQ(l.link().undelivered(Lab::kSession, 10).size(), std::size_t(0));
    expect_consistent(l, "G03-13");
}

SCENARIO("G03-14", "P0", "a receipt surviving a process restart is applied exactly once") {
    Lab l("g03-14");
    seed_order(l);
    const auto r = l.fill("O1", "E100", 40000, 10000, "K1");

    RawReceipt raw;
    raw.session_key = r.session_key;
    raw.session_epoch = r.session_epoch;
    raw.receipt_id = r.receipt_id;
    raw.raw = r.raw;
    raw.raw_hash = sha256(r.raw);
    l.link().persist_raw(raw);
    // W18: durable at P01, never handed over. The process dies here.

    l.restart();
    l.start();
    CHECK_EQ(l.link().unacked_count(Lab::kSession), std::size_t(1));

    CHECK_EQ(l.service().apply_report(r, l.token()).kind, CommitKind::Committed);
    CHECK_EQ(l.service().ack_delivery(r), StoreError::None);
    CHECK_EQ(l.order("O1").cum_qty, std::int64_t(40000));
    CHECK_EQ(l.row_count("fills"), std::size_t(1));
    expect_consistent(l, "G03-14");
}

SCENARIO_BLOCKED("G03-15", "P0", "P01 truncates its raw inbox before P06 acknowledges",
                 "the retention guard that would prevent this lives in P01, which is modelled "
                 "here only as far as its contract reaches. Enforcing it needs the real P01 "
                 "session engine from project 01.")

SCENARIO_BLOCKED("G03-16", "P1", "a FIX peer resends with a different MsgSeqNum",
                 "MsgSeqNum handling is P01's; this project consumes normalised receipts and "
                 "its defence is the business execution key, which G08 covers.")

}  // namespace

int main(int argc, char** argv) { return tt::run_all(argc, argv, "test_inbox"); }
