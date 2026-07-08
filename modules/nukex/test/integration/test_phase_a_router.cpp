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
#include "nukex/calibration/qe_database.hpp"
#include "nukex/calibration/channel_decomposer.hpp"
#include <Eigen/Dense>

#include <filesystem>

#define WIRED_BY_TASK_20 1

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

// Regression test for the "stale first-frame Bayer pattern" defect
// diagnosed in task-20-report.md (pipeline defect #2): the engine used
// to compute ONE BayerPattern from light_paths[0] and reuse it for every
// frame's debayer step, instead of each frame's own metadata. With a
// mono frame first, the batch-wide pattern latches to NONE, so a genuine
// Bayer frame arriving later in the batch never gets debayered — its raw
// 1-channel mosaic image is then routed through DUAL_NB_OSC accumulation,
// which unconditionally reads channels 1 and 2 (out of bounds for a
// 1-channel image).
//
// The prior "mixed batch" case above only asserted slot *names* were
// registered (via ChannelConfig::merge(), independent of whether
// debayering actually ran), so it never observed this. This case instead
// engineers a Bayer HaO3 frame via write_synthetic_q_solved_hao3, whose
// R/G/B photosite values are deliberately distinct (Q * (Ha, OIII) for
// non-degenerate Ha != OIII), and checks the PHASE A voxel welford means
// directly (cube.at(x,y).welford[idx].mean) -- populated synchronously
// during Phase A accumulation, before Phase B's distribution fitting or
// FrameCache reads ever run. Recovering the correct, distinct R/G/B means
// for the R_HaO3/G_HaO3/B_HaO3 slots requires that ALL THREE debayered
// channels for that frame carry their correct values. If the Bayer frame
// is silently skipped (bug), the DUAL_NB_OSC accumulator reads
// aligned.image.at(x,y,1)/.at(x,y,2) out of bounds past the end of a
// 1-channel image buffer, corrupting the R_HaO3/G_HaO3/B_HaO3 welford
// means.
//
// (Asserting via Phase A's welford stats rather than Phase B's derived
// Q-solve output is deliberate: it isolates this specific defect from
// Phase B/FrameCache machinery, which has its own separate concerns.)
TEST_CASE("Phase A: mono-then-Bayer batch debayers each frame with its OWN pattern",
          "[.integration][phase_a]") {
#if WIRED_BY_TASK_20
    auto t1 = fs::temp_directory_path() / "phase_a_regression_mono.fits";
    auto t2 = fs::temp_directory_path() / "phase_a_regression_hao3.fits";
    // Mono L frame FIRST -- this is what latches the stale outer `bayer`
    // variable to NONE for the whole batch prior to the fix.
    test_util::write_synthetic_mono(t1.string(), 16, 16, "ASI2600MM", "L", 0.6f);
    // Genuine Bayer HaO3 frame SECOND, with distinct engineered Ha/OIII so
    // R/G/B are not all equal (a uniform-value Bayer frame can't
    // distinguish "correctly debayered" from "never debayered").
    const float ha_target   = 0.5f;
    const float oiii_target = 0.3f;
    test_util::write_synthetic_q_solved_hao3(t2.string(), 16, 16, "ASI585MC",
                                              ha_target, oiii_target);

    // Compute the expected (distinct) R/G/B photosite values the same way
    // write_synthetic_q_solved_hao3 did internally (Q * (Ha, OIII)), so we
    // can assert the debayered per-frame values landed correctly rather
    // than round-tripping through Phase B's Q-solve.
    QEDatabase qe_db;
    auto load_result = qe_db.load_shipped(
        (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string());
    REQUIRE(load_result.ok);
    ChannelDecomposer decomposer(qe_db);
    Eigen::MatrixXd Q = decomposer.build_q("ASI585MC", "HaO3"); // 3x2
    Eigen::Vector2d truth(ha_target, oiii_target);
    Eigen::Vector3d expected_rgb = Q * truth;

    StackingEngine::Config cfg;
    cfg.qe_database_path = (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();
    StackingEngine engine(cfg);
    auto result = engine.execute({t1.string(), t2.string()}, {}, nullptr);

    REQUIRE(result.ok);

    // Sanity: the mono frame's own slot is unaffected.
    int l_idx = result.cube->channel_config.slot_index("L");
    REQUIRE(l_idx != -1);
    REQUIRE(result.cube->at(8, 8).welford[l_idx].mean == Catch::Approx(0.6f).margin(0.05f));

    // The smoking gun: the Bayer frame's slots exist AND carry the correct,
    // distinct per-channel values -- not garbage from an out-of-bounds read
    // past a 1-channel image buffer.
    int r_idx = result.cube->channel_config.slot_index("R_HaO3");
    int g_idx = result.cube->channel_config.slot_index("G_HaO3");
    int b_idx = result.cube->channel_config.slot_index("B_HaO3");
    REQUIRE(r_idx != -1);
    REQUIRE(g_idx != -1);
    REQUIRE(b_idx != -1);

    auto& voxel = result.cube->at(8, 8);
    REQUIRE(voxel.welford[r_idx].mean == Catch::Approx(expected_rgb(0)).margin(0.01f));
    REQUIRE(voxel.welford[g_idx].mean == Catch::Approx(expected_rgb(1)).margin(0.01f));
    REQUIRE(voxel.welford[b_idx].mean == Catch::Approx(expected_rgb(2)).margin(0.01f));
#else
    SKIP("wired by Task 20: synthetic FITS writer needed");
#endif
}
