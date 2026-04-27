#include "catch_amalgamated.hpp"
#include "nukex/classify/weight_computer.hpp"

using namespace nukex;

namespace {

FrameStats make_clean_frame() {
    FrameStats fs;
    fs.frame_weight = 1.0f;
    fs.psf_weight = 1.0f;
    fs.median_luminance = 0.5f;
    fs.read_noise = 3.0f;
    fs.gain = 1.5f;
    fs.has_noise_keywords = true;
    return fs;
}

} // anonymous namespace

TEST_CASE("WeightComputer: clean sample → weight near 1.0", "[classify]") {
    WeightComputer wc;
    FrameStats fs = make_clean_frame();
    float w = wc.compute(0.50f, fs, 0.50f, 0.03f);
    REQUIRE(w > 0.9f);
    REQUIRE(w <= 1.0f);
}

TEST_CASE("WeightComputer: outlier beyond 3σ → reduced weight", "[classify]") {
    WeightComputer wc;
    FrameStats fs = make_clean_frame();
    // value = 0.70, mean = 0.50, stddev = 0.03 → sigma_score ≈ 6.67σ
    // excess = 3.67σ → sigma_factor = exp(-0.5 * 3.67^2 / 4) ≈ 0.185
    float w = wc.compute(0.70f, fs, 0.50f, 0.03f);
    REQUIRE(w < 0.5f);
    REQUIRE(w >= 0.01f);
}

TEST_CASE("WeightComputer: cloud frame → cloud_score applied", "[classify]") {
    WeightComputer wc;
    FrameStats fs = make_clean_frame();
    // Frame-level cloud score: stacker precomputes this from median-of-medians
    fs.cloud_score = 0.30f;
    float w = wc.compute(0.50f, fs, 0.50f, 0.03f);
    REQUIRE(w == Catch::Approx(0.30f).margin(0.05f));
}

TEST_CASE("WeightComputer: failed alignment → weight halved", "[classify]") {
    WeightComputer wc;
    FrameStats fs = make_clean_frame();
    fs.frame_weight = 0.5f;
    float w = wc.compute(0.50f, fs, 0.50f, 0.03f);
    REQUIRE(w == Catch::Approx(0.5f).margin(0.05f));
}

TEST_CASE("WeightComputer: poor seeing → psf_weight reduces", "[classify]") {
    WeightComputer wc;
    FrameStats fs = make_clean_frame();
    fs.psf_weight = 0.6f;
    float w = wc.compute(0.50f, fs, 0.50f, 0.03f);
    REQUIRE(w == Catch::Approx(0.6f).margin(0.05f));
}

TEST_CASE("WeightComputer: weight never below floor", "[classify]") {
    WeightConfig cfg;
    cfg.weight_floor = 0.01f;
    WeightComputer wc(cfg);
    FrameStats fs = make_clean_frame();
    fs.frame_weight = 0.01f;
    fs.psf_weight = 0.01f;
    float w = wc.compute(5.0f, fs, 0.50f, 0.03f);
    REQUIRE(w >= 0.01f);
}

TEST_CASE("WeightComputer: zero stddev → no sigma penalty", "[classify]") {
    WeightComputer wc;
    FrameStats fs = make_clean_frame();
    float w = wc.compute(0.50f, fs, 0.50f, 0.0f);
    REQUIRE(w > 0.9f);
}
