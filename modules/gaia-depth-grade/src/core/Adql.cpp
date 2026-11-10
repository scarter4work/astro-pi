// Port of gaia_depth_grade/distances.py build_adql, with Python's repr(float).
#include "Adql.h"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace gdg {

std::string py_float_repr(double v) {
    if (!std::isfinite(v)) throw std::invalid_argument("py_float_repr: not a finite value");
    if (v == 0.0) return std::signbit(v) ? "-0.0" : "0.0";
    // Shortest round-trip digits (the same digit string as Python's dtoa mode 0), as d.ddde+X.
    char buf[64];
    const auto res = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::scientific);
    if (res.ec != std::errc()) throw std::runtime_error("py_float_repr: to_chars failed");
    const std::string sci(buf, res.ptr);
    const size_t epos = sci.find('e');
    const bool neg = sci[0] == '-';
    std::string digits;
    for (size_t i = neg ? 1 : 0; i < epos; ++i)
        if (sci[i] != '.') digits += sci[i];
    const int exp10 = std::atoi(sci.c_str() + epos + 1);
    const int decpt = exp10 + 1;  // value = 0.d1d2... * 10^decpt
    std::string out = neg ? "-" : "";
    if (decpt > -4 && decpt <= 16) {
        // fixed notation
        if (decpt <= 0) {
            out += "0.";
            out.append(static_cast<size_t>(-decpt), '0');
            out += digits;
        } else if (static_cast<size_t>(decpt) >= digits.size()) {
            out += digits;
            out.append(static_cast<size_t>(decpt) - digits.size(), '0');
            out += ".0";
        } else {
            out += digits.substr(0, static_cast<size_t>(decpt));
            out += '.';
            out += digits.substr(static_cast<size_t>(decpt));
        }
    } else {
        // exponent notation: d[.ddd]e+XX, at least two exponent digits
        out += digits[0];
        if (digits.size() > 1) {
            out += '.';
            out += digits.substr(1);
        }
        const int e = decpt - 1;
        char eb[16];
        std::snprintf(eb, sizeof(eb), "e%c%02d", e < 0 ? '-' : '+', std::abs(e));
        out += eb;
    }
    return out;
}

std::string build_adql(const Footprint& fp, long top, std::optional<double> mag_gt, std::optional<double> mag_lt) {
    std::string mag_clause;
    if (mag_gt) mag_clause = "AND g.phot_g_mean_mag > " + py_float_repr(*mag_gt) + " ";
    if (mag_lt) mag_clause += "AND g.phot_g_mean_mag < " + py_float_repr(*mag_lt) + " ";
    return "SELECT TOP " + std::to_string(top) + " " +
           "g.ra, g.dec, d.r_med_geo, d.r_lo_geo, d.r_hi_geo, g.source_id, g.phot_g_mean_mag, g.bp_rp "
           "FROM gaiadr3.gaia_source AS g "
           "JOIN external.gaiaedr3_distance AS d ON g.source_id = d.source_id "
           "WHERE 1 = CONTAINS(POINT('ICRS', g.ra, g.dec), "
           "CIRCLE('ICRS', " +
           py_float_repr(fp.center_ra) + ", " + py_float_repr(fp.center_dec) + ", " + py_float_repr(fp.radius_deg) +
           ")) "
           "AND d.r_med_geo IS NOT NULL "
           "AND g.phot_g_mean_mag IS NOT NULL " +
           mag_clause + "ORDER BY g.phot_g_mean_mag ASC";
}

}  // namespace gdg
