#include "SerTime.h"

#include <cstdio>

namespace planetary::ser {

namespace {
constexpr int64_t kTicksPerSecond = 10000000;
constexpr int64_t kTicksPerDay = 86400 * kTicksPerSecond;
constexpr int64_t kDaysFromYear1ToUnixEpoch = 719162;  // 0001-01-01 -> 1970-01-01
constexpr int64_t kTickMask = 0x3FFFFFFFFFFFFFFFLL;

// Howard Hinnant's civil_from_days (days since 1970-01-01, proleptic Gregorian).
void CivilFromDays(int64_t z, int64_t& y, unsigned& m, unsigned& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) ++y;
}
}  // namespace

std::optional<std::string> TicksToIsoUtc(int64_t ticks) {
    const int64_t t = ticks & kTickMask;
    if (t <= 0) return std::nullopt;

    const int64_t days = t / kTicksPerDay;
    const int64_t rem = t % kTicksPerDay;
    int64_t y;
    unsigned m, d;
    CivilFromDays(days - kDaysFromYear1ToUnixEpoch, y, m, d);

    const int64_t secs = rem / kTicksPerSecond;
    const int ms = static_cast<int>((rem % kTicksPerSecond) / 10000);
    char buf[48];
    std::snprintf(buf, sizeof buf, "%04lld-%02u-%02uT%02d:%02d:%02d.%03d",
                  static_cast<long long>(y), m, d, static_cast<int>(secs / 3600),
                  static_cast<int>(secs / 60 % 60), static_cast<int>(secs % 60), ms);
    return std::string(buf);
}

}  // namespace planetary::ser
