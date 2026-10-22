#include <catch_amalgamated.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "FitsText.h"
#include "PlanetaryVersion.h"
#include "StackCards.h"

using namespace planetary;
using stackcards::Card;
using stackcards::StackTimes;

namespace {

// A stacked outcome as RunStack returns it, for a SER or an AVI.
stack::StackOutcome Stacked(ser::Container container) {
    stack::StackOutcome o;
    o.info.container = container;
    if (container != ser::Container::Ser) {
        o.info.avi = ser::AviDetails();
        o.info.avi->bayerSource = "sidecar";
        o.info.colorId = ser::ColorId::BayerRGGB;
    }
    o.info.header.instrument = "ZWO ASI585MC";
    o.image = ser::FloatImage{4, 4, 3, std::vector<float>(48, 0.5f)};
    o.summary.measured = 40;
    o.summary.used = 18;
    o.summary.selection = "used 18 of 40 frames: the frames judged sharpest";
    o.summary.alignment = "aligned 18 of 18 selected frames to the reference";
    o.summary.warnings = {"image may be out of focus: detail 12.3 px"};
    o.frames.stacked = {3, 7, 39};
    return o;
}

const Card* Find(const std::vector<Card>& cards, const std::string& name) {
    const auto it = std::find_if(cards.begin(), cards.end(), [&](const Card& c) { return c.name == name; });
    return it == cards.end() ? nullptr : &*it;
}

StackTimes Times() {
    StackTimes t;
    t.start = "2026-09-07T02:10:03.000";
    t.end = "2026-09-07T02:10:39.000";
    t.fromFrames = true;
    return t;
}

}  // namespace

TEST_CASE("an AVI's stack never carries DATE-OBS or DATE-END, whatever times it is given", "[stackcards]") {
    for (ser::Container c : {ser::Container::Avi, ser::Container::AviOpenDml}) {
        const auto cards = stackcards::StackCards("Jupiter.avi", Stacked(c), Times());
        CHECK(Find(cards, "DATE-OBS") == nullptr);
        CHECK(Find(cards, "DATE-END") == nullptr);
        REQUIRE(Find(cards, "INSTRUME") != nullptr);
        CHECK(Find(cards, "INSTRUME")->comment == "ASIAIR sidecar camera");
    }
}

TEST_CASE("a SER's stack carries its frame times, or the header's start alone", "[stackcards]") {
    const auto cards = stackcards::StackCards("Jupiter.ser", Stacked(ser::Container::Ser), Times());
    REQUIRE(Find(cards, "DATE-OBS") != nullptr);
    CHECK(Find(cards, "DATE-OBS")->value == "2026-09-07T02:10:03.000");
    CHECK(Find(cards, "DATE-OBS")->text);
    REQUIRE(Find(cards, "DATE-END") != nullptr);
    CHECK(Find(cards, "DATE-END")->value == "2026-09-07T02:10:39.000");

    StackTimes header;
    header.start = "2026-09-07T02:04:21.500";
    const auto fallback = stackcards::StackCards("Jupiter.ser", Stacked(ser::Container::Ser), header);
    REQUIRE(Find(fallback, "DATE-OBS") != nullptr);
    CHECK(Find(fallback, "DATE-OBS")->comment == "UTC start of the capture (SER header)");
    CHECK(Find(fallback, "DATE-END") == nullptr);
    CHECK(Find(stackcards::StackCards("Jupiter.ser", Stacked(ser::Container::Ser), StackTimes()), "DATE-OBS") == nullptr);
}

TEST_CASE("SRCVIDEO fits its card, extension kept; HISTORY holds the full name", "[stackcards]") {
    const std::string name = "2026-01-30-195830-Jupiter-ZWO ASI585MC-Gain=252-Exposure=5ms-Bin1-Lqef-long-name.avi";
    const stack::StackOutcome o = Stacked(ser::Container::Avi);
    const auto cards = stackcards::StackCards(name, o, StackTimes());
    const Card* src = Find(cards, "SRCVIDEO");
    REQUIRE(src != nullptr);
    CHECK(fits::FitsStringLength(src->value) <= fits::kMaxStringValue);
    CHECK(src->value.size() > 60);
    CHECK(src->value.compare(src->value.size() - 7, 7, "....avi") == 0);
    CHECK(src->comment.find("HISTORY") != std::string::npos);

    const auto history = stackcards::StackHistory(name, o);
    REQUIRE_FALSE(history.empty());
    CHECK(history.front() == "PlanetaryStack " PLANETARY_VERSION_STRING ": stacked " + name);

    // A short name is written whole.
    CHECK(Find(stackcards::StackCards("Mars.ser", Stacked(ser::Container::Ser), StackTimes()), "SRCVIDEO")->value == "Mars.ser");
}

TEST_CASE("counts, history and warnings come from the summary", "[stackcards]") {
    const stack::StackOutcome o = Stacked(ser::Container::Avi);
    const auto cards = stackcards::StackCards("J.avi", o, StackTimes());
    CHECK(Find(cards, "NFRAMES")->value == "40");
    CHECK_FALSE(Find(cards, "NFRAMES")->text);
    CHECK(Find(cards, "NCOMBINE")->value == "18");
    CHECK(Find(cards, "BAYERPAT") == nullptr);
    const auto h = stackcards::StackHistory("J.avi", o);
    CHECK(std::find(h.begin(), h.end(), "PlanetaryStack: Bayer RGGB (sidecar)") != h.end());
    CHECK(std::find(h.begin(), h.end(), "PlanetaryStack: " + o.summary.selection) != h.end());
    CHECK(std::find(h.begin(), h.end(), "PlanetaryStack: " + o.summary.alignment) != h.end());
    CHECK(std::find(h.begin(), h.end(), "PlanetaryStack warning: image may be out of focus: detail 12.3 px") != h.end());
}

TEST_CASE("a diagnosis is written with its numbers", "[stackcards]") {
    stack::Diagnosis d;
    d.message = "No target: the frames hold only background (a target in 0 of 40 frames).";
    d.numbers = {{"frames", 40}, {"frames with a target", 0}, {"median clipped fraction", 0.85123}};
    CHECK(stackcards::DiagnosisText(d) == d.message + " (frames 40, frames with a target 0, median clipped fraction 0.8512)");
}

TEST_CASE("the window's info line: file, frames, size, depth and colour; an AVI's Bayer source", "[stackcards][window]") {
    ser::FileInfo ser;
    ser.container = ser::Container::Ser;
    ser.header.width = 128;
    ser.header.height = 112;
    ser.header.depth = 16;
    ser.frameCount = 60;
    ser.colorId = ser::ColorId::Mono;
    CHECK(stackcards::StackInfoLine("Saturn.ser", ser) == "Saturn.ser: 60 frames, 128 x 112, 16-bit, mono");
    ser.colorId = ser::ColorId::BayerGRBG;
    CHECK(stackcards::StackInfoLine("Saturn.ser", ser) == "Saturn.ser: 60 frames, 128 x 112, 16-bit, Bayer GRBG");
    ser.colorId = ser::ColorId::BGR;
    CHECK(stackcards::StackInfoLine("Saturn.ser", ser) == "Saturn.ser: 60 frames, 128 x 112, 16-bit, RGB");

    const stack::StackOutcome avi = Stacked(ser::Container::Avi);
    ser::FileInfo a = avi.info;
    a.header.width = 1024;
    a.header.height = 768;
    a.header.depth = 8;
    a.frameCount = 1500;
    CHECK(stackcards::StackInfoLine("Jupiter.avi", a) == "Jupiter.avi: 1500 frames, 1024 x 768, 8-bit, Bayer RGGB (sidecar)");
}

TEST_CASE("the window's result: counts and time, each warning, the selection and alignment lines", "[stackcards][window]") {
    stack::StackOutcome o = Stacked(ser::Container::Ser);
    o.summary.seconds = 5.5;
    CHECK(stackcards::ResultText(o) ==
          "Stacked: 18 of 40 frames used, in 5.50 s.\n"
          "Warning: image may be out of focus: detail 12.3 px\n"
          "Frames: used 18 of 40 frames: the frames judged sharpest\n"
          "Alignment: aligned 18 of 18 selected frames to the reference");
    // An AVI's reader warnings are reworded for the window's own controls.
    o = Stacked(ser::Container::Avi);
    o.summary.warnings = {"no Bayer pattern known (no .avi.txt sidecar); showing the raw CFA as mono -- choose the pattern "
                          "under Bayer pattern"};
    CHECK(stackcards::ResultText(o).find("Warning: no Bayer pattern known (no .avi.txt sidecar); the raw CFA is stacked as "
                                         "mono -- choose the pattern under Bayer pattern (Save pattern... remembers it)") !=
          std::string::npos);
}

TEST_CASE("the window's diagnosis: no image, the message, and each number on its own line", "[stackcards][window]") {
    stack::StackOutcome o;
    o.info.container = ser::Container::Ser;
    o.diagnosis.kind = stack::DiagnosisKind::NoTarget;
    o.diagnosis.message = "No target: the frames hold only background (a target in 0 of 40 frames).";
    o.diagnosis.numbers = {{"frames", 40}, {"frames with a target", 0}, {"limb blur (pixels)", 9.8564}};
    o.summary.measured = 40;
    o.summary.seconds = 0.004;
    o.summary.warnings = {"a reader warning"};
    CHECK(stackcards::ResultText(o) ==
          "No image: No target: the frames hold only background (a target in 0 of 40 frames).\n"
          "The numbers behind it:\n"
          "  frames: 40\n"
          "  frames with a target: 0\n"
          "  limb blur (pixels): 9.856\n"
          "40 frames measured in 0.00 s.\n"
          "Warning: a reader warning");
}

TEST_CASE("the progress text names the step and how far it is", "[stackcards][window]") {
    CHECK(stackcards::ProgressText(stack::kPhaseMeasuring, 0, 1564) == "Step 1 of 3: Measuring frames  0%");
    CHECK(stackcards::ProgressText(stack::kPhaseAligning, 733, 1464) == "Step 2 of 3: Aligning  50%");
    CHECK(stackcards::ProgressText(stack::kPhaseStacking, 1400, 1400) == "Step 3 of 3: Stacking  100%");
    CHECK(stackcards::ProgressText("Something else", 1, 0) == "Something else");
    CHECK(stackcards::ProgressFraction(733, 1464) == Catch::Approx(733.0 / 1464));
    CHECK(stackcards::ProgressFraction(5, 0) == 0.0);
    CHECK(stackcards::ProgressFraction(9, 4) == 1.0);
}
