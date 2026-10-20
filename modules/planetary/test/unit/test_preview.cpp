#include <catch_amalgamated.hpp>

#include <cmath>
#include <limits>
#include <stdexcept>

#include "PreviewPixels.h"
#include "SerInfoText.h"

using namespace planetary::preview;
using planetary::ser::ColorId;
using planetary::ser::FileInfo;
using planetary::ser::FloatImage;

TEST_CASE("one channel is stretched 0..max onto 0..255 and replicated to grey", "[preview]") {
    const FloatImage img{3, 1, 1, {0.0f, 0.25f, 0.5f}};
    const auto px = PreviewArgb(img);
    REQUIRE(px.size() == 3);
    CHECK(px[0] == 0xFF000000u);
    CHECK(px[1] == 0xFF808080u);  // 0.25/0.5*255 = 127.5 -> 128
    CHECK(px[2] == 0xFFFFFFFFu);
}

TEST_CASE("three channels map to R, G, B with one shared maximum", "[preview]") {
    const FloatImage img{2, 1, 3, {0.8f, 0.4f, 0.0f, 0.0f, 0.2f, 0.4f}};
    const auto px = PreviewArgb(img);
    REQUIRE(px.size() == 2);
    CHECK(px[0] == 0xFFFF8000u);  // R 255, G 127.5 -> 128, B 0
    CHECK(px[1] == 0xFF004080u);  // R 0, G 63.75 -> 64, B 127.5 -> 128
}

TEST_CASE("rows stay top-down in the order of the samples", "[preview]") {
    const FloatImage img{1, 2, 1, {1.0f, 0.0f}};
    const auto px = PreviewArgb(img);
    CHECK(px[0] == 0xFFFFFFFFu);
    CHECK(px[1] == 0xFF000000u);
}

TEST_CASE("a frame whose maximum is <= 0 renders black", "[preview]") {
    for (float v : {0.0f, -0.5f}) {
        const auto px = PreviewArgb(FloatImage{2, 2, 1, {v, v, v, v}});
        REQUIRE(px.size() == 4);
        for (uint32_t p : px) CHECK(p == 0xFF000000u);
    }
}

TEST_CASE("negative and NaN samples render 0; non-finite values do not set the maximum", "[preview]") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    const auto px = PreviewArgb(FloatImage{4, 1, 1, {nan, -1.0f, 0.5f, inf}});
    CHECK(px[0] == 0xFF000000u);
    CHECK(px[1] == 0xFF000000u);
    CHECK(px[2] == 0xFFFFFFFFu);  // 0.5 is the finite maximum
    CHECK(px[3] == 0xFFFFFFFFu);  // +inf clamps to white
}

TEST_CASE("bad images are refused", "[preview]") {
    CHECK_THROWS_AS(PreviewArgb(FloatImage{}), std::invalid_argument);
    CHECK_THROWS_AS(PreviewArgb(FloatImage{1, 1, 2, {0.0f, 0.0f}}), std::invalid_argument);
    CHECK_THROWS_AS(PreviewArgb(FloatImage{2, 2, 1, {0.0f}}), std::invalid_argument);
}

TEST_CASE("the preview rectangle keeps the aspect ratio, centred in the box", "[preview]") {
    auto check = [](FitBox r, int x0, int y0, int x1, int y1) {
        CHECK(r.x0 == x0);
        CHECK(r.y0 == y0);
        CHECK(r.x1 == x1);
        CHECK(r.y1 == y1);
    };
    check(FitRect(400, 300, 320, 240), 0, 0, 400, 300);   // same aspect, scaled up
    check(FitRect(400, 300, 32, 24), 0, 0, 400, 300);
    check(FitRect(400, 300, 100, 100), 50, 0, 350, 300);  // pillarbox
    check(FitRect(400, 300, 640, 240), 0, 75, 400, 225);  // letterbox
    check(FitRect(401, 300, 100, 100), 50, 0, 350, 300);  // odd remainder goes right
    check(FitRect(0, 300, 100, 100), 0, 0, 0, 0);
    check(FitRect(400, 300, 0, 100), 0, 0, 0, 0);
}

namespace {
FileInfo BayerInfo() {
    FileInfo i;
    i.header.width = 320;
    i.header.height = 240;
    i.header.depth = 12;
    i.header.dateTimeUtc = 639243434615000000LL;  // 2026-09-07T02:04:21.500
    i.colorId = ColorId::BayerRGGB;
    i.bytesPerSample = 2;
    i.frameCount = 563;
    i.byteOrder = planetary::ser::ByteOrder::Big;
    return i;
}
}  // namespace

TEST_CASE("info text names every field the viewer shows", "[preview][info]") {
    const std::string t = InfoText("Saturn.ser", BayerInfo(), 639243438000000000LL);
    CHECK(t ==
          "File: Saturn.ser\n"
          "Size: 320 \xc3\x97 240\n"
          "Frames: 563\n"
          "Colour: BAYER_RGGB\n"
          "Bit depth: 12\n"
          "Byte order: big-endian\n"
          "First frame UTC: 2026-09-07T02:10:00.000\n"
          "Header UTC: 2026-09-07T02:04:21.500\n"
          "Warnings: none");
}

TEST_CASE("info text: 8-bit samples, no times, and each warning on its own line", "[preview][info]") {
    FileInfo i = BayerInfo();
    i.colorId = ColorId::Mono;
    i.header.depth = 8;
    i.bytesPerSample = 1;
    i.header.dateTimeUtc = 0;
    i.warnings = {"header frame count is 0; using 2 complete frames found in the file", "second"};
    const std::string t = InfoText("m.ser", i, std::nullopt);
    CHECK(t.find("Colour: MONO\n") != std::string::npos);
    CHECK(t.find("Byte order: n/a (8-bit samples)\n") != std::string::npos);
    CHECK(t.find("First frame UTC: none\n") != std::string::npos);
    CHECK(t.find("Header UTC: none\n") != std::string::npos);
    CHECK(t.find("Warning: header frame count is 0; using 2 complete frames found in the file\nWarning: second") !=
          std::string::npos);
    CHECK(t.find("Warnings: none") == std::string::npos);
}

TEST_CASE("info text: little-endian 16-bit data", "[preview][info]") {
    FileInfo i = BayerInfo();
    i.byteOrder = planetary::ser::ByteOrder::Little;
    CHECK(InfoText("x.ser", i, std::nullopt).find("Byte order: little-endian\n") != std::string::npos);
}
