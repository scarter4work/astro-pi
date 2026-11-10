// Task 3: scipy-exact gaussian_filter against the Task 1 goldens (scipy 1.18.0) and hand-derived
// reflect-boundary answers.
#include <catch_amalgamated.hpp>

#include "Golden.h"
#include "core/Numerics.h"

#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

using gdg::Image;
using gdg::test::golden_image;
using gdg::test::max_abs_diff;

namespace {

// gauss_<sigma> goldens: scipy.ndimage.gaussian_filter(prim_input, sigma), prim_input 48x64.
double golden_gauss_error(double sigma, const std::string& name) {
    const Image in = golden_image("prim_input");
    const Image want = golden_image(name);
    REQUIRE(in.width == 64);
    REQUIRE(in.height == 48);
    const Image got = gdg::gaussian_filter(in, sigma);
    REQUIRE(got.same_shape(want));
    const double err = max_abs_diff(got.data, want.data);
    UNSCOPED_INFO("sigma " << sigma << " max |delta| " << err);
    return err;
}

}  // namespace

TEST_CASE("pairwise_sum reproduces np.sum", "[gaussian][sum]") {
    // np.sum(1/(arange(1000)+1)) and np.sum(sin(arange(37)*0.7)), numpy 2.5.0. The n=1000 value
    // differs from a sequential sum (0x1.df11f45f4e618p+2), so this pins the pairwise tree.
    std::vector<double> a(1000), b(37);
    for (int i = 0; i < 1000; ++i) a[i] = 1.0 / (static_cast<double>(i) + 1.0);
    for (int i = 0; i < 37; ++i) b[i] = std::sin(static_cast<double>(i) * 0.7);
    CHECK(gdg::pairwise_sum(a) == 0x1.df11f45f4e61ap+2);
    CHECK(gdg::pairwise_sum(b) == 0x1.2ca7be6c9a9d4p-5);
    CHECK(gdg::pairwise_sum(nullptr, 0) == 0.0);
    const double three[3] = {0.25, 0.5, 1.0};
    CHECK(gdg::pairwise_sum(three, 3) == 1.75);
}

TEST_CASE("reflect_index is scipy's half-sample symmetric extension", "[gaussian][reflect]") {
    // n = 4: ... d c b a | a b c d | d c b a | a b ...
    const long n = 4;
    const long want[] = {1, 0, 0, 1, 2, 3, 3, 2, 1, 0, 0, 1, 2, 3};  // i = -2 .. 11
    for (long i = -2; i <= 11; ++i) CHECK(gdg::reflect_index(i, n) == want[i + 2]);
    CHECK(gdg::reflect_index(-7, 1) == 0);
    CHECK(gdg::reflect_index(5, 1) == 0);
}

TEST_CASE("gaussian kernel: radius and weights", "[gaussian][kernel]") {
    CHECK(gdg::gaussian_radius(0.8) == 3);
    CHECK(gdg::gaussian_radius(1.0) == 4);
    CHECK(gdg::gaussian_radius(2.5) == 10);
    CHECK(gdg::gaussian_radius(65.0) == 260);
    CHECK(gdg::gaussian_radius(0.1) == 0);
    // sigma 0.5, radius 2: weights exp(-2 k^2) / Z.
    const auto k = gdg::gaussian_kernel1d(0.5, 2);
    REQUIRE(k.size() == 5);
    const double z = 1.0 + 2.0 * std::exp(-2.0) + 2.0 * std::exp(-8.0);
    CHECK(k[2] == Catch::Approx(1.0 / z).epsilon(1e-15));
    CHECK(k[1] == Catch::Approx(std::exp(-2.0) / z).epsilon(1e-15));
    CHECK(k[0] == Catch::Approx(std::exp(-8.0) / z).epsilon(1e-15));
    CHECK(k[0] == k[4]);
    CHECK(k[1] == k[3]);
}

TEST_CASE("gauss_matches_scipy_0.8", "[gaussian][golden]") { CHECK(golden_gauss_error(0.8, "gauss_0p8") <= 1e-12); }
TEST_CASE("gauss_matches_scipy_1", "[gaussian][golden]") { CHECK(golden_gauss_error(1.0, "gauss_1p0") <= 1e-12); }
TEST_CASE("gauss_matches_scipy_2", "[gaussian][golden]") { CHECK(golden_gauss_error(2.0, "gauss_2p0") <= 1e-12); }
TEST_CASE("gauss_matches_scipy_2.5", "[gaussian][golden]") { CHECK(golden_gauss_error(2.5, "gauss_2p5") <= 1e-12); }
// sigma 4 on the 64x48 field: the 16 px kernel reaches past every edge.
TEST_CASE("gauss_matches_scipy_4", "[gaussian][golden]") { CHECK(golden_gauss_error(4.0, "gauss_4p0") <= 1e-12); }
// sigma 65: the 260 px radius is longer than both axes, so the reflection wraps several times.
TEST_CASE("gauss_matches_scipy_65", "[gaussian][golden]") { CHECK(golden_gauss_error(65.0, "gauss_65p0") <= 1e-9); }

TEST_CASE("gauss_odd_tiny_image", "[gaussian][reflect]") {
    // A 2x1 image [1, 0] at sigma 2 (radius 8, four times the axis). The reflect extension has
    // period 4 (x0 x1 x1 x0), so pixel 0 collects every offset k with k mod 4 in {0, 3}:
    //   out0 = (w0 + w1 + w3 + 2 w4 + w5 + w7 + 2 w8) / Z,  out1 = 1 - out0,
    // with w_k = exp(-k^2/8) and Z = w0 + 2 sum_{k=1..8} w_k. The y axis (one row) is a pure
    // reflection of one value, so it only rescales by sum(w) = 1.
    Image in(2, 1, 1);
    in.at(0, 0) = 1.0;
    in.at(1, 0) = 0.0;
    double w[9], z = 0.0;
    for (int k = 0; k <= 8; ++k) {
        w[k] = std::exp(-0.125 * k * k);
        z += (k == 0 ? 1.0 : 2.0) * w[k];
    }
    const double out0 = (w[0] + w[1] + w[3] + 2 * w[4] + w[5] + w[7] + 2 * w[8]) / z;
    const Image got = gdg::gaussian_filter(in, 2.0);
    REQUIRE(got.same_shape(in));
    CHECK(got.at(0, 0) == Catch::Approx(out0).margin(1e-15));
    CHECK(got.at(1, 0) == Catch::Approx(1.0 - out0).margin(1e-15));

    // A 3x1 line [1, 0, 0] at sigma 0.5 (radius 2): reflect puts x0 at offsets -1 of pixel 0.
    Image line(3, 1, 1);
    line.at(0, 0) = 1.0;
    const double e1 = std::exp(-2.0), e2 = std::exp(-8.0), zz = 1.0 + 2.0 * e1 + 2.0 * e2;
    const Image g2 = gdg::gaussian_filter(line, 0.5);
    CHECK(g2.at(0, 0) == Catch::Approx((1.0 + e1) / zz).margin(1e-15));
    CHECK(g2.at(1, 0) == Catch::Approx((e1 + e2) / zz).margin(1e-15));
    CHECK(g2.at(2, 0) == Catch::Approx(e2 / zz).margin(1e-15));

    // One pixel: every reflection is the pixel itself.
    Image one(1, 1, 1, 0.375);
    CHECK(gdg::gaussian_filter(one, 3.0).at(0, 0) == Catch::Approx(0.375).margin(1e-15));
}

TEST_CASE("gauss_colour_is_per_channel", "[gaussian][golden]") {
    // gaussian_filter(prim_stack3, sigma=2, axes=(0, 1)) on a 17x20x3 stack.
    const Image in = golden_image("prim_stack3");
    const Image want = golden_image("gauss_axes01_2");
    REQUIRE(in.channels == 3);
    const Image got = gdg::gaussian_filter(in, 2.0);
    REQUIRE(got.same_shape(want));
    const double err = max_abs_diff(got.data, want.data);
    UNSCOPED_INFO("axes01 max |delta| " << err);
    CHECK(err <= 1e-12);

    // Each channel equals the mono filter of that channel alone, bit for bit.
    for (int c = 0; c < 3; ++c) {
        Image mono(in.width, in.height, 1);
        for (int y = 0; y < in.height; ++y)
            for (int x = 0; x < in.width; ++x) mono.at(x, y) = in.at(x, y, c);
        const Image fm = gdg::gaussian_filter(mono, 2.0);
        size_t unequal = 0;
        for (int y = 0; y < in.height; ++y)
            for (int x = 0; x < in.width; ++x) unequal += fm.at(x, y) != got.at(x, y, c);
        CHECK(unequal == 0);
    }
}

TEST_CASE("gauss_parallel_path_matches_serial", "[gaussian]") {
    // 600x500 at sigma 3 crosses the parallel threshold; 6x5 crops of the same data stay serial.
    // Both paths run the same per-line arithmetic, so a large image must equal the scipy-order
    // result regardless of how lines are scheduled: compare against a second run.
    Image big(600, 500, 1);
    for (size_t i = 0; i < big.data.size(); ++i) big.data[i] = std::sin(0.37 * static_cast<double>(i));
    const Image a = gdg::gaussian_filter(big, 3.0);
    const Image b = gdg::gaussian_filter(big, 3.0);
    CHECK(gdg::test::count_unequal(a.data, b.data) == 0);
    // Pixel (300, 250) by brute force: correlate along y, then x, scipy's symmetric loop order.
    const int r = gdg::gaussian_radius(3.0);
    const auto w = gdg::gaussian_kernel1d(3.0, r);
    auto ycol = [&](int x) {
        const int y = 250;
        double o = big.at(x, y) * w[r];
        for (int j = -r; j < 0; ++j) o += (big.at(x, y + j) + big.at(x, y - j)) * w[r + j];
        return o;
    };
    double o = ycol(300) * w[r];
    for (int j = -r; j < 0; ++j) o += (ycol(300 + j) + ycol(300 - j)) * w[r + j];
    CHECK(a.at(300, 250) == o);
}

TEST_CASE("gauss_sigma_edge_cases", "[gaussian]") {
    Image in(4, 3, 2, 0.5);
    in.at(1, 1, 1) = 2.0;
    const Image same = gdg::gaussian_filter(in, 0.0);
    CHECK(gdg::test::count_unequal(same.data, in.data) == 0);
    CHECK_THROWS_AS(gdg::gaussian_filter(in, -1.0), std::invalid_argument);
    CHECK_THROWS_AS(gdg::gaussian_filter(in, std::nan("")), std::invalid_argument);
    CHECK_THROWS_AS(gdg::gaussian_filter(in, INFINITY), std::invalid_argument);
    const Image empty;
    CHECK(gdg::gaussian_filter(empty, 2.0).data.empty());
}
