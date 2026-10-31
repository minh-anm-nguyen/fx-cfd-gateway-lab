// Property and differential testing, section 26.3.
//
// The reference model below is deliberately written from the prose rules
// rather than from the reducer, and shares no code with it. Section 26.3
// item 1 asks for exactly that: a model the system under test cannot
// accidentally agree with because they are the same function.
//
// A counterexample is shrunk to the shortest prefix that still fails and
// reported with its seed, so the failure message is a reproduction recipe.
#include <set>
#include <string>
#include <vector>

#include "lab.hpp"

using namespace lab;

namespace {

struct Rng {
    std::uint32_t s;
    explicit Rng(std::uint32_t seed) : s(seed) {}
    std::uint32_t next() {
        s = s * 1664525u + 1013904223u;
        return s;
    }
    std::uint32_t below(std::uint32_t n) { return n == 0 ? 0 : next() % n; }
};

// The independent model. Plain arithmetic over the rules as written in
// sections 7 and 9: an order has a quantity, fills accumulate but may not
// exceed it, a terminal state closes it, and a hold covers what has not
// yet traded.
struct Model {
    bool exists = false;
    bool terminal = false;
    std::int64_t qty = 0;
    std::int64_t cum = 0;
    std::int64_t held = 0;
    std::int64_t exposed = 0;
    // Executions the model has already accounted for. A second report of
    // one of these must change nothing; a first report of an id the model
    // has not seen is an ordinary fill, whatever the generator called it.
    std::set<int> applied;

    std::int64_t leaves() const { return terminal ? 0 : qty - cum; }
};

enum class Op { New, Fill, Cancel, Duplicate };

struct Step {
    Op op = Op::New;
    std::int64_t qty = 0;
    int exec = 0;
};

std::string describe(const std::vector<Step>& steps) {
    std::string out;
    for (const Step& s : steps) {
        switch (s.op) {
            case Op::New: out += "NEW(" + std::to_string(s.qty) + ") "; break;
            case Op::Fill: out += "FILL(E" + std::to_string(s.exec) + "," +
                                  std::to_string(s.qty) + ") "; break;
            case Op::Cancel: out += "CANCEL "; break;
            case Op::Duplicate: out += "DUP(E" + std::to_string(s.exec) + ") "; break;
        }
    }
    return out;
}

// Runs one sequence against both the engine and the model and returns the
// first disagreement, or an empty string.
std::string run(const std::vector<Step>& steps, const std::string& label) {
    Lab l(label, 1000000);
    l.start();
    Model m;
    std::vector<Step> applied;

    auto mismatch = [&](const std::string& what) {
        return what + " after: " + describe(applied);
    };

    for (const Step& s : steps) {
        applied.push_back(s);
        switch (s.op) {
            case Op::New: {
                if (m.exists) break;  // the generator only opens one chain
                const CommitOutcome o =
                    l.service().submit_new(l.new_order("K1", "O1", s.qty), l.token());
                if (o.kind == CommitKind::Committed) {
                    m.exists = true;
                    m.qty = s.qty;
                    m.held = s.qty;
                }
                break;
            }
            case Op::Fill:
            case Op::Duplicate: {
                if (!m.exists) break;
                const CommitOutcome o =
                    l.deliver(l.fill("O1", "E" + std::to_string(s.exec), s.qty, 10000, "K1"));
                if (m.applied.count(s.exec) != 0) {
                    // Already accounted for: a redelivery must be inert.
                    if (o.kind == CommitKind::Committed) {
                        return mismatch("the engine applied an execution twice");
                    }
                    break;
                }
                // The model's rule for a first sighting: it applies only
                // when the order is live and the quantity fits.
                const bool model_applies = !m.terminal && s.qty > 0 && s.qty <= m.leaves();
                if (model_applies) {
                    if (o.kind != CommitKind::Committed) {
                        return mismatch("the model applied a fill the engine refused");
                    }
                    const std::int64_t take = s.qty < m.held ? s.qty : m.held;
                    m.cum += s.qty;
                    m.held -= take;
                    m.exposed += take;
                    m.applied.insert(s.exec);
                    if (m.cum == m.qty) {
                        m.terminal = true;
                        m.exposed += m.held;
                        m.held = 0;
                    }
                } else if (o.kind == CommitKind::Committed) {
                    return mismatch("the engine applied a fill the model refused");
                }
                break;
            }
            case Op::Cancel: {
                if (!m.exists) break;
                const CommitOutcome o = l.deliver(l.terminal("O1", "CANCELED", "K1"));
                const bool model_applies = !m.terminal;
                if (model_applies) {
                    if (o.kind != CommitKind::Committed) {
                        return mismatch("the model cancelled where the engine refused");
                    }
                    m.terminal = true;
                    m.held = 0;
                } else if (o.kind == CommitKind::Committed) {
                    return mismatch("the engine cancelled an already-terminal order");
                }
                break;
            }
        }

        // Invariants after every single step, not only at the end.
        const OrderState o = l.order("O1");
        if (o.cum_qty != m.cum) {
            return mismatch("cum_qty " + std::to_string(o.cum_qty) + " but the model says " +
                            std::to_string(m.cum));
        }
        if (o.exists && o.leaves_qty != m.leaves()) {
            return mismatch("leaves_qty " + std::to_string(o.leaves_qty) +
                            " but the model says " + std::to_string(m.leaves()));
        }
        const RiskDomainState r = l.risk();
        if (r.reserved != m.held) {
            return mismatch("reserved " + std::to_string(r.reserved) + " but the model says " +
                            std::to_string(m.held));
        }
        if (r.exposure != m.exposed) {
            return mismatch("exposure " + std::to_string(r.exposure) + " but the model says " +
                            std::to_string(m.exposed));
        }
        if (o.cum_qty > o.order_qty) return mismatch("the engine overfilled");
        if (r.utilised() > r.limit) return mismatch("the engine exceeded the risk ceiling");
    }

    const VerifyReport v = verify_all(l.store(), Lab::kTenant);
    if (!v.ok()) {
        return mismatch("integrity verification failed: " +
                        (v.details.empty() ? std::string() : v.details.front()));
    }
    // The replay oracle, on top of the model oracle.
    for (const StreamId& s : l.store().list_streams(Lab::kTenant)) {
        if (!replay_from_empty(l.store(), Lab::kTenant, s).ok) {
            return mismatch("stream " + s + " did not replay");
        }
    }
    return std::string();
}

std::vector<Step> generate(Rng& rng) {
    std::vector<Step> steps;
    Step open;
    open.op = Op::New;
    open.qty = 1000 * static_cast<std::int64_t>(1 + rng.below(20));
    steps.push_back(open);

    const int n = static_cast<int>(3 + rng.below(8));
    for (int i = 0; i < n; ++i) {
        Step s;
        const std::uint32_t roll = rng.below(10);
        if (roll < 6) {
            s.op = Op::Fill;
            s.exec = i;
            // Deliberately sometimes too large, to exercise the refusal.
            s.qty = static_cast<std::int64_t>(rng.below(2) == 0 ? 500 + rng.below(3000)
                                                                 : 1 + rng.below(30000));
        } else if (roll < 8) {
            // A redelivery has to carry the SAME economics, or it is a
            // fingerprint conflict rather than a duplicate, and the right
            // answer would be a quarantine instead of a no-op.
            s.op = Op::Duplicate;
            s.exec = static_cast<int>(rng.below(static_cast<std::uint32_t>(i + 1)));
            for (const Step& prior : steps) {
                if (prior.exec == s.exec &&
                    (prior.op == Op::Fill || prior.op == Op::Duplicate)) {
                    s.qty = prior.qty;
                    break;
                }
            }
            if (s.qty == 0) s.qty = 500;
        } else {
            s.op = Op::Cancel;
        }
        steps.push_back(s);
    }
    return steps;
}

// Shortest failing prefix. Section 26.3 asks for a shrunk counterexample
// rather than the raw sequence that happened to find the bug.
std::vector<Step> shrink(const std::vector<Step>& steps, const std::string& label) {
    for (std::size_t len = 1; len < steps.size(); ++len) {
        const std::vector<Step> prefix(steps.begin(), steps.begin() + len);
        if (!run(prefix, label + "-shrink-" + std::to_string(len)).empty()) return prefix;
    }
    return steps;
}

SCENARIO("PROP-01", "P0", "the engine agrees with an independently written model") {
    for (std::uint32_t seed = 1; seed <= 40; ++seed) {
        Rng rng(seed * 2654435761u);
        const std::vector<Step> steps = generate(rng);
        const std::string why = run(steps, "prop-01-" + std::to_string(seed));
        if (!why.empty()) {
            const std::vector<Step> small = shrink(steps, "prop-01-" + std::to_string(seed));
            TT_FAIL("seed " + std::to_string(seed) + ": " + why +
                    "\n          shrunk to: " + describe(small));
        }
    }
}

SCENARIO("PROP-02", "P0", "no sequence can overfill or breach the ceiling") {
    for (std::uint32_t seed = 100; seed < 130; ++seed) {
        Rng rng(seed * 40503u);
        Lab l("prop-02-" + std::to_string(seed), /*limit=*/30000);
        l.start();
        for (int i = 0; i < 12; ++i) {
            const std::string k = "K" + std::to_string(i);
            const std::string c = "O" + std::to_string(i);
            l.service().submit_new(
                l.new_order(k, c, 1000 + static_cast<std::int64_t>(rng.below(8000))), l.token());
            if (l.order(c).exists) {
                l.deliver(l.fill(c, "E" + std::to_string(i),
                                 1 + static_cast<std::int64_t>(rng.below(12000)), 10000, k));
            }
        }
        const RiskDomainState r = l.risk();
        CHECK_MSG(r.utilised() <= r.limit, "seed " + std::to_string(seed) + ": utilised " +
                                               std::to_string(r.utilised()) + " over limit " +
                                               std::to_string(r.limit));
        CHECK_EQ(l.scalar("SELECT COUNT(*) FROM orders WHERE cum_qty>order_qty;"),
                 std::int64_t(0));
        expect_consistent(l, "PROP-02");
    }
}

SCENARIO("PROP-03", "P0", "replay from empty equals the live projection for every sequence") {
    for (std::uint32_t seed = 200; seed < 220; ++seed) {
        Rng rng(seed * 2246822519u);
        Lab l("prop-03-" + std::to_string(seed), 1000000);
        l.start();
        for (int i = 0; i < 8; ++i) {
            const std::string k = "K" + std::to_string(i);
            const std::string c = "O" + std::to_string(i);
            l.service().submit_new(l.new_order(k, c, 10000), l.token());
            const int fills = static_cast<int>(rng.below(4));
            for (int f = 0; f < fills; ++f) {
                l.deliver(l.fill(c, "E" + std::to_string(i) + "-" + std::to_string(f), 1000,
                                 10000, k));
            }
            if (rng.below(3) == 0) l.deliver(l.terminal(c, "CANCELED", k));
        }
        for (const StreamId& s : l.store().list_streams(Lab::kTenant)) {
            const ReplayResult r = replay_from_empty(l.store(), Lab::kTenant, s);
            CHECK_MSG(r.ok, "seed " + std::to_string(seed) + ", stream " + s + ": " + r.detail);
            CHECK_EQ(to_hex(r.state_hash), to_hex(l.store().head(Lab::kTenant, s).state_hash));
        }
    }
}

SCENARIO("PROP-04", "P0", "the canonical codec round-trips arbitrary generated payloads") {
    Rng rng(31337);
    for (int i = 0; i < 400; ++i) {
        Payload p;
        const int fields = static_cast<int>(1 + rng.below(6));
        for (int f = 0; f < fields; ++f) {
            const std::string key = "k" + std::to_string(rng.below(26));
            switch (rng.below(4)) {
                case 0: p.i(key, static_cast<std::int64_t>(rng.next()) - 2000000000LL); break;
                case 1: p.s(key, std::string(1 + rng.below(8), static_cast<char>(
                                                 'a' + rng.below(26)))); break;
                case 2: p.b(key, rng.below(2) == 0); break;
                default: p.n(key); break;
            }
        }
        const CanonValue v = p.build();
        const std::string text = canonical_encode(v);
        CanonValue back;
        std::string why;
        CHECK_MSG(canonical_decode(text, &back, &why), "iteration " + std::to_string(i) + ": " +
                                                           why + " on " + text);
        CHECK_EQ(canonical_encode(back), text);
        CHECK(back == v);
    }
}

SCENARIO("PROP-05", "P0", "a duplicate execution is idempotent under every interleaving") {
    Rng rng(4242);
    for (std::uint32_t round = 0; round < 12; ++round) {
        Lab l("prop-05-" + std::to_string(round), 1000000);
        l.start();
        l.service().submit_new(l.new_order("K1", "O1", 50000), l.token());

        std::vector<std::string> execs;
        for (int i = 0; i < 10; ++i) {
            const bool repeat = !execs.empty() && rng.below(2) == 0;
            const std::string id =
                repeat ? execs[rng.below(static_cast<std::uint32_t>(execs.size()))]
                       : "E" + std::to_string(i);
            const CommitOutcome o = l.deliver(l.fill("O1", id, 1000, 10000, "K1"));
            if (o.kind == CommitKind::Committed) execs.push_back(id);
        }
        // Exactly one ledger row per distinct execution that was accepted.
        CHECK_EQ(l.row_count("fills"), execs.size());
        CHECK_EQ(l.order("O1").cum_qty, static_cast<std::int64_t>(execs.size()) * 1000);
        expect_consistent(l, "PROP-05");
    }
}

}  // namespace

int main(int argc, char** argv) { return tt::run_all(argc, argv, "test_model_property"); }
