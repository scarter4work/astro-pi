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
    // 4x4 RGGB mosaic, depth 8: v(x,y) = 1 + 3x + 17y.
    Frame cfa{4, 4, 1, std::vector<uint16_t>(16)};
    auto v = [](int x, int y) { return 1 + 3 * x + 17 * y; };
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) cfa.samples[y * 4 + x] = static_cast<uint16_t>(v(x, y));
    FloatImage out = DebayerBilinear(cfa, ColorId::BayerRGGB, 8);
    auto at = [&](int x, int y, int c) { return out.data[(y * 4 + x) * 3 + c]; };
    const float s = 255.0f;
    // (2,2) is a red site: R is the sample, G the mean of 4 neighbours, B the mean of 4 diagonals.
    CHECK(at(2, 2, 0) == Catch::Approx(v(2, 2) / s));
    CHECK(at(2, 2, 1) == Catch::Approx((v(2, 1) + v(1, 2) + v(3, 2) + v(2, 3)) / 4.0f / s));
    CHECK(at(2, 2, 2) == Catch::Approx((v(1, 1) + v(3, 1) + v(1, 3) + v(3, 3)) / 4.0f / s));
    // (1,1) is a blue site: R is the mean of 4 diagonals.
    CHECK(at(1, 1, 2) == Catch::Approx(v(1, 1) / s));
    CHECK(at(1, 1, 0) == Catch::Approx((v(0, 0) + v(2, 0) + v(0, 2) + v(2, 2)) / 4.0f / s));
    CHECK(at(1, 1, 1) == Catch::Approx((v(1, 0) + v(0, 1) + v(2, 1) + v(1, 2)) / 4.0f / s));
    // (2,1) is green on a blue row (horizontal neighbours blue): B horizontal, R vertical.
    CHECK(at(2, 1, 1) == Catch::Approx(v(2, 1) / s));
    CHECK(at(2, 1, 2) == Catch::Approx((v(1, 1) + v(3, 1)) / 2.0f / s));
    CHECK(at(2, 1, 0) == Catch::Approx((v(2, 0) + v(2, 2)) / 2.0f / s));
    // (1,2) is green on a red row: R horizontal, B vertical.
    CHECK(at(1, 2, 0) == Catch::Approx((v(0, 2) + v(2, 2)) / 2.0f / s));
    CHECK(at(1, 2, 2) == Catch::Approx((v(1, 1) + v(1, 3)) / 2.0f / s));
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
