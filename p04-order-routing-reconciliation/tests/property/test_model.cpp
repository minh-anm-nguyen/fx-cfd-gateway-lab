// Property based exploration. Each case is a deterministic random walk with the
// twenty-six invariants checked after every step, so a failure is reproducible
// from the seed printed in the message (design doc 19.1 and 19.4).
#include "support/model.hpp"

using namespace router;

SCENARIO("PROP-01", "P1", "a long random walk never breaks an invariant") {
    lab::Model::Options o;
    o.seed = 42006;
    o.max_chains = 12;
    lab::Model m(o);
    m.run(20000);
    CHECK_EQ(m.steps_run, std::size_t{20000});
    // The walk has to actually have done something, or it proves nothing.
    CHECK(m.applied_fills > 0);
    CHECK(m.crashes > 0);
    CHECK(m.duplicate_reports > 0);
    CHECK(m.lab.disk.chains.size() > 0);
}

SCENARIO("PROP-02", "P1", "ten seeds of the same walk all hold") {
    for (std::uint64_t seed = 1; seed <= 10; ++seed) {
        lab::Model::Options o;
        o.seed = seed;
        o.max_chains = 8;
        lab::Model m(o);
        m.run(2000);
    }
}

SCENARIO("PROP-03", "P0", "forced identifier collisions stay quarantined, never credited") {
    lab::Model::Options o;
    o.seed = 777;
    o.inject_id_collisions = true;
    lab::Model m(o);
    m.run(6000);

    // The collided identity exists at most once, on exactly one chain.
    std::size_t holders = 0;
    for (const auto& [id, c] : m.lab.disk.chains) {
        (void)id;
        for (const p03::Fill& f : c.fills)
            if (f.exec_id.str() == "EX-COLLIDE") ++holders;
    }
    CHECK(holders <= std::size_t{1});
    // And the dedup ledger agrees with the chains.
    for (const auto& [k, ek] : m.lab.disk.exec_keys) {
        (void)k;
        CHECK(m.lab.disk.chains.count(ek.chain_id.str()) == std::size_t{1});
    }
}

SCENARIO("PROP-04", "P0", "values at the integer boundary never wrap the ledger") {
    lab::Model::Options o;
    o.seed = 31337;
    o.inject_extreme_numbers = true;
    lab::Model m(o);
    m.run(6000);

    for (const auto& [id, c] : m.lab.disk.chains) {
        (void)id;
        CHECK(c.cum_qty >= 0);
        CHECK(c.leaves_qty >= 0);
        CHECK(c.px_qty_weighted >= 0);
        std::int64_t sum = 0;
        for (const p03::Fill& f : c.fills) {
            CHECK(f.qty > 0);
            CHECK(f.px > 0);
            sum += f.qty;
        }
        CHECK_EQ(sum, c.cum_qty);
    }
}

SCENARIO("PROP-05", "P0", "a random walk replays from its journal to the same state") {
    for (std::uint64_t seed : {std::uint64_t{11}, std::uint64_t{22}, std::uint64_t{33}}) {
        lab::Model::Options o;
        o.seed = seed;
        o.max_chains = 6;
        o.inject_faults = false;  // no crashes, so the journal covers everything
        lab::Model m(o);
        m.run(3000);

        const auto replayed = replay_chains_from_journal(m.lab.disk);
        for (const auto& [id, live] : m.lab.disk.chains) {
            auto it = replayed.find(id);
            CHECK(it != replayed.end());
            CHECK_EQ(it->second.state_hash().hex(), live.state_hash().hex());
        }
    }
}
