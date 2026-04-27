#include "catch_amalgamated.hpp"
#include "nukex/stretch/mtf_stretch.hpp"
#include "nukex/stretch/arcsinh_stretch.hpp"
#include "png_writer.hpp"
#include "test_data_loader.hpp"
#include <cmath>

using namespace nukex;

TEST_CASE("MTF: apply_scalar(0) == 0", "[mtf]") {
    MTFStretch mtf;
    REQUIRE(mtf.apply_scalar(0.0f) == Catch::Approx(0.0f));
}

TEST_CASE("MTF: apply_scalar(1) == 1", "[mtf]") {
    MTFStretch mtf;
    REQUIRE(mtf.apply_scalar(1.0f) == Catch::Approx(1.0f));
}

TEST_CASE("MTF: apply_scalar(midtone) == 0.5", "[mtf]") {
    MTFStretch mtf;
    mtf.midtone = 0.25f;
    REQUIRE(mtf.apply_scalar(0.25f) == Catch::Approx(0.5f).margin(1e-5f));
}

TEST_CASE("MTF: midtone=0.5 is identity", "[mtf]") {
    MTFStretch mtf;
    mtf.midtone = 0.5f;
    for (float x : {0.0f, 0.1f, 0.3f, 0.5f, 0.7f, 0.9f, 1.0f}) {
        REQUIRE(mtf.apply_scalar(x) == Catch::Approx(x).margin(1e-5f));
    }
}

TEST_CASE("MTF: monotonically increasing", "[mtf]") {
    MTFStretch mtf;
    mtf.midtone = 0.25f;
    float prev = -1.0f;
    for (int i = 0; i <= 100; i++) {
        float x = i / 100.0f;
        float y = mtf.apply_scalar(x);
        REQUIRE(y >= prev - 1e-6f);
        prev = y;
    }
}

TEST_CASE("MTF: shadows/highlights clip and remap", "[mtf]") {
    MTFStretch mtf;
    mtf.shadows = 0.1f;
    mtf.highlights = 0.9f;
    mtf.midtone = 0.5f;
    REQUIRE(mtf.apply_scalar(0.05f) == Catch::Approx(0.0f));
    REQUIRE(mtf.apply_scalar(0.95f) == Catch::Approx(1.0f));
    REQUIRE(mtf.apply_scalar(0.5f) == Catch::Approx(0.5f).margin(0.01f));
}

TEST_CASE("MTF: midtone=0 returns 0", "[mtf]") {
    MTFStretch mtf;
    mtf.midtone = 0.0f;
    REQUIRE(mtf.apply_scalar(0.5f) == Catch::Approx(0.0f));
}

TEST_CASE("MTF: midtone=1 returns 1 for x<1, but formula gives 1", "[mtf]") {
    MTFStretch mtf;
    mtf.midtone = 1.0f;
    REQUIRE(mtf.apply_scalar(0.5f) == Catch::Approx(1.0f));
}

TEST_CASE("MTF: low midtone brightens image", "[mtf]") {
    MTFStretch mtf;
    mtf.midtone = 0.1f;
    // Low midtone → aggressive stretch → 0.1 should map well above 0.1
    REQUIRE(mtf.apply_scalar(0.1f) > 0.3f);
}

TEST_CASE("MTF: high midtone darkens image", "[mtf]") {
    MTFStretch mtf;
    mtf.midtone = 0.9f;
    // High midtone → mild stretch → 0.5 should map below 0.5
    REQUIRE(mtf.apply_scalar(0.5f) < 0.5f);
}

TEST_CASE("MTF: visual output on M16", "[mtf][visual]") {
    auto img = test_util::load_m16_test_frame();
    if (img.empty()) { SKIP("M16 test data not available"); }

    test_util::prepare_for_stretch(img);

    // MTF is a secondary stretch — apply arcsinh first to simulate a primary stretch
    ArcSinhStretch pre;
    pre.alpha = 500.0f;
    pre.luminance_only = true;
    pre.apply(img);

    MTFStretch mtf;
    mtf.midtone = 0.15f;
    mtf.enabled = true;
    mtf.apply(img);

    bool ok = test_util::write_png("test/output/stretch_mtf.png", img, false);
    REQUIRE(ok);
}
