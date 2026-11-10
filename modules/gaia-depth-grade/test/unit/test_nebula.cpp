// Task 10: the nebula grade (nebula.py) against the Task 1 goldens. Image goldens are float32
// copies of the float64 Python result: compared at float32's half ulp (<= 6e-8) and reported as
// float32-identical when rounding the port's float64 result reproduces them exactly.
#include <catch_amalgamated.hpp>

#include "Golden.h"
#include "core/Nebula.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

using gdg::Image;
using gdg::test::golden;
using gdg::test::golden_image;

namespace {

std::vector<gdg::StarRow> table(const std::string& prefix, bool with_r) {
    const auto x = golden(prefix + "_x").data, y = golden(prefix + "_y").data, f = golden(prefix + "_flux").data;
    std::vector<double> r(x.size(), NAN);
    if (with_r) r = golden(prefix + "_r_med_geo").data;
    std::vector<gdg::StarRow> t;
    for (size_t i = 0; i < x.size(); ++i) {
        gdg::StarRow row{x[i], y[i], f[i]};
        row.r_med = r[i];
        t.push_back(row);
    }
    return t;
}

void check_f32(const Image& got, const std::string& name) {
    const Image want = golden_image(name);
    REQUIRE(got.same_shape(want));
    double d = 0.0;
    size_t f32_unequal = 0;
    for (size_t i = 0; i < got.data.size(); ++i) {
        d = std::max(d, std::fabs(got.data[i] - want.data[i]));
        if (static_cast<double>(static_cast<float>(got.data[i])) != want.data[i]) ++f32_unequal;
    }
    WARN(name << ": max |delta| " << d << " vs the float32 golden, " << f32_unequal << " of " << got.data.size()
              << " samples not float32-identical");
    CHECK(d <= 6e-8);
    CHECK(f32_unequal == 0);
}

}  // namespace

TEST_CASE("budget_matches_golden", "[nebula][golden]") {
    for (const std::string p : {"", "mono_"}) {
        const double want = golden(p + "budget").data.at(0);
        const double got = gdg::gaia_depth_budget(golden(p + "match_table_r_med_geo").data);
        WARN(p << "budget: " << got << " vs " << want << ", |delta| " << std::fabs(got - want));
        CHECK(std::fabs(got - want) <= 1e-15);  // log10: numpy SIMD vs libm, <= 1 ulp
        CHECK(got > 0.0);
    }
    CHECK(gdg::gaia_depth_budget({100.0, 1000.0}, 0.5) == 0.0);
    // Both synthetic fields clip at 1, so an unclipped value from numpy too: 400..760 pc in
    // 15 pc steps plus a NaN and a 0 (nebula.gaia_depth_budget, ref_dex 0.5 and 0.3).
    std::vector<double> r;
    for (int i = 0; i < 25; ++i) r.push_back(400.0 + 15.0 * i);
    r.push_back(NAN);
    r.push_back(0.0);
    CHECK(std::fabs(gdg::gaia_depth_budget(r) - 0x1.fe7aacd52b8f0p-2) <= 1e-15);
    CHECK(std::fabs(gdg::gaia_depth_budget(r, 0.3) - 0x1.a9663ab1a44c8p-1) <= 1e-15);
}

TEST_CASE("budget_zero_below_20_matches", "[nebula]") {
    // nebula.py: fewer than 20 finite positive distances -> 0. NaN, 0 and negatives do not count.
    std::vector<double> r;
    for (int i = 0; i < 19; ++i) r.push_back(100.0 * (i + 1));
    r.insert(r.end(), {NAN, 0.0, -5.0, std::numeric_limits<double>::infinity()});
    CHECK(gdg::gaia_depth_budget(r) == 0.0);
    r.push_back(2500.0);  // the 20th
    const double b = gdg::gaia_depth_budget(r);
    CHECK(b > 0.0);
    CHECK(b <= 1.0);
    CHECK(gdg::gaia_depth_budget(golden("odd_table_r_med_geo").data) == 0.0);  // 6 finite
    // Spread clipped at 1: 1 pc .. 1e6 pc over ref 0.5 dex.
    std::vector<double> wide;
    for (int i = 0; i < 40; ++i) wide.push_back(std::pow(10.0, i * 0.15));
    CHECK(gdg::gaia_depth_budget(wide) == 1.0);
}

TEST_CASE("structural_depth_matches_golden", "[nebula][golden]") {
    // sigma = 0.03 * min(h, w) = 4.32 on the 192x144 field, 0.5 sigma for the final smoothing.
    check_f32(gdg::structural_depth(gdg::luminance(golden_image("starless_rgb"))), "structural_depth");
    check_f32(gdg::structural_depth(golden_image("mono_starless")), "mono_structural_depth");
    CHECK_THROWS_AS(gdg::structural_depth(golden_image("starless_rgb")), std::invalid_argument);
    Image bad(8, 8, 1, 0.1);
    bad.data[3] = NAN;
    CHECK_THROWS_AS(gdg::structural_depth(bad), std::invalid_argument);
}

TEST_CASE("nebula_matches_golden_111", "[nebula][golden]") {
    // Budget from the matched table, as render_nebula(budget=None) computes it.
    check_f32(gdg::render_nebula(golden_image("starless_rgb"), table("match_table", true), 1.0, 1.0, 1.0),
              "nebula_out_s1_a1_t1");
}

TEST_CASE("nebula_matches_golden_2_05_15", "[nebula][golden]") {
    check_f32(gdg::render_nebula(golden_image("starless_rgb"), table("match_table", true), 2.0, 0.5, 1.5),
              "nebula_out_s2_a0p5_t1p5");
    // The 64x48 edge frame with an explicit budget (its 7-row table is below the 20-star floor).
    check_f32(gdg::render_nebula(golden_image("odd_starless"), table("odd_table", false), 2.0, 0.5, 1.5, 0.8),
              "odd_nebula_out_s2_a0p5_t1p5");
}

TEST_CASE("nebula_mono_has_no_colour_tilt", "[nebula][golden]") {
    // Review Focus 2: a one-channel starless layer takes the ndim == 2 branch (brightness and
    // structural pop only) and matches Python's mono output.
    const Image mono = golden_image("mono_starless");
    REQUIRE(mono.channels == 1);
    check_f32(gdg::render_nebula(mono, table("mono_match_table", true), 2.0, 0.5, 1.5), "mono_nebula_out_s2_a0p5_t1p5");
    // A grey colour layer is tilted (red lifted, blue cooled where s != 0); the same data as mono
    // is not, and its brightness step equals the grey layer's green channel before the tilt.
    Image grey(mono.width, mono.height, 3);
    for (size_t i = 0; i < mono.data.size(); ++i)
        for (int c = 0; c < 3; ++c) grey.data[i * 3 + c] = mono.data[i];
    const Image g = gdg::render_nebula(grey, {}, 1.0, 1.0, 0.0, 1.0);
    const Image m = gdg::render_nebula(mono, {}, 1.0, 1.0, 0.0, 1.0);
    double tilt = 0.0, green_vs_mono = 0.0;
    for (size_t i = 0; i < m.data.size(); ++i) {
        tilt = std::max(tilt, std::fabs(g.data[i * 3] - g.data[i * 3 + 2]));
        green_vs_mono = std::max(green_vs_mono, std::fabs(g.data[i * 3 + 1] - m.data[i]));
    }
    CHECK(tilt > 1e-4);
    CHECK(green_vs_mono <= 1e-15);  // grey: saturation is a no-op on green, tilt never touches it
}

TEST_CASE("nebula_zero_strength_identity", "[nebula]") {
    // amount <= 1e-6 returns the input unchanged -- not even clipped.
    Image s = golden_image("starless_rgb");
    s.data[0] = 1.25;
    s.data[1] = -0.5;
    const auto t = table("match_table", true);
    CHECK(gdg::render_nebula(s, t, 0.0).data == s.data);
    CHECK(gdg::render_nebula(s, t, 1.0, 1.0, 1.0, 0.0).data == s.data);
    CHECK(gdg::render_nebula(s, t, 1e-6, 1.0, 1.0, 1.0).data == s.data);
    CHECK(gdg::render_nebula(s, table("odd_table", true), 1.0).data == s.data);  // budget 0: < 20 stars
    const Image graded = gdg::render_nebula(s, t, 1.0);
    CHECK(graded.data[0] == 1.0);  // graded output is clipped
    CHECK(graded.data[1] == 0.0);
}
