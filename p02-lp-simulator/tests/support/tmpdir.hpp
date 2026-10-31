// Per-test scratch directory.
//
// Scenario M-04 requires every test to own its database, configuration and
// business namespace, and the suite to survive being run in a shuffled order.
// Sharing one file would make a later test depend on an earlier one, which is
// the single most common way a suite like this starts passing for the wrong
// reason.
//
// Crash tests deliberately keep their directory: design doc 17 says the
// database must survive the kill so the restart can be inspected.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

namespace tt {

class TmpDir {
public:
    explicit TmpDir(const std::string& tag, bool keep = false) : keep_(keep) {
        static std::atomic<unsigned> counter{0};
        const unsigned n = counter.fetch_add(1);
        path_ = std::filesystem::temp_directory_path() /
                ("lpsim-" + tag + "-" + std::to_string(n) + "-" +
                 std::to_string(reinterpret_cast<std::uintptr_t>(this) & 0xffffff));
        std::error_code ec;
        std::filesystem::create_directories(path_, ec);
    }

    ~TmpDir() {
        if (keep_) return;
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    TmpDir(const TmpDir&) = delete;
    TmpDir& operator=(const TmpDir&) = delete;

    std::string file(const std::string& name) const { return (path_ / name).string(); }
    std::string db() const { return file("lp.db"); }
    const std::filesystem::path& path() const { return path_; }
    void keep() { keep_ = true; }

private:
    std::filesystem::path path_;
    bool keep_ = false;
};

}  // namespace tt
