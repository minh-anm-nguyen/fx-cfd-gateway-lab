// Throwaway directory for store tests. Each test gets its own directory so that
// a failing case leaves its database behind for inspection while never sharing
// state with another case.
#pragma once

#include <atomic>
#include <filesystem>
#include <string>

namespace tt {

class TempDir {
public:
    explicit TempDir(const std::string& label) {
        static std::atomic<unsigned> counter{0};
        const unsigned n = counter.fetch_add(1);
        path_ = std::filesystem::temp_directory_path() / ("jrnl-test" ) /
                (label + "-" + std::to_string(n));
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
        std::filesystem::create_directories(path_, ec);
    }

    std::string file(const std::string& name) const { return (path_ / name).string(); }
    std::string dir() const { return path_.string(); }
    std::string missing_dir() const { return (path_ / "no-such-volume").string(); }

private:
    std::filesystem::path path_;
};

}  // namespace tt
