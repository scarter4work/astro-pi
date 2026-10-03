#include "catch_amalgamated.hpp"
#include "nukex/combine/output_assembler.hpp"
#include "nukex/core/cube.hpp"
#include "nukex/core/channel_config.hpp"
#include "nukex/core/filter.hpp"
#include <cmath>
#include <random>

using namespace nukex;

static ChannelConfig cfg_for(FilterClass cls, const char* name) {
    Filter f;
    f.cls  = cls;
    f.name = name;
    return ChannelConfig::from_filter(f);
}

TEST_CASE("OutputAssembler: quality map has 4 channels", "[assembler]") {
    auto config = cfg_for(FilterClass::BROADBAND_L, "L");
    Cube cube(8, 8, config);

    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            auto& v = cube.at(x, y);
            v.channel(0).distribution.shape = DistributionShape::GAUSSIAN;
            v.channel(0).distribution.true_signal_estimate = 0.5f;
            v.channel(0).distribution.signal_uncertainty = 0.01f;
            v.channel(0).distribution.confidence = 0.9f;
            v.dominant_shape = DistributionShape::GAUSSIAN;
        }

    auto quality = OutputAssembler::assemble_quality_map(cube);
    REQUIRE(quality.width() == 8);
    REQUIRE(quality.height() == 8);
    REQUIRE(quality.n_channels() == 4);
    REQUIRE(quality.at(4, 4, 0) == Catch::Approx(0.5f));
    REQUIRE(quality.at(4, 4, 1) == Catch::Approx(0.01f));
    REQUIRE(quality.at(4, 4, 2) == Catch::Approx(0.9f));
    REQUIRE(quality.at(4, 4, 3) == Catch::Approx(0.0f));
}

TEST_CASE("OutputAssembler: shape channel encodes dominant_shape correctly", "[assembler]") {
    auto config = cfg_for(FilterClass::BROADBAND_L, "L");
    Cube cube(4, 4, config);

    cube.at(0, 0).dominant_shape = DistributionShape::GAUSSIAN;
    cube.at(1, 0).dominant_shape = DistributionShape::BIMODAL;
    cube.at(2, 0).dominant_shape = DistributionShape::HEAVY_TAILED;
    cube.at(3, 0).dominant_shape = DistributionShape::CONTAMINATED;

    auto quality = OutputAssembler::assemble_quality_map(cube);
    REQUIRE(quality.at(0, 0, 3) == Catch::Approx(0.0f));
    REQUIRE(quality.at(1, 0, 3) == Catch::Approx(1.0f));
    REQUIRE(quality.at(2, 0, 3) == Catch::Approx(2.0f));
    REQUIRE(quality.at(3, 0, 3) == Catch::Approx(3.0f));
}

// ── Measured noise: the stack's realised scatter, surfaced ──
//
// NukeX's noise_map is the PREDICTED uncertainty of each estimate (a CCD
// model, or Welford across frames). It cannot see noise the estimator itself
// injects, which is how a 1.05-1.47x penalty survived several releases.
// assemble_measured_noise surfaces the realised pixel-to-pixel scatter beside
// it, and the ratio of the two is the instrument that would have caught it.

TEST_CASE("OutputAssembler: measured noise map carries local_rms", "[assembler]") {
    auto config = cfg_for(FilterClass::BROADBAND_L, "L");
    Cube cube(8, 8, config);
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            cube.at(x, y).local_rms = 0.25f * static_cast<float>(x);

    auto measured = OutputAssembler::assemble_measured_noise(cube);
    REQUIRE(measured.width() == 8);
    REQUIRE(measured.height() == 8);
    REQUIRE(measured.n_channels() == 1);
    REQUIRE(measured.at(0, 3, 0) == Catch::Approx(0.0f));
    REQUIRE(measured.at(4, 3, 0) == Catch::Approx(1.0f));
}

TEST_CASE("OutputAssembler: noise check is 1 when measured and predicted agree (mono)",
          "[assembler]") {
    Image measured(8, 8, 1);
    Image predicted(8, 8, 1);
    measured.fill(0.02f);
    predicted.fill(0.02f);
    const auto nc = OutputAssembler::noise_check(measured, predicted);
    REQUIRE(nc.comparable);
    REQUIRE(nc.ratio == Catch::Approx(1.0).epsilon(1e-4));
}

TEST_CASE("OutputAssembler: noise check reports an inflated measurement", "[assembler]") {
    Image measured(8, 8, 1);
    Image predicted(8, 8, 1);
    measured.fill(0.03f);
    predicted.fill(0.02f);
    const auto nc = OutputAssembler::noise_check(measured, predicted);
    REQUIRE(nc.ratio == Catch::Approx(1.5).epsilon(1e-4));
}

TEST_CASE("OutputAssembler: noise check combines colour channels in quadrature",
          "[assembler]") {
    // Measured noise is a LUMINANCE quantity (the spatial kernel builds a
    // luminance window), so a 3-channel predicted map must be combined the
    // same way before the two are comparable:
    //   sigma_L = sqrt((0.2126 s_R)^2 + (0.7152 s_G)^2 + (0.0722 s_B)^2)
    // With every channel at 1.0 that is 0.74961, so a measured map holding
    // 0.74961 must read as a ratio of exactly 1.
    Image predicted(4, 4, 3);
    predicted.fill(1.0f);
    Image measured(4, 4, 1);
    measured.fill(0.74961f);
    const auto nc = OutputAssembler::noise_check(measured, predicted);
    REQUIRE(nc.predicted_median == Catch::Approx(0.74961).epsilon(1e-3));
    REQUIRE(nc.ratio == Catch::Approx(1.0).epsilon(1e-3));
}

TEST_CASE("OutputAssembler: the three printed numbers reconcile on a real-shaped map",
          "[assembler]") {
    // Not a constant image: per-pixel measured/predicted varies, so a median
    // of ratios would NOT equal the ratio of medians. The console prints the
    // two medians and the ratio; dividing the first two must give the third.
    Image predicted(16, 16, 3);
    Image measured(16, 16, 1);
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            for (int c = 0; c < 3; c++) predicted.at(x, y, c) = 0.01f + 0.001f * ((x * 3 + y * 5 + c) % 11);
            measured.at(x, y, 0) = 0.012f + 0.002f * ((x * 7 + y) % 13);
        }
    const auto nc = OutputAssembler::noise_check(measured, predicted);
    REQUIRE(nc.comparable);
    REQUIRE(nc.ratio == Catch::Approx(nc.measured_median / nc.predicted_median).epsilon(1e-9));
}

TEST_CASE("OutputAssembler: noise check is not comparable when a map is empty or zero",
          "[assembler]") {
    Image measured(8, 8, 1);
    Image predicted(8, 8, 1);
    measured.fill(0.02f);
    predicted.fill(0.0f);
    REQUIRE_FALSE(OutputAssembler::noise_check(measured, predicted).comparable);
    REQUIRE_FALSE(OutputAssembler::noise_check(Image{}, predicted).comparable);
}

// ── Stochastic vs fixed-pattern: the half-stack decomposition ────────────

TEST_CASE("OutputAssembler: noise decomposition recovers stochastic and fixed-pattern "
          "shares from two half-stacks", "[assembler]") {
    // Full stack = fixed pattern + white noise s. Each half = the same fixed
    // pattern + white noise s*sqrt(2). The kernel measured the full stack's
    // noise: sqrt(s^2 + f^2). The decomposition must give s back, and f.
    const int W = 200, H = 160;
    const float s = 0.010f, f = 0.012f;
    std::mt19937 rng(21);
    std::normal_distribution<float> gn(0.0f, s * std::sqrt(2.0f));
    std::normal_distribution<float> gf(0.0f, f);
    Image even(W, H, 1), odd(W, H, 1);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const float fixed = gf(rng);     // per-pixel, identical in both halves
            even.at(x, y, 0) = 0.3f + fixed + gn(rng);
            odd.at(x, y, 0)  = 0.3f + fixed + gn(rng);
        }
    const double measured = std::sqrt(static_cast<double>(s) * s + static_cast<double>(f) * f);
    const auto nd = OutputAssembler::noise_decomposition(measured, even, odd);
    REQUIRE(nd.valid);
    REQUIRE(nd.stochastic == Catch::Approx(s).epsilon(0.12));
    REQUIRE(nd.fixed == Catch::Approx(f).epsilon(0.15));
    REQUIRE(nd.fixed_share == Catch::Approx(f * f / (s * s + f * f)).epsilon(0.2));
}

TEST_CASE("OutputAssembler: with no fixed pattern the fixed share is ~0", "[assembler]") {
    const int W = 200, H = 160;
    const float s = 0.010f;
    std::mt19937 rng(22);
    std::normal_distribution<float> gn(0.0f, s * std::sqrt(2.0f));
    Image even(W, H, 1), odd(W, H, 1);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) { even.at(x, y, 0) = 0.3f + gn(rng); odd.at(x, y, 0) = 0.3f + gn(rng); }
    const auto nd = OutputAssembler::noise_decomposition(s, even, odd);
    REQUIRE(nd.valid);
    REQUIRE(nd.stochastic == Catch::Approx(s).epsilon(0.12));
    REQUIRE(nd.fixed_share < 0.25);
}
