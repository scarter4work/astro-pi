#include "catch_amalgamated.hpp"
#include "nukex/stretch/arcsinh_stretch.hpp"
#include "png_writer.hpp"
#include "test_data_loader.hpp"
#include <cmath>

using namespace nukex;

TEST_CASE("ArcSinh: apply_scalar(0) == 0", "[arcsinh]") {
    ArcSinhStretch s;
    REQUIRE(s.apply_scalar(0.0f) == Catch::Approx(0.0f));
}

TEST_CASE("ArcSinh: apply_scalar(1) == 1", "[arcsinh]") {
    ArcSinhStretch s;
    REQUIRE(s.apply_scalar(1.0f) == Catch::Approx(1.0f).margin(1e-5f));
}

TEST_CASE("ArcSinh: monotonically increasing", "[arcsinh]") {
    ArcSinhStretch s;
    s.alpha = 500.0f;
    float prev = -1.0f;
    for (int i = 0; i <= 100; i++) {
        float x = i / 100.0f;
        float y = s.apply_scalar(x);
        REQUIRE(y >= prev - 1e-6f);
        prev = y;
    }
}

TEST_CASE("ArcSinh: higher alpha → more aggressive stretch", "[arcsinh]") {
    ArcSinhStretch lo, hi;
    lo.alpha = 100.0f;
    hi.alpha = 1000.0f;
    // At x=0.01 (faint signal), higher alpha should produce brighter output
    REQUIRE(hi.apply_scalar(0.01f) > lo.apply_scalar(0.01f));
}

TEST_CASE("ArcSinh: alpha near 0 is identity", "[arcsinh]") {
    ArcSinhStretch s;
    s.alpha = 0.001f;
    for (float x : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        REQUIRE(s.apply_scalar(x) == Catch::Approx(x).margin(0.01f));
    }
}

TEST_CASE("ArcSinh: known value at alpha=500, x=0.01", "[arcsinh]") {
    ArcSinhStretch s;
    s.alpha = 500.0f;
    float expected = std::asinh(500.0f * 0.01f) / std::asinh(500.0f);
    REQUIRE(s.apply_scalar(0.01f) == Catch::Approx(expected).margin(1e-5f));
}

TEST_CASE("ArcSinh: preserves hue in luminance-only mode", "[arcsinh]") {
    ArcSinhStretch s;
    s.luminance_only = true;
    s.alpha = 500.0f;

    Image img(4, 4, 3);
    // Set a colored pixel: R=0.01, G=0.005, B=0.002
    img.at(2, 2, 0) = 0.01f;
    img.at(2, 2, 1) = 0.005f;
    img.at(2, 2, 2) = 0.002f;

    float ratio_rg_before = img.at(2, 2, 0) / img.at(2, 2, 1);
    float ratio_gb_before = img.at(2, 2, 1) / img.at(2, 2, 2);

    s.apply(img);

    float ratio_rg_after = img.at(2, 2, 0) / img.at(2, 2, 1);
    float ratio_gb_after = img.at(2, 2, 1) / img.at(2, 2, 2);

    // Ratios should be preserved (hue unchanged)
    REQUIRE(ratio_rg_after == Catch::Approx(ratio_rg_before).margin(0.01f));
    REQUIRE(ratio_gb_after == Catch::Approx(ratio_gb_before).margin(0.01f));
}

TEST_CASE("ArcSinh: output in [0, 1]", "[arcsinh]") {
    ArcSinhStretch s;
    s.alpha = 500.0f;
    for (float x = 0.0f; x <= 1.0f; x += 0.01f) {
        float y = s.apply_scalar(x);
        REQUIRE(y >= -1e-6f);
        REQUIRE(y <= 1.0f + 1e-6f);
    }
}

TEST_CASE("ArcSinh: faint signal boost — x=0.001 maps well above linear", "[arcsinh]") {
    ArcSinhStretch s;
    s.alpha = 500.0f;
    // arcsinh(500 * 0.001) / arcsinh(500) ≈ 0.0697 — 70x boost over linear 0.001
    REQUIRE(s.apply_scalar(0.001f) > 0.05f);
}

TEST_CASE("ArcSinh: cross-validation with manual computation", "[arcsinh]") {
    ArcSinhStretch s;
    s.alpha = 200.0f;
    // arcsinh(200 * 0.05) / arcsinh(200) = arcsinh(10) / arcsinh(200)
    float expected = std::asinh(10.0f) / std::asinh(200.0f);
    REQUIRE(s.apply_scalar(0.05f) == Catch::Approx(expected).margin(1e-5f));
}

TEST_CASE("ArcSinh: visual output on M16", "[arcsinh][visual]") {
    auto img = test_util::load_m16_test_frame();
    if (img.empty()) { SKIP("M16 test data not available"); }

    // Background-subtract and normalize so signal fills [0, 1]
    test_util::prepare_for_stretch(img);

    ArcSinhStretch s;
    s.alpha = 500.0f;
    s.luminance_only = true;
    s.enabled = true;
    s.apply(img);

    bool ok = test_util::write_png("test/output/stretch_arcsinh.png", img, false);
    REQUIRE(ok);
}
