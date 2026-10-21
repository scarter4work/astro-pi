#include <catch_amalgamated.hpp>

#include <algorithm>
#include <chrono>
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

std::string ReadAll(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void WriteAt(const std::string& path, size_t at, const std::string& bytes) {
    std::fstream io(path, std::ios::in | std::ios::out | std::ios::binary);
    io.seekp(static_cast<std::streamoff>(at));
    io.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string Le32(uint32_t v) {
    std::string s(4, '\0');
    for (int i = 0; i < 4; ++i) s[i] = static_cast<char>(v >> (8 * i));
    return s;
}

// Renames the chunk holding `frame` (its first row is unique in these fixtures) to "01db":
// a well-formed chunk of the same size that belongs to another stream.
void MakeForeignChunk(const std::string& path, const std::vector<uint8_t>& frame, int width) {
    const std::string bytes = ReadAll(path);
    const size_t at = bytes.find(std::string(frame.begin(), frame.begin() + width));
    REQUIRE(at != std::string::npos);
    REQUIRE(bytes.compare(at - 8, 4, "00db") == 0);
    WriteAt(path, at - 8, "01db");
}

// Offset of the n-th AVISUPERINDEX entry ({qwOffset, dwSize, dwDuration}) in an OpenDML file.
size_t SuperIndexEntry(const std::string& path, int n) {
    const size_t indx = ReadAll(path).find("indx");
    REQUIRE(indx != std::string::npos);
    return indx + 8 + 24 + 16 * static_cast<size_t>(n);
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

TEST_CASE("Y800 with a positive biHeight is not flipped") {
    // BITMAPINFOHEADER: YUV formats are top-down whatever the sign; only RGB DIBs flip.
    TempDir dir;
    const std::string p = dir.NewPath(".avi");
    const auto frames = MakeFrames(6, 3, 2);
    AviSpec s = Avi(6, 3);
    s.fourcc = "Y800";
    s.bottomUp = true;  // biHeight > 0; the writer keeps Y800 rows top-down
    WriteAvi(p, s, frames);

    const AviFile avi = AviFile::Open(p);
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
    const auto corruption = GENERATE(AviSpec::PastEnd, AviSpec::WrongSize);
    INFO("index " << IndexName(index) << ", corruption " << (corruption == AviSpec::PastEnd ? "past end" : "wrong size"));
    TempDir dir;
    const std::string p = dir.NewPath(".avi");
    const auto frames = MakeFrames(7, 5, 4);
    AviSpec s = Avi(7, 5);
    s.index = index;
    s.corruptIndexEntry = 2;
    s.corruption = corruption;
    WriteAvi(p, s, frames);

    const AviFile avi = AviFile::Open(p);
    CHECK(avi.Info().frameCount == 2);
    CHECK(avi.Info().warnings ==
          std::vector<std::string>{"index entry 2 is invalid; using the first 2 frames"});
    for (int f = 0; f < 2; ++f) CheckFrame(avi, f, frames[f]);
    CHECK_THROWS_AS(avi.ReadFrame(2), SerError);
}

TEST_CASE("a foreign chunk at the first, middle or last entry is caught at open") {
    const auto index = GENERATE(AviSpec::Idx1, AviSpec::OpenDml);
    INFO("index " << IndexName(index));
    TempDir dir;
    const std::string p = dir.NewPath(".avi");
    const auto frames = MakeFrames(7, 5, 4);  // spot checks: frames 0, 2, 3
    AviSpec s = Avi(7, 5);
    s.index = index;
    WriteAvi(p, s, frames);

    SECTION("first") {
        MakeForeignChunk(p, frames[0], 7);
        CHECK(OpenError(p) ==
              "AVI has no usable video frames (index entry 0 is invalid; using the first 0 frames)");
    }
    SECTION("middle") {
        MakeForeignChunk(p, frames[2], 7);
        const AviFile avi = AviFile::Open(p);
        CHECK(avi.Info().frameCount == 2);
        CHECK(avi.Info().warnings ==
              std::vector<std::string>{"index entry 2 is invalid; using the first 2 frames"});
        for (int f = 0; f < 2; ++f) CheckFrame(avi, f, frames[f]);
    }
    SECTION("last") {
        MakeForeignChunk(p, frames[3], 7);
        const AviFile avi = AviFile::Open(p);
        CHECK(avi.Info().frameCount == 3);
        CHECK(avi.Info().warnings ==
              std::vector<std::string>{"index entry 3 is invalid; using the first 3 frames"});
        for (int f = 0; f < 3; ++f) CheckFrame(avi, f, frames[f]);
    }
}

TEST_CASE("a foreign chunk between the spot checks is caught by ReadFrame") {
    const auto index = GENERATE(AviSpec::Idx1, AviSpec::OpenDml);
    INFO("index " << IndexName(index));
    TempDir dir;
    const std::string p = dir.NewPath(".avi");
    const auto frames = MakeFrames(7, 5, 5);  // spot checks: frames 0, 2, 4
    AviSpec s = Avi(7, 5);
    s.index = index;
    WriteAvi(p, s, frames);
    MakeForeignChunk(p, frames[1], 7);

    const AviFile avi = AviFile::Open(p);
    CHECK(avi.Info().frameCount == 5);
    CHECK(avi.Info().warnings.empty());
    try {
        avi.ReadFrame(1);
        FAIL("expected SerError");
    } catch (const SerError& e) {
        CHECK(std::string(e.what()) == "index entry 1 points at 01db/40, not a video chunk");
    }
    for (int f : {0, 2, 3, 4}) CheckFrame(avi, f, frames[f]);
}

TEST_CASE("a super index repeating an ix00 offset ends the index") {
    TempDir dir;
    const std::string p = dir.NewPath(".avi");
    const auto frames = MakeFrames(7, 5, 5);
    AviSpec s = Avi(7, 5);
    s.index = AviSpec::OpenDml;
    s.framesPerRiff = 2;  // three ix00 chunks
    WriteAvi(p, s, frames);
    const std::string bytes = ReadAll(p);
    WriteAt(p, SuperIndexEntry(p, 1), bytes.substr(SuperIndexEntry(p, 0), 8));  // entry 1 -> ix00 #0

    const auto t0 = std::chrono::steady_clock::now();
    const AviFile avi = AviFile::Open(p);
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(1));
    CHECK(avi.Info().frameCount == 2);
    CHECK(avi.Info().warnings ==
          std::vector<std::string>{"index entry 2 is invalid; using the first 2 frames"});
}

TEST_CASE("a super index repeating an ix00 of zero-length entries ends the index") {
    // Zero-length entries skip the frame-overlap check, so only the ix## ordering rule stops a
    // repeated chunk of them from being counted again (and again).
    TempDir dir;
    const std::string p = dir.NewPath(".avi");
    const auto frames = MakeFrames(7, 5, 6);
    AviSpec s = Avi(7, 5);
    s.index = AviSpec::OpenDml;
    s.framesPerRiff = 2;
    s.zeroLengthFrames = {2, 3};  // the second ix00 holds only dropped frames
    WriteAvi(p, s, frames);
    const std::string bytes = ReadAll(p);
    WriteAt(p, SuperIndexEntry(p, 2), bytes.substr(SuperIndexEntry(p, 1), 8));  // entry 2 -> ix00 #1

    const AviFile avi = AviFile::Open(p);
    CHECK(avi.Info().frameCount == 2);
    CHECK(avi.Info().avi->droppedFrames == 2);
    CHECK(avi.Info().warnings ==
          std::vector<std::string>{"2 dropped (zero-length) frames skipped",
                                   "index entry 4 is invalid; using the first 2 frames"});
}

TEST_CASE("absurd index entry counts are bounded by the file") {
    TempDir dir;
    const std::string p = dir.NewPath(".avi");
    const auto frames = MakeFrames(7, 5, 5);
    AviSpec s = Avi(7, 5);
    s.index = AviSpec::OpenDml;
    s.framesPerRiff = 2;
    WriteAvi(p, s, frames);

    SECTION("an ix00 declaring 2^32-1 entries") {
        const std::string bytes = ReadAll(p);
        const size_t ix1 = bytes.find("ix00", bytes.find("ix00") + 4);  // the second segment's
        REQUIRE(ix1 != std::string::npos);
        WriteAt(p, ix1 + 8 + 4, Le32(0xFFFFFFFFu));  // nEntriesInUse
        const auto t0 = std::chrono::steady_clock::now();
        const AviFile avi = AviFile::Open(p);
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(1));
        CHECK(avi.Info().frameCount == 2);
        CHECK(avi.Info().warnings ==
              std::vector<std::string>{"index entry 2 is invalid; using the first 2 frames"});
    }
    SECTION("a super index declaring 2^32-1 entries") {
        WriteAt(p, SuperIndexEntry(p, 0) - 24 + 4, Le32(0xFFFFFFFFu));
        const AviFile avi = AviFile::Open(p);
        CHECK(avi.Info().frameCount == 5);
        CHECK(avi.Info().avi->indexKind == "scanned");
        CHECK(avi.Info().warnings ==
              std::vector<std::string>{"OpenDML indx is malformed (4294967295 entries do not fit in the chunk); not used",
                                       "idx1 covers only the first RIFF segment; scanned the file for frames"});
    }
}

TEST_CASE("a run of zero-length entries beyond the cap ends the frame table") {
    const auto index = GENERATE(AviSpec::Idx1, AviSpec::None);
    INFO("index " << IndexName(index));
    const int64_t cap = AviFile::kMaxDroppedRun;
    TempDir dir;
    const std::string p = dir.NewPath(".avi");
    auto frames = MakeFrames(4, 2, 1);
    frames.resize(static_cast<size_t>(cap + 3), frames[0]);  // frame 0, cap + 1 empty, one more
    AviSpec s = Avi(4, 2);
    s.index = index;
    for (int f = 1; f <= cap + 1; ++f) s.zeroLengthFrames.push_back(f);
    WriteAvi(p, s, frames);

    const AviFile avi = AviFile::Open(p);
    CHECK(avi.Info().frameCount == 1);
    CHECK(avi.Info().avi->droppedFrames == cap);
    CHECK(HasWarning(avi.Info(), std::to_string(cap) + " dropped (zero-length) frames skipped"));
    CHECK(HasWarning(avi.Info(), "more than " + std::to_string(cap) + " consecutive zero-length frames at entry " +
                                     std::to_string(cap + 1) + "; using the first 1 frames"));
}

TEST_CASE("nested 'rec ' lists are scanned up to the depth cap") {
    TempDir dir;
    const std::string p = dir.NewPath(".avi");
    const auto frames = MakeFrames(7, 5, 3);
    AviSpec s = Avi(7, 5);
    s.index = AviSpec::None;

    SECTION("within the cap") {
        s.recNesting = GENERATE(1, AviFile::kMaxRecDepth);
        WriteAvi(p, s, frames);
        const AviFile avi = AviFile::Open(p);
        CHECK(avi.Info().frameCount == 3);
        for (int f = 0; f < 3; ++f) CheckFrame(avi, f, frames[f]);
    }
    SECTION("1000 deep") {
        s.recNesting = 1000;
        WriteAvi(p, s, frames);
        const std::string msg = OpenError(p);
        CHECK(msg.find("'rec ' lists are nested 5 deep") != std::string::npos);
    }
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

// Ruling 4: open walks every gap only when strh declares palette changes; otherwise ReadFrame
// checks the gap immediately before the frame it decodes.
TEST_CASE("palette change with the strh flag set: rejected at open") {
    const bool withIdx1 = GENERATE(true, false);
    INFO("idx1 " << withIdx1);
    TempDir dir;
    const std::string p = dir.NewPath(".avi");
    AviSpec s = Avi(7, 5);
    s.index = AviSpec::OpenDml;
    s.openDmlIdx1 = withIdx1;  // idx1 covers segment 0 only, so it cannot see this one
    s.framesPerRiff = 2;
    s.paletteChangeChunk = true;
    s.paletteChangeAfter = 2;  // segment 1, between frames 2 and 3
    s.palChangesFlag = true;
    WriteAvi(p, s, MakeFrames(7, 5, 6));
    CHECK(OpenError(p).find("palette-change chunk") != std::string::npos);
}

TEST_CASE("palette change with the flag clear: the frame right after it throws") {
    const bool withIdx1 = GENERATE(true, false);
    INFO("idx1 " << withIdx1);
    TempDir dir;
    const std::string p = dir.NewPath(".avi");
    const auto frames = MakeFrames(7, 5, 6);
    AviSpec s = Avi(7, 5);
    s.index = AviSpec::OpenDml;
    s.openDmlIdx1 = withIdx1;
    s.framesPerRiff = 2;
    s.paletteChangeChunk = true;
    s.paletteChangeAfter = 2;  // segment 1, immediately before frame 3
    WriteAvi(p, s, frames);

    const AviFile avi = AviFile::Open(p);
    CHECK(avi.Info().frameCount == 6);
    CHECK(avi.Info().warnings.empty());
    for (int f = 0; f < 3; ++f) CheckFrame(avi, f, frames[f]);
    CHECK_THROWS_WITH(avi.ReadFrame(3), Catch::Matchers::ContainsSubstring("palette-change chunk"));
    CheckFrame(avi, 1, frames[1]);
}

TEST_CASE("palette change with the flag clear, before frame 0: rejected at open") {
    TempDir dir;
    const std::string p = dir.NewPath(".avi");
    AviSpec s = Avi(7, 5);
    s.index = AviSpec::OpenDml;
    s.openDmlIdx1 = false;
    s.paletteChangeChunk = true;
    s.paletteChangeAfter = -1;
    WriteAvi(p, s, MakeFrames(7, 5, 4));
    CHECK(OpenError(p).find("palette-change chunk") != std::string::npos);
}

TEST_CASE("palette change with the flag clear, not next to a read frame: the accepted limit") {
    // Documents Ruling 4's limit: a change in the gap before frame 3 goes unseen when frame 3
    // is never read; later frames decode with the identity palette.
    TempDir dir;
    const std::string p = dir.NewPath(".avi");
    const auto frames = MakeFrames(7, 5, 6);
    AviSpec s = Avi(7, 5);
    s.index = AviSpec::OpenDml;
    s.openDmlIdx1 = false;
    s.framesPerRiff = 2;
    s.paletteChangeChunk = true;
    s.paletteChangeAfter = 2;
    WriteAvi(p, s, frames);

    const AviFile avi = AviFile::Open(p);
    CHECK(avi.Info().frameCount == 6);
    CHECK(avi.Info().warnings.empty());
    for (int f : {5, 4, 0}) CheckFrame(avi, f, frames[f]);
}

TEST_CASE("an indx with no usable frames falls back to idx1 or the scan") {
    TempDir dir;
    const std::string p = dir.NewPath(".avi");
    const auto frames = MakeFrames(7, 5, 5);
    AviSpec s = Avi(7, 5);
    s.index = AviSpec::OpenDml;
    const std::string indxWarning =
        "OpenDML indx gave no usable frames (index entry 0 is invalid; using the first 0 frames); not used";

    SECTION("single RIFF: idx1") {
        WriteAvi(p, s, frames);
        WriteAt(p, ReadAll(p).find("ix00"), "JUNK");  // the only ix00
        const AviFile avi = AviFile::Open(p);
        CHECK(avi.Info().frameCount == 5);
        CHECK(avi.Info().avi->indexKind == "idx1");
        CHECK(avi.Info().warnings == std::vector<std::string>{indxWarning});
        for (int f = 0; f < 5; ++f) CheckFrame(avi, f, frames[f]);
    }
    SECTION("multi RIFF: scan") {
        s.framesPerRiff = 2;
        WriteAvi(p, s, frames);
        WriteAt(p, ReadAll(p).find("ix00"), "JUNK");  // segment 0's ix00
        const AviFile avi = AviFile::Open(p);
        CHECK(avi.Info().frameCount == 5);
        CHECK(avi.Info().avi->indexKind == "scanned");
        CHECK(avi.Info().warnings ==
              std::vector<std::string>{indxWarning, "idx1 covers only the first RIFF segment; scanned the file for frames"});
        for (int f = 0; f < 5; ++f) CheckFrame(avi, f, frames[f]);
    }
}

TEST_CASE("ASIAIR AVIX size 8 bytes too large: the next segment is still found") {
    const auto index = GENERATE(AviSpec::OpenDml, AviSpec::None);
    INFO("index " << IndexName(index));
    TempDir dir;
    const std::string p = dir.NewPath(".avi");
    const auto frames = MakeFrames(7, 5, 6);
    AviSpec s = Avi(7, 5);
    s.index = index;
    s.framesPerRiff = 2;  // segment 1 is inflated; segment 2 starts 8 bytes before its declared end
    s.asiairAvixSize = true;
    WriteAvi(p, s, frames);

    const AviFile avi = AviFile::Open(p);
    CHECK(avi.Info().frameCount == 6);
    for (int f = 0; f < 6; ++f) CheckFrame(avi, f, frames[f]);
}

TEST_CASE("frame rate falls back to avih when strh has none") {
    TempDir dir;
    const std::string p = dir.NewPath(".avi");
    AviSpec s = Avi(4, 2);
    s.fps = 25;
    s.strhRate = false;  // avih dwMicroSecPerFrame = 40000
    WriteAvi(p, s, MakeFrames(4, 2, 2));
    const AviFile avi = AviFile::Open(p);
    CHECK(std::abs(avi.Info().avi->frameRate - 25.0) < 1e-9);
}

TEST_CASE("a RIFF form other than AVIX after the AVI segment is rejected") {
    TempDir dir;
    const std::string p = dir.NewPath(".avi");
    AviSpec s = Avi(4, 2);
    s.framesPerRiff = 1;
    s.segmentForm = "WAVE";
    WriteAvi(p, s, MakeFrames(4, 2, 2));
    CHECK(OpenError(p).find("unexpected RIFF form 'WAVE'") != std::string::npos);
}

TEST_CASE("an AVIX segment too small for a movi list is rejected") {
    TempDir dir;
    const std::string p = dir.NewPath(".avi");
    WriteAvi(p, Avi(4, 2), MakeFrames(4, 2, 2));
    const size_t end = ReadAll(p).size();
    WriteAt(p, end, std::string("RIFF") + Le32(4) + "AVIX");
    CHECK(OpenError(p).find("is too small (4 bytes)") != std::string::npos);
}
