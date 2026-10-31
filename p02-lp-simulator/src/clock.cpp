#include "lp/clock.hpp"

#include <cstdio>

namespace lpsim {

std::string_view to_string(ClockMode m) {
    switch (m) {
        case ClockMode::Virtual: return "virtual";
        case ClockMode::Wall: return "wall";
    }
    return "unknown";
}

namespace {

// Days since 1970-01-01 for a proleptic Gregorian civil date. Howard Hinnant's
// days_from_civil algorithm, used because std::chrono::year_month_day is not
// uniformly available on the toolchains this project targets.
std::int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);              // [0, 399]
    const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + d - 1; // [0, 365]
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;             // [0, 146096]
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

void civil_from_days(std::int64_t z, int* y, unsigned* m, unsigned* d) {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);           // [0, 146096]
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t yy = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);           // [0, 365]
    const unsigned mp = (5 * doy + 2) / 153;                                // [0, 11]
    const unsigned dd = doy - (153 * mp + 2) / 5 + 1;                       // [1, 31]
    const unsigned mm = mp + (mp < 10 ? 3 : -9);                            // [1, 12]
    *y = static_cast<int>(yy + (mm <= 2));
    *m = mm;
    *d = dd;
}

bool is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

unsigned days_in_month(int y, unsigned m) {
    static const unsigned kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m < 1 || m > 12) return 0;
    if (m == 2 && is_leap(y)) return 29;
    return kDays[m - 1];
}

bool all_digits(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

// Parse a fixed-width digit run. The caller has already checked the width.
int to_int(std::string_view s) {
    int v = 0;
    for (char c : s) v = v * 10 + (c - '0');
    return v;
}

}  // namespace

std::string format_utc_timestamp(WallTime tp) {
    using namespace std::chrono;
    const auto since = tp.time_since_epoch();
    const auto total_ms = duration_cast<milliseconds>(since).count();
    std::int64_t days = total_ms / 86400000;
    std::int64_t rem_ms = total_ms % 86400000;
    if (rem_ms < 0) {  // normalise for instants before the epoch
        rem_ms += 86400000;
        --days;
    }
    int y = 0;
    unsigned m = 0, d = 0;
    civil_from_days(days, &y, &m, &d);

    const int hh = static_cast<int>(rem_ms / 3600000);
    const int mi = static_cast<int>((rem_ms / 60000) % 60);
    const int ss = static_cast<int>((rem_ms / 1000) % 60);
    const int ms = static_cast<int>(rem_ms % 1000);

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d%02u%02u-%02d:%02d:%02d.%03d", y, m, d, hh, mi, ss,
                  ms);
    return std::string(buf);
}

bool parse_utc_timestamp(std::string_view s, WallTime* out) {
    // YYYYMMDD-HH:MM:SS with an optional .sss / .ssssss / .sssssssss suffix.
    if (s.size() < 17) return false;
    if (s[8] != '-' || s[11] != ':' || s[14] != ':') return false;
    if (!all_digits(s.substr(0, 8))) return false;
    if (!all_digits(s.substr(9, 2))) return false;
    if (!all_digits(s.substr(12, 2))) return false;
    if (!all_digits(s.substr(15, 2))) return false;

    std::int64_t nanos = 0;
    if (s.size() > 17) {
        if (s[17] != '.') return false;
        const std::string_view frac = s.substr(18);
        if (frac.size() != 3 && frac.size() != 6 && frac.size() != 9) return false;
        if (!all_digits(frac)) return false;
        std::int64_t scaled = 0;
        for (char c : frac) scaled = scaled * 10 + (c - '0');
        if (frac.size() == 3) nanos = scaled * 1000000;
        else if (frac.size() == 6) nanos = scaled * 1000;
        else nanos = scaled;
    }

    const int y = to_int(s.substr(0, 4));
    const unsigned mo = static_cast<unsigned>(to_int(s.substr(4, 2)));
    const unsigned da = static_cast<unsigned>(to_int(s.substr(6, 2)));
    const int hh = to_int(s.substr(9, 2));
    const int mi = to_int(s.substr(12, 2));
    const int ss = to_int(s.substr(15, 2));

    if (y < 1900 || y > 9999) return false;
    if (mo < 1 || mo > 12) return false;
    if (da < 1 || da > days_in_month(y, mo)) return false;
    if (hh > 23 || mi > 59) return false;
    // FIX permits a leap second as 60; it is accepted and normalised, not
    // silently truncated, so that the value round-trips predictably.
    if (ss > 60) return false;

    const std::int64_t days = days_from_civil(y, mo, da);
    const std::int64_t secs = days * 86400 + hh * 3600 + mi * 60 + ss;
    *out = WallTime{} + std::chrono::seconds(secs) + std::chrono::nanoseconds(nanos);
    return true;
}

std::string business_day_utc(WallTime tp) {
    using namespace std::chrono;
    const auto total_ms = duration_cast<milliseconds>(tp.time_since_epoch()).count();
    std::int64_t days = total_ms / 86400000;
    if (total_ms % 86400000 < 0) --days;
    int y = 0;
    unsigned m = 0, d = 0;
    civil_from_days(days, &y, &m, &d);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d%02u%02u", y, m, d);
    return std::string(buf);
}

}  // namespace lpsim
