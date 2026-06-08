#include "catch_amalgamated.hpp"
#include "nukex/stretch/rnc_stretch.hpp"
#include "nukex/stretch/arcsinh_stretch.hpp"
#include "png_writer.hpp"
#include "test_data_loader.hpp"
#include <cmath>

using namespace nukex;

TEST_CASE("RNC: apply_scalar(0) == 0", "[rnc]") {
    RNCStretch s;
    REQUIRE(s.apply_scalar(0.0f) == Catch::Approx(0.0f));
}

TEST_CASE("RNC: apply_scalar(1) == 1", "[rnc]") {
    RNCStretch s;
    REQUIRE(s.apply_scalar(1.0f) == Catch::Approx(1.0f));
}

TEST_CASE("RNC: gamma=1 is identity", "[rnc]") {
    RNCStretch s;
    s.gamma = 1.0f;
    for (float x : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        REQUIRE(s.apply_scalar(x) == Catch::Approx(x).margin(1e-5f));
    }
}

TEST_CASE("RNC: gamma=2.2 standard", "[rnc]") {
    RNCStretch s;
    s.gamma = 2.2f;
    float expected = std::pow(0.5f, 1.0f / 2.2f);
    REQUIRE(s.apply_scalar(0.5f) == Catch::Approx(expected).margin(1e-5f));
}

TEST_CASE("RNC: monotonically increasing", "[rnc]") {
    RNCStretch s;
    float prev = -1.0f;
    for (int i = 0; i <= 100; i++) {
        float x = i / 100.0f;
        float y = s.apply_scalar(x);
        REQUIRE(y >= prev - 1e-6f);
        prev = y;
    }
}

TEST_CASE("RNC: black_point clips below", "[rnc]") {
    RNCStretch s;
    s.black_point = 0.1f;
    REQUIRE(s.apply_scalar(0.05f) == Catch::Approx(0.0f));
}

TEST_CASE("RNC: white_point clips above", "[rnc]") {
    RNCStretch s;
    s.white_point = 0.8f;
    REQUIRE(s.apply_scalar(0.9f) == Catch::Approx(1.0f));
}

TEST_CASE("RNC: apply_scalar(bp) == 0, apply_scalar(wp) == 1", "[rnc]") {
    RNCStretch s;
    s.black_point = 0.2f;
    s.white_point = 0.8f;
    REQUIRE(s.apply_scalar(0.2f) == Catch::Approx(0.0f));
    REQUIRE(s.apply_scalar(0.8f) == Catch::Approx(1.0f));
}

TEST_CASE("RNC: higher gamma → less aggressive (darker result)", "[rnc]") {
    RNCStretch lo, hi;
    lo.gamma = 1.5f;
    hi.gamma = 3.0f;
    // Higher gamma → 1/gamma smaller → pow(x, smaller) → closer to 1 for x<1
    // Actually: pow(0.5, 1/1.5) > pow(0.5, 1/3.0)
    // 1/1.5 = 0.667, 1/3 = 0.333 → 0.5^0.667 < 0.5^0.333
    REQUIRE(lo.apply_scalar(0.5f) < hi.apply_scalar(0.5f));
}

TEST_CASE("RNC: output in [0, 1]", "[rnc]") {
    RNCStretch s;
    for (float x = 0.0f; x <= 1.0f; x += 0.01f) {
        float y = s.apply_scalar(x);
        REQUIRE(y >= -1e-6f);
        REQUIRE(y <= 1.0f + 1e-6f);
    }
}

TEST_CASE("RNC: visual output on M16", "[rnc][visual]") {
    auto img = test_util::load_m16_test_frame();
    if (img.empty()) { SKIP("M16 test data not available"); }

    test_util::prepare_for_stretch(img);

    // RNC is a secondary stretch — apply arcsinh first to simulate a primary stretch
    ArcSinhStretch pre;
    pre.alpha = 500.0f;
    pre.luminance_only = true;
    pre.apply(img);

    RNCStretch s;
    s.gamma = 2.2f;
    s.black_point = 0.05f;
    s.white_point = 0.95f;
    s.enabled = true;
    s.apply(img);

    bool ok = test_util::write_png("test/output/stretch_rnc.png", img, false);
    REQUIRE(ok);
}
