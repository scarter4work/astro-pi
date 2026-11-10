// printf-style formatting for log and error texts copied verbatim from the Python/PJSR sources
// (Python's %-formatting follows C printf for the conversions used: %d, %.1f, %.2f, %s).
#pragma once

#include <cstdarg>
#include <cstdio>
#include <string>

namespace gdg {

#if defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
inline std::string strformat(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    const int n = std::vsnprintf(nullptr, 0, fmt, ap);
    va_end(ap);
    std::string out(n > 0 ? static_cast<size_t>(n) : 0, '\0');
    if (n > 0) std::vsnprintf(out.data(), out.size() + 1, fmt, ap2);
    va_end(ap2);
    return out;
}

}  // namespace gdg
