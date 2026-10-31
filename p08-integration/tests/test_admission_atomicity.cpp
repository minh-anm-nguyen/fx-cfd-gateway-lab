// EI-22 across components, which is the part P05 cannot answer alone.
//
// A correction first, because an earlier reading of this was wrong. EI-22 asks
// that "reservation + command intent + outbox" be written atomically, and P05
// already does it: one `commit(CommitRequest)` carries the command record, the
// reservation, the outbox intent, the exposure projection and the audit event
// into a single CRC-protected journal record, flushed before the
// acknowledgement. P05's own crash suite proves it -- for windows CW-05 through
// CW-11 it reopens the journal and asserts no reservation and no outbox row
// survived, and for CW-12 that a durable commit whose acknowledgement was lost
// is recoverable by command id. So the write-set is atomic and durable.
//
// What P05 cannot check is whether anyone else agrees. The router keeps its own
// record of the same admission, and P06 keeps the send binding. The question
// this file asks is the integration one: when P05's commit rolls back, does the
// rest of the gateway also show nothing accepted, or does an order exist
// somewhere with no budget behind it?
//
// The remaining non-atomicity, stated so it is not mistaken for a gap: P06's
// send binding is a separate commit on purpose. Its own header explains why --
// the mapping has to be durable BEFORE the socket write, so it cannot be inside
// the business transaction that precedes it.
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "deps/p01_session.hpp"
#include "deps/p02_lp.hpp"
#include "gw/order_reducer.hpp"
#include "gw/risk_adapter.hpp"
#include "gw/session_adapter.hpp"
#include "jrnl/stable_send.hpp"
#include "risk/failpoint.hpp"
#include "risk/policy.hpp"
#include "risk/scenario.hpp"
#include "router/router.hpp"

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

constexpr const char* kSessionKey = "ROUTER04:LPSIM01:PRIMARY";
constexpr const char* kAccount = "ACC1";
constexpr const char* kWireSymbol = "EUR/USD";
constexpr const char* kInstrument = "EURUSD";

// The whole gateway, assembled the way apps/gateway does it, on a scratch
// directory. Kept here rather than shared with the app because a test that
// reuses the app's wiring proves the wiring, not the behaviour.
struct Gateway {
    std::filesystem::path dir;

    std::unique_ptr<risk::LabEnv> risk_env;
    router::VirtualClock clock;
    router::FailpointRegistry fp;
    router::Metrics metrics;
    router::StoreImage store_image;
    router::Config cfg = router::Config::lab_default();

    std::shared_ptr<router::p02::LpSimulator*> wire_target =
        std::make_shared<router::p02::LpSimulator*>(nullptr);
    std::unique_ptr<jrnl::StableSendStore> send_store;

    router::p01::SessionImage session_image;
    std::unique_ptr<gw::OrderCoreReducer> reducer;
    std::unique_ptr<router::Store> store;
    std::unique_ptr<gw::SessionAdapter> session;
    std::unique_ptr<gw::RiskAdmissionAdapter> risk;
    router::p02::LpImage lp_image;
    std::unique_ptr<router::p02::LpSimulator> lp;
    std::unique_ptr<router::Router> rt;
    bool booted = false;

    explicit Gateway(const std::string& name) {
        std::error_code ec;
        dir = std::filesystem::temp_directory_path(ec) / ("p08-atomicity-" + name);
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);

        risk::LabOptions o;
        // Journal-backed, because the whole subject is what survives a commit
        // that did not happen.
        o.journal_path = (dir / "risk.journal").string();
        risk::AccountProfile acc = o.policy.accounts.at("ACC-A");
        acc.account_id = kAccount;
        o.policy.accounts[kAccount] = acc;
        auto created = risk::LabEnv::create(o);
        CHECK_MSG(created.ok(), created.detail());
        if (!created.ok()) return;
        risk_env = std::move(created.value());

        jrnl::StableSendStore::Options so;
        so.path = (dir / "stable_send.db").string();
        auto target = wire_target;
        so.wire = [target](const jrnl::SessionKey&, std::uint64_t, const std::string& payload) {
            if (*target == nullptr) return false;
            (*target)->on_app_frame(payload);
            return true;
        };
        std::string why;
        CHECK_MSG(jrnl::StableSendStore::open(so, &send_store, &why) == jrnl::StoreError::None,
                  why);
        if (!send_store) return;

        gw::OrderCoreReducer::Options ro;
        ro.wire_to_instrument = {{kWireSymbol, kInstrument}};
        reducer = std::make_unique<gw::OrderCoreReducer>(ro);
        store = std::make_unique<router::Store>(store_image, fp, clock, reducer.get());

        session_image.identity = {"ROUTER04", "LPSIM01", "PRIMARY"};
        session_image.state = router::p01::SessionState::Active;
        session = std::make_unique<gw::SessionAdapter>(session_image, fp, *send_store,
                                                       gw::SessionAdapter::Options{kSessionKey});

        gw::RiskAdmissionAdapter::Options ao;
        ao.principal = risk_env->trader();
        ao.wire_to_instrument = {{kWireSymbol, kInstrument}};
        risk = std::make_unique<gw::RiskAdmissionAdapter>(risk_env->coordinator(),
                                                          risk_env->store(), risk_env->clock(),
                                                          clock, ao);

        lp = std::make_unique<router::p02::LpSimulator>(lp_image, *session);
        *wire_target = lp.get();

        rt = std::make_unique<router::Router>(cfg, *store, *session, *risk, metrics, fp, clock);
        booted = rt->boot().is_ok();
    }

    ~Gateway() {
        rt.reset();
        lp.reset();
        risk.reset();
        session.reset();
        store.reset();
        reducer.reset();
        send_store.reset();
        risk_env.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    router::CommandEnvelope new_order(const std::string& id) const {
        router::CommandEnvelope cmd;
        cmd.principal.actor = router::ActorId("trader-1");
        cmd.principal.tenant = router::TenantId("T1");
        cmd.principal.role = router::Role::Trader;
        cmd.account = router::AccountId(kAccount);
        cmd.kind = router::CommandKind::New;
        cmd.client_command_id = id;
        cmd.symbol = kWireSymbol;
        cmd.side = router::Side::Buy;
        cmd.qty_text = "10000";
        cmd.price_text = "1.10000";
        return cmd;
    }

    bool any_reservation() const {
        return !risk_env->store().reservations().empty();
    }
    bool any_outbox() const { return !risk_env->store().outbox().empty(); }
    bool any_send_binding(const std::string& command) const {
        std::uint64_t seq = 0;
        jrnl::Digest hash{};
        return send_store->bound_sequence(kSessionKey, command, &seq, &hash);
    }
};

// ---------------------------------------------------------------------------

void a_clean_admission_lands_in_all_three() {
    std::printf("a_clean_admission_lands_in_all_three\n");

    Gateway g("clean");
    CHECK(g.booted);
    if (!g.booted) return;

    const auto accepted = g.rt->submit(g.new_order("K-1"));
    CHECK(accepted.is_ok());
    if (!accepted.is_ok()) return;

    // P05's single transaction: the command record, the reservation and the
    // outbox intent, all of them or none.
    CHECK(g.any_reservation());
    CHECK(g.any_outbox());
    CHECK(g.risk_env->store().find_command("T1", kAccount, accepted.value().command_id.str())
              .has_value());

    // P04 agrees, and after dispatch so does P06.
    CHECK(g.rt->command_status(router::TenantId("T1"), accepted.value().command_id).has_value());
    g.rt->dispatch_pending();
    CHECK(g.any_send_binding(accepted.value().command_id.str()));
}

void a_rolled_back_risk_commit_leaves_nothing_anywhere() {
    std::printf("a_rolled_back_risk_commit_leaves_nothing_anywhere\n");

    // Every window where P05 could die between the parts of its write-set. Its
    // own suite proves the journal rolls back; this asks whether the router and
    // P06 agree afterwards.
    const risk::Failpoint windows[] = {
        risk::Failpoint::AfterCommandInsert,      // CW-06
        risk::Failpoint::AfterReservationInsert,  // CW-07
        risk::Failpoint::AfterProjectionUpdate,   // CW-08
        risk::Failpoint::AfterAuditInsert,        // CW-09
        risk::Failpoint::AfterOutboxInsert,       // CW-10
        risk::Failpoint::AfterRevisionCas,        // CW-11
    };

    for (const risk::Failpoint fp : windows) {
        const std::string window(risk::failpoint_window_id(fp));
        Gateway g("rollback-" + window);
        if (!g.booted) {
            CHECK_MSG(false, window + ": gateway did not boot");
            continue;
        }

        g.risk_env->failpoints().arm(fp, risk::FailAction::CommitFail);
        const auto accepted = g.rt->submit(g.new_order("K-1"));

        // The router must refuse. UNAVAILABLE fails closed for a New, which is
        // OR-A-16, and P05 reports HoldRetryable for a definitive commit
        // failure.
        CHECK_MSG(accepted.is_err(), window + ": the router accepted an order whose risk "
                                              "commit rolled back");

        // Nothing durable on P05's side.
        CHECK_MSG(!g.any_reservation(), window + ": a hold survived a rollback");
        CHECK_MSG(!g.any_outbox(), window + ": an outbox intent survived a rollback");

        // And nothing anywhere else. This is the part P05 cannot assert: no
        // command the router will account for, and no sequence bound that would
        // make an order look sent.
        CHECK_MSG(!g.rt->command_status(router::TenantId("T1"), router::CommandId("K-1"))
                       .has_value(),
                  window + ": the router kept a command record");
        CHECK_MSG(!g.any_send_binding("K-1"), window + ": a sequence was bound");
        CHECK_MSG(g.rt->original_sends() == 0, window + ": something was sent");

        // Nothing reached the LP either, which is the only evidence that
        // matters to the counterparty.
        CHECK_MSG(g.lp_image.received_frames.empty(),
                  window + ": a frame reached the LP");
    }
}

void an_unacknowledged_commit_is_not_a_rollback() {
    std::printf("an_unacknowledged_commit_is_not_a_rollback\n");

    Gateway g("ack-lost");
    CHECK(g.booted);
    if (!g.booted) return;

    // CW-12: the journal record is durable, the acknowledgement is not. RI-24
    // and EI-31: this must never be read as a rollback, because the budget is
    // genuinely held.
    g.risk_env->failpoints().arm(risk::Failpoint::AfterDurableCommitBeforeAck,
                                 risk::FailAction::CommitAckLost);
    const auto accepted = g.rt->submit(g.new_order("K-1"));

    // The router fails closed, as it must: it cannot prove the order is safe.
    CHECK(accepted.is_err());

    // But the reservation is there, and that is the point. Treating this as a
    // rollback and retrying would reserve twice for one order.
    CHECK(g.any_reservation());

    // And nothing was sent on the strength of an answer the gateway never got.
    CHECK(!g.any_send_binding("K-1"));
    CHECK(g.rt->original_sends() == 0);
}

}  // namespace

int main() {
    std::printf("P08 admission atomicity: EI-22 across P04, P05 and P06\n\n");

    a_clean_admission_lands_in_all_three();
    a_rolled_back_risk_commit_leaves_nothing_anywhere();
    an_unacknowledged_commit_is_not_a_rollback();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
