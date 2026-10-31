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

const char* verdict_name(Verdict v) {
    switch (v) {
        case Verdict::Pass: return "PASS";
        case Verdict::Fail: return "FAIL";
        case Verdict::NotRun: return "NOT_RUN";
        case Verdict::Blocked: return "BLOCKED";
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
            o.append("\n");
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
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
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
          << json_escape(r.priority) << "\", \"verdict\": \"" << verdict_name(r.verdict)
          << "\", \"title\": \"" << json_escape(r.title) << "\", \"seconds\": " << r.seconds
          << ", \"detail\": \"" << json_escape(r.detail) << "\"}";
    }
    f << "\n  ]\n}\n";
}

}  // namespace

int run_all(int argc, char** argv, const std::string& suite) {
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
            std::printf("%s\t%s\t%s\n", c.id.c_str(), c.priority.c_str(), c.title.c_str());
        return 0;
    }

    int passed = 0, failed = 0, blocked = 0;
    for (const Case& c : registry()) {
        if (!filter.empty() && c.id.find(filter) == std::string::npos) continue;
        Result r;
        r.id = c.id;
        r.title = c.title;
        r.priority = c.priority;

        if (c.fn == nullptr) {
            r.verdict = Verdict::Blocked;
            r.detail = c.blocked_reason;
            ++blocked;
            std::printf("[BLOCKED] %-7s %-5s %s -- %s\n", c.id.c_str(), c.priority.c_str(),
                        c.title.c_str(), c.blocked_reason.c_str());
            results().push_back(r);
            continue;
        }

        const auto t0 = std::chrono::steady_clock::now();
        try {
            c.fn();
            r.verdict = Verdict::Pass;
            ++passed;
        } catch (const AssertionFailure& e) {
            r.verdict = Verdict::Fail;
            r.detail = e.message;
            ++failed;
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

        if (r.verdict == Verdict::Pass)
            std::printf("[PASS]    %-7s %-5s %s\n", c.id.c_str(), c.priority.c_str(),
                        c.title.c_str());
        else
            std::printf("[FAIL]    %-7s %-5s %s\n          %s\n", c.id.c_str(),
                        c.priority.c_str(), c.title.c_str(), r.detail.c_str());
        results().push_back(r);
    }

    write_artifact(artifact, suite);
    std::printf("\n%s: %d passed, %d failed, %d blocked (artifact: %s)\n", suite.c_str(), passed,
                failed, blocked, artifact.c_str());
    return failed == 0 ? 0 : 1;
}

}  // namespace tt
