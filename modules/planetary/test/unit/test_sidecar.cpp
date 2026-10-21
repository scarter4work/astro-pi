#include <catch_amalgamated.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "AsiairSidecar.h"
#include "AviWriter.h"
#include "FrameSource.h"
#include "TestUtil.h"

using namespace planetary::ser;
using namespace planetary::testing;

namespace {

const char* kSample =
    "[ZWO ASI2400MC Pro]\n"
    "Bin = 1\n"
    "Capture Area Size = 1280 * 960\n"
    "Colour Format = RAW8\n"
    "Exposure = 0.00045 Sec\n"
    "Flip = Vert(unimplemented)\n"
    "Gain = 273\n"
    "Temperature = -7.6 C\n"
    "Bayer = RG\n"
    "White Balance (B) = 95\n"
    "White Balance (R) = 52\n"
    "Duration=9 Sec\n";

const std::string kArrowBGGR = "sidecar RG, Flip=Vert \xE2\x86\x92 BGGR";

AsiairSidecar Sc(const std::string& bayer, const std::string& flip,
                 const std::string& colour = "RAW8") {
    AsiairSidecar s;
    s.camera = "ZWO Test";
    s.bayer = bayer;
    s.flip = flip;
    s.colourFormat = colour;
    return s;
}

bool Has(const std::vector<std::string>& w, const std::string& t) {
    return std::find(w.begin(), w.end(), t) != w.end();
}

const std::string kNoBayer =
    "no Bayer pattern known (no .avi.txt sidecar); showing the raw CFA as mono -- choose the "
    "pattern under Bayer pattern";

struct Resolved {
    ColorId id;
    std::string source;
    std::vector<std::string> warnings;
};
Resolved Resolve(BayerChoice c, const std::optional<AsiairSidecar>& sc) {
    Resolved r;
    r.id = ResolveBayer(c, sc, r.source, r.warnings);
    return r;
}

void WriteText(const std::string& path, const std::string& text) {
    std::ofstream(path, std::ios::binary) << text;
}

AviSpec Avi(int w, int h) {
    AviSpec s;
    s.width = w;
    s.height = h;
    return s;
}

std::vector<std::vector<uint8_t>> Frames(int w, int h, int n) {
    return std::vector<std::vector<uint8_t>>(n, std::vector<uint8_t>(size_t(w) * h, 7));
}

std::string FileName(const std::string& p) { return std::filesystem::path(p).filename().string(); }

}  // namespace

const std::string kTail = "; showing the raw CFA as mono -- choose the pattern under Bayer pattern";

TEST_CASE("every no-pattern warning ends with the published viewing tail") {
    CHECK(kTail == kRawCfaAsMonoTail);
    CHECK(kNoBayer == "no Bayer pattern known (no .avi.txt sidecar)" + kTail);
}

TEST_CASE("ResolveBayer: a sidecar without a Bayer value says so, not 'no sidecar'") {
    Resolved r;
    r.id = ResolveBayer(BayerChoice::Auto, Sc("", "None"), r.source, r.warnings, "x.avi.txt");
    CHECK(r.id == ColorId::Mono);
    CHECK(r.source == "none");
    REQUIRE(r.warnings.size() == 1);
    CHECK(r.warnings[0] == "sidecar x.avi.txt has no Bayer value" + kTail);
}

TEST_CASE("OpenVideo: a sidecar without a Bayer line warns with its name") {
    TempDir dir;
    const std::string p = dir.NewPath(".avi");
    WriteAvi(p, Avi(8, 6), Frames(8, 6, 1));
    WriteText(p + ".txt", "[ZWO X]\nCapture Area Size = 8 * 6\nFlip = None\nGain = 90\n");
    const auto src = OpenVideo(p);
    const FileInfo& info = src->Info();
    CHECK(info.colorId == ColorId::Mono);
    CHECK(info.avi->sidecar.has_value());
    CHECK(info.avi->bayerSource == "none");
    CHECK(Has(info.warnings, "sidecar " + FileName(p + ".txt") + " has no Bayer value" + kTail));
    CHECK_FALSE(Has(info.warnings, kNoBayer));
}

TEST_CASE("ParseAsiairSidecar reads the real sample") {
    std::vector<std::string> w;
    const auto sc = ParseAsiairSidecar(kSample, w, "a.avi.txt");
    REQUIRE(sc.has_value());
    CHECK(sc->camera == "ZWO ASI2400MC Pro");
    CHECK(sc->colourFormat == "RAW8");
    CHECK(sc->bayer == "RG");
    CHECK(sc->flip == "Vert(unimplemented)");
    CHECK(sc->gain == "273");
    CHECK(sc->exposure == "0.00045 Sec");
    CHECK(sc->captureArea == "1280 * 960");
    CHECK(w.empty());
}

TEST_CASE("ParseAsiairSidecar handles CRLF, no spaces, unknown keys") {
    std::vector<std::string> w;
    const auto sc = ParseAsiairSidecar("[Cam X]\r\nBayer=GB\r\nFoo = bar\r\nGain =  5  \r\n", w);
    REQUIRE(sc.has_value());
    CHECK(sc->camera == "Cam X");
    CHECK(sc->bayer == "GB");
    CHECK(sc->gain == "5");
}

TEST_CASE("ParseAsiairSidecar unparseable text") {
    std::vector<std::string> w;
    CHECK_FALSE(ParseAsiairSidecar("hello world\nnothing here\n", w, "x.avi.txt").has_value());
    CHECK(Has(w, "sidecar x.avi.txt could not be read; ignored"));
}

TEST_CASE("ResolveBayer: Flip=Vert is an inference and says so (Ruling 10)") {
    auto r = Resolve(BayerChoice::Auto, Sc("RG", "Vert(unimplemented)"));
    CHECK(r.id == ColorId::BayerBGGR);
    CHECK(r.source == kArrowBGGR);
    REQUIRE(r.warnings.size() == 1);
    CHECK(r.warnings[0] ==
          "Bayer pattern inferred from Flip=Vert (BGGR); some ASIAIR captures are RGGB -- if red and blue "
          "look swapped, choose RGGB under Bayer pattern");

    // The inferred and the un-rotated sidecar pattern are substituted, not hardcoded.
    r = Resolve(BayerChoice::Auto, Sc("GR", "Vert"));
    REQUIRE(r.warnings.size() == 1);
    CHECK(r.warnings[0] ==
          "Bayer pattern inferred from Flip=Vert (GBRG); some ASIAIR captures are GRBG -- if red and blue "
          "look swapped, choose GRBG under Bayer pattern");

    // No inference, no warning: Flip None, and an explicit choice on a Flip=Vert file.
    CHECK(Resolve(BayerChoice::Auto, Sc("RG", "None")).warnings.empty());
    CHECK(Resolve(BayerChoice::RGGB, Sc("RG", "Vert(unimplemented)")).warnings.empty());
    CHECK(Resolve(BayerChoice::BGGR, Sc("RG", "Vert(unimplemented)")).warnings.empty());
}

TEST_CASE("ResolveBayer table") {
    auto r = Resolve(BayerChoice::Auto, Sc("RG", "None"));
    CHECK(r.id == ColorId::BayerRGGB);
    CHECK(r.source == "sidecar");
    CHECK(r.warnings.empty());

    r = Resolve(BayerChoice::Auto, Sc("RG", "Vert(unimplemented)"));
    CHECK(r.id == ColorId::BayerBGGR);
    CHECK(r.source == kArrowBGGR);

    r = Resolve(BayerChoice::Auto, Sc("GR", "Vert"));
    CHECK(r.id == ColorId::BayerGBRG);
    CHECK(r.source == "sidecar GR, Flip=Vert \xE2\x86\x92 GBRG");
    CHECK(Resolve(BayerChoice::Auto, Sc("GB", "Vert")).id == ColorId::BayerGRBG);
    CHECK(Resolve(BayerChoice::Auto, Sc("BG", "Vert")).id == ColorId::BayerRGGB);
    CHECK(Resolve(BayerChoice::Auto, Sc("BG", "")).id == ColorId::BayerBGGR);

    r = Resolve(BayerChoice::RGGB, Sc("BG", "None"));
    CHECK(r.id == ColorId::BayerRGGB);
    CHECK(r.source == "chosen");

    r = Resolve(BayerChoice::None, Sc("RG", "None"));
    CHECK(r.id == ColorId::Mono);
    CHECK(r.source == "chosen");
}

TEST_CASE("ResolveBayer without a sidecar or Bayer key") {
    auto r = Resolve(BayerChoice::Auto, std::nullopt);
    CHECK(r.id == ColorId::Mono);
    CHECK(r.source == "none");
    CHECK(Has(r.warnings, kNoBayer));

    // Without the sidecar's name (the default argument) the text still says a sidecar exists.
    r = Resolve(BayerChoice::Auto, Sc("", "None"));
    CHECK(r.id == ColorId::Mono);
    CHECK(r.source == "none");
    CHECK(Has(r.warnings, "the sidecar has no Bayer value" + kTail));
    CHECK_FALSE(Has(r.warnings, kNoBayer));

    r = Resolve(BayerChoice::BGGR, std::nullopt);
    CHECK(r.id == ColorId::BayerBGGR);
    CHECK(r.warnings.empty());
}

TEST_CASE("ResolveBayer warns on a non-RAW8 colour format") {
    const auto r = Resolve(BayerChoice::Auto, Sc("RG", "None", "RAW16"));
    CHECK(r.id == ColorId::BayerRGGB);
    CHECK(Has(r.warnings, "sidecar Colour Format is RAW16, not RAW8; using its Bayer value anyway"));
}

TEST_CASE("FilenameStart") {
    CHECK(FilenameStart("2023-05-26-230053-Moon-Bin1 18.3F.avi") == "2023-05-26 23:00:53");
    CHECK_FALSE(FilenameStart("capture.avi").has_value());
    CHECK_FALSE(FilenameStart("2023-05-26-2300-Moon.avi").has_value());
    CHECK_FALSE(FilenameStart("2023-13-26-230053-x.avi").has_value());
}

TEST_CASE("OpenVideo reads the sidecar next to an AVI") {
    TempDir dir;
    const std::string p = dir.NewPath(".avi");
    WriteAvi(p, Avi(8, 6), Frames(8, 6, 2));
    WriteText(p + ".txt",
              "[ZWO ASI071MC Pro]\nCapture Area Size = 8 * 6\nColour Format = RAW8\n"
              "Flip = Vert(unimplemented)\nBayer = RG\nGain = 90\n");

    const auto src = OpenVideo(p);
    const FileInfo& info = src->Info();
    CHECK(info.colorId == ColorId::BayerBGGR);
    CHECK(info.header.instrument == "ZWO ASI071MC Pro");
    REQUIRE(info.avi.has_value());
    CHECK(info.avi->bayerSource == kArrowBGGR);
    REQUIRE(info.avi->sidecar.has_value());
    CHECK(info.avi->sidecar->gain == "90");
    CHECK_FALSE(info.avi->filenameStart.has_value());
    // The only warning: the Flip=Vert pattern is an inference (Ruling 10).
    REQUIRE(info.warnings.size() == 1);
    CHECK(info.warnings[0].rfind("Bayer pattern inferred from Flip=Vert (BGGR); some ASIAIR captures are RGGB", 0) == 0);

    const auto chosen = OpenVideo(p, BayerChoice::GRBG);
    CHECK(chosen->Info().colorId == ColorId::BayerGRBG);
    CHECK(chosen->Info().avi->bayerSource == "chosen");
    CHECK(chosen->Info().warnings.empty());
}

TEST_CASE("OpenVideo without a sidecar and with a filename start time") {
    TempDir dir;
    const std::string dummy = dir.NewPath(".avi");
    const std::string named =
        std::filesystem::path(dummy).parent_path().string() + "/2023-05-26-230053-Moon.avi";
    WriteAvi(named, Avi(8, 6), Frames(8, 6, 1));

    const auto src = OpenVideo(named);
    const FileInfo& info = src->Info();
    CHECK(info.colorId == ColorId::Mono);
    CHECK(info.avi->bayerSource == "none");
    CHECK(info.avi->filenameStart == "2023-05-26 23:00:53");
    CHECK(Has(info.warnings, kNoBayer));
}

TEST_CASE("OpenVideo warns when the sidecar ROI differs from the video") {
    TempDir dir;
    const std::string p = dir.NewPath(".avi");
    WriteAvi(p, Avi(8, 6), Frames(8, 6, 1));
    WriteText(p + ".txt", "[ZWO X]\nCapture Area Size = 1280 * 960\nBayer = RG\nFlip = None\n");
    const auto src = OpenVideo(p);
    CHECK(Has(src->Info().warnings,
              "sidecar Capture Area Size 1280 * 960 differs from the video's 8 x 6"));
    CHECK(src->Info().colorId == ColorId::BayerRGGB);
}

TEST_CASE("OpenVideo ignores an unreadable or oversized sidecar") {
    TempDir dir;
    const std::string p = dir.NewPath(".avi");
    WriteAvi(p, Avi(8, 6), Frames(8, 6, 1));
    const std::string name = FileName(p + ".txt");

    WriteText(p + ".txt", "garbage only\n");
    auto src = OpenVideo(p);
    CHECK(Has(src->Info().warnings, "sidecar " + name + " could not be read; ignored"));
    CHECK_FALSE(src->Info().avi->sidecar.has_value());

    WriteText(p + ".txt", std::string(65 * 1024, 'x'));
    src = OpenVideo(p);
    CHECK_FALSE(src->Info().avi->sidecar.has_value());
    CHECK(Has(src->Info().warnings, "sidecar " + name + " is larger than 64 KiB; ignored"));
}

TEST_CASE("ResolveBayer with an unrecognised Bayer value warns once, specifically") {
    const auto r = Resolve(BayerChoice::Auto, Sc("XY", "None"));
    CHECK(r.id == ColorId::Mono);
    CHECK(r.source == "none");
    REQUIRE(r.warnings.size() == 1);
    CHECK(r.warnings[0] == "sidecar Bayer value XY is not recognised" + kTail);
}

TEST_CASE("ParseAsiairSidecar strips a UTF-8 BOM") {
    std::vector<std::string> w;
    const auto sc = ParseAsiairSidecar("\xEF\xBB\xBF[ZWO BOM]\nBayer = RG\n", w, "x.avi.txt");
    REQUIRE(sc.has_value());
    CHECK(sc->camera == "ZWO BOM");
    CHECK(sc->bayer == "RG");
    CHECK(w.empty());
}

TEST_CASE("OpenVideo keeps the instrument when the sidecar has no camera") {
    TempDir dir;
    const std::string p = dir.NewPath(".avi");
    WriteAvi(p, Avi(8, 6), Frames(8, 6, 1));
    WriteText(p + ".txt", "Bayer = RG\nFlip = None\n");
    const auto src = OpenVideo(p);
    CHECK(src->Info().avi->sidecar.has_value());
    CHECK(src->Info().header.instrument.empty());
    CHECK(src->Info().colorId == ColorId::BayerRGGB);
}
