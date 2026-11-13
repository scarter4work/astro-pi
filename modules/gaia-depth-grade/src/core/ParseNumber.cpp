#include "ParseNumber.h"

#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <locale.h>
#include <stdexcept>
#include <system_error>

namespace gdg {

namespace {

/// The "C" locale, created once (thread-safe). A failed creation is not cached silently: the static
/// stays null and every use throws.
locale_t c_locale() {
    static const locale_t loc = newlocale(LC_ALL_MASK, "C", static_cast<locale_t>(nullptr));
    if (loc == static_cast<locale_t>(nullptr)) throw std::runtime_error("cannot create the C locale for number parsing");
    return loc;
}

template <class T, class Strto>
bool parse_exact(std::string_view text, T& out, Strto strto) {
    const char* b = text.data();
    const char* e = b + text.size();
    T v{};
    const auto r = std::from_chars(b, e, v);
    if (r.ec == std::errc()) {
        if (r.ptr != e) return false;
        out = v;
        return true;
    }
    if (r.ec != std::errc::result_out_of_range || r.ptr != e) return false;
    // A well-formed number spanning the whole text whose value this implementation will not return:
    // retry with strtod, which must consume exactly the span from_chars validated.
    const locale_t loc = c_locale();
    const std::string z(text);  // strtod needs a terminating NUL
    char* end = nullptr;
    const int saved_errno = errno;  // strtod sets ERANGE for subnormals: not the caller's to see
    const T w = strto(z.c_str(), &end, loc);
    errno = saved_errno;
    if (end != z.c_str() + (r.ptr - b) || !std::isfinite(w) || w == T(0)) return false;
    out = w;
    return true;
}

}  // namespace

bool parse_double_exact(std::string_view text, double& out) {
    return parse_exact<double>(text, out, [](const char* s, char** e, locale_t l) { return strtod_l(s, e, l); });
}

bool parse_float_exact(std::string_view text, float& out) {
    return parse_exact<float>(text, out, [](const char* s, char** e, locale_t l) { return strtof_l(s, e, l); });
}

}  // namespace gdg
