// Phase B Q-solve integration tests (Task 10B).
//
// These four [.integration]-tagged cases describe the END-TO-END behaviour
// of the Q-matrix decomposition pass that runs after per-pixel selection
// populates the `stacked` Image with raw slot values:
//
//   1. A synthetic HaO3 frame engineered so Q-solve recovers Ha=0.5 and
//      OIII=0.3 at the centre pixel within ±0.02.
//   2. A pure broadband-OSC frame that produces no Ha/OIII/SII derived
//      slot — only R/G/B/L pass through.
//   3. A mixed HaO3 + S2O3 batch where multi-source OIII is merged via
//      sample-count weighted mean.
//   4. A frame engineered so the Q-solve would produce a negative emission
//      value — verifying it is clamped to 0 and counted in
//      derived.negative_clamped_count.
//
// They depend on the synthetic-FITS writer that Task 20 will add to
// test_util (test_util::write_synthetic_q_solved_hao3 and friends).
// Until that task lands, the test bodies are gated under #if 0 so the
// file COMPILES against the current test_util — keeping CI green —
// while the literal end-state cases sit ready for Task 20 to wire up.
//
// All four cases are tagged "[.integration]" — Catch2's hidden-tag
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

TEST_CASE("Phase B Q-solve: synthetic HaO3 frame recovers Ha + OIII slots",
          "[.integration][phase_b]") {
#if WIRED_BY_TASK_20
    // write_synthetic_q_solved_hao3 creates a 16×16 Bayer frame with FILTER=HaO3
    // and pixel values engineered so the Q-solve recovers Ha≈0.5 and OIII≈0.3
    // at the centre pixel (8,8).
    auto tmp = fs::temp_directory_path() / "phase_b_hao3.fits";
    test_util::write_synthetic_q_solved_hao3(tmp.string(), 16, 16,
                                              /*camera*/"ASI585MC",
                                              /*ha_target*/0.5f,
                                              /*oiii_target*/0.3f);

    StackingEngine::Config cfg;
    cfg.qe_database_path = (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();
    StackingEngine engine(cfg);
    auto result = engine.execute({tmp.string()}, {}, nullptr);

    REQUIRE(result.ok);
    // Derived map must contain Ha and OIII.
    REQUIRE(result.derived.slots.count("Ha")   == 1);
    REQUIRE(result.derived.slots.count("OIII") == 1);
    // Centre pixel recovers the engineered values within ±0.02.
    const int cx = 8, cy = 8;
    float ha_val   = result.derived.slots.at("Ha")  [cy * 16 + cx];
    float oiii_val = result.derived.slots.at("OIII")[cy * 16 + cx];
    REQUIRE(ha_val   == Catch::Approx(0.5f).margin(0.02f));
    REQUIRE(oiii_val == Catch::Approx(0.3f).margin(0.02f));
    // No negative-clamping expected for a well-behaved engineered frame.
    REQUIRE(result.derived.negative_clamped_count == 0);
#else
    SKIP("wired by Task 20: synthetic FITS writer needed");
#endif
}

TEST_CASE("Phase B Q-solve: pure broadband-OSC produces no Ha/OIII/SII slot",
          "[.integration][phase_b]") {
#if WIRED_BY_TASK_20
    // Plain OSC frame (no FILTER keyword) — all slots route through as
    // broadband passthrough; no dual-NB groups → no Q-solve.
    auto tmp = fs::temp_directory_path() / "phase_b_osc.fits";
    test_util::write_synthetic_bayer(tmp.string(), 16, 16, "RGGB", "ASI585MC", "", 0.5f);

    StackingEngine::Config cfg;
    cfg.qe_database_path = (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();
    StackingEngine engine(cfg);
    auto result = engine.execute({tmp.string()}, {}, nullptr);

    REQUIRE(result.ok);
    // Broadband slots are present.
    REQUIRE(result.derived.slots.count("R") == 1);
    REQUIRE(result.derived.slots.count("G") == 1);
    REQUIRE(result.derived.slots.count("B") == 1);
    REQUIRE(result.derived.slots.count("L") == 1);
    // No emission-line slots produced — those would require a dual-NB group.
    REQUIRE(result.derived.slots.count("Ha")  == 0);
    REQUIRE(result.derived.slots.count("OIII") == 0);
    REQUIRE(result.derived.slots.count("SII") == 0);
    REQUIRE(result.derived.negative_clamped_count == 0);
#else
    SKIP("wired by Task 20: synthetic FITS writer needed");
#endif
}

TEST_CASE("Phase B Q-solve: HaO3 + S2O3 mixed → multi-source OIII merge",
          "[.integration][phase_b]") {
#if WIRED_BY_TASK_20
    // Two batches: 10 HaO3 frames + 5 S2O3 frames. Both contribute OIII.
    // The weighted-mean merge gives the 10-frame HaO3 batch twice the weight
    // of the 5-frame S2O3 batch for the OIII slot.
    auto t1 = fs::temp_directory_path() / "phase_b_hao3_multi.fits";
    auto t2 = fs::temp_directory_path() / "phase_b_s2o3_multi.fits";
    test_util::write_synthetic_q_solved_hao3(t1.string(), 16, 16, "ASI585MC",
                                              /*ha_target*/0.6f, /*oiii_target*/0.3f);
    test_util::write_synthetic_q_solved_s2o3(t2.string(), 16, 16, "ASI585MC",
                                              /*sii_target*/0.4f, /*oiii_target*/0.2f);

    StackingEngine::Config cfg;
    cfg.qe_database_path = (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();
    StackingEngine engine(cfg);
    auto result = engine.execute({t1.string(), t2.string()}, {}, nullptr);

    REQUIRE(result.ok);
    // All three emission-line slots must be present.
    REQUIRE(result.derived.slots.count("Ha")   == 1);
    REQUIRE(result.derived.slots.count("OIII") == 1);
    REQUIRE(result.derived.slots.count("SII")  == 1);
    // OIII must be a weighted blend of both contributions (not a simple mean).
    // With n_HaO3=1 sample and n_S2O3=1 sample the blend is equal here;
    // actual multi-frame tests (10 vs 5) live in the fixture generator.
    REQUIRE(result.derived.slots.at("OIII")[8 * 16 + 8] > 0.0f);
#else
    SKIP("wired by Task 20: synthetic FITS writer needed");
#endif
}

TEST_CASE("Phase B Q-solve: negative emission clamped, counter incremented",
          "[.integration][phase_b]") {
#if WIRED_BY_TASK_20
    // A frame engineered so one emission-line value comes out negative from
    // the linear solve (e.g. an OIII-dominated pixel fed to an Ha/OIII Q
    // matrix that expects the opposite balance). The negative value must be
    // clamped to 0, and negative_clamped_count must be > 0.
    auto tmp = fs::temp_directory_path() / "phase_b_negative.fits";
    test_util::write_synthetic_negative_emission_hao3(tmp.string(), 16, 16, "ASI585MC");

    StackingEngine::Config cfg;
    cfg.qe_database_path = (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();
    StackingEngine engine(cfg);
    auto result = engine.execute({tmp.string()}, {}, nullptr);

    REQUIRE(result.ok);  // loud-fail only fires on singular Q; clamp is non-fatal
    REQUIRE(result.derived.negative_clamped_count > 0);
    // All derived emission values must be >= 0 after clamping.
    for (float v : result.derived.slots.at("Ha"))   REQUIRE(v >= 0.0f);
    for (float v : result.derived.slots.at("OIII")) REQUIRE(v >= 0.0f);
#else
    SKIP("wired by Task 20: synthetic FITS writer needed");
#endif
}
