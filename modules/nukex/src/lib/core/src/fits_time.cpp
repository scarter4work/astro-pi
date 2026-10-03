#include "nukex/core/fits_time.hpp"

#include <cstdio>
#include <cstdlib>

namespace nukex {

double parse_fits_datetime(const std::string& s) {
    if (s.size() < 19) return 0.0;
    int Y = 0, M = 0, D = 0, h = 0, m = 0;
    double sec = 0.0;
    char sep = 0;
    if (std::sscanf(s.c_str(), "%4d-%2d-%2d%c%2d:%2d:%lf", &Y, &M, &D, &sep, &h, &m, &sec) != 7) return 0.0;
    if (sep != 'T' && sep != ' ') return 0.0;
    if (M < 1 || M > 12 || D < 1 || D > 31 || h > 23 || m > 59 || sec < 0.0 || sec >= 61.0) return 0.0;
    // Days from civil (Howard Hinnant's algorithm), so no timezone machinery.
    const int y = Y - (M <= 2 ? 1 : 0);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153u * static_cast<unsigned>(M + (M > 2 ? -3 : 9)) + 2u) / 5u + static_cast<unsigned>(D) - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    const long long days = static_cast<long long>(era) * 146097LL + static_cast<long long>(doe) - 719468LL;
    return static_cast<double>(days) * 86400.0 + h * 3600.0 + m * 60.0 + sec;
}

} // namespace nukex
