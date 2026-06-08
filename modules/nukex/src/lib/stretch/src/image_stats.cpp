#include "nukex/stretch/image_stats.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <vector>

namespace nukex {

namespace {

double percentile_sorted(const std::vector<float>& sorted, double q) {
    if (sorted.empty()) return std::numeric_limits<double>::quiet_NaN();
    const double pos = q * static_cast<double>(sorted.size() - 1);
    const std::size_t i = static_cast<std::size_t>(pos);
    const double frac = pos - static_cast<double>(i);
    if (i + 1 >= sorted.size()) return sorted.back();
    return sorted[i] + frac * (sorted[i + 1] - sorted[i]);
}

double median_of_sorted(const std::vector<float>& sorted) {
    return percentile_sorted(sorted, 0.5);
}

double mad_of_sorted(const std::vector<float>& sorted, double med) {
    if (sorted.empty()) return 0.0;
    std::vector<float> abs_dev;
    abs_dev.reserve(sorted.size());
    for (float v : sorted) abs_dev.push_back(std::fabs(static_cast<float>(v - med)));
    std::sort(abs_dev.begin(), abs_dev.end());
    return median_of_sorted(abs_dev);
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

void fill_channel(const float* px, int n, float sat_level,
                  int idx, ImageStats& s) {
    std::vector<float> v(px, px + n);
    std::sort(v.begin(), v.end());
    s.median[idx]   = median_of_sorted(v);
    s.mad[idx]      = mad_of_sorted(v, s.median[idx]);
    s.p50[idx]      = s.median[idx];
    s.p95[idx]      = percentile_sorted(v, 0.95);
    s.p99[idx]      = percentile_sorted(v, 0.99);
    s.p999[idx]     = percentile_sorted(v, 0.999);
    const double mean_ =
        std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
    s.skew[idx]     = skewness(v, mean_);
    int sat_n = 0;
    for (float x : v) if (x >= sat_level) ++sat_n;
    s.sat_frac[idx] = static_cast<double>(sat_n) / static_cast<double>(v.size());
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
    if (c == 1) {
        fill_channel(base, n, saturation_level, 0, s);
        s.color_rg = 1.0;
        s.color_bg = 1.0;
    } else {
        for (int ch = 0; ch < 3; ++ch) {
            fill_channel(base + ch * n, n, saturation_level, ch, s);
        }
        const double eps = 1e-9;
        s.color_rg = s.median[0] / std::max(s.median[1], eps);
        s.color_bg = s.median[2] / std::max(s.median[1], eps);
    }

    // Bright concentration: fraction of luminance above p99 median.
    // For mono we use channel 0; for RGB we use luminance = .2126 R + .7152 G + .0722 B.
    double total = 0.0, bright = 0.0;
    for (int i = 0; i < n; ++i) {
        double lum = 0.0;
        if (c == 1) lum = base[i];
        else        lum = 0.2126 * base[i]
                         + 0.7152 * base[n + i]
                         + 0.0722 * base[2*n + i];
        total += lum;
        if (lum >= s.p99[0] && !std::isnan(s.p99[0])) bright += lum;
    }
    if (total > 0.0) s.bright_concentration = bright / total;

    s.fwhm_median = star_fwhm_median;
    s.star_count  = star_count;
    return s;
}

} // namespace nukex
