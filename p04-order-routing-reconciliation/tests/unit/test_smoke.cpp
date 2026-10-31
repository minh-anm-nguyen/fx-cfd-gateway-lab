// Foundation checks: the numeric parser, the FIX codec, the hash chain and one
// end-to-end happy path. These are the U-series: not from the what-if matrix,
// but the things every matrix case silently depends on.
#include "router/fix44.hpp"
#include "router/numeric.hpp"
#include "support/lab.hpp"

using namespace router;

SCENARIO("U-01", "P0", "a decimal is converted to ticks without rounding or locale tricks") {
    CHECK_EQ(parse_decimal_ticks("1.08500", 5).value(), 108500);
    CHECK_EQ(parse_decimal_ticks("1.085", 5).value(), 108500);
    CHECK_EQ(parse_decimal_ticks("100000", 0).value(), 100000);
    // Extra precision is refused rather than rounded (design doc 9.2).
    CHECK(parse_decimal_ticks("1.085001", 5).is_err());
    CHECK_EQ(parse_decimal_ticks("1.085001", 5).error(), NumError::TooPrecise);
    // Exponent and locale forms are not FIX decimals.
    CHECK_EQ(parse_decimal_ticks("1.085e0", 5).error(), NumError::NotDigit);
    CHECK_EQ(parse_decimal_ticks("1,0850", 5).error(), NumError::Separator);
    CHECK_EQ(parse_decimal_ticks("-100", 0).error(), NumError::Negative);
    CHECK_EQ(parse_decimal_ticks("99999999999999999999", 0).error(), NumError::TooManyDigits);
}

SCENARIO("U-02", "P0", "a tick count renders back to the same decimal text") {
    CHECK_EQ(format_decimal_ticks(108500, 5), std::string("1.08500"));
    CHECK_EQ(format_decimal_ticks(100000, 0), std::string("100000"));
    CHECK_EQ(format_decimal_ticks(0, 5), std::string("0.00000"));
}

SCENARIO("U-03", "P0", "checked arithmetic refuses to wrap") {
    std::int64_t out = 0;
    CHECK(!checked_mul(INT64_MAX, 2, out));
    CHECK(!checked_add(INT64_MAX, 1, out));
    CHECK(checked_mul(100000, 108500, out));
    CHECK_EQ(out, 10850000000LL);
}

SCENARIO("U-04", "P0", "sha256 matches the published digest of the empty string") {
    CHECK_EQ(sha256("").hex(),
             std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_EQ(sha256("abc").hex(),
             std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
}

SCENARIO("U-05", "P0", "a FIX UTC timestamp round trips") {
    const WallMicros t = VirtualClock::kLabEpochUtc;
    const std::string text = format_fix_utc(t);
    CHECK_EQ(text, std::string("20261030-08:00:00.000"));
    WallMicros back = 0;
    CHECK(parse_fix_utc(text, back));
    CHECK_EQ(back, t);
    CHECK(!parse_fix_utc("20261030-08:00:00", back));
}

SCENARIO("U-06", "P0", "the audit chain detects an edited record") {
    AuditLog log;
    for (int i = 0; i < 5; ++i) {
        AuditRecord r;
        r.kind = AuditKind::CommandAdmitted;
        r.actor = "alice";
        r.subject = "cmd-" + std::to_string(i);
        r.reason = "ok";
        CHECK(log.append(r).has_value());
    }
    CHECK(log.verify());
    CHECK(log.tamper_for_test(2, "edited"));
    CHECK(!log.verify());
}

SCENARIO("U-07", "P0", "text from a peer cannot inject a log line") {
    const std::string nasty = "ok\nFAKE: admin approved\r\n\x01";
    const std::string clean = sanitize_text(nasty);
    CHECK(clean.find('\n') == std::string::npos);
    CHECK(clean.find('\r') == std::string::npos);
    CHECK(clean.find('\x01') == std::string::npos);
}

SCENARIO("U-08", "P0", "a credential tag is redacted in a stored frame") {
    const std::string frame = "35=A\x01" "554=hunter2\x01" "55=EUR/USD\x01";
    const std::string red = redact_fix_frame(frame);
    CHECK(red.find("hunter2") == std::string::npos);
    CHECK(red.find("554=***") != std::string::npos);
    CHECK(red.find("EUR/USD") != std::string::npos);
}

SCENARIO("U-09", "P0", "the New order encoder produces the fields design doc 9.3 lists") {
    fix::NewOrderFields f;
    f.cl_ord_id = ClientOrderId("O-42-A");
    f.account = AccountId("ACC1");
    f.wire_symbol = "EUR/USD";
    f.side = Side::Buy;
    f.qty = 100000;
    f.price_ticks = 108500;
    f.price_scale = 5;
    f.transact_time = "20261030-08:00:00.000";
    auto m = fix::encode_new_order(f);
    CHECK(m.is_ok());
    const std::string wire = wire_text(m.value().serialize());
    CHECK_EQ(wire,
             std::string("35=D|11=O-42-A|1=ACC1|55=EUR/USD|54=1|60=20261030-08:00:00.000|"
                         "38=100000|40=2|44=1.08500|59=0|"));
    // No sequence number anywhere: that belongs to P01 (design doc 9.1).
    CHECK(wire.find("34=") == std::string::npos);
}

SCENARIO("U-10", "P0", "a cancel may not reuse the ClOrdID it is cancelling") {
    fix::CancelFields f;
    f.cl_ord_id = ClientOrderId("O-1");
    f.orig_cl_ord_id = ClientOrderId("O-1");
    f.account = AccountId("ACC1");
    f.wire_symbol = "EUR/USD";
    f.transact_time = "20261030-08:00:00.000";
    CHECK(fix::encode_cancel(f).is_err());
}

SCENARIO("U-11", "P0", "the parser refuses a duplicated tag instead of taking the last one") {
    fix::ParseLimits lim;
    const std::string frame = "35=8\x01" "11=O-1\x01" "11=O-2\x01";
    auto r = fix::parse_app_frame(frame, lim);
    CHECK(r.is_err());
    CHECK_EQ(r.error(), ValidationError::DuplicateTag);
}

SCENARIO("U-12", "P0", "a session message reaching the application parser is an incident") {
    fix::ParseLimits lim;
    auto r = fix::parse_app_frame("35=3\x01" "45=7\x01", lim);
    CHECK(r.is_err());
    CHECK_EQ(r.error(), ValidationError::NotApplicationMessage);
}

SCENARIO("U-13", "P0", "FIX 4.4 has no ExecType 1 or 2 for a partial fill") {
    CHECK(!fix::exec_type_from_wire("1").has_value());
    CHECK(!fix::exec_type_from_wire("2").has_value());
    CHECK_EQ(fix::exec_type_from_wire("F").value(), ExecType::Trade);
    CHECK_EQ(fix::exec_type_from_wire("I").value(), ExecType::OrderStatus);
}

SCENARIO("U-14", "P0", "an end-to-end New, acknowledgement and full fill") {
    lab::Lab L;
    CHECK(L.last_boot_ok);
    const auto accepted = L.must_submit_new("K1");
    CHECK_EQ(accepted.delivery, DeliveryState::AcceptedDurable);
    // ACCEPTED_DURABLE is not an LP acknowledgement (design doc 1.4).
    CHECK_EQ(accepted.business, BusinessOutcome::Unknown);
    CHECK_INV(L);

    L.drive();
    // The LP acknowledged in auto mode, so the chain is New with nothing filled.
    const p03::ChainState* c = L.chain(accepted.chain_id);
    CHECK(c != nullptr);
    CHECK_EQ(c->status, OrdStatus::New);
    CHECK_EQ(c->cum_qty, 0);
    CHECK_EQ(L.rt->original_sends(), std::size_t{1});
    CHECK_INV(L);

    L.lp->execute(accepted.cl_ord_id.str(), 100000, 108500);
    L.drive();
    c = L.chain(accepted.chain_id);
    CHECK_EQ(c->status, OrdStatus::Filled);
    CHECK_EQ(c->cum_qty, 100000);
    CHECK_EQ(c->leaves_qty, 0);
    CHECK_EQ(c->fills.size(), std::size_t{1});
    CHECK_EQ(c->avg_px(), 108500);
    CHECK_INV(L);
    // Nothing was quarantined and no status query was ever sent.
    CHECK_EQ(L.quarantine_depth(), std::size_t{0});
    CHECK_EQ(L.wire_count('H'), std::size_t{0});
}

SCENARIO("U-15", "P0", "replaying the journal reproduces the live projection") {
    lab::Lab L;
    const auto a = L.must_submit_new("K1");
    L.drive();
    L.lp->execute(a.cl_ord_id.str(), 40000, 108500);
    L.drive();
    L.lp->execute(a.cl_ord_id.str(), 60000, 108510);
    L.drive();

    const auto replayed = replay_chains_from_journal(L.disk);
    CHECK_EQ(replayed.size(), L.disk.chains.size());
    for (const auto& [id, live] : L.disk.chains) {
        auto it = replayed.find(id);
        CHECK(it != replayed.end());
        CHECK_EQ(it->second.state_hash().hex(), live.state_hash().hex());
    }
    CHECK_INV(L);
}
