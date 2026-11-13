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

/// `s` as a strict JSON string literal (RFC 8259), quotes included. Escapes `"` and `\`, writes
/// \b \f \n \r \t for those control characters and \u00xx (lowercase hex) for the other bytes below
/// 0x20. Every other byte, DEL (0x7f) and UTF-8 sequences included, is copied unchanged. This
/// matches Python's json.dumps for printable ASCII and control characters only: json.dumps' default
/// ensure_ascii would also \u-escape non-ASCII text.
inline std::string json_quote(const std::string& s) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out = "\"";
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                out += "\\u00";
                out += kHex[c >> 4];
                out += kHex[c & 0xF];
            } else {
                out += ch;
            }
        }
    }
    out += '"';
    return out;
}

}  // namespace gdg
