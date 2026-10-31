// Scoped temporary directory for the durable-store tests.
//
// Each test gets its own directory so a crash test can leave a journal behind
// for inspection without colliding with the next case.
#pragma once

#include <atomic>
#include <filesystem>
#include <string>

namespace tests {

class TmpDir {
 public:
  explicit TmpDir(const std::string& label) {
    static std::atomic<unsigned> counter{0};
    const unsigned id = counter.fetch_add(1);
    path_ = std::filesystem::temp_directory_path() /
            ("p05-" + label + "-" + std::to_string(id));
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
    std::filesystem::create_directories(path_, ec);
  }

  ~TmpDir() {
    if (keep_) return;
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  TmpDir(const TmpDir&) = delete;
  TmpDir& operator=(const TmpDir&) = delete;

  std::string file(const std::string& name) const {
    return (path_ / name).string();
  }
  std::string dir() const { return path_.string(); }

  // Leaves the directory in place, for inspecting a failing crash case.
  void keep() { keep_ = true; }

 private:
  std::filesystem::path path_;
  bool keep_ = false;
};

}  // namespace tests
