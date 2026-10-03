#include "catch_amalgamated.hpp"
#include "nukex/core/channel_config.hpp"
#include "nukex/core/filter.hpp"

using namespace nukex;

static ChannelConfig cfg_for(FilterClass cls, const char* name) {
    Filter f;
    f.cls  = cls;
    f.name = name;
    return ChannelConfig::from_filter(f);
}

TEST_CASE("ChannelConfig: channel_index_for_name / slot_index / slot_name over a merged LRGB config", "[channel]") {
    ChannelConfig cfg = cfg_for(FilterClass::BROADBAND_L, "L");
    for (const char* c : {"R", "G", "B"}) {
        cfg = ChannelConfig::merge(cfg, cfg_for(FilterClass::BROADBAND_RGB, c));
    }
    REQUIRE(cfg.n_channels == 4);
    REQUIRE(cfg.channel_index_for_name("L") == 0);
    REQUIRE(cfg.channel_index_for_name("R") == 1);
    REQUIRE(cfg.channel_index_for_name("G") == 2);
    REQUIRE(cfg.channel_index_for_name("B") == 3);
    REQUIRE(cfg.channel_index_for_name("Ha") == -1);
    REQUIRE(cfg.slot_index("G") == cfg.channel_index_for_name("G"));
    REQUIRE(cfg.slot_name(3) == "B");
}

TEST_CASE("ChannelConfig: bayer defaults to NONE for mono classes and RGGB for Bayer classes", "[channel]") {
    REQUIRE(cfg_for(FilterClass::BROADBAND_L, "L").bayer        == BayerPattern::NONE);
    REQUIRE(cfg_for(FilterClass::NARROWBAND_SINGLE, "Ha").bayer == BayerPattern::NONE);
    REQUIRE(cfg_for(FilterClass::BROADBAND_OSC, "OSC").bayer    == BayerPattern::RGGB);
    REQUIRE(cfg_for(FilterClass::DUAL_NB_OSC, "HaO3").bayer     == BayerPattern::RGGB);
}
