#include "catch_amalgamated.hpp"
#include "nukex/fitting/robust_stats.hpp"
#include <vector>
#include <cmath>

using namespace nukex;

TEST_CASE("median_inplace: odd count", "[robust]") {
    float data[] = {3.0f, 1.0f, 2.0f, 5.0f, 4.0f};
    REQUIRE(median_inplace(data, 5) == Catch::Approx(3.0f));
}

TEST_CASE("median_inplace: even count", "[robust]") {
    float data[] = {4.0f, 1.0f, 3.0f, 2.0f};
    REQUIRE(median_inplace(data, 4) == Catch::Approx(2.5f));
}

TEST_CASE("median_inplace: single element", "[robust]") {
    float data[] = {42.0f};
    REQUIRE(median_inplace(data, 1) == Catch::Approx(42.0f));
}

TEST_CASE("mad: known answer — symmetric data", "[robust]") {
    float data[] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
    REQUIRE(mad(data, 5) == Catch::Approx(1.0f));
}

TEST_CASE("mad: all identical → zero", "[robust]") {
    float data[] = {0.5f, 0.5f, 0.5f, 0.5f};
    REQUIRE(mad(data, 4) == Catch::Approx(0.0f));
}

TEST_CASE("biweight_location: Gaussian data recovers mean", "[robust]") {
    std::vector<float> data;
    float base = 0.5f;
    for (int i = 0; i < 50; i++) {
        float offset = 0.01f * (i - 25) / 25.0f;
        data.push_back(base + offset);
    }
    float loc = biweight_location(data.data(), static_cast<int>(data.size()));
    REQUIRE(loc == Catch::Approx(0.5f).margin(0.005f));
}

TEST_CASE("biweight_location: robust to outliers", "[robust]") {
    std::vector<float> data(20, 0.5f);
    data.push_back(10.0f);
    float loc = biweight_location(data.data(), static_cast<int>(data.size()));
    REQUIRE(loc == Catch::Approx(0.5f).margin(0.01f));
}

TEST_CASE("biweight_midvariance: known Gaussian data", "[robust]") {
    std::vector<float> data;
    float sigma = 0.05f;
    for (int i = 0; i < 100; i++) {
        float u = (i - 50) / 50.0f;
        data.push_back(0.5f + sigma * u);
    }
    float bwmv = biweight_midvariance(data.data(), static_cast<int>(data.size()));
    REQUIRE(bwmv > 0.0f);
}

TEST_CASE("iqr: known answer", "[robust]") {
    float data[] = {1, 2, 3, 4, 5, 6, 7, 8};
    float result = iqr(data, 8);
    REQUIRE(result == Catch::Approx(4.0f));
}

TEST_CASE("weighted_median: equal weights is unweighted median", "[robust]") {
    float data[] = {5.0f, 1.0f, 3.0f};
    float weights[] = {1.0f, 1.0f, 1.0f};
    REQUIRE(weighted_median(data, weights, 3) == Catch::Approx(3.0f));
}

TEST_CASE("weighted_median: heavy weight pulls toward value", "[robust]") {
    float data[] = {1.0f, 2.0f, 3.0f};
    float weights[] = {0.1f, 0.1f, 10.0f};
    REQUIRE(weighted_median(data, weights, 3) == Catch::Approx(3.0f));
}
