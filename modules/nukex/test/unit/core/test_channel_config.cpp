#include "catch_amalgamated.hpp"
#include "nukex/core/channel_config.hpp"
#include <string>

using namespace nukex;

TEST_CASE("StackingMode enum covers all modes", "[channel]") {
    REQUIRE(static_cast<uint8_t>(StackingMode::MONO_L) == 0);
    REQUIRE(static_cast<uint8_t>(StackingMode::MONO_LRGB) == 1);
    REQUIRE(static_cast<uint8_t>(StackingMode::OSC_RGB) == 2);
    REQUIRE(static_cast<uint8_t>(StackingMode::OSC_HAO3) == 3);
    REQUIRE(static_cast<uint8_t>(StackingMode::OSC_S2O3) == 4);
    REQUIRE(static_cast<uint8_t>(StackingMode::CUSTOM) == 5);
}

TEST_CASE("ChannelConfig::from_mode MONO_L", "[channel]") {
    auto cfg = ChannelConfig::from_mode(StackingMode::MONO_L);
    REQUIRE(cfg.mode == StackingMode::MONO_L);
    REQUIRE(cfg.n_channels == 1);
    REQUIRE(cfg.channel_names[0] == "L");
    REQUIRE(cfg.bayer == BayerPattern::NONE);
}

TEST_CASE("ChannelConfig::from_mode MONO_LRGB", "[channel]") {
    auto cfg = ChannelConfig::from_mode(StackingMode::MONO_LRGB);
    REQUIRE(cfg.n_channels == 4);
    REQUIRE(cfg.channel_names[0] == "L");
    REQUIRE(cfg.channel_names[1] == "R");
    REQUIRE(cfg.channel_names[2] == "G");
    REQUIRE(cfg.channel_names[3] == "B");
}

TEST_CASE("ChannelConfig::from_mode OSC_RGB", "[channel]") {
    auto cfg = ChannelConfig::from_mode(StackingMode::OSC_RGB);
    REQUIRE(cfg.n_channels == 3);
    REQUIRE(cfg.channel_names[0] == "R");
    REQUIRE(cfg.channel_names[1] == "G");
    REQUIRE(cfg.channel_names[2] == "B");
    REQUIRE(cfg.bayer == BayerPattern::RGGB);
    REQUIRE(cfg.output_rgb_mapping[0] == 0);
    REQUIRE(cfg.output_rgb_mapping[1] == 1);
    REQUIRE(cfg.output_rgb_mapping[2] == 2);
}

TEST_CASE("ChannelConfig::from_mode OSC_HAO3", "[channel]") {
    auto cfg = ChannelConfig::from_mode(StackingMode::OSC_HAO3);
    REQUIRE(cfg.n_channels == 2);
    REQUIRE(cfg.channel_names[0] == "Ha");
    REQUIRE(cfg.channel_names[1] == "OIII");
    REQUIRE(cfg.bayer == BayerPattern::RGGB);
}

TEST_CASE("ChannelConfig::from_mode OSC_S2O3", "[channel]") {
    auto cfg = ChannelConfig::from_mode(StackingMode::OSC_S2O3);
    REQUIRE(cfg.n_channels == 2);
    REQUIRE(cfg.channel_names[0] == "SII");
    REQUIRE(cfg.channel_names[1] == "OIII");
}

TEST_CASE("ChannelConfig: channel_index_for_name", "[channel]") {
    auto cfg = ChannelConfig::from_mode(StackingMode::MONO_LRGB);
    REQUIRE(cfg.channel_index_for_name("L") == 0);
    REQUIRE(cfg.channel_index_for_name("R") == 1);
    REQUIRE(cfg.channel_index_for_name("G") == 2);
    REQUIRE(cfg.channel_index_for_name("B") == 3);
    REQUIRE(cfg.channel_index_for_name("Ha") == -1);
}

TEST_CASE("ChannelConfig: is_mono", "[channel]") {
    REQUIRE(ChannelConfig::from_mode(StackingMode::MONO_L).is_mono() == true);
    REQUIRE(ChannelConfig::from_mode(StackingMode::MONO_LRGB).is_mono() == true);
    REQUIRE(ChannelConfig::from_mode(StackingMode::OSC_RGB).is_mono() == false);
    REQUIRE(ChannelConfig::from_mode(StackingMode::OSC_HAO3).is_mono() == false);
}

TEST_CASE("stacking_mode_name", "[channel]") {
    REQUIRE(stacking_mode_name(StackingMode::MONO_L) == std::string("MONO_L"));
    REQUIRE(stacking_mode_name(StackingMode::OSC_RGB) == std::string("OSC_RGB"));
}
