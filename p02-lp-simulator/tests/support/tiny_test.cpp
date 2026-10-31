#include "tiny_test.hpp"

#include <cstdlib>
#include <cstring>
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

std::string_view to_string(Verdict v) {
    switch (v) {
        case Verdict::Pass: return "PASS";
        case Verdict::Fail: return "FAIL";
        case Verdict::NotRun: return "NOT_RUN";
        case Verdict::Blocked: return "BLOCKED";
        case Verdict::NotImplemented: return "NOT_IMPLEMENTED";
    }
    return "NOT_RUN";
}

namespace {

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': out.append("\\\""); break;
            case '\\': out.append("\\\\"); break;
            case '\n': out.append("\\n"); break;
            case '\r': out.append("\\r"); break;
            case '\t': out.append("\\t"); break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out.append(buf);
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

}  // namespace

int run_all(int argc, char** argv, const std::string& suite) {
    std::string filter;
    std::string artifact_dir = "test-artifacts";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--filter") == 0 && i + 1 < argc) filter = argv[++i];
        else if (std::strcmp(argv[i], "--artifacts") == 0 && i + 1 < argc)
            artifact_dir = argv[++i];
    }

    int failures = 0;
    for (const Case& c : registry()) {
        if (!filter.empty() && c.id.find(filter) == std::string::npos) continue;

        Result r;
        r.id = c.id;
        r.title = c.title;
        r.priority = c.priority;

        if (c.fn == nullptr) {
            r.verdict = c.not_implemented ? Verdict::NotImplemented : Verdict::Blocked;
            r.detail = c.blocked_reason;
            std::printf("[%-15s] %-8s %-6s %s\n", std::string(to_string(r.verdict)).c_str(),
                        c.id.c_str(), c.priority.c_str(), c.title.c_str());
            results().push_back(r);
            continue;
        }

        const auto t0 = std::chrono::steady_clock::now();
        try {
            c.fn();
            r.verdict = Verdict::Pass;
        } catch (const AssertionFailure& e) {
            r.verdict = Verdict::Fail;
            r.detail = e.message;
            ++failures;
        } catch (const std::exception& e) {
            r.verdict = Verdict::Fail;
            r.detail = std::string("unexpected exception: ") + e.what();
            ++failures;
        } catch (...) {
            r.verdict = Verdict::Fail;
            r.detail = "unexpected non-standard exception";
            ++failures;
        }
        const auto t1 = std::chrono::steady_clock::now();
        r.seconds = std::chrono::duration<double>(t1 - t0).count();

        std::printf("[%-15s] %-8s %-6s %s\n", std::string(to_string(r.verdict)).c_str(),
                    c.id.c_str(), c.priority.c_str(), c.title.c_str());
        if (r.verdict == Verdict::Fail) std::printf("                 %s\n", r.detail.c_str());
        results().push_back(r);
    }

    // Artifact. Design doc 18.4 requires machine-readable results with the
    // matrix id and priority, so the gate report cannot be assembled by hand.
    std::error_code ec;
    std::filesystem::create_directories(artifact_dir, ec);
    std::ofstream out(artifact_dir + "/" + suite + ".json", std::ios::binary);
    if (out) {
        out << "{\n  \"suite\": \"" << json_escape(suite) << "\",\n  \"cases\": [\n";
        for (std::size_t i = 0; i < results().size(); ++i) {
            const Result& r = results()[i];
            out << "    {\"id\": \"" << json_escape(r.id) << "\", \"priority\": \""
                << json_escape(r.priority) << "\", \"verdict\": \"" << to_string(r.verdict)
                << "\", \"title\": \"" << json_escape(r.title) << "\", \"seconds\": "
                << r.seconds << ", \"detail\": \"" << json_escape(r.detail) << "\"}";
            if (i + 1 < results().size()) out << ",";
            out << "\n";
        }
        out << "  ]\n}\n";
    }

    std::size_t pass = 0, fail = 0, blocked = 0, not_impl = 0;
    for (const Result& r : results()) {
        switch (r.verdict) {
            case Verdict::Pass: ++pass; break;
            case Verdict::Fail: ++fail; break;
            case Verdict::Blocked: ++blocked; break;
            case Verdict::NotImplemented: ++not_impl; break;
            case Verdict::NotRun: break;
        }
    }
    std::printf("\n%s: %zu passed, %zu failed, %zu blocked, %zu not implemented\n",
                suite.c_str(), pass, fail, blocked, not_impl);
    return failures == 0 ? 0 : 1;
}

}  // namespace tt
