// The golden fixture files, used rather than described.
//
// Design doc 9.3 gives illustrative D/F/G frames and then warns that they are
// not TCP frames and must not be copy-pasted. The files under fixtures/fix44
// are the application field blocks only, and these cases make them
// load-bearing: the encoder output is compared to the bytes on disk, and every
// file in invalid/ is fed to the real parser and has to be refused.
//
// If a fixture file is missing the case fails rather than quietly passing, so a
// deleted fixture cannot look like a green suite.
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "router/fix44.hpp"
#include "support/lab.hpp"

using namespace router;

namespace {

const std::filesystem::path kRoot{"fixtures/fix44"};

std::string read_fixture(const std::string& relative) {
    const std::filesystem::path p = kRoot / relative;
    std::ifstream f(p, std::ios::binary);
    if (!f)
        TT_FAIL("fixture not found: " + p.string() +
                " (run from the project root, where fixtures/ lives)");
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

fix::ParseLimits lab_limits() {
    const Config cfg = Config::lab_default();
    fix::ParseLimits lim;
    lim.max_frame_bytes = cfg.limits.max_payload_bytes;
    lim.max_field_bytes = cfg.limits.max_field_bytes;
    lim.max_fields = cfg.limits.max_fields_per_message;
    lim.tolerated_custom_tags = cfg.order_profile.tolerated_custom_tags;
    return lim;
}

}  // namespace

SCENARIO("U-20", "P0", "the encoder reproduces the golden outbound fixtures byte for byte") {
    const std::string when = "20261030-08:00:00.000";

    fix::NewOrderFields nf;
    nf.cl_ord_id = ClientOrderId("O-1");
    nf.account = AccountId("ACC1");
    nf.wire_symbol = "EUR/USD";
    nf.side = Side::Buy;
    nf.qty = 100000;
    nf.price_ticks = 108500;
    nf.price_scale = 5;
    nf.transact_time = when;
    CHECK_EQ(fix::encode_new_order(nf).value().serialize(),
             read_fixture("new/golden_new_order.bin"));

    fix::CancelFields cf;
    cf.cl_ord_id = ClientOrderId("C-2");
    cf.orig_cl_ord_id = ClientOrderId("O-1");
    cf.account = AccountId("ACC1");
    cf.wire_symbol = "EUR/USD";
    cf.side = Side::Buy;
    cf.transact_time = when;
    CHECK_EQ(fix::encode_cancel(cf).value().serialize(),
             read_fixture("cancel/golden_cancel.bin"));

    fix::ReplaceFields rf;
    rf.cl_ord_id = ClientOrderId("R-2");
    rf.orig_cl_ord_id = ClientOrderId("O-1");
    rf.account = AccountId("ACC1");
    rf.wire_symbol = "EUR/USD";
    rf.side = Side::Buy;
    rf.total_qty = 80000;
    rf.price_ticks = 108490;
    rf.price_scale = 5;
    rf.transact_time = when;
    CHECK_EQ(fix::encode_replace(rf).value().serialize(),
             read_fixture("replace/golden_replace.bin"));

    fix::StatusFields sf;
    sf.cl_ord_id = ClientOrderId("O-1");
    sf.account = AccountId("ACC1");
    sf.wire_symbol = "EUR/USD";
    sf.side = Side::Buy;
    CHECK_EQ(fix::encode_status(sf).value().serialize(),
             read_fixture("status/golden_status.bin"));
    sf.ord_status_req_id = QueryId("rc-1-q1");
    CHECK_EQ(fix::encode_status(sf).value().serialize(),
             read_fixture("status/golden_status_with_reqid.bin"));
}

SCENARIO("U-21", "P0", "every valid inbound fixture passes the parser and the dictionary") {
    const Config cfg = Config::lab_default();
    const fix::ParseLimits lim = lab_limits();

    struct Expect {
        const char* file;
        char msg_type;
        ExecType exec_type;
    };
    const Expect reports[] = {
        {"execution/new_ack.bin", '8', ExecType::New},
        {"execution/trade_partial.bin", '8', ExecType::Trade},
        {"execution/trade_fill.bin", '8', ExecType::Trade},
        {"execution/pending_cancel.bin", '8', ExecType::PendingCancel},
        {"execution/canceled.bin", '8', ExecType::Canceled},
        {"execution/pending_replace.bin", '8', ExecType::PendingReplace},
        {"execution/replaced.bin", '8', ExecType::Replaced},
        {"execution/rejected.bin", '8', ExecType::Rejected},
        {"execution/order_status.bin", '8', ExecType::OrderStatus},
    };
    for (const Expect& e : reports) {
        const std::string frame = read_fixture(e.file);
        auto parsed = fix::parse_app_frame(frame, lim);
        CHECK_MSG(parsed.is_ok(), std::string(e.file));
        CHECK_EQ(parsed.value().msg_type(), e.msg_type);
        CHECK_EQ(fix::exec_type_from_wire(parsed.value().get_or(fix::tag::kExecType)).value(),
                 e.exec_type);
        auto validated = fix::validate_execution_report(parsed.value(), cfg);
        CHECK_MSG(validated.is_ok(),
                  std::string(e.file) + ": " + std::string(to_string(validated.error())));
    }

    for (const char* file : {"reject/cancel_reject_f.bin", "reject/cancel_reject_g.bin",
                             "reject/cancel_reject_unknown_order.bin"}) {
        const std::string frame = read_fixture(file);
        auto parsed = fix::parse_app_frame(frame, lim);
        CHECK_MSG(parsed.is_ok(), std::string(file));
        CHECK_EQ(parsed.value().msg_type(), '9');
        auto validated = fix::validate_cancel_reject(parsed.value());
        CHECK_MSG(validated.is_ok(), std::string(file));
    }

    // The status snapshot carries the sentinel ExecID, and nothing treats it as
    // a business execution identity.
    const std::string status = read_fixture("execution/order_status.bin");
    CHECK(status.find("17=0\x01") != std::string::npos);
}

SCENARIO("U-22", "P0", "every fixture under invalid/ is refused, each for its own reason") {
    const Config cfg = Config::lab_default();
    const fix::ParseLimits lim = lab_limits();

    struct Expect {
        const char* file;
        ValidationError expected;
    };
    const Expect cases[] = {
        {"invalid/trade_missing_lastqty.bin", ValidationError::MissingTradeFields},
        {"invalid/trade_missing_lastpx.bin", ValidationError::MissingTradeFields},
        {"invalid/trade_exponent_price.bin", ValidationError::BadDecimalFormat},
        {"invalid/trade_locale_price.bin", ValidationError::BadDecimalFormat},
        {"invalid/trade_negative_lastqty.bin", ValidationError::ValueOutOfRange},
        {"invalid/trade_legacy_exectype.bin", ValidationError::UnknownExecType},
        {"invalid/cancel_reject_no_434.bin", ValidationError::MissingCancelRejectResponseTo},
        {"invalid/duplicate_clordid_tag.bin", ValidationError::DuplicateTag},
        {"invalid/prohibited_custom_tag.bin", ValidationError::ProhibitedCustomTag},
    };

    for (const Expect& e : cases) {
        const std::string frame = read_fixture(e.file);
        auto parsed = fix::parse_app_frame(frame, lim);
        if (parsed.is_err()) {
            CHECK_EQ(parsed.error(), e.expected);
            continue;
        }
        // It got past the structural rules, so the dictionary has to catch it.
        const char mt = parsed.value().msg_type();
        auto validated = mt == '9' ? fix::validate_cancel_reject(parsed.value())
                                   : fix::validate_execution_report(parsed.value(), cfg);
        CHECK_MSG(validated.is_err(), std::string(e.file) + " was accepted");
        CHECK_EQ(validated.error(), e.expected);
    }
}

SCENARIO("U-23", "P0", "a golden fixture fed through the whole router behaves as expected") {
    // The valid trade fixture, replayed verbatim into a live chain, produces
    // exactly one fill with the values on disk.
    lab::Lab L;
    const auto a = L.must_submit_new("K1", "100000", "1.08500");
    L.drive();
    CHECK_EQ(a.cl_ord_id.str(), std::string("O-1"));  // the fixture names O-1

    const std::string frame = read_fixture("execution/trade_partial.bin");
    L.session->deliver_inbound(frame);
    L.rt->pump_inbound();

    const p03::ChainState* c = L.chain(a.chain_id);
    CHECK_EQ(c->cum_qty, 40000);
    CHECK_EQ(c->fills.size(), std::size_t{1});
    CHECK_EQ(c->fills.at(0).exec_id.str(), std::string("EX-1"));
    CHECK_EQ(c->fills.at(0).px, 108500);
    CHECK_INV(L);

    // And the invalid one changes nothing.
    L.session->deliver_inbound(read_fixture("invalid/trade_missing_lastqty.bin"));
    L.rt->pump_inbound();
    CHECK_EQ(L.chain(a.chain_id)->cum_qty, 40000);
    CHECK(L.quarantine_depth() >= std::size_t{1});
    CHECK_INV(L);
}
