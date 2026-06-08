// tests/integration/test_full_pipeline.cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <random>
#include <numeric>
#include <cmath>

#include "engine/SubCube.h"
#include "engine/PixelSelector.h"
#include "engine/NumericalUtils.h"

TEST_CASE("Full pipeline: cosmic ray rejection", "[integration]") {
    // 20 subs, 16x16 image, sky background ~100, noise sigma ~5
    const size_t nSubs = 20;
    const size_t H = 16, W = 16;

    nukex::SubCube cube(nSubs, H, W);
    std::mt19937 rng(42);
    std::normal_distribution<double> noise(100.0, 5.0);

    // Fill with Gaussian noise
    for (size_t z = 0; z < nSubs; ++z)
        for (size_t y = 0; y < H; ++y)
            for (size_t x = 0; x < W; ++x)
                cube.setPixel(z, y, x, static_cast<float>(noise(rng)));

    // Inject cosmic ray at (8, 8) in sub 5: spike to 10000
    cube.setPixel(5, 8, 8, 10000.0f);
    // Inject cosmic ray at (4, 12) in sub 15: spike to 5000
    cube.setPixel(15, 4, 12, 5000.0f);

    nukex::PixelSelector selector;
    auto result = selector.processImage(cube, nullptr);

    SECTION("cosmic ray at (8,8) is rejected") {
        // Provenance should NOT select sub 5
        REQUIRE(cube.provenance(8, 8) != 5);
        // Selected value should be near sky background
        REQUIRE(result[8 * W + 8] == Catch::Approx(100.0f).margin(30.0f));
    }

    SECTION("cosmic ray at (4,12) is rejected") {
        REQUIRE(cube.provenance(4, 12) != 15);
        REQUIRE(result[4 * W + 12] == Catch::Approx(100.0f).margin(30.0f));
    }

    SECTION("clean pixels have reasonable values") {
        // Check a pixel with no artifacts: (0, 0)
        float val = result[0];
        REQUIRE(val > 70.0f);
        REQUIRE(val < 130.0f);
    }

    SECTION("all provenances are valid") {
        for (size_t y = 0; y < H; ++y)
            for (size_t x = 0; x < W; ++x)
                REQUIRE(cube.provenance(y, x) < nSubs);
    }

    SECTION("all distribution types are valid") {
        for (size_t y = 0; y < H; ++y)
            for (size_t x = 0; x < W; ++x)
                REQUIRE(cube.distType(y, x) <= 3);
    }
}

TEST_CASE("Full pipeline: satellite trail rejection", "[integration]") {
    const size_t nSubs = 20;
    const size_t H = 16, W = 16;

    nukex::SubCube cube(nSubs, H, W);
    std::mt19937 rng(123);
    std::normal_distribution<double> noise(200.0, 8.0);

    for (size_t z = 0; z < nSubs; ++z)
        for (size_t y = 0; y < H; ++y)
            for (size_t x = 0; x < W; ++x)
                cube.setPixel(z, y, x, static_cast<float>(noise(rng)));

    // Inject satellite trail across row 4 in sub 7: bright streak
    for (size_t x = 0; x < W; ++x)
        cube.setPixel(7, 4, x, 5000.0f);

    nukex::PixelSelector selector;
    auto result = selector.processImage(cube, nullptr);

    // All pixels in row 4 should NOT select sub 7
    for (size_t x = 0; x < W; ++x) {
        REQUIRE(cube.provenance(4, x) != 7);
        REQUIRE(result[4 * W + x] == Catch::Approx(200.0f).margin(40.0f));
    }
}

TEST_CASE("Full pipeline: quality weighted selection", "[integration]") {
    // 10 subs, 8x8 image
    // 5 "good" subs (low noise), 5 "bad" subs (high noise)
    const size_t nSubs = 10;
    const size_t H = 8, W = 8;

    nukex::SubCube cube(nSubs, H, W);
    std::mt19937 rng(42);

    // Good subs (0-4): sky=100, sigma=2
    for (size_t z = 0; z < 5; ++z) {
        std::normal_distribution<double> good(100.0, 2.0);
        for (size_t y = 0; y < H; ++y)
            for (size_t x = 0; x < W; ++x)
                cube.setPixel(z, y, x, static_cast<float>(good(rng)));
    }

    // Bad subs (5-9): sky=100, sigma=20
    for (size_t z = 5; z < 10; ++z) {
        std::normal_distribution<double> bad(100.0, 20.0);
        for (size_t y = 0; y < H; ++y)
            for (size_t x = 0; x < W; ++x)
                cube.setPixel(z, y, x, static_cast<float>(bad(rng)));
    }

    // Give good subs higher quality scores
    std::vector<double> scores = {0.15, 0.15, 0.15, 0.15, 0.15, 0.05, 0.05, 0.05, 0.05, 0.05};

    nukex::PixelSelector selector;
    auto result = selector.processImage(cube, scores.data());

    // Output mean should be close to 100 (sky background)
    double sum = 0;
    for (size_t i = 0; i < H * W; ++i)
        sum += result[i];
    double mean = sum / (H * W);
    REQUIRE(mean == Catch::Approx(100.0).margin(15.0));
}

TEST_CASE("Full pipeline: output image dimensions", "[integration]") {
    nukex::SubCube cube(5, 32, 24);
    std::mt19937 rng(42);
    std::normal_distribution<double> noise(50.0, 3.0);
    for (size_t z = 0; z < 5; ++z)
        for (size_t y = 0; y < 32; ++y)
            for (size_t x = 0; x < 24; ++x)
                cube.setPixel(z, y, x, static_cast<float>(noise(rng)));

    nukex::PixelSelector selector;
    auto result = selector.processImage(cube, nullptr);

    REQUIRE(result.size() == 32 * 24);
}
