// The two-store gap, measured rather than argued about.
//
// EI-22 asks that "reservation + command intent + outbox" be written
// atomically. They are not, and both components say so themselves. P05's
// journal store: "This is the single-node lab adapter the spec sanctions
// (§1.3). It is not a substitute for the P06 PostgreSQL contract." P06's stable
// send store: "Two stores means two commits, which is exactly the hazard the
// doc describes, and it is the only way the crash windows W13 to W21 can be
// reproduced rather than asserted."
//
// There are three durable files in this lab -- P04's store, P05's journal,
// P06's journal -- and no transaction spans them. Closing that properly means
// one database holding all three, which is a persistence change and not an
// adapter. Until then §16 and EI-31 ask for the thing this file does: prove
// what the gap can actually produce, in both directions, so a recovery path has
// something to detect rather than a comment to trust.
//
// The two directions are not symmetric, and that asymmetry is the point:
//
//   reservation without intent   budget held for an order that does not exist.
//                                Costs availability. Detectable. Safe.
//   intent without reservation   an order on the wire with no budget behind it.
//                                Costs money. Must be detectable, and must
//                                never be treated as risk-approved.
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>

#include "deps/p05_risk.hpp"
#include "gw/evidence.hpp"
#include "gw/risk_adapter.hpp"
#include "jrnl/stable_send.hpp"
#include "risk/scenario.hpp"
#include "router/clock.hpp"
#include "router/hash.hpp"

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* expr, int line, const std::string& detail) {
    ++g_checks;
    if (ok) return;
    ++g_failures;
    std::printf("  FAIL line %d  %s%s%s\n", line, expr, detail.empty() ? "" : " -- ",
                detail.c_str());
}

#define CHECK(expr) check((expr), #expr, __LINE__, {})
#define CHECK_MSG(expr, detail) check((expr), #expr, __LINE__, (detail))

constexpr const char* kSession = "ROUTER04:LPSIM01:PRIMARY";
constexpr const char* kChain = "CH-1";
constexpr const char* kCommand = "C-1";
constexpr std::int64_t kQty = 10000;
constexpr std::int64_t kPriceTicks = 110000;

std::filesystem::path make_dir(const std::string& name) {
    std::error_code ec;
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path(ec) / ("p08-two-store-" + name);
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

gw::RiskAdmissionAdapter::Options adapter_options(const risk::Principal& trader) {
    gw::RiskAdmissionAdapter::Options o;
    o.principal = trader;
    o.wire_to_instrument = {{"EUR/USD", "EURUSD"}};
    return o;
}

router::p05::RiskRequest a_new_order() {
    router::p05::RiskRequest rr;
    rr.command_id = router::CommandId(kCommand);
    rr.tenant = router::TenantId("T1");
    rr.account = router::AccountId("ACC-A");
    rr.wire_symbol = "EUR/USD";
    rr.side = router::Side::Buy;
    rr.qty = kQty;
    rr.price_ticks = kPriceTicks;
    rr.increases_exposure = true;
    rr.order_chain_id = router::OrderChainId(kChain);
    rr.price_scale = 5;
    rr.total_units_after = kQty;
    return rr;
}

bool reservation_held_for_chain(const risk::RiskStore& store) {
    const std::optional<risk::ReservationId> id = store.find_chain("T1", "ACC-A", kChain);
    if (!id.has_value()) return false;
    const std::optional<risk::Reservation> r = store.find_reservation(*id);
    return r.has_value() && r->holds_budget();
}

// What a recovery path has to be able to compute from the two stores alone.
// Written as a function because this is the detection EI-31 needs to exist: a
// staged reservation or a durable send that the other side knows nothing about
// must be findable without a third source of truth.
struct GapFinding {
    bool send_bound = false;
    bool budget_held = false;

    // Durable evidence that something may be on the wire, with nothing holding
    // budget behind it. This is the direction that costs money.
    [[nodiscard]] bool unbacked_send() const { return send_bound && !budget_held; }
    // Budget held for an order nobody is tracking. Costs availability only.
    [[nodiscard]] bool orphan_reservation() const { return budget_held && !send_bound; }
};

GapFinding inspect(jrnl::StableSendStore& send_store, const risk::RiskStore& risk_store) {
    GapFinding f;
    std::uint64_t seq = 0;
    jrnl::Digest hash{};
    f.send_bound = send_store.bound_sequence(kSession, kCommand, &seq, &hash);
    f.budget_held = reservation_held_for_chain(risk_store);
    return f;
}

std::unique_ptr<jrnl::StableSendStore> open_send_store(const std::filesystem::path& dir,
                                                       bool* wire_called) {
    jrnl::StableSendStore::Options o;
    o.path = (dir / "stable_send.db").string();
    o.wire = [wire_called](const jrnl::SessionKey&, std::uint64_t, const std::string&) {
        if (wire_called != nullptr) *wire_called = true;
        return true;
    };
    std::unique_ptr<jrnl::StableSendStore> store;
    std::string why;
    const jrnl::StoreError e = jrnl::StableSendStore::open(o, &store, &why);
    CHECK_MSG(e == jrnl::StoreError::None, why);
    return store;
}

// ---------------------------------------------------------------------------

void a_reservation_survives_a_restart_on_its_own() {
    std::printf("a_reservation_survives_a_restart_on_its_own\n");

    const std::filesystem::path dir = make_dir("restart");
    risk::LabOptions options;
    options.journal_path = (dir / "risk.journal").string();

    auto env = risk::LabEnv::create(options);
    CHECK_MSG(env.ok(), std::string(risk::reason_name(env.reason())) + ": " + env.detail());
    if (!env.ok()) return;
    risk::LabEnv& lab = *env.value();

    router::VirtualClock router_clock;
    {
        gw::RiskAdmissionAdapter adapter(lab.coordinator(), lab.store(), lab.clock(),
                                         router_clock, adapter_options(lab.trader()));
        const router::p05::RiskDecision d = adapter.reserve(a_new_order());
        CHECK_MSG(d.verdict == router::p05::Verdict::Approve, d.reason);
    }

    CHECK(reservation_held_for_chain(lab.store()));

    // P05's journal is durable on its own terms: one CRC-protected record per
    // commit, flushed before the acknowledgement. Nothing about P04 or P06 was
    // written, and the reservation does not care.
    const risk::Status restarted = lab.restart();
    CHECK_MSG(restarted.ok(), restarted.detail());

    // Still held, and still findable by chain. The budget did not quietly come
    // back: EI-25 again, from the other direction -- a restart is not evidence
    // that an order is gone.
    CHECK(reservation_held_for_chain(lab.store()));

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

void an_orphan_reservation_is_the_safe_direction() {
    std::printf("an_orphan_reservation_is_the_safe_direction\n");

    const std::filesystem::path dir = make_dir("orphan");
    risk::LabOptions options;
    options.journal_path = (dir / "risk.journal").string();
    auto env = risk::LabEnv::create(options);
    CHECK(env.ok());
    if (!env.ok()) return;
    risk::LabEnv& lab = *env.value();

    bool wire_called = false;
    std::unique_ptr<jrnl::StableSendStore> send_store = open_send_store(dir, &wire_called);
    if (!send_store) return;

    router::VirtualClock router_clock;
    gw::RiskAdmissionAdapter adapter(lab.coordinator(), lab.store(), lab.clock(), router_clock,
                                     adapter_options(lab.trader()));

    // The crash window: P05 committed, and the process died before P04's intent
    // and before P06 bound a sequence.
    CHECK(adapter.reserve(a_new_order()).verdict == router::p05::Verdict::Approve);

    const GapFinding f = inspect(*send_store, lab.store());
    CHECK(f.budget_held);
    CHECK(!f.send_bound);
    CHECK(f.orphan_reservation());
    CHECK(!f.unbacked_send());
    // Nothing reached the wire, which is the whole reason this direction is the
    // survivable one.
    CHECK(!wire_called);

    // And the evidence is honest about it: at INTENT_RECORDED an original may
    // still be sent, because nothing has left the building.
    CHECK(gw::may_send_original(gw::Evidence::IntentRecorded));

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

void an_unbacked_send_is_detectable_and_must_not_look_approved() {
    std::printf("an_unbacked_send_is_detectable_and_must_not_look_approved\n");

    const std::filesystem::path dir = make_dir("unbacked");

    bool wire_called = false;
    std::unique_ptr<jrnl::StableSendStore> send_store = open_send_store(dir, &wire_called);
    if (!send_store) return;

    // The dangerous window, in the only order that can produce it: P06 bound a
    // sequence and the bytes went out, and P05's commit is not there -- because
    // it was lost, or because this is a restored backup taken before it, or
    // because the two files were copied at different moments. Nothing in the
    // code prevents the combination, so the recovery path has to find it.
    const jrnl::SendBinding bound =
        send_store->send_once(kSession, kCommand, jrnl::sha256("35=D|11=O-1|"), "35=D|11=O-1|");
    CHECK(bound.bound);
    CHECK(wire_called);

    risk::LabOptions options;
    options.journal_path = (dir / "risk.journal").string();
    auto env = risk::LabEnv::create(options);
    CHECK(env.ok());
    if (!env.ok()) return;
    risk::LabEnv& lab = *env.value();

    const GapFinding f = inspect(*send_store, lab.store());
    CHECK(f.send_bound);
    CHECK(!f.budget_held);
    // The finding a recovery path must raise. It is computable from the two
    // stores alone, which is what makes it a check rather than a hope.
    CHECK(f.unbacked_send());
    CHECK(!f.orphan_reservation());

    // What may not happen next. The outcome is unknown -- the LP may be working
    // an order this gateway cannot account for -- and at that rung nothing may
    // be released and no original may be reissued. EI-20: a timeout is not
    // evidence, and EI-34: an unknown outbox row is not resent.
    CHECK(!gw::may_send_original(gw::Evidence::UnknownOutcome));
    CHECK(!gw::may_release_reservation(gw::Evidence::UnknownOutcome));
    // And the rung cannot be talked down to something that would permit either.
    CHECK(!gw::advances_to(gw::Evidence::UnknownOutcome, gw::Evidence::IntentRecorded));
    CHECK(!gw::advances_to(gw::Evidence::UnknownOutcome, gw::Evidence::FixQueued));
    // Only reconciled evidence leaves it.
    CHECK(gw::advances_to(gw::Evidence::UnknownOutcome, gw::Evidence::LpAccepted));
    CHECK(gw::advances_to(gw::Evidence::UnknownOutcome, gw::Evidence::Executed));

    // P06 can also count it without being asked about a particular key, which
    // is what a startup sweep needs.
    CHECK(send_store->allocated_sequences(kSession) == 1);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

}  // namespace

int main() {
    std::printf("P08 two-store gap: what EI-22 does not yet hold\n\n");

    a_reservation_survives_a_restart_on_its_own();
    an_orphan_reservation_is_the_safe_direction();
    an_unbacked_send_is_detectable_and_must_not_look_approved();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
