#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "engine/NumericalUtils.h"
#include <cmath>
#include <limits>

TEST_CASE("log-sum-exp basic", "[numerical]") {
    std::vector<double> vals = {1.0, 2.0, 3.0};
    double result = nukex::logSumExp(vals);
    double expected = std::log(std::exp(1.0) + std::exp(2.0) + std::exp(3.0));
    REQUIRE(result == Catch::Approx(expected).epsilon(1e-12));
}

TEST_CASE("log-sum-exp large values don't overflow", "[numerical]") {
    std::vector<double> vals = {1000.0, 1001.0, 1002.0};
    double result = nukex::logSumExp(vals);
    REQUIRE(std::isfinite(result));
    REQUIRE(result > 1001.0);
}

TEST_CASE("log-sum-exp large negative values don't underflow", "[numerical]") {
    std::vector<double> vals = {-1000.0, -1001.0, -1002.0};
    double result = nukex::logSumExp(vals);
    REQUIRE(std::isfinite(result));
    REQUIRE(result < -999.0);
}

TEST_CASE("log-sum-exp single element", "[numerical]") {
    std::vector<double> vals = {5.0};
    REQUIRE(nukex::logSumExp(vals) == Catch::Approx(5.0));
}

TEST_CASE("log-sum-exp empty vector", "[numerical]") {
    std::vector<double> vals = {};
    double result = nukex::logSumExp(vals);
    REQUIRE(result == -std::numeric_limits<double>::infinity());
}

TEST_CASE("AIC/BIC scoring", "[numerical]") {
    double logL = -100.0;
    int k = 2, n = 100;
    REQUIRE(nukex::aic(logL, k) == Catch::Approx(204.0));
    REQUIRE(nukex::bic(logL, k, n) == Catch::Approx(200.0 + 2*std::log(100.0)));
    REQUIRE(nukex::aicc(logL, k, n) > nukex::aic(logL, k));
}

TEST_CASE("Kahan sum accuracy", "[numerical]") {
    // Sum many small values where naive sum loses precision
    std::vector<double> data(10000, 0.0001);
    double result = nukex::kahanSum(data.data(), data.size());
    REQUIRE(result == Catch::Approx(1.0).epsilon(1e-10));
}

TEST_CASE("variance and stddev", "[numerical]") {
    std::vector<double> data = {2, 4, 4, 4, 5, 5, 7, 9};
    double v = nukex::variance(data.data(), data.size());
    REQUIRE(v == Catch::Approx(4.0).margin(0.1));
    REQUIRE(nukex::stddev(data.data(), data.size()) == Catch::Approx(2.0).margin(0.05));
}

TEST_CASE("skewness of symmetric data is near zero", "[numerical]") {
    std::vector<double> data = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    REQUIRE(std::abs(nukex::skewness(data.data(), data.size())) < 0.1);
}

TEST_CASE("promoteToDouble", "[numerical]") {
    std::vector<float> fdata = {1.0f, 2.5f, 3.7f};
    auto ddata = nukex::promoteToDouble(fdata.data(), fdata.size());
    REQUIRE(ddata.size() == 3);
    REQUIRE(ddata[0] == Catch::Approx(1.0));
    REQUIRE(ddata[1] == Catch::Approx(2.5));
}
