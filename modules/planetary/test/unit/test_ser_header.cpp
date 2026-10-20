#include <catch_amalgamated.hpp>

#include "SerFile.h"
#include "SerTime.h"
#include "SerWriter.h"
#include "TestUtil.h"

using namespace planetary::ser;
using namespace planetary::testing;

namespace {
std::vector<std::vector<uint8_t>> Frames(int n, size_t bytes) {
    return std::vector<std::vector<uint8_t>>(n, std::vector<uint8_t>(bytes, 7));
}

// Days from 0001-01-01 to y-m-d, independent of the library (Gregorian leap rule).
int64_t DaysFromYear1(int y, int m, int d) {
    auto leap = [](int yy) { return (yy % 4 == 0 && yy % 100 != 0) || yy % 400 == 0; };
    static const int md[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int64_t days = 0;
    for (int yy = 1; yy < y; ++yy) days += leap(yy) ? 366 : 365;
    for (int mm = 1; mm < m; ++mm) days += md[mm - 1] + ((mm == 2 && leap(y)) ? 1 : 0);
    return days + (d - 1);
}
}  // namespace

TEST_CASE("parses mono 8-bit header", "[ser]") {
    auto p = TempPath();
    auto s = Spec(0, 64, 48, 8, 3);
    s.observer = "Scott";
    s.littleEndianField = 1;
    s.dateTimeUtc = 638000000000000000LL;
    WriteSer(p, s, Frames(3, 64 * 48));
    auto f = SerFile::Open(p);
    const auto& i = f.Info();
    CHECK(i.colorId == ColorId::Mono);
    CHECK(i.planes == 1);
    CHECK(i.bytesPerSample == 1);
    CHECK(i.frameBytes == 3072);
    CHECK(i.frameCount == 3);
    CHECK(i.header.observer == "Scott");
    CHECK(i.header.littleEndianField == 1);
    CHECK(i.header.dateTimeUtc == 638000000000000000LL);
    CHECK(i.byteOrder == ByteOrder::Big);
    CHECK(i.warnings.empty());
}

TEST_CASE("header strings are trimmed at both ends", "[ser]") {
    auto p = TempPath();
    auto s = Spec(0, 4, 4, 8, 1);
    s.observer = "  Scott  ";
    s.instrument = "\tZWO ASI585MC ";
    s.telescope = "   ";
    WriteSer(p, s, Frames(1, 16));
    const auto i = SerFile::Open(p).Info();
    CHECK(i.header.observer == "Scott");
    CHECK(i.header.instrument == "ZWO ASI585MC");
    CHECK(i.header.telescope.empty());
}

TEST_CASE("parses rgb 16-bit header", "[ser]") {
    auto p = TempPath();
    WriteSer(p, Spec(100, 10, 8, 12, 2), Frames(2, 10 * 8 * 6));
    auto i = SerFile::Open(p).Info();
    CHECK(i.colorId == ColorId::RGB);
    CHECK(i.planes == 3);
    CHECK(i.bytesPerSample == 2);
    CHECK(i.frameBytes == 10 * 8 * 6);
}

TEST_CASE("bgr is accepted", "[ser]") {
    auto p = TempPath();
    WriteSer(p, Spec(101, 4, 4, 8, 1), Frames(1, 48));
    CHECK(SerFile::Open(p).Info().colorId == ColorId::BGR);
}

TEST_CASE("rejects bad magic", "[ser]") {
    auto p = TempPath();
    auto s = Spec(0, 4, 4, 8, 1);
    s.fileId = "NOT-A-SERFILE.";
    WriteSer(p, s, Frames(1, 16));
    try {
        SerFile::Open(p);
        FAIL("expected SerError");
    } catch (const SerError& e) {
        CHECK(std::string(e.what()).find("LUCAM-RECORDER") != std::string::npos);
    }
}

TEST_CASE("rejects cmy bayer", "[ser]") {
    auto p = TempPath();
    WriteSer(p, Spec(16, 4, 4, 8, 1), Frames(1, 16));
    try {
        SerFile::Open(p);
        FAIL("expected SerError");
    } catch (const SerError& e) {
        CHECK(std::string(e.what()).find("not supported") != std::string::npos);
    }
}

TEST_CASE("rejects depth 0 and 17", "[ser]") {
    for (int depth : {0, 17}) {
        auto p = TempPath();
        WriteSer(p, Spec(0, 4, 4, depth, 1), Frames(1, 32));
        CHECK_THROWS_AS(SerFile::Open(p), SerError);
    }
}

TEST_CASE("rejects zero width", "[ser]") {
    auto p = TempPath();
    WriteSer(p, Spec(0, 0, 4, 8, 1), Frames(1, 16));
    CHECK_THROWS_AS(SerFile::Open(p), SerError);
}

TEST_CASE("rejects zero and negative dimensions", "[ser]") {
    struct { int w, h; } cases[] = {{4, 0}, {4, -4}, {-4, 4}};
    for (auto c : cases) {
        auto p = TempPath();
        WriteSer(p, Spec(0, c.w, c.h, 8, 1), Frames(1, 16));
        CHECK_THROWS_AS(SerFile::Open(p), SerError);
    }
}

TEST_CASE("rejects 100000x100000", "[ser]") {
    auto p = TempPath();
    WriteSer(p, Spec(0, 100000, 100000, 8, 1), Frames(1, 16));
    CHECK_THROWS_AS(SerFile::Open(p), SerError);
}

TEST_CASE("rejects oversized dimensions", "[ser]") {
    auto p = TempPath();
    WriteSer(p, Spec(0, 65537, 4, 8, 1), Frames(1, 16));
    CHECK_THROWS_AS(SerFile::Open(p), SerError);
    auto q = TempPath();
    WriteSer(q, Spec(0, 4, 65537, 8, 1), Frames(1, 16));
    CHECK_THROWS_AS(SerFile::Open(q), SerError);
}

TEST_CASE("recovers frame count 0", "[ser]") {
    auto p = TempPath();
    WriteSer(p, Spec(0, 8, 8, 8, 0), Frames(5, 64));
    auto i = SerFile::Open(p).Info();
    CHECK(i.frameCount == 5);
    REQUIRE(i.warnings.size() == 1);
    CHECK(i.warnings[0].find("frame count") != std::string::npos);
    CHECK_FALSE(i.hasTimestamps);
}

TEST_CASE("truncated file uses complete frames", "[ser]") {
    auto p = TempPath();
    auto frames = Frames(5, 64);
    frames[4].resize(32);  // 4.5 frames of data
    WriteSer(p, Spec(0, 8, 8, 8, 10), frames);
    auto i = SerFile::Open(p).Info();
    CHECK(i.frameCount == 4);
    CHECK(i.warnings.size() == 1);
    CHECK_FALSE(i.hasTimestamps);
}

TEST_CASE("zero complete frames is an error", "[ser]") {
    auto p = TempPath();
    auto frames = Frames(1, 20);  // less than one 64-byte frame
    WriteSer(p, Spec(0, 8, 8, 8, 3), frames);
    CHECK_THROWS_AS(SerFile::Open(p), SerError);
}

TEST_CASE("detects timestamp trailer", "[ser]") {
    auto p = TempPath();
    WriteSer(p, Spec(0, 8, 8, 8, 3), Frames(3, 64), {111, 222, 333});
    auto f = SerFile::Open(p);
    CHECK(f.Info().hasTimestamps);
    CHECK(f.Info().warnings.empty());
    CHECK(f.FrameTimestamp(1) == 222);
    CHECK_THROWS_AS(f.FrameTimestamp(3), SerError);
    CHECK_THROWS_AS(f.FrameTimestamp(-1), SerError);

    auto q = TempPath();
    WriteSer(q, Spec(0, 8, 8, 8, 3), Frames(3, 64));
    auto g = SerFile::Open(q);
    CHECK_FALSE(g.Info().hasTimestamps);
    CHECK(g.FrameTimestamp(0) == std::nullopt);
}

TEST_CASE("ticks to iso", "[ser]") {
    const int64_t ticks = DaysFromYear1(2024, 9, 7) * 864000000000LL +
                          (2 * 3600 + 4 * 60 + 21) * 10000000LL;
    CHECK(TicksToIsoUtc(ticks) == std::string("2024-09-07T02:04:21.000"));
    CHECK(TicksToIsoUtc(ticks + 1234567) == std::string("2024-09-07T02:04:21.123"));
    CHECK(TicksToIsoUtc(0) == std::nullopt);
    const int64_t flagged = ticks | (int64_t{1} << 62);
    CHECK(TicksToIsoUtc(flagged) == TicksToIsoUtc(ticks));
    const int64_t signFlag = static_cast<int64_t>(static_cast<uint64_t>(ticks) | (uint64_t{1} << 63));
    CHECK(TicksToIsoUtc(signFlag) == TicksToIsoUtc(ticks));
}

namespace {
// Builds ticks from a calendar date with the independent day counter above.
int64_t Ticks(int y, int m, int d, int hh, int mm, int ss, int ms) {
    return DaysFromYear1(y, m, d) * 864000000000LL +
           ((hh * 3600 + mm * 60 + ss) * 1000LL + ms) * 10000LL;
}
}  // namespace

TEST_CASE("ticks to iso across calendar edge cases", "[ser]") {
    CHECK(TicksToIsoUtc(Ticks(2024, 2, 29, 12, 0, 0, 0)) == std::string("2024-02-29T12:00:00.000"));
    CHECK(TicksToIsoUtc(Ticks(2000, 2, 29, 0, 0, 0, 1)) == std::string("2000-02-29T00:00:00.001"));
    CHECK(TicksToIsoUtc(Ticks(1900, 3, 1, 6, 30, 15, 500)) == std::string("1900-03-01T06:30:15.500"));
    CHECK(TicksToIsoUtc(Ticks(2100, 3, 1, 23, 0, 0, 0)) == std::string("2100-03-01T23:00:00.000"));
    CHECK(TicksToIsoUtc(Ticks(1969, 12, 31, 18, 45, 0, 250)) == std::string("1969-12-31T18:45:00.250"));
    const int64_t lastMs = Ticks(2023, 12, 31, 23, 59, 59, 999);
    CHECK(TicksToIsoUtc(lastMs) == std::string("2023-12-31T23:59:59.999"));
    CHECK(TicksToIsoUtc(lastMs + 10000) == std::string("2024-01-01T00:00:00.000"));
}

TEST_CASE("data after the declared frames: trailer only when it cannot be pixels", "[ser]") {
    // 3 declared 64-byte frames. File holds 5 frames' worth of bytes: the two extra
    // frames are not a trailer, and must not be read as ticks.
    auto p = TempPath();
    WriteSer(p, Spec(0, 8, 8, 8, 3), Frames(5, 64));
    auto f = SerFile::Open(p);
    CHECK(f.Info().frameCount == 3);
    CHECK_FALSE(f.Info().hasTimestamps);
    REQUIRE(f.Info().warnings.size() == 1);
    CHECK(f.Info().warnings[0].find("extra data") != std::string::npos);

    // Exactly one extra frame: leftover (64) - 8N (24) < frameBytes, yet it is pixels.
    auto q = TempPath();
    WriteSer(q, Spec(0, 8, 8, 8, 3), Frames(4, 64));
    auto g = SerFile::Open(q);
    CHECK_FALSE(g.Info().hasTimestamps);
    CHECK(g.Info().warnings.size() == 1);

    // A genuine trailer is still recognised and silent, even when 8N exceeds one frame.
    auto r = TempPath();
    std::vector<int64_t> ticks(10, 5);
    WriteSer(r, Spec(0, 4, 4, 8, 10), Frames(10, 16), ticks);  // trailer 80 B > frame 16 B
    auto h = SerFile::Open(r);
    CHECK(h.Info().hasTimestamps);
    CHECK(h.Info().warnings.empty());
    CHECK(h.FrameTimestamp(9) == 5);

    // Trailer followed by a few stray bytes (less than a frame): still a trailer.
    auto t = TempPath();
    WriteSer(t, Spec(0, 8, 8, 8, 3), Frames(3, 64), {1, 2, 3, 4});  // 32 B leftover
    auto k = SerFile::Open(t);
    CHECK(k.Info().hasTimestamps);
    CHECK(k.FrameTimestamp(2) == 3);
}
