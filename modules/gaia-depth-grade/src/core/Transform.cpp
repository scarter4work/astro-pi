#include "Transform.h"

#include "Numerics.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace gdg {

namespace {

// np.clip(x, lo, hi): NaN stays NaN.
double np_clip(double x, double lo, double hi) {
    if (std::isnan(x)) return x;
    return std::min(std::max(x, lo), hi);
}

void require_same(size_t a, size_t b, size_t c, const char* fn) {
    if (a != b || a != c) throw std::invalid_argument(std::string(fn) + ": r_med, r_lo and r_hi lengths differ");
}

}  // namespace

std::vector<double> depth_strength(const std::vector<double>& r_med, double p_low, double p_high, double neutral) {
    std::vector<double> out(r_med.size(), neutral);
    std::vector<size_t> idx;
    std::vector<double> logr;
    for (size_t i = 0; i < r_med.size(); ++i)
        if (std::isfinite(r_med[i]) && r_med[i] > 0.0) {
            idx.push_back(i);
            logr.push_back(std::log10(r_med[i]));
        }
    if (idx.empty()) return out;
    const double lo = percentile(logr, p_low);
    const double hi = percentile(logr, p_high);
    if (hi <= lo) return out;  // degenerate: no distance spread -> neutral, not a boost
    for (size_t k = 0; k < idx.size(); ++k) {
        const double norm = (np_clip(logr[k], lo, hi) - lo) / (hi - lo);  // 0 = near .. 1 = far
        out[idx[k]] = 1.0 - 2.0 * norm;                                    // +1 near .. -1 far
    }
    return out;
}

std::vector<double> confidence(const std::vector<double>& r_med, const std::vector<double>& r_lo,
                               const std::vector<double>& r_hi) {
    require_same(r_med.size(), r_lo.size(), r_hi.size(), "confidence");
    std::vector<double> c(r_med.size());
    for (size_t i = 0; i < c.size(); ++i) {
        const double frac = (r_hi[i] - r_lo[i]) / (2.0 * r_med[i]);
        const double v = 1.0 - np_clip(frac, 0.0, 1.0);
        c[i] = std::isfinite(v) ? v : 0.0;
    }
    return c;
}

std::vector<double> effective_strength(const std::vector<double>& r_med, const std::vector<double>& r_lo,
                                       const std::vector<double>& r_hi, double p_low, double p_high, double neutral) {
    require_same(r_med.size(), r_lo.size(), r_hi.size(), "effective_strength");
    const std::vector<double> s = depth_strength(r_med, p_low, p_high, neutral);
    const std::vector<double> c = confidence(r_med, r_lo, r_hi);
    std::vector<double> e(s.size());
    for (size_t i = 0; i < e.size(); ++i) e[i] = std::isfinite(r_med[i]) ? s[i] * c[i] : neutral;
    return e;
}

}  // namespace gdg
