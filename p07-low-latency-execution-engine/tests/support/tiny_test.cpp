#include "tiny_test.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>

namespace tt {

std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}

std::vector<Result>& results() {
    static std::vector<Result> r;
    return r;
}

int register_case(Case c) {
    registry().push_back(std::move(c));
    return static_cast<int>(registry().size());
}

namespace {

// Evidence recorded by the case currently running. Design doc 23 wants the seed
// and the final state hash in the artifact of every case, and the only place
// that knows them is the case body.
std::uint64_t g_current_seed = 0;
std::string g_current_state_hash;

const char* verdict_name(Verdict v) {
    switch (v) {
        case Verdict::Pass: return "PASS";
        case Verdict::Fail: return "FAIL";
        case Verdict::NotRun: return "NOT_RUN";
        case Verdict::Blocked: return "BLOCKED";
        case Verdict::Inconclusive: return "INCONCLUSIVE";
    }
    return "UNKNOWN";
}

std::string json_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == 92) {
            o.push_back(92);
            o.push_back(c);
        } else if (c == 10) {
            o.append("\\n");
        } else if (static_cast<unsigned char>(c) < 0x20) {
            o.push_back(' ');
        } else {
            o.push_back(c);
        }
    }
    return o;
}

void write_artifact(const std::string& path, const std::string& suite) {
    std::error_code ec;
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        std::fprintf(stderr, "warning: cannot write artifact %s\n", path.c_str());
        return;
    }
    f << "{\n  \"suite\": \"" << json_escape(suite) << "\",\n  \"cases\": [\n";
    bool first = true;
    for (const Result& r : results()) {
        if (!first) f << ",\n";
        first = false;
        f << "    {\"id\": \"" << json_escape(r.id) << "\", \"priority\": \""
          << json_escape(r.priority) << "\", \"oracles\": \"" << json_escape(r.oracles)
          << "\", \"verdict\": \"" << verdict_name(r.verdict) << "\", \"title\": \""
          << json_escape(r.title) << "\", \"seconds\": " << r.seconds << ", \"seed\": "
          << r.seed << ", \"state_hash\": \"" << json_escape(r.state_hash)
          << "\", \"detail\": \"" << json_escape(r.detail) << "\"}";
    }
    f << "\n  ]\n}\n";
}

}  // namespace

void note_seed(std::uint64_t seed) { g_current_seed = seed; }
void note_state_hash(std::string hex) { g_current_state_hash = std::move(hex); }

int run_all(int argc, char** argv, const std::string& suite) {
    // Unbuffered: the crash-window suites terminate the process on purpose
    // (fp::Action::AbruptExit uses _Exit, which flushes nothing), and a case
    // whose verdict was lost with the buffer would be indistinguishable from a
    // case that never ran.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string filter;
    std::string artifact = "test-artifacts/" + suite + ".json";
    bool list_only = false;
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a.rfind("--filter=", 0) == 0) filter = std::string(a.substr(9));
        else if (a.rfind("--artifact=", 0) == 0) artifact = std::string(a.substr(11));
        else if (a == "--list") list_only = true;
    }

    if (list_only) {
        for (const Case& c : registry())
            std::printf("%s\t%s\t%s\t%s\n", c.id.c_str(), c.priority.c_str(),
                        c.oracles.c_str(), c.title.c_str());
        return 0;
    }

    int passed = 0, failed = 0, blocked = 0, inconclusive = 0;
    for (const Case& c : registry()) {
        if (!filter.empty() && c.id.find(filter) == std::string::npos) continue;
        Result r;
        r.id = c.id;
        r.title = c.title;
        r.priority = c.priority;
        r.oracles = c.oracles;

        if (c.fn == nullptr) {
            r.verdict = Verdict::Blocked;
            r.detail = c.blocked_reason;
            ++blocked;
            std::printf("[BLOCKED] %-7s %-4s %s -- %s\n", c.id.c_str(), c.priority.c_str(),
                        c.title.c_str(), c.blocked_reason.c_str());
            results().push_back(r);
            continue;
        }

        g_current_seed = 0;
        g_current_state_hash.clear();
        const auto t0 = std::chrono::steady_clock::now();
        try {
            c.fn();
            r.verdict = Verdict::Pass;
            ++passed;
        } catch (const AssertionFailure& e) {
            r.verdict = Verdict::Fail;
            r.detail = e.message;
            ++failed;
        } catch (const Inconclusive& e) {
            r.verdict = Verdict::Inconclusive;
            r.detail = e.message;
            ++inconclusive;
        } catch (const std::exception& e) {
            r.verdict = Verdict::Fail;
            r.detail = std::string("unexpected exception: ") + e.what();
            ++failed;
        } catch (...) {
            r.verdict = Verdict::Fail;
            r.detail = "unexpected non-standard exception";
            ++failed;
        }
        r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        r.seed = g_current_seed;
        r.state_hash = g_current_state_hash;

        switch (r.verdict) {
            case Verdict::Pass:
                std::printf("[PASS]    %-7s %-4s %s\n", c.id.c_str(), c.priority.c_str(),
                            c.title.c_str());
                break;
            case Verdict::Inconclusive:
                std::printf("[INCONCL] %-7s %-4s %s -- %s\n", c.id.c_str(),
                            c.priority.c_str(), c.title.c_str(), r.detail.c_str());
                break;
            default:
                std::printf("[FAIL]    %-7s %-4s %s\n          %s\n", c.id.c_str(),
                            c.priority.c_str(), c.title.c_str(), r.detail.c_str());
                break;
        }
        results().push_back(r);
    }

    write_artifact(artifact, suite);
    std::printf("\n%s: %d passed, %d failed, %d blocked, %d inconclusive (artifact: %s)\n",
                suite.c_str(), passed, failed, blocked, inconclusive, artifact.c_str());
    return failed == 0 ? 0 : 1;
}

}  // namespace tt
