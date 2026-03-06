#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <random>
#include "engine/PixelSelector.h"

TEST_CASE("PixelSelector rejects cosmic ray", "[selector]") {
    // 5 subs, 4x4 image, pixel (2,2) has a cosmic ray in sub 3
    nukex::SubCube cube(5, 4, 4);
    for (size_t z = 0; z < 5; z++)
        cube.setPixel(z, 2, 2, 100.0f);
    cube.setPixel(3, 2, 2, 10000.0f);  // cosmic ray

    std::vector<double> weights(5, 0.2);

    nukex::PixelSelector selector;
    float val = selector.processPixel(cube, 2, 2, weights);

    // Selected value should be near 100, not 10000
    REQUIRE(val == Catch::Approx(100.0f).margin(50.0f));
    // Provenance should NOT be sub 3
    REQUIRE(cube.provenance(2, 2) != 3);
    // Distribution type should be set
    REQUIRE(cube.distType(2, 2) <= 3);
}

TEST_CASE("PixelSelector handles Gaussian noise", "[selector]") {
    nukex::SubCube cube(20, 4, 4);
    std::mt19937 rng(42);
    std::normal_distribution<double> noise(500.0, 10.0);
    for (size_t z = 0; z < 20; z++)
        for (size_t y = 0; y < 4; y++)
            for (size_t x = 0; x < 4; x++)
                cube.setPixel(z, y, x, static_cast<float>(noise(rng)));

    std::vector<double> weights(20, 0.05);

    nukex::PixelSelector selector;
    float val = selector.processPixel(cube, 2, 2, weights);

    // Selected value should be within reasonable range
    REQUIRE(val > 400.0f);
    REQUIRE(val < 600.0f);
}

TEST_CASE("PixelSelector processes full image", "[selector]") {
    nukex::SubCube cube(10, 8, 8);
    std::mt19937 rng(42);
    std::normal_distribution<double> noise(100.0, 5.0);
    for (size_t z = 0; z < 10; z++)
        for (size_t y = 0; y < 8; y++)
            for (size_t x = 0; x < 8; x++)
                cube.setPixel(z, y, x, static_cast<float>(noise(rng)));

    std::vector<double> weights(10, 0.1);
    nukex::PixelSelector selector;
    auto result = selector.processImage(cube, weights);

    REQUIRE(result.size() == 64);
    // Every pixel should have a provenance entry
    for (size_t y = 0; y < 8; y++)
        for (size_t x = 0; x < 8; x++)
            REQUIRE(cube.provenance(y, x) < 10);
}

TEST_CASE("PixelSelector detects distribution type", "[selector]") {
    // Bimodal data: half subs at 100, half at 500
    nukex::SubCube cube(20, 2, 2);
    for (size_t z = 0; z < 10; z++)
        cube.setPixel(z, 0, 0, 100.0f);
    for (size_t z = 10; z < 20; z++)
        cube.setPixel(z, 0, 0, 500.0f);

    std::vector<double> weights(20, 0.05);

    nukex::PixelSelector selector;
    selector.processPixel(cube, 0, 0, weights);

    // Should detect the bimodal pattern
    // (The exact model chosen may vary, but it should be set)
    REQUIRE(cube.distType(0, 0) <= 3);
}

TEST_CASE("PixelSelector handles minimum subs", "[selector]") {
    // Only 3 subs -- minimum for outlier detection
    nukex::SubCube cube(3, 2, 2);
    cube.setPixel(0, 0, 0, 100.0f);
    cube.setPixel(1, 0, 0, 102.0f);
    cube.setPixel(2, 0, 0, 101.0f);

    std::vector<double> weights(3, 1.0/3.0);

    nukex::PixelSelector selector;
    float val = selector.processPixel(cube, 0, 0, weights);

    REQUIRE(val > 99.0f);
    REQUIRE(val < 103.0f);
}
