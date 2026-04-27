#include "catch_amalgamated.hpp"
#include "nukex/compose/palette.hpp"

#include <cmath>

using namespace nukex;

TEST_CASE("Palette: Ha vector points to warm red", "[palette]") {
    LabColor v = Palette::for_line(EmissionLineId::Ha);
    REQUIRE(v.a == Catch::Approx(+50.0).margin(0.5));
    REQUIRE(v.b == Catch::Approx(+10.0).margin(0.5));
}

TEST_CASE("Palette: OIII vector points to cyan-blue", "[palette]") {
    LabColor v = Palette::for_line(EmissionLineId::OIII);
    REQUIRE(v.a == Catch::Approx(-15.0).margin(0.5));
    REQUIRE(v.b == Catch::Approx(-35.0).margin(0.5));
}

TEST_CASE("Palette: SII vector points to deep red-orange", "[palette]") {
    LabColor v = Palette::for_line(EmissionLineId::SII);
    REQUIRE(v.a == Catch::Approx(+60.0).margin(0.5));
    REQUIRE(v.b == Catch::Approx(+25.0).margin(0.5));
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
