#include "fix/ownership.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace fixse {

OwnershipLock::~OwnershipLock() { release(); }

bool OwnershipLock::acquire(const std::string& lock_file_path) {
    release();
    path_ = lock_file_path;
#ifdef _WIN32
    // dwShareMode 0: no other process may open the file at all, so a second
    // engine instance cannot acquire ownership while this one lives. The OS
    // releases the handle when the process dies, which is what lets a restart
    // after SIGKILL recover (G-22).
    HANDLE h = CreateFileA(path_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    handle_ = h;
#else
    int fd = ::open(path_.c_str(), O_RDWR | O_CREAT, 0600);
    if (fd < 0) return false;
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        return false;
    }
    handle_ = reinterpret_cast<void*>(static_cast<intptr_t>(fd));
#endif
    held_ = true;
    return true;
}

void OwnershipLock::release() {
    if (!held_) return;
#ifdef _WIN32
    CloseHandle(static_cast<HANDLE>(handle_));
#else
    const int fd = static_cast<int>(reinterpret_cast<intptr_t>(handle_));
    ::flock(fd, LOCK_UN);
    ::close(fd);
#endif
    handle_ = nullptr;
    held_ = false;
}

}  // namespace fixse
