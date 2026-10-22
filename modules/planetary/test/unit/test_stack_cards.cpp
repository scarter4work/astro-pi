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
