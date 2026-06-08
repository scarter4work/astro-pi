#include <catch2/catch_test_macros.hpp>
#include "engine/AutoStretchSelector.h"

using namespace nukex;

static std::vector<uint8_t> uniformDistMap(size_t size, DistributionType type) {
    return std::vector<uint8_t>(size, static_cast<uint8_t>(type));
}

TEST_CASE("AutoStretchSelector: RGB Gaussian -> MTF", "[autostretch]") {
    size_t sz = 1000;
    auto map = uniformDistMap(sz, DistributionType::Gaussian);
    std::vector<std::vector<uint8_t>> maps = { map, map, map };

    ChannelStats stats;
    stats.median = 0.1;
    stats.mad = 0.01;
    stats.mean = 0.12;
    std::vector<ChannelStats> perChannel = { stats, stats, stats };

    auto result = AutoStretchSelector::Select(maps, perChannel);
    // 100% Gaussian → MTF (PI STF equivalent), regardless of channel count
    REQUIRE(result.algorithm == StretchAlgorithm::MTF);
}

TEST_CASE("AutoStretchSelector: mono Gaussian -> MTF", "[autostretch]") {
    size_t sz = 1000;
    auto map = uniformDistMap(sz, DistributionType::Gaussian);
    std::vector<std::vector<uint8_t>> maps = { map };

    ChannelStats stats;
    stats.median = 0.1;
    stats.mad = 0.01;
    stats.mean = 0.12;
    std::vector<ChannelStats> perChannel = { stats };

    auto result = AutoStretchSelector::Select(maps, perChannel);
    // Mono: nCh=1, skips RNC rule, maxGaussian=1.0 > 0.70 → MTF
    REQUIRE(result.algorithm == StretchAlgorithm::MTF);
}

TEST_CASE("AutoStretchSelector: high Bimodal fraction -> ArcSinh", "[autostretch]") {
    size_t sz = 1000;
    std::vector<uint8_t> map(sz, static_cast<uint8_t>(DistributionType::Gaussian));
    for (size_t i = 0; i < 200; ++i)
        map[i] = static_cast<uint8_t>(DistributionType::Bimodal);

    std::vector<std::vector<uint8_t>> maps = { map, map, map };

    ChannelStats stats;
    stats.median = 0.1;
    stats.mad = 0.01;
    stats.mean = 0.12;
    std::vector<ChannelStats> perChannel = { stats, stats, stats };

    auto result = AutoStretchSelector::Select(maps, perChannel);
    REQUIRE(result.algorithm == StretchAlgorithm::ArcSinh);
}

TEST_CASE("AutoStretchSelector: high Skew-Normal fraction -> GHS", "[autostretch]") {
    size_t sz = 1000;
    std::vector<uint8_t> map(sz, static_cast<uint8_t>(DistributionType::Gaussian));
    for (size_t i = 0; i < 250; ++i)
        map[i] = static_cast<uint8_t>(DistributionType::SkewNormal);

    std::vector<std::vector<uint8_t>> maps = { map, map, map };

    ChannelStats stats;
    stats.median = 0.1;
    stats.mad = 0.01;
    stats.mean = 0.12;
    std::vector<ChannelStats> perChannel = { stats, stats, stats };

    auto result = AutoStretchSelector::Select(maps, perChannel);
    REQUIRE(result.algorithm == StretchAlgorithm::GHS);
}

TEST_CASE("AutoStretchSelector: high channel divergence -> Lumpton", "[autostretch]") {
    size_t sz = 1000;
    auto mapR = uniformDistMap(sz, DistributionType::Gaussian);
    auto mapG = uniformDistMap(sz, DistributionType::Poisson);
    auto mapB = uniformDistMap(sz, DistributionType::Gaussian);

    std::vector<std::vector<uint8_t>> maps = { mapR, mapG, mapB };

    ChannelStats stats;
    stats.median = 0.1;
    stats.mad = 0.01;
    stats.mean = 0.12;
    std::vector<ChannelStats> perChannel = { stats, stats, stats };

    auto result = AutoStretchSelector::Select(maps, perChannel);
    REQUIRE(result.algorithm == StretchAlgorithm::Lumpton);
}

TEST_CASE("AutoStretchSelector: high Poisson -> GHS", "[autostretch]") {
    size_t sz = 1000;
    std::vector<uint8_t> map(sz, static_cast<uint8_t>(DistributionType::Gaussian));
    for (size_t i = 0; i < 500; ++i)
        map[i] = static_cast<uint8_t>(DistributionType::Poisson);

    std::vector<std::vector<uint8_t>> maps = { map, map, map };

    ChannelStats stats;
    stats.median = 0.05;
    stats.mad = 0.005;
    stats.mean = 0.06;
    std::vector<ChannelStats> perChannel = { stats, stats, stats };

    auto result = AutoStretchSelector::Select(maps, perChannel);
    REQUIRE((result.algorithm == StretchAlgorithm::GHS ||
             result.algorithm == StretchAlgorithm::Veralux));
}
