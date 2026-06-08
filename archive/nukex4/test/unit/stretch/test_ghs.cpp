#include "catch_amalgamated.hpp"
#include "nukex/stretch/ghs_stretch.hpp"
#include "png_writer.hpp"
#include "test_data_loader.hpp"
#include <cmath>

using namespace nukex;

// ── Boundary conditions ──

TEST_CASE("GHS: f(0) == 0", "[ghs]") {
    GHSStretch s;
    s.D = 5.0f;
    for (float b : {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f}) {
        s.b = b;
        REQUIRE(s.apply_scalar(0.0f) == Catch::Approx(0.0f).margin(1e-5f));
    }
}

TEST_CASE("GHS: f(1) == 1", "[ghs]") {
    GHSStretch s;
    s.D = 5.0f;
    for (float b : {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f}) {
        s.b = b;
        REQUIRE(s.apply_scalar(1.0f) == Catch::Approx(1.0f).margin(1e-5f));
    }
}

TEST_CASE("GHS: D=0 is identity", "[ghs]") {
    GHSStretch s;
    s.D = 0.0f;
    for (float x : {0.0f, 0.1f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        REQUIRE(s.apply_scalar(x) == Catch::Approx(x).margin(1e-5f));
    }
}

// ── Monotonicity ──

TEST_CASE("GHS: monotonically increasing for all b families", "[ghs]") {
    GHSStretch s;
    s.D = 5.0f;
    for (float b : {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f}) {
        s.b = b;
        float prev = -1.0f;
        for (int i = 0; i <= 100; i++) {
            float x = i / 100.0f;
            float y = s.apply_scalar(x);
            REQUIRE(y >= prev - 1e-6f);
            prev = y;
        }
    }
}

// ── Output range ──

TEST_CASE("GHS: output in [0, 1] for all b families and D values", "[ghs]") {
    GHSStretch s;
    for (float D : {1.0f, 5.0f, 20.0f}) {
        s.D = D;
        for (float b : {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f}) {
            s.b = b;
            for (int i = 0; i <= 100; i++) {
                float x = i / 100.0f;
                float y = s.apply_scalar(x);
                REQUIRE(y >= -1e-6f);
                REQUIRE(y <= 1.0f + 1e-6f);
            }
        }
    }
}

// ── Curve family cross-validation ──

TEST_CASE("GHS: b=-1 matches log1p stretch", "[ghs]") {
    // b=-1 with SP=0, LP=0, HP=1 should be log1p(D*x) / log1p(D)
    GHSStretch s;
    s.D = 10.0f;
    s.b = -1.0f;
    s.SP = 0.0f;
    s.LP = 0.0f;
    s.HP = 1.0f;

    for (float x : {0.0f, 0.1f, 0.3f, 0.5f, 0.7f, 1.0f}) {
        float expected = std::log1p(10.0f * x) / std::log1p(10.0f);
        REQUIRE(s.apply_scalar(x) == Catch::Approx(expected).margin(1e-4f));
    }
}

TEST_CASE("GHS: b=0 matches exponential stretch", "[ghs]") {
    // b=0 with SP=0: T(x) = 1 - exp(-D*x), normalized
    GHSStretch s;
    s.D = 5.0f;
    s.b = 0.0f;
    s.SP = 0.0f;
    s.LP = 0.0f;
    s.HP = 1.0f;

    float norm = 1.0f - std::exp(-5.0f);  // T(1)
    for (float x : {0.0f, 0.1f, 0.5f, 1.0f}) {
        float expected = (1.0f - std::exp(-5.0f * x)) / norm;
        REQUIRE(s.apply_scalar(x) == Catch::Approx(expected).margin(1e-4f));
    }
}

// ── Symmetry point ──

TEST_CASE("GHS: higher D → more aggressive stretch", "[ghs]") {
    GHSStretch lo, hi;
    lo.D = 2.0f; lo.b = 0.0f;
    hi.D = 10.0f; hi.b = 0.0f;
    // At x=0.1 (faint signal), higher D should produce brighter output
    REQUIRE(hi.apply_scalar(0.1f) > lo.apply_scalar(0.1f));
}

TEST_CASE("GHS: SP shifts peak contrast", "[ghs]") {
    GHSStretch s;
    s.D = 5.0f;
    s.b = 0.0f;

    // With SP=0, the steepest part of the curve is at x=0
    s.SP = 0.0f;
    float slope_at_0_sp0 = (s.apply_scalar(0.01f) - s.apply_scalar(0.0f)) / 0.01f;

    // With SP=0.5, the steepest part should be near x=0.5
    s.SP = 0.5f;
    float slope_at_05_sp05 = (s.apply_scalar(0.51f) - s.apply_scalar(0.49f)) / 0.02f;
    float slope_at_0_sp05 = (s.apply_scalar(0.01f) - s.apply_scalar(0.0f)) / 0.01f;

    // Slope near SP should be higher than slope far from SP
    REQUIRE(slope_at_05_sp05 > slope_at_0_sp05);
}

// ── Shadow/highlight protection ──

TEST_CASE("GHS: LP creates linear region below shadow point", "[ghs]") {
    GHSStretch s;
    s.D = 10.0f;
    s.b = 0.0f;
    s.SP = 0.5f;
    s.LP = 0.2f;
    s.HP = 1.0f;

    // In the linear region [0, LP), the slope should be constant
    float slope1 = (s.apply_scalar(0.05f) - s.apply_scalar(0.0f)) / 0.05f;
    float slope2 = (s.apply_scalar(0.15f) - s.apply_scalar(0.10f)) / 0.05f;
    REQUIRE(slope1 == Catch::Approx(slope2).margin(1e-3f));
}

TEST_CASE("GHS: HP creates linear region above highlight point", "[ghs]") {
    GHSStretch s;
    s.D = 10.0f;
    s.b = 0.0f;
    s.SP = 0.3f;
    s.LP = 0.0f;
    s.HP = 0.7f;

    // In the linear region (HP, 1], the slope should be constant
    float slope1 = (s.apply_scalar(0.85f) - s.apply_scalar(0.80f)) / 0.05f;
    float slope2 = (s.apply_scalar(0.95f) - s.apply_scalar(0.90f)) / 0.05f;
    REQUIRE(slope1 == Catch::Approx(slope2).margin(1e-3f));
}

// ── Visual output ──

TEST_CASE("GHS: visual output on M16", "[ghs][visual]") {
    auto img = test_util::load_m16_test_frame();
    if (img.empty()) { SKIP("M16 test data not available"); }

    test_util::prepare_for_stretch(img);

    GHSStretch s;
    s.D = 4.0f;
    s.b = 0.0f;     // Exponential family
    s.SP = 0.0f;    // Peak contrast at black point (like our optimized stretches)
    s.luminance_only = true;
    s.apply(img);

    bool ok = test_util::write_png_8bit("test/output/stretch_ghs.png", img);
    REQUIRE(ok);
}
