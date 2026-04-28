// Phase A router integration tests (Task 9).
//
// These five [.integration]-tagged cases describe the END-TO-END
// behaviour of the new FilterClassifier-driven Phase A pipeline:
//
//   1. BROADBAND_OSC frame synthesises an "L" slot via rec709 luminance.
//   2. DUAL_NB_OSC HaO3 frame routes into R_HaO3 / G_HaO3 / B_HaO3.
//   3. UNKNOWN FILTER on a Bayer frame fails the batch loud at start.
//   4. Missing FILTER on a Bayer frame is silently treated as OSC.
//   5. Mixed L (mono) + HaO3 (Bayer) batch builds a union slot config.
//
// They depend on the synthetic-FITS writer that Task 20 will add to
// test_util (test_util::write_synthetic_bayer / write_synthetic_mono).
// Until that task lands, the test bodies are gated under #if 0 so the
// file COMPILES against the current test_util — keeping CI green —
// while the literal end-state cases sit ready for Task 20 to wire up.
//
// All five cases are tagged "[.integration]" — Catch2's hidden-tag
// convention — so default `ctest` runs skip them entirely. When Task 20
// drops the writer in, flip `WIRED_BY_TASK_20` to 1 (or remove the gate)
// and the cases will be enabled by passing the [integration] filter to
// the test binary.

#include "catch_amalgamated.hpp"
#include "nukex/stacker/stacking_engine.hpp"
#include "nukex/core/filter.hpp"
#include "nukex/core/cube.hpp"
#include "nukex/core/channel_config.hpp"

#include <filesystem>

#define WIRED_BY_TASK_20 0

using namespace nukex;
namespace fs = std::filesystem;

#if WIRED_BY_TASK_20
#include "test_data_loader.hpp"  // brings in test_util::write_synthetic_*
#endif

// Each case carries the same minimal placeholder body so the test binary
// links until Task 20. When the gate flips, the real bodies (kept inline
// in #if blocks below) become the live cases.

TEST_CASE("Phase A: BROADBAND_OSC frame synthesizes L slot",
          "[.integration][phase_a]") {
#if WIRED_BY_TASK_20
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
    REQUIRE(px.welford[L_idx].mean == Catch::Approx(0.5f).margin(0.05f));
#else
    SKIP("wired by Task 20: synthetic FITS writer needed");
#endif
}

TEST_CASE("Phase A: HaO3 dual-NB frame routes into R_HaO3/G_HaO3/B_HaO3",
          "[.integration][phase_a]") {
#if WIRED_BY_TASK_20
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
#else
    SKIP("wired by Task 20: synthetic FITS writer needed");
#endif
}

TEST_CASE("Phase A: unknown FILTER on Bayer fails the batch loud at start",
          "[.integration][phase_a]") {
#if WIRED_BY_TASK_20
    auto tmp = fs::temp_directory_path() / "phase_a_unknown.fits";
    test_util::write_synthetic_bayer(tmp.string(), 16, 16, "RGGB", "ASI585MC", "ALP-T-fake-2026", 0.5f);

    StackingEngine::Config cfg;
    cfg.qe_database_path = (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();
    StackingEngine engine(cfg);
    auto result = engine.execute({tmp.string()}, {}, nullptr);

    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error.find("ALP-T-fake-2026") != std::string::npos);
    REQUIRE(result.error.find("qe_overrides.json") != std::string::npos);
#else
    SKIP("wired by Task 20: synthetic FITS writer needed");
#endif
}

TEST_CASE("Phase A: missing FILTER on Bayer is silent BROADBAND_OSC",
          "[.integration][phase_a]") {
#if WIRED_BY_TASK_20
    auto tmp = fs::temp_directory_path() / "phase_a_no_filter.fits";
    test_util::write_synthetic_bayer(tmp.string(), 16, 16, "RGGB", "ASI585MC", "", 0.5f);

    StackingEngine::Config cfg;
    cfg.qe_database_path = (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();
    StackingEngine engine(cfg);
    auto result = engine.execute({tmp.string()}, {}, nullptr);

    REQUIRE(result.ok);
    REQUIRE(result.cube->channel_config.slot_index("L") != -1);
#else
    SKIP("wired by Task 20: synthetic FITS writer needed");
#endif
}

TEST_CASE("Phase A: mixed L + HaO3 batch builds union slot config",
          "[.integration][phase_a]") {
#if WIRED_BY_TASK_20
    auto t1 = fs::temp_directory_path() / "phase_a_l.fits";
    auto t2 = fs::temp_directory_path() / "phase_a_hao3_2.fits";
    test_util::write_synthetic_mono(t1.string(), 16, 16, "ASI2600MM", "L", 0.5f);
    test_util::write_synthetic_bayer(t2.string(), 16, 16, "RGGB", "ASI585MC", "HaO3", 0.5f);

    StackingEngine::Config cfg;
    cfg.qe_database_path = (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();
    StackingEngine engine(cfg);
    auto result = engine.execute({t1.string(), t2.string()}, {}, nullptr);

    REQUIRE(result.ok);
    REQUIRE(result.cube->channel_config.slot_index("L")      != -1);
    REQUIRE(result.cube->channel_config.slot_index("R_HaO3") != -1);
    REQUIRE(result.cube->channel_config.slot_index("G_HaO3") != -1);
    REQUIRE(result.cube->channel_config.slot_index("B_HaO3") != -1);
#else
    SKIP("wired by Task 20: synthetic FITS writer needed");
#endif
}
