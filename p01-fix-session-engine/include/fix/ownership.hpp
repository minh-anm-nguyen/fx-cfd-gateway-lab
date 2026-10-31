// Exclusive session ownership.
//
// Invariant I-01: at most one writer may mutate a given session identity. A
// second engine process sharing the same store must fail to start rather than
// produce a second stream of sequence numbers (scenarios C-18, G-21, K-14,
// playbook PB-09).
//
// This is an advisory OS file lock, which covers the single-host case the design
// doc scopes for v1. The doc is explicit that a local file lock is NOT
// sufficient for multi-host HA: that needs external fencing or a lease, and is
// out of scope here. The class name and this comment keep that limit visible.
#pragma once

#include <string>

namespace fixse {

class OwnershipLock {
public:
    OwnershipLock() = default;
    ~OwnershipLock();
    OwnershipLock(const OwnershipLock&) = delete;
    OwnershipLock& operator=(const OwnershipLock&) = delete;

    // Try to take the lock without blocking. Returns false when another live
    // process holds it; the caller must then refuse to start.
    bool acquire(const std::string& lock_file_path);
    void release();
    bool held() const { return held_; }
    const std::string& path() const { return path_; }

private:
    bool held_ = false;
    std::string path_;
    void* handle_ = nullptr;  // HANDLE on Windows, int fd cast on POSIX
};

}  // namespace fixse
