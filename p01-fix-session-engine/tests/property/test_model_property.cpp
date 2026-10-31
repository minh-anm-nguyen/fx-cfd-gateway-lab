// Property and model based testing.
//
// Design doc 17.3 layers 2 and 3, and the pass criteria in 17.5. A random event
// trace is replayed against the engine and after EVERY step the invariant set
// is re-checked. The seed is printed on failure so a counterexample replays
// exactly.
//
// Note the trap the doc calls out: "every outbound frame increments NextNumOut"
// is FALSE, because retransmissions and GapFills reuse the original sequence.
// The property asserted here is the correct one: every frame carrying a NEW
// sequence has a matching committed history row.
#include <map>
#include <set>
#include <string>
#include <vector>

#include "engine_fixture.hpp"
#include "fake_store.hpp"
#include "fix/wire.hpp"

using namespace fixse;

namespace {

// Deterministic generator so a seed reproduces a trace byte for byte.
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 6364136223846793005ull + 1442695040888963407ull) {}
    uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    size_t below(size_t n) { return n == 0 ? 0 : static_cast<size_t>(next() % n); }
    bool chance(size_t percent) { return below(100) < percent; }
};

// The reference model: just enough session state to judge the engine, written
// separately from the engine itself.
struct Model {
    uint64_t epoch = 1;
    uint64_t next_in = 1;
    uint64_t next_out = 1;
    std::set<uint64_t> committed_in;
    std::vector<uint64_t> delivered;
};

struct Invariants {
    std::string problem;

    // Checked after every event.
    bool check(const tt::MiniEngine& m, Model* model, size_t written_at_fault) {
        const SessionState st = m.engine->state();

        // I-02: neither sequence may regress inside an epoch.
        if (m.store.state.epoch == model->epoch) {
            if (m.store.state.next_in < model->next_in) {
                problem = "next_in regressed from " + std::to_string(model->next_in) + " to " +
                          std::to_string(m.store.state.next_in);
                return false;
            }
            if (m.store.state.next_out < model->next_out) {
                problem = "next_out regressed from " + std::to_string(model->next_out) + " to " +
                          std::to_string(m.store.state.next_out);
                return false;
            }
        }
        model->epoch = m.store.state.epoch;
        model->next_in = m.store.state.next_in;
        model->next_out = m.store.state.next_out;

        // I-04: an inbox row exists at most once per sequence, and application
        // deliveries are strictly increasing.
        for (const auto& kv : m.store.inbox) {
            if (kv.first >= m.store.state.next_in) {
                problem = "inbox row " + std::to_string(kv.first) + " is at or beyond next_in " +
                          std::to_string(m.store.state.next_in);
                return false;
            }
        }
        for (size_t i = 1; i < m.sink.delivered.size(); ++i) {
            if (m.sink.delivered[i] <= m.sink.delivered[i - 1]) {
                problem = "application delivery order broke at index " + std::to_string(i);
                return false;
            }
        }

        // I-06: every frame on the wire that carries a NEW sequence number must
        // have a committed history row with exactly those bytes.
        for (const std::string& f : m.transport.frames()) {
            if (peer::get(f, 43).value_or("") == "Y") continue;  // a retransmission
            const auto seq_text = peer::get(f, 34);
            if (!seq_text.has_value()) {
                problem = "a frame without MsgSeqNum reached the wire";
                return false;
            }
            const uint64_t seq = std::stoull(*seq_text);
            const auto row = m.store.outbound.find(seq);
            if (row == m.store.outbound.end()) {
                problem = "original sequence " + std::to_string(seq) +
                          " reached the wire with no history row";
                return false;
            }
            if (row->second.payload != f) {
                problem = "the frame on the wire differs from the stored original at sequence " +
                          std::to_string(seq);
                return false;
            }
        }

        // Nothing is transmitted once the engine has FAULTED.
        if (st == SessionState::Faulted && m.transport.written.size() > written_at_fault) {
            problem = "bytes were written after the engine faulted";
            return false;
        }

        // I-09: bounded buffers.
        if (m.engine->future_buffer_size() > m.cfg.max_future_messages) {
            problem = "out of order buffer exceeded its message cap";
            return false;
        }
        if (m.engine->future_buffer_bytes() > m.cfg.max_future_bytes) {
            problem = "out of order buffer exceeded its byte cap";
            return false;
        }
        if (m.engine->pending_inbox() > m.cfg.max_inbox_pending + 1) {
            problem = "durable inbox backlog exceeded its cap";
            return false;
        }

        // I-10: ACTIVE implies a completed handshake with no open gap.
        if (st == SessionState::Active && m.engine->open_gap().has_value()) {
            problem = "the session is ACTIVE while a gap is still open";
            return false;
        }
        if (m.engine->ready() && st != SessionState::Active) {
            problem = "readiness is true outside ACTIVE";
            return false;
        }
        return true;
    }
};

}  // namespace

namespace {

// One randomised trace. Returns an empty string on success, or a description of
// the invariant that broke.
std::string run_trace(uint64_t seed, int events) {
    Rng rng(seed);
    tt::MiniEngine m(1, 1);
    Model model;
    Invariants inv;
    size_t written_at_fault = static_cast<size_t>(-1);
    bool logged_on = false;
    uint64_t peer_seq = 1;

    const auto note_fault = [&]() {
        if (m.engine->state() == SessionState::Faulted &&
            written_at_fault == static_cast<size_t>(-1))
            written_at_fault = m.transport.written.size();
    };

    for (int step = 0; step < events; ++step) {
        const size_t pick = rng.below(100);

        if (m.engine->state() == SessionState::Faulted) {
            // Terminal until an operator intervenes: keep poking it to prove
            // nothing moves.
            m.advance(1000);
            if (!inv.check(m, &model, written_at_fault))
                return "step " + std::to_string(step) + ": " + inv.problem;
            continue;
        }

        if (m.engine->state() == SessionState::Backoff ||
            m.engine->state() == SessionState::Connecting) {
            m.advance(70000);
            if (m.engine->state() == SessionState::Connecting) {
                ++m.generation;
                m.connect();
                logged_on = false;
            }
        } else if (!logged_on && m.engine->state() == SessionState::AwaitLogon) {
            if (pick < 80) {
                m.feed(m.logon_reply(peer_seq));
                ++peer_seq;
            } else {
                peer_seq += 1 + rng.below(3);
                m.feed(m.logon_reply(peer_seq));
                ++peer_seq;
            }
            logged_on = m.engine->state() == SessionState::Active ||
                        m.engine->state() == SessionState::Synchronizing;
        } else if (pick < 30) {
            const uint64_t seq = m.store.state.next_in;
            m.feed(m.app(seq, "payload-" + std::to_string(seq)));
            if (peer_seq <= seq) peer_seq = seq + 1;
        } else if (pick < 40) {
            const uint64_t seq = m.store.state.next_in + 1 + rng.below(4);
            m.feed(m.app(seq, "future-" + std::to_string(seq)));
            if (peer_seq <= seq) peer_seq = seq + 1;
        } else if (pick < 48) {
            // A true duplicate: same sequence, same payload, properly flagged.
            if (m.store.state.next_in > 1) {
                const uint64_t seq =
                    1 + rng.below(static_cast<size_t>(m.store.state.next_in - 1));
                const auto row = m.store.inbox.find(seq);
                if (row != m.store.inbox.end()) {
                    peer::Msg dup = m.msg("U1", seq);
                    dup.poss_dup = true;
                    dup.orig_sending_time = format_utc_timestamp(m.clock.wall_now());
                    dup.extra.push_back({20001, peer::get(row->second.payload, 20001).value_or("p")});
                    m.feed(dup.build());
                }
            }
        } else if (pick < 56) {
            m.feed(m.msg("0", m.store.state.next_in).build());
        } else if (pick < 62) {
            peer::Msg tr = m.msg("1", m.store.state.next_in);
            tr.extra.push_back({112, "probe-" + std::to_string(step)});
            m.feed(tr.build());
        } else if (pick < 70) {
            peer::Msg rr = m.msg("2", m.store.state.next_in);
            const uint64_t begin = 1 + rng.below(4);
            rr.extra.push_back({7, std::to_string(begin)});
            rr.extra.push_back({16, rng.chance(50) ? std::string("0")
                                                   : std::to_string(begin + rng.below(5))});
            m.feed(rr.build());
        } else if (pick < 76) {
            peer::Msg gf = m.msg("4", m.store.state.next_in);
            gf.extra.push_back({123, "Y"});
            const uint64_t forward = m.store.state.next_in + 1 + rng.below(3);
            const uint64_t backward = m.store.state.next_in > 1 ? m.store.state.next_in - 1 : 1;
            gf.extra.push_back({36, std::to_string(rng.chance(70) ? forward : backward)});
            m.feed(gf.build());
        } else if (pick < 84) {
            if (m.engine->state() == SessionState::Active)
                m.engine->submit_app_message("U1", {{20001, "out-" + std::to_string(step)}});
        } else if (pick < 90) {
            m.advance(1000 + static_cast<long long>(rng.below(40000)));
        } else if (pick < 95) {
            m.engine->on_bytes(
                std::string(1 + rng.below(16), static_cast<char>('a' + rng.below(26))),
                m.generation);
        } else {
            m.engine->on_disconnect(m.generation, "random drop");
            logged_on = false;
        }

        note_fault();
        if (!inv.check(m, &model, written_at_fault))
            return "step " + std::to_string(step) + ": " + inv.problem;
    }
    return std::string();
}

}  // namespace

SCENARIO("PROP-01", "P0", "two hundred random event traces hold every invariant at every step") {
    for (uint64_t seed = 1; seed <= 200; ++seed) {
        const std::string problem = run_trace(seed, 60);
        CHECK_MSG(problem.empty(), "seed " + std::to_string(seed) + " -- " + problem);
    }
}

SCENARIO("PROP-02", "P0", "the same byte stream parses identically however it is fragmented") {
    // Design doc 17.3 layer 3: splitting the stream differently must not change
    // what the engine does.
    Rng rng(424242);
    for (int iter = 0; iter < 40; ++iter) {
        tt::EngineFixture whole("prop02-w-" + std::to_string(iter));
        tt::EngineFixture split("prop02-s-" + std::to_string(iter));
        whole.connect_and_logon();
        split.connect_and_logon();

        std::string stream;
        const uint64_t base = whole.engine->next_in();
        const size_t count = 1 + rng.below(8);
        for (size_t i = 0; i < count; ++i)
            stream += whole.app(base + i, "frag-" + std::to_string(i));

        whole.feed_bytes(stream);
        for (size_t pos = 0; pos < stream.size();) {
            const size_t n = 1 + rng.below(9);
            split.feed_bytes(std::string_view(stream).substr(pos, n));
            pos += n;
        }

        CHECK_EQ(split.sink.delivered.size(), whole.sink.delivered.size());
        CHECK_EQ(split.engine->next_in(), whole.engine->next_in());
        CHECK_EQ(split.engine->state(), whole.engine->state());
        for (size_t i = 0; i < whole.sink.delivered.size(); ++i)
            CHECK_EQ(split.sink.delivered[i], whole.sink.delivered[i]);
    }
}

SCENARIO("PROP-03", "P0", "an unclean restart at any point preserves committed state") {
    // Design doc 17.5: on crash, restart, resynchronise and re-check.
    for (int cut = 1; cut <= 12; ++cut) {
        tt::EngineFixture f("prop03-" + std::to_string(cut));
        f.connect_and_logon();
        for (int i = 0; i < cut; ++i) {
            if (i % 3 == 0) f.engine->submit_app_message("U1", {{20001, "o"}});
            f.feed(f.app(f.engine->next_in(), "i"));
        }
        const uint64_t in_before = f.engine->next_in();
        const uint64_t out_before = f.engine->next_out();
        const uint64_t epoch_before = f.engine->epoch();
        const size_t delivered_before = f.sink.delivered.size();

        f.restart(false);  // unclean, as after a crash
        CHECK_EQ(f.engine->epoch(), epoch_before);
        CHECK_EQ(f.engine->next_in(), in_before);
        CHECK_EQ(f.engine->next_out(), out_before);
        CHECK(f.sink.delivered.size() >= delivered_before);
        const ConsistencyReport rep = f.store->verify_consistency();
        CHECK_MSG(rep.ok, rep.problem);
    }
}

SCENARIO("PROP-04", "P0", "a retransmission never consumes a new sequence number") {
    // The explicit counter-property from design doc 17.5: it is NOT true that
    // every outbound frame increments NextNumOut.
    Rng rng(99887766);
    for (int iter = 0; iter < 30; ++iter) {
        tt::MiniEngine m(1, 1);
        m.connect_and_logon();
        const size_t originals = 1 + rng.below(6);
        for (size_t i = 0; i < originals; ++i)
            m.engine->submit_app_message("U1", {{20001, "h" + std::to_string(i)}});
        const uint64_t out_after_originals = m.store.state.next_out;
        m.out();

        // Any number of resend requests must not move NextNumOut.
        const size_t requests = 1 + rng.below(3);
        for (size_t r = 0; r < requests; ++r) {
            peer::Msg rr = m.msg("2", m.store.state.next_in);
            rr.extra.push_back({7, "1"});
            rr.extra.push_back({16, std::to_string(out_after_originals - 1)});
            m.feed(rr.build());
        }
        CHECK_EQ(m.store.state.next_out, out_after_originals);

        // And every replayed frame reuses a sequence that already has a row.
        for (const std::string& f : m.transport.new_frames()) {
            if (peer::get(f, 43).value_or("") != "Y") continue;
            const uint64_t seq = std::stoull(peer::get(f, 34).value());
            CHECK(seq < out_after_originals);
            if (peer::get(f, 35).value_or("") == "4") continue;  // a GapFill
            CHECK_EQ(m.store.outbound.count(seq), size_t{1});
        }
    }
}

int main(int argc, char** argv) { return tt::run_all(argc, argv, "model_property"); }
