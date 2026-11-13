#include "Cavity.h"

#include "Channels.h"
#include "core/Format.h"
#include "core/Numerics.h"
#include "core/Parallel.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <set>
#include <stdexcept>

namespace gdg::recon {

namespace {

void require_mono(const Image& a, const char* what) {
    if (a.channels != 1) throw std::invalid_argument(std::string(what) + ": expected a (ny, nx) plane");
}

/// `array ** p` for a Python-float exponent: numpy's fast_scalar_power routes 1, -1, 0, 0.5 and 2
/// to positive / reciprocal / ones_like / sqrt / square; every other exponent goes to np.power.
double np_scalar_power(double x, double p) {
    if (p == 2.0) return x * x;
    if (p == 1.0) return x;
    if (p == 0.0) return 1.0;
    if (p == 0.5) return std::sqrt(x);
    if (p == -1.0) return 1.0 / x;
    return std::pow(x, p);
}

}  // namespace

Image geometry_prior(int ny, int nx, std::pair<double, double> apex_xy, const Mask* footprint_mask, double power) {
    if (ny < 0 || nx < 0) throw std::invalid_argument("geometry_prior: negative shape");
    const double ax = apex_xy.first, ay = apex_xy.second;
    if (footprint_mask && (footprint_mask->height != ny || footprint_mask->width != nx))
        throw std::invalid_argument("footprint_mask shape (" + std::to_string(footprint_mask->height) + ", " +
                                    std::to_string(footprint_mask->width) + ") != (" + std::to_string(ny) + ", " +
                                    std::to_string(nx) + ")");
    const Mask all(nx, ny, true);
    const Mask& mask = footprint_mask ? *footprint_mask : all;

    Image r(nx, ny);
    for (int y = 0; y < ny; ++y)
        for (int x = 0; x < nx; ++x) r.at(x, y) = std::hypot(static_cast<double>(x) - ax, static_cast<double>(y) - ay);

    // R = r[mask].max(), or r.max() when the mask is empty; 1 when R <= 0.
    const bool any = mask.any();
    double R = -INFINITY;
    for (size_t i = 0; i < r.data.size(); ++i)
        if (!any || mask.data[i]) R = std::max(R, r.data[i]);
    if (r.data.empty()) R = 0.0;  // numpy would raise on an empty max; a 0-size image has no pixels
    if (R <= 0.0) R = 1.0;

    Image z(nx, ny);
    parallel_for_if(static_cast<long long>(nx) * ny * 8, ny, [&](int y) {
        for (int x = 0; x < nx; ++x) {
            const size_t i = z.index(x, y);
            const double rn = clip(r.data[i] / R, 0.0, 1.0);
            const double v = clip(1.0 - np_scalar_power(rn, power), 0.0, 1.0);
            z.data[i] = mask.data[i] ? f32(v) : 0.0;
        }
    });
    return z;
}

Image ionization_depth(const IonizationMap& ion_map, double gamma) {
    Image z = ion_map.oiii_fraction;
    for (double& v : z.data) {
        double f = clip(nan_to_num(v), 0.0, 1.0);
        if (gamma != 1.0) f = np_scalar_power(f, gamma);
        v = f32(clip(1.0 - f, 0.0, 1.0));
    }
    return z;
}

Image emission_measure_relief(const Image& lum, double sigma_small, double sigma_large, double gain) {
    require_mono(lum, "emission_measure_relief");
    Image L = lum;
    for (double& v : L.data) v = std::max(nan_to_num(v), 0.0);
    const Image small = gaussian_filter(L, sigma_small);
    const Image large = gaussian_filter(L, sigma_large);
    Image band(L.width, L.height);
    std::vector<double> mag(band.data.size());
    for (size_t i = 0; i < band.data.size(); ++i) {
        band.data[i] = small.data[i] - large.data[i];
        mag[i] = std::fabs(band.data[i]);
    }
    const double scale = mag.empty() ? 0.0 : percentile(std::move(mag), EM_NORM_PCT);
    Image z(L.width, L.height);
    if (scale <= EM_EPS) return z;
    const double neg_gain = -gain;
    for (size_t i = 0; i < z.data.size(); ++i) z.data[i] = f32(clip(neg_gain * (band.data[i] / scale), -gain, gain));
    return z;
}

Mask footprint_from_lum(const Image& lum, double frac) {
    require_mono(lum, "footprint_from_lum");
    const std::vector<double> p = percentiles(lum.data, {FOOTPRINT_SKY_PCT, FOOTPRINT_PEAK_PCT});
    const double floor = p[0], peak = p[1];
    const double rng = std::max(peak - floor, RECON_EPS);
    const double thr = floor + frac * rng;
    Mask m(lum.width, lum.height);
    for (size_t i = 0; i < lum.data.size(); ++i) m.data[i] = lum.data[i] > thr ? 1 : 0;
    if (!m.any()) m = Mask(lum.width, lum.height, true);
    return m;
}

Image linear_emissivity(const ChannelSet& channels) {
    const size_t n = channels.ha.data.size();
    // clip(nan_to_num(float64(.)), 0, None); sii None -> zeros. np.stack([ha, oiii, sii]) pooled.
    std::vector<double> stacked(3 * n);
    for (size_t i = 0; i < n; ++i) {
        stacked[i] = std::max(nan_to_num(channels.ha.data[i]), 0.0);
        stacked[n + i] = std::max(nan_to_num(channels.oiii.data[i]), 0.0);
        stacked[2 * n + i] = channels.sii ? std::max(nan_to_num(channels.sii->data[i]), 0.0) : 0.0;
    }
    double peak = stacked.empty() ? 0.0 : percentile(stacked, EMISSIVITY_PEAK_PCT);
    if (peak <= RECON_EPS) peak = 1.0;
    Image em(channels.ha.width, channels.ha.height, 3);
    for (size_t i = 0; i < n; ++i) {
        const double ha = stacked[i], oiii = stacked[n + i], sii = stacked[2 * n + i];
        em.data[3 * i] = f32(std::max((ha + sii) / peak, 0.0));     // Halpha + [S II]
        em.data[3 * i + 1] = f32(std::max(0.5 * oiii / peak, 0.0));  // half-weight green -> teal
        em.data[3 * i + 2] = f32(std::max(oiii / peak, 0.0));        // [O III]
    }
    return em;
}

std::string capture_mode(const ChannelSet& channels) {
    const double c = channels.ratio_confidence;
    if (channels.sii && std::fabs(c - CONF_SHO) < 1e-6) return "sho";
    if (std::fabs(c - CONF_DUOBAND) < 1e-6) return "duoband";
    if (std::fabs(c - CONF_OSC) < 1e-6 || !channels.sii) return "osc";
    if (c == 0.0) return "nonlinear";
    return "unknown";
}

CavityModel reconstruct_cavity(const ChannelSet& channels, const IonizationMap& ionization, const Ionizer& ionizer,
                               const Image& lum, const Image& dust, std::optional<double> pc_per_px,
                               const ReconConfig& config) {
    validate_shapes({&channels.ha, &channels.oiii, channels.sii ? &*channels.sii : nullptr, &ionization.oiii_fraction,
                     &lum, &dust});
    require_mono(lum, "reconstruct_cavity lum");
    require_mono(dust, "reconstruct_cavity dust");

    Image L = lum;
    for (double& v : L.data) v = std::max(nan_to_num(v), 0.0);
    const int ny = L.height, nx = L.width;

    const Mask foot = footprint_from_lum(L, config.footprint_frac);
    const std::pair<double, double> apex{ionizer.x, ionizer.y};

    const Image z_geo = geometry_prior(ny, nx, apex, &foot, config.geometry_power);
    const Image z_ion = ionization_depth(ionization, config.ionization_gamma);

    const double c = clip(ionization.confidence, 0.0, 1.0);
    const double w_geo = std::max(config.w_geometry, 0.0);
    const double w_ion = std::max(config.w_ionization, 0.0) * c;
    const double denom = w_geo + w_ion;
    const bool degenerate = denom <= RECON_EPS;  // fall back to the geometric bowl

    const Image relief = emission_measure_relief(L, EM_SIGMA_SMALL_PX, EM_SIGMA_LARGE_PX, 1.0);
    const double em_w = config.emission_sign * config.w_emission;
    const double wd = clip(config.w_dust, 0.0, 1.0);

    Image z(nx, ny), d_out(nx, ny);
    parallel_for_if(static_cast<long long>(nx) * ny * 8, ny, [&](int y) {
        for (int x = 0; x < nx; ++x) {
            const size_t i = z.index(x, y);
            double v = degenerate ? z_geo.data[i] : (w_geo * z_geo.data[i] + w_ion * z_ion.data[i]) / denom;
            v = v + em_w * relief.data[i];
            const double d = clip(nan_to_num(dust.data[i]), 0.0, 1.0);
            v = v * (1.0 - wd * d);
            v = foot.data[i] ? v : 0.0;
            z.data[i] = f32(clip(v, 0.0, 1.0));
            d_out.data[i] = f32(d);
        }
    });

    CavityMeta meta;
    meta.w_geometry = w_geo;
    meta.w_ionization_max = config.w_ionization;
    meta.w_ionization_effective = w_ion;
    meta.w_emission = config.w_emission;
    meta.emission_sign = config.emission_sign;
    meta.w_dust = config.w_dust;
    meta.ionization_confidence = c;
    meta.ratio_confidence = channels.ratio_confidence;
    meta.is_linear = channels.is_linear;
    meta.sii_present = channels.sii.has_value();
    meta.capture_mode = capture_mode(channels);
    meta.ionizer_source = ionizer.source;
    meta.footprint_frac = config.footprint_frac;
    meta.footprint_px = static_cast<long long>(foot.count());

    return CavityModel(std::move(z), linear_emissivity(channels), std::move(d_out), pc_per_px, apex, std::move(meta));
}

namespace {

/// Python's repr(float) (json.dumps writes floats with it): the shortest round-trip digits, in
/// exponent form when the decimal exponent is < -4 or >= 16, else fixed with at least one
/// fractional digit. json.dumps writes NaN / Infinity / -Infinity for the non-finite.
std::string py_float_repr(double v) {
    if (std::isnan(v)) return "NaN";
    if (std::isinf(v)) return v > 0 ? "Infinity" : "-Infinity";
    char buf[64];
    const auto res = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::scientific);
    std::string s(buf, res.ptr);  // [-]d[.ddd]e[+-]XX
    std::string sign;
    if (s[0] == '-') {
        sign = "-";
        s.erase(0, 1);
    }
    const size_t epos = s.find('e');
    const int exp10 = std::stoi(s.substr(epos + 1));
    std::string digits = s.substr(0, epos);
    digits.erase(std::remove(digits.begin(), digits.end(), '.'), digits.end());
    if (exp10 < -4 || exp10 >= 16) {
        std::string mant = digits.substr(0, 1);
        if (digits.size() > 1) mant += "." + digits.substr(1);
        const int a = std::abs(exp10);
        return sign + mant + "e" + (exp10 < 0 ? "-" : "+") + (a < 10 ? "0" : "") + std::to_string(a);
    }
    std::string out;
    if (exp10 < 0) {
        out = "0." + std::string(static_cast<size_t>(-exp10 - 1), '0') + digits;
    } else {
        const size_t int_len = static_cast<size_t>(exp10) + 1;
        if (digits.size() <= int_len) {
            out = digits + std::string(int_len - digits.size(), '0') + ".0";
        } else {
            out = digits.substr(0, int_len) + "." + digits.substr(int_len);
        }
    }
    return sign + out;
}

}  // namespace

std::string cavity_meta_json(const CavityMeta& m) {
    auto b = [](bool v) { return std::string(v ? "true" : "false"); };
    return "{\"w_geometry\": " + py_float_repr(m.w_geometry) +
           ", \"w_ionization_max\": " + py_float_repr(m.w_ionization_max) +
           ", \"w_ionization_effective\": " + py_float_repr(m.w_ionization_effective) +
           ", \"w_emission\": " + py_float_repr(m.w_emission) +
           ", \"emission_sign\": " + py_float_repr(m.emission_sign) + ", \"w_dust\": " + py_float_repr(m.w_dust) +
           ", \"ionization_confidence\": " + py_float_repr(m.ionization_confidence) +
           ", \"ratio_confidence\": " + py_float_repr(m.ratio_confidence) + ", \"is_linear\": " + b(m.is_linear) +
           ", \"sii_present\": " + b(m.sii_present) + ", \"capture_mode\": " + gdg::json_quote(m.capture_mode) +
           ", \"ionizer_source\": " + gdg::json_quote(m.ionizer_source) +
           ", \"footprint_frac\": " + py_float_repr(m.footprint_frac) +
           ", \"footprint_px\": " + std::to_string(m.footprint_px) + "}";
}

}  // namespace gdg::recon

namespace gdg::recon {

namespace {

/// The recursive-descent reader behind cavity_meta_from_json.
class MetaReader {
public:
    explicit MetaReader(const std::string& s) : s_(s) {}

    CavityMeta parse() {
        skip_ws();
        if (!peek('{')) fail("expected a JSON object");
        ++pos_;
        CavityMeta m;
        std::set<std::string> seen;
        skip_ws();
        if (peek('}')) {
            ++pos_;
            finish(seen);
        } else {
            for (;;) {
                skip_ws();
                if (!peek('"')) fail(at_end() ? "unexpected end of input, expected a key" : "expected a key");
                const std::string key = string_value("a key");
                if (!is_known(key)) fail("unknown key " + gdg::json_quote(key));
                if (!seen.insert(key).second) fail("duplicate key " + gdg::json_quote(key));
                skip_ws();
                if (!peek(':')) fail("expected ':' after key " + gdg::json_quote(key));
                ++pos_;
                skip_ws();
                value(m, key);
                skip_ws();
                if (peek(',')) {
                    ++pos_;
                    continue;
                }
                if (peek('}')) {
                    ++pos_;
                    break;
                }
                fail((at_end() ? "unexpected end of input after the value of " : "expected ',' or '}' after the value of ") +
                     gdg::json_quote(key));
            }
            finish(seen);
        }
        return m;
    }

private:
    const std::string& s_;
    size_t pos_ = 0;

    [[noreturn]] static void fail(const std::string& what) { throw std::invalid_argument("cavity meta: " + what); }
    bool at_end() const { return pos_ >= s_.size(); }
    bool peek(char c) const { return pos_ < s_.size() && s_[pos_] == c; }
    void skip_ws() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) ++pos_;
    }

    void finish(const std::set<std::string>& seen) {
        skip_ws();
        if (!at_end()) fail("trailing characters after the object");
        for (const char* k : kKeys)
            if (!seen.count(k)) fail("missing key " + gdg::json_quote(k));
    }

    static constexpr const char* kKeys[] = {"w_geometry",   "w_ionization_max", "w_ionization_effective",
                                            "w_emission",   "emission_sign",    "w_dust",
                                            "ionization_confidence", "ratio_confidence", "is_linear",
                                            "sii_present",  "capture_mode",     "ionizer_source",
                                            "footprint_frac", "footprint_px"};
    static bool is_known(const std::string& k) {
        for (const char* n : kKeys)
            if (k == n) return true;
        return false;
    }

    /// A JSON string literal as json_quote writes it. `what` names it in errors.
    std::string string_value(const std::string& what) {
        ++pos_;  // opening quote
        std::string out;
        for (;;) {
            if (at_end()) fail("unterminated string for " + what);
            const char ch = s_[pos_++];
            const auto c = static_cast<unsigned char>(ch);
            if (ch == '"') return out;
            if (c < 0x20) fail("raw control character in the string for " + what);
            if (ch != '\\') {
                out += ch;
                continue;
            }
            if (at_end()) fail("unterminated string for " + what);
            const char e = s_[pos_++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                // json_quote writes \u00xx (lowercase) for the other control bytes only.
                if (pos_ + 4 > s_.size() || s_[pos_] != '0' || s_[pos_ + 1] != '0')
                    fail("unsupported \\u escape in the string for " + what);
                unsigned v = 0;
                for (size_t i = 2; i < 4; ++i) {
                    const char h = s_[pos_ + i];
                    v <<= 4;
                    if (h >= '0' && h <= '9') v |= static_cast<unsigned>(h - '0');
                    else if (h >= 'a' && h <= 'f') v |= static_cast<unsigned>(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') v |= static_cast<unsigned>(h - 'A' + 10);
                    else fail("bad \\u escape in the string for " + what);
                }
                if (v >= 0x20) fail("unsupported \\u escape in the string for " + what);
                out += static_cast<char>(v);
                pos_ += 4;
                break;
            }
            default: fail("unsupported escape \\" + std::string(1, e) + " in the string for " + what);
            }
        }
    }

    /// The text of a JSON number (RFC 8259 grammar); `integer` forbids a fraction and exponent.
    std::string number_text(const std::string& key, bool integer) {
        const size_t start = pos_;
        auto digit = [&] { return pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9'; };
        const std::string quoted = gdg::json_quote(key);
        if (peek('-')) ++pos_;
        if (!digit()) fail("the value of " + quoted + " must be a " + (integer ? "integer" : "number"));
        if (s_[pos_] == '0') ++pos_;
        else
            while (digit()) ++pos_;
        if (!integer) {
            if (peek('.')) {
                ++pos_;
                if (!digit()) fail("malformed number for " + quoted);
                while (digit()) ++pos_;
            }
            if (peek('e') || peek('E')) {
                ++pos_;
                if (peek('+') || peek('-')) ++pos_;
                if (!digit()) fail("malformed number for " + quoted);
                while (digit()) ++pos_;
            }
        }
        if (peek('.') || peek('e') || peek('E') || digit())
            fail("the value of " + quoted + " must be " + (integer ? "an integer" : "a well-formed number"));
        return s_.substr(start, pos_ - start);
    }

    double double_value(const std::string& key) {
        const std::string t = number_text(key, false);
        double v = 0.0;
        const auto r = std::from_chars(t.data(), t.data() + t.size(), v);
        if (r.ec != std::errc() || r.ptr != t.data() + t.size() || !std::isfinite(v))
            fail("the number for " + gdg::json_quote(key) + " is out of range for a double: " + t);
        return v;
    }

    long long int_value(const std::string& key) {
        const std::string t = number_text(key, true);
        long long v = 0;
        const auto r = std::from_chars(t.data(), t.data() + t.size(), v);
        if (r.ec != std::errc() || r.ptr != t.data() + t.size())
            fail("the integer for " + gdg::json_quote(key) + " is out of range: " + t);
        return v;
    }

    bool bool_value(const std::string& key) {
        for (const auto& [lit, v] : {std::pair<const char*, bool>{"true", true}, {"false", false}}) {
            const size_t n = std::strlen(lit);
            if (s_.compare(pos_, n, lit) == 0) {
                pos_ += n;
                return v;
            }
        }
        fail("the value of " + gdg::json_quote(key) + " must be true or false");
    }

    std::string str_value(const std::string& key) {
        if (!peek('"')) fail("the value of " + gdg::json_quote(key) + " must be a string");
        return string_value(gdg::json_quote(key));
    }

    void value(CavityMeta& m, const std::string& k) {
        if (k == "w_geometry") m.w_geometry = double_value(k);
        else if (k == "w_ionization_max") m.w_ionization_max = double_value(k);
        else if (k == "w_ionization_effective") m.w_ionization_effective = double_value(k);
        else if (k == "w_emission") m.w_emission = double_value(k);
        else if (k == "emission_sign") m.emission_sign = double_value(k);
        else if (k == "w_dust") m.w_dust = double_value(k);
        else if (k == "ionization_confidence") m.ionization_confidence = double_value(k);
        else if (k == "ratio_confidence") m.ratio_confidence = double_value(k);
        else if (k == "is_linear") m.is_linear = bool_value(k);
        else if (k == "sii_present") m.sii_present = bool_value(k);
        else if (k == "capture_mode") m.capture_mode = str_value(k);
        else if (k == "ionizer_source") m.ionizer_source = str_value(k);
        else if (k == "footprint_frac") m.footprint_frac = double_value(k);
        else m.footprint_px = int_value(k);
    }
};

}  // namespace

CavityMeta cavity_meta_from_json(const std::string& json) { return MetaReader(json).parse(); }

}  // namespace gdg::recon
