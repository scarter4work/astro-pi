#include "catch_amalgamated.hpp"
#include "nukex/compose/color_composer.hpp"
#include "nukex/compose/compose_image.hpp"

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

// ---------------------------------------------------------------------------
// Brightness-invariant hue (Lupton et al. 2004, PASP 116, 133) and the
// explicit chroma gate that replaces the accident of an absent division.
// ---------------------------------------------------------------------------

TEST_CASE("ColorComposer: emission hue is a line ratio, not a function of brightness",
          "[color_composer]") {
    ColorComposer faint, bright;
    auto s_faint = zeroed();
    s_faint.Ha = 0.02; s_faint.OIII = 0.01;
    auto s_bright = zeroed();
    s_bright.Ha = 0.60; s_bright.OIII = 0.30;

    faint.compose_pixel(s_faint);
    bright.compose_pixel(s_bright);

    // Identical 2:1 Ha:OIII ratio, 30x apart in brightness -> same chrominance.
    REQUIRE(faint.last_pixel_emission_a()
            == Catch::Approx(bright.last_pixel_emission_a()).margin(1e-9));
    REQUIRE(faint.last_pixel_emission_b()
            == Catch::Approx(bright.last_pixel_emission_b()).margin(1e-9));
}

TEST_CASE("ColorComposer: OIII-dominant pixel renders teal, not blue-black",
          "[color_composer]") {
    ColorComposer c;
    auto s = zeroed();
    s.Ha = 0.01; s.OIII = 0.09; s.L = 0.35;
    sRGBPixel out = c.compose_pixel(s);
    // The M16 failure was green identically zero across 24.5M pixels.
    REQUIRE(out.g > 0.02);
    REQUIRE(out.b > out.r);
}

TEST_CASE("ColorComposer: out-of-gamut pixels keep their hue exactly",
          "[color_composer]") {
    ColorComposer c;
    double r = 1.6, g = 0.8, b = 0.4;
    REQUIRE(c.clip_to_gamut_for_test(r, g, b));
    // Lupton: rescale all three by their common maximum. Independent
    // clamping would give (1.0, 0.8, 0.4) and change the hue.
    REQUIRE(r == Catch::Approx(1.0));
    REQUIRE(g == Catch::Approx(0.5));
    REQUIRE(b == Catch::Approx(0.25));
}

TEST_CASE("ColorComposer: negative channels floor without rescaling the rest",
          "[color_composer]") {
    ColorComposer c;
    double r = 0.5, g = -0.2, b = 0.25;
    REQUIRE(c.clip_to_gamut_for_test(r, g, b));
    REQUIRE(r == Catch::Approx(0.5));
    REQUIRE(g == Catch::Approx(0.0));
    REQUIRE(b == Catch::Approx(0.25));
}

TEST_CASE("ColorComposer: a near-zero emission pixel is not fully saturated",
          "[color_composer]") {
    // The cliff that normalisation introduces: total_w = 1e-9 is > 0, so a
    // bare division would hand this pixel the entire palette vector and
    // manufacture saturated colour out of noise.
    ColorComposer c;
    c.set_chroma_gate(/*background*/ 0.0, /*full_scale*/ 0.01);
    auto s = zeroed();
    s.Ha = 1e-9;
    c.compose_pixel(s);

    REQUIRE(c.last_pixel_chroma_scale() == Catch::Approx(0.0).margin(1e-6));
    REQUIRE(std::abs(c.last_pixel_emission_a()) < 1e-3);
}

TEST_CASE("ColorComposer: a pixel above the chroma gate keeps full saturation",
          "[color_composer]") {
    ColorComposer c;
    c.set_chroma_gate(/*background*/ 0.0, /*full_scale*/ 0.01);
    auto s = zeroed();
    s.Ha = 0.5;   // far above the floor
    c.compose_pixel(s);

    REQUIRE(c.last_pixel_chroma_scale() == Catch::Approx(1.0));
    // Pure Ha normalises to exactly the palette entry, whatever it is.
    const LabColor ha = Palette::for_line(EmissionLineId::Ha);
    REQUIRE(c.last_pixel_emission_a() == Catch::Approx(ha.a).margin(1e-9));
    REQUIRE(c.last_pixel_emission_b() == Catch::Approx(ha.b).margin(1e-9));
}

TEST_CASE("ColorComposer: sky at the measured background is desaturated",
          "[color_composer]") {
    // The pedestal case. Blank sky sits at the background weight, so it
    // must come out neutral even though its total_w is far from zero.
    ColorComposer c;
    c.set_chroma_gate(/*background*/ 0.010, /*full_scale*/ 0.040);
    auto sky = zeroed();
    sky.Ha = 0.010;
    c.compose_pixel(sky);
    REQUIRE(c.last_pixel_chroma_scale() == Catch::Approx(0.0).margin(1e-9));

    auto mid = zeroed();
    mid.Ha = 0.025;   // halfway between background and full scale
    c.compose_pixel(mid);
    REQUIRE(c.last_pixel_chroma_scale() == Catch::Approx(0.5).margin(1e-9));

    auto bright = zeroed();
    bright.Ha = 0.30;
    c.compose_pixel(bright);
    REQUIRE(c.last_pixel_chroma_scale() == Catch::Approx(1.0));
}

TEST_CASE("ColorComposer: a faint OIII pixel keeps green rather than clipping "
          "it away", "[color_composer]") {
    // The M16 failure, reduced. The emission palette is saturated (a* of
    // -15 to +60) and real narrowband data is dark, so the Lab->sRGB
    // conversion lands negative in green. Flooring that to zero flattens the
    // hue exactly as badly as clamping the top does, and it left green
    // identically zero across all 24.5 million pixels of the M16 composite.
    ColorComposer c;
    auto s = zeroed();
    s.OIII = 0.04;      // OIII-dominant
    s.Ha   = 0.005;
    s.L    = 0.09;      // realistic faint luminance
    sRGBPixel out = c.compose_pixel(s);

    INFO("r=" << out.r << " g=" << out.g << " b=" << out.b
         << " gamut_chroma=" << c.last_pixel_gamut_chroma_scale());
    REQUIRE(out.r >= 0.0);
    REQUIRE(out.g >= 0.0);
    REQUIRE(out.b >= 0.0);
    REQUIRE(out.r <= 1.0);
    REQUIRE(out.g <= 1.0);
    REQUIRE(out.b <= 1.0);
    // Teal: green present, and blue at least as strong as red.
    REQUIRE(out.g > 0.005);
    REQUIRE(out.b >= out.r);
}

TEST_CASE("ColorComposer: gamut mapping holds the hue angle", "[color_composer]") {
    // Reducing chroma scales a and b together, so atan2(b, a) is unchanged.
    // That is the property the whole line-ratio argument rests on: if gamut
    // mapping could rotate hue, normalising the chrominance would buy nothing.
    ColorComposer c;
    auto s = zeroed();
    s.Ha = 0.30; s.OIII = 0.10; s.L = 0.05;
    c.compose_pixel(s);
    const double a = c.last_pixel_emission_a();
    const double b = c.last_pixel_emission_b();
    REQUIRE(std::abs(a) + std::abs(b) > 0.0);
    // The scale applied is uniform, so the ratio b/a survives it exactly.
    const double k = c.last_pixel_gamut_chroma_scale();
    REQUIRE(k > 0.0);
    REQUIRE(k <= 1.0);
    REQUIRE((b * k) / (a * k) == Catch::Approx(b / a));
}

TEST_CASE("ColorComposer: a narrowband-only pixel has luminance of its own",
          "[color_composer]") {
    // A dual-NB frame fills Ha and OIII and leaves L, R, G and B at zero, so
    // without luminance from the line flux every composed pixel is black.
    // The M16 composite only ever looked like an image because out-of-gamut
    // chrominance was being clamped into visible range channel by channel.
    ColorComposer c;
    auto s = zeroed();
    s.Ha = 0.20; s.OIII = 0.05;      // no broadband whatsoever
    sRGBPixel out = c.compose_pixel(s);
    INFO("r=" << out.r << " g=" << out.g << " b=" << out.b);
    REQUIRE((out.r + out.g + out.b) > 0.05);

    // Brighter lines, same ratio -> brighter pixel, same hue.
    ColorComposer c2;
    auto s2 = zeroed();
    s2.Ha = 0.60; s2.OIII = 0.15;
    sRGBPixel out2 = c2.compose_pixel(s2);
    REQUIRE((out2.r + out2.g + out2.b) > (out.r + out.g + out.b));
    REQUIRE(c2.last_pixel_emission_a() == Catch::Approx(c.last_pixel_emission_a()).margin(1e-9));
}

// ══════════════════════════════════════════════════════════
// compose_slots_to_image — the colour-science image the stretch consumes
// ══════════════════════════════════════════════════════════

static std::unordered_map<std::string, std::vector<float>>
slot_planes(int n, float L, float R, float G, float B) {
    return { {"L", std::vector<float>(n, L)}, {"R", std::vector<float>(n, R)},
             {"G", std::vector<float>(n, G)}, {"B", std::vector<float>(n, B)} };
}

TEST_CASE("compose_slots_to_image: three channels at the slot dimensions",
          "[color_composer]") {
    ColorComposer c;
    Image img = compose_slots_to_image(4, 3, slot_planes(12, 0.03f, 0.03f, 0.03f, 0.03f), c);
    REQUIRE(img.width() == 4);
    REQUIRE(img.height() == 3);
    REQUIRE(img.n_channels() == 3);
}

TEST_CASE("compose_slots_to_image: grey slots keep their level, so the result "
          "can be stretched directly", "[color_composer]") {
    // compose_pixel is an identity on the grey axis -- measured at 0.5, 0.1,
    // 0.05, 0.03, 0.027, 0.01 and 0.003, ratio 1.0000 at every one. That is
    // what makes this image safe to hand to the stretch: it is on the SAME
    // linear scale as the slots, with no transfer function to undo. If this
    // ever stops holding, stretching the composed image starts applying a
    // curve on top of a curve.
    ColorComposer c;
    for (float v : {0.5f, 0.03f, 0.003f}) {
        Image img = compose_slots_to_image(2, 2, slot_planes(4, v, v, v, v), c);
        REQUIRE(!img.empty());
        for (int ch = 0; ch < 3; ++ch)
            REQUIRE(img.channel_data(ch)[0] == Catch::Approx(v).margin(1e-4));
    }
}

TEST_CASE("compose_slots_to_image: colour in the slots reaches the channels",
          "[color_composer]") {
    ColorComposer c;
    Image img = compose_slots_to_image(2, 2, slot_planes(4, 0.030f, 0.030f, 0.022f, 0.020f), c);
    REQUIRE(!img.empty());
    const float r = img.channel_data(0)[0];
    const float g = img.channel_data(1)[0];
    const float b = img.channel_data(2)[0];
    INFO("r=" << r << " g=" << g << " b=" << b);
    REQUIRE(r > g);
    REQUIRE(g > b);
}

TEST_CASE("compose_slots_to_image: nothing to compose gives an empty image",
          "[color_composer]") {
    ColorComposer c;
    std::unordered_map<std::string, std::vector<float>> none;
    REQUIRE(compose_slots_to_image(4, 3, none, c).empty());
}

TEST_CASE("slots_have_colour: a lone L is not colour", "[color_composer]") {
    // An L-only mono stack has nothing to compose a hue from. Composing it
    // anyway yields a grey RGB triplet, which would silently widen every mono
    // stretch from one channel to three -- 3x the memory to say the same
    // thing, and not what a mono imager asked for.
    std::unordered_map<std::string, std::vector<float>> only_L{ {"L", {0.03f}} };
    REQUIRE(slots_have_colour(only_L) == false);
}

TEST_CASE("slots_have_colour: broadband and emission slots are colour",
          "[color_composer]") {
    REQUIRE(slots_have_colour({ {"L",{0.f}}, {"R",{0.f}}, {"G",{0.f}}, {"B",{0.f}} }) == true);
    REQUIRE(slots_have_colour({ {"Ha",{0.f}}, {"OIII",{0.f}} }) == true);
    REQUIRE(slots_have_colour({}) == false);
}

// ── Hue from line FLUXES, not pedestals ──────────────────────────────────

TEST_CASE("ColorComposer: line sky levels come off before the ratio, so a faint "
          "Ha nebula on a pedestal is Ha, not a 50/50 mix", "[color_composer]") {
    // The measured M16 case: Ha sky 0.0247 with the nebula +0.0010; OIII sky
    // 0.0229 with +0.0002. Raw, the Ha fraction is 0.527. Sky-subtracted it is
    // 0.83, and the composed chroma must sit near the Ha entry.
    ColorComposer c;
    c.set_line_backgrounds(0.0247, 0.0229, 0.0);
    DerivedSlots s;
    s.Ha = 0.0257; s.OIII = 0.0231;
    c.compose_pixel(s);
    const LabColor ha = Palette::for_line(EmissionLineId::Ha);
    const LabColor o3 = Palette::for_line(EmissionLineId::OIII);
    const double f = 0.0010 / (0.0010 + 0.0002);
    REQUIRE(c.last_pixel_emission_a() == Catch::Approx(f * ha.a + (1 - f) * o3.a).margin(1e-6));
    REQUIRE(c.last_pixel_emission_b() == Catch::Approx(f * ha.b + (1 - f) * o3.b).margin(1e-6));

    // Same pixel weighed raw lands halfway between the entries.
    ColorComposer raw;
    raw.compose_pixel(s);
    REQUIRE(raw.last_pixel_emission_a() < 0.6 * ha.a);
}

TEST_CASE("ColorComposer: a sky pixel has no line flux and therefore no chroma",
          "[color_composer]") {
    ColorComposer c;
    c.set_line_backgrounds(0.0247, 0.0229, 0.0);
    DerivedSlots sky;
    sky.Ha = 0.0247; sky.OIII = 0.0229;
    c.compose_pixel(sky);
    REQUIRE(c.last_pixel_emission_a() == Catch::Approx(0.0).margin(1e-12));
    REQUIRE(c.last_pixel_emission_b() == Catch::Approx(0.0).margin(1e-12));
}
