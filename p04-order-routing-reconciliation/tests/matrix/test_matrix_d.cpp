// Section D: FIX 4.4 application encoding, parsing and schema.
#include <string>

#include "router/fix44.hpp"
#include "support/lab.hpp"

using namespace router;
using lab::Lab;

namespace {

// The standard working order used by this section.
struct Fixture {
    Lab lab;
    AcceptedIntent order;
    Fixture() {
        order = lab.must_submit_new("K1");
        lab.drive();
    }
};

}  // namespace

SCENARIO("OR-D-01", "P0", "the golden NewOrderSingle matches the design doc field set") {
    Lab L;
    L.must_submit_new("K1", "100000", "1.08500");
    L.drive();
    const std::string wire = wire_text(L.session_disk.wire_log.at(0));
    CHECK_EQ(wire, std::string("35=D|11=O-1|1=ACC1|55=EUR/USD|54=1|"
                               "60=20261030-08:00:00.000|38=100000|40=2|44=1.08500|59=0|"));
    CHECK_INV(L);
}

SCENARIO("OR-D-02", "P0", "the golden OrderCancelRequest carries the lineage and no made-up 37") {
    Fixture f;
    const auto c = f.lab.must_submit_cancel("C1", f.order.chain_id);
    f.lab.drive();
    const std::string wire = wire_text(f.lab.session_disk.wire_log.back());
    CHECK_EQ(wire, std::string("35=F|41=O-1|11=") + c.cl_ord_id.str() +
                       "|1=ACC1|55=EUR/USD|54=1|60=20261030-08:00:00.000|");
    // The peer did not ask for OrderID, so none is invented.
    CHECK(wire.find("37=") == std::string::npos);
    CHECK_INV(f.lab);
}

SCENARIO("OR-D-03", "P0", "the golden OrderCancelReplaceRequest sends the total, not the leaves") {
    Fixture f;
    f.lab.lp->execute(f.order.cl_ord_id.str(), 40000, 108500);
    f.lab.drive();
    CHECK_EQ(f.lab.chain(f.order.chain_id)->cum_qty, 40000);

    const auto r = f.lab.must_submit_replace("R1", f.order.chain_id, "80000", "1.08490");
    f.lab.drive();
    const std::string wire = wire_text(f.lab.session_disk.wire_log.back());
    // 38 is the whole intended order size. LeavesQty (40000) must not appear.
    CHECK(wire.find("38=80000|") != std::string::npos);
    CHECK(wire.find("38=40000|") == std::string::npos);
    CHECK(wire.find("41=O-1|") != std::string::npos);
    CHECK(wire.find("11=" + r.cl_ord_id.str() + "|") != std::string::npos);
    CHECK(wire.find("44=1.08490|") != std::string::npos);
    CHECK_INV(f.lab);
}

SCENARIO("OR-D-04", "P0", "the optional OrderStatusRequest is well formed when enabled") {
    fix::StatusFields s;
    s.cl_ord_id = ClientOrderId("O-1");
    s.account = AccountId("ACC1");
    s.wire_symbol = "EUR/USD";
    s.side = Side::Buy;
    auto m = fix::encode_status(s);
    CHECK(m.is_ok());
    CHECK_EQ(wire_text(m.value().serialize()),
             std::string("35=H|11=O-1|1=ACC1|55=EUR/USD|54=1|"));
    // Without the echo capability 790 is absent; with it, present.
    s.ord_status_req_id = QueryId("Q1");
    auto m2 = fix::encode_status(s);
    CHECK(wire_text(m2.value().serialize()).find("790=Q1|") != std::string::npos);
}

SCENARIO("OR-D-05", "P0", "a Trade is ExecType F, and the FIX 4.2 codes are not accepted") {
    Fixture f;
    p02::ReportSpec s = f.lab.lp->trade(f.order.cl_ord_id.str(), "EX1", 40000, 108500);
    s.ord_status = OrdStatus::PartiallyFilled;
    f.lab.inject(s);
    CHECK_EQ(f.lab.chain(f.order.chain_id)->cum_qty, 40000);
    CHECK_EQ(f.lab.chain(f.order.chain_id)->status, OrdStatus::PartiallyFilled);
    CHECK_INV(f.lab);

    // The legacy partial-fill code is not an ExecType in 4.4.
    p02::ReportSpec legacy = f.lab.lp->trade(f.order.cl_ord_id.str(), "EX2", 10000, 108500);
    legacy.raw[150] = "1";
    f.lab.inject(legacy);
    CHECK_EQ(f.lab.chain(f.order.chain_id)->cum_qty, 40000);  // unchanged
    CHECK(f.lab.quarantine_depth() >= std::size_t{1});
    CHECK_INV(f.lab);
}

SCENARIO("OR-D-06", "P0", "an Order Status snapshot with ExecID 0 creates no fill and no key") {
    Fixture f;
    f.lab.lp->execute(f.order.cl_ord_id.str(), 40000, 108500);
    f.lab.drive();
    const std::size_t keys = f.lab.disk.exec_keys.size();
    const std::size_t fills = f.lab.chain(f.order.chain_id)->fills.size();

    f.lab.inject(f.lab.lp->status_snapshot(f.order.cl_ord_id.str(), std::nullopt));
    CHECK_EQ(f.lab.chain(f.order.chain_id)->fills.size(), fills);
    CHECK_EQ(f.lab.disk.exec_keys.size(), keys);
    // And the sentinel never entered the uniqueness index.
    for (const auto& [k, ek] : f.lab.disk.exec_keys) {
        (void)k;
        CHECK_NE(ek.semantic_exec_id, std::string("0"));
    }
    CHECK_INV(f.lab);
}

SCENARIO("OR-D-07", "P0", "a Trade without LastQty is a schema violation, not a guessed fill") {
    Fixture f;
    p02::ReportSpec s = f.lab.lp->trade(f.order.cl_ord_id.str(), "EX1", 40000, 108500);
    s.omit.insert(32);
    f.lab.inject(s);
    CHECK_EQ(f.lab.chain(f.order.chain_id)->cum_qty, 0);
    CHECK_EQ(f.lab.quarantine_depth(), std::size_t{1});
    CHECK_EQ(f.lab.disk.quarantine.at(0).reason, HoldReason::SchemaViolation);
    CHECK_INV(f.lab);
}

SCENARIO("OR-D-08", "P0", "a Trade without LastPx is refused before the reducer") {
    Fixture f;
    p02::ReportSpec s = f.lab.lp->trade(f.order.cl_ord_id.str(), "EX1", 40000, 108500);
    s.omit.insert(31);
    f.lab.inject(s);
    CHECK_EQ(f.lab.chain(f.order.chain_id)->cum_qty, 0);
    CHECK_EQ(f.lab.chain(f.order.chain_id)->fills.size(), std::size_t{0});
    CHECK(f.lab.quarantine_depth() >= std::size_t{1});
    CHECK_INV(f.lab);
}

SCENARIO("OR-D-09", "P0", "a report missing CumQty, LeavesQty or AvgPx is malformed") {
    for (int tag : {14, 151, 6}) {
        Fixture f;
        p02::ReportSpec s = f.lab.lp->trade(f.order.cl_ord_id.str(), "EX1", 40000, 108500);
        s.omit.insert(tag);
        f.lab.inject(s);
        CHECK_EQ(f.lab.chain(f.order.chain_id)->cum_qty, 0);
        CHECK(f.lab.quarantine_depth() >= std::size_t{1});
        CHECK_INV(f.lab);
    }
}

SCENARIO("OR-D-10", "P0", "a cancel reject without 434 cannot be applied to anything") {
    Fixture f;
    const auto c = f.lab.must_submit_cancel("C1", f.order.chain_id);
    f.lab.drive();
    p02::ReportSpec s = f.lab.lp->cancel_reject(c.cl_ord_id.str(), f.order.cl_ord_id.str(),
                                                CxlRejResponseTo::OrderCancelRequest, 0);
    s.omit.insert(434);
    f.lab.inject(s);
    // The cancel request is still outstanding and the order is untouched.
    CHECK(f.lab.chain(f.order.chain_id)->pending_cancel.has_value());
    CHECK_NE(f.lab.chain(f.order.chain_id)->status, OrdStatus::Canceled);
    CHECK(f.lab.quarantine_depth() >= std::size_t{1});
    CHECK_INV(f.lab);
}

SCENARIO("OR-D-11", "P0", "an unknown ExecType is quarantined, never mapped to Trade") {
    for (const char* code : {"Z", "1", "2", ""}) {
        Fixture f;
        p02::ReportSpec s = f.lab.lp->trade(f.order.cl_ord_id.str(), "EX1", 40000, 108500);
        if (*code == '\0')
            s.omit.insert(150);
        else
            s.raw[150] = code;
        f.lab.inject(s);
        CHECK_EQ(f.lab.chain(f.order.chain_id)->cum_qty, 0);
        CHECK(f.lab.quarantine_depth() >= std::size_t{1});
        CHECK_INV(f.lab);
    }
}

SCENARIO("OR-D-12", "P0", "an unknown OrdStatus does not default to New") {
    Fixture f;
    p02::ReportSpec s = f.lab.lp->new_ack(f.order.cl_ord_id.str());
    s.raw[39] = "Z";
    f.lab.inject(s);
    CHECK(f.lab.quarantine_depth() >= std::size_t{1});
    CHECK_INV(f.lab);
}

SCENARIO("OR-D-13", "P0", "an oversized field is refused by the bounded parser") {
    Fixture f;
    p02::ReportSpec s = f.lab.lp->trade(f.order.cl_ord_id.str(), "EX1", 40000, 108500);
    s.raw[11] = std::string(300, 'A');  // the field cap is 256 bytes
    f.lab.inject(s);
    CHECK_EQ(f.lab.chain(f.order.chain_id)->cum_qty, 0);
    CHECK(f.lab.quarantine_depth() >= std::size_t{1});

    // A megabyte-scale value is rejected the same way, with no allocation spike.
    p02::ReportSpec huge = f.lab.lp->trade(f.order.cl_ord_id.str(), "EX2", 40000, 108500);
    huge.raw[11] = std::string(2 * 1024 * 1024, 'B');
    f.lab.inject(huge);
    CHECK_EQ(f.lab.chain(f.order.chain_id)->cum_qty, 0);
    CHECK_INV(f.lab);
}

SCENARIO("OR-D-14", "P0", "control characters in tag 58 cannot forge a log or audit line") {
    Fixture f;
    p02::ReportSpec s = f.lab.lp->trade(f.order.cl_ord_id.str(), "EX1", 40000, 108500);
    s.text = "ok\nFAKE_AUDIT: approved by admin\r\n";
    f.lab.inject(s);
    // The frame is refused because the value is not printable, and nothing that
    // was written contains a raw newline.
    CHECK_EQ(f.lab.chain(f.order.chain_id)->cum_qty, 0);
    for (const AuditRecord& rec : f.lab.disk.audit.records()) {
        CHECK(rec.detail.find('\n') == std::string::npos);
        CHECK(rec.detail.find('\r') == std::string::npos);
    }
    for (const QuarantineEntry& q : f.lab.disk.quarantine)
        CHECK(q.note.find('\n') == std::string::npos);
    CHECK_INV(f.lab);
}

SCENARIO("OR-D-15", "P0", "a price in exponent form is not a FIX decimal") {
    Fixture f;
    p02::ReportSpec s = f.lab.lp->trade(f.order.cl_ord_id.str(), "EX1", 40000, 108500);
    s.raw[31] = "1.085e0";
    f.lab.inject(s);
    CHECK_EQ(f.lab.chain(f.order.chain_id)->cum_qty, 0);
    CHECK(f.lab.quarantine_depth() >= std::size_t{1});
    CHECK_INV(f.lab);
}

SCENARIO("OR-D-16", "P0", "a price with a locale separator is refused") {
    Fixture f;
    p02::ReportSpec s = f.lab.lp->trade(f.order.cl_ord_id.str(), "EX1", 40000, 108500);
    s.raw[31] = "1,0850";
    f.lab.inject(s);
    CHECK_EQ(f.lab.chain(f.order.chain_id)->cum_qty, 0);
    CHECK(f.lab.quarantine_depth() >= std::size_t{1});
    CHECK_INV(f.lab);
}

SCENARIO("OR-D-17", "P0", "a negative LastQty changes nothing in the ledger") {
    Fixture f;
    p02::ReportSpec s = f.lab.lp->trade(f.order.cl_ord_id.str(), "EX1", 40000, 108500);
    s.raw[32] = "-100";
    f.lab.inject(s);
    CHECK_EQ(f.lab.chain(f.order.chain_id)->cum_qty, 0);
    CHECK_EQ(f.lab.chain(f.order.chain_id)->fills.size(), std::size_t{0});
    CHECK(f.lab.quarantine_depth() >= std::size_t{1});
    CHECK_INV(f.lab);
}

SCENARIO("OR-D-18", "P0", "a CumQty past the integer range is held, not wrapped") {
    Fixture f;
    p02::ReportSpec s = f.lab.lp->trade(f.order.cl_ord_id.str(), "EX1", 40000, 108500);
    s.raw[14] = "99999999999999999999";
    f.lab.inject(s);
    CHECK_EQ(f.lab.chain(f.order.chain_id)->cum_qty, 0);
    CHECK(f.lab.quarantine_depth() >= std::size_t{1});
    CHECK_INV(f.lab);
}

SCENARIO("OR-D-19", "P1", "an agreed optional tag is tolerated and its provenance kept") {
    Lab L;
    L.cfg.order_profile.tolerated_custom_tags.push_back(9001);
    L.cfg.finalize();
    const auto a = L.must_submit_new("K1");
    L.drive();

    p02::ReportSpec s = L.lp->trade(a.cl_ord_id.str(), "EX1", 40000, 108500);
    s.extra.push_back({9001, "vendor-note"});
    const std::string frame = s.encode();
    L.inject(s);

    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 40000);
    // The digest in the journal is the digest of the frame that carried 9001,
    // so the audit trail still points at the exact bytes received.
    bool found = false;
    for (const JournalRecord& rec : L.disk.journal) {
        if (rec.kind != JournalRecord::Kind::ReportApplied || !rec.report) continue;
        if (rec.report->raw_frame_hash == sha256(frame)) found = true;
    }
    CHECK(found);
    CHECK_INV(L);
}

SCENARIO("OR-D-20", "P1", "a duplicated tag is never resolved by last-write-wins") {
    Fixture f;
    p02::ReportSpec s = f.lab.lp->trade(f.order.cl_ord_id.str(), "EX1", 40000, 108500);
    // Append a second tag 11 naming a different order.
    s.extra.push_back({11, "O-9999"});
    f.lab.inject(s);
    CHECK_EQ(f.lab.chain(f.order.chain_id)->cum_qty, 0);
    CHECK(f.lab.quarantine_depth() >= std::size_t{1});

    // A duplicated quantity tag is refused too, rather than one of them winning.
    p02::ReportSpec s2 = f.lab.lp->trade(f.order.cl_ord_id.str(), "EX2", 40000, 108500);
    s2.extra.push_back({32, "90000"});
    f.lab.inject(s2);
    CHECK_EQ(f.lab.chain(f.order.chain_id)->cum_qty, 0);
    CHECK_INV(f.lab);
}
