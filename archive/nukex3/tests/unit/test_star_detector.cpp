#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <vector>
#include <algorithm>
#include <numeric>
#include <cmath>
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

TEST_CASE("blobToStar computes intensity-weighted centroid", "[star_detector]") {
    std::vector<float> image(100, 0.0f);
    image[5*10 + 5] = 1.0f;
    image[4*10 + 5] = 0.5f;
    image[6*10 + 5] = 0.5f;
    image[5*10 + 4] = 0.5f;
    image[5*10 + 6] = 0.5f;

    nukex::Blob blob = {{5,5}, {5,4}, {5,6}, {4,5}, {6,5}};
    auto star = nukex::blobToStar(blob, image.data(), 10, 10);
    REQUIRE(star.has_value());
    REQUIRE(star->x == Catch::Approx(5.0).margin(0.01));
    REQUIRE(star->y == Catch::Approx(5.0).margin(0.01));
    REQUIRE(star->flux == Catch::Approx(3.0).margin(0.01));
}

TEST_CASE("blobToStar rejects too-small blobs", "[star_detector]") {
    std::vector<float> image(100, 0.0f);
    image[5*10 + 5] = 1.0f;
    nukex::Blob blob = {{5,5}};
    auto star = nukex::blobToStar(blob, image.data(), 10, 10, 3);
    REQUIRE(!star.has_value());
}

TEST_CASE("blobToStar rejects too-large blobs", "[star_detector]") {
    std::vector<float> image(10000, 1.0f);
    nukex::Blob blob;
    for (int y = 0; y < 60; ++y)
        for (int x = 0; x < 60; ++x)
            blob.push_back({x, y});
    auto star = nukex::blobToStar(blob, image.data(), 100, 100, 3, 50);
    REQUIRE(!star.has_value());
}

TEST_CASE("detectStars end-to-end on synthetic image", "[star_detector]") {
    std::vector<float> image(10000, 0.1f);

    auto addStar = [&](int cx, int cy, float peak) {
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx) {
                float r2 = float(dx*dx + dy*dy);
                float val = peak * std::exp(-r2 / 2.0f);
                image[(cy+dy)*100 + (cx+dx)] = 0.1f + val;
            }
    };
    addStar(20, 30, 0.8f);
    addStar(70, 50, 0.6f);
    addStar(40, 80, 0.9f);

    auto stars = nukex::detectStars(image.data(), 100, 100);
    REQUIRE(stars.size() == 3);
    REQUIRE(stars[0].flux >= stars[1].flux);
    REQUIRE(stars[1].flux >= stars[2].flux);
}
