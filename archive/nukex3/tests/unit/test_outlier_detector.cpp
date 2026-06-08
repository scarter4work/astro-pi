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

// --- sigmaClipMAD tests ---

TEST_CASE("sigmaClipMAD detects bright transient in background", "[outlier][mad]") {
    // Simulate 30 sky background frames with 1 airplane trail
    std::vector<double> data(30, 0.05);
    data[15] = 0.30;  // airplane light in frame 15
    auto outliers = nukex::sigmaClipMAD(data, 3.0);
    REQUIRE(outliers.size() == 1);
    REQUIRE(outliers[0] == 15);
}

TEST_CASE("sigmaClipMAD catches multiple transients", "[outlier][mad]") {
    std::vector<double> data(30, 0.05);
    data[5] = 0.25;   // satellite trail
    data[20] = 0.35;  // airplane
    auto outliers = nukex::sigmaClipMAD(data, 3.0);
    REQUIRE(outliers.size() == 2);
    bool has5 = std::find(outliers.begin(), outliers.end(), 5) != outliers.end();
    bool has20 = std::find(outliers.begin(), outliers.end(), 20) != outliers.end();
    REQUIRE((has5 && has20));
}

TEST_CASE("sigmaClipMAD catches transient in noisy background", "[outlier][mad]") {
    // Realistic: noisy background with one bright transient
    std::mt19937 rng(42);
    std::normal_distribution<double> dist(0.05, 0.002);
    std::vector<double> data(30);
    for (auto& d : data) d = dist(rng);
    data[10] = 0.30;  // airplane light — ~125σ above background
    auto outliers = nukex::sigmaClipMAD(data, 3.0);
    bool found10 = std::find(outliers.begin(), outliers.end(), 10) != outliers.end();
    REQUIRE(found10);
}

TEST_CASE("sigmaClipMAD no false positives on clean Gaussian", "[outlier][mad]") {
    std::mt19937 rng(42);
    std::normal_distribution<double> dist(0.05, 0.002);
    std::vector<double> data(30);
    for (auto& d : data) d = dist(rng);
    auto outliers = nukex::sigmaClipMAD(data, 3.0);
    REQUIRE(outliers.size() <= 1);  // 3σ should have <1% false positive rate
}

TEST_CASE("sigmaClipMAD handles constant data", "[outlier][mad]") {
    std::vector<double> data(20, 5.0);
    auto outliers = nukex::sigmaClipMAD(data, 3.0);
    REQUIRE(outliers.empty());
}

TEST_CASE("sigmaClipMAD handles small samples", "[outlier][mad]") {
    std::vector<double> data = {1.0, 100.0};
    auto outliers = nukex::sigmaClipMAD(data, 3.0);
    REQUIRE(outliers.empty());  // n < 3, returns empty
}
