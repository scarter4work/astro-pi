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

TEST_CASE("extractBlobs finds connected bright regions", "[star_detector]") {
    // 10x10 image, background 0.1, one 3x3 bright blob centered at (5,5)
    std::vector<float> image(100, 0.1f);
    image[4*10 + 4] = 0.9f; image[4*10 + 5] = 0.95f; image[4*10 + 6] = 0.9f;
    image[5*10 + 4] = 0.95f; image[5*10 + 5] = 1.0f; image[5*10 + 6] = 0.95f;
    image[6*10 + 4] = 0.9f; image[6*10 + 5] = 0.95f; image[6*10 + 6] = 0.9f;

    double threshold = 0.5;
    auto blobs = nukex::extractBlobs(image.data(), 10, 10, threshold);
    REQUIRE(blobs.size() == 1);
    REQUIRE(blobs[0].size() == 9);
}

TEST_CASE("extractBlobs finds multiple separated blobs", "[star_detector]") {
    // 20x10 image with two blobs far apart
    std::vector<float> image(200, 0.1f);
    image[2*20 + 2] = 1.0f; image[2*20 + 3] = 0.8f; image[3*20 + 2] = 0.8f;
    image[7*20 + 17] = 1.0f; image[7*20 + 18] = 0.8f; image[8*20 + 17] = 0.8f;

    auto blobs = nukex::extractBlobs(image.data(), 20, 10, 0.5);
    REQUIRE(blobs.size() == 2);
}

TEST_CASE("extractBlobs returns empty for uniform image", "[star_detector]") {
    std::vector<float> image(100, 0.3f);
    auto blobs = nukex::extractBlobs(image.data(), 10, 10, 0.5);
    REQUIRE(blobs.empty());
}
