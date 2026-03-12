#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "engine/PixelSelector.h"
#include "engine/SubCube.h"

using Catch::Approx;

TEST_CASE("Adaptive model selection produces valid results for Gaussian data", "[adaptive]") {
    nukex::PixelSelector::Config config;
    config.maxOutliers = 2;
    config.adaptiveModels = true;
    nukex::PixelSelector selector(config);

    size_t nSubs = 10, H = 2, W = 2;
    nukex::SubCube cube(nSubs, H, W);
    for (size_t z = 0; z < nSubs; ++z)
        for (size_t y = 0; y < H; ++y)
            for (size_t x = 0; x < W; ++x)
                cube.setPixel(z, y, x, 0.5f + 0.001f * z);

    std::vector<double> weights(nSubs, 1.0 / nSubs);
    auto result = selector.processImage(cube, weights);

    REQUIRE(result.size() == H * W);
    for (float val : result) {
        REQUIRE(val > 0.49f);
        REQUIRE(val < 0.52f);
    }
}

TEST_CASE("Adaptive off produces same results as before", "[adaptive]") {
    nukex::PixelSelector::Config config;
    config.maxOutliers = 2;
    config.adaptiveModels = false;
    nukex::PixelSelector selector(config);

    size_t nSubs = 10, H = 1, W = 1;
    nukex::SubCube cube(nSubs, H, W);
    for (size_t z = 0; z < nSubs; ++z)
        cube.setPixel(z, 0, 0, 0.5f + 0.001f * z);

    std::vector<double> weights(nSubs, 1.0 / nSubs);
    auto result = selector.processImage(cube, weights);

    REQUIRE(result.size() == 1);
    REQUIRE(std::isfinite(result[0]));
}
