#include <catch_amalgamated.hpp>

#include <filesystem>
#include <fstream>

#include "Debayer.h"
#include "SerFile.h"
#include "SerWriter.h"
#include "TestUtil.h"

using namespace planetary::ser;
using namespace planetary::testing;
namespace fs = std::filesystem;

namespace {
SerSpec Spec(int color, int w, int h, int depth, int frames, int le = 0) {
    SerSpec s;
    s.colorId = color; s.width = w; s.height = h; s.depth = depth; s.frameCount = frames;
    s.littleEndianField = le;
    return s;
}

// 16-bit samples to bytes, little- or big-endian.
std::vector<uint8_t> Pack16(const std::vector<uint16_t>& v, bool bigEndian) {
    std::vector<uint8_t> b;
    for (uint16_t x : v) {
        const uint8_t lo = x & 0xFF, hi = x >> 8;
        if (bigEndian) { b.push_back(hi); b.push_back(lo); }
        else { b.push_back(lo); b.push_back(hi); }
    }
    return b;
}
}  // namespace

TEST_CASE("names and bayer predicates", "[ser][frames]") {
    CHECK(std::string(ColorIdName(ColorId::Mono)) == "MONO");
    CHECK(std::string(ColorIdName(ColorId::BayerRGGB)) == "BAYER_RGGB");
    CHECK(std::string(ColorIdName(ColorId::BayerGRBG)) == "BAYER_GRBG");
    CHECK(std::string(ColorIdName(ColorId::BayerGBRG)) == "BAYER_GBRG");
    CHECK(std::string(ColorIdName(ColorId::BayerBGGR)) == "BAYER_BGGR");
    CHECK(std::string(ColorIdName(ColorId::RGB)) == "RGB");
    CHECK(std::string(ColorIdName(ColorId::BGR)) == "BGR");
    CHECK(std::string(BayerPatternKeyword(ColorId::BayerRGGB)) == "RGGB");
    CHECK(std::string(BayerPatternKeyword(ColorId::BayerGRBG)) == "GRBG");
    CHECK(std::string(BayerPatternKeyword(ColorId::BayerGBRG)) == "GBRG");
    CHECK(std::string(BayerPatternKeyword(ColorId::BayerBGGR)) == "BGGR");
    CHECK(IsBayer(ColorId::BayerBGGR));
    CHECK_FALSE(IsBayer(ColorId::Mono));
    CHECK_FALSE(IsBayer(ColorId::RGB));
    CHECK_THROWS_AS(BayerPatternKeyword(ColorId::Mono), SerError);
}

TEST_CASE("reads mono 8-bit frame", "[ser][frames]") {
    const int w = 13, h = 7, n = 4;
    std::vector<std::vector<uint8_t>> frames;
    for (int k = 0; k < n; ++k) {
        std::vector<uint8_t> f(w * h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) f[y * w + x] = static_cast<uint8_t>((x + 2 * y + 7 * k) & 0xFF);
        frames.push_back(f);
    }
    auto p = TempPath();
    WriteSer(p, Spec(0, w, h, 8, n), frames);
    auto f = SerFile::Open(p);
    Frame fr = f.ReadFrame(2);
    CHECK(fr.width == w);
    CHECK(fr.height == h);
    CHECK(fr.channels == 1);
    REQUIRE(fr.samples.size() == static_cast<size_t>(w * h));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            REQUIRE(fr.samples[y * w + x] == ((x + 2 * y + 14) & 0xFF));
}

TEST_CASE("reads 16-bit little and big endian identically", "[ser][frames]") {
    const std::vector<uint16_t> v = {0, 1, 256, 4660, 40000, 65535, 513, 32768};
    auto pl = TempPath();
    WriteSer(pl, Spec(0, 4, 2, 16, 1, 0), {Pack16(v, false)});
    auto pb = TempPath();
    WriteSer(pb, Spec(0, 4, 2, 16, 1, 1), {Pack16(v, true)});
    auto fl = SerFile::Open(pl);
    auto fb = SerFile::Open(pb);
    CHECK(fl.Info().byteOrder == ByteOrder::Little);
    CHECK(fb.Info().byteOrder == ByteOrder::Big);
    CHECK(fl.ReadFrame(0).samples == v);
    CHECK(fb.ReadFrame(0).samples == v);
}

TEST_CASE("byte order plausibility overrides flag below 16 bits", "[ser][frames]") {
    const std::vector<uint16_t> v = {0, 1, 4095, 2048, 100, 3000, 7, 4094};
    auto p = TempPath();
    WriteSer(p, Spec(0, 4, 2, 12, 1, 1), {Pack16(v, false)});  // LE data, flag says BE
    auto f = SerFile::Open(p);
    CHECK(f.Info().byteOrder == ByteOrder::Little);
    REQUIRE(f.Info().warnings.size() == 1);
    CHECK(f.Info().warnings[0].find("byte order") != std::string::npos);
    CHECK(f.ReadFrame(0).samples == v);

    // Same in the other direction: BE data, flag says LE.
    auto q = TempPath();
    WriteSer(q, Spec(0, 4, 2, 12, 1, 0), {Pack16(v, true)});
    auto g = SerFile::Open(q);
    CHECK(g.Info().byteOrder == ByteOrder::Big);
    REQUIRE(g.Info().warnings.size() == 1);
    CHECK(g.ReadFrame(0).samples == v);
}

TEST_CASE("byte order flag is kept when plausible or when both orders are not", "[ser][frames]") {
    // Plausible under the flag: no warning.
    const std::vector<uint16_t> v = {1, 2, 3, 4, 5, 6, 7, 8};
    auto p = TempPath();
    WriteSer(p, Spec(0, 4, 2, 12, 1, 0), {Pack16(v, false)});
    CHECK(SerFile::Open(p).Info().warnings.empty());

    // Neither order fits in 12 bits: nothing to decide on, flag stands, no warning.
    const std::vector<uint16_t> bad = {0xFFFF, 0xF0F0, 0xABCD, 0xFFFF, 1, 2, 3, 4};
    auto q = TempPath();
    WriteSer(q, Spec(0, 4, 2, 12, 1, 1), {Pack16(bad, true)});
    auto g = SerFile::Open(q);
    CHECK(g.Info().byteOrder == ByteOrder::Big);
    CHECK(g.Info().warnings.empty());
}

TEST_CASE("no byte order override at depth 16", "[ser][frames]") {
    const std::vector<uint16_t> v = {0, 1, 4095, 2048, 100, 3000, 7, 4094};
    auto p = TempPath();
    WriteSer(p, Spec(0, 4, 2, 16, 1, 1), {Pack16(v, false)});
    auto f = SerFile::Open(p);
    CHECK(f.Info().byteOrder == ByteOrder::Big);
    CHECK(f.Info().warnings.empty());
}

TEST_CASE("rgb order kept, bgr reordered", "[ser][frames]") {
    auto p = TempPath();
    WriteSer(p, Spec(100, 1, 1, 8, 1), {{10, 20, 30}});
    auto q = TempPath();
    WriteSer(q, Spec(101, 1, 1, 8, 1), {{30, 20, 10}});
    Frame a = SerFile::Open(p).ReadFrame(0);
    Frame b = SerFile::Open(q).ReadFrame(0);
    CHECK(a.channels == 3);
    CHECK(a.samples == std::vector<uint16_t>{10, 20, 30});
    CHECK(b.samples == std::vector<uint16_t>{10, 20, 30});
}

TEST_CASE("rgb 16-bit big-endian decodes per sample", "[ser][frames]") {
    const std::vector<uint16_t> v = {1000, 2000, 3000, 4000, 5000, 6000};
    auto p = TempPath();
    WriteSer(p, Spec(100, 2, 1, 16, 1, 1), {Pack16(v, true)});
    CHECK(SerFile::Open(p).ReadFrame(0).samples == v);
}

TEST_CASE("frame index out of range throws", "[ser][frames]") {
    auto p = TempPath();
    WriteSer(p, Spec(0, 4, 4, 8, 2), std::vector<std::vector<uint8_t>>(2, std::vector<uint8_t>(16, 1)));
    auto f = SerFile::Open(p);
    CHECK_THROWS_AS(f.ReadFrame(-1), SerError);
    CHECK_THROWS_AS(f.ReadFrame(2), SerError);
    CHECK_NOTHROW(f.ReadFrame(1));
}

TEST_CASE("short read throws", "[ser][frames]") {
    auto p = TempPath();
    WriteSer(p, Spec(0, 4, 4, 8, 2), std::vector<std::vector<uint8_t>>(2, std::vector<uint8_t>(16, 1)));
    auto f = SerFile::Open(p);
    fs::resize_file(p, 178 + 16 + 5);  // file shrinks after Open
    CHECK_NOTHROW(f.ReadFrame(0));
    CHECK_THROWS_AS(f.ReadFrame(1), SerError);
}

TEST_CASE("frame offset uses 64-bit arithmetic", "[ser][frames][large]") {
    const int w = 4096, h = 4096, n = 90;
    const int64_t frameBytes = int64_t{w} * h * 3 * 2;
    const int64_t total = 178 + frameBytes * n;  // > 4 GiB, sparse on disk
    auto p = TempPath();
    WriteSer(p, Spec(100, w, h, 16, n), {});
    std::error_code ec;
    fs::resize_file(p, static_cast<uintmax_t>(total), ec);
    if (ec) { SKIP("filesystem refuses sparse files: " + ec.message()); }
    {
        std::fstream out(p, std::ios::in | std::ios::out | std::ios::binary);
        out.seekp(static_cast<std::streamoff>(178 + frameBytes * 89));
        const uint8_t px[6] = {0x34, 0x12, 0x78, 0x56, 0xBC, 0x9A};  // LE 0x1234 0x5678 0x9ABC
        out.write(reinterpret_cast<const char*>(px), 6);
        REQUIRE(out.good());
    }
    auto f = SerFile::Open(p);
    REQUIRE(f.Info().frameCount == n);
    Frame fr = f.ReadFrame(89);
    REQUIRE(fr.samples.size() == static_cast<size_t>(w) * h * 3);
    CHECK(fr.samples[0] == 0x1234);
    CHECK(fr.samples[1] == 0x5678);
    CHECK(fr.samples[2] == 0x9ABC);
    CHECK(fr.samples[3] == 0);
    CHECK(f.ReadFrame(88).samples[0] == 0);
}
