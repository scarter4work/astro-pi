#include "catch_amalgamated.hpp"
#include "nukex/fitting/gmm_fitter.hpp"
#include "nukex/fitting/robust_stats.hpp"
#include <vector>
#include <random>
#include <cmath>

using namespace nukex;

namespace {

std::vector<float> generate_bimodal(float mu1, float sigma1, float mu2, float sigma2,
                                     float pi1, int n, unsigned seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<float> d1(mu1, sigma1);
    std::normal_distribution<float> d2(mu2, sigma2);
    std::bernoulli_distribution bern(pi1);
    std::vector<float> samples(n);
    for (int i = 0; i < n; i++) {
        samples[i] = bern(rng) ? d1(rng) : d2(rng);
    }
    return samples;
}

std::vector<float> uniform_weights(int n) {
    return std::vector<float>(n, 1.0f);
}

} // anonymous namespace

// ============================================================
// Test 1: Recover two well-separated Gaussian components.
// pi1 = 0.6 → comp1 (mu=0.3) is dominant; after canonicalization
// comp1.mu < comp2.mu, and comp1.mu ≈ 0.3, comp2.mu ≈ 0.7.
// ============================================================
TEST_CASE("GaussianMixtureFitter: recovers two well-separated components", "[gmm]") {
    auto data = generate_bimodal(0.3f, 0.02f, 0.7f, 0.02f, 0.6f, 200, 11111);
    auto wt = uniform_weights(200);
    float rl = biweight_location(data.data(), 200);
    float rs = mad(data.data(), 200) * 1.4826f;

    GaussianMixtureFitter fitter;
    auto result = fitter.fit(data.data(), wt.data(), 200, rl, rs);

    REQUIRE(result.converged);
    REQUIRE(result.distribution.shape == DistributionShape::BIMODAL);
    auto& b = result.distribution.params.bimodal;
    REQUIRE(b.comp1.mu == Catch::Approx(0.3f).margin(0.05f));
    REQUIRE(b.comp2.mu == Catch::Approx(0.7f).margin(0.05f));
}

// ============================================================
// Test 2: One dominant component + small outlier cluster → SPIKE_OUTLIER.
// 95 samples near 0.5, 5 samples near 0.9. The tiny component
// has pi ≈ 0.05, so dominant_pi ≈ 0.95 — triggers SPIKE_OUTLIER.
// Signal estimate should land near the dominant component (0.5).
// ============================================================
TEST_CASE("GaussianMixtureFitter: single outlier cluster → SPIKE_OUTLIER", "[gmm]") {
    std::vector<float> data;
    std::mt19937 rng(22222);
    std::normal_distribution<float> clean(0.5f, 0.02f);
    for (int i = 0; i < 95; i++) data.push_back(clean(rng));
    std::normal_distribution<float> outlier(0.9f, 0.01f);
    for (int i = 0; i < 5; i++) data.push_back(outlier(rng));
    auto wt = uniform_weights(100);
    float rl = biweight_location(data.data(), 100);
    float rs = mad(data.data(), 100) * 1.4826f;

    GaussianMixtureFitter fitter;
    auto result = fitter.fit(data.data(), wt.data(), 100, rl, rs);

    REQUIRE(result.converged);
    REQUIRE(result.distribution.shape == DistributionShape::SPIKE_OUTLIER);
    REQUIRE(result.distribution.true_signal_estimate == Catch::Approx(0.5f).margin(0.03f));
}

// ============================================================
// Test 3: AICc is finite and n_params = 5 (mu1, sigma1, mu2, sigma2, pi).
// ============================================================
TEST_CASE("GaussianMixtureFitter: AICc is finite and correct param count", "[gmm]") {
    auto data = generate_bimodal(0.3f, 0.02f, 0.7f, 0.02f, 0.5f, 64, 33333);
    auto wt = uniform_weights(64);
    float rl = biweight_location(data.data(), 64);
    float rs = mad(data.data(), 64) * 1.4826f;

    GaussianMixtureFitter fitter;
    auto result = fitter.fit(data.data(), wt.data(), 64, rl, rs);

    REQUIRE(result.converged);
    REQUIRE(result.n_params == 5);
    REQUIRE(std::isfinite(result.aicc()));
}

// ============================================================
// Test 4: Too few samples (n < 10) → not converged.
// ============================================================
TEST_CASE("GaussianMixtureFitter: too few samples → fails gracefully", "[gmm]") {
    float data[] = {0.5f, 0.6f};
    float wt[]   = {1.0f, 1.0f};
    GaussianMixtureFitter fitter;
    auto result = fitter.fit(data, wt, 2, 0.55, 0.05);
    REQUIRE(!result.converged);
}
