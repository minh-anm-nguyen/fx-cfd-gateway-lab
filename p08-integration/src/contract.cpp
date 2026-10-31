#include "gw/contract.hpp"

// One header from each of the seven components. The includes are half the
// check on their own: the seven were written separately, and every one of them
// ships its own store.hpp, failpoint.hpp, clock.hpp, result.hpp, metrics.hpp,
// ids.hpp and hash.hpp. They coexist here only because each stayed inside its
// own namespace, and this translation unit is where that stops being an
// assumption.
#include "fix/result.hpp"
#include "fix/sqlite_store.hpp"
#include "jrnl/result.hpp"
#include "jrnl/schema.hpp"
#include "llx/dispatch.hpp"
#include "llx/event.hpp"
#include "lp/result.hpp"
#include "lp/sqlite_store.hpp"
#include "order_core/reducer.hpp"
#include "risk/rational.hpp"
#include "router/config.hpp"
#include "router/result.hpp"

namespace gw {
namespace {

// Every durable store in the lab is pinned to one migration level. Three
// components own a store and each tracks its own version independently
// (fixse::SqliteSessionStore, lpsim and jrnl), so nothing but this check stops
// one of them from being migrated alone.
constexpr int kPinnedStoreSchema = 1;

std::int64_t pow10(int n) {
    std::int64_t v = 1;
    for (int i = 0; i < n; ++i) v *= 10;
    return v;
}

// Calls one out-of-line function from each of the seven libraries.
//
// A header constant proves nothing about linkage, because the compiler folds
// it and the linker never looks for the library. With static libraries the
// linker also pulls in only the objects something references, so an
// unreferenced component contributes no symbols and any conflict with it stays
// hidden until a scenario touches it. These seven calls are the references
// that make a conflict -- two copies of sqlite3, a duplicate symbol between
// two components -- fail at link time, before any run claims a result.
bool all_libraries_linked() {
    const bool p01 = !fixse::to_string(fixse::StoreError::None).empty();
    const bool p02 = !lpsim::to_string(lpsim::StoreError::None).empty();
    const bool p03 = order_core::to_string(order_core::Decision::Applied) != nullptr;
    const bool p04 = !router::to_string(router::RoutingError::None).empty();
    const bool p05 = !risk::rational_to_string(risk::Rational{1, 1}).empty();
    const bool p06 = !jrnl::to_string(jrnl::StoreError::None).empty();
    const bool p07 = !llx::to_string(llx::SendResult::Acked).empty();
    return p01 && p02 && p03 && p04 && p05 && p06 && p07;
}

void expect(GateReport& r, bool ok, std::string_view check, std::string detail) {
    if (ok) return;
    r.findings.push_back(Finding{check, std::move(detail)});
}

}  // namespace

std::string_view to_string(GateStatus s) {
    switch (s) {
        case GateStatus::Ready: return "READY";
        case GateStatus::Blocked: return "BLOCKED";
    }
    return "GATE_INVALID";
}

std::vector<ComponentPin> components() {
    return {
        {"P01", "fixse", "TCP/FIX session, sequence numbers, wire receipt",
         fixse::SqliteSessionStore::kSchemaVersion},
        {"P02", "lpsim", "LP orders, liquidity fills, independent LP ledger",
         lpsim::kSchemaVersion},
        {"P03", "order_core", "order status and the fill ledger", -1},
        {"P04", "order_router", "command ids, FIX mapping, correlation, recon", -1},
        {"P05", "risk_engine", "risk budget, exposure, reservations", -1},
        {"P06", "jrnl", "the atomic transaction, inbox, outbox, checkpoints",
         jrnl::kSchemaVersion},
        {"P07", "llx", "queue, pool and thread execution", -1},
    };
}

GateReport verify_contracts() {
    GateReport r;

    expect(r, all_libraries_linked(), "libraries.linked",
           "at least one component library reported an empty identity string");

    // The numeric contract. P07 is the binding component: kPxScale and
    // kQtyScale are compile-time constants, so a disagreement here cannot be
    // configured away at startup -- it needs a recompile. EI-15 requires a
    // scale mismatch to be rejected rather than rounded.
    expect(r, llx::kPxScale == kNumericContract.px_scale, "numeric.px_scale",
           "P07 compiled with llx::kPxScale=" + std::to_string(llx::kPxScale) +
               ", contract pins " + std::to_string(kNumericContract.px_scale));
    expect(r, llx::kQtyScale == kNumericContract.qty_scale, "numeric.qty_scale",
           "P07 compiled with llx::kQtyScale=" + std::to_string(llx::kQtyScale) +
               ", contract pins " + std::to_string(kNumericContract.qty_scale));
    expect(r, pow10(kNumericContract.price_digits) == kNumericContract.px_scale,
           "numeric.self_consistent",
           "contract pins price_digits=" + std::to_string(kNumericContract.price_digits) +
               " but px_scale=" + std::to_string(kNumericContract.px_scale));

    // P04 carries the same number per symbol rather than globally, so its
    // default is the only part of it a static check can reach. A symbol
    // configured with a different scale is a runtime admission concern, which
    // is why the adapter rejects rather than rescales.
    const router::SymbolProfile default_profile{};
    expect(r, default_profile.price_scale == kNumericContract.price_digits,
           "numeric.router_default_scale",
           "P04 router::SymbolProfile defaults to price_scale=" +
               std::to_string(default_profile.price_scale) + ", contract pins " +
               std::to_string(kNumericContract.price_digits));

    // The three independent store schemas.
    for (const ComponentPin& c : components()) {
        if (c.store_schema < 0) continue;
        expect(r, c.store_schema == kPinnedStoreSchema,
               "schema.store_version",
               std::string(c.id) + " (" + std::string(c.library) + ") is at schema " +
                   std::to_string(c.store_schema) + ", lab pins " +
                   std::to_string(kPinnedStoreSchema));
    }

    r.status = r.findings.empty() ? GateStatus::Ready : GateStatus::Blocked;
    return r;
}

}  // namespace gw
