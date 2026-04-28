#include "catch_amalgamated.hpp"
#include "nukex/core/channel_config.hpp"
#include "nukex/core/filter.hpp"

using namespace nukex;

TEST_CASE("ChannelConfig::from_filter BROADBAND_OSC allocates 4 slots", "[channel_config]") {
    Filter f{ FilterClass::BROADBAND_OSC, "OSC", "ASI585MC", {} };
    auto cfg = ChannelConfig::from_filter(f);
    REQUIRE(cfg.n_channels == 4);
    REQUIRE(cfg.slot_index("R") == 0);
    REQUIRE(cfg.slot_index("G") == 1);
    REQUIRE(cfg.slot_index("B") == 2);
    REQUIRE(cfg.slot_index("L") == 3);
    REQUIRE(cfg.bayer != BayerPattern::NONE);
}

TEST_CASE("ChannelConfig::from_filter BROADBAND_L allocates 1 slot", "[channel_config]") {
    Filter f{ FilterClass::BROADBAND_L, "L", "ASI2600MM", {} };
    auto cfg = ChannelConfig::from_filter(f);
    REQUIRE(cfg.n_channels == 1);
    REQUIRE(cfg.slot_index("L") == 0);
    REQUIRE(cfg.bayer == BayerPattern::NONE);
}

TEST_CASE("ChannelConfig::from_filter DUAL_NB_OSC HaO3 allocates 3 slots",
          "[channel_config]") {
    Filter f{ FilterClass::DUAL_NB_OSC, "HaO3", "ASI585MC", {} };
    auto cfg = ChannelConfig::from_filter(f);
    REQUIRE(cfg.n_channels == 3);
    REQUIRE(cfg.slot_index("R_HaO3") == 0);
    REQUIRE(cfg.slot_index("G_HaO3") == 1);
    REQUIRE(cfg.slot_index("B_HaO3") == 2);
}

TEST_CASE("ChannelConfig::from_filter DUAL_NB_OSC S2O3 allocates 3 slots",
          "[channel_config]") {
    Filter f{ FilterClass::DUAL_NB_OSC, "S2O3", "ASI585MC", {} };
    auto cfg = ChannelConfig::from_filter(f);
    REQUIRE(cfg.n_channels == 3);
    REQUIRE(cfg.slot_index("R_S2O3") == 0);
    REQUIRE(cfg.slot_index("G_S2O3") == 1);
    REQUIRE(cfg.slot_index("B_S2O3") == 2);
}

TEST_CASE("ChannelConfig::from_filter NARROWBAND_SINGLE allocates 1 slot per line",
          "[channel_config]") {
    Filter f_ha{ FilterClass::NARROWBAND_SINGLE, "Ha", "ASI2600MM", {} };
    auto cfg_ha = ChannelConfig::from_filter(f_ha);
    REQUIRE(cfg_ha.n_channels == 1);
    REQUIRE(cfg_ha.slot_index("Ha") == 0);

    Filter f_oiii{ FilterClass::NARROWBAND_SINGLE, "OIII", "ASI2600MM", {} };
    auto cfg_oiii = ChannelConfig::from_filter(f_oiii);
    REQUIRE(cfg_oiii.slot_index("OIII") == 0);

    Filter f_sii{ FilterClass::NARROWBAND_SINGLE, "SII", "ASI2600MM", {} };
    auto cfg_sii = ChannelConfig::from_filter(f_sii);
    REQUIRE(cfg_sii.slot_index("SII") == 0);
}

TEST_CASE("ChannelConfig::from_filter BROADBAND_RGB names map to channel index",
          "[channel_config]") {
    auto cfg_r = ChannelConfig::from_filter({FilterClass::BROADBAND_RGB, "R", "ASI2600MM", {}});
    REQUIRE(cfg_r.n_channels == 1);
    REQUIRE(cfg_r.slot_index("R") == 0);

    auto cfg_g = ChannelConfig::from_filter({FilterClass::BROADBAND_RGB, "G", "ASI2600MM", {}});
    REQUIRE(cfg_g.slot_index("G") == 0);

    auto cfg_b = ChannelConfig::from_filter({FilterClass::BROADBAND_RGB, "B", "ASI2600MM", {}});
    REQUIRE(cfg_b.slot_index("B") == 0);
}

TEST_CASE("ChannelConfig::merge unions slots from two configs", "[channel_config]") {
    auto cfg1 = ChannelConfig::from_filter({FilterClass::BROADBAND_L, "L", "ASI2600MM", {}});
    auto cfg2 = ChannelConfig::from_filter({FilterClass::DUAL_NB_OSC, "HaO3", "ASI585MC", {}});
    auto merged = ChannelConfig::merge(cfg1, cfg2);
    REQUIRE(merged.n_channels == 4); // L + R_HaO3 + G_HaO3 + B_HaO3
    REQUIRE(merged.slot_index("L")      != -1);
    REQUIRE(merged.slot_index("R_HaO3") != -1);
    REQUIRE(merged.slot_index("G_HaO3") != -1);
    REQUIRE(merged.slot_index("B_HaO3") != -1);
}

TEST_CASE("ChannelConfig::slot_index returns -1 for unknown name", "[channel_config]") {
    auto cfg = ChannelConfig::from_filter({FilterClass::BROADBAND_L, "L", "ASI2600MM", {}});
    REQUIRE(cfg.slot_index("DoesNotExist") == -1);
}
