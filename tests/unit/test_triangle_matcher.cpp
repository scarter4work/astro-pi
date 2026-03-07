#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <vector>
#include "engine/TriangleMatcher.h"

TEST_CASE("TriangleDescriptor normalizes side ratios correctly", "[triangle]") {
    nukex::StarPosition s1{0, 0, 1.0};
    nukex::StarPosition s2{10, 0, 1.0};
    nukex::StarPosition s3{5, 8.66, 1.0};

    auto desc = nukex::makeTriangleDescriptor(s1, s2, s3, 0, 1, 2);
    REQUIRE(desc.ratioBA == Catch::Approx(1.0).margin(0.05));
    REQUIRE(desc.ratioCA == Catch::Approx(1.0).margin(0.05));
}

TEST_CASE("TriangleDescriptor is invariant to translation", "[triangle]") {
    nukex::StarPosition s1a{0, 0, 1.0}, s2a{10, 0, 1.0}, s3a{5, 8, 1.0};
    nukex::StarPosition s1b{100, 200, 1.0}, s2b{110, 200, 1.0}, s3b{105, 208, 1.0};

    auto a = nukex::makeTriangleDescriptor(s1a, s2a, s3a, 0, 1, 2);
    auto b = nukex::makeTriangleDescriptor(s1b, s2b, s3b, 0, 1, 2);
    REQUIRE(a.ratioBA == Catch::Approx(b.ratioBA).margin(0.001));
    REQUIRE(a.ratioCA == Catch::Approx(b.ratioCA).margin(0.001));
}

TEST_CASE("buildDescriptors produces correct count", "[triangle]") {
    std::vector<nukex::StarPosition> stars = {
        {0, 0, 1.0}, {10, 0, 0.9}, {5, 8, 0.8}, {15, 5, 0.7}
    };
    auto descs = nukex::buildDescriptors(stars, 4);
    REQUIRE(descs.size() == 4); // C(4,3) = 4
}

TEST_CASE("matchFrames detects known integer translation", "[triangle]") {
    std::vector<nukex::StarPosition> ref = {
        {100, 100, 1.0}, {200, 150, 0.9}, {150, 250, 0.8},
        {300, 100, 0.7}, {250, 300, 0.6}, {50, 200, 0.5}
    };

    int trueDx = 7, trueDy = -3;
    std::vector<nukex::StarPosition> target;
    for (const auto& s : ref)
        target.push_back({s.x + trueDx, s.y + trueDy, s.flux});

    auto result = nukex::matchFrames(ref, target, 6);
    REQUIRE(result.valid);
    REQUIRE(result.dx == trueDx);
    REQUIRE(result.dy == trueDy);
    REQUIRE(result.numMatchedStars >= 4);
}

TEST_CASE("matchFrames detects zero offset (identical frames)", "[triangle]") {
    std::vector<nukex::StarPosition> stars = {
        {100, 100, 1.0}, {200, 150, 0.9}, {150, 250, 0.8},
        {300, 100, 0.7}, {250, 300, 0.6}
    };

    auto result = nukex::matchFrames(stars, stars, 5);
    REQUIRE(result.valid);
    REQUIRE(result.dx == 0);
    REQUIRE(result.dy == 0);
}

TEST_CASE("matchFrames handles large dither offset", "[triangle]") {
    std::vector<nukex::StarPosition> ref = {
        {100, 100, 1.0}, {200, 150, 0.9}, {150, 250, 0.8},
        {300, 100, 0.7}, {250, 300, 0.6}, {350, 200, 0.5}
    };

    int trueDx = -15, trueDy = 12;
    std::vector<nukex::StarPosition> target;
    for (const auto& s : ref)
        target.push_back({s.x + trueDx, s.y + trueDy, s.flux});

    auto result = nukex::matchFrames(ref, target, 6);
    REQUIRE(result.valid);
    REQUIRE(result.dx == trueDx);
    REQUIRE(result.dy == trueDy);
}

TEST_CASE("matchFrames returns invalid for too few stars", "[triangle]") {
    std::vector<nukex::StarPosition> ref = {{100, 100, 1.0}};
    std::vector<nukex::StarPosition> target = {{107, 97, 1.0}};

    auto result = nukex::matchFrames(ref, target, 1, 0.01, 3);
    REQUIRE(!result.valid);
}
