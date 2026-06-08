#include "catch_amalgamated.hpp"
#include "nukex/core/cube.hpp"

using namespace nukex;

TEST_CASE("Cube: construction with dimensions", "[cube]") {
    auto cfg = ChannelConfig::from_mode(StackingMode::OSC_RGB);
    Cube cube(100, 80, cfg);
    REQUIRE(cube.width == 100);
    REQUIRE(cube.height == 80);
    REQUIRE(cube.channel_config.n_channels == 3);
    REQUIRE(cube.n_frames_loaded == 0);
    REQUIRE(cube.total_pixels() == 8000);
}

TEST_CASE("Cube: voxel access by coordinates", "[cube]") {
    auto cfg = ChannelConfig::from_mode(StackingMode::MONO_L);
    Cube cube(10, 10, cfg);
    cube.at(3, 5).n_frames = 42;
    cube.at(3, 5).confidence = 0.95f;
    REQUIRE(cube.at(3, 5).n_frames == 42);
    REQUIRE(cube.at(3, 5).confidence == Catch::Approx(0.95f));
    REQUIRE(cube.at(0, 0).n_frames == 0);
}

TEST_CASE("Cube: voxels initialized with correct channel count", "[cube]") {
    auto cfg = ChannelConfig::from_mode(StackingMode::OSC_HAO3);
    Cube cube(4, 4, cfg);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            REQUIRE(cube.at(x, y).n_channels == 2);
}

TEST_CASE("Cube: const access", "[cube]") {
    auto cfg = ChannelConfig::from_mode(StackingMode::OSC_RGB);
    Cube cube(5, 5, cfg);
    cube.at(2, 3).confidence = 0.8f;
    const Cube& c = cube;
    REQUIRE(c.at(2, 3).confidence == Catch::Approx(0.8f));
}

TEST_CASE("Cube: is_valid_coord", "[cube]") {
    auto cfg = ChannelConfig::from_mode(StackingMode::MONO_L);
    Cube cube(10, 8, cfg);
    REQUIRE(cube.is_valid_coord(0, 0) == true);
    REQUIRE(cube.is_valid_coord(9, 7) == true);
    REQUIRE(cube.is_valid_coord(10, 0) == false);
    REQUIRE(cube.is_valid_coord(0, 8) == false);
    REQUIRE(cube.is_valid_coord(-1, 0) == false);
}
