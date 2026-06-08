#include "catch_amalgamated.hpp"
#include "nukex/core/reservoir.hpp"
#include <cmath>
#include <vector>
#include <array>
#include <algorithm>
#include <numeric>

using nukex::ReservoirSample;

TEST_CASE("ReservoirSample: default state is empty", "[reservoir]") {
    ReservoirSample rs;
    REQUIRE(rs.count == 0);
}

TEST_CASE("ReservoirSample: first K items always stored", "[reservoir]") {
    ReservoirSample rs;
    rs.seed(42);
    for (int i = 0; i < ReservoirSample::K; i++) {
        ReservoirSample::Sample s{};
        s.value = static_cast<float>(i);
        s.frame_index = static_cast<uint16_t>(i);
        rs.update(s);
    }
    REQUIRE(rs.count == ReservoirSample::K);
    for (int i = 0; i < ReservoirSample::K; i++) {
        REQUIRE(rs.samples[i].value == Catch::Approx(static_cast<float>(i)));
        REQUIRE(rs.samples[i].frame_index == static_cast<uint16_t>(i));
    }
}

TEST_CASE("ReservoirSample: count tracks total insertions", "[reservoir]") {
    ReservoirSample rs;
    rs.seed(42);
    for (int i = 0; i < 500; i++) {
        ReservoirSample::Sample s{};
        s.value = static_cast<float>(i);
        rs.update(s);
    }
    REQUIRE(rs.count == 500);
}

TEST_CASE("ReservoirSample: stored count never exceeds K", "[reservoir]") {
    ReservoirSample rs;
    rs.seed(42);
    for (int i = 0; i < 10000; i++) {
        ReservoirSample::Sample s{};
        s.value = static_cast<float>(i);
        rs.update(s);
    }
    REQUIRE(rs.count == 10000);
    REQUIRE(rs.stored_count() == ReservoirSample::K);
}

TEST_CASE("ReservoirSample: statistical fairness", "[reservoir]") {
    constexpr int N = 256;
    constexpr int n_trials = 1000;
    std::array<int, N> hit_count = {};

    for (int trial = 0; trial < n_trials; trial++) {
        ReservoirSample rs;
        rs.seed(static_cast<uint64_t>(trial * 7919 + 1));

        for (int i = 0; i < N; i++) {
            ReservoirSample::Sample s{};
            s.value = static_cast<float>(i);
            s.frame_index = static_cast<uint16_t>(i);
            rs.update(s);
        }

        for (int j = 0; j < rs.stored_count(); j++) {
            int idx = static_cast<int>(rs.samples[j].frame_index);
            hit_count[idx]++;
        }
    }

    float expected = static_cast<float>(n_trials) *
                     static_cast<float>(ReservoirSample::K) / static_cast<float>(N);
    float sigma = std::sqrt(expected * (1.0f - static_cast<float>(ReservoirSample::K)
                                                / static_cast<float>(N)));
    float tolerance = 4.0f * sigma;

    for (int i = 0; i < N; i++) {
        float deviation = std::abs(static_cast<float>(hit_count[i]) - expected);
        INFO("Index " << i << ": hits=" << hit_count[i]
             << " expected=" << expected << " deviation=" << deviation
             << " tolerance=" << tolerance);
        REQUIRE(deviation < tolerance);
    }
}

TEST_CASE("ReservoirSample: Sample carries full metadata", "[reservoir]") {
    ReservoirSample rs;
    rs.seed(42);

    ReservoirSample::Sample s{};
    s.value = 0.5f;
    s.frame_weight = 0.9f;
    s.psf_weight = 0.85f;
    s.read_noise = 3.2f;
    s.gain = 1.5f;
    s.exposure = 300.0f;
    s.lum_ratio = 0.95f;
    s.sigma_score = 0.3f;
    s.frame_index = 42;
    s.is_meridian_flipped = true;
    rs.update(s);

    REQUIRE(rs.count == 1);
    REQUIRE(rs.samples[0].value == Catch::Approx(0.5f));
    REQUIRE(rs.samples[0].frame_weight == Catch::Approx(0.9f));
    REQUIRE(rs.samples[0].psf_weight == Catch::Approx(0.85f));
    REQUIRE(rs.samples[0].read_noise == Catch::Approx(3.2f));
    REQUIRE(rs.samples[0].gain == Catch::Approx(1.5f));
    REQUIRE(rs.samples[0].exposure == Catch::Approx(300.0f));
    REQUIRE(rs.samples[0].lum_ratio == Catch::Approx(0.95f));
    REQUIRE(rs.samples[0].sigma_score == Catch::Approx(0.3f));
    REQUIRE(rs.samples[0].frame_index == 42);
    REQUIRE(rs.samples[0].is_meridian_flipped == true);
}

TEST_CASE("ReservoirSample: reset clears state", "[reservoir]") {
    ReservoirSample rs;
    rs.seed(42);
    for (int i = 0; i < 100; i++) {
        ReservoirSample::Sample s{};
        s.value = static_cast<float>(i);
        rs.update(s);
    }
    rs.reset();
    REQUIRE(rs.count == 0);
    REQUIRE(rs.stored_count() == 0);
}
