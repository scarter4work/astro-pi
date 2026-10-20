#include <catch_amalgamated.hpp>

#include <cmath>

#include "Debayer.h"

using namespace planetary::ser;

namespace {
// Channel index (0=R,1=G,2=B) at site (x,y) for a pattern.
int SiteColor(ColorId p, int x, int y) {
    static const int rggb[4] = {0, 1, 1, 2}, grbg[4] = {1, 0, 2, 1},
                     gbrg[4] = {1, 2, 0, 1}, bggr[4] = {2, 1, 1, 0};
    const int* t = p == ColorId::BayerRGGB ? rggb : p == ColorId::BayerGRBG ? grbg
                   : p == ColorId::BayerGBRG ? gbrg : bggr;
    return t[(y & 1) * 2 + (x & 1)];
}
}  // namespace

TEST_CASE("normalize divides by 2^depth - 1", "[debayer]") {
    Frame f{2, 1, 1, {4095, 0}};
    FloatImage n = Normalize(f, 12);
    CHECK(n.width == 2);
    CHECK(n.height == 1);
    CHECK(n.channels == 1);
    CHECK(n.data[0] == 1.0f);
    CHECK(n.data[1] == 0.0f);
    FloatImage m = Normalize(Frame{1, 1, 1, {255}}, 8);
    CHECK(m.data[0] == 1.0f);
}

TEST_CASE("debayer flat colours in all four patterns", "[debayer]") {
    const float rgb[3] = {0.8f, 0.5f, 0.2f};
    for (ColorId p : {ColorId::BayerRGGB, ColorId::BayerGRBG, ColorId::BayerGBRG, ColorId::BayerBGGR}) {
        Frame cfa{6, 6, 1, std::vector<uint16_t>(36)};
        for (int y = 0; y < 6; ++y)
            for (int x = 0; x < 6; ++x)
                cfa.samples[y * 6 + x] = static_cast<uint16_t>(std::lround(rgb[SiteColor(p, x, y)] * 65535.0f));
        FloatImage out = DebayerBilinear(cfa, p, 16);
        REQUIRE(out.channels == 3);
        REQUIRE(out.width == 6);
        REQUIRE(out.height == 6);
        for (int y = 0; y < 6; ++y)
            for (int x = 0; x < 6; ++x)
                for (int c = 0; c < 3; ++c)
                    REQUIRE(std::fabs(out.data[(y * 6 + x) * 3 + c] - rgb[c]) < 1e-4f);
    }
}

TEST_CASE("debayer interior is bilinear", "[debayer]") {
    // 4x4 RGGB mosaic, depth 8, non-linear field so neighbour mix-ups change the result.
    const int m[4][4] = {{ 10, 200,  35, 140},
                         { 90,  17, 230,  60},
                         {170,  45,  80, 250},
                         { 25, 120,  66, 190}};
    Frame cfa{4, 4, 1, std::vector<uint16_t>(16)};
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) cfa.samples[y * 4 + x] = static_cast<uint16_t>(m[y][x]);
    FloatImage out = DebayerBilinear(cfa, ColorId::BayerRGGB, 8);
    auto at = [&](int x, int y, int c) { return out.data[(y * 4 + x) * 3 + c]; };
    // Expected values, summed by hand from m[y][x] (x right, y down).
    // (2,2) red site: G = (230+45+250+66)/4 = 147.75 ; B = (17+60+120+190)/4 = 96.75
    CHECK(at(2, 2, 0) == Catch::Approx(80.0f / 255));
    CHECK(at(2, 2, 1) == Catch::Approx(147.75f / 255));
    CHECK(at(2, 2, 2) == Catch::Approx(96.75f / 255));
    // (1,1) blue site: G = (200+90+230+45)/4 = 141.25 ; R = (10+35+170+80)/4 = 73.75
    CHECK(at(1, 1, 2) == Catch::Approx(17.0f / 255));
    CHECK(at(1, 1, 1) == Catch::Approx(141.25f / 255));
    CHECK(at(1, 1, 0) == Catch::Approx(73.75f / 255));
    // (2,1) green on a blue row: B = (17+60)/2 = 38.5 horizontal ; R = (35+80)/2 = 57.5 vertical
    CHECK(at(2, 1, 1) == Catch::Approx(230.0f / 255));
    CHECK(at(2, 1, 2) == Catch::Approx(38.5f / 255));
    CHECK(at(2, 1, 0) == Catch::Approx(57.5f / 255));
    // (1,2) green on a red row: R = (170+80)/2 = 125 horizontal ; B = (17+120)/2 = 68.5 vertical
    CHECK(at(1, 2, 1) == Catch::Approx(45.0f / 255));
    CHECK(at(1, 2, 0) == Catch::Approx(125.0f / 255));
    CHECK(at(1, 2, 2) == Catch::Approx(68.5f / 255));
}

TEST_CASE("normalize maps the maximum to exactly 1.0 at every depth", "[debayer]") {
    for (int depth = 1; depth <= 16; ++depth) {
        const uint16_t maxV = static_cast<uint16_t>((1u << depth) - 1u);
        Frame f{1, 1, 1, {maxV}};
        CHECK(Normalize(f, depth).data[0] == 1.0f);
        Frame cfa{2, 2, 1, {maxV, maxV, maxV, maxV}};
        for (float v : DebayerBilinear(cfa, ColorId::BayerRGGB, depth).data) CHECK(v == 1.0f);
    }
}

TEST_CASE("normalize and debayer reject a wrong sample count", "[debayer]") {
    CHECK_THROWS_AS(Normalize(Frame{2, 2, 1, std::vector<uint16_t>(3)}, 8), SerError);
    CHECK_THROWS_AS(DebayerBilinear(Frame{4, 4, 1, std::vector<uint16_t>(15)}, ColorId::BayerRGGB, 8), SerError);
}

TEST_CASE("debayer edges mirror without changing parity", "[debayer]") {
    // Corner (0,0) is red in RGGB: G = mean(right, down, mirrored left=right, mirrored up=down).
    Frame cfa{4, 4, 1, std::vector<uint16_t>(16)};
    auto v = [](int x, int y) { return 5 + 4 * x + 9 * y; };
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) cfa.samples[y * 4 + x] = static_cast<uint16_t>(v(x, y));
    FloatImage out = DebayerBilinear(cfa, ColorId::BayerRGGB, 8);
    CHECK(out.data[1] == Catch::Approx((2 * v(1, 0) + 2 * v(0, 1)) / 4.0f / 255.0f));
    CHECK(out.data[2] == Catch::Approx(v(1, 1) / 255.0f));  // all four diagonals mirror to (1,1)
}

TEST_CASE("debayer rejects non-bayer and wrong input", "[debayer]") {
    Frame cfa{4, 4, 1, std::vector<uint16_t>(16)};
    CHECK_THROWS_AS(DebayerBilinear(cfa, ColorId::Mono, 8), SerError);
    CHECK_THROWS_AS(DebayerBilinear(cfa, ColorId::RGB, 8), SerError);
    Frame rgb{4, 4, 3, std::vector<uint16_t>(48)};
    CHECK_THROWS_AS(DebayerBilinear(rgb, ColorId::BayerRGGB, 8), SerError);
    Frame thin{1, 4, 1, std::vector<uint16_t>(4)};
    CHECK_THROWS_AS(DebayerBilinear(thin, ColorId::BayerRGGB, 8), SerError);
}

TEST_CASE("normalize and debayer reject a depth outside 1..16", "[debayer]") {
    Frame f{2, 2, 1, std::vector<uint16_t>(4)};
    for (int depth : {0, -1, 17, 32, 33}) {
        CHECK_THROWS_WITH(Normalize(f, depth), Catch::Matchers::ContainsSubstring("bit depth"));
        CHECK_THROWS_WITH(DebayerBilinear(f, ColorId::BayerRGGB, depth),
                          Catch::Matchers::ContainsSubstring("bit depth"));
        CHECK_THROWS_AS(Normalize(f, depth), SerError);
        CHECK_THROWS_AS(DebayerBilinear(f, ColorId::BayerRGGB, depth), SerError);
    }
}
