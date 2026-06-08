#include "catch_amalgamated.hpp"
#include "nukex/stretch/image_stats.hpp"
#include "nukex/io/image.hpp"

#include <cmath>

using namespace nukex;

namespace {
Image single_value_image(int w, int h, int c, float v) {
    Image im(w, h, c);
    float* d = im.data();
    for (int i = 0; i < w * h * c; ++i) d[i] = v;
    return im;
}
} // namespace

TEST_CASE("compute_image_stats: all-zero mono image", "[stretch][image_stats]") {
    auto im = single_value_image(16, 16, 1, 0.0f);
    auto s = compute_image_stats(im);
    REQUIRE(s.median[0] == Catch::Approx(0.0));
    REQUIRE(s.mad[0]    == Catch::Approx(0.0));
    REQUIRE(s.p95[0]    == Catch::Approx(0.0));
    REQUIRE(std::isnan(s.median[1]));
    REQUIRE(s.sat_frac[0] == Catch::Approx(0.0));
}

TEST_CASE("compute_image_stats: all-one mono image saturates", "[stretch][image_stats]") {
    auto im = single_value_image(16, 16, 1, 1.0f);
    auto s = compute_image_stats(im, 0.0, 0, 0.95f);
    REQUIRE(s.median[0]   == Catch::Approx(1.0));
    REQUIRE(s.sat_frac[0] == Catch::Approx(1.0));  // everything >= 0.95
    REQUIRE(s.p999[0]     == Catch::Approx(1.0));
}

TEST_CASE("compute_image_stats: known-percentile mono image", "[stretch][image_stats]") {
    // Ramp 0..255 -> normalize -> p50 = 127.5/255 ~= 0.5, p95 ~= 0.95, etc.
    Image im(256, 1, 1);
    float* d = im.data();
    for (int i = 0; i < 256; ++i) d[i] = static_cast<float>(i) / 255.0f;
    auto s = compute_image_stats(im);
    REQUIRE(s.p50[0] == Catch::Approx(0.5f).margin(0.01));
    REQUIRE(s.p95[0] == Catch::Approx(0.95f).margin(0.01));
    REQUIRE(s.p99[0] == Catch::Approx(0.99f).margin(0.01));
}

TEST_CASE("compute_image_stats: three-channel color ratios", "[stretch][image_stats]") {
    Image im(8, 8, 3);
    float* d = im.data();
    const int n = 64;
    // Channel 0 (R) = 0.6, channel 1 (G) = 0.3, channel 2 (B) = 0.9
    for (int i = 0; i < n; ++i) d[i]       = 0.6f;
    for (int i = 0; i < n; ++i) d[n + i]   = 0.3f;
    for (int i = 0; i < n; ++i) d[2*n + i] = 0.9f;

    auto s = compute_image_stats(im);
    REQUIRE(s.color_rg == Catch::Approx(2.0));  // 0.6/0.3
    REQUIRE(s.color_bg == Catch::Approx(3.0));  // 0.9/0.3
}

TEST_CASE("to_feature_row: mono fills index 0 and NaN for others",
          "[stretch][image_stats]") {
    auto im = single_value_image(4, 4, 1, 0.5f);
    auto s = compute_image_stats(im);
    auto row = s.to_feature_row();
    REQUIRE(row[0] == Catch::Approx(0.5));
    REQUIRE(std::isnan(row[1]));
    REQUIRE(std::isnan(row[2]));
}
