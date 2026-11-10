// Task 6: the photutils 3.0.0 DAOStarFinder port against goldens dumped from photutils itself
// (make_goldens.py: dao_table / mono_dao_table, detect_table, and the phase_c_cases dao_*).
#include <catch_amalgamated.hpp>

#include "Golden.h"
#include "core/DaoFind.h"
#include "core/Detect.h"
#include "core/SigmaClip.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

using gdg::DaoParams;
using gdg::DaoStar;
using gdg::Image;
using gdg::test::golden;
using gdg::test::golden_image;
using gdg::test::max_abs_diff;

namespace {

constexpr double kNone = DaoParams::kNoLimit;

struct Err {
    double xy = 0.0;   // max |dx|, |dy| in px
    double rel = 0.0;  // max relative error of flux, peak, sharpness, roundness1, roundness2
    size_t unequal = 0;
};

double rel_err(double a, double b) {
    if (a == b) return 0.0;
    return std::fabs(a - b) / std::max(std::fabs(b), 1e-300);
}

// The port is bit-exact wherever libm's exp agrees with numpy's SIMD exp on the kernel (true on
// the reference host). When it does, every output must be bit-identical too, which pins numpy's
// summation order (pairwise blocks vs sequential axes) beyond what the plan tolerance can see.
bool kernels_bit_exact() {
    static const bool exact = [] {
        const gdg::DaoKernel k = gdg::dao_kernel(3.0), g = gdg::dao_kernel(5.0, 0.6, 35.0, 2.0);
        return k.data == golden("dao_kernel_fwhm3_data").data && k.unmasked == golden("dao_kernel_fwhm3_unmasked").data &&
               g.data == golden("dao_kernel_general_data").data;
    }();
    return exact;
}

// `want` is an (N, 7) golden: x, y, flux, peak, sharpness, roundness1, roundness2.
Err compare(const std::vector<DaoStar>& got, const std::string& name) {
    const auto g = golden(name);
    const size_t n = g.shape.size() == 2 ? g.shape[0] : 0;
    REQUIRE(got.size() == n);
    Err e;
    for (size_t i = 0; i < n; ++i) {
        const double* w = g.data.data() + i * 7;
        const DaoStar& s = got[i];
        const double v[7] = {s.x, s.y, s.flux, s.peak, s.sharpness, s.roundness1, s.roundness2};
        for (int c = 0; c < 7; ++c) {
            if (v[c] != w[c]) ++e.unequal;
            if (c < 2)
                e.xy = std::max(e.xy, std::fabs(v[c] - w[c]));
            else
                e.rel = std::max(e.rel, rel_err(v[c], w[c]));
        }
    }
    WARN(name << ": " << n << " stars, max |dxy| " << e.xy << " px, max rel " << e.rel << ", "
              << e.unequal << " of " << 7 * n << " values not bit-identical");
    // Plan tolerance: x/y <= 1e-9 px, flux/sharpness/roundness rel <= 1e-9.
    CHECK(e.xy <= 1e-9);
    CHECK(e.rel <= 1e-9);
    if (kernels_bit_exact()) CHECK(e.unequal == 0);
    return e;
}

Image minus_median(const Image& lum, double* std_out = nullptr) {
    const gdg::ClipStats st = gdg::sigma_clipped_stats(lum.data, 3.0);
    Image sub = lum;
    for (double& v : sub.data) v -= st.median;
    if (std_out) *std_out = st.std;
    return sub;
}

}  // namespace

TEST_CASE("dao_kernel_matches_photutils", "[dao][golden]") {
    const gdg::DaoKernel k = gdg::dao_kernel(3.0);
    CHECK(k.nx == 5);
    CHECK(k.ny == 5);
    CHECK(k.n_pixels == 13);
    CHECK(k.x_sigma == 1.2739827004320285);  // stage.json dao_kernel_fwhm3
    CHECK(k.rel_err == Catch::Approx(1.2850333090795458).epsilon(1e-15));
    const auto mask = golden("dao_kernel_fwhm3_mask").data;
    REQUIRE(mask.size() == k.mask.size());
    for (size_t i = 0; i < mask.size(); ++i) CHECK((mask[i] != 0.0) == (k.mask[i] != 0));
    const double du = max_abs_diff(k.unmasked, golden("dao_kernel_fwhm3_unmasked").data);
    const double dd = max_abs_diff(k.data, golden("dao_kernel_fwhm3_data").data);
    WARN("fwhm-3 kernel: max |d unmasked| " << du << ", max |d data| " << dd);
    CHECK(du <= 1e-15);  // numpy's SIMD exp vs libm: <= 1 ulp
    CHECK(dd <= 1e-15);

    const gdg::DaoKernel g = gdg::dao_kernel(5.0, 0.6, 35.0, 2.0);
    CHECK(g.nx == 7);
    CHECK(g.ny == 7);
    CHECK(g.n_pixels == 33);
    CHECK(g.y_sigma == 1.2739827004320285);
    CHECK(g.rel_err == Catch::Approx(0.7278703781836119).epsilon(1e-15));
    const double dg = max_abs_diff(g.data, golden("dao_kernel_general_data").data);
    WARN("general kernel: max |d data| " << dg);
    CHECK(dg <= 1e-15);

    CHECK_THROWS_AS(gdg::dao_kernel(0.0), std::invalid_argument);
    CHECK_THROWS_AS(gdg::dao_kernel(3.0, 1.5), std::invalid_argument);
    CHECK_THROWS_AS(gdg::dao_kernel(3.0, 1.0, 0.0, 0.0), std::invalid_argument);
}

TEST_CASE("dao_convolve_matches_scipy", "[dao][golden]") {
    const Image pair = golden_image("dao_pair_input");
    const Image c3 = gdg::dao_convolve(pair, gdg::dao_kernel(3.0));
    const Image cg = gdg::dao_convolve(pair, gdg::dao_kernel(5.0, 0.6, 35.0, 2.0));
    const double d3 = max_abs_diff(c3.data, golden("dao_pair_conv").data);
    const double dg = max_abs_diff(cg.data, golden("dao_general_conv_pair").data);
    WARN("convolution: max |delta| fwhm-3 " << d3 << ", general (asymmetric, flipped) " << dg);
    CHECK(d3 <= 1e-15);
    CHECK(dg <= 1e-15);
}

TEST_CASE("dao_matches_photutils_synthetic", "[dao][golden]") {
    // detect_stars' exact call: DAOStarFinder(fwhm=3, threshold=6*std)(lum - median).
    for (const auto& [stars, table] : {std::pair<std::string, std::string>{"stars_rgb", "dao_table"},
                                       {"mono_stars", "mono_dao_table"}}) {
        double std = 0.0;
        const Image sub = minus_median(gdg::luminance(golden_image(stars)), &std);
        DaoParams p;
        p.fwhm = 3.0;
        p.threshold = 6.0 * std;
        const auto got = gdg::dao_find(sub, p);
        // The (N,) column goldens of the generator's raw photutils table, stacked as (N, 7).
        const char* cols[7] = {"x", "y", "flux", "peak", "sharpness", "roundness1", "roundness2"};
        std::vector<std::vector<double>> want;
        for (const char* c : cols) want.push_back(golden(table + "_" + c).data);
        REQUIRE(got.size() == want[0].size());
        REQUIRE(got.size() > 30);
        double exy = 0.0, erel = 0.0;
        size_t unequal = 0;
        for (size_t i = 0; i < got.size(); ++i) {
            const double v[7] = {got[i].x, got[i].y, got[i].flux, got[i].peak, got[i].sharpness,
                                 got[i].roundness1, got[i].roundness2};
            for (int c = 0; c < 7; ++c) {
                if (v[c] != want[c][i]) ++unequal;
                if (c < 2)
                    exy = std::max(exy, std::fabs(v[c] - want[c][i]));
                else
                    erel = std::max(erel, rel_err(v[c], want[c][i]));
            }
        }
        WARN(table << ": " << got.size() << " stars, max |dxy| " << exy << " px, max rel " << erel << ", "
                   << unequal << " of " << 7 * got.size() << " values not bit-identical");
        CHECK(exy <= 1e-9);
        CHECK(erel <= 1e-9);
        if (kernels_bit_exact()) CHECK(unequal == 0);
    }
}

TEST_CASE("dao_noise_every_branch", "[dao][golden]") {
    // Unit noise, a NaN block, threshold -100 and no shape filters (see make_goldens.py 4b).
    const Image noise = golden_image("dao_noise_input");
    DaoParams p;
    p.threshold = -100.0;
    p.fwhm = 3.0;
    p.sharp_lo = p.round_lo = -kNone;
    p.sharp_hi = p.round_hi = kNone;
    compare(gdg::dao_find(noise, p), "dao_noise_default");
    p.min_separation = 3.0;
    compare(gdg::dao_find(noise, p), "dao_noise_minsep3");
    p.min_separation = 0.0;
    compare(gdg::dao_find(noise, p), "dao_noise_minsep0");
}

TEST_CASE("dao_peak_finder_edge_cases", "[dao][golden]") {
    DaoParams p;
    p.fwhm = 3.0;
    p.sharp_lo = p.round_lo = -kNone;
    p.sharp_hi = p.round_hi = kNone;
    // A negative local maximum whose circle the frame clips: photutils' cval-0 rule rejects it.
    p.threshold = -1e9;
    const auto bowl = gdg::dao_find(golden_image("dao_edge_bowl_input"), p);
    compare(bowl, "dao_edge_bowl");
    for (const DaoStar& s : bowl) CHECK_FALSE((std::fabs(s.x - 6.0) < 1.0 && std::fabs(s.y - 12.0) < 1.0));
    // A fainter source with a brighter one 5 px away on both axes: inside the 16x16 box but
    // outside the circle, so both are peaks.
    p.threshold = 0.1;
    const auto pair = gdg::dao_find(golden_image("dao_corner_pair_input"), p);
    compare(pair, "dao_corner_pair");
    CHECK(pair.size() == 2);
}

TEST_CASE("dao_threshold_is_strict_and_constant_input_finds_nothing", "[dao][golden]") {
    DaoParams p;
    p.fwhm = 3.0;
    p.threshold = golden("dao_pair_strict_threshold").data.at(0);  // the faintest peak's value
    p.scale_threshold = false;
    compare(gdg::dao_find(golden_image("dao_pair_input"), p), "dao_pair_strict");
    // Constant input gives no rows even with a threshold every pixel passes (photutils: None).
    // All zeros makes the convolved image constant too ("Input data is constant").
    DaoParams q;
    q.threshold = -1.0;
    q.fwhm = 3.0;
    CHECK(gdg::dao_find(Image(24, 20, 1, 0.3), q).empty());
    CHECK(gdg::dao_find(Image(24, 20, 1, 0.0), q).empty());
}

TEST_CASE("dao_min_separation_default_is_2p5_fwhm", "[dao][golden]") {
    const Image pair = golden_image("dao_pair_input");
    DaoParams p;
    p.threshold = 0.02;
    p.fwhm = 3.0;
    const auto def = gdg::dao_find(pair, p);
    compare(def, "dao_pair_default");
    p.min_separation = 7.5;  // 2.5 * fwhm, spelled out: identical rows
    const auto explicit_sep = gdg::dao_find(pair, p);
    REQUIRE(explicit_sep.size() == def.size());
    for (size_t i = 0; i < def.size(); ++i) {
        CHECK(explicit_sep[i].x == def[i].x);
        CHECK(explicit_sep[i].y == def[i].y);
    }
    p.min_separation = 0.0;  // kernel footprint: the close pair resolves into two peaks
    const auto zero = gdg::dao_find(pair, p);
    compare(zero, "dao_pair_minsep0");
    CHECK(zero.size() == def.size() + 1);
    p.min_separation = 3.0;  // odd circular footprint (arange(-3, 4))
    compare(gdg::dao_find(pair, p), "dao_pair_minsep3");
}

TEST_CASE("dao_rejects_by_sharpness_roundness", "[dao][golden]") {
    const Image rej = golden_image("dao_reject_input");
    DaoParams p;
    p.threshold = 0.02;
    p.fwhm = 3.0;
    const auto def = gdg::dao_find(rej, p);
    compare(def, "dao_reject_default");
    p.round_lo = -kNone;
    p.round_hi = kNone;
    const auto sharp_only = gdg::dao_find(rej, p);
    compare(sharp_only, "dao_reject_sharp_only");
    p.sharp_lo = -kNone;
    p.sharp_hi = kNone;
    const auto none = gdg::dao_find(rej, p);
    compare(none, "dao_reject_nofilter");
    REQUIRE(none.size() == 3);
    REQUIRE(def.size() == 1);
    // The hot pixel is the row with sharpness > 1; the streak fails roundness only.
    int hot = 0, streak = 0;
    for (const DaoStar& s : none) {
        if (s.sharpness > 1.0) ++hot;
        if (s.sharpness >= 0.2 && s.sharpness <= 1.0 &&
            (std::fabs(s.roundness1) > 1.0 || std::fabs(s.roundness2) > 1.0))
            ++streak;
    }
    CHECK(hot == 1);
    CHECK(streak == 1);
}

TEST_CASE("dao_general_kernel_border_unscaled", "[dao][golden]") {
    const Image sub = minus_median(gdg::luminance(golden_image("stars_rgb")));
    DaoParams p;
    p.threshold = 0.05;
    p.fwhm = 5.0;
    p.ratio = 0.6;
    p.theta = 35.0;
    p.sigma_radius = 2.0;
    p.exclude_border = true;
    p.scale_threshold = false;
    p.min_separation = 4.0;
    compare(gdg::dao_find(sub, p), "dao_general");
}

TEST_CASE("dao_nan_input", "[dao][golden]") {
    DaoParams p;
    p.threshold = 0.02;
    p.fwhm = 3.0;
    const auto got = gdg::dao_find(golden_image("dao_nan_input"), p);
    compare(got, "dao_nan");
    CHECK(got.size() == 2);  // the NaN beside the isolated star removes it (NaN cutout)
}

TEST_CASE("detect_empty_field_returns_empty", "[dao][detect]") {
    CHECK(gdg::detect_stars(golden_image("detect_empty_input"), 3.0, 6.0).empty());
    CHECK(gdg::detect_stars(Image(40, 30, 1, 0.25), 3.0, 6.0).empty());  // constant: no peaks
    CHECK(gdg::detect_stars(Image(0, 0, 1), 3.0, 6.0).empty());
    CHECK_THROWS_AS(gdg::detect_stars(Image(8, 8, 3), 3.0, 6.0), std::invalid_argument);
    CHECK_THROWS_AS(gdg::dao_find(Image(8, 8, 3), DaoParams{}), std::invalid_argument);
}

TEST_CASE("detect_mono_and_lum_of_colour", "[dao][detect][golden]") {
    for (const auto& [stars, table] : {std::pair<std::string, std::string>{"stars_rgb", "detect_table"},
                                       {"mono_stars", "mono_detect_table"}}) {
        const auto got = gdg::detect_stars(gdg::luminance(golden_image(stars)), 3.0, 6.0);
        const auto wx = golden(table + "_x").data, wy = golden(table + "_y").data, wf = golden(table + "_flux").data;
        REQUIRE(got.size() == wx.size());
        double exy = 0.0, ef = 0.0;
        for (size_t i = 0; i < got.size(); ++i) {
            exy = std::max({exy, std::fabs(got[i].x - wx[i]), std::fabs(got[i].y - wy[i])});
            ef = std::max(ef, rel_err(got[i].flux, wf[i]));
            CHECK(std::isnan(got[i].r_med));
            CHECK_FALSE(got[i].matched);
        }
        WARN(table << ": " << got.size() << " rows, max |dxy| " << exy << ", max rel flux " << ef);
        CHECK(exy <= 1e-9);
        CHECK(ef <= 1e-9);
    }
    // luminance: numpy's ((r + g) + b) / 3, mono passes through.
    Image c(1, 1, 3);
    c.data = {0.1, 0.2, 0.7};
    CHECK(gdg::luminance(c).data[0] == ((0.1 + 0.2) + 0.7) / 3.0);
    CHECK(gdg::luminance(golden_image("mono_stars")).data == golden_image("mono_stars").data);
}
