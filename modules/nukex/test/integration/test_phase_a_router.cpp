// Phase A router integration tests (Task 9).
//
// These five [integration]-tagged cases describe the END-TO-END
// behaviour of the new FilterClassifier-driven Phase A pipeline:
//
//   1. BROADBAND_OSC frame synthesises an "L" slot via rec709 luminance.
//   2. DUAL_NB_OSC HaO3 frame routes into R_HaO3 / G_HaO3 / B_HaO3.
//   3. UNKNOWN FILTER on a Bayer frame fails the batch loud at start.
//   4. Missing FILTER on a Bayer frame is silently treated as OSC.
//   5. Mixed L (mono) + HaO3 (Bayer) batch builds a union slot config.

#include "catch_amalgamated.hpp"
#include "nukex/stacker/stacking_engine.hpp"
#include "nukex/core/filter.hpp"
#include "nukex/core/cube.hpp"
#include "nukex/core/channel_config.hpp"
#include "nukex/io/filter_alias.hpp"
#include "synthetic_fits.hpp"

#include <filesystem>

using namespace nukex;
namespace fs = std::filesystem;

TEST_CASE("Phase A: BROADBAND_OSC frame synthesizes L slot",
          "[integration][phase_a]") {
    auto tmp = fs::temp_directory_path() / "phase_a_osc.fits";
    test_util::write_synthetic_bayer(tmp.string(), 16, 16, "RGGB", "ASI585MC", "", 0.5f);

    StackingEngine::Config cfg;
    cfg.qe_database_path = (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();
    StackingEngine engine(cfg);
    auto result = engine.execute({tmp.string()}, {}, /*progress*/nullptr);

    REQUIRE(result.ok);
    REQUIRE(result.cube->channel_config.slot_index("R") != -1);
    REQUIRE(result.cube->channel_config.slot_index("G") != -1);
    REQUIRE(result.cube->channel_config.slot_index("B") != -1);
    REQUIRE(result.cube->channel_config.slot_index("L") != -1);
    int  L_idx = result.cube->channel_config.slot_index("L");
    auto& px   = result.cube->at(8, 8);
    REQUIRE(px.channel(L_idx).welford.mean == Catch::Approx(0.5f).margin(0.05f));
}

TEST_CASE("Phase A: HaO3 dual-NB frame routes into R_HaO3/G_HaO3/B_HaO3",
          "[integration][phase_a]") {
    auto tmp = fs::temp_directory_path() / "phase_a_hao3.fits";
    test_util::write_synthetic_bayer(tmp.string(), 16, 16, "RGGB", "ASI585MC", "HaO3", 0.5f);

    StackingEngine::Config cfg;
    cfg.qe_database_path = (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();
    StackingEngine engine(cfg);
    auto result = engine.execute({tmp.string()}, {}, nullptr);

    REQUIRE(result.ok);
    REQUIRE(result.cube->channel_config.slot_index("R_HaO3") != -1);
    REQUIRE(result.cube->channel_config.slot_index("G_HaO3") != -1);
    REQUIRE(result.cube->channel_config.slot_index("B_HaO3") != -1);
    REQUIRE(result.cube->channel_config.slot_index("R") == -1);
}

TEST_CASE("Phase A: unknown FILTER on Bayer fails the batch loud at start",
          "[integration][phase_a]") {
    auto tmp = fs::temp_directory_path() / "phase_a_unknown.fits";
    test_util::write_synthetic_bayer(tmp.string(), 16, 16, "RGGB", "ASI585MC", "ALP-T-fake-2026", 0.5f);

    StackingEngine::Config cfg;
    cfg.qe_database_path = (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();
    StackingEngine engine(cfg);
    auto result = engine.execute({tmp.string()}, {}, nullptr);

    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error.find("ALP-T-fake-2026") != std::string::npos);
    REQUIRE(result.error.find("qe_overrides.json") != std::string::npos);
}

TEST_CASE("Phase A: missing FILTER on Bayer is silent BROADBAND_OSC",
          "[integration][phase_a]") {
    auto tmp = fs::temp_directory_path() / "phase_a_no_filter.fits";
    test_util::write_synthetic_bayer(tmp.string(), 16, 16, "RGGB", "ASI585MC", "", 0.5f);

    StackingEngine::Config cfg;
    cfg.qe_database_path = (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();
    StackingEngine engine(cfg);
    auto result = engine.execute({tmp.string()}, {}, nullptr);

    REQUIRE(result.ok);
    REQUIRE(result.cube->channel_config.slot_index("L") != -1);
}

TEST_CASE("Phase A: a mono L + Bayer HaO3 batch is refused, not silently mixed",
          "[integration][phase_a]") {
    // This case used to assert only that the slot union came out as
    // {L, R_HaO3, G_HaO3, B_HaO3} -- which it did, while the pixels behind
    // those slots were never right. The batch-level Bayer pattern comes from
    // the first frame, so with a mono frame first the HaO3 frame is never
    // debayered, and Phase B reads three channels out of a one-channel cache
    // that every frame shares. Asserting the slot names passed a
    // configuration that cannot produce correct output.
    //
    // Same root cause as the LRGB-mono case (test_lrgb_mono.cpp): FrameCache
    // is keyed on geometry, so Phase B cannot separate one slot's frames from
    // another's. Until that is fixed the batch is refused.
    auto t1 = fs::temp_directory_path() / "phase_a_l.fits";
    auto t2 = fs::temp_directory_path() / "phase_a_hao3_2.fits";
    test_util::write_synthetic_mono(t1.string(), 16, 16, "ASI2600MM", "L", 0.5f);
    test_util::write_synthetic_bayer(t2.string(), 16, 16, "RGGB", "ASI585MC", "HaO3", 0.5f);

    StackingEngine::Config cfg;
    cfg.qe_database_path = (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();
    StackingEngine engine(cfg);
    auto result = engine.execute({t1.string(), t2.string()}, {}, nullptr);

    REQUIRE_FALSE(result.ok);
    INFO("error: " << result.error);
    REQUIRE(result.error.find("separately") != std::string::npos);
}

TEST_CASE("Phase A: a batch mixing Bayer and mono frames is refused in either order",
          "[integration][phase_a]") {
    // The batch-level Bayer pattern is taken from frame 0 alone and line ~569
    // debayers on that one global, so the two orderings fail differently and
    // both silently:
    //
    //   mono first  -> the Bayer frame is never demosaiced, and the OSC
    //                  routing branches read channels 1 and 2 of a
    //                  one-channel image. Image::at is unchecked, so that is
    //                  an out-of-bounds read.
    //   Bayer first -> every mono frame IS demosaiced, as if it were a CFA
    //                  mosaic, and BROADBAND_L routes channel 0 of that
    //                  fabricated image into the L slot. No fault, wrong
    //                  pixels.
    //
    // Debayering per frame instead would fix the read but produce mixed
    // geometries in one batch, which lands on the same Phase B limitation as
    // multi-filter mono: FrameCache is keyed on geometry and cannot keep two
    // frame sets apart. So the batch is refused until that is addressed.
    //
    // Note for anyone tempted to assert on pixel values here instead: a
    // uniform synthetic frame demosaics to itself, so a value assertion
    // passes even when the frame was wrongly demosaiced. Refusal is the
    // observable behaviour.
    auto mono  = fs::temp_directory_path() / "phase_a_mix_mono.fits";
    auto bayer = fs::temp_directory_path() / "phase_a_mix_bayer.fits";
    test_util::write_synthetic_mono(mono.string(), 16, 16, "ASI2600MM", "L", 0.8f);
    test_util::write_synthetic_bayer(bayer.string(), 16, 16, "RGGB", "ASI585MC", "HaO3", 0.5f);

    StackingEngine::Config cfg;
    cfg.qe_database_path = (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();

    SECTION("mono frame first") {
        StackingEngine engine(cfg);
        auto r = engine.execute({mono.string(), bayer.string()}, {}, nullptr);
        REQUIRE_FALSE(r.ok);
        INFO("error: " << r.error);
        REQUIRE(r.error.find("separately") != std::string::npos);
    }

    SECTION("Bayer frame first") {
        StackingEngine engine(cfg);
        auto r = engine.execute({bayer.string(), mono.string()}, {}, nullptr);
        REQUIRE_FALSE(r.ok);
        INFO("error: " << r.error);
        REQUIRE(r.error.find("separately") != std::string::npos);
    }
}

TEST_CASE("Phase A: missing FILTER on mono (L_unnamed) routes into the L slot",
          "[integration][phase_a]") {
    auto tmp = fs::temp_directory_path() / "phase_a_mono_unnamed.fits";
    test_util::write_synthetic_mono(tmp.string(), 16, 16, "ASI2600MM", /*filter*/"", 0.5f);

    StackingEngine::Config cfg;
    cfg.qe_database_path = (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();
    StackingEngine engine(cfg);
    auto result = engine.execute({tmp.string()}, {}, nullptr);

    REQUIRE(result.ok);
    int L_idx = result.cube->channel_config.slot_index("L");
    REQUIRE(L_idx != -1);
    REQUIRE(result.cube->at(8, 8).channel(L_idx).welford.mean == Catch::Approx(0.5f).margin(0.05f));
}

TEST_CASE("Phase A: unknown FILTER on mono routes into the L slot (spec 6.3, e.g. a wheel slot number)",
          "[integration][phase_a]") {
    auto tmp = fs::temp_directory_path() / "phase_a_mono_unknown.fits";
    test_util::write_synthetic_mono(tmp.string(), 16, 16, "ATR585M", /*filter*/"1", 0.5f);

    StackingEngine::Config cfg;
    cfg.qe_database_path = (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();
    StackingEngine engine(cfg);
    auto result = engine.execute({tmp.string()}, {}, nullptr);

    REQUIRE(result.ok);
    int L_idx = result.cube->channel_config.slot_index("L");
    REQUIRE(L_idx != -1);
    REQUIRE(result.cube->at(8, 8).channel(L_idx).welford.mean == Catch::Approx(0.5f).margin(0.05f));
}

TEST_CASE("Phase A: an unrecognised filter names itself in the result",
          "[integration][phase_a]") {
    // The interface can only offer to learn the name if it is told which name
    // stopped the batch. Matching on the message text would break the first
    // time the wording changed.
    auto tmp = fs::temp_directory_path() / "phase_a_unknown_named.fits";
    test_util::write_synthetic_bayer(tmp.string(), 16, 16, "RGGB", "ASI585MC",
                                     "WhateverTheCaptureSoftwareWrote", 0.5f);

    StackingEngine::Config cfg;
    cfg.qe_database_path = (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();
    StackingEngine engine(cfg);
    auto r = engine.execute({tmp.string()}, {}, nullptr);

    REQUIRE_FALSE(r.ok);
    REQUIRE(r.unknown_filter == "WhateverTheCaptureSoftwareWrote");
}

TEST_CASE("Phase A: a taught filter name stacks", "[integration][phase_a]") {
    // The other half: once the user has said which lines it passes, the same
    // batch runs without asking again.
    auto tmp = fs::temp_directory_path() / "phase_a_taught.fits";
    test_util::write_synthetic_bayer(tmp.string(), 16, 16, "RGGB", "ASI585MC",
                                     "MyDuoBand", 0.5f);
    auto alias_path = fs::temp_directory_path() / "phase_a_aliases.json";
    {
        FilterAliasStore s;
        s.set("MyDuoBand", "HaO3");
        REQUIRE(s.save(alias_path.string()));
    }

    StackingEngine::Config cfg;
    cfg.qe_database_path = (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();
    cfg.filter_alias_path = alias_path.string();
    StackingEngine engine(cfg);
    auto r = engine.execute({tmp.string()}, {}, nullptr);

    INFO("error: " << r.error);
    REQUIRE(r.ok);
    REQUIRE(r.unknown_filter.empty());
    REQUIRE(r.cube->channel_config.slot_index("R_HaO3") != -1);
    fs::remove(alias_path);
}
