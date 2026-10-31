#include "tiny_test.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>

namespace tt {
namespace {

// The group a case belongs to is derived from its identifier rather than from a
// global the test files have to set: static initialisation order across
// translation units is unspecified, so anything stateful here would be fragile.
std::string group_from_id(const std::string& id) {
    if (id.rfind("OR-", 0) == 0 && id.size() > 4) return std::string("matrix-") + id[3];
    if (id.rfind("PB-", 0) == 0) return "playbook";
    if (id.rfind("CW-", 0) == 0) return "crash-window";
    if (id.rfind("U-", 0) == 0) return "unit";
    if (id.rfind("PROP-", 0) == 0) return "property";
    if (id.rfind("LOAD-", 0) == 0) return "load";
    return "other";
}

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
        if (c == '"' || c == '\\') {
            o.push_back('\\');
            o.push_back(c);
        } else if (c == '\n' || c == '\r' || c == '\t') {
            o.push_back(' ');
        } else if (static_cast<unsigned char>(c) < 0x20 ||
                   static_cast<unsigned char>(c) > 0x7e) {
            o.push_back('?');
        } else {
            o.push_back(c);
        }
    }
    return o;
}

void write_artifact(const std::string& path, const std::string& suite, const std::string& seed) {
    std::error_code ec;
    const std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        std::fprintf(stderr, "warning: cannot write artifact %s\n", path.c_str());
        return;
    }
    std::map<std::string, int> by_verdict;
    std::map<std::string, int> p0_by_verdict;
    for (const Result& r : results()) {
        ++by_verdict[verdict_name(r.verdict)];
        if (r.priority == "P0") ++p0_by_verdict[verdict_name(r.verdict)];
    }
    f << "{\n";
    f << "  \"suite\": \"" << json_escape(suite) << "\",\n";
    f << "  \"seed\": \"" << json_escape(seed) << "\",\n";
    f << "  \"compiler\": \"" <<
#if defined(__clang__)
        "clang " << __clang_major__ << "." << __clang_minor__
#elif defined(__GNUC__)
        "gcc " << __GNUC__ << "." << __GNUC_MINOR__
#else
        "unknown"
#endif
      << "\",\n";
    f << "  \"totals\": {";
    bool first_total = true;
    for (const auto& [k, n] : by_verdict) {
        if (!first_total) f << ", ";
        first_total = false;
        f << "\"" << k << "\": " << n;
    }
    f << "},\n";
    f << "  \"p0_totals\": {";
    first_total = true;
    for (const auto& [k, n] : p0_by_verdict) {
        if (!first_total) f << ", ";
        first_total = false;
        f << "\"" << k << "\": " << n;
    }
    f << "},\n";
    f << "  \"cases\": [\n";
    bool first = true;
    for (const Result& r : results()) {
        if (!first) f << ",\n";
        first = false;
        f << "    {\"id\": \"" << json_escape(r.id) << "\", \"group\": \""
          << json_escape(r.group) << "\", \"priority\": \"" << json_escape(r.priority)
          << "\", \"verdict\": \"" << verdict_name(r.verdict) << "\", \"title\": \""
          << json_escape(r.title) << "\", \"seconds\": " << r.seconds << ", \"detail\": \""
          << json_escape(r.detail) << "\"}";
    }
    f << "\n  ]\n}\n";
}

}  // namespace

std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}

std::vector<Result>& results() {
    static std::vector<Result> r;
    return r;
}

int register_case(Case c) {
    c.group = group_from_id(c.id);
    registry().push_back(std::move(c));
    return static_cast<int>(registry().size());
}

int run_all(int argc, char** argv, const std::string& suite) {
    std::string filter;
    std::string priority_filter;
    std::string artifact = "test-artifacts/" + suite + ".json";
    std::string seed = "42";
    bool list_only = false;
    bool quiet = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a.rfind("--filter=", 0) == 0) filter = std::string(a.substr(9));
        else if (a.rfind("--artifact=", 0) == 0) artifact = std::string(a.substr(11));
        else if (a.rfind("--seed=", 0) == 0) seed = std::string(a.substr(7));
        else if (a.rfind("--priority=", 0) == 0) priority_filter = std::string(a.substr(11));
        else if (a == "--list") list_only = true;
        else if (a == "--quiet") quiet = true;
    }

    if (list_only) {
        for (const Case& c : registry())
            std::printf("%s\t%s\t%s\t%s\n", c.id.c_str(), c.priority.c_str(), c.group.c_str(),
                        c.title.c_str());
        return 0;
    }

    int passed = 0, failed = 0, blocked = 0;
    for (const Case& c : registry()) {
        if (!filter.empty() && c.id.find(filter) == std::string::npos &&
            c.group.find(filter) == std::string::npos)
            continue;
        if (!priority_filter.empty() && c.priority != priority_filter) continue;

        Result r;
        r.id = c.id;
        r.title = c.title;
        r.priority = c.priority;
        r.group = c.group;

        if (c.fn == nullptr) {
            r.verdict = Verdict::Blocked;
            r.detail = c.blocked_reason;
            ++blocked;
            std::printf("[BLOCKED] %-10s %-3s %s -- %s\n", c.id.c_str(), c.priority.c_str(),
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

        if (r.verdict == Verdict::Pass) {
            if (!quiet)
                std::printf("[PASS]    %-10s %-3s %s\n", c.id.c_str(), c.priority.c_str(),
                            c.title.c_str());
        } else {
            std::printf("[FAIL]    %-10s %-3s %s\n          %s\n", c.id.c_str(),
                        c.priority.c_str(), c.title.c_str(), r.detail.c_str());
        }
        results().push_back(r);
    }

    write_artifact(artifact, suite, seed);
    std::printf("\n%s: %d passed, %d failed, %d blocked (artifact: %s)\n", suite.c_str(), passed,
                failed, blocked, artifact.c_str());
    return failed == 0 ? 0 : 1;
}

}  // namespace tt
