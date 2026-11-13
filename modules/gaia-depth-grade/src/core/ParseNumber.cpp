#include "ParseNumber.h"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <locale.h>
#include <system_error>

namespace gdg {

namespace {

locale_t c_locale() {
    static const locale_t loc = newlocale(LC_ALL_MASK, "C", static_cast<locale_t>(nullptr));
    return loc;
}

template <class T, class Strto>
bool parse_exact(const std::string& text, T& out, Strto strto) {
    const char* b = text.data();
    const char* e = b + text.size();
    T v{};
    const auto r = std::from_chars(b, e, v);
    if (r.ec == std::errc()) {
        if (r.ptr != e) return false;
        out = v;
        return true;
    }
    if (r.ec != std::errc::result_out_of_range) return false;
    // The text is a well-formed number whose value this libstdc++ will not return: retry with strtod.
    const locale_t loc = c_locale();
    if (loc == static_cast<locale_t>(nullptr)) return false;
    char* end = nullptr;
    const T w = strto(text.c_str(), &end, loc);
    if (end != text.c_str() + text.size() || !std::isfinite(w) || w == T(0)) return false;
    out = w;
    return true;
}

}  // namespace

bool parse_double_exact(const std::string& text, double& out) {
    return parse_exact<double>(text, out, [](const char* s, char** e, locale_t l) { return strtod_l(s, e, l); });
}

bool parse_float_exact(const std::string& text, float& out) {
    return parse_exact<float>(text, out, [](const char* s, char** e, locale_t l) { return strtof_l(s, e, l); });
}

}  // namespace gdg
