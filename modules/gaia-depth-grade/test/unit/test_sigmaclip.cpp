// Task 5: astropy 8.0 sigma_clipped_stats against the Task 1 goldens and astropy-computed
// literals for hand-built sets.
#include <catch_amalgamated.hpp>

#include "Golden.h"
#include "core/SigmaClip.h"

#include <cmath>
#include <limits>
#include <string>
#include <vector>

using gdg::test::golden;
using gdg::test::golden_image;

namespace {

// lum exactly as the generator builds it: stars.mean(axis=2) for colour (numpy reduces the three
// channels as ((r + g) + b) / 3), the image itself for mono.
std::vector<double> lum_of(const std::string& name) {
    const gdg::Image s = golden_image(name);
    if (s.channels == 1) return s.data;
    std::vector<double> lum(s.pixels());
    for (size_t i = 0; i < lum.size(); ++i) {
        const double* p = s.data.data() + i * 3;
        lum[i] = ((p[0] + p[1]) + p[2]) / 3.0;
    }
    return lum;
}

void check_golden(const std::string& stars, const std::string& want_name) {
    const auto want = golden(want_name).data;  // [mean, median, std], sigma 3
    REQUIRE(want.size() == 3);
    const auto lum = lum_of(stars);
    REQUIRE(lum.size() == 144u * 192u);
    const gdg::ClipStats got = gdg::sigma_clipped_stats(lum, 3.0);
    UNSCOPED_INFO(stars << ": |d mean| " << std::fabs(got.mean - want[0]) << " |d median| "
                        << std::fabs(got.median - want[1]) << " |d std| " << std::fabs(got.std - want[2])
                        << " iterations " << got.iterations);
    // Plan tolerance is 1e-12; the port is bit-exact (pairwise sums, exact median, IEEE sqrt).
    CHECK(got.mean == want[0]);
    CHECK(got.median == want[1]);
    CHECK(got.std == want[2]);
    CHECK(got.nonfinite == 0);
    CHECK(got.iterations >= 1);
}

}  // namespace

TEST_CASE("sigma_clip_matches_astropy", "[sigmaclip][golden]") {
    check_golden("stars_rgb", "sigma_clip");
    check_golden("mono_stars", "mono_sigma_clip");
}

TEST_CASE("sigma_clip_converges_early", "[sigmaclip]") {
    // 1..5: median 3, std sqrt(2), bounds 3 -/+ 4.24 keep everything -> one pass.
    const auto a = gdg::sigma_clipped_stats({1.0, 2.0, 3.0, 4.0, 5.0});
    CHECK(a.iterations == 1);
    CHECK(a.mean == 3.0);
    CHECK(a.median == 3.0);
    CHECK(a.std == 0x1.6a09e667f3bcdp+0);  // astropy: sqrt(2)

    // Ten 1s, nine 2s and 50. Pass 1: median 1.5, std 10.587 -> 50 clipped. Pass 2: median 1,
    // std 0.4993 keeps all 19 -> stop after 2 of 5 allowed passes. mean 28/19, std sqrt(90)/19.
    std::vector<double> w(10, 1.0);
    w.insert(w.end(), 9, 2.0);
    w.push_back(50.0);
    const auto b = gdg::sigma_clipped_stats(w);
    CHECK(b.iterations == 2);
    CHECK(b.mean == 0x1.79435e50d7943p+0);   // astropy literal (= 28/19)
    CHECK(b.median == 1.0);
    CHECK(b.std == 0x1.ff4a557d0735ap-2);    // astropy literal (sqrt(90)/19 to 1 ulp)
    CHECK(b.mean == 28.0 / 19.0);
    CHECK(b.std == Catch::Approx(std::sqrt(90.0) / 19.0).epsilon(1e-15));

    // All equal: std 0, bounds equal the value, which stays (lo <= x <= hi is inclusive).
    const auto c = gdg::sigma_clipped_stats({0.25, 0.25, 0.25, 0.25});
    CHECK(c.iterations == 1);
    CHECK(c.mean == 0.25);
    CHECK(c.median == 0.25);
    CHECK(c.std == 0.0);
}

TEST_CASE("sigma_clip_maxiters", "[sigmaclip]") {
    // 30 zeros, six 1s and 2, 4, 8, 16, 40: one value goes per pass. astropy 8.0 literals.
    std::vector<double> v(30, 0.0);
    v.insert(v.end(), 6, 1.0);
    for (double x : {2.0, 4.0, 8.0, 16.0, 40.0}) v.push_back(x);
    const auto m1 = gdg::sigma_clipped_stats(v, 3.0, 1);
    CHECK(m1.iterations == 1);
    CHECK(m1.mean == 0x1.ccccccccccccdp-1);
    CHECK(m1.median == 0.0);
    CHECK(m1.std == 0x1.6666666666666p+1);
    const auto m2 = gdg::sigma_clipped_stats(v, 3.0, 2);
    CHECK(m2.iterations == 2);
    CHECK(m2.mean == 0x1.0690690690690p-1);
    CHECK(m2.std == 0x1.6e101133fedd4p+0);
    const auto m5 = gdg::sigma_clipped_stats(v, 3.0, 5);
    CHECK(m5.iterations == 5);
    CHECK(m5.mean == 0x1.5555555555555p-3);
    CHECK(m5.std == 0x1.7d9f4cf754636p-2);
    // maxiters 0 = no limit: one more pass that changes nothing (astropy: 6 iterations).
    const auto m0 = gdg::sigma_clipped_stats(v, 3.0, 0);
    CHECK(m0.iterations == 6);
    CHECK(m0.mean == m5.mean);
    CHECK(m0.std == m5.std);
    // Negative maxiters: no pass, stats of the input.
    const auto mn = gdg::sigma_clipped_stats({1.0, 2.0, 30.0}, 3.0, -1);
    CHECK(mn.iterations == 0);
    CHECK(mn.mean == 11.0);
}

TEST_CASE("sigma_clip_nonfinite_and_empty", "[sigmaclip]") {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    // astropy drops NaN/inf first (with a warning): stats of {1, 2, 3}.
    const auto a = gdg::sigma_clipped_stats({1.0, nan, 2.0, inf, 3.0});
    CHECK(a.nonfinite == 2);
    CHECK(a.mean == 2.0);
    CHECK(a.median == 2.0);
    CHECK(a.std == Catch::Approx(0.816496580927726).epsilon(1e-15));
    const auto e = gdg::sigma_clipped_stats({nan, -inf});
    CHECK(e.nonfinite == 2);
    CHECK(std::isnan(e.mean));
    CHECK(std::isnan(e.median));
    CHECK(std::isnan(e.std));
    const auto z = gdg::sigma_clipped_stats({});
    CHECK(std::isnan(z.mean));
}
