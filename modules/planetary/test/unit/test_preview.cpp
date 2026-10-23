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
    i.normDepth = 12;
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
    i.normDepth = 8;
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

TEST_CASE("info text: the normalisation depth is shown when it differs from the declared one", "[preview][info]") {
    FileInfo i = BayerInfo();
    i.normDepth = 16;
    CHECK(InfoText("x.ser", i, std::nullopt).find("Bit depth: 12 (normalised to the full 16-bit range)\n") !=
          std::string::npos);
    CHECK(InfoText("x.ser", BayerInfo(), std::nullopt).find("Bit depth: 12\n") != std::string::npos);
}

TEST_CASE("info text: a frame's own range warning follows the file's info", "[preview][info]") {
    CHECK(FrameInfoText("File: x.ser\nWarnings: none", "") == "File: x.ser\nWarnings: none");
    CHECK(FrameInfoText("File: x.ser\nWarnings: none", "frame 4 samples exceed the 6-bit range; values above 1.0") ==
          "File: x.ser\nWarnings: none\nWarning: frame 4 samples exceed the 6-bit range; values above 1.0");
}

TEST_CASE("info text: little-endian 16-bit data", "[preview][info]") {
    FileInfo i = BayerInfo();
    i.byteOrder = planetary::ser::ByteOrder::Little;
    CHECK(InfoText("x.ser", i, std::nullopt).find("Byte order: little-endian\n") != std::string::npos);
}

TEST_CASE("red lands in bits 16-23 and blue in bits 0-7 (ffRRGGBB, the RGB32 bitmap layout)", "[preview]") {
    const auto px = PreviewArgb(FloatImage{3, 1, 3, {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f}});
    CHECK(px[0] == 0xFFFF0000u);  // pure red
    CHECK(px[1] == 0xFF00FF00u);  // pure green
    CHECK(px[2] == 0xFF0000FFu);  // pure blue
}

TEST_CASE("RawSafe keeps text verbatim inside a <raw> block", "[preview][info]") {
    CHECK(RawSafe("Saturn <1> & co.ser") == "Saturn <1> & co.ser");
    // A closing raw tag (any case, whitespace inside) would end the block early:
    // its '<' is closed out, emitted as an entity, and the block reopened.
    CHECK(RawSafe("a</raw>b") == "a</raw>&lt;<raw>/raw>b");
    CHECK(RawSafe("a< / RaW>b") == "a</raw>&lt;<raw> / RaW>b");
    CHECK(RawSafe("a</rawb") == "a</raw>&lt;<raw>/rawb");
    CHECK(RawSafe("a</ra") == "a</ra");
}

TEST_CASE("DescribeException names any in-flight exception", "[preview][info]") {
    auto describe = [](auto thrower) {
        try {
            thrower();
        } catch (...) {
            return DescribeException(std::current_exception());
        }
        return std::string("nothing thrown");
    };
    CHECK(describe([] { throw planetary::ser::SerError("short read"); }) == "short read");
    CHECK(describe([] { throw std::bad_alloc(); }) == "out of memory");
    CHECK(describe([] { throw std::runtime_error("boom"); }) == "unexpected error: boom");
    CHECK(describe([] { throw 42; }) == "unknown error");
}

TEST_CASE("a red-dominant RGGB mosaic (the GUI smoke's bayer-colour.ser) previews red", "[preview]") {
    // Same mosaic as make-fixtures' bayer-colour.ser, frame 0.
    const int w = 64, h = 48;
    planetary::ser::Frame cfa{w, h, 1, {}};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int site = (y % 2) * 2 + x % 2;
            cfa.samples.push_back(static_cast<uint16_t>(site == 0 ? 60 + 150 * ((x / 8) % 2) : site == 3 ? 12 : 40));
        }
    const auto px = PreviewArgb(planetary::ser::DebayerBilinear(cfa, ColorId::BayerRGGB, 8));
    double r = 0, g = 0, b = 0;
    for (uint32_t p : px) {
        r += (p >> 16) & 0xFF;
        g += (p >> 8) & 0xFF;
        b += p & 0xFF;
    }
    CHECK(r >= 2 * g);
    CHECK(r >= 2 * b);
    // Raw CFA (debayer off) is grey: R == G == B everywhere.
    for (uint32_t p : PreviewArgb(planetary::ser::Normalize(cfa, 8)))
        CHECK((((p >> 16) & 0xFF) == ((p >> 8) & 0xFF) && ((p >> 8) & 0xFF) == (p & 0xFF)));
}

namespace {
FileInfo AviInfo() {
    FileInfo i;
    i.header.width = 32;
    i.header.height = 24;
    i.header.depth = 8;
    i.normDepth = 8;
    i.bytesPerSample = 1;
    i.frameCount = 1168;
    i.colorId = ColorId::BayerRGGB;
    i.container = planetary::ser::Container::Avi;
    planetary::ser::AviDetails d;
    d.frameRate = 29.97;
    d.bayerSource = "sidecar";
    planetary::ser::AsiairSidecar sc;
    sc.camera = "ZWO ASI585MC";
    sc.colourFormat = "RAW8";
    sc.bayer = "RG";
    sc.flip = "Vert(unimplemented)";
    sc.gain = "200";
    sc.exposure = "10ms";
    d.sidecar = sc;
    d.filenameStart = "2026-10-04 03:15:00";
    i.avi = d;
    return i;
}
}  // namespace

TEST_CASE("info text for an AVI: container, frame rate, Bayer source, sidecar, filename start", "[preview][info]") {
    const std::string t = InfoText("2026-10-04-031500-Saturn.avi", AviInfo(), std::nullopt);
    CHECK(t ==
          "File: 2026-10-04-031500-Saturn.avi\n"
          "Container: AVI\n"
          "Size: 32 \xc3\x97 24\n"
          "Frames: 1168\n"
          "Frame rate: 29.97 fps\n"
          "Colour: BAYER_RGGB\n"
          "Bayer: RGGB (sidecar)\n"
          "Bit depth: 8\n"
          "Byte order: n/a (8-bit samples)\n"
          "Camera: ZWO ASI585MC\n"
          "Gain: 200\n"
          "Exposure: 10ms\n"
          "Flip (sidecar; image not flipped): Vert(unimplemented)\n"
          "Start: 2026-10-04 03:15:00 (from filename, time zone unknown)\n"
          "Warnings: none");
}

TEST_CASE("info text for an AVI without sidecar or filename start: OpenDML, mono, warnings", "[preview][info]") {
    FileInfo i = AviInfo();
    i.container = planetary::ser::Container::AviOpenDml;
    i.colorId = ColorId::Mono;
    i.avi->frameRate = 25;
    i.avi->bayerSource = "none";
    i.avi->sidecar.reset();
    i.avi->filenameStart.reset();
    i.warnings = {"no Bayer pattern known"};
    const std::string t = InfoText("m.avi", i, std::nullopt);
    CHECK(t.find("Container: AVI (OpenDML)\n") != std::string::npos);
    CHECK(t.find("Frame rate: 25.00 fps\n") != std::string::npos);
    CHECK(t.find("Bayer: none (no sidecar pattern; choose one under Bayer pattern)\n") != std::string::npos);
    CHECK(t.find("(none)") == std::string::npos);  // the reader's token is not shown raw
    CHECK(t.find("Camera:") == std::string::npos);
    CHECK(t.find("Gain:") == std::string::npos);
    CHECK(t.find("Exposure:") == std::string::npos);
    CHECK(t.find("Flip") == std::string::npos);
    CHECK(t.find("Start:") == std::string::npos);
    CHECK(t.find("UTC") == std::string::npos);  // an AVI has no clock
    CHECK(t.find("\nWarning: no Bayer pattern known") != std::string::npos);
}

TEST_CASE("Bayer source tokens read as text", "[preview][info]") {
    FileInfo i = AviInfo();
    i.colorId = ColorId::Mono;
    i.avi->bayerSource = "none";
    // A sidecar whose Bayer value the reader did not recognise.
    i.avi->sidecar->bayer = "XY";
    CHECK(BayerText(i) == "none (sidecar value not recognised)");
    // A sidecar without a Bayer value: nothing to recognise.
    i.avi->sidecar->bayer.clear();
    CHECK(BayerText(i) == "none (no sidecar pattern; choose one under Bayer pattern)");
    i.avi->sidecar.reset();
    i.avi->sidecarStatus = planetary::ser::SidecarStatus::Absent;
    CHECK(BayerText(i) == "none (no sidecar pattern; choose one under Bayer pattern)");
    // A sidecar that is there but could not be read (or is over the size cap).
    for (auto status : {planetary::ser::SidecarStatus::Unreadable, planetary::ser::SidecarStatus::TooLarge}) {
        i.avi->sidecarStatus = status;
        CHECK(BayerText(i) == "none (sidecar could not be read; choose one under Bayer pattern)");
    }
    // Something at the sidecar path that is not a file (a directory, a FIFO, a dangling link).
    i.avi->sidecarStatus = planetary::ser::SidecarStatus::NotAFile;
    CHECK(BayerText(i) == "none (sidecar path is not a file; choose one under Bayer pattern)");
    i.avi->sidecarStatus = planetary::ser::SidecarStatus::Absent;
    i.avi->bayerSource = "chosen";
    CHECK(BayerText(i) == "none (chosen)");
    i.colorId = ColorId::BayerGBRG;
    CHECK(BayerText(i) == "GBRG (chosen)");
    i.colorId = ColorId::BayerRGGB;
    i.avi->bayerSource = "sidecar";
    CHECK(BayerText(i) == "RGGB (sidecar)");
    CHECK(BayerText(AviInfo()) == "RGGB (sidecar)");
    CHECK(InfoText("x.avi", i, std::nullopt).find("Bayer: RGGB (sidecar)\n") != std::string::npos);
    // The sidecar SERViewer wrote when the user saved a chosen pattern (Increment 2.1).
    i.avi->bayerSource = "sidecar (SERViewer)";
    CHECK(BayerText(i) == "RGGB (sidecar written by SERViewer)");
    CHECK(InfoText("x.avi", i, std::nullopt).find("Bayer: RGGB (sidecar written by SERViewer)\n") != std::string::npos);
}

TEST_CASE("info text for an AVI: empty sidecar fields get no line", "[preview][info]") {
    FileInfo i = AviInfo();
    i.avi->sidecar->gain.clear();
    i.avi->sidecar->flip.clear();
    i.colorId = ColorId::BayerRGGB;
    i.avi->bayerSource = "chosen";
    const std::string t = InfoText("x.avi", i, std::nullopt);
    CHECK(t.find("Bayer: RGGB (chosen)\n") != std::string::npos);
    CHECK(t.find("Camera: ZWO ASI585MC\n") != std::string::npos);
    CHECK(t.find("Gain:") == std::string::npos);
    CHECK(t.find("Flip") == std::string::npos);
}

TEST_CASE("info text for a SER is unchanged by the AVI fields", "[preview][info]") {
    // The same expected string as "info text names every field the viewer shows".
    CHECK(InfoText("Saturn.ser", BayerInfo(), 639243438000000000LL) ==
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

TEST_CASE("conversion rewords every 'showing the raw CFA as mono' warning to what it writes", "[preview][info]") {
    const std::string tail = "; showing the raw CFA as mono -- choose the pattern under Bayer pattern";
    const std::string writes = "; the SER will be written as MONO (raw CFA) -- set the Bayer pattern to write a Bayer SER";
    // No sidecar, an unreadable or oversized one, a sidecar without a Bayer line, an
    // unrecognised Bayer value.
    for (const std::string& reason : {std::string("no Bayer pattern known (no .avi.txt sidecar)"),
                                     std::string("sidecar a.avi.txt could not be read; no Bayer pattern known"),
                                     std::string("sidecar a.avi.txt could not be read (larger than 64 KiB); "
                                                 "no Bayer pattern known"),
                                     std::string("sidecar a.avi.txt has no Bayer value"),
                                     std::string("sidecar Bayer value XY is not recognised")}) {
        INFO(reason);
        CHECK(ConversionWarning(reason + tail) == reason + writes);
    }
    // Every other warning passes through untouched.
    CHECK(ConversionWarning("sidecar Colour Format is RAW16, not RAW8; using its Bayer value anyway") ==
          "sidecar Colour Format is RAW16, not RAW8; using its Bayer value anyway");
    CHECK(ConversionWarning(tail.substr(2)) == tail.substr(2));
}
