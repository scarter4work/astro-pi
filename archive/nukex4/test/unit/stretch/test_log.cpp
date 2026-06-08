#include "catch_amalgamated.hpp"
#include "nukex/stretch/log_stretch.hpp"
#include "png_writer.hpp"
#include "test_data_loader.hpp"
#include <cmath>

using namespace nukex;

TEST_CASE("Log: apply_scalar(0) == 0", "[log]") {
    LogStretch s;
    REQUIRE(s.apply_scalar(0.0f) == Catch::Approx(0.0f));
}

TEST_CASE("Log: apply_scalar(1) == 1", "[log]") {
    LogStretch s;
    REQUIRE(s.apply_scalar(1.0f) == Catch::Approx(1.0f).margin(1e-5f));
}

TEST_CASE("Log: monotonically increasing", "[log]") {
    LogStretch s;
    float prev = -1.0f;
    for (int i = 0; i <= 100; i++) {
        float x = i / 100.0f;
        float y = s.apply_scalar(x);
        REQUIRE(y >= prev - 1e-6f);
        prev = y;
    }
}

TEST_CASE("Log: higher alpha → more aggressive", "[log]") {
    LogStretch lo, hi;
    lo.alpha = 100.0f;
    hi.alpha = 10000.0f;
    REQUIRE(hi.apply_scalar(0.01f) > lo.apply_scalar(0.01f));
}

TEST_CASE("Log: alpha near 0 → identity", "[log]") {
    LogStretch s;
    s.alpha = 0.001f;
    for (float x : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        REQUIRE(s.apply_scalar(x) == Catch::Approx(x).margin(0.01f));
    }
}

TEST_CASE("Log: known value cross-validation", "[log]") {
    LogStretch s;
    s.alpha = 1000.0f;
    float expected = std::log1p(1000.0f * 0.01f) / std::log1p(1000.0f);
    REQUIRE(s.apply_scalar(0.01f) == Catch::Approx(expected).margin(1e-5f));
}

TEST_CASE("Log: output in [0, 1]", "[log]") {
    LogStretch s;
    for (float x = 0.0f; x <= 1.0f; x += 0.01f) {
        float y = s.apply_scalar(x);
        REQUIRE(y >= -1e-6f);
        REQUIRE(y <= 1.0f + 1e-6f);
    }
}

TEST_CASE("Log: less aggressive than arcsinh at same alpha", "[log]") {
    // log1p grows slower than asinh, so log stretch is gentler
    LogStretch log_s;
    log_s.alpha = 500.0f;
    float log_val = log_s.apply_scalar(0.01f);
    float arcsinh_val = std::asinh(500.0f * 0.01f) / std::asinh(500.0f);
    REQUIRE(log_val < arcsinh_val);
}

TEST_CASE("Log: faint signal boost", "[log]") {
    LogStretch s;
    s.alpha = 1000.0f;
    REQUIRE(s.apply_scalar(0.001f) > 0.05f);
}

TEST_CASE("Log: concave (second derivative negative)", "[log]") {
    LogStretch s;
    s.alpha = 1000.0f;
    float y0 = s.apply_scalar(0.2f);
    float y1 = s.apply_scalar(0.5f);
    float y2 = s.apply_scalar(0.8f);
    // Concave: midpoint stretched value > linear interpolation
    float linear_mid = (y0 + y2) / 2.0f;
    REQUIRE(y1 > linear_mid);
}

TEST_CASE("Log: visual output on M16", "[log][visual]") {
    auto img = test_util::load_m16_test_frame();
    if (img.empty()) { SKIP("M16 test data not available"); }

    test_util::prepare_for_stretch(img);

    LogStretch s;
    s.alpha = 1000.0f;
    s.luminance_only = false;
    s.enabled = true;
    s.apply(img);

    bool ok = test_util::write_png("test/output/stretch_log.png", img, false);
    REQUIRE(ok);
}
