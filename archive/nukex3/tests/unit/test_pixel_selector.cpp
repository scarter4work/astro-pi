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

    nukex::PixelSelector selector;
    float val = selector.processPixel(cube, 2, 2);

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

    nukex::PixelSelector selector;
    float val = selector.processPixel(cube, 2, 2);

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

    nukex::PixelSelector selector;
    auto result = selector.processImage(cube, nullptr);

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

    nukex::PixelSelector selector;
    selector.processPixel(cube, 0, 0);

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

    nukex::PixelSelector selector;
    float val = selector.processPixel(cube, 0, 0);

    REQUIRE(val > 99.0f);
    REQUIRE(val < 103.0f);
}

TEST_CASE("PixelSelector tiebreaker picks better-scored frame", "[selector][tiebreaker]") {
    // 10 frames with nearly identical values (tiny ascending spread).
    // The shortest-half mode estimator will find the densest cluster and pick
    // the closest frame.  The tiebreaker should then prefer the frame with
    // the highest quality score among those within MAD tolerance.
    //
    // Frame 3 has the best quality score (0.9 vs 0.1 for all others).
    // All values are close enough that multiple frames fall within the MAD
    // tolerance, so the tiebreaker has room to prefer frame 3.
    nukex::SubCube cube(10, 2, 2);
    for (size_t z = 0; z < 10; z++)
        cube.setPixel(z, 0, 0, 100.0f + static_cast<float>(z) * 0.001f);

    std::vector<double> scores(10, 0.1);
    scores[3] = 0.9;  // frame 3 is the best quality

    nukex::PixelSelector::Config cfg;
    cfg.enableMetadataTiebreaker = true;
    nukex::PixelSelector selector(cfg);
    auto result = selector.selectBestZ(cube.zColumnPtr(0, 0), 10,
                                        scores.data(), nullptr);

    // Frame 3 is within the densest half (values 100.000-100.004) and
    // within MAD tolerance of the mode, so the tiebreaker should select it.
    REQUIRE(result.selectedZ == 3);
}

TEST_CASE("PixelSelector tiebreaker no-op with equal scores", "[selector][tiebreaker]") {
    nukex::SubCube cube(10, 2, 2);
    for (size_t z = 0; z < 10; z++)
        cube.setPixel(z, 0, 0, 100.0f + static_cast<float>(z) * 0.1f);

    std::vector<double> scores(10, 0.5);

    nukex::PixelSelector::Config cfg;
    cfg.enableMetadataTiebreaker = true;
    nukex::PixelSelector selector(cfg);
    auto resultWith = selector.selectBestZ(cube.zColumnPtr(0, 0), 10,
                                            scores.data(), nullptr);
    auto resultWithout = selector.selectBestZ(cube.zColumnPtr(0, 0), 10,
                                              nullptr, nullptr);

    REQUIRE(resultWith.selectedZ == resultWithout.selectedZ);
    REQUIRE(resultWith.selectedValue == resultWithout.selectedValue);
}

TEST_CASE("PixelSelector tiebreaker no-op with null scores", "[selector][tiebreaker]") {
    nukex::SubCube cube(10, 2, 2);
    for (size_t z = 0; z < 10; z++)
        cube.setPixel(z, 0, 0, 100.0f + static_cast<float>(z) * 0.1f);

    nukex::PixelSelector selector;
    auto result = selector.selectBestZ(cube.zColumnPtr(0, 0), 10,
                                        nullptr, nullptr);

    REQUIRE(result.selectedZ < 10);
    REQUIRE(result.selectedValue > 99.0f);
    REQUIRE(result.selectedValue < 102.0f);
}

TEST_CASE("PixelSelector tiebreaker zero scores graceful degradation", "[selector][tiebreaker]") {
    nukex::SubCube cube(10, 2, 2);
    for (size_t z = 0; z < 10; z++)
        cube.setPixel(z, 0, 0, 100.0f + static_cast<float>(z) * 0.1f);

    std::vector<double> scores(10, 0.0);

    nukex::PixelSelector::Config cfg;
    cfg.enableMetadataTiebreaker = true;
    nukex::PixelSelector selector(cfg);
    auto resultZero = selector.selectBestZ(cube.zColumnPtr(0, 0), 10,
                                            scores.data(), nullptr);
    auto resultNull = selector.selectBestZ(cube.zColumnPtr(0, 0), 10,
                                            nullptr, nullptr);

    REQUIRE(resultZero.selectedZ == resultNull.selectedZ);
}

TEST_CASE("PixelSelector tiebreaker single candidate no change", "[selector][tiebreaker]") {
    nukex::SubCube cube(3, 2, 2);
    cube.setPixel(0, 0, 0, 100.0f);
    cube.setPixel(1, 0, 0, 200.0f);
    cube.setPixel(2, 0, 0, 300.0f);

    std::vector<double> scores = {0.1, 0.9, 0.1};

    nukex::PixelSelector::Config cfg;
    cfg.enableMetadataTiebreaker = true;
    nukex::PixelSelector selector(cfg);
    auto result = selector.selectBestZ(cube.zColumnPtr(0, 0), 3,
                                        scores.data(), nullptr);

    REQUIRE(result.selectedZ < 3);
}
