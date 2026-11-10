// Task 18 (recon plan task 6): extinction.py against the synthetic_blister goldens, plus the edge
// cases of gaia-depth-grade/tests/recon/test_extinction.py.
#include <catch_amalgamated.hpp>

#include "ReconGolden.h"
#include "core/recon/Extinction.h"

#include <cmath>
#include <random>
#include <stdexcept>

using namespace gdg::recon;
using gdg::Image;
using gdg::test::golden_image;

namespace {

/// test_extinction._lane_field: sky 0.01, a bright field from column 20 with a 0.02 sin ripple,
/// a dark lane at rows 30..65, columns 50..55 (float32).
Image lane_field() {
    const int n = 96;
    Image lum(n, n, 1, f32(0.01));
    for (int y = 0; y < n; ++y)
        for (int x = 20; x < n; ++x) lum.at(x, y) = f32(1.0 + 0.02 * std::sin(static_cast<double>(x - 20)));
    for (int y = 30; y < 66; ++y)
        for (int x = 50; x < 56; ++x) lum.at(x, y) = f32(0.1);
    return lum;
}

double region_mean(const Image& im, int y0, int y1, int x0, int x1) {
    double s = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) s += im.at(x, y);
    return s / ((y1 - y0) * (x1 - x0));
}

double region_max(const Image& im, int y0, int y1, int x0, int x1) {
    double m = -INFINITY;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) m = std::max(m, im.at(x, y));
    return m;
}

void check_opacity_plane(const Image& d) {
    for (double v : d.data) {
        REQUIRE(v >= 0.0);
        REQUIRE(v <= 1.0);
        REQUIRE(v == f32(v));
    }
}

}  // namespace

TEST_CASE("dust_from_structure_matches_golden", "[recon][extinction][golden]") {
    // Gaussian, grey closing and percentiles are scipy/numpy-exact; the rest is IEEE arithmetic.
    gdg::test::check_golden_image(dust_from_structure(golden_image("recon_lum")), "recon_dust_structure", 0.0, 0);
}

TEST_CASE("dust_from_balmer_matches_golden", "[recon][extinction][golden]") {
    // np.exp is numpy's own SIMD exp, glibc exp here; they may differ by an ulp in float64, which
    // the float32 output absorbs on this fixture (measured: bit-identical). The plan's 1e-6 stays
    // the bound; the bit-identical count is pinned at the measured 0.
    const Image d = dust_from_balmer(golden_image("recon_ha"), golden_image("recon_hb"));
    gdg::test::check_golden_image(d, "recon_dust_balmer", 1e-6, 0);
    size_t nonzero = 0;
    for (double v : d.data) nonzero += v > 0.0 ? 1 : 0;
    CHECK(nonzero > 1000);  // the golden exercises the exp path, not just the gate's zeros
}

TEST_CASE("dust_defaults_verbatim", "[recon][extinction]") {
    CHECK(CASE_B_HA_HB == 2.86);
    CHECK(DUST_LANE_MAX_WIDTH_PX == 15);
    CHECK(DUST_SKY_PERCENTILE == 5.0);
    CHECK(DUST_BRIGHT_PERCENTILE == 99.0);
    CHECK(DUST_MIN_SURROUND_FRAC == 0.05);
    CHECK(BALMER_SATURATION == 3.0);
}

TEST_CASE("dark_lane_reads_high_opacity", "[recon][extinction]") {
    const Image lum = lane_field();
    const Image dust = dust_from_structure(lum);
    REQUIRE(dust.width == lum.width);
    REQUIRE(dust.height == lum.height);
    check_opacity_plane(dust);
    CHECK(region_mean(dust, 40, 56, 51, 55) > 0.6);   // lane embedded in bright surroundings
    CHECK(region_mean(dust, 0, 20, 0, 10) < 0.02);    // plain sky corner is not dust
    CHECK(region_max(dust, 0, 96, 0, 12) < 0.05);     // nor the sky/field boundary region
    CHECK(region_mean(dust, 10, 20, 70, 90) < 0.05);  // bright field itself
}

TEST_CASE("dust_from_structure_flat_sky_is_zero", "[recon][extinction]") {
    CHECK(region_max(dust_from_structure(Image(32, 32, 1, f32(0.01))), 0, 32, 0, 32) < 1e-3);
}

TEST_CASE("balmer_decrement_tracks_extinction", "[recon][extinction]") {
    Image hb(32, 32, 1, 1.0), ha(32, 32, 1, f32(2.86));
    for (int y = 0; y < 32; ++y)
        for (int x = 16; x < 32; ++x) ha.at(x, y) = f32(2.86 * 3.0);
    const Image d = dust_from_balmer(ha, hb);
    check_opacity_plane(d);
    CHECK(region_max(d, 0, 32, 0, 16) < 0.01);
    CHECK(region_mean(d, 0, 32, 16, 32) > 0.3);
    Image ha2 = ha;
    for (int y = 0; y < 32; ++y)
        for (int x = 16; x < 32; ++x) ha2.at(x, y) = 2.0;  // below Case B -> ~0
    CHECK(region_max(dust_from_balmer(ha2, hb), 0, 32, 0, 32) < 0.01);
}

TEST_CASE("dust_from_structure_noisy_sky_stays_clean", "[recon][extinction]") {
    // numpy's PCG64 normal stream replaced by std::mt19937_64 (the properties do not depend on it).
    std::mt19937_64 rng(0);
    std::normal_distribution<double> nd(0.0, 0.01);
    Image lum = lane_field();
    for (double& v : lum.data) v = f32(v + f32(nd(rng)));
    const Image dust = dust_from_structure(lum);
    CHECK(region_mean(dust, 0, 96, 0, 12) < 0.05);
    CHECK(region_max(dust, 0, 96, 0, 12) < 0.2);
    CHECK(region_max(dust, 0, 20, 0, 10) < 0.2);
    CHECK(region_mean(dust, 40, 56, 51, 55) > 0.6);
}

TEST_CASE("dust_from_balmer_noisy_sky_gated", "[recon][extinction]") {
    std::mt19937_64 rng(1);
    std::normal_distribution<double> nd(0.0, 0.01);
    Image hb(64, 64), ha(64, 64);
    for (double& v : hb.data) v = 0.02 + nd(rng);
    for (double& v : ha.data) v = 0.02 + nd(rng);
    for (int y = 0; y < 64; ++y)
        for (int x = 32; x < 64; ++x) {
            hb.at(x, y) += 1.0;  // genuine signal region, reddened
            ha.at(x, y) += 2.86 * 3.0;
        }
    const Image d = dust_from_balmer(ha, hb);
    CHECK(region_max(d, 0, 64, 0, 24) < 0.05);   // noisy sky: no spurious dust
    CHECK(region_mean(d, 0, 64, 36, 64) > 0.3);  // real high-ratio signal still reads high
}

TEST_CASE("balmer_flat_field_is_not_gated", "[recon][extinction]") {
    // A flat field has no distinguishable sky: the gate is 1, so the decrement alone decides.
    const Image hb(16, 16, 1, 1.0), ha(16, 16, 1, 2.86 * 4.0);
    const Image d = dust_from_balmer(ha, hb);
    const double want = f32(1.0 - std::exp(-((2.86 * 4.0 / (1.0 + 1e-6) / 2.86 - 1.0) / 3.0)));
    for (double v : d.data) CHECK(v == Catch::Approx(want).margin(1e-7));
}

TEST_CASE("dust_rejects_bad_input", "[recon][extinction]") {
    CHECK_THROWS_AS(dust_from_structure(Image(8, 8, 3)), std::invalid_argument);
    CHECK_THROWS_AS(dust_from_balmer(Image(8, 8), Image(8, 9)), std::invalid_argument);
    CHECK_THROWS_AS(dust_from_balmer(Image(8, 8, 3), Image(8, 8, 3)), std::invalid_argument);
}
