#include "catch_amalgamated.hpp"
#include "nukex/fitting/huber_estimator.hpp"
#include "nukex/core/voxel.hpp"

#include <cmath>
#include <numeric>
#include <random>
#include <vector>

using namespace nukex;

TEST_CASE("HuberEstimator: on clean Gaussian samples the location is the mean, "
          "to within its own standard error", "[fitting][huber]") {
    std::mt19937 rng(5);
    std::normal_distribution<float> g(0.30f, 0.01f);
    const int n = 200;
    std::vector<float> x(n), w(n, 1.0f), scratch(n);
    for (auto& v : x) v = g(rng);
    const float mean = std::accumulate(x.begin(), x.end(), 0.0f) / n;
    HuberEstimator::Config c;
    const float mu = HuberEstimator::location(x.data(), w.data(), n, c, scratch.data());
    REQUIRE(std::fabs(mu - mean) < 0.01f / std::sqrt(static_cast<float>(n)));
}

TEST_CASE("HuberEstimator: one gross outlier in twenty moves the location by "
          "less than a tenth of a sigma, where the mean moves by four", "[fitting][huber]") {
    const int n = 20;
    std::vector<float> x(n, 0.0f), w(n, 1.0f), scratch(n);
    std::mt19937 rng(9);
    std::normal_distribution<float> g(0.50f, 0.01f);
    for (auto& v : x) v = g(rng);
    // The clean mean is the reference, not the population value: the sample
    // itself scatters by sigma / sqrt(n).
    float clean_mean = 0.0f;
    for (int i = 0; i < n; i++) if (i != 7) clean_mean += x[i];
    clean_mean /= (n - 1);
    x[7] = 1.3f;                                // a satellite, 80 sigma out
    const float mean = std::accumulate(x.begin(), x.end(), 0.0f) / n;
    HuberEstimator::Config c;
    const float mu = HuberEstimator::location(x.data(), w.data(), n, c, scratch.data());
    REQUIRE(std::fabs(mu - clean_mean) < 0.001f);      // under 0.1 sigma
    REQUIRE(std::fabs(mean - clean_mean) > 0.03f);     // the mean is dragged 4 sigma
}

TEST_CASE("HuberEstimator: zero-weight samples do not contribute", "[fitting][huber]") {
    // Ten samples at 0.4 with weight 1, ten at 0.6 with weight 0. Seeded at
    // the (unweighted) median between them, the weighted iteration must
    // settle on 0.4.
    const int n = 20;
    std::vector<float> x(n), w(n), scratch(n);
    for (int i = 0; i < n; i++) { x[i] = (i < 10) ? 0.4f + 0.001f * (i % 3) : 0.6f + 0.001f * (i % 3); w[i] = (i < 10) ? 1.0f : 0.0f; }
    HuberEstimator::Config c;
    c.iterations = 50;
    const float mu = HuberEstimator::location(x.data(), w.data(), n, c, scratch.data());
    REQUIRE(mu == Catch::Approx(0.401f).margin(0.002f));
}

TEST_CASE("HuberEstimator: fewer than min_samples returns the median and flags the voxel",
          "[fitting][huber]") {
    std::vector<float> x{0.2f, 0.9f}, w{1.0f, 1.0f}, scratch(2);
    HuberEstimator::Config c;
    const float mu = HuberEstimator::location(x.data(), w.data(), 2, c, scratch.data());
    REQUIRE(mu == Catch::Approx(0.55f));
}

TEST_CASE("HuberEstimator: estimate() writes the voxel record the race writes",
          "[fitting][huber]") {
    StandaloneVoxel<1> sv;
    SubcubeVoxel& v = *sv;
    const int n = 30;
    std::vector<float> x(n), w(n, 1.0f);
    std::mt19937 rng(3);
    std::normal_distribution<float> g(0.25f, 0.02f);
    for (auto& s : x) s = g(rng);
    x[3] = 0.9f;
    HuberEstimator h;
    h.estimate(x.data(), w.data(), n, v, 0);
    const auto& d = v.channel(0).distribution;
    REQUIRE(d.shape == DistributionShape::GAUSSIAN);
    REQUIRE(d.true_signal_estimate == Catch::Approx(0.25f).margin(0.01f));
    REQUIRE(d.signal_uncertainty > 0.0f);
    REQUIRE(d.signal_uncertainty < 0.02f);          // sigma / sqrt(n_eff)
    // Inside delta = 1.345 sigma a Gaussian keeps ~82% of its samples, and
    // the MAD-based sigma scatters on 30 samples: the outlier is out, most
    // of the rest are in.
    REQUIRE(d.confidence > 0.6f);
    REQUIRE(d.confidence < 1.0f);
    REQUIRE(v.channel(0).mad > 0.0f);
    REQUIRE_FALSE(v.has_flag(VoxelFlags::FIT_FAILED));
}
