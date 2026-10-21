#include <catch_amalgamated.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>

#include "AsiairSidecar.h"
#include "AviWriter.h"
#include "FrameSource.h"
#include "SerExport.h"
#include "SerFile.h"
#include "TestUtil.h"

using namespace planetary::ser;
using namespace planetary::testing;
namespace fs = std::filesystem;

namespace {

constexpr int kW = 9, kH = 7, kN = 4;

std::vector<std::vector<uint8_t>> Frames() {
    std::vector<std::vector<uint8_t>> f(kN, std::vector<uint8_t>(kW * kH));
    for (int i = 0; i < kN; ++i)
        for (int k = 0; k < kW * kH; ++k) f[i][k] = static_cast<uint8_t>(i * 50 + k);
    return f;
}

// Writes the named AVI fixture with a Vert/RG sidecar into `dir`.
std::string MakeAvi(const fs::path& dir, const std::string& name) {
    const std::string p = (dir / name).string();
    AviSpec s;
    s.width = kW;
    s.height = kH;
    WriteAvi(p, s, Frames());
    std::ofstream(p + ".txt")
        << "[ZWO ASI071MC Pro]\nColour Format = RAW8\nFlip = Vert(unimplemented)\nBayer = RG\n";
    return p;
}

std::string Slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

const ExportProgress kNone;

}  // namespace

TEST_CASE("FilenameStartTicks") {
    // 2023-05-26 23:00:53 local; independently: days 0001-01-01 -> 2023-05-26 = 738665 (ordinal 738666 - 1)
    CHECK(FilenameStartTicks("2023-05-26-230053-Moon-x.avi") ==
          (738665LL * 86400 + 23 * 3600 + 53) * 10000000LL);
    CHECK(FilenameStartTicks("capture.avi") == 0);
    CHECK(FilenameStartTicks("0001-01-01-000000-x") == 0);
}

TEST_CASE("ExportSer round trip from an AVI") {
    TempDir td;
    const fs::path dir = fs::path(td.NewPath(".d"));
    fs::create_directories(dir);
    const std::string avi = MakeAvi(dir, "2023-05-26-230053-Moon-x.avi");
    const auto src = OpenVideo(avi);
    REQUIRE(src->Info().colorId == ColorId::BayerBGGR);

    const std::string out = (dir / "out.ser").string();
    std::vector<int64_t> seen;
    const auto r = ExportSer(*src, avi, out, false, [&](int64_t d, int64_t n) {
        CHECK(n == kN);
        seen.push_back(d);
        return true;
    });
    CHECK(r.framesWritten == kN);
    CHECK(r.bytesWritten == 178 + kN * kW * kH);
    CHECK(fs::file_size(out) == static_cast<uintmax_t>(r.bytesWritten));
    CHECK_FALSE(fs::exists(out + ".partial"));
    REQUIRE(seen.size() == kN + 1);
    for (size_t i = 0; i < seen.size(); ++i) CHECK(seen[i] == static_cast<int64_t>(i));

    SerFile ser = SerFile::Open(out);
    const FileInfo& info = ser.Info();
    CHECK(info.colorId == ColorId::BayerBGGR);
    CHECK(info.header.width == kW);
    CHECK(info.header.height == kH);
    CHECK(info.header.depth == 8);
    CHECK(info.header.littleEndianField == 0);
    {
        const std::string raw = Slurp(out);  // FrameCount: raw bytes 38..41, little-endian
        CHECK(static_cast<uint8_t>(raw[38]) == kN);
        CHECK(raw[39] == 0);
        CHECK(raw[40] == 0);
        CHECK(raw[41] == 0);
    }
    CHECK(info.header.instrument == "ZWO ASI071MC Pro");
    CHECK(info.header.observer.empty());
    CHECK(info.header.telescope.empty());
    CHECK(info.header.dateTimeUtc == 0);
    CHECK(info.header.dateTimeLocal == FilenameStartTicks("2023-05-26-230053-Moon-x.avi"));
    CHECK_FALSE(info.hasTimestamps);
    REQUIRE(info.frameCount == kN);
    for (int i = 0; i < kN; ++i) {
        const Frame a = src->ReadFrame(i), b = ser.ReadFrame(i);
        CHECK(a.samples == b.samples);
    }
}

TEST_CASE("ExportSer truncates a long instrument name to 40 bytes") {
    TempDir td;
    const fs::path dir = fs::path(td.NewPath(".d"));
    fs::create_directories(dir);
    const std::string avi = (dir / "a.avi").string();
    AviSpec s;
    s.width = kW;
    s.height = kH;
    WriteAvi(avi, s, Frames());
    const std::string cam(50, 'C');
    std::ofstream(avi + ".txt") << "[" << cam << "]\nBayer = RG\n";
    const auto src = OpenVideo(avi);
    const std::string out = (dir / "o.ser").string();
    ExportSer(*src, avi, out, false, kNone);
    CHECK(SerFile::Open(out).Info().header.instrument == cam.substr(0, 40));
    CHECK(SerFile::Open(out).Info().header.telescope.empty());
    const std::string raw = Slurp(out);
    CHECK(raw[82 + 40] == 0);  // byte 41 of Instrument stays zero
    CHECK(raw[122] == 0);      // Telescope starts empty
}

TEST_CASE("ExportSer cancel leaves no files") {
    TempDir td;
    const fs::path dir = fs::path(td.NewPath(".d"));
    fs::create_directories(dir);
    const std::string avi = MakeAvi(dir, "a.avi");
    const auto src = OpenVideo(avi);
    const std::string out = (dir / "out.ser").string();
    CHECK_THROWS_AS(ExportSer(*src, avi, out, false,
                              [](int64_t d, int64_t) { return d < 2; }),
                    ExportCancelled);
    CHECK_FALSE(fs::exists(out));
    CHECK_FALSE(fs::exists(out + ".partial"));
}

TEST_CASE("ExportSer refuses the source file as target") {
    TempDir td;
    const fs::path dir = fs::path(td.NewPath(".d"));
    fs::create_directories(dir);
    const std::string avi = MakeAvi(dir, "a.avi");
    const std::string before = Slurp(avi);
    const auto src = OpenVideo(avi);
    for (const std::string& target : {avi, (dir / "sub" / ".." / "a.avi").string()}) {
        fs::create_directories(dir / "sub");
        for (bool overwrite : {false, true}) {
            try {
                ExportSer(*src, avi, target, overwrite, kNone);
                FAIL("expected SerError");
            } catch (const SerError& e) {
                CHECK(std::string(e.what()) == "refusing to overwrite the source file");
            }
        }
    }
    CHECK(Slurp(avi) == before);
    CHECK_FALSE(fs::exists(avi + ".partial"));
}

TEST_CASE("ExportSer existing target needs allowOverwrite") {
    TempDir td;
    const fs::path dir = fs::path(td.NewPath(".d"));
    fs::create_directories(dir);
    const std::string avi = MakeAvi(dir, "a.avi");
    const auto src = OpenVideo(avi);
    const std::string out = (dir / "out.ser").string();
    std::ofstream(out) << "old";
    try {
        ExportSer(*src, avi, out, false, kNone);
        FAIL("expected SerError");
    } catch (const SerError& e) {
        CHECK(std::string(e.what()) == out + " exists");
    }
    CHECK(Slurp(out) == "old");
    ExportSer(*src, avi, out, true, kNone);
    CHECK(SerFile::Open(out).Info().frameCount == kN);
}

TEST_CASE("ExportSer accepts 8-bit mono SER, refuses 16-bit and RGB") {
    TempDir td;
    const fs::path dir = fs::path(td.NewPath(".d"));
    fs::create_directories(dir);
    const std::string msg = "Save as SER converts 8-bit single-channel video only";
    const std::string out = (dir / "out.ser").string();

    const std::string s8 = (dir / "m8.ser").string();
    WriteSer(s8, Spec(0, kW, kH, 8, 2), {Frames()[0], Frames()[1]});
    {
        SerFile src = SerFile::Open(s8);
        CHECK(ExportSer(src, s8, out, false, kNone).framesWritten == 2);
    }

    const std::string s16 = (dir / "m16.ser").string();
    WriteSer(s16, Spec(0, kW, kH, 16, 1), {std::vector<uint8_t>(kW * kH * 2, 1)});
    const std::string rgb = (dir / "rgb.ser").string();
    WriteSer(rgb, Spec(100, kW, kH, 8, 1), {std::vector<uint8_t>(kW * kH * 3, 1)});
    for (const std::string& p : {s16, rgb}) {
        SerFile src = SerFile::Open(p);
        try {
            ExportSer(src, p, (dir / "x.ser").string(), false, kNone);
            FAIL("expected SerError");
        } catch (const SerError& e) {
            CHECK(std::string(e.what()) == msg);
        }
        CHECK_FALSE(fs::exists(dir / "x.ser"));
    }
}

TEST_CASE("ExportSer reports a write failure and leaves no partial") {
    TempDir td;
    const fs::path dir = fs::path(td.NewPath(".d"));
    fs::create_directories(dir);
    const std::string avi = MakeAvi(dir, "a.avi");
    const auto src = OpenVideo(avi);
    const std::string out = (dir / "missing-dir" / "out.ser").string();
    CHECK_THROWS_AS(ExportSer(*src, avi, out, false, kNone), SerError);
}

namespace {
// Reports a valid-looking Info but hands back a wrong-size frame at index 1.
class BadFrames : public FrameSource {
public:
    BadFrames() {
        info_.header.width = kW;
        info_.header.height = kH;
        info_.header.depth = 8;
        info_.frameCount = 3;
    }
    const FileInfo& Info() const override { return info_; }
    Frame ReadFrame(int64_t i) const override {
        Frame f;
        f.width = kW;
        f.height = kH;
        f.channels = 1;
        f.samples.assign(kW * kH - (i == 1 ? 1 : 0), 5);
        return f;
    }
    std::optional<int64_t> FrameTimestamp(int64_t) const override { return std::nullopt; }

private:
    FileInfo info_;
};
}  // namespace

TEST_CASE("ExportSer rejects a wrong-size frame and leaves no files") {
    TempDir td;
    const fs::path dir = fs::path(td.NewPath(".d"));
    fs::create_directories(dir);
    const std::string out = (dir / "out.ser").string();
    BadFrames src;
    try {
        ExportSer(src, (dir / "src.avi").string(), out, false, kNone);
        FAIL("expected SerError");
    } catch (const SerError& e) {
        CHECK(std::string(e.what()) == "frame 1 does not match the video size");
    }
    CHECK_FALSE(fs::exists(out));
    CHECK_FALSE(fs::exists(out + ".partial"));
}

TEST_CASE("ExportSer cancel on the final frame leaves no files") {
    TempDir td;
    const fs::path dir = fs::path(td.NewPath(".d"));
    fs::create_directories(dir);
    const std::string avi = MakeAvi(dir, "a.avi");
    const auto src = OpenVideo(avi);
    const std::string out = (dir / "out.ser").string();
    CHECK_THROWS_AS(ExportSer(*src, avi, out, false, [](int64_t d, int64_t n) { return d < n; }),
                    ExportCancelled);
    CHECK_FALSE(fs::exists(out));
    CHECK_FALSE(fs::exists(out + ".partial"));
}

TEST_CASE("ExportSer refuses when there is not enough free space") {
    TempDir td;
    const fs::path dir = fs::path(td.NewPath(".d"));
    fs::create_directories(dir);
    const std::string avi = MakeAvi(dir, "a.avi");
    const auto src = OpenVideo(avi);
    const std::string out = (dir / "out.ser").string();
    const int64_t need = 178 + kN * kW * kH;
    try {
        ExportSerWithSpace(*src, avi, out, false, kNone, [&](const std::string&, std::error_code&) {
            return static_cast<uint64_t>(need - 1);
        });
        FAIL("expected SerError");
    } catch (const SerError& e) {
        CHECK(std::string(e.what()) == "not enough free space: need " + std::to_string(need) +
                                           " bytes, " + std::to_string(need - 1) + " available");
    }
    CHECK_FALSE(fs::exists(out));
    CHECK_FALSE(fs::exists(out + ".partial"));
    ExportSerWithSpace(*src, avi, out, false, kNone, [&](const std::string&, std::error_code&) {
        return static_cast<uint64_t>(need);
    });
    CHECK(fs::exists(out));
}

TEST_CASE("ExportSer replaces a stale .partial") {
    TempDir td;
    const fs::path dir = fs::path(td.NewPath(".d"));
    fs::create_directories(dir);
    const std::string avi = MakeAvi(dir, "a.avi");
    const auto src = OpenVideo(avi);
    const std::string out = (dir / "out.ser").string();
    std::ofstream(out + ".partial") << std::string(5000, 'x');
    ExportSer(*src, avi, out, false, kNone);
    CHECK_FALSE(fs::exists(out + ".partial"));
    CHECK(fs::file_size(out) == static_cast<uintmax_t>(178 + kN * kW * kH));
}
