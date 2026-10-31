// Exclusive single-writer ownership of a business store.
//
// Reference: design doc 10.4 step 1 and scenario J-12. The LP order actor is a
// single writer by design (invariant I-02); two processes pointed at the same
// database file would each believe that, and the result would be interleaved
// fills nobody can reconcile.
//
// The lock is a separate file next to the database rather than a row inside it,
// for one reason: the operating system releases it when the process dies, so a
// restart after SIGKILL can take ownership immediately instead of waiting for a
// lease to expire or needing an operator to clear a stale flag.
#pragma once

#include <string>

namespace lpsim {

class OwnershipLock {
public:
    OwnershipLock() = default;
    ~OwnershipLock();

    OwnershipLock(const OwnershipLock&) = delete;
    OwnershipLock& operator=(const OwnershipLock&) = delete;

    // Returns false when another live process holds the lock.
    bool acquire(const std::string& lock_file_path);
    void release();

    bool held() const { return held_; }
    const std::string& path() const { return path_; }

private:
    std::string path_;
    void* handle_ = nullptr;
    bool held_ = false;
};

}  // namespace lpsim
