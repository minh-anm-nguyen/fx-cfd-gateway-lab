// Property-based and model-based testing against an INDEPENDENT oracle.
//
// Matrix coverage: H-11, M-02, M-05, M-10, and the property layers 2 and 3 of
// design doc 18.2.
//
// Design doc 18.3 is explicit that the reference model must not be the
// OrderActor: comparing an implementation against itself proves nothing. The
// model below is therefore written from the FIX rules directly, in a few dozen
// lines, with no shared code with the actor beyond the fixed-point helpers. It
// is deliberately simple-minded - a flat struct and straight-line arithmetic -
// because its job is to be obviously correct, not to be efficient.
//
// The PRNG is pinned (a small xorshift with a stated algorithm) rather than
// std::rand or std::mt19937 with an unspecified distribution, because M-12
// requires a failing run to be reproducible from its seed alone.

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "lp/scenario.hpp"
#include "../support/harness.hpp"

using namespace lpsim;
using namespace tt;

namespace {

// ---------------------------------------------------------------------------
// Pinned PRNG: xorshift64*, stated here so a seed means the same thing forever.
// ---------------------------------------------------------------------------
class Rng {
public:
    explicit Rng(std::uint64_t seed) : s_(seed == 0 ? 0x9E3779B97F4A7C15ull : seed) {}
    std::uint64_t next() {
        s_ ^= s_ >> 12;
        s_ ^= s_ << 25;
        s_ ^= s_ >> 27;
        return s_ * 0x2545F4914F6CDD1Dull;
    }
    // Uniform in [0, n).
    std::uint64_t below(std::uint64_t n) { return n == 0 ? 0 : next() % n; }
    bool chance(int percent) { return static_cast<int>(below(100)) < percent; }

private:
    std::uint64_t s_;
};

// ---------------------------------------------------------------------------
// Independent reference model
// ---------------------------------------------------------------------------

// Written from the FIX 4.4 rules, not from the implementation. Shares nothing
// with OrderChain.
struct ModelOrder {
    Units order_qty = 0;
    Units filled = 0;
    Ticks limit = 0;
    Side side = Side::Buy;
    bool finished = false;
    std::string final_state;  // FILLED / CANCELED / EXPIRED, empty while live
    std::string pending;      // CANCEL / REPLACE, empty when none
    std::string current_id;
    std::vector<std::pair<Units, Ticks>> trades;

    Units remaining() const {
        if (finished) return 0;
        const Units r = order_qty - filled;
        return r > 0 ? r : 0;
    }

    std::string status() const {
        if (finished) return final_state;
        if (!pending.empty()) return pending == "CANCEL" ? "PENDING_CANCEL" : "PENDING_REPLACE";
        return filled == 0 ? "NEW" : "PARTIALLY_FILLED";
    }

    // Weighted average, recomputed from the trade list every time. No
    // incremental state, so there is nothing to drift.
    Ticks avg() const {
        if (trades.empty()) return 0;
        __int128 notional = 0;
        Units qty = 0;
        for (const auto& t : trades) {
            notional += static_cast<__int128>(t.first) * t.second;
            qty += t.first;
        }
        if (qty == 0) return 0;
        const __int128 d = qty;
        return static_cast<Ticks>((notional * 2 + d) / (d * 2));
    }
};

// What the model says should happen to a fill request.
bool model_accepts_fill(const ModelOrder& o, Units qty, Ticks px, Units slice_available,
                        Ticks quoted, bool quote_fresh) {
    if (o.finished) return false;
    if (qty <= 0) return false;
    if (!quote_fresh) return false;
    if (px != quoted) return false;
    // A buyer may not pay above its limit; a seller may not accept below it.
    if (o.side == Side::Buy ? (px > o.limit) : (px < o.limit)) return false;
    if (qty > o.remaining()) return false;
    if (qty > slice_available) return false;
    return true;
}

// ---------------------------------------------------------------------------

struct Sut {
    LpFixture fx;
    std::vector<std::string> storage;

    explicit Sut(const std::string& tag, FixtureOptions opt = {}) : fx(tag, std::move(opt)) {
        CHECK(fx.start());
        CHECK(fx.logon());
    }
    const OrderChain* order(const std::string& id) { return fx.actor().find_by_cl_ord_id(id); }
};

// Compare one chain against the model on every field that matters.
void compare(const OrderChain& sut, const ModelOrder& model, const std::string& where) {
    CHECK_MSG(sut.cum_qty == model.filled,
              where + ": CumQty " + format_qty(sut.cum_qty) + " vs model " +
                  format_qty(model.filled));
    CHECK_MSG(sut.leaves_qty == model.remaining(),
              where + ": LeavesQty " + format_qty(sut.leaves_qty) + " vs model " +
                  format_qty(model.remaining()));
    CHECK_MSG(sut.effective_qty == model.order_qty,
              where + ": OrderQty " + format_qty(sut.effective_qty) + " vs model " +
                  format_qty(model.order_qty));
    CHECK_MSG(sut.avg_px_ticks == model.avg(),
              where + ": AvgPx " + format_qty(sut.avg_px_ticks) + " vs model " +
                  format_qty(model.avg()));
    const std::string sut_status =
        sut.pending != Pending::None && !is_terminal(sut.base)
            ? std::string(to_string(sut.pending))
            : std::string(to_string(sut.base));
    CHECK_MSG(sut_status == model.status(),
              where + ": status " + sut_status + " vs model " + model.status());
}

}  // namespace

// ---------------------------------------------------------------------------
// Property: randomised event sequences against the model
// ---------------------------------------------------------------------------

SCENARIO("M-02", "P0", "randomised legal and illegal event sequences track the model") {
    // A spread of seeds rather than one, so a property that holds by luck for a
    // single interleaving does not pass.
    const std::uint64_t seeds[] = {20261030, 1, 7, 42, 99991, 123456789};

    for (std::uint64_t seed : seeds) {
        Rng rng(seed);
        Sut s("model-" + std::to_string(seed));

        // The book the model also reasons about.
        Units ask_available = 0;
        Ticks quoted_ask = 108490;
        std::uint64_t quote_version = 0;

        std::map<std::string, ModelOrder> model;  // keyed by the order's ORIGINAL id
        std::map<std::string, std::string> id_to_original;  // current id -> original
        int next_order = 0;
        int next_request = 0;

        for (int step = 0; step < 120; ++step) {
            const int roll = static_cast<int>(rng.below(100));

            if (roll < 20) {
                // --- a new quote -------------------------------------------
                ++quote_version;
                quoted_ask = 108480 + static_cast<Ticks>(rng.below(40));
                ask_available = static_cast<Units>(1000 * (1 + rng.below(80)));
                const QuoteError qe = s.fx.set_quote(
                    "EURUSD", format_price(quoted_ask - 10, 5).c_str(),
                    format_price(quoted_ask, 5).c_str(), 80000, ask_available,
                    quote_version, 600000);
                CHECK_EQ(qe, QuoteError::Ok);

            } else if (roll < 50) {
                // --- a new order -------------------------------------------
                ++next_order;
                const std::string id = "O" + std::to_string(next_order);
                NewOrderArgs a;
                a.cl_ord_id = id;
                // Always on the lot step, so the order is legal and the model
                // does not have to duplicate the validator.
                const Units qty = static_cast<Units>(1000 * (1 + rng.below(100)));
                a.qty = format_qty(qty);
                const Ticks limit = 108480 + static_cast<Ticks>(rng.below(60));
                a.price = format_price(limit, 5);
                s.fx.feed(s.fx.peer().new_order(a));

                const OrderChain* c = s.order(id);
                CHECK_MSG(c != nullptr, "order " + id + " was not accepted");
                ModelOrder m;
                m.order_qty = qty;
                m.limit = limit;
                m.side = Side::Buy;
                m.current_id = id;
                model[id] = m;
                id_to_original[id] = id;
                compare(*c, m, "after new " + id);

            } else if (roll < 80 && !model.empty()) {
                // --- a fill attempt ----------------------------------------
                auto it = model.begin();
                std::advance(it, static_cast<long>(rng.below(model.size())));
                const std::string original = it->first;
                ModelOrder& m = it->second;
                const OrderChain* c = s.order(m.current_id);
                CHECK(c != nullptr);

                // Sometimes a legal fill, sometimes a deliberately illegal one.
                Units qty = 0;
                Ticks px = quoted_ask;
                if (rng.chance(70) && m.remaining() > 0 && ask_available > 0) {
                    qty = std::min<Units>(m.remaining(), ask_available);
                    if (qty > 1000) qty = 1000 * (1 + static_cast<Units>(rng.below(
                                                           static_cast<std::uint64_t>(qty / 1000))));
                } else {
                    // An overfill, a zero, or a price off the quote.
                    switch (rng.below(3)) {
                        case 0: qty = m.order_qty + 1000; break;
                        case 1: qty = 0; break;
                        default:
                            qty = 1000;
                            px = quoted_ask + 7;
                            break;
                    }
                }

                const bool expect =
                    model_accepts_fill(m, qty, px, ask_available, quoted_ask,
                                       quote_version > 0);
                const ActorOutcome out =
                    s.fx.inject_fill(c->order_id, qty, format_price(px, 5).c_str());

                CHECK_MSG(out.accepted == expect,
                          "fill on " + original + " qty=" + format_qty(qty) + " px=" +
                              format_price(px, 5) + ": sut " +
                              (out.accepted ? "accepted" : "refused") + ", model " +
                              (expect ? "accepted" : "refused"));
                if (expect) {
                    m.filled += qty;
                    m.trades.emplace_back(qty, px);
                    ask_available -= qty;
                    if (m.filled >= m.order_qty) {
                        m.finished = true;
                        m.final_state = "FILLED";
                        m.pending.clear();
                    }
                }
                compare(*s.order(m.current_id), m, "after fill on " + original);

            } else if (roll < 92 && !model.empty()) {
                // --- a cancel ----------------------------------------------
                auto it = model.begin();
                std::advance(it, static_cast<long>(rng.below(model.size())));
                ModelOrder& m = it->second;
                ++next_request;
                CancelArgs ca;
                ca.cl_ord_id = "C" + std::to_string(next_request);
                ca.orig_cl_ord_id = m.current_id;
                const bool expect = !m.finished && m.pending.empty();
                s.fx.feed(s.fx.peer().cancel(ca));

                if (expect) {
                    m.finished = true;
                    m.final_state = "CANCELED";
                    m.pending.clear();
                    m.current_id = ca.cl_ord_id;
                    id_to_original[ca.cl_ord_id] = it->first;
                }
                compare(*s.order(m.current_id), m, "after cancel on " + it->first);

            } else if (!model.empty()) {
                // --- a replace ---------------------------------------------
                auto it = model.begin();
                std::advance(it, static_cast<long>(rng.below(model.size())));
                ModelOrder& m = it->second;
                ++next_request;
                ReplaceArgs ra;
                ra.cl_ord_id = "R" + std::to_string(next_request);
                ra.orig_cl_ord_id = m.current_id;
                // A quantity that is sometimes below the filled amount, which
                // the strict profile must refuse (G-04).
                const Units new_qty = static_cast<Units>(1000 * (1 + rng.below(120)));
                ra.qty = format_qty(new_qty);
                ra.price = format_price(m.limit, 5);

                const bool expect =
                    !m.finished && m.pending.empty() && new_qty >= m.filled;
                s.fx.feed(s.fx.peer().replace(ra));

                if (expect) {
                    m.order_qty = new_qty;
                    m.current_id = ra.cl_ord_id;
                    id_to_original[ra.cl_ord_id] = it->first;
                    if (m.filled >= m.order_qty) {
                        m.finished = true;
                        m.final_state = "FILLED";
                    }
                }
                compare(*s.order(m.current_id), m, "after replace on " + it->first);
            }

            // Invariants after every single committed event, which is what
            // design doc 18.3 asks for.
            std::string why;
            CHECK_MSG(s.fx.actor().check_invariants(&why),
                      "seed " + std::to_string(seed) + " step " + std::to_string(step) +
                          ": " + why);
            CHECK_MSG(s.fx.actor().state() != ActorState::Halted,
                      "actor halted: " + s.fx.actor().halt_detail());
        }

        // The wire is conformant and every identity is unique, at the end of a
        // long randomised run rather than only in hand-built cases.
        check_all_sent_conformant(s.fx);
        check_exec_ids_unique(s.fx);
        check_outbound_seq_unique(s.fx);

        // Conservation: the sum of all filled quantities equals the sum of the
        // ledger, and never exceeds what the book offered.
        Units model_total = 0;
        for (const auto& kv : model) model_total += kv.second.filled;
        Units sut_total = 0;
        for (const OrderChain* c : s.fx.actor().chains()) sut_total += c->cum_qty;
        CHECK_EQ(sut_total, model_total);
    }
}

// ---------------------------------------------------------------------------
// Model-based: exhaust the small state space
// ---------------------------------------------------------------------------

SCENARIO("M-05", "P0", "every cancel, fill and replace interleaving on a tiny order holds") {
    // Design doc 18.2 layer 3: a finite model with bounded quantities, to
    // exhaust interleavings rather than sample them. The order is two lots, so
    // the reachable states are few enough to enumerate completely.
    enum class Act { Fill1, Fill2, Cancel, Replace3, Replace1 };
    const Act acts[] = {Act::Fill1, Act::Fill2, Act::Cancel, Act::Replace3, Act::Replace1};
    const int kActs = 5;

    int run = 0;
    // Every ordered triple of actions.
    for (int a = 0; a < kActs; ++a) {
        for (int b = 0; b < kActs; ++b) {
            for (int c = 0; c < kActs; ++c) {
                const Act seq[3] = {acts[a], acts[b], acts[c]};
                ++run;

                Sut s("mbt-" + std::to_string(run));
                CHECK_EQ(s.fx.set_quote("EURUSD", "1.08480", "1.08490", 80000, 10000, 1,
                                        600000),
                         QuoteError::Ok);
                NewOrderArgs na;
                na.qty = "2000";
                na.price = "1.08500";
                s.fx.feed(s.fx.peer().new_order(na));
                const OrderChain* chain = s.order("O1");
                CHECK(chain != nullptr);
                const std::string order_id = chain->order_id;

                ModelOrder m;
                m.order_qty = 2000;
                m.limit = 108500;
                m.current_id = "O1";

                Units available = 10000;
                int request = 0;

                for (int step = 0; step < 3; ++step) {
                    switch (seq[step]) {
                        case Act::Fill1:
                        case Act::Fill2: {
                            const Units qty = seq[step] == Act::Fill1 ? 1000 : 2000;
                            const bool expect = model_accepts_fill(m, qty, 108490, available,
                                                                   108490, true);
                            const ActorOutcome out =
                                s.fx.inject_fill(order_id, qty, "1.08490");
                            CHECK_MSG(out.accepted == expect,
                                      "run " + std::to_string(run) + " step " +
                                          std::to_string(step) + " fill " +
                                          format_qty(qty));
                            if (expect) {
                                m.filled += qty;
                                m.trades.emplace_back(qty, 108490);
                                available -= qty;
                                if (m.filled >= m.order_qty) {
                                    m.finished = true;
                                    m.final_state = "FILLED";
                                }
                            }
                            break;
                        }
                        case Act::Cancel: {
                            ++request;
                            CancelArgs ca;
                            ca.cl_ord_id = "C" + std::to_string(request);
                            ca.orig_cl_ord_id = m.current_id;
                            const bool expect = !m.finished;
                            s.fx.feed(s.fx.peer().cancel(ca));
                            if (expect) {
                                m.finished = true;
                                m.final_state = "CANCELED";
                                m.current_id = ca.cl_ord_id;
                            }
                            break;
                        }
                        case Act::Replace3:
                        case Act::Replace1: {
                            ++request;
                            const Units new_qty = seq[step] == Act::Replace3 ? 3000 : 1000;
                            ReplaceArgs ra;
                            ra.cl_ord_id = "R" + std::to_string(request);
                            ra.orig_cl_ord_id = m.current_id;
                            ra.qty = format_qty(new_qty);
                            ra.price = "1.08500";
                            const bool expect = !m.finished && new_qty >= m.filled;
                            s.fx.feed(s.fx.peer().replace(ra));
                            if (expect) {
                                m.order_qty = new_qty;
                                m.current_id = ra.cl_ord_id;
                                if (m.filled >= m.order_qty) {
                                    m.finished = true;
                                    m.final_state = "FILLED";
                                }
                            }
                            break;
                        }
                    }

                    compare(*s.order(m.current_id), m,
                            "run " + std::to_string(run) + " step " + std::to_string(step));
                    std::string why;
                    CHECK_MSG(s.fx.actor().check_invariants(&why), why);
                }
                check_all_sent_conformant(s.fx);
                check_exec_ids_unique(s.fx);
            }
        }
    }
    CHECK_EQ(run, kActs * kActs * kActs);
}

// ---------------------------------------------------------------------------
// Determinism
// ---------------------------------------------------------------------------

SCENARIO("H-11c", "P0", "the same seed produces the same logical outcome twice") {
    auto run_once = [](const std::string& tag) {
        Sut s(tag);
        Rng rng(20261030);
        CHECK_EQ(s.fx.set_quote("EURUSD", "1.08480", "1.08490", 80000, 100000, 1, 600000),
                 QuoteError::Ok);

        std::vector<std::string> trace;
        for (int i = 1; i <= 25; ++i) {
            NewOrderArgs a;
            a.cl_ord_id = "O" + std::to_string(i);
            a.qty = format_qty(static_cast<Units>(1000 * (1 + rng.below(5))));
            a.price = format_price(108490 + static_cast<Ticks>(rng.below(20)), 5);
            s.fx.feed(s.fx.peer().new_order(a));
            const OrderChain* c = s.order(a.cl_ord_id);
            if (c == nullptr) continue;

            if (rng.chance(60)) {
                const Units qty = std::min<Units>(c->leaves_qty, 1000);
                const ActorOutcome out = s.fx.inject_fill(c->order_id, qty, "1.08490");
                trace.push_back(a.cl_ord_id + " fill=" + (out.accepted ? "y" : "n"));
            }
            const OrderChain* after = s.order(a.cl_ord_id);
            trace.push_back(a.cl_ord_id + " cum=" + format_qty(after->cum_qty) +
                            " leaves=" + format_qty(after->leaves_qty) +
                            " avg=" + format_qty(after->avg_px_ticks));
        }
        return trace_hash(trace);
    };

    // Two independent runs from clean stores: the canonical log must match.
    const std::uint64_t a = run_once("det-a");
    const std::uint64_t b = run_once("det-b");
    CHECK_EQ(a, b);
}

SCENARIO("M-04", "P0", "each case owns its store, so order of execution cannot matter") {
    // Two fixtures alive at once with the same business identities. If they
    // shared a database the second logon or the second O1 would collide.
    Sut a("isolate-a");
    Sut b("isolate-b");
    CHECK_EQ(a.fx.set_quote("EURUSD", "1.08480", "1.08490", 80000, 40000, 1, 600000),
             QuoteError::Ok);
    CHECK_EQ(b.fx.set_quote("EURUSD", "1.08480", "1.08490", 80000, 40000, 1, 600000),
             QuoteError::Ok);

    a.fx.feed(a.fx.peer().new_order(NewOrderArgs{}));
    b.fx.feed(b.fx.peer().new_order(NewOrderArgs{}));

    CHECK(a.order("O1") != nullptr);
    CHECK(b.order("O1") != nullptr);
    // Same ClOrdID, same OrderID, entirely separate state.
    CHECK_EQ(a.order("O1")->order_id, b.order("O1")->order_id);

    a.fx.inject_fill(a.order("O1")->order_id, 40000, "1.08490");
    CHECK_EQ(a.order("O1")->cum_qty, static_cast<Units>(40000));
    CHECK_EQ(b.order("O1")->cum_qty, static_cast<Units>(0));
}

int main(int argc, char** argv) { return tt::run_all(argc, argv, "model"); }
