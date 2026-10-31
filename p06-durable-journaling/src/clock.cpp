#include "jrnl/clock.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>

namespace jrnl {

std::int64_t SystemClock::now_ms() const {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::int64_t SystemClock::steady_ms() const {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string IClock::now_iso8601() const {
    const std::int64_t ms = now_ms();
    const std::time_t secs = static_cast<std::time_t>(ms / 1000);
    const int millis = static_cast<int>(ms % 1000);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &secs);
#else
    gmtime_r(&secs, &tm);
#endif
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, millis);
    return std::string(buf);
}

}  // namespace jrnl
