#include "lp/ownership.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <cstdint>
#endif

namespace lpsim {

OwnershipLock::~OwnershipLock() { release(); }

bool OwnershipLock::acquire(const std::string& lock_file_path) {
    release();
    path_ = lock_file_path;
#ifdef _WIN32
    // dwShareMode 0: no other process may open the file at all, so a second
    // simulator cannot take ownership while this one lives. The handle is
    // released by the OS when the process dies, which is what lets a restart
    // after an abrupt exit recover without operator action.
    HANDLE h = CreateFileA(path_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    handle_ = h;
#else
    const int fd = ::open(path_.c_str(), O_RDWR | O_CREAT, 0600);
    if (fd < 0) return false;
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        return false;
    }
    handle_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(fd));
#endif
    held_ = true;
    return true;
}

void OwnershipLock::release() {
    if (!held_) return;
#ifdef _WIN32
    CloseHandle(static_cast<HANDLE>(handle_));
#else
    const int fd = static_cast<int>(reinterpret_cast<std::intptr_t>(handle_));
    ::flock(fd, LOCK_UN);
    ::close(fd);
#endif
    handle_ = nullptr;
    held_ = false;
}

}  // namespace lpsim
