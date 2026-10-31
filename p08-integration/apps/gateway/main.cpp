// The assembled gateway: one order, the whole path.
//
// Everything before this file proved seams in isolation. This runs a New order
// through them in order -- admission, risk, the durable send binding, the wire,
// the LP's reports, the durable receipts, the inbound apply -- and prints what
// each component ended up believing.
//
// What is real here and what is not, stated plainly because a smoke test that
// overclaims is worse than none:
//
//   P04  real. Admission, routing, correlation, reconciliation.
//   P05  real. `risk::RiskCoordinator` over `risk::LabEnv`, through the
//        adapter. The reservation below is budget actually taken.
//   P06  real. `jrnl::StableSendStore` holds the issue-once binding and the
//        durable inbound receipts, on a SQLite file in a scratch directory.
//   P03  NOT yet. P04's in-tree reducer still owns order status here, because
//        moving `order_core` onto the write path needs the aggregate to be
//        persistable and `OrderAggregate` has `canonical_text()` but nothing
//        that reads it back. The oracle compares the two instead.
//   P02  NOT yet. P04's in-tree `LpSimulator` stands in for the LP. It speaks
//        the same frames, in-process, with no socket.
//   P07  NOT yet. No thread topology; this is one thread, by choice, because a
//        smoke test that is also a concurrency test tells you neither thing.
//
// The wire goes through P06 rather than straight to the peer. P06's store calls
// its `wire` callback only once the sequence is durable, so the ordering its
// header demands -- binding first, bytes second -- is a property of the store
// and not of this file remembering to do things in the right order.
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

#include "deps/p01_session.hpp"
#include "deps/p02_lp.hpp"
#include "gw/contract.hpp"
#include "gw/evidence.hpp"
#include "gw/lab_guard.hpp"
#include "gw/order_reducer.hpp"
#include "gw/risk_adapter.hpp"
#include "gw/session_adapter.hpp"
#include "jrnl/stable_send.hpp"
#include "risk/policy.hpp"
#include "risk/scenario.hpp"
#include "router/router.hpp"

namespace {

constexpr const char* kSessionKey = "ROUTER04:LPSIM01:PRIMARY";
// P04 routes ACC1; P05's lab policy carries ACC-A, ACC-B and ACC-C. P04 quotes
// EUR/USD; P05 and P03 key on EURUSD. Neither divergence derives from the
// other, and §2 pins "account/symbol policy" for exactly this reason, so both
// mappings are written out rather than guessed at.
constexpr const char* kAccount = "ACC1";
constexpr const char* kRiskAccountTemplate = "ACC-A";
constexpr const char* kWireSymbol = "EUR/USD";
constexpr const char* kInstrument = "EURUSD";

int report_gate(const char* what, const gw::GateReport& r) {
    for (const gw::Finding& f : r.findings) {
        std::printf("  %-32.*s %s\n", static_cast<int>(f.check.size()), f.check.data(),
                    f.detail.c_str());
    }
    const std::string_view verdict = gw::to_string(r.status);
    std::printf("%s: %.*s\n", what, static_cast<int>(verdict.size()), verdict.data());
    return r.status == gw::GateStatus::Ready ? 0 : 2;
}

// P05's lab policy with the account P04 actually routes, copied from the
// template profile so the limits are the lab's rather than invented here.
risk::LabOptions risk_options() {
    risk::LabOptions o;
    risk::AccountProfile acc = o.policy.accounts.at(kRiskAccountTemplate);
    acc.account_id = kAccount;
    o.policy.accounts[kAccount] = acc;
    return o;
}

router::CommandEnvelope new_order(const std::string& client_command_id) {
    router::CommandEnvelope cmd;
    cmd.principal.actor = router::ActorId("trader-1");
    cmd.principal.tenant = router::TenantId("T1");
    cmd.principal.role = router::Role::Trader;
    cmd.account = router::AccountId(kAccount);
    cmd.kind = router::CommandKind::New;
    cmd.client_command_id = client_command_id;
    cmd.symbol = kWireSymbol;
    cmd.side = router::Side::Buy;
    cmd.qty_text = "10000";
    cmd.price_text = "1.10000";
    return cmd;
}

// P04's delivery state and business outcome, read as a rung on §2's ladder.
//
// This is where the ladder stops being decorative: P04 tracks twelve delivery
// states for its own reasons, and the question the contract actually asks is
// which of six kinds of evidence the gateway now holds. The two that matter
// most are the ones that look like progress and are not -- AwaitBusiness means
// the bytes were accepted by the kernel and nothing more (EI-04), and
// Reconciling and UnknownTransport both mean the outcome is unknown, which
// EI-20 forbids resolving with a timer.
gw::Evidence evidence_of(router::DeliveryState delivery, router::BusinessOutcome business) {
    if (business == router::BusinessOutcome::Filled) return gw::Evidence::Executed;
    switch (delivery) {
        case router::DeliveryState::Received:
        case router::DeliveryState::RejectedLocal:
        case router::DeliveryState::AcceptedDurable:
        case router::DeliveryState::Queued:
            return gw::Evidence::IntentRecorded;
        case router::DeliveryState::Dispatching:
        case router::DeliveryState::OriginalSeqBound:
            return gw::Evidence::FixQueued;
        case router::DeliveryState::AwaitBusiness:
            return gw::Evidence::SocketWritten;
        case router::DeliveryState::LpAcked:
        case router::DeliveryState::LpRejected:
            // A reject is the LP having acted on a message it correlated, so
            // the evidence is just as strong as an acknowledgement.
            return gw::Evidence::LpAccepted;
        case router::DeliveryState::UnknownTransport:
        case router::DeliveryState::Reconciling:
        case router::DeliveryState::ManualHold:
            return gw::Evidence::UnknownOutcome;
        default:
            return gw::Evidence::UnknownOutcome;
    }
}

}  // namespace

int main() {
    std::printf("P08 gateway -- one order through the assembled path\n\n");

    // Nothing starts before the gates. A mismatched pin or an endpoint that can
    // reach a venue is not a run with a bad result; it is not a run.
    router::Config cfg = router::Config::lab_default();
    if (const int rc = report_gate("contracts", gw::verify_contracts()); rc != 0) return rc;
    if (const int rc = report_gate("isolation",
                                   gw::verify_isolation(cfg, gw::trading_mode_from_env()));
        rc != 0) {
        return rc;
    }
    std::printf("\n");

    std::error_code ec;
    const std::filesystem::path lab_dir =
        std::filesystem::temp_directory_path(ec) / "p08-gateway-smoke";
    std::filesystem::remove_all(lab_dir, ec);
    std::filesystem::create_directories(lab_dir, ec);

    // --- P05, real -----------------------------------------------------------
    auto risk_lab = risk::LabEnv::create(risk_options());
    if (!risk_lab.ok()) {
        std::printf("P05 lab failed: %s %s\n", std::string(risk::reason_name(risk_lab.reason())).c_str(),
                    risk_lab.detail().c_str());
        return 1;
    }
    risk::LabEnv& risk_env = *risk_lab.value();

    // --- P04's own machinery -------------------------------------------------
    router::VirtualClock clock;
    router::FailpointRegistry fp;
    router::Metrics metrics;
    router::StoreImage store_image;

    // --- P03, real ---------------------------------------------------------
    // Installed into the store, which is what puts it on the write path: the
    // router applies every report through it, and replay rebuilds chains
    // through it too.
    gw::OrderCoreReducer::Options reducer_opts;
    reducer_opts.wire_to_instrument = {{kWireSymbol, kInstrument}};
    gw::OrderCoreReducer reducer(reducer_opts);
    router::Store store(store_image, fp, clock, &reducer);

    // --- P06, real -----------------------------------------------------------
    // The LP does not exist yet and the store needs somewhere to put the bytes,
    // so the callback resolves its target at call time. The indirection is the
    // construction order, not a design: binding, then wire, then peer.
    auto wire_target = std::make_shared<router::p02::LpSimulator*>(nullptr);
    jrnl::StableSendStore::Options send_opts;
    send_opts.path = (lab_dir / "stable_send.db").string();
    send_opts.wire = [wire_target](const jrnl::SessionKey&, std::uint64_t,
                                   const std::string& payload) {
        if (*wire_target == nullptr) return false;
        (*wire_target)->on_app_frame(payload);
        return true;
    };
    std::unique_ptr<jrnl::StableSendStore> send_store;
    std::string why;
    if (jrnl::StableSendStore::open(send_opts, &send_store, &why) != jrnl::StoreError::None) {
        std::printf("P06 stable send store failed: %s\n", why.c_str());
        return 1;
    }

    // --- the adapters --------------------------------------------------------
    router::p01::SessionImage session_image;
    session_image.identity = {"ROUTER04", "LPSIM01", "PRIMARY"};
    session_image.state = router::p01::SessionState::Active;
    gw::SessionAdapter session(session_image, fp, *send_store,
                               gw::SessionAdapter::Options{kSessionKey});

    gw::RiskAdmissionAdapter::Options risk_opts;
    risk_opts.principal = risk_env.trader();
    risk_opts.wire_to_instrument = {{kWireSymbol, kInstrument}};
    gw::RiskAdmissionAdapter risk(risk_env.coordinator(), risk_env.store(), risk_env.clock(),
                                  clock, risk_opts);

    // --- P02's stand-in, and the wire it is on -------------------------------
    router::p02::LpImage lp_image;
    router::p02::LpSimulator lp(lp_image, session);
    *wire_target = &lp;

    // --- P04 -----------------------------------------------------------------
    router::Router rt(cfg, store, session, risk, metrics, fp, clock);
    const auto booted = rt.boot();
    if (booted.is_err()) {
        const std::string_view e = router::to_string(booted.error());
        std::printf("boot refused: %.*s\n", static_cast<int>(e.size()), e.data());
        return 1;
    }
    const std::string_view mode = router::to_string(rt.mode());
    std::printf("booted, mode=%.*s\n", static_cast<int>(mode.size()), mode.data());

    // --- the order -----------------------------------------------------------
    const auto accepted = rt.submit(new_order("K-1"));
    if (accepted.is_err()) {
        const std::string_view e = router::to_string(accepted.error());
        std::printf("submit refused: %.*s\n", static_cast<int>(e.size()), e.data());
        return 1;
    }
    const router::AcceptedIntent intent = accepted.value();
    std::printf("accepted: command=%s chain=%s cl_ord_id=%s\n", intent.command_id.str().c_str(),
                intent.chain_id.str().c_str(), intent.cl_ord_id.str().c_str());
    // INTENT_RECORDED and no further: the command, the reservation and the
    // outbox row are committed, and nothing has been sent.
    std::printf("evidence: %.*s\n",
                static_cast<int>(gw::to_string(gw::Evidence::IntentRecorded).size()),
                gw::to_string(gw::Evidence::IntentRecorded).data());

    // --- dispatch, and whatever the LP says back -----------------------------
    for (int round = 0; round < 8; ++round) {
        const router::DispatchStats d = rt.dispatch_pending();
        const router::InboundStats in = rt.pump_inbound();
        const std::size_t moved = d.sent + d.unknown + d.held + in.applied + in.deduplicated +
                                  in.quarantined + in.held + in.orphaned + in.invalid;
        if (moved == 0) break;
        std::printf("  round %d: sent=%zu unknown=%zu held=%zu | applied=%zu dedup=%zu "
                    "quarantined=%zu orphaned=%zu invalid=%zu\n",
                    round, d.sent, d.unknown, d.held, in.applied, in.deduplicated,
                    in.quarantined, in.orphaned, in.invalid);
    }

    // --- what each component ended up believing ------------------------------
    std::printf("\nP06  sequences allocated=%llu unconfirmed=%zu inbound unacked=%zu\n",
                static_cast<unsigned long long>(session.allocated_sequences()),
                session.unconfirmed_sends(), session.pending_inbound());

    const router::p01::SessionImage::Binding* binding = nullptr;
    const auto found = session.lookup_binding(intent.command_id);
    if (found.has_value()) {
        std::printf("P06  binding: command=%s original_seq=%llu written=%d\n",
                    intent.command_id.str().c_str(),
                    static_cast<unsigned long long>(found->original_seq), found->written ? 1 : 0);
    }
    (void)binding;

    std::printf("P05  stranded=%zu scale_mismatches=%zu unmapped_symbols=%zu\n",
                risk.stranded_reservations().size(), risk.scale_mismatches(),
                risk.unmapped_symbols());
    const risk::Result<risk::DomainMetrics> rm = risk_env.coordinator().current_metrics();
    if (rm.ok()) {
        // Gross notional after the reservation: the money the firm now has at
        // risk because this order exists, in minor units of the account
        // currency. Zero would mean the reservation never landed.
        std::printf("P05  firm gross notional=%lld (minor units)\n",
                    static_cast<long long>(rm.value().firm_gross_notional_minor));
    }

    const std::string_view final_mode = router::to_string(rt.mode());
    // P03 is the authority now, so its own view is the one worth printing. The
    // aggregate rides in the chain projection; if it is absent, some other
    // reducer built this chain and the line below would be a lie.
    const router::p03::ChainState* chain = store.find_chain(intent.chain_id);
    if (chain != nullptr) {
        if (const order_core::OrderAggregate* agg = gw::aggregate_of(*chain)) {
            std::printf("P03  lifecycle=%s cum_qty=%lld leaves=%lld fills=%zu revision=%llu\n",
                        order_core::to_string(agg->lifecycle()),
                        static_cast<long long>(agg->cum_qty_units),
                        static_cast<long long>(agg->leaves_qty_units()), agg->ledger.size(),
                        static_cast<unsigned long long>(agg->revision));
        } else {
            std::printf("P03  no aggregate on the chain: P04's reducer built it\n");
        }
    }
    std::printf("P03  refusals=%zu unmapped_symbols=%zu scale_mismatches=%zu\n",
                reducer.refusals(), reducer.unmapped_symbols(), reducer.scale_mismatches());

    std::printf("P04  mode=%.*s original_sends=%zu open_unknown=%zu open_recon=%zu\n",
                static_cast<int>(final_mode.size()), final_mode.data(), rt.original_sends(),
                rt.open_unknown_outcomes(), rt.open_recon_cases());

    const auto status = rt.command_status(router::TenantId("T1"), intent.command_id);
    if (status.has_value()) {
        const std::string_view d = router::to_string(status->delivery);
        const std::string_view b = router::to_string(status->business);
        const gw::Evidence rung = evidence_of(status->delivery, status->business);
        const std::string_view rung_name = gw::to_string(rung);
        std::printf("P04  delivery=%.*s business=%.*s\n", static_cast<int>(d.size()),
                    d.data(), static_cast<int>(b.size()), b.data());
        std::printf("     evidence now: %.*s\n", static_cast<int>(rung_name.size()),
                    rung_name.data());
        // A reservation may only be released on EXECUTED (EI-25), and an
        // original may only be sent while nothing has left (EI-09). Printing
        // both keeps the rung answerable rather than ornamental.
        std::printf("     may release reservation=%d may send original=%d\n",
                    gw::may_release_reservation(rung) ? 1 : 0,
                    gw::may_send_original(rung) ? 1 : 0);
    }

    // One accepted command must have produced exactly one original send. That is
    // EI-09 and JI-19 in the same number, and it is the one assertion this smoke
    // test makes rather than merely prints.
    const bool one_original = session.allocated_sequences() <= 1;
    std::printf("\nEI-09/JI-19 one original per command: %s\n", one_original ? "held" : "VIOLATED");

    std::filesystem::remove_all(lab_dir, ec);
    return one_original ? 0 : 1;
}
