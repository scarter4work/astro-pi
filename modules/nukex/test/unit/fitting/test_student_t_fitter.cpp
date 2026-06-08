#include "catch_amalgamated.hpp"
#include "nukex/fitting/student_t_fitter.hpp"
#include "nukex/fitting/robust_stats.hpp"
#include <vector>
#include <random>
#include <cmath>

using namespace nukex;

// ============================================================
// Test 1: Clean Gaussian recovery (N=200)
// Generate N(0.42, 0.03^2) samples. The Student-t fitter should
// recover mu ~= 0.42, sigma ~= 0.03, and push nu high (> 20),
// classifying as GAUSSIAN.
// ============================================================
TEST_CASE("StudentTFitter: clean Gaussian recovery", "[student_t]") {
    constexpr int N = 200;
    std::mt19937 rng(12345);
    std::normal_distribution<float> dist(0.42f, 0.03f);

    std::vector<float> values(N);
    std::vector<float> weights(N, 1.0f);
    for (int i = 0; i < N; ++i) {
        values[i] = dist(rng);
    }

    // Compute robust seeds
    double loc   = biweight_location(values.data(), N);
    double scale = mad(values.data(), N) * 1.4826;  // MAD-based sigma estimate

    StudentTFitter fitter;
    FitResult result = fitter.fit(values.data(), weights.data(), N, loc, scale);

    REQUIRE(result.converged);
    REQUIRE(result.n_params == 3);
    REQUIRE(result.n_samples == N);

    double mu    = result.distribution.params.student_t.mu;
    double sigma = result.distribution.params.student_t.sigma;
    double nu    = result.distribution.params.student_t.nu;

    // mu should be close to 0.42
    REQUIRE(mu == Catch::Approx(0.42).margin(0.01));

    // sigma should be close to 0.03
    REQUIRE(sigma == Catch::Approx(0.03).margin(0.01));

    // nu should be high for Gaussian data (> 20)
    REQUIRE(nu > 20.0);

    // Shape should be classified as GAUSSIAN (nu > 30 threshold)
    REQUIRE(result.distribution.shape == DistributionShape::GAUSSIAN);
}

// ============================================================
// Test 2: Heavy-tailed recovery (N=200)
// Generate Student-t(0.50, 0.02, nu=4) samples. The fitter should
// recover the location and detect heavy tails.
// ============================================================
TEST_CASE("StudentTFitter: heavy-tailed recovery", "[student_t]") {
    constexpr int N = 200;
    std::mt19937 rng(54321);
    std::student_t_distribution<double> tdist(4.0);

    std::vector<float> values(N);
    std::vector<float> weights(N, 1.0f);
    for (int i = 0; i < N; ++i) {
        // Student-t location-scale: x = mu + sigma * t
        double t = tdist(rng);
        values[i] = static_cast<float>(0.50 + 0.02 * t);
    }

    double loc   = biweight_location(values.data(), N);
    double scale = mad(values.data(), N) * 1.4826;

    StudentTFitter fitter;
    FitResult result = fitter.fit(values.data(), weights.data(), N, loc, scale);

    REQUIRE(result.converged);

    double mu = result.distribution.params.student_t.mu;
    double nu = result.distribution.params.student_t.nu;

    // mu should be close to 0.50
    REQUIRE(mu == Catch::Approx(0.50).margin(0.02));

    // nu should be low for heavy-tailed data
    REQUIRE(nu < 15.0);

    // Shape should be HEAVY_TAILED
    REQUIRE(result.distribution.shape == DistributionShape::HEAVY_TAILED);
}

// ============================================================
// Test 3: Robust to single outlier (N=101)
// 100 samples from N(0.50, 0.02^2), plus one extreme outlier at 5.0.
// Student-t should downweight the outlier and recover mu ~= 0.50.
// ============================================================
TEST_CASE("StudentTFitter: robust to single outlier", "[student_t]") {
    constexpr int N_clean = 100;
    std::mt19937 rng(99999);
    std::normal_distribution<float> dist(0.50f, 0.02f);

    std::vector<float> values;
    std::vector<float> weights;
    values.reserve(N_clean + 1);
    weights.reserve(N_clean + 1);

    for (int i = 0; i < N_clean; ++i) {
        values.push_back(dist(rng));
        weights.push_back(1.0f);
    }
    // Add extreme outlier
    values.push_back(5.0f);
    weights.push_back(1.0f);

    int n = static_cast<int>(values.size());
    double loc   = biweight_location(values.data(), n);
    double scale = mad(values.data(), n) * 1.4826;

    StudentTFitter fitter;
    FitResult result = fitter.fit(values.data(), weights.data(), n, loc, scale);

    REQUIRE(result.converged);

    double mu = result.distribution.params.student_t.mu;

    // The outlier at 5.0 should NOT pull the estimate away from 0.50
    REQUIRE(mu == Catch::Approx(0.50).margin(0.02));
}

// ============================================================
// Test 4: AICc is finite and negative
// Generate N(0.5, 0.05^2) with N=64. A good fit to its own data
// should produce a finite, negative AICc.
// ============================================================
TEST_CASE("StudentTFitter: AICc is finite and negative", "[student_t]") {
    constexpr int N = 64;
    std::mt19937 rng(77777);
    std::normal_distribution<float> dist(0.5f, 0.05f);

    std::vector<float> values(N);
    std::vector<float> weights(N, 1.0f);
    for (int i = 0; i < N; ++i) {
        values[i] = dist(rng);
    }

    double loc   = biweight_location(values.data(), N);
    double scale = mad(values.data(), N) * 1.4826;

    StudentTFitter fitter;
    FitResult result = fitter.fit(values.data(), weights.data(), N, loc, scale);

    REQUIRE(result.converged);
    REQUIRE(result.n_params == 3);
    REQUIRE(result.n_samples == 64);

    double aicc = result.aicc();
    REQUIRE(std::isfinite(aicc));
    REQUIRE(aicc < 0.0);
}

// ============================================================
// Test 5: Too few samples does not converge
// N=3 is below the minimum threshold of 5.
// ============================================================
TEST_CASE("StudentTFitter: too few samples does not converge", "[student_t]") {
    float values[]  = {0.5f, 0.51f, 0.49f};
    float weights[] = {1.0f, 1.0f, 1.0f};

    StudentTFitter fitter;
    FitResult result = fitter.fit(values, weights, 3, 0.5, 0.01);

    REQUIRE_FALSE(result.converged);
}

// ============================================================
// Test 6: Extreme outlier does not crash the fitter
// A single value of 1e30 among normal data would push the gradient
// (or cost) to inf/NaN on some Ceres line-search steps.  Before the
// isfinite guard in Evaluate(), that reached ceres/line_search.cc:705
// and CHECK-aborted the process 26 min into a 65-frame NGC7635 stack.
// This test exercises the guard: fit must return — converged or not —
// without throwing or aborting.
// ============================================================
TEST_CASE("StudentTFitter: extreme outlier does not crash", "[student_t]") {
    constexpr int N = 64;
    std::mt19937 rng(31337);
    std::normal_distribution<float> dist(0.50f, 0.02f);

    std::vector<float> values(N);
    std::vector<float> weights(N, 1.0f);
    for (int i = 0; i < N - 1; ++i) values[i] = dist(rng);
    values[N - 1] = 1.0e30f;   // Pathological outlier.

    double loc   = biweight_location(values.data(), N);
    double scale = mad(values.data(), N) * 1.4826;

    StudentTFitter fitter;
    FitResult result = fitter.fit(values.data(), weights.data(), N, loc, scale);

    // We don't care whether it converges — only that it returns
    // without aborting the process and that any fitted parameters
    // are finite.
    if (result.converged) {
        REQUIRE(std::isfinite(result.distribution.params.student_t.mu));
        REQUIRE(std::isfinite(result.distribution.params.student_t.sigma));
        REQUIRE(std::isfinite(result.distribution.params.student_t.nu));
    }
    SUCCEED("fit() returned instead of CHECK-aborting");
}

// ============================================================
// Test 7: All-identical samples don't crash the fitter
// Constant data has zero variance; sigma can collapse to a value
// where log(nu*pi*sigma^2) + NLL terms go to -inf, and nearby
// parameter probes produce NaN.  Guard must catch it.
// ============================================================
TEST_CASE("StudentTFitter: all-identical samples do not crash", "[student_t]") {
    constexpr int N = 32;
    std::vector<float> values(N, 0.42f);
    std::vector<float> weights(N, 1.0f);

    StudentTFitter fitter;
    // robust_scale of zero would short-circuit via the n<5 guard in fit();
    // use a tiny positive scale so we actually enter Ceres with nearly-
    // constant data.
    FitResult result = fitter.fit(values.data(), weights.data(), N, 0.42, 1e-6);

    if (result.converged) {
        REQUIRE(std::isfinite(result.distribution.params.student_t.mu));
        REQUIRE(std::isfinite(result.distribution.params.student_t.sigma));
        REQUIRE(std::isfinite(result.distribution.params.student_t.nu));
    }
    SUCCEED("fit() returned instead of CHECK-aborting");
}
