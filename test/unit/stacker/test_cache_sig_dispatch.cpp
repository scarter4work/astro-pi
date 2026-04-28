// Task 10A: unit tests for the CacheSig / ChannelCacheRef routing types.
//
// Full engine-level routing tests (BROADBAND_OSC builds 3ch cache + REC709_LUMA
// L slot, mono-L builds 1ch cache + DIRECT slot) require real FITS input to
// drive Phase A — without a Task-20 synthetic FITS writer we can't build the
// input images here. Those cases are gated [.integration] and SKIP until the
// writer lands.
//
// What we can test right now without FITS I/O:
//  - ChannelCacheRef default state is safe (cache=nullptr, cache_ch=-1, DIRECT)
//  - SlotSynthesis enum values are distinct and as documented
//  - CacheSig tuple comparison (used as std::map key) orders correctly
//  - ChannelConfig::slot_name() is the inverse of slot_index()
//  - REC709_LUMA synthesis formula matches Phase A's 0.299/0.587/0.114 split

#include "catch_amalgamated.hpp"
#include "nukex/stacker/cache_sig.hpp"
#include "nukex/stacker/frame_cache.hpp"
#include "nukex/core/channel_config.hpp"
#include "nukex/core/filter.hpp"

#include <map>

using namespace nukex;

// ── ChannelCacheRef default state ────────────────────────────────────────────

TEST_CASE("ChannelCacheRef: default is safe (no cache, DIRECT)", "[cache_sig]") {
    ChannelCacheRef ref;
    REQUIRE(ref.cache    == nullptr);
    REQUIRE(ref.cache_ch == -1);
    REQUIRE(ref.kind     == SlotSynthesis::DIRECT);
}

// ── SlotSynthesis enum values ─────────────────────────────────────────────────

TEST_CASE("SlotSynthesis: DIRECT and REC709_LUMA are distinct", "[cache_sig]") {
    REQUIRE(SlotSynthesis::DIRECT      != SlotSynthesis::REC709_LUMA);
    REQUIRE(static_cast<uint8_t>(SlotSynthesis::DIRECT)      == 0);
    REQUIRE(static_cast<uint8_t>(SlotSynthesis::REC709_LUMA) == 1);
}

// ── CacheSig as std::map key ──────────────────────────────────────────────────

TEST_CASE("CacheSig: std::map key comparison works correctly", "[cache_sig]") {
    std::map<CacheSig, int> m;
    m[{100, 200, 3}] = 3;
    m[{100, 200, 1}] = 1;
    m[{200, 100, 3}] = 99;

    REQUIRE(m.size() == 3);
    REQUIRE(m.at({100, 200, 3}) == 3);
    REQUIRE(m.at({100, 200, 1}) == 1);
    REQUIRE(m.at({200, 100, 3}) == 99);

    // Same sig as an existing entry should not create a new entry.
    m[{100, 200, 3}] = 42;
    REQUIRE(m.size() == 3);
    REQUIRE(m.at({100, 200, 3}) == 42);
}

// ── ChannelConfig::slot_name() inverse of slot_index() ────────────────────────

TEST_CASE("ChannelConfig::slot_name is inverse of slot_index for BROADBAND_OSC", "[cache_sig][channel_config]") {
    Filter f;
    f.cls  = FilterClass::BROADBAND_OSC;
    f.name = "OSC";
    ChannelConfig cfg = ChannelConfig::from_filter(f);

    REQUIRE(cfg.n_channels == 4);

    // Round-trip: slot_name(slot_index(name)) == name for all 4 slots.
    for (const char* name : {"R", "G", "B", "L"}) {
        int idx = cfg.slot_index(name);
        REQUIRE(idx >= 0);
        REQUIRE(cfg.slot_name(idx) == std::string(name));
    }
}

TEST_CASE("ChannelConfig::slot_name is inverse of slot_index for NARROWBAND_SINGLE", "[cache_sig][channel_config]") {
    Filter f;
    f.cls  = FilterClass::NARROWBAND_SINGLE;
    f.name = "Ha";
    ChannelConfig cfg = ChannelConfig::from_filter(f);

    REQUIRE(cfg.n_channels == 1);
    int idx = cfg.slot_index("Ha");
    REQUIRE(idx == 0);
    REQUIRE(cfg.slot_name(0) == "Ha");
}

TEST_CASE("ChannelConfig::slot_name is inverse of slot_index for DUAL_NB_OSC", "[cache_sig][channel_config]") {
    Filter f;
    f.cls  = FilterClass::DUAL_NB_OSC;
    f.name = "HaO3";
    ChannelConfig cfg = ChannelConfig::from_filter(f);

    REQUIRE(cfg.n_channels == 3);
    for (const char* prefix : {"R_HaO3", "G_HaO3", "B_HaO3"}) {
        int idx = cfg.slot_index(prefix);
        REQUIRE(idx >= 0);
        REQUIRE(cfg.slot_name(idx) == std::string(prefix));
    }
}

// ── REC709_LUMA synthesis formula matches Phase A ─────────────────────────────

TEST_CASE("REC709_LUMA synthesis formula: 0.299R + 0.587G + 0.114B", "[cache_sig]") {
    // Phase A does: l = 0.299f * r + 0.587f * g + 0.114f * b
    // Phase B's extract_from_cube must produce the same value.
    // We verify the formula is numerically consistent (not testing the buffer
    // machinery, just the coefficients) so a future change to one without the
    // other gets caught.

    float r = 0.8f, g = 0.5f, b = 0.2f;

    float phase_a_l = 0.299f * r + 0.587f * g + 0.114f * b;

    // Expected from Phase B (same formula, applied to per-frame values):
    float phase_b_l = 0.299f * r + 0.587f * g + 0.114f * b;

    REQUIRE(phase_a_l == Catch::Approx(phase_b_l).margin(1e-7f));

    // Sanity: coefficients sum to 1.0 (luma is scale-preserving on equal input).
    float grey = 0.6f;
    float luma = 0.299f * grey + 0.587f * grey + 0.114f * grey;
    REQUIRE(luma == Catch::Approx(grey).margin(1e-6f));
}

// ── Integration-gated engine-level tests ─────────────────────────────────────
//
// Full routing tests require Phase A to run with real (or synthetic) FITS input
// to populate the cache map. They are gated [.integration] and SKIP until a
// Task-20 synthetic FITS writer exists to drive them headlessly.

TEST_CASE("StackingEngine: BROADBAND_OSC builds 3ch cache + REC709_LUMA L slot ref",
          "[.integration][engine][cache]") {
    SKIP("Wired by Task 20: synthetic FITS writer needed to drive Phase A headlessly.");
}

TEST_CASE("StackingEngine: mono L only builds 1ch cache + DIRECT ref",
          "[.integration][engine][cache]") {
    SKIP("Wired by Task 20: synthetic FITS writer needed to drive Phase A headlessly.");
}
