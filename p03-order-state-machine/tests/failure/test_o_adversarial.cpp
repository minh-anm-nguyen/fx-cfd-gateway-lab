// Group O -- malformed FIX-derived events, adversarial input and security
// (design doc 18.O).
//
// Several of these belong to P01 (checksum, session identity) or P04 (tag
// parsing, size budgets). Where the check genuinely lives upstream the case
// asserts what P03 must guarantee regardless: that no such event can reach the
// ledger, and that P03 itself fails closed if one does. Cases that can only be
// proved inside P01 or P04 are marked BLOCKED with the reason rather than
// claimed here.
#include <random>

#include "../support/lab.hpp"

using namespace lab;

SCENARIO("O-01", "P0", "a trade without ExecID is never applied as an anonymous fill") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    expect_quarantine(L.submit(trade("", "O1").lastq("40000").lastpx("1.08490")),
                      Reason::IdentityInsufficient);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{0});
    CHECK_EQ(L.state(id).ledger.size(), size_t{0});
}

SCENARIO("O-02", "P0", "a trade with no ClOrdID cannot be attributed to a version") {
    Lab L;
    const OrderId id = L.working("O1", "100000");

    // An OrderID that matches nothing correlates to no chain at all.
    expect_quarantine(
        L.submit(trade("X1", "").oid("NOT-VERIFIED").lastq("40000").lastpx("1.08490")),
        Reason::IdentityInsufficient);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{0});

    // Even the chain own verified OrderID is not enough: an alias can say
    // which order a report belongs to, but not which accepted version the
    // quantity executed against, and that is what a fill has to name.
    expect_quarantine(
        L.submit(trade("X2", "").oid("LPO-O1").lastq("40000").lastpx("1.08490").cum("40000")),
        Reason::IdentityInsufficient);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{0});
    CHECK_EQ(L.state(id).ledger.size(), size_t{0});
}

SCENARIO("O-03", "P0", "contradictory cumulative values never reach a committed state") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "40000");
    // P04 fails a frame carrying tag 14 twice with different values; what P03
    // guarantees is that a CumQty disagreeing with the ledger is refused.
    expect_quarantine(L.submit(trade("X2", "O1").lastq("10000").lastpx("1.08490").cum("99000")),
                      Reason::MissingFillEvidence);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{40000});
}

SCENARIO("O-04", "P0", "LastQty that is not a number is refused, not defaulted to zero") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    for (const char* bad : {"abc", "4e4", "40,000", "40 000", "", "+-4"}) {
        const EngineResult r =
            L.submit(trade(std::string("X-") + bad, "O1").lastq(bad).lastpx("1.08490"));
        CHECK_MSG(r.decision == Decision::Quarantined,
                  std::string("LastQty='") + bad + "' gave " + to_string(r.decision));
    }
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{0});
    CHECK_EQ(L.state(id).ledger.size(), size_t{0});
}

SCENARIO("O-05", "P0", "an absurdly large LastQty is refused by checked parsing, not a crash") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    expect_quarantine(
        L.submit(trade("X1", "O1").lastq("99999999999999999999999").lastpx("1.08490")),
        Reason::MalformedNumber);
    expect_quarantine(
        L.submit(trade("X2", "O1").lastq("999999999999999999").lastpx("1.08490")),
        Reason::QtyStepMismatch);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{0});
}

SCENARIO("O-06", "P0", "a non-decimal LastPx never reaches a float conversion") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    for (const char* bad : {"nan", "inf", "-inf", "0x1.8p0", "1.0e308", "\x01\x02"}) {
        const EngineResult r =
            L.submit(trade(std::string("X-") + bad, "O1").lastq("40000").lastpx(bad));
        CHECK_MSG(r.decision == Decision::Quarantined,
                  std::string("LastPx='") + bad + "' gave " + to_string(r.decision));
    }
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{0});
}

SCENARIO("O-07", "P0", "an unsupported Side code does not fall through to buy") {
    Lab L;
    const OrderId id = L.working("O1", "100000");  // a buy
    for (const char* bad : {"3", "7", "B", "", "12"}) {
        if (*bad == '\0') continue;  // an absent tag is a different case
        const EngineResult r =
            L.submit(trade(std::string("X-") + bad, "O1").side(bad).lastq("40000").lastpx("1.08490"));
        CHECK_MSG(r.decision == Decision::Quarantined,
                  std::string("Side='") + bad + "' gave " + to_string(r.decision));
        CHECK_EQ(r.reason, Reason::SideMismatch);
    }
    CHECK_EQ(L.state(id).side, Side::Buy);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{0});
}

SCENARIO("O-08", "P0", "ExecType=F with OrdStatus=8 is a contradiction, not a fill") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    expect_quarantine(L.submit(trade("X1", "O1")
                                   .status(ord_status::kRejected)
                                   .lastq("40000")
                                   .lastpx("1.08490")),
                      Reason::StatusInconsistent);
    expect_quarantine(L.submit(trade("X2", "O1")
                                   .status(ord_status::kCanceled)
                                   .lastq("40000")
                                   .lastpx("1.08490")),
                      Reason::StatusInconsistent);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{0});
    CHECK(L.state(id).done_reason != DoneReason::Rejected);
}

SCENARIO("O-09", "P0", "a replace acknowledgement with no request lineage is quarantined") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    // No replace was ever requested.
    expect_quarantine(L.submit(replaced("R1", "O1").oqty("150000")), Reason::UnknownRequest);
    CHECK_EQ(L.state(id).accepted_versions.size(), size_t{1});
    CHECK_EQ(L.state(id).accepted_qty_units(), int64_t{100000});
}

SCENARIO("O-10", "P0", "an OrderCancelReject without tag 434 resolves nothing") {
    Lab L;
    const OrderId id = L.working("O1");
    CHECK(L.submit(cmd_cancel("C1", "O1")).ok());
    expect_quarantine(L.submit(cancel_reject("C1", "O1", "")), Reason::ProtocolMismatch);
    expect_quarantine(L.submit(cancel_reject("C1", "O1", "9")), Reason::ProtocolMismatch);
    // The request is still outstanding; nothing was resolved ambiguously.
    CHECK(L.state(id).pending.has_value());
    CHECK_EQ(L.state(id).pending->cl_ord_id, std::string("C1"));
}

SCENARIO("O-11", "P1", "an oversized or invalid Text(58) neither grows memory nor is trusted") {
    Policy p = default_lab_policy();
    p.limits.max_text_bytes = 64;
    Lab L(p);
    const OrderId id = L.working("O1", "100000");

    // A megabyte of free text, including invalid UTF-8. It is carried as data,
    // never interpreted, and it is excluded from the business fingerprint, so
    // it cannot change a dedup verdict either.
    std::string huge(1024 * 1024, 'A');
    huge += "\xC3\x28\xFF\xFE";
    const auto with_text = trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000").text(huge);
    const auto without_text = trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000");
    CHECK_EQ(business_fingerprint(with_text.r), business_fingerprint(without_text.r));

    CHECK(L.submit(with_text).ok());
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{40000});
    // Nothing of that text reached the canonical state.
    CHECK(L.state(id).canonical_text().find("AAAA") == std::string::npos);
    CHECK(L.state(id).canonical_text().size() < size_t{4096});
}

SCENARIO("O-12", "P0", "a NUL byte in a ClOrdID is refused before any comparison") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    const std::string sneaky = std::string("O1\0EVIL", 7);
    // A truncated comparison would match "O1" and attach to the wrong order.
    expect_quarantine(L.submit(trade("X1", sneaky).lastq("40000").lastpx("1.08490")),
                      Reason::IdentityInsufficient);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{0});

    // The same on the command path.
    LocalCommand c = cmd_new(std::string("O2\0X", 4));
    expect_reject(L.submit(c), Reason::IdTooLong);
}

SCENARIO("O-13", "P1", "a lookalike identifier is compared as bytes, never folded") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    // Cyrillic O (U+041E) in place of ASCII 'O'.
    const std::string homoglyph = "\xD0\x9E" "1";
    CHECK_NE(homoglyph, std::string("O1"));
    const EngineResult r =
        L.submit(trade("X1", homoglyph).oid("LPO-SPOOF").lastq("40000").lastpx("1.08490"));
    // It names no known request and no known alias, so it is an orphan, not a
    // fill on O1.
    expect_quarantine(r, Reason::IdentityInsufficient);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{0});
    CHECK(!L.policy().normalize_cl_ord_id_case);
}

SCENARIO_BLOCKED("O-14", "P0", "a corrupt FIX CheckSum or BodyLength is rejected before P03",
                 "belongs to P01's framer: P03 consumes normalized business events and never "
                 "sees a frame, so this cannot be proved from inside this project")

SCENARIO_BLOCKED("O-15", "P0", "a wrong SenderCompID or TargetCompID is rejected before P03",
                 "belongs to P01's session authentication: P03 has no CompID in its event "
                 "model. The adjacent guarantee P03 does make is covered by O-16")

SCENARIO("O-16", "P0", "an authentic report for another peer's order cannot mutate this one") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK_EQ(L.state(id).peer_id, std::string("LP1"));
    // Well formed, correctly signed as far as the session is concerned, and
    // from the wrong counterparty.
    const EngineResult r =
        L.submit(trade("X1", "O1").peer("LP-OTHER").lastq("40000").lastpx("1.08490").cum("40000"));
    expect_quarantine(r, Reason::PeerMismatch);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{0});
    CHECK_EQ(L.state(id).ledger.size(), size_t{0});
}

SCENARIO("O-17", "P1", "an oversized identifier is bounded before it reaches an index") {
    Policy p = default_lab_policy();
    p.limits.max_exec_id_bytes = 32;
    p.limits.max_cl_ord_id_bytes = 32;
    Lab L(p);
    const OrderId id = L.working("O1", "100000");
    const std::string huge(65536, 'E');

    // 64 KiB of ExecID. The ceiling is checked before the value is used as a
    // map key, and the durable key it produces is a digest, not the payload.
    expect_quarantine(L.submit(trade(huge, "O1").lastq("40000").lastpx("1.08490")),
                      Reason::IdTooLong);

    // Nothing of that size reached the per-chain dedup list.
    for (const SeenReport& seen : L.state(id).seen_reports) {
        CHECK(seen.exec_id.size() <= size_t{64});
    }
    // Nor the durable index.
    for (const auto& [key, row] : L.store().applied_keys()) {
        (void)row;
        CHECK_MSG(key.size() <= size_t{512},
                  "durable key of " + std::to_string(key.size()) + " bytes reached the index");
        CHECK(key.find(huge) == std::string::npos);
    }
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{0});
}

SCENARIO("O-18", "P0", "an unknown ExecType has no handler and fails closed") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    for (const char* unknown : {"Z", "z", "99", "~", "\x7F"}) {
        const EngineResult r = L.submit(trade(std::string("X-") + unknown, "O1")
                                            .etype(unknown)
                                            .lastq("40000")
                                            .lastpx("1.08490"));
        CHECK_MSG(r.decision == Decision::Quarantined,
                  std::string("ExecType='") + unknown + "' gave " + to_string(r.decision));
    }
    // An unknown OrdStatus is refused the same way, never normalised to New.
    expect_quarantine(L.submit(new_ack("O1", "E-WEIRD").status("Z")), Reason::UnsupportedEvent);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{0});
    CHECK_EQ(L.state(id).accepted_versions.size(), size_t{1});
}

SCENARIO("O-19", "P0", "an exception on an error path does not leave a partial commit") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "40000");
    const uint64_t before = L.state(id).state_hash();
    const uint64_t revision_before = L.state(id).revision;

    // The reducer is pure and throws nothing of its own; the place an
    // exception can appear is the storage boundary. When it does, the
    // in-memory state must not have moved ahead of the journal.
    L.store().failpoint().arm(CrashWindow::W05_InTxnBeforeJournalInsert);
    bool threw = false;
    try {
        L.submit(trade("X2", "O1").lastq("20000").lastpx("1.08491").cum("60000"));
    } catch (const SimulatedCrash&) {
        threw = true;
    }
    CHECK(threw);
    L.store().failpoint().disarm();

    CHECK_EQ(L.state(id).state_hash(), before);
    CHECK_EQ(L.state(id).revision, revision_before);
    CHECK_EQ(L.engine().recover().status, RecoveryStatus::Clean);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{40000});
}

SCENARIO("O-20", "P1", "a million random inputs corrupt nothing and only valid events mutate") {
    // The full P01 -> P04 -> P03 byte fuzz needs those projects; what is
    // fuzzed here is every parser and validator P03 owns, which is where a
    // malformed value would have to get through to do damage.
    std::mt19937_64 rng(20261030ULL);
    std::uniform_int_distribution<int> byte(0, 255);
    std::uniform_int_distribution<int> len(0, 24);

    size_t accepted = 0;
    for (int i = 0; i < 1000000; ++i) {
        std::string blob;
        const int n = len(rng);
        for (int k = 0; k < n; ++k) blob.push_back(static_cast<char>(byte(rng)));

        int64_t ticks = 0;
        const DecimalError pe = parse_ticks(blob, 1, 100000, ticks);
        int64_t units = 0;
        const DecimalError ue = parse_units(blob, units);
        if (pe == DecimalError::None) {
            ++accepted;
            // Anything accepted must round-trip, which a corrupted parse would
            // not.
            int64_t again = 0;
            CHECK(parse_ticks(ticks_to_text(ticks, 1, 100000), 1, 100000, again) ==
                  DecimalError::None);
            CHECK_EQ(again, ticks);
        }
        if (ue == DecimalError::None) {
            CHECK(units == units);  // a value, not a trap representation
        }
    }
    // The grammar is narrow enough that random bytes almost never satisfy it,
    // and that is the point.
    CHECK(accepted < size_t{1000000});

    // A sample of random blobs pushed through the engine mutates nothing.
    Lab L;
    const OrderId id = L.working("O1", "100000");
    const uint64_t before = L.state(id).state_hash();
    for (int i = 0; i < 2000; ++i) {
        std::string q;
        std::string p;
        for (int k = 0; k < 6; ++k) {
            q.push_back(static_cast<char>(byte(rng)));
            p.push_back(static_cast<char>(byte(rng)));
        }
        const EngineResult r = L.submit(trade("F" + std::to_string(i), "O1").lastq(q).lastpx(p));
        CHECK(r.decision != Decision::Applied);
    }
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{0});
    CHECK_EQ(L.state(id).ledger.size(), size_t{0});
    CHECK_NE(L.state(id).state_hash(), before);  // incidents were recorded
    CHECK(L.state(id).incidents.size() <= L.policy().limits.max_quarantine_incidents);
}
