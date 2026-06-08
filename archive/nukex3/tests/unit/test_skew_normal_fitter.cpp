#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <random>
#include <cmath>
#include "engine/SkewNormalFitter.h"

TEST_CASE("Skew-Normal MLE detects positive skew", "[skewnormal]") {
    std::mt19937 rng(42);
    std::vector<double> data(300);
    // Simulate positively skewed data using Azzalini method
    for (auto& d : data) {
        std::normal_distribution<double> n1(0, 1), n2(0, 1);
        double u = n1(rng), v = n2(rng);
        double alpha = 5.0;
        d = (alpha * std::abs(u) + v) / std::sqrt(1.0 + alpha * alpha);
        d = d * 2.0 + 3.0;  // scale=2, location=3
    }

    auto result = nukex::fitSkewNormal(data);
    REQUIRE(result.k == 3);
    REQUIRE(result.alpha > 0);  // should detect positive skew
    REQUIRE(result.omega > 0);  // scale must be positive
    REQUIRE(result.logLikelihood < 0);
}

TEST_CASE("Skew-Normal MLE detects negative skew", "[skewnormal]") {
    std::mt19937 rng(123);
    std::vector<double> data(300);
    for (auto& d : data) {
        std::normal_distribution<double> n1(0, 1), n2(0, 1);
        double u = n1(rng), v = n2(rng);
        double alpha = -5.0;
        // For negative skew, flip the sign
        d = (std::abs(alpha) * std::abs(u) + v) / std::sqrt(1.0 + alpha * alpha);
        d = -d * 2.0 + 10.0;  // negate to get negative skew
    }

    auto result = nukex::fitSkewNormal(data);
    REQUIRE(result.alpha < 0);  // should detect negative skew
}

TEST_CASE("Skew-Normal reduces to near-Gaussian for symmetric data", "[skewnormal]") {
    std::mt19937 rng(42);
    std::normal_distribution<double> dist(5.0, 1.0);
    std::vector<double> data(500);
    for (auto& d : data) d = dist(rng);

    auto result = nukex::fitSkewNormal(data);
    REQUIRE(std::abs(result.alpha) < 3.0);  // alpha near 0 for symmetric
    REQUIRE(result.xi == Catch::Approx(5.0).margin(1.0));
    REQUIRE(result.omega == Catch::Approx(1.0).margin(0.5));
}

TEST_CASE("Skew-Normal convergence flag", "[skewnormal]") {
    std::mt19937 rng(42);
    std::normal_distribution<double> dist(5.0, 1.0);
    std::vector<double> data(100);
    for (auto& d : data) d = dist(rng);

    auto result = nukex::fitSkewNormal(data);
    REQUIRE(result.converged);
    REQUIRE(result.iterations > 0);
}

TEST_CASE("Skew-Normal handles small samples", "[skewnormal]") {
    std::vector<double> data = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0};
    auto result = nukex::fitSkewNormal(data);
    REQUIRE(result.omega > 0);
    REQUIRE(std::isfinite(result.logLikelihood));
}
