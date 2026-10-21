#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <system_error>

#include <sys/stat.h>

#include "AsiairSidecar.h"
#include "AviWriter.h"
#include "PartialFile.h"
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
    const auto sc = ParseAsiairSidecar(kSample);
    REQUIRE(sc.has_value());
    CHECK(sc->camera == "ZWO ASI2400MC Pro");
    CHECK(sc->colourFormat == "RAW8");
    CHECK(sc->bayer == "RG");
    CHECK(sc->flip == "Vert(unimplemented)");
    CHECK(sc->gain == "273");
    CHECK(sc->exposure == "0.00045 Sec");
    CHECK(sc->captureArea == "1280 * 960");
}

TEST_CASE("ParseAsiairSidecar handles CRLF, no spaces, unknown keys") {
    const auto sc = ParseAsiairSidecar("[Cam X]\r\nBayer=GB\r\nFoo = bar\r\nGain =  5  \r\n");
    REQUIRE(sc.has_value());
    CHECK(sc->camera == "Cam X");
    CHECK(sc->bayer == "GB");
    CHECK(sc->gain == "5");
}

TEST_CASE("ParseAsiairSidecar unparseable text") {
    CHECK_FALSE(ParseAsiairSidecar("hello world\nnothing here\n").has_value());
    CHECK_FALSE(ParseAsiairSidecar("").has_value());
}

TEST_CASE("ResolveBayer: an unreadable or oversized sidecar is never 'no sidecar'") {
    for (auto [status, why] : {std::pair{SidecarStatus::Unreadable, std::string("could not be read")},
                               std::pair{SidecarStatus::TooLarge,
                                         std::string("could not be read (larger than 64 KiB)")}}) {
        INFO(why);
        Resolved r;
        r.id = ResolveBayer(BayerChoice::Auto, std::nullopt, r.source, r.warnings, "x.avi.txt", status);
        CHECK(r.id == ColorId::Mono);
        CHECK(r.source == "none");
        CHECK(r.warnings == std::vector<std::string>{"sidecar x.avi.txt " + why + "; no Bayer pattern known" + kTail});

        // An explicit choice still says the sidecar was not read.
        r = Resolved();
        r.id = ResolveBayer(BayerChoice::RGGB, std::nullopt, r.source, r.warnings, "x.avi.txt", status);
        CHECK(r.id == ColorId::BayerRGGB);
        CHECK(r.source == "chosen");
        CHECK(r.warnings == std::vector<std::string>{"sidecar x.avi.txt " + why + "; ignored"});
    }
    // A parsed sidecar wins over the status argument.
    Resolved r;
    r.id = ResolveBayer(BayerChoice::Auto, Sc("RG", "None"), r.source, r.warnings, "x.avi.txt",
                        SidecarStatus::Unreadable);
    CHECK(r.id == ColorId::BayerRGGB);
    CHECK(r.warnings.empty());
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

TEST_CASE("ResolveBayer: a Flip other than None/Vert keeps the sidecar pattern and says it is unmeasured (Ruling 15)") {
    auto r = Resolve(BayerChoice::Auto, Sc("RG", "Horiz"));
    CHECK(r.id == ColorId::BayerRGGB);
    CHECK(r.source == "sidecar");
    CHECK(r.warnings == std::vector<std::string>{
                            "sidecar Flip=Horiz is unmeasured; using RGGB as the sidecar says -- if colours look "
                            "wrong, choose the pattern under Bayer pattern"});

    r = Resolve(BayerChoice::Auto, Sc("GB", "Both"));
    CHECK(r.id == ColorId::BayerGBRG);
    CHECK(r.source == "sidecar");
    CHECK(r.warnings == std::vector<std::string>{
                            "sidecar Flip=Both is unmeasured; using GBRG as the sidecar says -- if colours look "
                            "wrong, choose the pattern under Bayer pattern"});

    // Measured or absent Flip values, and an explicit choice, carry no such warning.
    CHECK(Resolve(BayerChoice::Auto, Sc("RG", "None")).warnings.empty());
    CHECK(Resolve(BayerChoice::Auto, Sc("RG", "")).warnings.empty());
    CHECK(Resolve(BayerChoice::GRBG, Sc("RG", "Horiz")).warnings.empty());
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
    CHECK(info.avi->sidecarStatus == SidecarStatus::Parsed);
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
    CHECK_FALSE(src->Info().avi->sidecar.has_value());
    CHECK(src->Info().avi->sidecarStatus == SidecarStatus::Unreadable);
    CHECK(src->Info().avi->bayerSource == "none");
    CHECK_FALSE(Has(src->Info().warnings, kNoBayer));  // a sidecar exists; "no .avi.txt sidecar" is false
    CHECK(src->Info().warnings ==
          std::vector<std::string>{"sidecar " + name + " could not be read; no Bayer pattern known" + kTail});
    src = OpenVideo(p, BayerChoice::GBRG);
    CHECK(src->Info().colorId == ColorId::BayerGBRG);
    CHECK(src->Info().warnings == std::vector<std::string>{"sidecar " + name + " could not be read; ignored"});

    WriteText(p + ".txt", std::string(65 * 1024, 'x'));
    src = OpenVideo(p);
    CHECK_FALSE(src->Info().avi->sidecar.has_value());
    CHECK(src->Info().avi->sidecarStatus == SidecarStatus::TooLarge);
    CHECK_FALSE(Has(src->Info().warnings, kNoBayer));
    CHECK(src->Info().warnings == std::vector<std::string>{"sidecar " + name +
                                                           " could not be read (larger than 64 KiB); no Bayer "
                                                           "pattern known" + kTail});

    // Present but not readable by this user (an I/O failure, not a parse failure).
    WriteText(p + ".txt", "[ZWO X]\nBayer = RG\n");
    std::filesystem::permissions(p + ".txt", std::filesystem::perms::none);
    if (!std::ifstream(p + ".txt")) {
        src = OpenVideo(p);
        CHECK(src->Info().avi->sidecarStatus == SidecarStatus::Unreadable);
        CHECK(src->Info().warnings ==
              std::vector<std::string>{"sidecar " + name + " could not be read; no Bayer pattern known" + kTail});
    }
    std::filesystem::permissions(p + ".txt", std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);

    std::filesystem::remove(p + ".txt");
    src = OpenVideo(p);
    CHECK(src->Info().avi->sidecarStatus == SidecarStatus::Absent);
    CHECK(src->Info().warnings == std::vector<std::string>{kNoBayer});
}

TEST_CASE("ResolveBayer with an unrecognised Bayer value warns once, specifically") {
    const auto r = Resolve(BayerChoice::Auto, Sc("XY", "None"));
    CHECK(r.id == ColorId::Mono);
    CHECK(r.source == "none");
    REQUIRE(r.warnings.size() == 1);
    CHECK(r.warnings[0] == "sidecar Bayer value XY is not recognised" + kTail);
}

TEST_CASE("ParseAsiairSidecar strips a UTF-8 BOM") {
    const auto sc = ParseAsiairSidecar("\xEF\xBB\xBF[ZWO BOM]\nBayer = RG\n");
    REQUIRE(sc.has_value());
    CHECK(sc->camera == "ZWO BOM");
    CHECK(sc->bayer == "RG");
}

TEST_CASE("OpenVideo leaves the instrument empty when the sidecar has no camera") {
    TempDir dir;
    const std::string p = dir.NewPath(".avi");
    WriteAvi(p, Avi(8, 6), Frames(8, 6, 1));
    WriteText(p + ".txt", "Bayer = RG\nFlip = None\n");
    const auto src = OpenVideo(p);
    CHECK(src->Info().avi->sidecar.has_value());
    CHECK(src->Info().header.instrument.empty());
    CHECK(src->Info().colorId == ColorId::BayerRGGB);
}

// ---- Increment 2.1: the sidecar SERViewer writes when the user saves a chosen pattern ----

namespace {

std::string ReadAll(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

const char* const kWritten = "Written By = SERViewer 0.2.1.0 (Bayer pattern chosen by the user)\n";

}  // namespace

TEST_CASE("WriteAsiairSidecar writes the exact text for each Bayer pattern") {
    TempDir dir;
    const struct {
        ColorId id;
        const char* value;
    } cases[] = {{ColorId::BayerRGGB, "RG"}, {ColorId::BayerGRBG, "GR"},
                 {ColorId::BayerGBRG, "GB"}, {ColorId::BayerBGGR, "BG"}};
    for (const auto& c : cases) {
        const std::string p = dir.NewPath(".avi");
        WriteAsiairSidecar(p, c.id, "0.2.1.0");
        CHECK(ReadAll(p + ".txt") == std::string("[SERViewer]\nBayer = ") + c.value + "\n" + kWritten);
        CHECK_FALSE(std::filesystem::exists(p + ".txt.partial"));
    }
}

TEST_CASE("WriteAsiairSidecar never overwrites a sidecar") {
    TempDir dir;
    const std::string p = dir.NewPath(".avi");
    WriteText(p + ".txt", "[ZWO X]\nBayer = GB\n");
    try {
        WriteAsiairSidecar(p, ColorId::BayerRGGB, "0.2.1.0");
        FAIL("an existing sidecar was not refused");
    } catch (const SerError& e) {
        CHECK(std::string(e.what()) ==
              FileName(p + ".txt") + " already exists; SERViewer never overwrites a sidecar");
    }
    CHECK(ReadAll(p + ".txt") == "[ZWO X]\nBayer = GB\n");
    CHECK_FALSE(std::filesystem::exists(p + ".txt.partial"));

    // Nor a symbolic link in its place, even one that leads nowhere (rename would replace it).
    const std::string q = dir.NewPath(".avi");
    std::filesystem::create_symlink(q + ".missing", q + ".txt");
    CHECK_THROWS_WITH(WriteAsiairSidecar(q, ColorId::BayerRGGB, "0.2.1.0"),
                      FileName(q + ".txt") + " already exists; SERViewer never overwrites a sidecar");
    CHECK(std::filesystem::is_symlink(q + ".txt"));
    CHECK_FALSE(std::filesystem::exists(q + ".missing"));
}

TEST_CASE("WriteAsiairSidecar refuses anything but the four Bayer patterns") {
    TempDir dir;
    for (ColorId id : {ColorId::Mono, ColorId::RGB, ColorId::BGR}) {
        const std::string p = dir.NewPath(".avi");
        CHECK_THROWS_AS(WriteAsiairSidecar(p, id, "0.2.1.0"), SerError);
        CHECK_FALSE(std::filesystem::exists(p + ".txt"));
        CHECK_FALSE(std::filesystem::exists(p + ".txt.partial"));
    }
}

TEST_CASE("WriteAsiairSidecar into a read-only folder names the path and the reason") {
    TempDir dir;
    const std::string sub = dir.NewPath("_ro");
    std::filesystem::create_directory(sub);
    const std::string p = sub + "/clip.avi";
    WriteAvi(p, Avi(8, 6), Frames(8, 6, 1));
    std::filesystem::permissions(sub, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
    const bool enforced = !std::ofstream(sub + "/probe");  // root ignores the mode
    if (enforced) {
        try {
            WriteAsiairSidecar(p, ColorId::BayerGRBG, "0.2.1.0");
            FAIL("writing into a read-only folder did not fail");
        } catch (const SerError& e) {
            const std::string m = e.what();
            CHECK(m.find(p + ".txt") != std::string::npos);
            CHECK(m.find("Permission denied") != std::string::npos);
        }
        CHECK_FALSE(std::filesystem::exists(p + ".txt"));
        CHECK_FALSE(std::filesystem::exists(p + ".txt.partial"));
    }
    std::filesystem::permissions(sub, std::filesystem::perms::owner_all);
}

TEST_CASE("WriteAsiairSidecar replaces a stale partial file and fails cleanly on a partial it cannot write") {
    TempDir dir;
    const std::string p = dir.NewPath(".avi");
    WriteText(p + ".txt.partial", "left over from an interrupted write\n");
    WriteAsiairSidecar(p, ColorId::BayerBGGR, "0.2.1.0");
    CHECK(ReadAll(p + ".txt") == std::string("[SERViewer]\nBayer = BG\n") + kWritten);
    CHECK_FALSE(std::filesystem::exists(p + ".txt.partial"));

    // A directory where the partial goes: it cannot be written; the error names it, no sidecar.
    const std::string q = dir.NewPath(".avi");
    std::filesystem::create_directory(q + ".txt.partial");
    try {
        WriteAsiairSidecar(q, ColorId::BayerRGGB, "0.2.1.0");
        FAIL("a directory in the partial's place did not fail");
    } catch (const SerError& e) {
        CHECK(std::string(e.what()).find(q + ".txt.partial") != std::string::npos);
    }
    CHECK_FALSE(std::filesystem::exists(q + ".txt"));
    CHECK(std::filesystem::is_directory(q + ".txt.partial"));  // not ours to remove
}

TEST_CASE("ParseAsiairSidecar: a [SERViewer] section is not a camera") {
    const auto sc = ParseAsiairSidecar(std::string("[SERViewer]\nBayer = GR\n") + kWritten);
    REQUIRE(sc.has_value());
    CHECK(sc->camera.empty());
    CHECK(sc->writtenBySERViewer);
    CHECK(sc->bayer == "GR");
    CHECK_FALSE(ParseAsiairSidecar("[ZWO X]\nBayer = GR\n")->writtenBySERViewer);
}

TEST_CASE("A written sidecar round-trips through OpenVideo(Auto)") {
    TempDir dir;
    for (ColorId id : {ColorId::BayerRGGB, ColorId::BayerGRBG, ColorId::BayerGBRG, ColorId::BayerBGGR}) {
        const std::string p = dir.NewPath(".avi");
        WriteAvi(p, Avi(8, 6), Frames(8, 6, 2));
        REQUIRE(OpenVideo(p)->Info().avi->sidecarStatus == SidecarStatus::Absent);
        WriteAsiairSidecar(p, id, "0.2.1.0");
        const auto src = OpenVideo(p);
        const FileInfo& info = src->Info();
        CHECK(info.colorId == id);
        CHECK(info.avi->sidecarStatus == SidecarStatus::Parsed);
        CHECK(info.avi->bayerSource == "sidecar (SERViewer)");
        CHECK(info.header.instrument.empty());
        CHECK(info.warnings.empty());  // no Flip line: no Flip warning, nothing else to say
        // An explicit choice still wins over it.
        CHECK(OpenVideo(p, BayerChoice::None)->Info().colorId == ColorId::Mono);
    }
}

TEST_CASE("WriteAsiairSidecar never writes through a symbolic link at the partial path") {
    TempDir dir;
    namespace fs = std::filesystem;
    // A victim file with known content and an old mtime, and a link to it where the partial goes.
    const std::string victim = dir.NewPath(".victim");
    WriteText(victim, "victim\n");
    const auto old = fs::file_time_type::clock::now() - std::chrono::hours(48);
    fs::last_write_time(victim, old);
    const std::string p = dir.NewPath(".avi");
    fs::create_symlink(victim, p + ".txt.partial");
    try {
        WriteAsiairSidecar(p, ColorId::BayerRGGB, "0.2.1.0");
        FAIL("a symbolic link at the partial path was written through or replaced silently");
    } catch (const SerError& e) {
        CHECK(std::string(e.what()) == "cannot write sidecar " + p + ".txt: cannot create " + p +
                                           ".txt.partial: it is a symbolic link; SERViewer never writes through one");
    }
    CHECK(ReadAll(victim) == "victim\n");
    CHECK(fs::last_write_time(victim) == old);
    CHECK_FALSE(fs::exists(fs::symlink_status(p + ".txt")));
    CHECK(fs::is_symlink(p + ".txt.partial"));  // reported, not removed

    // A dangling link must not create the file it names.
    const std::string q = dir.NewPath(".avi");
    const std::string nowhere = dir.NewPath(".nowhere");
    fs::create_symlink(nowhere, q + ".txt.partial");
    CHECK_THROWS_AS(WriteAsiairSidecar(q, ColorId::BayerRGGB, "0.2.1.0"), SerError);
    CHECK_FALSE(fs::exists(fs::symlink_status(nowhere)));
    CHECK_FALSE(fs::exists(fs::symlink_status(q + ".txt")));
}

TEST_CASE("WriteAsiairSidecar creates the file 0666 less the umask") {
    TempDir dir;
    namespace fs = std::filesystem;
    const mode_t old = ::umask(002);
    const std::string p = dir.NewPath(".avi");
    WriteAsiairSidecar(p, ColorId::BayerRGGB, "0.2.1.0");
    ::umask(old);
    CHECK((fs::status(p + ".txt").permissions() & fs::perms::mask) ==
          (fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read |
           fs::perms::group_write | fs::perms::others_read));
}

namespace {
int g_linkErrno = 0;
int FailingLink(const char*, const char*) {
    errno = g_linkErrno;
    return -1;
}
}  // namespace

TEST_CASE("PublishPartial: hard link where possible, re-check + rename only without hard links") {
    TempDir dir;
    namespace fs = std::filesystem;
    std::string why;
    auto partialWith = [&](const std::string& target, const std::string& text) {
        WriteText(target + ".partial", text);
        return target + ".partial";
    };

    // Hard link: published, the partial name gone.
    std::string t = dir.NewPath(".txt");
    CHECK(PublishPartial(partialWith(t, "new\n"), t, false, why) == PublishResult::Published);
    CHECK(ReadAll(t) == "new\n");
    CHECK_FALSE(fs::exists(t + ".partial"));

    // Hard link onto an existing target: refused atomically, target unchanged, partial kept for the caller.
    t = dir.NewPath(".txt");
    WriteText(t, "old\n");
    CHECK(PublishPartial(partialWith(t, "new\n"), t, false, why) == PublishResult::TargetExists);
    CHECK(ReadAll(t) == "old\n");
    CHECK(fs::exists(t + ".partial"));

    // No hard links on this filesystem (CIFS): re-check + rename.
    for (int err : {EPERM, EOPNOTSUPP, ENOSYS, EXDEV}) {
        g_linkErrno = err;
        t = dir.NewPath(".txt");
        CHECK(PublishPartial(partialWith(t, "new\n"), t, false, why, &FailingLink) == PublishResult::Published);
        CHECK(ReadAll(t) == "new\n");
        CHECK_FALSE(fs::exists(t + ".partial"));
        t = dir.NewPath(".txt");
        WriteText(t, "old\n");
        CHECK(PublishPartial(partialWith(t, "new\n"), t, false, why, &FailingLink) == PublishResult::TargetExists);
        CHECK(ReadAll(t) == "old\n");
    }

    // Any other link failure is a failure, with its reason.
    g_linkErrno = EIO;
    t = dir.NewPath(".txt");
    CHECK(PublishPartial(partialWith(t, "new\n"), t, false, why, &FailingLink) == PublishResult::Failed);
    CHECK(why.find(std::error_code(EIO, std::generic_category()).message()) != std::string::npos);
    CHECK_FALSE(fs::exists(t));

    // Replace (Save as SER after its dialog asked): rename over the target.
    t = dir.NewPath(".txt");
    WriteText(t, "old\n");
    CHECK(PublishPartial(partialWith(t, "new\n"), t, true, why) == PublishResult::Published);
    CHECK(ReadAll(t) == "new\n");
}

TEST_CASE("OpenVideo: anything but a file at the sidecar path is not a sidecar, and says so") {
    TempDir dir;
    namespace fs = std::filesystem;
    for (int kind = 0; kind < 3; ++kind) {
        const std::string p = dir.NewPath(".avi");
        WriteAvi(p, Avi(8, 6), Frames(8, 6, 1));
        if (kind == 0) fs::create_directory(p + ".txt");
        if (kind == 1) REQUIRE(::mkfifo((p + ".txt").c_str(), 0600) == 0);
        if (kind == 2) fs::create_symlink(p + ".nowhere", p + ".txt");
        const std::string name = FileName(p + ".txt");
        auto src = OpenVideo(p);
        CHECK(src->Info().avi->sidecarStatus == SidecarStatus::NotAFile);
        CHECK(src->Info().colorId == ColorId::Mono);
        CHECK(src->Info().warnings == std::vector<std::string>{"sidecar path " + name +
                                                               " is not a file; ignored; no Bayer pattern known" + kTail});
        src = OpenVideo(p, BayerChoice::GRBG);
        CHECK(src->Info().colorId == ColorId::BayerGRBG);
        CHECK(src->Info().warnings == std::vector<std::string>{"sidecar path " + name + " is not a file; ignored"});
    }
}

TEST_CASE("ParseAsiairSidecar: the [SERViewer] section matches in any case") {
    for (const char* s : {"[serviewer]\nBayer = GR\n", "[ SERVIEWER ]\nBayer = GR\n", "[SerViewer]\nBayer = GR\n"}) {
        const auto sc = ParseAsiairSidecar(s);
        REQUIRE(sc.has_value());
        CHECK(sc->writtenBySERViewer);
        CHECK(sc->camera.empty());
    }
}
