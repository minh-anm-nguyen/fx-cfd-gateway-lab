#include "fix/clock.hpp"

#include <cstdio>
#include <cstdlib>

namespace fixse {
namespace {

// Days from 1970-01-01 for a proleptic Gregorian date. Howard Hinnant's
// algorithm; used instead of timegm/_mkgmtime so that behaviour is identical on
// every platform and independent of the local time zone (design doc 10).
constexpr long long days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<long long>(era) * 146097LL + static_cast<long long>(doe) - 719468LL;
}

constexpr bool is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

constexpr unsigned days_in_month(int y, unsigned m) {
    constexpr unsigned k[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && is_leap(y)) return 29;
    return k[m - 1];
}

bool all_digits(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

bool to_int(std::string_view s, int* out) {
    if (!all_digits(s) || s.size() > 9) return false;
    int v = 0;
    for (char c : s) v = v * 10 + (c - '0');
    *out = v;
    return true;
}

// Convert a civil date-time to a system_clock time point without using any
// time-zone dependent facility.
WallTime from_civil(int y, unsigned mo, unsigned d, int h, int mi, int s, long long nanos) {
    const long long days = days_from_civil(y, mo, d);
    const long long secs = days * 86400LL + h * 3600LL + mi * 60LL + s;
    return WallTime{} + std::chrono::seconds(secs) + std::chrono::nanoseconds(nanos);
}

}  // namespace

std::string format_utc_timestamp(WallTime tp) {
    using namespace std::chrono;
    const auto total_ns = duration_cast<nanoseconds>(tp.time_since_epoch()).count();
    long long secs = total_ns / 1000000000LL;
    long long frac_ns = total_ns % 1000000000LL;
    if (frac_ns < 0) {
        frac_ns += 1000000000LL;
        secs -= 1;
    }
    long long days = secs / 86400;
    long long rem = secs % 86400;
    if (rem < 0) {
        rem += 86400;
        days -= 1;
    }
    // Inverse of days_from_civil.
    long long z = days + 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long long y = static_cast<long long>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp + (mp < 10 ? 3 : -9);
    const long long year = y + (m <= 2);

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04lld%02u%02u-%02lld:%02lld:%02lld.%03lld", year, m, d,
                  rem / 3600, (rem % 3600) / 60, rem % 60, frac_ns / 1000000LL);
    return std::string(buf);
}

bool parse_utc_timestamp(std::string_view s, WallTime* out) {
    // YYYYMMDD-HH:MM:SS[.sss|.ssssss|.sssssssss]
    if (s.size() != 17 && s.size() != 21 && s.size() != 24 && s.size() != 27) return false;
    if (s[8] != '-' || s[11] != ':' || s[14] != ':') return false;

    int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
    if (!to_int(s.substr(0, 4), &y) || !to_int(s.substr(4, 2), &mo) ||
        !to_int(s.substr(6, 2), &d) || !to_int(s.substr(9, 2), &h) ||
        !to_int(s.substr(12, 2), &mi) || !to_int(s.substr(15, 2), &sec))
        return false;

    // Calendar correctness: 2028-02-29 is valid, 2027-02-29 is not (H-05).
    if (y < 1970 || y > 9999) return false;
    if (mo < 1 || mo > 12) return false;
    if (d < 1 || static_cast<unsigned>(d) > days_in_month(y, static_cast<unsigned>(mo)))
        return false;
    if (h > 23 || mi > 59) return false;
    // FIX permits a leap second value of 60 in the seconds field.
    if (sec > 60) return false;

    long long nanos = 0;
    if (s.size() > 17) {
        if (s[17] != '.') return false;
        std::string_view frac = s.substr(18);
        if (frac.size() != 3 && frac.size() != 6 && frac.size() != 9) return false;
        if (!all_digits(frac)) return false;
        long long v = 0;
        for (char c : frac) v = v * 10 + (c - '0');
        if (frac.size() == 3) nanos = v * 1000000LL;
        else if (frac.size() == 6) nanos = v * 1000LL;
        else nanos = v;
    }

    if (out) *out = from_civil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d), h, mi, sec,
                               nanos);
    return true;
}

}  // namespace fixse
