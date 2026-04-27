#include "catch_amalgamated.hpp"
#include "nukex/core/histogram.hpp"
#include <numeric>

using nukex::PixelHistogram;

TEST_CASE("PixelHistogram: default state is empty", "[histogram]") {
    PixelHistogram h;
    for (int i = 0; i < PixelHistogram::N_BINS; i++) {
        REQUIRE(h.bins[i] == 0);
    }
    REQUIRE(h.total_count() == 0);
}

TEST_CASE("PixelHistogram: initialize_range sets bounds", "[histogram]") {
    PixelHistogram h;
    h.initialize_range(100.0f, 200.0f);
    REQUIRE(h.range_min == Catch::Approx(95.0f));
    REQUIRE(h.range_max == Catch::Approx(205.0f));
}

TEST_CASE("PixelHistogram: single value lands in correct bin", "[histogram]") {
    PixelHistogram h;
    h.initialize_range(0.0f, 1.0f);
    // range_min = -0.05, range_max = 1.05, span = 1.1
    // bin_width = 1.1 / 16 = 0.06875
    // Value 0.5: bin = (0.5 - (-0.05)) / 1.1 * 16 = 0.55/1.1 * 16 = 8
    h.update(0.5f);
    REQUIRE(h.bins[8] == 1);
    REQUIRE(h.total_count() == 1);
}

TEST_CASE("PixelHistogram: values at range edges are clamped", "[histogram]") {
    PixelHistogram h;
    h.initialize_range(0.0f, 1.0f);
    h.update(-10.0f);
    REQUIRE(h.bins[0] == 1);
    h.update(100.0f);
    REQUIRE(h.bins[PixelHistogram::N_BINS - 1] == 1);
    REQUIRE(h.total_count() == 2);
}

TEST_CASE("PixelHistogram: uniform distribution fills bins evenly", "[histogram]") {
    PixelHistogram h;
    h.initialize_range(0.0f, 1.0f);
    for (int i = 0; i < 1600; i++) {
        float v = static_cast<float>(i) / 1600.0f;
        h.update(v);
    }
    REQUIRE(h.total_count() == 1600);
    uint32_t total = 0;
    for (int i = 0; i < PixelHistogram::N_BINS; i++) {
        total += h.bins[i];
    }
    REQUIRE(total == 1600);
}

TEST_CASE("PixelHistogram: Gaussian-like distribution has central peak", "[histogram]") {
    PixelHistogram h;
    h.initialize_range(0.0f, 1.0f);
    for (int i = 0; i < 1000; i++) {
        float v = 0.5f + 0.1f * static_cast<float>(i % 21 - 10) / 10.0f;
        h.update(v);
    }
    int center_bin = static_cast<int>(
        (0.5f - h.range_min) / (h.range_max - h.range_min) * PixelHistogram::N_BINS);
    center_bin = std::clamp(center_bin, 0, PixelHistogram::N_BINS - 1);
    REQUIRE(h.bins[center_bin] > h.bins[0]);
    REQUIRE(h.bins[center_bin] > h.bins[PixelHistogram::N_BINS - 1]);
}

TEST_CASE("PixelHistogram: peak_bin returns correct bin", "[histogram]") {
    PixelHistogram h;
    h.initialize_range(0.0f, 1.0f);
    for (int i = 0; i < 100; i++) {
        h.update(0.8f);
    }
    for (int i = 0; i < 10; i++) {
        h.update(0.2f);
    }
    int peak = h.peak_bin();
    float peak_value = h.bin_center(peak);
    REQUIRE(peak_value == Catch::Approx(0.8f).margin(0.1f));
}

TEST_CASE("PixelHistogram: reset clears all bins", "[histogram]") {
    PixelHistogram h;
    h.initialize_range(0.0f, 1.0f);
    h.update(0.5f);
    h.update(0.6f);
    h.reset();
    for (int i = 0; i < PixelHistogram::N_BINS; i++) {
        REQUIRE(h.bins[i] == 0);
    }
    REQUIRE(h.total_count() == 0);
}
