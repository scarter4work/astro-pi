#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <random>
#include <algorithm>
#include <cmath>
#include "engine/GaussianMixEM.h"

TEST_CASE("EM separates well-separated bimodal data", "[em]") {
    std::mt19937 rng(42);
    std::normal_distribution<double> d1(2.0, 0.5), d2(8.0, 0.5);
    std::vector<double> data;
    for (int i = 0; i < 200; i++) {
        data.push_back(d1(rng));
        data.push_back(d2(rng));
    }
    std::shuffle(data.begin(), data.end(), rng);

    auto result = nukex::fitGaussianMixture2(data);

    double lo = std::min(result.mu1, result.mu2);
    double hi = std::max(result.mu1, result.mu2);
    REQUIRE(lo == Catch::Approx(2.0).margin(0.5));
    REQUIRE(hi == Catch::Approx(8.0).margin(0.5));
    REQUIRE(result.converged);
}

TEST_CASE("EM finds correct mixing weights", "[em]") {
    std::mt19937 rng(42);
    std::normal_distribution<double> d1(0.0, 1.0), d2(10.0, 1.0);
    std::vector<double> data;
    // 70/30 split
    for (int i = 0; i < 700; i++) data.push_back(d1(rng));
    for (int i = 0; i < 300; i++) data.push_back(d2(rng));
    std::shuffle(data.begin(), data.end(), rng);

    auto result = nukex::fitGaussianMixture2(data);

    // One weight should be ~0.7, the other ~0.3
    double w1 = result.weight;
    double w2 = 1.0 - result.weight;
    double maxW = std::max(w1, w2);
    double minW = std::min(w1, w2);
    REQUIRE(maxW == Catch::Approx(0.7).margin(0.1));
    REQUIRE(minW == Catch::Approx(0.3).margin(0.1));
}

TEST_CASE("EM on unimodal data returns near-degenerate mixture", "[em]") {
    std::mt19937 rng(42);
    std::normal_distribution<double> d(5.0, 1.0);
    std::vector<double> data(300);
    for (auto& v : data) v = d(rng);

    auto result = nukex::fitGaussianMixture2(data, 500, 1e-6);
    // Both means should be close together
    REQUIRE(std::abs(result.mu1 - result.mu2) < 3.0);
    REQUIRE(result.converged);
}

TEST_CASE("EM convergence within max iterations", "[em]") {
    std::mt19937 rng(42);
    std::normal_distribution<double> d1(0.0, 1.0), d2(5.0, 1.0);
    std::vector<double> data;
    for (int i = 0; i < 100; i++) {
        data.push_back(d1(rng));
        data.push_back(d2(rng));
    }
    std::shuffle(data.begin(), data.end(), rng);

    auto result = nukex::fitGaussianMixture2(data, 200, 1e-6);
    REQUIRE(result.converged);
    REQUIRE(result.iterations < 200);
}

TEST_CASE("EM log-likelihood is finite", "[em]") {
    std::mt19937 rng(42);
    std::normal_distribution<double> d1(3.0, 1.0), d2(7.0, 1.0);
    std::vector<double> data;
    for (int i = 0; i < 100; i++) {
        data.push_back(d1(rng));
        data.push_back(d2(rng));
    }

    auto result = nukex::fitGaussianMixture2(data);
    REQUIRE(std::isfinite(result.logLikelihood));
    REQUIRE(result.logLikelihood < 0);  // log-likelihood is negative
}

TEST_CASE("EM handles small samples", "[em]") {
    std::vector<double> data = {1.0, 2.0, 8.0, 9.0, 1.5, 8.5};
    auto result = nukex::fitGaussianMixture2(data);
    REQUIRE(result.sigma1 > 0);
    REQUIRE(result.sigma2 > 0);
    REQUIRE(std::isfinite(result.logLikelihood));
}

TEST_CASE("EM handles identical values gracefully", "[em]") {
    std::vector<double> data(50, 5.0);
    auto result = nukex::fitGaussianMixture2(data);
    // Should not crash -- floored sigma prevents singularity
    REQUIRE(std::isfinite(result.logLikelihood));
}
