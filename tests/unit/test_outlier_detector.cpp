#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <random>
#include <algorithm>
#include "engine/OutlierDetector.h"

TEST_CASE("Grubbs test detects single outlier", "[outlier]") {
    std::vector<double> data = {1, 2, 2, 3, 3, 3, 4, 4, 5, 100};
    auto idx = nukex::grubbsTest(data);
    REQUIRE(idx == 9);  // 100 is the outlier
}

TEST_CASE("Grubbs test returns SIZE_MAX for clean data", "[outlier]") {
    std::vector<double> data = {4.5, 5.0, 5.5, 5.2, 4.8, 5.1};
    auto idx = nukex::grubbsTest(data);
    REQUIRE(idx == SIZE_MAX);
}

TEST_CASE("Generalized ESD detects multiple outliers", "[outlier]") {
    std::vector<double> data = {1, 2, 2, 3, 3, 3, 4, 4, 5, 100};
    auto outliers = nukex::detectOutliersESD(data, 3);
    REQUIRE(std::find(outliers.begin(), outliers.end(), 9) != outliers.end());
}

TEST_CASE("Generalized ESD detects multiple extreme values", "[outlier]") {
    std::vector<double> data = {3, 3, 3, 3, 3, 3, 3, 3, 100, -50};
    auto outliers = nukex::detectOutliersESD(data, 3);
    // Both 100 and -50 should be detected
    REQUIRE(outliers.size() >= 2);
    bool has8 = std::find(outliers.begin(), outliers.end(), 8) != outliers.end();
    bool has9 = std::find(outliers.begin(), outliers.end(), 9) != outliers.end();
    REQUIRE((has8 && has9));
}

TEST_CASE("Chauvenet criterion detects outlier", "[outlier]") {
    std::vector<double> data = {1, 2, 2, 3, 3, 3, 4, 4, 5, 100};
    auto outliers = nukex::detectOutliersChauvenet(data);
    REQUIRE(!outliers.empty());
    REQUIRE(std::find(outliers.begin(), outliers.end(), 9) != outliers.end());
}

TEST_CASE("No false positives on clean Gaussian data", "[outlier]") {
    std::mt19937 rng(42);
    std::normal_distribution<double> dist(5.0, 1.0);
    std::vector<double> data(100);
    for (auto& d : data) d = dist(rng);
    auto outliers = nukex::detectOutliersESD(data, 5);
    REQUIRE(outliers.size() <= 2);
}

TEST_CASE("Outlier detection handles small samples", "[outlier]") {
    SECTION("2 data points -- too few for Grubbs") {
        std::vector<double> data = {1.0, 100.0};
        auto idx = nukex::grubbsTest(data);
        REQUIRE(idx == SIZE_MAX);  // can't detect with n<3
    }
    SECTION("3 data points -- minimum for Grubbs") {
        std::vector<double> data = {1.0, 2.0, 1000.0};
        auto idx = nukex::grubbsTest(data);
        REQUIRE(idx == 2);
    }
}

TEST_CASE("Outlier detection handles constant data", "[outlier]") {
    std::vector<double> data(20, 5.0);
    auto esd = nukex::detectOutliersESD(data, 3);
    REQUIRE(esd.empty());  // no outliers when all identical
    auto chauv = nukex::detectOutliersChauvenet(data);
    REQUIRE(chauv.empty());
}
