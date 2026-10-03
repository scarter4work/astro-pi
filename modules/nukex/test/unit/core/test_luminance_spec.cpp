#include "catch_amalgamated.hpp"
#include "nukex/core/luminance_spec.hpp"
#include "nukex/core/channel_config.hpp"
#include "nukex/core/filter.hpp"

using namespace nukex;

namespace {
ChannelConfig cfg_for(FilterClass cls, const char* name) {
    Filter f; f.cls = cls; f.name = name;
    return ChannelConfig::from_filter(f);
}
} // namespace

TEST_CASE("LuminanceSpec: an OSC stack measures noise on its synthesized L plane",
          "[core][luminance]") {
    auto cfg = cfg_for(FilterClass::BROADBAND_OSC, "OSC");
    const int l = cfg.slot_index("L");
    REQUIRE(l >= 0);
    auto s = LuminanceSpec::for_config(cfg, cfg.n_channels);
    REQUIRE(s.mode == LuminanceSpec::SINGLE);
    REQUIRE(s.c0 == l);
    REQUIRE(s.describe(&cfg) == "the L plane");
}

TEST_CASE("LuminanceSpec: a mono L stack is its one plane", "[core][luminance]") {
    auto cfg = cfg_for(FilterClass::BROADBAND_L, "L");
    auto s = LuminanceSpec::for_config(cfg, cfg.n_channels);
    REQUIRE(s.mode == LuminanceSpec::SINGLE);
    REQUIRE(s.c0 == 0);
}

TEST_CASE("LuminanceSpec: LRGB-mono slots in arrival order still find L by name",
          "[core][luminance]") {
    // Build the config the way Phase A does: merge first-seen filters. B first,
    // then L, so a positional plane 0 would be blue.
    ChannelConfig cfg = cfg_for(FilterClass::BROADBAND_RGB, "B");
    cfg = ChannelConfig::merge(cfg, cfg_for(FilterClass::BROADBAND_L, "L"));
    cfg = ChannelConfig::merge(cfg, cfg_for(FilterClass::BROADBAND_RGB, "R"));
    cfg = ChannelConfig::merge(cfg, cfg_for(FilterClass::BROADBAND_RGB, "G"));
    REQUIRE(cfg.slot_index("L") != 0);
    auto s = LuminanceSpec::for_config(cfg, cfg.n_channels);
    REQUIRE(s.mode == LuminanceSpec::SINGLE);
    REQUIRE(s.c0 == cfg.slot_index("L"));
}

TEST_CASE("LuminanceSpec: without L or named RGB the historical positional rule holds",
          "[core][luminance]") {
    auto cfg = cfg_for(FilterClass::DUAL_NB_OSC, "HaO3");
    REQUIRE(cfg.slot_index("L") < 0);
    REQUIRE(cfg.slot_index("R") < 0);
    auto s = LuminanceSpec::for_config(cfg, cfg.n_channels);
    REQUIRE(s.mode == LuminanceSpec::REC709);
    REQUIRE(s.c0 == 0); REQUIRE(s.c1 == 1); REQUIRE(s.c2 == 2);
    // and AUTO resolves the same way
    REQUIRE(LuminanceSpec{}.resolved(3).mode == LuminanceSpec::REC709);
    REQUIRE(LuminanceSpec{}.resolved(1).mode == LuminanceSpec::SINGLE);
}
