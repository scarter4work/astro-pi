#include "catch_amalgamated.hpp"
#include "nukex/compose/color_composer.hpp"

#include <cmath>

using namespace nukex;

static DerivedSlots zeroed() {
    DerivedSlots s;
    s.L = 0; s.R = 0; s.G = 0; s.B = 0;
    s.Ha = 0; s.OIII = 0; s.SII = 0;
    return s;
}

TEST_CASE("ColorComposer: pure broadband mid-gray maps to mid-gray sRGB",
          "[color_composer]") {
    ColorComposer c;
    auto pix = zeroed();
    pix.L = 0.5; pix.R = 0.5; pix.G = 0.5; pix.B = 0.5;
    sRGBPixel out = c.compose_pixel(pix);
    REQUIRE(out.r == Catch::Approx(0.5).margin(0.01));
    REQUIRE(out.g == Catch::Approx(0.5).margin(0.01));
    REQUIRE(out.b == Catch::Approx(0.5).margin(0.01));
}

TEST_CASE("ColorComposer: pure Hα signal (no broadband) drives red",
          "[color_composer]") {
    ColorComposer c;
    auto pix = zeroed();
    pix.L = 0.5; // mid luminance
    pix.Ha = 0.8;
    sRGBPixel out = c.compose_pixel(pix);
    REQUIRE(out.r > out.g);
    REQUIRE(out.r > out.b);
}

TEST_CASE("ColorComposer: pure OIII signal (no broadband) drives cyan-blue",
          "[color_composer]") {
    ColorComposer c;
    auto pix = zeroed();
    pix.L = 0.5;
    pix.OIII = 0.8;
    sRGBPixel out = c.compose_pixel(pix);
    REQUIRE(out.b > out.r);
    REQUIRE(out.g > out.r); // cyan = G+B > R
}

TEST_CASE("ColorComposer: pure SII signal (no broadband) drives deep red",
          "[color_composer]") {
    ColorComposer c;
    auto pix = zeroed();
    pix.L = 0.5;
    pix.SII = 0.8;
    sRGBPixel out = c.compose_pixel(pix);
    REQUIRE(out.r > out.g);
    REQUIRE(out.r > out.b);
}

TEST_CASE("ColorComposer: gamut soft-clip preserves hue, increments counter",
          "[color_composer]") {
    ColorComposer c;
    auto pix = zeroed();
    pix.L = 1.0;
    pix.Ha = 5.0;   // huge over-saturation
    pix.OIII = 0.0;
    pix.SII = 0.0;

    sRGBPixel out = c.compose_pixel(pix);
    REQUIRE(out.r >= 0.0); REQUIRE(out.r <= 1.0);
    REQUIRE(out.g >= 0.0); REQUIRE(out.g <= 1.0);
    REQUIRE(out.b >= 0.0); REQUIRE(out.b <= 1.0);
    REQUIRE(c.gamut_clipped_count() >= 1);

    // Hue preserved: red dominant
    REQUIRE(out.r >= out.g);
    REQUIRE(out.r >= out.b);
}

TEST_CASE("ColorComposer continuum-subtract: pure broadband stays unchanged",
          "[color_composer][continuum]") {
    ColorComposer c;
    c.set_mode(ColorComposer::Mode::CONTINUUM_SUBTRACT);
    c.set_continuum_coefficients({0.1, 0.1, 0.1});

    auto pix = zeroed();
    pix.L = 0.5; pix.R = 0.5; pix.G = 0.5; pix.B = 0.5;
    sRGBPixel out = c.compose_pixel(pix);
    REQUIRE(out.r == Catch::Approx(0.5).margin(0.01));
    REQUIRE(out.g == Catch::Approx(0.5).margin(0.01));
    REQUIRE(out.b == Catch::Approx(0.5).margin(0.01));
}

TEST_CASE("ColorComposer continuum-subtract: Hα with continuum is suppressed",
          "[color_composer][continuum]") {
    ColorComposer c;
    c.set_mode(ColorComposer::Mode::CONTINUUM_SUBTRACT);
    c.set_continuum_coefficients({1.0, 0.0, 0.0}); // k_Ha = 1

    // Pixel with continuum Ha + matching broadband R → subtraction zeroes Ha
    auto pix = zeroed();
    pix.L = 0.5; pix.R = 0.3; pix.G = 0; pix.B = 0;
    pix.Ha = 0.3; // exactly matches continuum

    sRGBPixel out = c.compose_pixel(pix);
    // Should look like a pure broadband pixel with no Hα chroma boost
    REQUIRE(c.last_pixel_emission_a() == Catch::Approx(0.0).margin(0.5));
    (void)out;
}

TEST_CASE("ColorComposer: zero signal everywhere → black", "[color_composer]") {
    ColorComposer c;
    sRGBPixel out = c.compose_pixel(zeroed());
    REQUIRE(out.r == Catch::Approx(0.0).margin(0.001));
    REQUIRE(out.g == Catch::Approx(0.0).margin(0.001));
    REQUIRE(out.b == Catch::Approx(0.0).margin(0.001));
}
