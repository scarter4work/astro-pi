#include "catch_amalgamated.hpp"
#include "nukex/fitting/kde_fitter.hpp"
#include "nukex/fitting/robust_stats.hpp"
#include <vector>
#include <random>
#include <cmath>

using namespace nukex;

TEST_CASE("KDEFitter: mode of unimodal Gaussian data", "[kde]") {
    std::mt19937 rng(77777);
    std::normal_distribution<float> d(0.5f, 0.03f);
    std::vector<float> data(200);
    for (auto& v : data) v = d(rng);
    std::vector<float> wt(200, 1.0f);
    float rl = biweight_location(data.data(), 200);
    float rs = mad(data.data(), 200) * 1.4826f;

    KDEFitter fitter;
    auto result = fitter.fit(data.data(), wt.data(), 200, rl, rs);

    REQUIRE(result.converged);
    REQUIRE(result.distribution.kde_mode == Catch::Approx(0.5f).margin(0.03f));
    REQUIRE(result.distribution.used_nonparametric);
    REQUIRE(result.distribution.kde_bandwidth > 0.0f);
}

TEST_CASE("KDEFitter: mode of bimodal data finds taller peak", "[kde]") {
    std::vector<float> data;
    std::mt19937 rng(88888);
    std::normal_distribution<float> d1(0.3f, 0.02f);
    std::normal_distribution<float> d2(0.7f, 0.02f);
    for (int i = 0; i < 140; i++) data.push_back(d1(rng));
    for (int i = 0; i < 60; i++) data.push_back(d2(rng));
    std::vector<float> wt(200, 1.0f);
    float rl = biweight_location(data.data(), 200);
    float rs = mad(data.data(), 200) * 1.4826f;

    KDEFitter fitter;
    auto result = fitter.fit(data.data(), wt.data(), 200, rl, rs);

    REQUIRE(result.converged);
    REQUIRE(result.distribution.kde_mode == Catch::Approx(0.3f).margin(0.05f));
}

TEST_CASE("KDEFitter: ISJ bandwidth is reasonable", "[kde]") {
    std::mt19937 rng(99999);
    std::normal_distribution<float> d(0.5f, 0.05f);
    std::vector<float> data(100);
    for (auto& v : data) v = d(rng);

    double h = KDEFitter::isj_bandwidth(data.data(), 100);
    REQUIRE(h > 0.005);
    REQUIRE(h < 0.1);
}

TEST_CASE("KDEFitter: confidence is 0.5", "[kde]") {
    std::mt19937 rng(11111);
    std::normal_distribution<float> d(0.5f, 0.05f);
    std::vector<float> data(64);
    for (auto& v : data) v = d(rng);
    std::vector<float> wt(64, 1.0f);
    float rl = biweight_location(data.data(), 64);
    float rs = mad(data.data(), 64) * 1.4826f;

    KDEFitter fitter;
    auto result = fitter.fit(data.data(), wt.data(), 64, rl, rs);

    REQUIRE(result.converged);
    REQUIRE(result.distribution.confidence == Catch::Approx(0.5f));
}
