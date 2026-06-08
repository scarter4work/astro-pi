#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <random>
#include "engine/DistributionFitter.h"
#include "engine/NumericalUtils.h"

TEST_CASE("Gaussian MLE", "[distributions]") {
    std::vector<double> data = {4.0, 4.5, 5.0, 5.5, 6.0, 4.8, 5.2, 5.1, 4.9, 5.0};

    auto result = nukex::fitGaussian(data);
    REQUIRE(result.type == nukex::DistributionType::Gaussian);
    REQUIRE(result.logLikelihood < 0);
    REQUIRE(result.gaussian.mu == Catch::Approx(5.0).margin(0.1));
    REQUIRE(result.gaussian.sigma > 0);
    REQUIRE(result.k == 2);
}

TEST_CASE("Gaussian MLE with large sample", "[distributions]") {
    std::mt19937 rng(42);
    std::normal_distribution<double> dist(10.0, 2.0);
    std::vector<double> data(1000);
    for (auto& d : data) d = dist(rng);

    auto result = nukex::fitGaussian(data);
    REQUIRE(result.gaussian.mu == Catch::Approx(10.0).margin(0.2));
    REQUIRE(result.gaussian.sigma == Catch::Approx(2.0).margin(0.2));
}

TEST_CASE("Poisson MLE", "[distributions]") {
    std::vector<double> data = {2, 3, 4, 3, 2, 3, 4, 3, 2, 5};

    auto result = nukex::fitPoisson(data);
    REQUIRE(result.type == nukex::DistributionType::Poisson);
    REQUIRE(result.poisson.lambda == Catch::Approx(3.1).margin(0.2));
    REQUIRE(result.k == 1);
}

TEST_CASE("Poisson MLE with large sample", "[distributions]") {
    std::mt19937 rng(42);
    std::poisson_distribution<int> dist(7);
    std::vector<double> data(1000);
    for (auto& d : data) d = static_cast<double>(dist(rng));

    auto result = nukex::fitPoisson(data);
    REQUIRE(result.poisson.lambda == Catch::Approx(7.0).margin(0.3));
}

TEST_CASE("AIC selects correct model for Gaussian data", "[distributions]") {
    std::mt19937 rng(42);
    std::normal_distribution<double> dist(5.0, 1.0);
    std::vector<double> data(200);
    for (auto& d : data) d = dist(rng);

    auto gauss = nukex::fitGaussian(data);
    auto poisson = nukex::fitPoisson(data);

    double aicGauss = nukex::aic(gauss.logLikelihood, gauss.k);
    double aicPoisson = nukex::aic(poisson.logLikelihood, poisson.k);
    REQUIRE(aicGauss < aicPoisson);  // Gaussian should win
}

TEST_CASE("AIC selects correct model for Poisson data", "[distributions]") {
    std::mt19937 rng(42);
    std::poisson_distribution<int> dist(5);
    std::vector<double> data(200);
    for (auto& d : data) d = static_cast<double>(dist(rng));

    auto gauss = nukex::fitGaussian(data);
    auto poisson = nukex::fitPoisson(data);

    double aicGauss = nukex::aic(gauss.logLikelihood, gauss.k);
    double aicPoisson = nukex::aic(poisson.logLikelihood, poisson.k);
    REQUIRE(aicPoisson < aicGauss);  // Poisson should win
}

TEST_CASE("Gaussian MLE handles constant data", "[distributions]") {
    std::vector<double> data(100, 5.0);
    auto result = nukex::fitGaussian(data);
    REQUIRE(result.gaussian.mu == Catch::Approx(5.0));
    REQUIRE(result.gaussian.sigma > 0);  // floored, not zero
}
