#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>

#include "AviFile.h"
#include "AviWriter.h"
#include "FrameSource.h"
#include "TestUtil.h"

using namespace planetary::ser;
using namespace planetary::testing;

namespace {

// Frame f, top-down: sample (x, y) = x + 10*y + 50*f (mod 256).
std::vector<std::vector<uint8_t>> MakeFrames(int w, int h, int n) {
    std::vector<std::vector<uint8_t>> frames(static_cast<size_t>(n));
    for (int f = 0; f < n; ++f)
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) frames[f].push_back(static_cast<uint8_t>((x + 10 * y + 50 * f) & 0xFF));
    return frames;
}

AviSpec Avi(int w, int h) {
    AviSpec s;
    s.width = w;
    s.height = h;
    return s;
}

void CheckFrame(const FrameSource& src, int64_t index, const std::vector<uint8_t>& expected) {
    const Frame fr = src.ReadFrame(index);
    REQUIRE(fr.width == src.Info().header.width);
    REQUIRE(fr.height == src.Info().header.height);
    REQUIRE(fr.channels == 1);
    REQUIRE(fr.samples.size() == expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        INFO("frame " << index << " sample " << i);
        REQUIRE(fr.samples[i] == expected[i]);
    }
}

bool HasWarning(const FileInfo& info, const std::string& text) {
    return std::find(info.warnings.begin(), info.warnings.end(), text) != info.warnings.end();
}

std::string OpenError(const std::string& path) {
    try {
        AviFile::Open(path);
    } catch (const SerError& e) {
        return e.what();
    }
    FAIL("expected SerError");
    return {};
}

const char* IndexName(AviSpec::Index i) {
    return i == AviSpec::Idx1 ? "idx1" : i == AviSpec::OpenDml ? "OpenDML" : "none";
}

}  // namespace

TEST_CASE("AVI 1.0 idx1 top-down") {
    TempDir dir;
    const std::string p = dir.NewPath(".avi");
    const auto frames = MakeFrames(7, 5, 3);
    WriteAvi(p, Avi(7, 5), frames);

    const AviFile avi = AviFile::Open(p);
    const FileInfo& info = avi.Info();
    CHECK(info.header.width == 7);
    CHECK(info.header.height == 5);
    CHECK(info.header.depth == 8);
    CHECK(info.normDepth == 8);
    CHECK(info.planes == 1);
    CHECK(info.bytesPerSample == 1);
    CHECK(info.frameBytes == 35);
    CHECK(info.frameCount == 3);
    CHECK(info.colorId == ColorId::Mono);
    CHECK_FALSE(info.hasTimestamps);
    CHECK(info.container == Container::Avi);
    CHECK(info.warnings.empty());
    REQUIRE(info.avi.has_value());
    CHECK(info.avi->indexKind == "idx1");
    CHECK(info.avi->bayerSource == "none");
    CHECK(info.avi->droppedFrames == 0);
    CHECK(std::abs(info.avi->frameRate - 25.0) < 1e-9);
    for (int f = 0; f < 3; ++f) CheckFrame(avi, f, frames[f]);
    CHECK_FALSE(avi.FrameTimestamp(2).has_value());
}

TEST_CASE("idx1 with absolute offsets") {
    TempDir dir;
    const std::string p = dir.NewPath(".avi");
    const auto frames = MakeFrames(7, 5, 3);
    AviSpec s = Avi(7, 5);
    s.idx1Absolute = true;
    WriteAvi(p, s, frames);

    const AviFile avi = AviFile::Open(p);
    CHECK(avi.Info().avi->indexKind == "idx1");
    CHECK(avi.Info().frameCount == 3);
    CHECK(avi.Info().warnings.empty());
    for (int f = 0; f < 3; ++f) CheckFrame(avi, f, frames[f]);
}

TEST_CASE("OpenDML with two AVIX segments") {
    TempDir dir;
    const std::string p = dir.NewPath(".avi");
    const auto frames = MakeFrames(7, 5, 5);
    AviSpec s = Avi(7, 5);
    s.index = AviSpec::OpenDml;
    s.framesPerRiff = 2;
    WriteAvi(p, s, frames);

    const AviFile avi = AviFile::Open(p);
    CHECK(avi.Info().frameCount == 5);
    CHECK(avi.Info().container == Container::AviOpenDml);
    REQUIRE(avi.Info().avi.has_value());
    CHECK(avi.Info().avi->indexKind == "OpenDML indx");
    CHECK(avi.Info().warnings.empty());
    for (int f = 0; f < 5; ++f) CheckFrame(avi, f, frames[f]);
}

TEST_CASE("bottom-up rows are flipped") {
    TempDir dir;
    const std::string p = dir.NewPath(".avi");
    const auto frames = MakeFrames(8, 6, 2);
    AviSpec s = Avi(8, 6);
    s.bottomUp = true;
    WriteAvi(p, s, frames);

    const AviFile avi = AviFile::Open(p);
    CHECK(avi.Info().warnings.empty());
    for (int f = 0; f < 2; ++f) CheckFrame(avi, f, frames[f]);
}

TEST_CASE("width 13 padding") {
    const bool bottomUp = GENERATE(false, true);
    INFO("bottomUp " << bottomUp);
    TempDir dir;
    const std::string p = dir.NewPath(".avi");
    const auto frames = MakeFrames(13, 4, 3);
    AviSpec s = Avi(13, 4);  // DIB stride 16
    s.bottomUp = bottomUp;
    WriteAvi(p, s, frames);

    const AviFile avi = AviFile::Open(p);
    CHECK(avi.Info().frameBytes == 13 * 4);
    CHECK(avi.Info().frameCount == 3);
    for (int f = 0; f < 3; ++f) CheckFrame(avi, f, frames[f]);
}

TEST_CASE("Y800 fourcc") {
    const std::string fourcc = GENERATE(std::string("Y800"), std::string("GREY"), std::string("Y8  "));
    INFO("fourcc '" << fourcc << "'");
    TempDir dir;
    const std::string p = dir.NewPath(".avi");
    const auto frames = MakeFrames(6, 3, 2);
    AviSpec s = Avi(6, 3);
    s.fourcc = fourcc;
    WriteAvi(p, s, frames);

    const AviFile avi = AviFile::Open(p);
    CHECK(avi.Info().frameCount == 2);
    CHECK(avi.Info().warnings.empty());
    for (int f = 0; f < 2; ++f) CheckFrame(avi, f, frames[f]);
}

TEST_CASE("unindexed file is scanned with a warning") {
    const int framesPerRiff = GENERATE(0, 2);
    INFO("framesPerRiff " << framesPerRiff);
    TempDir dir;
    const std::string p = dir.NewPath(".avi");
    const auto frames = MakeFrames(7, 5, 5);
    AviSpec s = Avi(7, 5);
    s.index = AviSpec::None;
    s.framesPerRiff = framesPerRiff;
    WriteAvi(p, s, frames);

    const AviFile avi = AviFile::Open(p);
    CHECK(avi.Info().frameCount == 5);
    REQUIRE(avi.Info().avi.has_value());
    CHECK(avi.Info().avi->indexKind == "scanned");
    CHECK(avi.Info().warnings == std::vector<std::string>{"no index; scanned the file for frames"});
    CHECK(avi.Info().container == (framesPerRiff ? Container::AviOpenDml : Container::Avi));
    for (int f = 0; f < 5; ++f) CheckFrame(avi, f, frames[f]);
}

TEST_CASE("idx1 without indx in a multi-RIFF file: the AVIX frames are scanned, not lost") {
    TempDir dir;
    const std::string p = dir.NewPath(".avi");
    const auto frames = MakeFrames(7, 5, 5);
    AviSpec s = Avi(7, 5);
    s.framesPerRiff = 2;  // idx1 only covers frames 0-1
    WriteAvi(p, s, frames);

    const AviFile avi = AviFile::Open(p);
    CHECK(avi.Info().frameCount == 5);
    CHECK(avi.Info().avi->indexKind == "scanned");
    CHECK(avi.Info().warnings ==
          std::vector<std::string>{"idx1 covers only the first RIFF segment; scanned the file for frames"});
    for (int f = 0; f < 5; ++f) CheckFrame(avi, f, frames[f]);
}

TEST_CASE("zero-length frames are skipped and counted") {
    const auto index = GENERATE(AviSpec::Idx1, AviSpec::OpenDml, AviSpec::None);
    INFO("index " << IndexName(index));
    TempDir dir;
    const std::string p = dir.NewPath(".avi");
    const auto frames = MakeFrames(7, 5, 5);
    AviSpec s = Avi(7, 5);
    s.index = index;
    s.zeroLengthFrames = {1, 3};
    WriteAvi(p, s, frames);

    const AviFile avi = AviFile::Open(p);
    CHECK(avi.Info().frameCount == 3);
    REQUIRE(avi.Info().avi.has_value());
    CHECK(avi.Info().avi->droppedFrames == 2);
    CHECK(HasWarning(avi.Info(), "2 dropped (zero-length) frames skipped"));
    CheckFrame(avi, 0, frames[0]);
    CheckFrame(avi, 1, frames[2]);
    CheckFrame(avi, 2, frames[4]);
}

TEST_CASE("bad index offset") {
    const auto index = GENERATE(AviSpec::Idx1, AviSpec::OpenDml);
    INFO("index " << IndexName(index));
    TempDir dir;
    const std::string p = dir.NewPath(".avi");
    const auto frames = MakeFrames(7, 5, 4);
    AviSpec s = Avi(7, 5);
    s.index = index;
    s.corruptIndexEntry = 2;
    WriteAvi(p, s, frames);

    const AviFile avi = AviFile::Open(p);
    CHECK(avi.Info().frameCount == 2);
    CHECK(avi.Info().warnings ==
          std::vector<std::string>{"index entry 2 is invalid; using the first 2 frames"});
    for (int f = 0; f < 2; ++f) CheckFrame(avi, f, frames[f]);
    CHECK_THROWS_AS(avi.ReadFrame(2), SerError);
}

TEST_CASE("index entry pointing at a same-size chunk of another stream") {
    TempDir dir;
    const std::string p = dir.NewPath(".avi");
    const auto frames = MakeFrames(7, 5, 4);
    WriteAvi(p, Avi(7, 5), frames);  // idx1: the frame chunk headers come before the idx1 entries
    {
        std::fstream io(p, std::ios::in | std::ios::out | std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(io)), std::istreambuf_iterator<char>());
        size_t at = 0;
        for (int i = 0; i < 3; ++i) at = bytes.find("00db", i ? at + 4 : 0);  // frame 2's header
        REQUIRE(at != std::string::npos);
        io.seekp(static_cast<std::streamoff>(at));
        io.write("01db", 4);
    }
    const AviFile avi = AviFile::Open(p);
    CHECK(avi.Info().frameCount == 2);
    CHECK(avi.Info().warnings ==
          std::vector<std::string>{"index entry 2 is invalid; using the first 2 frames"});
}

TEST_CASE("truncated tail") {
    TempDir dir;
    const std::string p = dir.NewPath(".avi");
    const auto frames = MakeFrames(7, 5, 5);
    AviSpec s = Avi(7, 5);

    SECTION("OpenDML: the last index entry points past the end of the file") {
        s.index = AviSpec::OpenDml;
        s.framesPerRiff = 2;  // last segment is [ix00][frame 4]
        s.truncateBytes = 3;
        WriteAvi(p, s, frames);
        const AviFile avi = AviFile::Open(p);
        CHECK(avi.Info().frameCount == 4);
        CHECK(avi.Info().warnings ==
              std::vector<std::string>{"index entry 4 is invalid; using the first 4 frames"});
        for (int f = 0; f < 4; ++f) CheckFrame(avi, f, frames[f]);
    }
    SECTION("unindexed: the scan stops at the cut frame") {
        s.index = AviSpec::None;
        s.truncateBytes = 3;
        WriteAvi(p, s, frames);
        const AviFile avi = AviFile::Open(p);
        CHECK(avi.Info().frameCount == 4);
        CHECK(avi.Info().warnings ==
              std::vector<std::string>{"no index; scanned the file for frames",
                                       "video chunk 4 is truncated or malformed; using the first 4 frames"});
        for (int f = 0; f < 4; ++f) CheckFrame(avi, f, frames[f]);
    }
    SECTION("AVI 1.0: idx1 is cut off, so the file is scanned") {
        s.index = AviSpec::Idx1;
        s.truncateBytes = 8 + 16 * 5 + 3;  // the whole idx1 chunk and 3 bytes of frame 4
        WriteAvi(p, s, frames);
        const AviFile avi = AviFile::Open(p);
        CHECK(avi.Info().frameCount == 4);
        CHECK(avi.Info().avi->indexKind == "scanned");
        CHECK(avi.Info().warnings ==
              std::vector<std::string>{"no index; scanned the file for frames",
                                       "video chunk 4 is truncated or malformed; using the first 4 frames"});
        for (int f = 0; f < 4; ++f) CheckFrame(avi, f, frames[f]);
    }
}

TEST_CASE("trailing junk after the last RIFF is ignored") {
    const auto index = GENERATE(AviSpec::Idx1, AviSpec::OpenDml, AviSpec::None);
    INFO("index " << IndexName(index));
    TempDir dir;
    const std::string p = dir.NewPath(".avi");
    const auto frames = MakeFrames(7, 5, 5);
    AviSpec s = Avi(7, 5);
    s.index = index;
    s.framesPerRiff = index == AviSpec::OpenDml ? 2 : 0;
    s.trailingJunk = 1000;
    WriteAvi(p, s, frames);

    const AviFile avi = AviFile::Open(p);
    CHECK(avi.Info().frameCount == 5);
    if (index == AviSpec::None)
        CHECK(avi.Info().warnings == std::vector<std::string>{"no index; scanned the file for frames"});
    else
        CHECK(avi.Info().warnings.empty());
    for (int f = 0; f < 5; ++f) CheckFrame(avi, f, frames[f]);
}

TEST_CASE("non-identity palette is rejected") {
    TempDir dir;
    const std::string p = dir.NewPath(".avi");
    AviSpec s = Avi(4, 2);
    s.badPaletteEntry = 5;
    WriteAvi(p, s, MakeFrames(4, 2, 1));
    const std::string msg = OpenError(p);
    CHECK(msg.find("5=(200,0,0)") != std::string::npos);
}

TEST_CASE("inverted palette lists at most three differing entries") {
    TempDir dir;
    const std::string p = dir.NewPath(".avi");
    AviSpec s = Avi(4, 2);
    s.identityPalette = false;
    WriteAvi(p, s, MakeFrames(4, 2, 1));
    const std::string msg = OpenError(p);
    CHECK(msg.find("0=(255,255,255), 1=(254,254,254), 2=(253,253,253)") != std::string::npos);
    CHECK(msg.find("3=(") == std::string::npos);
}

TEST_CASE("MJPG fourcc is rejected") {
    TempDir dir;
    const std::string p = dir.NewPath(".avi");
    AviSpec s = Avi(4, 2);
    s.fourcc = "MJPG";
    s.bitCount = 24;
    WriteAvi(p, s, {std::vector<uint8_t>(4 * 2 * 3, 0)});
    const std::string msg = OpenError(p);
    CHECK(msg == "not supported: MJPG 24-bit video");
}

TEST_CASE("24-bit BI_RGB is rejected") {
    TempDir dir;
    const std::string p = dir.NewPath(".avi");
    AviSpec s = Avi(4, 2);
    s.bitCount = 24;
    WriteAvi(p, s, {std::vector<uint8_t>(4 * 2 * 3, 0)});
    const std::string msg = OpenError(p);
    CHECK(msg == "not supported: BI_RGB 24-bit video");
}

TEST_CASE("palette-change chunk is rejected") {
    const auto index = GENERATE(AviSpec::Idx1, AviSpec::OpenDml, AviSpec::None);
    INFO("index " << IndexName(index));
    TempDir dir;
    const std::string p = dir.NewPath(".avi");
    AviSpec s = Avi(4, 2);
    s.index = index;
    s.paletteChangeChunk = true;
    WriteAvi(p, s, MakeFrames(4, 2, 3));
    const std::string msg = OpenError(p);
    CHECK(msg.find("palette-change chunk") != std::string::npos);
    CHECK(msg.find("not supported") != std::string::npos);
}

TEST_CASE("frame rate from strh") {
    TempDir dir;
    const std::string p = dir.NewPath(".avi");
    AviSpec s = Avi(4, 2);
    s.fps = 29.97;
    WriteAvi(p, s, MakeFrames(4, 2, 2));
    const AviFile avi = AviFile::Open(p);
    REQUIRE(avi.Info().avi.has_value());
    CHECK(std::abs(avi.Info().avi->frameRate - 29.97) < 1e-6);
}

TEST_CASE("OpenVideo dispatches RIFF AVI to AviFile") {
    TempDir dir;
    const std::string p = dir.NewPath(".dat");
    const auto frames = MakeFrames(7, 5, 2);
    WriteAvi(p, Avi(7, 5), frames);

    auto src = OpenVideo(p);
    REQUIRE(src != nullptr);
    CHECK(dynamic_cast<AviFile*>(src.get()) != nullptr);
    CHECK(src->Info().container == Container::Avi);
    REQUIRE(src->Info().avi.has_value());
    CHECK(src->Info().avi->bayerSource == "none");
    for (int f = 0; f < 2; ++f) CheckFrame(*src, f, frames[f]);
}

TEST_CASE("ReadFrame out of range throws") {
    TempDir dir;
    const std::string p = dir.NewPath(".avi");
    WriteAvi(p, Avi(4, 2), MakeFrames(4, 2, 3));
    const AviFile avi = AviFile::Open(p);
    CHECK_THROWS_AS(avi.ReadFrame(-1), SerError);
    CHECK_THROWS_AS(avi.ReadFrame(3), SerError);
    CHECK_THROWS_AS(avi.FrameTimestamp(-1), SerError);
    CHECK_THROWS_AS(avi.FrameTimestamp(3), SerError);
}
