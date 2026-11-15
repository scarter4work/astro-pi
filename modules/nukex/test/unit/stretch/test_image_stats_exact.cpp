// compute_image_stats (nth_element selection, channels in parallel, finite
// samples only) against the original full-sort implementation, kept verbatim
// below as the reference. On finite data median, MAD, every percentile,
// sat_frac, the colour ratios and bright_concentration must be BIT-identical;
// skew may differ in the last bits only (it is a sum, and the reference summed
// in sorted order while the new code sums in pixel order).
//
// Real data (opt-in, never run by ctest): set NUKEX_STATS_REAL_FITS to a FITS
// file and run `test_image_stats_exact "[.realdata]"`; it prints the timings.
#include "catch_amalgamated.hpp"
#include "nukex/io/fits_reader.hpp"
#include "nukex/io/image.hpp"
#include "nukex/stretch/image_stats.hpp"
#include "nukex/stretch/param_model.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <limits>
#include <numeric>
#include <random>
#include <vector>

using namespace nukex;

namespace ref {
// ---- The original sort-based implementation (rev 9ca3152f), verbatim. ----
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
void fill_channel(const float* px, int n, float sat_level, int idx, ImageStats& s) {
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
ImageStats compute_image_stats(const Image& img, double star_fwhm_median = 0.0,
                               int star_count = 0, float saturation_level = 0.95f) {
    ImageStats s;
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
        for (int ch = 0; ch < 3; ++ch) fill_channel(base + ch * n, n, saturation_level, ch, s);
        const double eps = 1e-9;
        s.color_rg = s.median[0] / std::max(s.median[1], eps);
        s.color_bg = s.median[2] / std::max(s.median[1], eps);
    }
    double total = 0.0, bright = 0.0;
    for (int i = 0; i < n; ++i) {
        double lum = 0.0;
        if (c == 1) lum = base[i];
        else        lum = 0.2126 * base[i] + 0.7152 * base[n + i] + 0.0722 * base[2*n + i];
        total += lum;
        if (lum >= s.p99[0] && !std::isnan(s.p99[0])) bright += lum;
    }
    if (total > 0.0) s.bright_concentration = bright / total;
    s.fwhm_median = star_fwhm_median;
    s.star_count  = star_count;
    return s;
}
} // namespace ref

namespace {

bool same_bits(double a, double b) {
    return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

// Every field bit-identical, except skew (relative 1e-9; reported).
void require_exact(const ImageStats& got, const ImageStats& want, int channels) {
    for (int c = 0; c < 3; ++c) {
        INFO("channel " << c);
        CHECK(same_bits(got.median[c],   want.median[c]));
        CHECK(same_bits(got.mad[c],      want.mad[c]));
        CHECK(same_bits(got.p50[c],      want.p50[c]));
        CHECK(same_bits(got.p95[c],      want.p95[c]));
        CHECK(same_bits(got.p99[c],      want.p99[c]));
        CHECK(same_bits(got.p999[c],     want.p999[c]));
        CHECK(same_bits(got.sat_frac[c], want.sat_frac[c]));
        if (c < channels) {
            const double tol = 1e-9 * std::max(1.0, std::fabs(want.skew[c]));
            CHECK(std::fabs(got.skew[c] - want.skew[c]) <= tol);
        } else {
            CHECK(std::isnan(got.skew[c]));
        }
    }
    CHECK(same_bits(got.color_rg, want.color_rg));
    CHECK(same_bits(got.color_bg, want.color_bg));
    CHECK(same_bits(got.bright_concentration, want.bright_concentration));
}

// Quantised random pixels (many ties, like integer-ADU data) plus a few
// saturated ones.
Image random_image(int w, int h, int c, unsigned seed) {
    Image im(w, h, c);
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> q(0, 4095);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    float* d = im.data();
    for (std::size_t i = 0; i < im.data_size(); ++i) {
        float v = static_cast<float>(q(rng)) / 4095.0f;
        if (i % 3 == 0) v = u(rng) * 0.2f;  // a background-like mode
        if (i % 997 == 0) v = 1.0f;
        d[i] = v;
    }
    return im;
}

// The image with every pixel that is non-finite in ANY channel... is not what
// the stats mean: each channel drops its own non-finite samples. So build the
// finite-only reference one channel at a time, as mono images.
ImageStats per_channel_reference(const Image& img) {
    ImageStats s = ref::compute_image_stats(Image(0, 0, 1));
    const int n = img.total_pixels();
    for (int c = 0; c < img.n_channels(); ++c) {
        std::vector<float> fin;
        for (int i = 0; i < n; ++i) {
            const float v = img.channel_data(c)[i];
            if (std::isfinite(v)) fin.push_back(v);
        }
        Image mono(static_cast<int>(fin.size()), 1, 1);
        std::copy(fin.begin(), fin.end(), mono.data());
        const ImageStats m = ref::compute_image_stats(mono);
        s.median[c] = m.median[0]; s.mad[c] = m.mad[0]; s.p50[c] = m.p50[0];
        s.p95[c] = m.p95[0]; s.p99[c] = m.p99[0]; s.p999[c] = m.p999[0];
        s.skew[c] = m.skew[0]; s.sat_frac[c] = m.sat_frac[0];
    }
    return s;
}

void require_channel_exact(const ImageStats& got, const ImageStats& want, int c) {
    INFO("channel " << c);
    REQUIRE(same_bits(got.median[c],   want.median[c]));
    REQUIRE(same_bits(got.mad[c],      want.mad[c]));
    REQUIRE(same_bits(got.p95[c],      want.p95[c]));
    REQUIRE(same_bits(got.p99[c],      want.p99[c]));
    REQUIRE(same_bits(got.p999[c],     want.p999[c]));
    REQUIRE(same_bits(got.sat_frac[c], want.sat_frac[c]));
    REQUIRE(std::fabs(got.skew[c] - want.skew[c]) <= 1e-9 * std::max(1.0, std::fabs(want.skew[c])));
}

} // namespace

TEST_CASE("compute_image_stats: bit-identical to the sort-based original on finite data",
          "[stretch][image_stats][exact]") {
    struct Shape { int w, h, c; };
    const Shape shapes[] = {
        {1, 1, 1}, {2, 1, 1}, {3, 1, 1}, {1, 1, 3}, {2, 1, 3},
        {257, 131, 1}, {256, 128, 1},   // odd and even pixel counts
        {257, 131, 3}, {256, 128, 3},
        {1001, 999, 3},                 // ~1 MP RGB
    };
    unsigned seed = 1;
    for (const auto& sh : shapes) {
        INFO(sh.w << "x" << sh.h << "x" << sh.c);
        const Image im = random_image(sh.w, sh.h, sh.c, seed++);
        require_exact(compute_image_stats(im), ref::compute_image_stats(im), sh.c);
        require_exact(compute_image_stats(im, 2.5, 17, 0.5f),
                      ref::compute_image_stats(im, 2.5, 17, 0.5f), sh.c);
    }
}

TEST_CASE("compute_image_stats: NaN and Inf samples are excluded per channel",
          "[stretch][image_stats][nan]") {
    Image im = random_image(301, 203, 3, 42);
    const int n = im.total_pixels();
    const float nan_ = std::numeric_limits<float>::quiet_NaN();
    const float inf_ = std::numeric_limits<float>::infinity();
    // Scatter NaN / +Inf / -Inf at different densities per channel. A NaN in
    // the input of the old std::sort broke its strict weak ordering (UB).
    for (int i = 0; i < n; i += 7)  im.channel_data(0)[i] = nan_;
    for (int i = 3; i < n; i += 11) im.channel_data(1)[i] = (i % 2) ? inf_ : -inf_;
    for (int i = 5; i < n; i += 5)  im.channel_data(2)[i] = (i % 3) ? nan_ : inf_;

    const ImageStats got  = compute_image_stats(im);
    const ImageStats want = per_channel_reference(im);
    for (int c = 0; c < 3; ++c) require_channel_exact(got, want, c);
    REQUIRE(same_bits(got.color_rg, want.median[0] / std::max(want.median[1], 1e-9)));
    REQUIRE(same_bits(got.color_bg, want.median[2] / std::max(want.median[1], 1e-9)));
    // bright_concentration over the pixels whose luminance is finite.
    double total = 0.0, bright = 0.0;
    std::size_t used = 0;
    for (int i = 0; i < n; ++i) {
        const double lum = 0.2126 * im.channel_data(0)[i] + 0.7152 * im.channel_data(1)[i]
                         + 0.0722 * im.channel_data(2)[i];
        if (!std::isfinite(lum)) continue;
        ++used;
        total += lum;
        if (lum >= want.p99[0]) bright += lum;
    }
    REQUIRE(used > static_cast<std::size_t>(n) / 2);
    REQUIRE(std::isfinite(got.bright_concentration));
    REQUIRE(same_bits(got.bright_concentration, bright / total));
}

TEST_CASE("compute_image_stats: an all-NaN channel is NaN, the others are unaffected",
          "[stretch][image_stats][nan]") {
    Image im = random_image(64, 48, 3, 7);
    const int n = im.total_pixels();
    for (int i = 0; i < n; ++i) im.channel_data(1)[i] = std::numeric_limits<float>::quiet_NaN();

    const ImageStats got = compute_image_stats(im);
    for (double v : {got.median[1], got.mad[1], got.p50[1], got.p95[1], got.p99[1],
                     got.p999[1], got.skew[1], got.sat_frac[1]})
        REQUIRE(std::isnan(v));
    const ImageStats want = per_channel_reference(im);
    require_channel_exact(got, want, 0);
    require_channel_exact(got, want, 2);
    REQUIRE(std::isnan(got.color_rg));   // G has no finite sample
    REQUIRE(std::isnan(got.color_bg));
    // Every pixel has a NaN channel, so no luminance is finite.
    REQUIRE(got.bright_concentration == 0.0);

    // A non-finite feature row is refused, so the caller keeps its defaults.
    ParamModel pm("MTF");
    pm.add_param("midtone", ParamCoefficients{});
    struct Probe : StretchOp {
        std::map<std::string, std::pair<float, float>> param_bounds() const override {
            return {{"midtone", {0.0f, 1.0f}}};
        }
        bool set_param(const std::string&, float) override { return true; }
        void apply(Image&) const override {}
    } op;
    REQUIRE_FALSE(pm.predict_and_apply(got, op));
}

TEST_CASE("compute_image_stats: an all-NaN mono image is NaN throughout",
          "[stretch][image_stats][nan]") {
    Image im(16, 16, 1);
    std::fill(im.data(), im.data() + im.data_size(), std::numeric_limits<float>::quiet_NaN());
    const ImageStats s = compute_image_stats(im);
    REQUIRE(std::isnan(s.median[0]));
    REQUIRE(std::isnan(s.mad[0]));
    REQUIRE(std::isnan(s.sat_frac[0]));
    REQUIRE(s.color_rg == 1.0);
    REQUIRE(s.bright_concentration == 0.0);
}

TEST_CASE("compute_image_stats: real data, bit-exact and timed", "[.realdata]") {
    const char* path = std::getenv("NUKEX_STATS_REAL_FITS");
    if (!path) SKIP("set NUKEX_STATS_REAL_FITS to a FITS file");
    auto rd = FITSReader::read(path);
    REQUIRE(rd.success);
    const Image& im = rd.image;
    std::printf("image %dx%dx%d\n", im.width(), im.height(), im.n_channels());
    std::size_t nonfinite = 0;
    for (std::size_t i = 0; i < im.data_size(); ++i) nonfinite += !std::isfinite(im.data()[i]);
    std::printf("non-finite samples: %zu\n", nonfinite);
    REQUIRE(nonfinite == 0);  // the comparison below is for finite data

    using clk = std::chrono::steady_clock;
    auto t0 = clk::now();
    const ImageStats want = ref::compute_image_stats(im);
    auto t1 = clk::now();
    const ImageStats got = compute_image_stats(im);
    auto t2 = clk::now();
    double best = std::chrono::duration<double>(t2 - t1).count();
    for (int k = 0; k < 4; ++k) {
        auto a = clk::now();
        const ImageStats again = compute_image_stats(im);
        best = std::min(best, std::chrono::duration<double>(clk::now() - a).count());
        require_exact(again, got, im.n_channels());
    }
    std::printf("reference (full sort): %.3f s\n", std::chrono::duration<double>(t1 - t0).count());
    std::printf("new (first run):       %.3f s\n", std::chrono::duration<double>(t2 - t1).count());
    std::printf("new (best of 5):       %.3f s\n", best);
    for (int c = 0; c < im.n_channels(); ++c)
        std::printf("ch%d median %.9g mad %.9g skew new %.17g ref %.17g (rel diff %.3g)\n", c,
                    got.median[c], got.mad[c], got.skew[c], want.skew[c],
                    std::fabs(got.skew[c] - want.skew[c]) / std::fabs(want.skew[c]));
    require_exact(got, want, im.n_channels());
}
