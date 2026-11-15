#include "nukex/stretch/image_stats.hpp"

#include "nukex/core/parallel_for.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numeric>
#include <vector>

namespace nukex {

namespace {

// Linear-interpolated percentile at q in [0, 1] of n sorted samples, given the
// two order statistics it reads: lo = sorted[i], hi = sorted[i + 1], with
// i = floor(q * (n - 1)). This is the exact arithmetic of the original
// sort-based implementation -- including the float subtraction (hi - lo) -- so
// the result is bit-identical to it.
double interpolate(float lo, float hi, double frac) {
    return lo + frac * (hi - lo);
}

// Rank i = floor(q * (n - 1)) and the interpolation fraction for quantile q.
struct QuantilePos {
    std::size_t i;
    double      frac;
};

QuantilePos quantile_pos(std::size_t n, double q) {
    const double pos = q * static_cast<double>(n - 1);
    const std::size_t i = static_cast<std::size_t>(pos);
    return {i, pos - static_cast<double>(i)};
}

// Selects the order statistics at `ranks` (ascending, unique, each < v.size())
// in place, without sorting: after nth_element at rank r every element of
// [r + 1, end) is >= v[r], so the next rank is selected from that tail alone.
// v must hold no NaN (nth_element needs a strict weak ordering). Returns the
// value at each rank, in the order of `ranks`.
std::vector<float> select_ranks(std::vector<float>& v,
                                const std::vector<std::size_t>& ranks) {
    std::vector<float> out;
    out.reserve(ranks.size());
    std::size_t lo = 0;
    for (std::size_t r : ranks) {
        const auto at_r = v.begin() + static_cast<std::ptrdiff_t>(r);
        if (r == lo) {
            // The next rank right after the last one is the tail's minimum: a
            // single linear pass, much cheaper than another selection.
            std::iter_swap(at_r, std::min_element(at_r, v.end()));
        } else {
            std::nth_element(v.begin() + static_cast<std::ptrdiff_t>(lo), at_r, v.end());
        }
        out.push_back(v[r]);
        lo = r + 1;
    }
    return out;
}

// Exact percentiles of v (finite, non-empty) at every q in `qs`, with the
// original percentile_sorted() semantics. Permutes v.
std::vector<double> exact_percentiles(std::vector<float>& v,
                                      const std::vector<double>& qs) {
    const std::size_t n = v.size();
    std::vector<std::size_t> ranks;
    for (double q : qs) {
        const auto p = quantile_pos(n, q);
        ranks.push_back(p.i);
        if (p.i + 1 < n) ranks.push_back(p.i + 1);
    }
    std::sort(ranks.begin(), ranks.end());
    ranks.erase(std::unique(ranks.begin(), ranks.end()), ranks.end());

    const std::vector<float> vals = select_ranks(v, ranks);
    auto at = [&](std::size_t r) {
        return vals[static_cast<std::size_t>(
            std::lower_bound(ranks.begin(), ranks.end(), r) - ranks.begin())];
    };

    std::vector<double> out;
    out.reserve(qs.size());
    for (double q : qs) {
        const auto p = quantile_pos(n, q);
        // The original returned sorted.back() (= sorted[n - 1] = sorted[i])
        // when there is no i + 1.
        if (p.i + 1 >= n) out.push_back(at(p.i));
        else              out.push_back(interpolate(at(p.i), at(p.i + 1), p.frac));
    }
    return out;
}

double skewness(const std::vector<float>& v, double mean_) {
    if (v.size() < 3) return 0.0;
    double m2 = 0.0, m3 = 0.0;
    for (float x : v) { const double d = x - mean_; m2 += d*d; m3 += d*d*d; }
    m2 /= static_cast<double>(v.size());
    m3 /= static_cast<double>(v.size());
    if (m2 < 1e-12) return 0.0;
    return m3 / std::pow(m2, 1.5);
}

// Per-channel statistics over the channel's FINITE samples only (NaN and
// +/-Inf are skipped). A channel with no finite sample leaves every one of its
// fields NaN, the same "not applicable" marker the unused channels of a mono
// image carry. Writes only index `idx` of s, so channels may run concurrently.
void fill_channel(const float* px, int n, float sat_level,
                  int idx, ImageStats& s) {
    std::vector<float> v;
    v.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        if (std::isfinite(px[i])) v.push_back(px[i]);
    if (v.empty()) return;  // all fields stay NaN

    const double nf = static_cast<double>(v.size());

    // Order-independent of the selection below: mean, skew and saturation are
    // taken in pixel order BEFORE nth_element permutes v.
    const double mean_ = std::accumulate(v.begin(), v.end(), 0.0) / nf;
    s.skew[idx] = skewness(v, mean_);
    std::size_t sat_n = 0;
    for (float x : v) if (x >= sat_level) ++sat_n;
    s.sat_frac[idx] = static_cast<double>(sat_n) / nf;

    const auto pct = exact_percentiles(v, {0.5, 0.95, 0.99, 0.999});
    s.median[idx] = pct[0];
    s.p50[idx]    = pct[0];
    s.p95[idx]    = pct[1];
    s.p99[idx]    = pct[2];
    s.p999[idx]   = pct[3];

    // MAD: median of |x - median|, reusing v's storage (its order is free now).
    const double med = s.median[idx];
    for (float& x : v) x = std::fabs(static_cast<float>(x - med));
    s.mad[idx] = exact_percentiles(v, {0.5})[0];
}

} // namespace

std::array<double, 29> ImageStats::to_feature_row() const {
    std::array<double, 29> r{};
    int k = 0;
    for (double v : median)   r[k++] = v;
    for (double v : mad)      r[k++] = v;
    for (double v : p50)      r[k++] = v;
    for (double v : p95)      r[k++] = v;
    for (double v : p99)      r[k++] = v;
    for (double v : p999)     r[k++] = v;
    for (double v : skew)     r[k++] = v;
    for (double v : sat_frac) r[k++] = v;
    r[k++] = bright_concentration;
    r[k++] = color_rg;
    r[k++] = color_bg;
    r[k++] = fwhm_median;
    r[k++] = static_cast<double>(star_count);
    return r;
}

ImageStats compute_image_stats(const Image& img,
                               double       star_fwhm_median,
                               int          star_count,
                               float        saturation_level) {
    ImageStats s;
    // Mark un-populated channels as NaN up front
    const double nan_ = std::numeric_limits<double>::quiet_NaN();
    s.median.fill(nan_); s.mad.fill(nan_);
    s.p50.fill(nan_);    s.p95.fill(nan_); s.p99.fill(nan_); s.p999.fill(nan_);
    s.skew.fill(nan_);   s.sat_frac.fill(nan_);

    const int w = img.width(), h = img.height(), c = img.n_channels();
    const int n = w * h;
    if (n == 0 || (c != 1 && c != 3)) return s;

    const float* base = img.data();
    // One channel per task: each writes only its own index of s.
    parallel_for_dynamic(c, 1, [&](int ch, int /*worker*/) {
        fill_channel(base + static_cast<std::size_t>(ch) * n, n,
                     saturation_level, ch, s);
    });
    if (c == 1) {
        s.color_rg = 1.0;
        s.color_bg = 1.0;
    } else {
        // NaN when a channel involved has no finite sample (NaN median).
        const double eps = 1e-9;
        s.color_rg = s.median[0] / std::max(s.median[1], eps);
        s.color_bg = s.median[2] / std::max(s.median[1], eps);
    }

    // Bright concentration: fraction of luminance above p99 median.
    // For mono we use channel 0; for RGB we use luminance = .2126 R + .7152 G + .0722 B.
    // Pixels whose luminance is not finite (any channel NaN/Inf) are skipped.
    double total = 0.0, bright = 0.0;
    for (int i = 0; i < n; ++i) {
        double lum = 0.0;
        if (c == 1) lum = base[i];
        else        lum = 0.2126 * base[i]
                         + 0.7152 * base[n + i]
                         + 0.0722 * base[2*n + i];
        if (!std::isfinite(lum)) continue;
        total += lum;
        if (lum >= s.p99[0] && !std::isnan(s.p99[0])) bright += lum;
    }
    if (total > 0.0) s.bright_concentration = bright / total;

    s.fwhm_median = star_fwhm_median;
    s.star_count  = star_count;
    return s;
}

} // namespace nukex
