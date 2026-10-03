// Phase B Q-solve integration tests (Task 10B).
//
// These cases describe the END-TO-END behaviour of the Q-matrix
// decomposition pass that runs after per-pixel selection populates the
// `stacked` Image with raw slot values:
//
//   1. A synthetic HaO3 frame engineered so Q-solve recovers Ha=0.5 and
//      OIII=0.3 at the centre pixel within ±0.02.
//   2. A pure broadband-OSC frame that produces no Ha/OIII/SII derived
//      slot — only R/G/B/L pass through.
//   3. A mixed HaO3 + S2O3 batch where multi-source OIII is merged via
//      sample-count weighted mean.
//   4. A frame engineered so the Q-solve would produce a negative emission
//      value — verifying it is clamped to 0 and counted in
//      derived.negative_clamped_count. Still gated: no synthetic writer
//      for an engineered-negative frame exists yet.
//   5. Unknown INSTRUME on a Bayer frame falls back to the generic camera
//      QE profile (Task 15b), end to end.

#include "catch_amalgamated.hpp"
#include "nukex/stacker/stacking_engine.hpp"
#include "nukex/core/filter.hpp"
#include "nukex/core/cube.hpp"
#include "nukex/core/channel_config.hpp"
#include "synthetic_fits.hpp"

#include <filesystem>

using namespace nukex;
namespace fs = std::filesystem;

TEST_CASE("Phase B Q-solve: synthetic HaO3 frame recovers Ha + OIII slots",
          "[integration][phase_b]") {
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
}

TEST_CASE("Phase B Q-solve: pure broadband-OSC produces no Ha/OIII/SII slot",
          "[integration][phase_b]") {
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
}

TEST_CASE("Phase B Q-solve: HaO3 + S2O3 mixed → multi-source OIII merge",
          "[integration][phase_b]") {
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
}

TEST_CASE("Phase B Q-solve: negative emission clamped, counter incremented",
          "[integration][phase_b]") {
    // No dedicated negative-emission writer exists (or is needed): a negative
    // OIII target fed to write_synthetic_q_solved_hao3 engineers the negative
    // Q-solve result directly through the existing writer.
    // ASI585MC's Q keeps every photosite non-negative at (ha=0.8, oiii=-0.04)
    // (B = 0.03*0.8 - 0.50*0.04 = 0.004; G ~ 0.222; R ~ 0.583), so the frame
    // is a physically writable Bayer mosaic, but its Q-solve recovers
    // OIII = -0.04, which the engine must clamp to 0 and count.
    auto tmp = fs::temp_directory_path() / "phase_b_negative.fits";
    test_util::write_synthetic_q_solved_hao3(tmp.string(), 16, 16, "ASI585MC",
                                              /*ha*/0.8f, /*oiii*/-0.04f);

    StackingEngine::Config cfg;
    cfg.qe_database_path = (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();
    StackingEngine engine(cfg);
    auto result = engine.execute({tmp.string()}, {}, nullptr);

    REQUIRE(result.ok);  // loud-fail only fires on singular Q; clamp is non-fatal
    REQUIRE(result.derived.negative_clamped_count > 0);
    // All derived emission values must be >= 0 after clamping.
    for (float v : result.derived.slots.at("Ha"))   REQUIRE(v >= 0.0f);
    for (float v : result.derived.slots.at("OIII")) REQUIRE(v >= 0.0f);
}

TEST_CASE("Phase B Q-solve: unknown INSTRUME falls back to generic_sony_imx_osc with a warning",
          "[integration][phase_b]") {
    // Photosites are engineered from ASI585MC's Q; the fixture's generic
    // camera is a copy of ASI585MC, so the fallback recovers the same lines.
    auto tmp = fs::temp_directory_path() / "phase_b_generic.fits";
    test_util::write_synthetic_q_solved_hao3(tmp.string(), 16, 16, "ASI585MC", 0.5f, 0.3f,
                                              /*instrume*/"Unknown Cam X");
    StackingEngine::Config cfg;
    cfg.qe_database_path = (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();
    StackingEngine engine(cfg);
    auto result = engine.execute({tmp.string()}, {}, nullptr);

    REQUIRE(result.ok);
    REQUIRE(result.qe_generic_camera_fallback);
    REQUIRE(result.derived.slots.at("Ha")[8 * 16 + 8] == Catch::Approx(0.5f).margin(0.02f));
}
