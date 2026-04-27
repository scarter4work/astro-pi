#include "catch_amalgamated.hpp"
#include "nukex/combine/output_assembler.hpp"
#include "nukex/core/cube.hpp"
#include "nukex/core/channel_config.hpp"

using namespace nukex;

TEST_CASE("OutputAssembler: quality map has 4 channels", "[assembler]") {
    auto config = ChannelConfig::from_mode(StackingMode::MONO_L);
    Cube cube(8, 8, config);

    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            auto& v = cube.at(x, y);
            v.distribution[0].shape = DistributionShape::GAUSSIAN;
            v.distribution[0].true_signal_estimate = 0.5f;
            v.distribution[0].signal_uncertainty = 0.01f;
            v.distribution[0].confidence = 0.9f;
            v.dominant_shape = DistributionShape::GAUSSIAN;
        }

    auto quality = OutputAssembler::assemble_quality_map(cube);
    REQUIRE(quality.width() == 8);
    REQUIRE(quality.height() == 8);
    REQUIRE(quality.n_channels() == 4);
    REQUIRE(quality.at(4, 4, 0) == Catch::Approx(0.5f));
    REQUIRE(quality.at(4, 4, 1) == Catch::Approx(0.01f));
    REQUIRE(quality.at(4, 4, 2) == Catch::Approx(0.9f));
    REQUIRE(quality.at(4, 4, 3) == Catch::Approx(0.0f));
}

TEST_CASE("OutputAssembler: shape channel encodes dominant_shape correctly", "[assembler]") {
    auto config = ChannelConfig::from_mode(StackingMode::MONO_L);
    Cube cube(4, 4, config);

    cube.at(0, 0).dominant_shape = DistributionShape::GAUSSIAN;
    cube.at(1, 0).dominant_shape = DistributionShape::BIMODAL;
    cube.at(2, 0).dominant_shape = DistributionShape::HEAVY_TAILED;
    cube.at(3, 0).dominant_shape = DistributionShape::CONTAMINATED;

    auto quality = OutputAssembler::assemble_quality_map(cube);
    REQUIRE(quality.at(0, 0, 3) == Catch::Approx(0.0f));
    REQUIRE(quality.at(1, 0, 3) == Catch::Approx(1.0f));
    REQUIRE(quality.at(2, 0, 3) == Catch::Approx(2.0f));
    REQUIRE(quality.at(3, 0, 3) == Catch::Approx(3.0f));
}
