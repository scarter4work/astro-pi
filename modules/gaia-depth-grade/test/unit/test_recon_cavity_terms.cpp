// Task 18 (recon plan task 7): the cavity depth terms of cavity.py against the synthetic_blister
// goldens (default and non-default arguments), plus the edge cases of
// gaia-depth-grade/tests/recon/test_cavity_terms.py.
#include <catch_amalgamated.hpp>

#include "ReconGolden.h"
#include "core/recon/Cavity.h"
#include "core/recon/Ionization.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

using namespace gdg::recon;
using gdg::Image;
using gdg::test::check_golden_image;
using gdg::test::crop;
using gdg::test::golden_image;
using gdg::test::golden_mask;

namespace {

/// make_goldens.py: L = clip(nan_to_num(float64(lum)), 0, None).
Image blister_L() {
    Image L = golden_image("recon_lum");
    for (double& v : L.data) v = std::max(nan_to_num(v), 0.0);
    return L;
}

Mask crop_mask(const Mask& m, int y0, int y1, int x0, int x1) {
    Mask out(x1 - x0, y1 - y0);
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) out.data[(y - y0) * out.width + (x - x0)] = m.data[y * m.width + x];
    return out;
}

// stage.json recon_crop
constexpr int CY0 = 40, CY1 = 110, CX0 = 10, CX1 = 100;

}  // namespace

TEST_CASE("cavity_terms_defaults_verbatim", "[recon][cavity]") {
    // stage.json recon_terms and the cavity.py module constants
    CHECK(GEOMETRY_POWER == 2.0);
    CHECK(IONIZATION_GAMMA == 1.0);
    CHECK(EM_SIGMA_SMALL_PX == 1.0);
    CHECK(EM_SIGMA_LARGE_PX == 4.0);
    CHECK(EM_GAIN == 0.5);
    CHECK(EM_NORM_PCT == 99.5);
    CHECK(EM_EPS == 1e-12);
    CHECK(FOOTPRINT_SKY_PCT == 10.0);
    CHECK(FOOTPRINT_PEAK_PCT == 99.5);
    CHECK(EMISSIVITY_PEAK_PCT == 99.9);
    CHECK(RECON_EPS == 1e-12);
}

TEST_CASE("footprint_matches_golden", "[recon][cavity][golden]") {
    const Mask got = footprint_from_lum(blister_L(), 0.05);
    const Mask want = golden_mask("recon_footprint");
    REQUIRE(got.width == want.width);
    REQUIRE(got.height == want.height);
    CHECK(got.data == want.data);
    CHECK(got.count() == 7845u);  // stage.json recon_cavity meta footprint_px
    CHECK(crop_mask(want, CY0, CY1, CX0, CX1).data == golden_mask("recon_crop_footprint").data);
}

TEST_CASE("geometry_prior_matches_golden", "[recon][cavity][golden]") {
    // hypot is libm on both sides; power 2 is numpy's square fast path (x * x). Power 3 goes through
    // np.power (numpy may use a SIMD pow): bounded by the plan's 1e-6, measured bit-identical.
    const Mask foot = golden_mask("recon_footprint");
    check_golden_image(geometry_prior(140, 140, {50.0, 86.0}, &foot), "recon_geometry_prior", 0.0, 0);
    const Mask sub = golden_mask("recon_crop_footprint");
    check_golden_image(geometry_prior(70, 90, {40.0, 46.0}, &sub, 3.0), "recon_crop_geometry_p3", 1e-6, 0);
    check_golden_image(geometry_prior(70, 90, {40.0, 46.0}, nullptr), "recon_crop_geometry_nomask", 0.0, 0);
}

TEST_CASE("ionization_depth_matches_golden", "[recon][cavity][golden]") {
    const IonizationMap imap = ionization_ratio(gdg::test::blister_channels());
    check_golden_image(ionization_depth(imap), "recon_ionization_depth", 0.0, 0);
    const IonizationMap sub{crop(imap.oiii_fraction, CY0, CY1, CX0, CX1), imap.confidence};
    // gamma 1.5 goes through np.power: plan bound 1e-6, measured bit-identical.
    check_golden_image(ionization_depth(sub, 1.5), "recon_crop_ionization_depth_g1p5", 1e-6, 0);
}

TEST_CASE("emission_relief_matches_golden", "[recon][cavity][golden]") {
    const Image L = blister_L();
    check_golden_image(emission_measure_relief(L), "recon_emission_relief", 0.0, 0);
    check_golden_image(emission_measure_relief(crop(L, CY0, CY1, CX0, CX1), 1.0, 4.0, 1.0),
                       "recon_crop_emission_relief_g1", 0.0, 0);
}

TEST_CASE("geometry_prior_is_concave_about_apex", "[recon][cavity]") {
    const int n = 101;
    Mask foot(n, n);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) foot.data[y * n + x] = std::hypot(x - 50.0, y - 50.0) <= 40.0;
    const Image z = geometry_prior(n, n, {50.0, 50.0}, &foot);
    REQUIRE(z.width == n);
    double zmax = -1, zsum = 0;
    size_t cnt = 0;
    for (int i = 0; i < n * n; ++i) {
        REQUIRE(std::isfinite(z.data[i]));
        REQUIRE(z.data[i] >= 0.0);
        REQUIRE(z.data[i] <= 1.0);
        if (foot.data[i]) {
            zmax = std::max(zmax, z.data[i]);
            zsum += z.data[i];
            ++cnt;
        }
    }
    CHECK(z.at(50, 50) == zmax);
    CHECK(z.at(50, 50) > zsum / cnt);
    std::vector<double> prof;
    for (int x = 50; x < 91; ++x) prof.push_back(z.at(x, 50));
    for (size_t i = 1; i < prof.size(); ++i) CHECK(prof[i] - prof[i - 1] <= 1e-6);
    std::vector<double> d2;
    for (size_t i = 2; i < prof.size(); ++i) d2.push_back(prof[i] - 2 * prof[i - 1] + prof[i - 2]);
    std::sort(d2.begin(), d2.end());
    CHECK(d2[d2.size() / 2] < 0.0);  // concave
    CHECK(z.at(90, 50) < 0.1);
    CHECK(z.at(0, 0) == 0.0);
}

TEST_CASE("geometry_prior_edges", "[recon][cavity]") {
    Mask wrong(5, 4);
    CHECK_THROWS_AS(geometry_prior(5, 5, {1.0, 1.0}, &wrong), std::invalid_argument);
    // an all-False mask: R over every pixel, then every pixel is sky (0)
    const Mask none(5, 5);
    for (double v : geometry_prior(5, 5, {2.0, 2.0}, &none).data) CHECK(v == 0.0);
    // R <= 0 (one pixel at the apex) -> R = 1, z = 1
    CHECK(geometry_prior(1, 1, {0.0, 0.0}).data == std::vector<double>{1.0});
    // apex outside the image is allowed; values stay in [0, 1]
    for (double v : geometry_prior(6, 7, {-10.0, 30.0}).data) {
        CHECK(v >= 0.0);
        CHECK(v <= 1.0);
    }
    // no mask: R is the farthest pixel, which reads 0, and the apex 1
    const Image z = geometry_prior(3, 3, {0.0, 0.0});
    CHECK(z.at(0, 0) == 1.0);
    CHECK(z.at(2, 2) == 0.0);
    CHECK(z.at(1, 0) == f32(1.0 - 1.0 / 8.0));  // (1 / sqrt 8)^2
}

TEST_CASE("ionization_depth_places_oiii_forward", "[recon][cavity]") {
    Image f(16, 16);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) f.at(x, y) = x < 8 ? f32(0.9) : f32(0.1);
    const Image z = ionization_depth({f, 0.8});
    double lo = 0, hi = 0;
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            REQUIRE(z.at(x, y) >= 0.0);
            REQUIRE(z.at(x, y) <= 1.0);
            (x < 8 ? lo : hi) += z.at(x, y);
        }
    CHECK(lo < hi);
    // NaN and out-of-range fractions are cleaned: NaN -> 0 -> z 1; 1.5 -> 1 -> z 0; -1 -> 0 -> z 1
    Image g(3, 1);
    g.data = {std::nan(""), 1.5, -1.0};
    CHECK(ionization_depth({g, 1.0}).data == std::vector<double>{1.0, 0.0, 1.0});
    CHECK(ionization_depth({g, 2.0}).data == std::vector<double>{1.0, 0.0, 1.0});
}

TEST_CASE("emission_measure_relief_bulges_bright_edges", "[recon][cavity]") {
    const int n = 64;
    Image lum(n, n);
    std::vector<int> ring;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const double r = std::hypot(x - 32.0, y - 32.0);
            if (r >= 18 && r < 20) {
                lum.at(x, y) = 1.0;
                ring.push_back(y * n + x);
            }
        }
    const Image z = emission_measure_relief(lum);
    for (double v : z.data) REQUIRE(std::isfinite(v));
    CHECK(std::fabs(z.at(0, 0)) < 1e-3);
    double ring_max = 0;
    for (int i : ring) ring_max = std::max(ring_max, std::fabs(z.data[i]));
    CHECK(ring_max > 0.01);
    bool pos = false, neg = false;
    for (double v : z.data) {
        pos = pos || v > 0.0;
        neg = neg || v < 0.0;
        CHECK(std::fabs(v) <= 0.5);  // clipped to [-gain, gain]
    }
    CHECK(pos);
    CHECK(neg);
}

TEST_CASE("emission_measure_relief_flat_sky_is_zero", "[recon][cavity]") {
    const Image z = emission_measure_relief(Image(32, 32, 1, f32(0.3)));
    for (double v : z.data) CHECK(std::fabs(v) < 1e-3);
    // all-zero input: scale 0 <= eps -> exact zeros
    for (double v : emission_measure_relief(Image(8, 8)).data) CHECK(v == 0.0);
    CHECK_THROWS_AS(emission_measure_relief(Image(8, 8, 3)), std::invalid_argument);
}

TEST_CASE("footprint_from_flat_lum_is_everything", "[recon][cavity]") {
    const Mask m = footprint_from_lum(Image(6, 4, 1, 0.2), 0.05);
    CHECK(m.count() == 24u);
}
