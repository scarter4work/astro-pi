// TAP text handling for the online source (plan Task 13).
#include "TapText.h"

#include "Adql.h"
#include "Format.h"
#include "ParseNumber.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace gdg {

namespace {

std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> out;
    size_t p = 0;
    while (p <= text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        std::string l = text.substr(p, e - p);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        out.push_back(l);
        p = e + 1;
    }
    return out;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}

std::vector<std::string> split_csv(const std::string& line) {
    // The archive's numeric CSV; a quoted field is unquoted ("" -> ").
    std::vector<std::string> f;
    std::string cur;
    bool q = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (q) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    cur += '"';
                    ++i;
                } else {
                    q = false;
                }
            } else {
                cur += c;
            }
        } else if (c == '"') {
            q = true;
        } else if (c == ',') {
            f.push_back(trim(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    f.push_back(trim(cur));
    return f;
}

bool is_null(const std::string& s) {
    if (s.empty()) return true;
    std::string l = s;
    std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return l == "null" || l == "nan" || l == "--";
}

template <class T>
T parse_num(const std::string& s, long row, const char* col) {
    T v{};
    bool ok;
    const std::string digits = (!s.empty() && s[0] == '+') ? s.substr(1) : s;
    if constexpr (std::is_same_v<T, double>) {
        ok = parse_double_exact(digits, v);
    } else if constexpr (std::is_same_v<T, float>) {
        ok = parse_float_exact(digits, v);
    } else {
        const auto r = std::from_chars(digits.data(), digits.data() + digits.size(), v);
        ok = r.ec == std::errc() && r.ptr == digits.data() + digits.size();
    }
    if (!ok) throw std::runtime_error(strformat("Gaia result: row %ld, column %s: not a number: '%s'", row, col, s.c_str()));
    return v;
}

// DOUBLE column (ra, dec): parsed as double.
double as_double(const std::string& s, long row, const char* col) {
    return is_null(s) ? NAN : parse_num<double>(s, row, col);
}

// REAL column (distances, bp_rp): the archive prints the float32 value; parse as float, widen.
double as_real(const std::string& s, long row, const char* col) {
    return is_null(s) ? NAN : static_cast<double>(parse_num<float>(s, row, col));
}

// ---- SHA-1 (FIPS 180-4) ----------------------------------------------------------------------
inline uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

}  // namespace

std::string parse_uws_location(const std::string& body) {
    static const std::string prefix = std::string(kGaiaTapAsync) + "/";
    for (const std::string& raw : lines_of(body)) {
        const std::string l = trim(raw);
        if (l.size() < 9) continue;
        std::string head = l.substr(0, 9);
        std::transform(head.begin(), head.end(), head.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (head != "location:") continue;
        const std::string url = trim(l.substr(9));
        bool ok = url.size() > prefix.size() && url.compare(0, prefix.size(), prefix) == 0;
        if (ok)
            for (size_t i = prefix.size(); i < url.size(); ++i) {
                const char c = url[i];
                if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) ok = false;
            }
        if (!ok)
            throw std::runtime_error("Gaia TAP: refusing job location '" + url + "': not a job of " + kGaiaTapAsync);
        return url;
    }
    throw std::runtime_error("Gaia TAP: the job submission answer has no Location line");
}

long csv_row_count(const std::string& csv) {
    const std::vector<std::string> lines = lines_of(csv);
    long n = 0;
    bool header = true;
    for (const std::string& l : lines) {
        if (trim(l).empty()) continue;
        if (header) {
            header = false;
            continue;
        }
        ++n;
    }
    return n;
}

Catalog parse_tap_csv(const std::string& csv, const Log& log) {
    std::vector<std::string> lines = lines_of(csv);
    lines.erase(std::remove_if(lines.begin(), lines.end(), [](const std::string& l) { return trim(l).empty(); }),
                lines.end());
    std::map<std::string, size_t> col;
    if (!lines.empty()) {
        const std::vector<std::string> h = split_csv(lines[0]);
        for (size_t i = 0; i < h.size(); ++i) col.emplace(h[i], i);
    }
    // distances._REQUIRED, reported as Python's sorted(set) list repr
    static const char* kRequired[] = {"ra", "dec", "r_med_geo", "r_lo_geo", "r_hi_geo", "source_id"};
    std::set<std::string> missing;
    for (const char* r : kRequired)
        if (!col.count(r)) missing.insert(r);
    if (!missing.empty()) {
        std::string m = "[";
        for (const std::string& s : missing) m += (m.size() > 1 ? ", '" : "'") + s + "'";
        throw std::runtime_error("Gaia result missing columns: " + m + "]");
    }
    if (lines.size() < 2) throw std::runtime_error("Gaia query returned zero rows for this field");
    const bool has_bp = col.count("bp_rp") != 0;
    Catalog out;
    out.reserve(lines.size() - 1);
    for (size_t li = 1; li < lines.size(); ++li) {
        const std::vector<std::string> f = split_csv(lines[li]);
        const long row = static_cast<long>(li);
        auto field = [&](const char* name) -> const std::string& {
            const size_t i = col.at(name);
            if (i >= f.size()) throw std::runtime_error(strformat("Gaia result: row %ld is short", row));
            return f[i];
        };
        CatalogRow r;
        r.ra = as_double(field("ra"), row, "ra");
        r.dec = as_double(field("dec"), row, "dec");
        r.r_med = as_real(field("r_med_geo"), row, "r_med_geo");
        r.r_lo = as_real(field("r_lo_geo"), row, "r_lo_geo");
        r.r_hi = as_real(field("r_hi_geo"), row, "r_hi_geo");
        const std::string& sid = field("source_id");
        if (is_null(sid)) throw std::runtime_error(strformat("Gaia result: row %ld has no source_id", row));
        r.source_id = parse_num<long long>(sid, row, "source_id");
        r.bp_rp = has_bp ? as_real(field("bp_rp"), row, "bp_rp") : NAN;
        out.push_back(r);
    }
    (void)log;
    return out;
}

std::string sha1_hex(const std::string& bytes) {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    std::string m = bytes;
    const uint64_t bits = static_cast<uint64_t>(bytes.size()) * 8u;
    m += static_cast<char>(0x80);
    while (m.size() % 64 != 56) m += '\0';
    for (int i = 7; i >= 0; --i) m += static_cast<char>((bits >> (8 * i)) & 0xFF);
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(uint8_t(m[off + 4 * i])) << 24) | (uint32_t(uint8_t(m[off + 4 * i + 1])) << 16) |
                   (uint32_t(uint8_t(m[off + 4 * i + 2])) << 8) | uint32_t(uint8_t(m[off + 4 * i + 3]));
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    char out[41];
    for (int i = 0; i < 5; ++i) std::snprintf(out + 8 * i, 9, "%08x", h[i]);
    return std::string(out, 40);
}

std::string cache_key(const Footprint& fp, std::optional<double> mag_limit) {
    // f"{ra:.6f}_{dec:.6f}_{radius:.6f}_mag{mag_limit}"
    const std::string key = strformat("%.6f_%.6f_%.6f_mag", fp.center_ra, fp.center_dec, fp.radius_deg) +
                            (mag_limit ? py_float_repr(*mag_limit) : std::string("None"));
    return sha1_hex(key).substr(0, 16);
}

}  // namespace gdg
