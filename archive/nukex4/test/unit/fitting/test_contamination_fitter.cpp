#include "catch_amalgamated.hpp"
#include "nukex/fitting/contamination_fitter.hpp"
#include "nukex/fitting/robust_stats.hpp"
#include <vector>
#include <random>
#include <cmath>

using namespace nukex;

// ============================================================
// Test 1: Gaussian + 10% uniform contamination (N=200)
//
// 90% of samples from N(0.45, 0.02²), 10% from U(0, 1).
// The fitter should recover μ ≈ 0.45 and detect a meaningful ε.
// Shape must be CONTAMINATED.
// ============================================================
TEST_CASE("ContaminationFitter: recovers mu from 10% uniform contamination", "[contamination]") {
    constexpr int N = 200;
    std::mt19937 rng(42001);
    std::normal_distribution<float>  gauss(0.45f, 0.02f);
    std::uniform_real_distribution<float> uniform(0.0f, 1.0f);
    std::bernoulli_distribution contaminated(0.10);   // 10% outlier rate

    std::vector<float> values(N);
    std::vector<float> weights(N, 1.0f);
    for (int i = 0; i < N; ++i) {
        values[i] = contaminated(rng) ? uniform(rng) : gauss(rng);
    }

    double loc   = biweight_location(values.data(), N);
    double scale = mad(values.data(), N) * 1.4826;

    ContaminationFitter fitter;
    FitResult result = fitter.fit(values.data(), weights.data(), N, loc, scale);

    REQUIRE(result.converged);
    REQUIRE(result.n_params == 3);
    REQUIRE(result.n_samples == N);
    REQUIRE(result.distribution.shape == DistributionShape::CONTAMINATED);

    const double mu  = result.distribution.params.contamination.mu;
    const double eps = result.distribution.params.contamination.contamination_frac;

    // Location must be close to true signal (0.45)
    REQUIRE(mu == Catch::Approx(0.45).margin(0.03));

    // Contamination fraction should lie in a plausible range.
    // True ε = 0.10; allow wide margin because N=200 is modest.
    REQUIRE(eps > 0.02);
    REQUIRE(eps < 0.25);
}

// ============================================================
// Test 2: Pure Gaussian data → ε stays small (N=100)
//
// All samples from N(0.50, 0.03²). With no real contamination,
// the MLE should converge to a small ε (< 0.1).
// ============================================================
TEST_CASE("ContaminationFitter: pure Gaussian data yields small epsilon", "[contamination]") {
    constexpr int N = 100;
    std::mt19937 rng(42002);
    std::normal_distribution<float> dist(0.50f, 0.03f);

    std::vector<float> values(N);
    std::vector<float> weights(N, 1.0f);
    for (int i = 0; i < N; ++i) {
        values[i] = dist(rng);
    }

    double loc   = biweight_location(values.data(), N);
    double scale = mad(values.data(), N) * 1.4826;

    ContaminationFitter fitter;
    FitResult result = fitter.fit(values.data(), weights.data(), N, loc, scale);

    REQUIRE(result.converged);

    const double eps = result.distribution.params.contamination.contamination_frac;

    // No real contamination — ε should be driven to the lower end
    REQUIRE(eps < 0.10);
}

// ============================================================
// Test 3: AICc is computed and finite (N=64)
//
// Verify converged=true, n_params=3, and aicc() returns a finite value.
// ============================================================
TEST_CASE("ContaminationFitter: AICc is finite for N=64", "[contamination]") {
    constexpr int N = 64;
    std::mt19937 rng(42003);
    std::normal_distribution<float>  gauss(0.50f, 0.02f);
    std::uniform_real_distribution<float> uniform(0.0f, 1.0f);
    std::bernoulli_distribution contaminated(0.10);

    std::vector<float> values(N);
    std::vector<float> weights(N, 1.0f);
    for (int i = 0; i < N; ++i) {
        values[i] = contaminated(rng) ? uniform(rng) : gauss(rng);
    }

    double loc   = biweight_location(values.data(), N);
    double scale = mad(values.data(), N) * 1.4826;

    ContaminationFitter fitter;
    FitResult result = fitter.fit(values.data(), weights.data(), N, loc, scale);

    REQUIRE(result.converged);
    REQUIRE(result.n_params == 3);

    const double aicc_val = result.aicc();
    REQUIRE(std::isfinite(aicc_val));
}


// ============================================================
// Pathological input: extreme outlier must not crash the fitter.
// Guards in ContaminationNLL::Evaluate return false instead of
// letting a NaN/inf cost or gradient reach Ceres' Wolfe Zoom,
// which would LOG(FATAL) in ceres/line_search.cc:705.
// ============================================================
TEST_CASE("ContaminationFitter: extreme outlier does not crash",
          "[contamination]") {
    constexpr int N = 64;
    std::mt19937 rng(4242);
    std::normal_distribution<float> dist(0.50f, 0.02f);

    std::vector<float> values(N);
    std::vector<float> weights(N, 1.0f);
    for (int i = 0; i < N - 1; ++i) values[i] = dist(rng);
    values[N - 1] = 1.0e30f;

    double loc   = biweight_location(values.data(), N);
    double scale = mad(values.data(), N) * 1.4826;

    ContaminationFitter fitter;
    FitResult result = fitter.fit(values.data(), weights.data(), N, loc, scale);

    if (result.converged) {
        REQUIRE(std::isfinite(result.distribution.params.contamination.mu));
        REQUIRE(std::isfinite(result.distribution.params.contamination.sigma));
        REQUIRE(std::isfinite(result.distribution.params.contamination.contamination_frac));
    }
    SUCCEED("fit() returned instead of CHECK-aborting");
}
