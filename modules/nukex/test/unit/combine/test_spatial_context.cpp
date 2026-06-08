#include "catch_amalgamated.hpp"
#include "nukex/combine/spatial_context.hpp"
#include "nukex/io/image.hpp"
#include "nukex/core/cube.hpp"
#include "nukex/core/channel_config.hpp"
#include <cmath>

using namespace nukex;

TEST_CASE("SpatialContext::sobel_gradient: flat image → zero gradient", "[spatial]") {
    Image img(32, 32, 1);
    img.fill(0.5f);
    float grad = SpatialContext::sobel_gradient(img, 16, 16);
    REQUIRE(grad == Catch::Approx(0.0f).margin(1e-6f));
}

TEST_CASE("SpatialContext::sobel_gradient: sharp edge → high gradient", "[spatial]") {
    Image img(32, 32, 1);
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++)
            img.at(x, y, 0) = (x < 16) ? 0.0f : 1.0f;
    float grad = SpatialContext::sobel_gradient(img, 16, 16);
    REQUIRE(grad > 1.0f);
}

TEST_CASE("SpatialContext::sobel_gradient: border → zero", "[spatial]") {
    Image img(32, 32, 1);
    img.fill(0.5f);
    REQUIRE(SpatialContext::sobel_gradient(img, 0, 0) == 0.0f);
    REQUIRE(SpatialContext::sobel_gradient(img, 31, 31) == 0.0f);
}

TEST_CASE("SpatialContext::compute: writes to voxels", "[spatial]") {
    auto config = ChannelConfig::from_mode(StackingMode::MONO_L);
    Cube cube(16, 16, config);
    Image output(16, 16, 1);
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            output.at(x, y, 0) = 0.5f;

    SpatialContext ctx;
    ctx.compute(output, cube);

    auto& v = cube.at(8, 8);
    REQUIRE(v.gradient_mag == Catch::Approx(0.0f).margin(1e-6f));
    REQUIRE(v.local_background == Catch::Approx(0.5f).margin(0.01f));
    REQUIRE(v.local_rms == Catch::Approx(0.0f).margin(0.01f));
}
