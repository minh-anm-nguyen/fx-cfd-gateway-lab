#include "router/clock.hpp"

#include <cstdio>

namespace router {
namespace {

// Civil-from-days, the standard Howard Hinnant algorithm. Written out rather
// than pulled from a library so the timestamp format is not platform dependent.
void civil_from_days(std::int64_t z, int& y, unsigned& m, unsigned& d) {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t yy = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = static_cast<int>(yy + (m <= 2 ? 1 : 0));
}

std::int64_t days_from_civil(int y, unsigned m, unsigned d) {
    const std::int64_t yy = y - (m <= 2 ? 1 : 0);
    const std::int64_t era = (yy >= 0 ? yy : yy - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(yy - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

bool digits(const std::string& s, std::size_t pos, std::size_t n, int& out) {
    out = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (pos + i >= s.size()) return false;
        const char c = s[pos + i];
        if (c < '0' || c > '9') return false;
        out = out * 10 + (c - '0');
    }
    return true;
}

}  // namespace

std::string format_fix_utc(WallMicros wall) {
    const std::int64_t total_ms = wall / 1000;
    std::int64_t days = total_ms / 86400000;
    std::int64_t ms_of_day = total_ms % 86400000;
    if (ms_of_day < 0) {
        ms_of_day += 86400000;
        --days;
    }
    int y = 0;
    unsigned m = 0, d = 0;
    civil_from_days(days, y, m, d);
    const int hh = static_cast<int>(ms_of_day / 3600000);
    const int mm = static_cast<int>((ms_of_day / 60000) % 60);
    const int ss = static_cast<int>((ms_of_day / 1000) % 60);
    const int ms = static_cast<int>(ms_of_day % 1000);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d%02u%02u-%02d:%02d:%02d.%03d", y, m, d, hh, mm, ss, ms);
    return std::string(buf);
}

bool parse_fix_utc(const std::string& text, WallMicros& out) {
    // YYYYMMDD-HH:MM:SS.mmm is exactly 21 characters.
    if (text.size() != 21) return false;
    if (text[8] != '-' || text[11] != ':' || text[14] != ':' || text[17] != '.') return false;
    int y = 0, mo = 0, d = 0, hh = 0, mi = 0, ss = 0, ms = 0;
    if (!digits(text, 0, 4, y) || !digits(text, 4, 2, mo) || !digits(text, 6, 2, d)) return false;
    if (!digits(text, 9, 2, hh) || !digits(text, 12, 2, mi) || !digits(text, 15, 2, ss))
        return false;
    if (!digits(text, 18, 3, ms)) return false;
    if (mo < 1 || mo > 12 || d < 1 || d > 31) return false;
    // A leap second (60) is refused: the lab profile has no use for it and
    // accepting it would make the round trip in OR-D-01 lossy.
    if (hh > 23 || mi > 59 || ss > 59) return false;
    const std::int64_t days = days_from_civil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d));
    const std::int64_t ms_of_day = ((hh * 60LL + mi) * 60LL + ss) * 1000LL + ms;
    out = (days * 86400000LL + ms_of_day) * 1000LL;
    return true;
}

}  // namespace router
