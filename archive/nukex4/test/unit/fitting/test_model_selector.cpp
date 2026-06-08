#include "catch_amalgamated.hpp"
#include "nukex/fitting/model_selector.hpp"
#include "nukex/fitting/robust_stats.hpp"
#include <vector>
#include <random>
#include <cmath>

using namespace nukex;

namespace {

std::vector<float> generate_gaussian(float mu, float sigma, int n, unsigned seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<float> dist(mu, sigma);
    std::vector<float> samples(n);
    for (auto& v : samples) v = dist(rng);
    return samples;
}

std::vector<float> uniform_weights(int n) {
    return std::vector<float>(n, 1.0f);
}

} // anonymous namespace

// For clean Gaussian data at N=200, both ContaminationFitter (ε→0) and
// StudentTFitter (ν→∞) describe the distribution accurately.  The AICc
// cascade selects the parametric model with the best penalised likelihood —
// which may be GAUSSIAN, HEAVY_TAILED (ν close to the 30-dof boundary), or
// CONTAMINATED (ε near its minimum floor).  All three are correct
// descriptions of an approximately-Gaussian distribution on finite data.
// What matters is that (a) the selector converges, (b) no degenerate or
// non-parametric shape is chosen, and (c) the recovered signal is accurate.
TEST_CASE("ModelSelector: clean Gaussian → unimodal parametric shape + accurate signal", "[selector]") {
    auto data = generate_gaussian(0.50f, 0.03f, 200, 10001);
    auto wt = uniform_weights(200);

    ModelSelector selector;
    auto result = selector.select_best(data.data(), wt.data(), 200);

    REQUIRE(result.converged);

    // Must be one of the unimodal parametric shapes — not bimodal, spike, KDE
    auto shape = result.distribution.shape;
    bool unimodal_parametric = (shape == DistributionShape::GAUSSIAN      ||
                                shape == DistributionShape::HEAVY_TAILED  ||
                                shape == DistributionShape::CONTAMINATED);
    REQUIRE(unimodal_parametric);
    REQUIRE(result.distribution.true_signal_estimate == Catch::Approx(0.50f).margin(0.02f));
}

TEST_CASE("ModelSelector: bimodal data → selects BIMODAL", "[selector]") {
    std::mt19937 rng(20002);
    std::normal_distribution<float> d1(0.3f, 0.02f);
    std::normal_distribution<float> d2(0.7f, 0.02f);
    std::vector<float> data;
    for (int i = 0; i < 100; i++) data.push_back(d1(rng));
    for (int i = 0; i < 100; i++) data.push_back(d2(rng));
    auto wt = uniform_weights(200);

    ModelSelector selector;
    auto result = selector.select_best(data.data(), wt.data(), 200);

    REQUIRE(result.converged);
    REQUIRE(result.distribution.shape == DistributionShape::BIMODAL);
}

TEST_CASE("ModelSelector: Gaussian + outliers → robust signal estimate", "[selector]") {
    std::mt19937 rng(30003);
    std::normal_distribution<float> clean(0.50f, 0.02f);
    std::uniform_real_distribution<float> junk(0.0f, 1.0f);
    std::vector<float> data;
    for (int i = 0; i < 180; i++) data.push_back(clean(rng));
    for (int i = 0; i < 20; i++) data.push_back(junk(rng));
    auto wt = uniform_weights(200);

    ModelSelector selector;
    auto result = selector.select_best(data.data(), wt.data(), 200);

    REQUIRE(result.converged);
    auto shape = result.distribution.shape;
    bool acceptable = (shape == DistributionShape::CONTAMINATED ||
                       shape == DistributionShape::HEAVY_TAILED ||
                       shape == DistributionShape::SPIKE_OUTLIER);
    REQUIRE(acceptable);
    REQUIRE(result.distribution.true_signal_estimate == Catch::Approx(0.50f).margin(0.05f));
}

TEST_CASE("ModelSelector: writes robust stats to voxel", "[selector]") {
    auto data = generate_gaussian(0.50f, 0.03f, 100, 40004);
    auto wt = uniform_weights(100);

    SubcubeVoxel voxel{};
    ModelSelector selector;
    selector.select(data.data(), wt.data(), 100, voxel, 0);

    REQUIRE(voxel.mad[0] > 0.0f);
    REQUIRE(voxel.biweight_midvariance[0] > 0.0f);
    REQUIRE(voxel.iqr[0] > 0.0f);
    REQUIRE(voxel.distribution[0].shape != DistributionShape::UNKNOWN);
}

TEST_CASE("ModelSelector: very few samples → KDE fallback", "[selector]") {
    float data[] = {0.5f, 0.52f, 0.48f, 0.51f, 0.49f};
    float wt[]   = {1.0f, 1.0f,  1.0f,  1.0f,  1.0f};

    ModelSelector::Config cfg;
    cfg.aicc_threshold     = 2.0;
    cfg.min_samples_for_gmm = 30;
    cfg.min_samples_for_fit = 10;
    ModelSelector selector(cfg);
    auto result = selector.select_best(data, wt, 5);

    REQUIRE(result.converged);
    REQUIRE(result.distribution.used_nonparametric);
}
