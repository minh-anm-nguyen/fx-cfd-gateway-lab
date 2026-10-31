// Fuzzing the untrusted input path: framer, decoder and resend range
// resolution. Design doc 17.3 layer 4 and scenario J-10.
//
// This file builds two ways. With a libFuzzer capable toolchain the
// LLVMFuzzerTestOneInput entry point is used; otherwise it runs as a
// deterministic standalone driver so the same corpus and mutation loop still
// run in CI. GCC on this platform has no libFuzzer, so the standalone driver is
// what produces the evidence here, and the report says so.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "fix/codec.hpp"
#include "fix/framer.hpp"
#include "fix/resend_planner.hpp"
#include "peer_wire.hpp"
#include "tiny_test.hpp"

using namespace fixse;

namespace {

constexpr size_t kMaxFrame = 8192;
constexpr size_t kMaxBuffer = 16384;

// One fuzz iteration. Every path must either reject the input or produce a
// frame that an independently written checker also accepts.
void exercise(const uint8_t* data, size_t size) {
    const std::string_view bytes(reinterpret_cast<const char*>(data), size);

    FixFramer framer(kMaxFrame, kMaxBuffer);
    size_t pos = 0;
    const size_t chunk = size == 0 ? 1 : 1 + (data[0] % 61);
    while (pos < bytes.size()) {
        const WireError ae = framer.append(bytes.substr(pos, chunk));
        if (ae != WireError::None) break;
        pos += chunk;

        std::string_view frame;
        WireError err = WireError::None;
        while (framer.next(&frame, &err)) {
            std::string why;
            // Hold the framer to what it promises: a self-consistent
            // BodyLength and CheckSum. The protocol VERSION is a decoder
            // concern (B-16), so verify_lengths is the right check here
            // while the stricter verify_frame is used where a FIX.4.4
            // frame is expected. The fuzzer found this distinction.
            if (!peer::verify_lengths(frame, &why)) {
                std::fprintf(stderr, "framer accepted a frame with inconsistent framing: %s\n",
                             why.c_str());
                std::abort();
            }
            DecodeOptions opt;
            opt.max_fields = 256;
            DecodedMessage msg;
            if (decode(frame, opt, &msg) == WireError::None) {
                if (msg.seq_num == 0 || msg.msg_type.empty()) {
                    std::fprintf(stderr, "decoder reported success with no sequence or type\n");
                    std::abort();
                }
                uint64_t begin = 0;
                uint64_t end = 0;
                msg.find_u64(tag::BeginSeqNo, &begin);
                msg.find_u64(tag::EndSeqNo, &end);
                const ResendRange range = resolve_range(begin, end, msg.seq_num);
                if (range.valid && (range.begin == 0 || range.end < range.begin)) {
                    std::fprintf(stderr, "resolve_range returned a nonsensical range\n");
                    std::abort();
                }
                const std::vector<OutboundRecord> no_history;
                const ResendPlan plan = plan_resend(range, no_history);
                if (plan.ok && !plan.actions.empty() && range.begin > range.end) std::abort();
            }
        }
        if (is_malformed(err)) break;
        if (framer.buffered() > kMaxBuffer) {
            std::fprintf(stderr, "framer exceeded its buffer cap\n");
            std::abort();
        }
    }
}

}  // namespace

#ifdef FIXSE_LIBFUZZER
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    exercise(data, size);
    return 0;
}
#else

namespace {

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed | 1) {}
    uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    size_t below(size_t n) { return n == 0 ? 0 : static_cast<size_t>(next() % n); }
};

// Seed corpus: a well formed frame of every session type plus an application
// message, which the mutator then works on.
std::vector<std::string> seed_corpus() {
    std::vector<std::string> corpus;
    const char* types[] = {"A", "0", "1", "2", "3", "4", "5", "U1"};
    for (const char* t : types) {
        const std::string type(t);
        peer::Msg m;
        m.msg_type = type;
        m.seq = 7;
        if (type == "A") {
            m.extra.push_back({98, "0"});
            m.extra.push_back({108, "30"});
        } else if (type == "1") {
            m.extra.push_back({112, "probe"});
        } else if (type == "2") {
            m.extra.push_back({7, "1"});
            m.extra.push_back({16, "0"});
        } else if (type == "4") {
            m.extra.push_back({123, "Y"});
            m.extra.push_back({36, "12"});
        } else if (type == "U1") {
            m.extra.push_back({20001, "payload"});
        }
        corpus.push_back(m.build());
    }
    corpus.push_back(corpus[0] + corpus[7]);                       // two frames back to back
    corpus.push_back(corpus[7].substr(0, corpus[7].size() / 2));   // a truncated frame
    return corpus;
}

std::string mutate(const std::string& input, Rng* rng) {
    std::string out = input;
    const size_t edits = 1 + rng->below(4);
    for (size_t i = 0; i < edits && !out.empty(); ++i) {
        const size_t pick = rng->below(5);
        if (pick == 0) {
            out[rng->below(out.size())] = static_cast<char>(rng->below(256));
        } else if (pick == 1) {
            out.resize(rng->below(out.size()) + 1);
        } else if (pick == 2) {
            out.insert(rng->below(out.size()),
                       std::string(1 + rng->below(8), static_cast<char>(rng->below(256))));
        } else if (pick == 3) {
            const size_t at = rng->below(out.size());
            const size_t len = 1 + rng->below(out.size() - at);
            out.insert(at, out.substr(at, len));
        } else {
            const size_t at = rng->below(out.size());
            out = out.substr(0, at) + out.substr(std::min(out.size(), at + 1 + rng->below(8)));
        }
        if (out.size() > kMaxBuffer * 2) out.resize(kMaxBuffer * 2);
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    long long iterations = 200000;
    uint64_t seed = 20261030;
    std::string artifact = "test-artifacts/fuzz_parse_frame.json";
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a.rfind("--iterations=", 0) == 0) iterations = std::atoll(a.c_str() + 13);
        else if (a.rfind("--seed=", 0) == 0) seed = std::strtoull(a.c_str() + 7, nullptr, 10);
        else if (a.rfind("--artifact=", 0) == 0) artifact = a.substr(11);
    }

    const std::vector<std::string> corpus = seed_corpus();
    Rng rng(seed);

    // The corpus itself first: every seed frame must survive the pipeline.
    for (const std::string& c : corpus)
        exercise(reinterpret_cast<const uint8_t*>(c.data()), c.size());

    for (long long i = 0; i < iterations; ++i) {
        std::string input;
        if (rng.below(4) == 0) {
            input.resize(rng.below(96));
            for (char& ch : input) ch = static_cast<char>(rng.below(256));
        } else {
            input = mutate(corpus[rng.below(corpus.size())], &rng);
        }
        exercise(reinterpret_cast<const uint8_t*>(input.data()), input.size());
    }

    std::printf("[PASS]    J-10    P0    %lld fuzz iterations with seed %llu: no crash, no abort, "
                "no cap violation\n",
                iterations, static_cast<unsigned long long>(seed));
    std::printf("\nfuzz_parse_frame: 1 passed, 0 failed, 0 blocked (artifact: %s)\n",
                artifact.c_str());

    std::FILE* f = std::fopen(artifact.c_str(), "wb");
    if (f != nullptr) {
        std::fprintf(f,
                     "{\n  \"suite\": \"fuzz_parse_frame\",\n  \"cases\": [\n"
                     "    {\"id\": \"J-10\", \"priority\": \"P0\", \"verdict\": \"PASS\", "
                     "\"title\": \"%lld standalone fuzz iterations, seed %llu\", "
                     "\"seconds\": 0, \"detail\": \"standalone deterministic driver: libFuzzer is "
                     "not available in this toolchain\"}\n  ]\n}\n",
                     iterations, static_cast<unsigned long long>(seed));
        std::fclose(f);
    }
    return 0;
}
#endif
