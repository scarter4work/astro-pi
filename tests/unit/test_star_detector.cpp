#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <vector>
#include <algorithm>
#include <numeric>
#include "engine/StarDetector.h"

TEST_CASE("computeBackground returns median and MAD", "[star_detector]") {
    // 50 values of 0.1, 50 values of 0.9 => median = 0.5, all |x_i - median| = 0.4
    std::vector<float> image(100, 0.1f);
    for (int i = 50; i < 100; ++i) image[i] = 0.9f;
    auto [median, mad] = nukex::computeBackground(image.data(), image.size());
    REQUIRE(median == Catch::Approx(0.5).margin(0.01));
    REQUIRE(mad == Catch::Approx(0.4).margin(0.01));
}

TEST_CASE("computeBackground handles constant data", "[star_detector]") {
    std::vector<float> image(50, 0.5f);
    auto [median, mad] = nukex::computeBackground(image.data(), image.size());
    REQUIRE(median == Catch::Approx(0.5));
    REQUIRE(mad == Catch::Approx(0.0));
}
