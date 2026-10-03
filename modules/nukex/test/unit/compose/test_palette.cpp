#include "catch_amalgamated.hpp"
#include "nukex/compose/palette.hpp"

#include <cmath>

using namespace nukex;

TEST_CASE("Palette: Ha vector points to warm red", "[palette]") {
    LabColor v = Palette::for_line(EmissionLineId::Ha);
    REQUIRE(v.a == Catch::Approx(+50.0).margin(0.5));
    REQUIRE(v.b == Catch::Approx(+25.0).margin(0.5));
}

TEST_CASE("Palette: OIII vector points to teal", "[palette]") {
    LabColor v = Palette::for_line(EmissionLineId::OIII);
    REQUIRE(v.a == Catch::Approx(-32.0).margin(0.5));
    REQUIRE(v.b == Catch::Approx(-8.0).margin(0.5));
}

TEST_CASE("Palette: SII vector points to deep red", "[palette]") {
    LabColor v = Palette::for_line(EmissionLineId::SII);
    REQUIRE(v.a == Catch::Approx(+62.0).margin(0.5));
    REQUIRE(v.b == Catch::Approx(+12.0).margin(0.5));
}

TEST_CASE("Palette property: no vector in green quadrant (-a*, +b*)",
          "[palette][property]") {
    for (auto line : {EmissionLineId::Ha, EmissionLineId::OIII, EmissionLineId::SII}) {
        LabColor v = Palette::for_line(line);
        const bool in_green_quadrant = (v.a < 0.0 && v.b > 0.0);
        INFO("Line " << static_cast<int>(line) << " has (a=" << v.a << ", b=" << v.b << ")");
        REQUIRE_FALSE(in_green_quadrant);
    }
}

TEST_CASE("Palette property: every vector has nonzero chroma", "[palette][property]") {
    for (auto line : {EmissionLineId::Ha, EmissionLineId::OIII, EmissionLineId::SII}) {
        LabColor v = Palette::for_line(line);
        const double chroma = std::sqrt(v.a * v.a + v.b * v.b);
        INFO("Line " << static_cast<int>(line) << " chroma = " << chroma);
        REQUIRE(chroma > 10.0);
    }
}

TEST_CASE("Palette property: an Ha/OIII mix never turns magenta", "[palette][property]") {
    // The composer blends by the Lab vector mean. With Ha and OIII nearly
    // opposite, a mix desaturates toward grey rather than swinging through a
    // third hue; the previous OIII entry put every 40%+ OIII mix at -35 deg
    // (magenta). Assert the mix hue stays out of the magenta sector for every
    // blend that still has meaningful chroma.
    const auto ha = Palette::for_line(EmissionLineId::Ha);
    const auto o3 = Palette::for_line(EmissionLineId::OIII);
    for (double f = 0.0; f <= 1.0001; f += 0.05) {
        const double a = f * ha.a + (1.0 - f) * o3.a;
        const double b = f * ha.b + (1.0 - f) * o3.b;
        const double chroma = std::hypot(a, b);
        if (chroma < 8.0) continue;                 // effectively grey: no hue
        const double hue = std::atan2(b, a) * 180.0 / 3.14159265358979;
        INFO("Ha fraction " << f << " hue " << hue << " chroma " << chroma);
        REQUIRE_FALSE((hue > -120.0 && hue < -10.0)); // magenta/purple sector
    }
}
