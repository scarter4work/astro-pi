#include <catch_amalgamated.hpp>

#include <algorithm>
#include <optional>
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
    o.summary.selection = "selected 18 of 40 frames: the frames judged sharpest";
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

// The target of a file whose name is not an ASIAIR capture name, target Auto.
stackcards::TargetChoice Unnamed() { return stackcards::ResolveTarget(std::nullopt, "Jupiter.avi"); }

}  // namespace

TEST_CASE("an AVI's stack never carries DATE-OBS or DATE-END, whatever times it is given", "[stackcards]") {
    for (ser::Container c : {ser::Container::Avi, ser::Container::AviOpenDml}) {
        const auto cards = stackcards::StackCards("Jupiter.avi", Stacked(c), Times(), Unnamed());
        CHECK(Find(cards, "DATE-OBS") == nullptr);
        CHECK(Find(cards, "DATE-END") == nullptr);
        REQUIRE(Find(cards, "INSTRUME") != nullptr);
        CHECK(Find(cards, "INSTRUME")->comment == "ASIAIR sidecar camera");
    }
}

TEST_CASE("a SER's stack carries its frame times, or the header's start alone", "[stackcards]") {
    const auto cards = stackcards::StackCards("Jupiter.ser", Stacked(ser::Container::Ser), Times(), Unnamed());
    REQUIRE(Find(cards, "DATE-OBS") != nullptr);
    CHECK(Find(cards, "DATE-OBS")->value == "2026-09-07T02:10:03.000");
    CHECK(Find(cards, "DATE-OBS")->text);
    REQUIRE(Find(cards, "DATE-END") != nullptr);
    CHECK(Find(cards, "DATE-END")->value == "2026-09-07T02:10:39.000");

    StackTimes header;
    header.start = "2026-09-07T02:04:21.500";
    const auto fallback = stackcards::StackCards("Jupiter.ser", Stacked(ser::Container::Ser), header, Unnamed());
    REQUIRE(Find(fallback, "DATE-OBS") != nullptr);
    CHECK(Find(fallback, "DATE-OBS")->comment == "UTC start of the capture (SER header)");
    CHECK(Find(fallback, "DATE-END") == nullptr);
    CHECK(Find(stackcards::StackCards("Jupiter.ser", Stacked(ser::Container::Ser), StackTimes(), Unnamed()), "DATE-OBS") == nullptr);
}

TEST_CASE("SRCVIDEO fits its card, extension kept; HISTORY holds the full name", "[stackcards]") {
    const std::string name = "2026-01-30-195830-Jupiter-ZWO ASI585MC-Gain=252-Exposure=5ms-Bin1-Lqef-long-name.avi";
    const stack::StackOutcome o = Stacked(ser::Container::Avi);
    const auto cards = stackcards::StackCards(name, o, StackTimes(), Unnamed());
    const Card* src = Find(cards, "SRCVIDEO");
    REQUIRE(src != nullptr);
    CHECK(fits::FitsStringLength(src->value) <= fits::kMaxStringValue);
    CHECK(src->value.size() > 60);
    CHECK(src->value.compare(src->value.size() - 7, 7, "....avi") == 0);
    CHECK(src->comment.find("HISTORY") != std::string::npos);

    const auto history = stackcards::StackHistory(name, o, Unnamed());
    REQUIRE_FALSE(history.empty());
    CHECK(history.front() == "PlanetaryStack " PLANETARY_VERSION_STRING ": stacked " + name);

    // A short name is written whole.
    CHECK(Find(stackcards::StackCards("Mars.ser", Stacked(ser::Container::Ser), StackTimes(), Unnamed()), "SRCVIDEO")->value == "Mars.ser");
}

TEST_CASE("counts, history and warnings come from the summary", "[stackcards]") {
    const stack::StackOutcome o = Stacked(ser::Container::Avi);
    const auto cards = stackcards::StackCards("J.avi", o, StackTimes(), Unnamed());
    CHECK(Find(cards, "NFRAMES")->value == "40");
    CHECK_FALSE(Find(cards, "NFRAMES")->text);
    CHECK(Find(cards, "NCOMBINE")->value == "18");
    CHECK(Find(cards, "BAYERPAT") == nullptr);
    const auto h = stackcards::StackHistory("J.avi", o, Unnamed());
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
    CHECK(stackcards::StackInfoLine("Saturn.ser", ser, "Saturn.ser") == "Saturn.ser: 60 frames, 128 x 112, 16-bit, mono, target not in file name");
    ser.colorId = ser::ColorId::BayerGRBG;
    CHECK(stackcards::StackInfoLine("Saturn.ser", ser, "Saturn.ser") == "Saturn.ser: 60 frames, 128 x 112, 16-bit, Bayer GRBG, target not in file name");
    ser.colorId = ser::ColorId::BGR;
    CHECK(stackcards::StackInfoLine("Saturn.ser", ser, "Saturn.ser") == "Saturn.ser: 60 frames, 128 x 112, 16-bit, RGB, target not in file name");

    const stack::StackOutcome avi = Stacked(ser::Container::Avi);
    ser::FileInfo a = avi.info;
    a.header.width = 1024;
    a.header.height = 768;
    a.header.depth = 8;
    a.frameCount = 1500;
    CHECK(stackcards::StackInfoLine("Jupiter.avi", a, "Jupiter.avi") == "Jupiter.avi: 1500 frames, 1024 x 768, 8-bit, Bayer RGGB (sidecar), target not in file name");
}

TEST_CASE("the window's result: counts and time, each warning, the selection and alignment lines", "[stackcards][window]") {
    stack::StackOutcome o = Stacked(ser::Container::Ser);
    o.summary.seconds = 5.5;
    CHECK(stackcards::ResultText(o) ==
          "Stacked: 18 of 40 frames used, in 5.50 s.\n"
          "Warning: image may be out of focus: detail 12.3 px\n"
          "Frames: selected 18 of 40 frames: the frames judged sharpest\n"
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

TEST_CASE("the progress text names the step and how far it is, in frames where it counts frames", "[stackcards][window]") {
    // Measuring: the engine's total is the hot-pixel frames plus every frame; the text counts frames.
    CHECK(stackcards::ProgressText(stack::kPhaseMeasuring, 0, 1564, 1500, 3) == "Step 1 of 3: Measuring frames 0/1500");
    CHECK(stackcards::ProgressText(stack::kPhaseMeasuring, 40, 1564, 1500, 3) == "Step 1 of 3: Measuring frames 0/1500");
    CHECK(stackcards::ProgressText(stack::kPhaseMeasuring, 1298, 1564, 1500, 3) == "Step 1 of 3: Measuring frames 1234/1500");
    CHECK(stackcards::ProgressText(stack::kPhaseMeasuring, 1564, 1564, 1500, 3) == "Step 1 of 3: Measuring frames 1500/1500");
    // Aligning's total is not a frame count: a percentage.
    CHECK(stackcards::ProgressText(stack::kPhaseAligning, 733, 1464, 1500, 3) == "Step 2 of 3: Aligning 50%");
    // Stacking counts the frames the plan uses.
    CHECK(stackcards::ProgressText(stack::kPhaseStacking, 312, 1400, 1500, 3) == "Step 3 of 3: Stacking 312/1400");
    CHECK(stackcards::ProgressText("Something else", 1, 0, 10, 3) == "Something else");
    CHECK(stackcards::ProgressFraction(733, 1464) == Catch::Approx(733.0 / 1464));
    CHECK(stackcards::ProgressFraction(5, 0) == 0.0);
    CHECK(stackcards::ProgressFraction(9, 4) == 1.0);
}

namespace {

// An undeclared mosaic as RunStack returns it (Ruling P9): an AVI with no pattern, stacked as raw mono,
// with the reader's warning and the engine's P9 warning in its summary.
stack::StackOutcome Mosaic() {
    stack::StackOutcome o = Stacked(ser::Container::Avi);
    o.info.colorId = ser::ColorId::Mono;
    o.info.avi->bayerSource = "none";
    o.image = ser::FloatImage{4, 4, 1, std::vector<float>(16, 0.5f)};
    o.summary.warnings = {std::string("no Bayer pattern known (no .avi.txt sidecar)") + ser::kRawCfaAsMonoTail,
                          stack::kUndeclaredMosaicWarning,
                          "The stack may be out of focus: soft"};
    return o;
}

size_t Count(const std::string& text, const std::string& what) {
    size_t n = 0;
    for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++n;
    return n;
}

}  // namespace

TEST_CASE("an undeclared mosaic gives one warning, with the reader's reason and this context's remedy", "[stackcards]") {
    REQUIRE(stack::UndeclaredMosaic(Mosaic().info));
    for (auto where : {stackcards::WarningsFor::Window, stackcards::WarningsFor::Process}) {
        const std::vector<std::string> w = stackcards::ShownWarnings(Mosaic(), where);
        REQUIRE(w.size() == 2);   // the mosaic's one line, then the focus warning
        CHECK(w[0].rfind("No Bayer pattern known (no .avi.txt sidecar), so the raw colour mosaic was stacked as mono", 0) == 0);
        CHECK(w[0].find("cannot be debayered afterwards") != std::string::npos);
        CHECK(w[0].find("showing the raw CFA") == std::string::npos);
        CHECK(w[1] == "The stack may be out of focus: soft");
    }
    CHECK(stackcards::ShownWarnings(Mosaic(), stackcards::WarningsFor::Window)[0].find(
              "choose the pattern under Bayer pattern and Stack! again; Save pattern... then remembers it") != std::string::npos);
    CHECK(stackcards::ShownWarnings(Mosaic(), stackcards::WarningsFor::Process)[0].find(
              "set bayerPattern (or save the pattern with Save pattern... in PlanetaryStack or SERViewer) and stack again") !=
          std::string::npos);
}

TEST_CASE("an undeclared mosaic's result, console and HISTORY text carry its warning once", "[stackcards]") {
    const std::string result = stackcards::ResultText(Mosaic());
    CHECK(Count(result, "Warning: ") == 2);
    CHECK(Count(result, "cannot be debayered") == 1);
    CHECK(Count(result, "no .avi.txt sidecar") == 1);
    std::string history;
    for (const std::string& h : stackcards::StackHistory("m.avi", Mosaic(), Unnamed())) history += h + "\n";
    CHECK(Count(history, "PlanetaryStack warning: ") == 2);
    CHECK(Count(history, "cannot be debayered") == 1);
    CHECK(Count(history, "no .avi.txt sidecar") == 1);
    // The window's info line states the fact only.
    ser::FileInfo info = Mosaic().info;
    info.frameCount = 40;
    info.header.width = 192;
    info.header.height = 160;
    info.header.depth = 8;
    CHECK(stackcards::StackInfoLine("m.avi", info, "m.avi") == "m.avi: 40 frames, 192 x 160, 8-bit, no Bayer pattern known (raw CFA), target not in file name");
}

TEST_CASE("other warnings pass through, reworded for the context; a diagnosed mosaic keeps the reader's", "[stackcards]") {
    stack::StackOutcome o = Mosaic();
    o.image.reset();
    o.summary.warnings = {std::string("no Bayer pattern known (no .avi.txt sidecar)") + ser::kRawCfaAsMonoTail};
    const auto window = stackcards::ShownWarnings(o, stackcards::WarningsFor::Window);
    REQUIRE(window.size() == 1);
    CHECK(window[0] == planetary::preview::StackWindowWarning(o.summary.warnings[0]));
    const auto process = stackcards::ShownWarnings(o, stackcards::WarningsFor::Process);
    REQUIRE(process.size() == 1);
    CHECK(process[0] == planetary::preview::StackWarning(o.summary.warnings[0]));
    // A SER's warnings are its own text.
    stack::StackOutcome s = Stacked(ser::Container::Ser);
    CHECK(stackcards::ShownWarnings(s, stackcards::WarningsFor::Window) == s.summary.warnings);
}

TEST_CASE("a stack is reused only for the same capture: layout, pattern standing, frames and geometry", "[stackcards]") {
    const ser::FileInfo a = Stacked(ser::Container::Avi).info;
    CHECK(stackcards::SameCapture(a, a));
    ser::FileInfo b = a;
    b.colorId = ser::ColorId::BayerBGGR;
    CHECK_FALSE(stackcards::SameCapture(a, b));
    b = a;
    b.frameCount += 1;
    CHECK_FALSE(stackcards::SameCapture(a, b));
    b = a;
    b.header.width += 2;
    CHECK_FALSE(stackcards::SameCapture(a, b));
    b = a;
    b.header.depth = 16;
    CHECK_FALSE(stackcards::SameCapture(a, b));
    // The same pattern from another source (chosen, then the sidecar Save pattern... wrote) is the same capture.
    b = a;
    b.avi->bayerSource = "sidecar (SERViewer)";
    CHECK(stackcards::SameCapture(a, b));
    // Mono by choice vs mono because no pattern is known: not the same stack (P9's warning).
    ser::FileInfo none = Mosaic().info, chosen = none;
    chosen.avi->bayerSource = "chosen";
    CHECK_FALSE(stackcards::SameCapture(none, chosen));
}

// ---- Target and Mineral moon (Task B8) ----------------------------------------------------------

namespace {

const std::string kMoonName = "2026-10-23-030000-Moon-Bin1 Lqef 64.6F.ser";
const std::string kHaO3Name = "2026-10-23-030000-Moon-Bin1 HaO3 16.2F.ser";

ser::FileInfo BayerSer() {
    ser::FileInfo i;
    i.container = ser::Container::Ser;
    i.colorId = ser::ColorId::BayerRGGB;
    i.planes = 1;
    return i;
}

stack::MineralOutcome SevenSteps(const std::string& notice = "") {
    stack::MineralOutcome m;
    m.history = {"Lunar disk: 31.2% of the frame, sky 52.0%", "Colour calibration: R x 1.012, B x 1.204",
                 "Background: plane removed", "Display transfer: linked midtones 0.01234",
                 "Channel alignment: R 0.02 px, B 0.03 px; skipped",
                 "Sharpen (luminance): amount 1.50, starlet layers 1-4, noise 0.00014; gains 2.40 4.10 1.00 1.00 "
                 "(noise-limited); noise: the sky's (no stack noise model given)",
                 "Mineral colour: k 8.00, chroma blur 2.50 px, eps 0.0020"};
    m.notice = notice;
    m.clipped = !notice.empty();  // a no-disc notice (the caution on a processed image: ruling Q13's test)
    return m;
}

}  // namespace

TEST_CASE("the target: Auto from an ASIAIR file name, the user's choice, or not in the name", "[stackcards][target]") {
    stackcards::TargetChoice t = stackcards::ResolveTarget(std::nullopt, kMoonName);
    CHECK(t.target == ser::Target::Moon);
    CHECK_FALSE(t.setByUser);
    CHECK(t.inFileName);
    // A full path, and a Windows one.
    CHECK(stackcards::ResolveTarget(std::nullopt, "/mnt/qnap/astro/" + kMoonName).target == ser::Target::Moon);
    CHECK(stackcards::ResolveTarget(std::nullopt, "C:\\captures\\" + kMoonName).target == ser::Target::Moon);
    t = stackcards::ResolveTarget(ser::Target::Saturn, kMoonName);
    CHECK(t.target == ser::Target::Saturn);
    CHECK(t.setByUser);
    t = stackcards::ResolveTarget(std::nullopt, "moon_stack_final.ser");
    CHECK(t.target == ser::Target::Other);
    CHECK_FALSE(t.setByUser);
    CHECK_FALSE(t.inFileName);
    // A parsed name with an unknown target is Other, from the file name.
    t = stackcards::ResolveTarget(std::nullopt, "2026-02-10-215957-Betelgeuse-Bin1 Lqef -4.0F.avi");
    CHECK(t.target == ser::Target::Other);
    CHECK(t.inFileName);
}

TEST_CASE("OBJECT names the target unless it is Other; HISTORY says where it came from", "[stackcards][target]") {
    const stack::StackOutcome o = Stacked(ser::Container::Ser);
    auto cards = stackcards::StackCards(kMoonName, o, StackTimes(), stackcards::ResolveTarget(std::nullopt, kMoonName));
    REQUIRE(Find(cards, "OBJECT") != nullptr);
    CHECK(Find(cards, "OBJECT")->value == "Moon");
    CHECK(Find(cards, "OBJECT")->text);
    CHECK(Find(cards, "OBJECT")->comment == "Target, from the file name");
    auto h = stackcards::StackHistory(kMoonName, o, stackcards::ResolveTarget(std::nullopt, kMoonName));
    REQUIRE(h.size() > 1);
    CHECK(h[1] == "PlanetaryStack target: Moon (from file name)");

    cards = stackcards::StackCards(kMoonName, o, StackTimes(), stackcards::ResolveTarget(ser::Target::Jupiter, kMoonName));
    REQUIRE(Find(cards, "OBJECT") != nullptr);
    CHECK(Find(cards, "OBJECT")->value == "Jupiter");
    CHECK(Find(cards, "OBJECT")->comment == "Target, set by the user");
    h = stackcards::StackHistory(kMoonName, o, stackcards::ResolveTarget(ser::Target::Jupiter, kMoonName));
    CHECK(h[1] == "PlanetaryStack target: Jupiter (set by user)");

    cards = stackcards::StackCards("Mars.ser", o, StackTimes(), Unnamed());
    CHECK(Find(cards, "OBJECT") == nullptr);
    CHECK(stackcards::StackHistory("Mars.ser", o, Unnamed())[1] == "PlanetaryStack target: Other (not in file name)");
    CHECK(Find(stackcards::StackCards("Mars.ser", o, StackTimes(), stackcards::ResolveTarget(ser::Target::Other, "Mars.ser")),
               "OBJECT") == nullptr);
    CHECK(stackcards::StackHistory("Mars.ser", o, stackcards::ResolveTarget(ser::Target::Other, "Mars.ser"))[1] ==
          "PlanetaryStack target: Other (set by user)");
    const std::string betelgeuse = "2026-02-10-215957-Betelgeuse-Bin1 Lqef -4.0F.avi";
    CHECK(stackcards::StackHistory(betelgeuse, o, stackcards::ResolveTarget(std::nullopt, betelgeuse))[1] ==
          "PlanetaryStack target: Other (from file name)");
}

TEST_CASE("the window's info line notes the target from the file name, or that it is not there", "[stackcards][window][target]") {
    ser::FileInfo i = BayerSer();
    i.header.width = 1936;
    i.header.height = 1096;
    i.header.depth = 16;
    i.frameCount = 500;
    CHECK(stackcards::StackInfoLine(kMoonName, i, kMoonName) ==
          kMoonName + ": 500 frames, 1936 x 1096, 16-bit, Bayer RGGB, target from file name: Moon");
    const std::string betelgeuse = "2026-02-10-215957-Betelgeuse-Bin1 Lqef -4.0F.ser";
    CHECK(stackcards::StackInfoLine(betelgeuse, i, betelgeuse) ==
          betelgeuse + ": 500 frames, 1936 x 1096, 16-bit, Bayer RGGB, target from file name: Other (Betelgeuse)");
}

TEST_CASE("the mineral moon's HISTORY: its seven step lines, in order, prefixed", "[stackcards][mineral]") {
    const stack::MineralOutcome m = SevenSteps();
    const std::vector<std::string> h = stackcards::MineralHistory(m);
    REQUIRE(h.size() == 7);
    for (size_t i = 0; i < h.size(); ++i) CHECK(h[i] == "PlanetaryStack mineral: " + m.history[i]);
    // Only ASCII ever reaches a card.
    stack::MineralOutcome odd = m;
    odd.history[0] = "Lunar disk: caf\xc3\xa9";
    CHECK(stackcards::MineralHistory(odd)[0] == "PlanetaryStack mineral: Lunar disk: caf?");
}

TEST_CASE("a mineral moon with a notice says why on the result line; none without", "[stackcards][mineral]") {
    CHECK(stackcards::MineralNoticeText("").empty());
    CHECK(stackcards::MineralNoticeText("no lunar disk distinguished from the sky") ==
          "Mineral moon: no lunar disk distinguished from the sky, so the mineral image is the stack clipped to "
          "[0, 1], with no colour or sharpening.");
    // The window's mineral lines: the colour step, the Sharpen step up to its noise note (Task SHARP: the slider's
    // amount and the gains it gave), then the notice as a warning.
    CHECK(stackcards::MineralResultText(SevenSteps()) ==
          "Mineral moon shown: Mineral colour: k 8.00, chroma blur 2.50 px, eps 0.0020\n"
          "Sharpen (luminance): amount 1.50, starlet layers 1-4, noise 0.00014; gains 2.40 4.10 1.00 1.00 (noise-limited)");
    const std::string withNotice = stackcards::MineralResultText(SevenSteps("no lunar disk distinguished from the sky"));
    CHECK(withNotice.find("\nWarning: Mineral moon: no lunar disk distinguished from the sky, so the mineral image") !=
          std::string::npos);
}

TEST_CASE("Mineral moon is refused from the capture's name, the target and the reader's colour", "[stackcards][mineral]") {
    const ser::FileInfo bayer = BayerSer();
    CHECK_FALSE(stackcards::MineralRefusalFor(kMoonName, bayer, stackcards::ResolveTarget(std::nullopt, kMoonName)));
    // The filter is named in the narrowband reason (B1 m1), from a full path too.
    const auto nb = stackcards::MineralRefusalFor("/d/" + kHaO3Name, bayer, stackcards::ResolveTarget(std::nullopt, kHaO3Name));
    REQUIRE(nb);
    CHECK(*nb == "Mineral moon needs a broadband filter; HaO3 is narrowband, so its colours come from the filter.");
    const auto saturn = stackcards::MineralRefusalFor(kMoonName, bayer, stackcards::ResolveTarget(ser::Target::Saturn, kMoonName));
    REQUIRE(saturn);
    CHECK(*saturn == "Mineral moon is for the Moon; the target is Saturn.");
    // An IR-pass filter, as named by the file (final review B, m4).
    const std::string ir = "2026-10-23-030000-Moon-Bin1 IR685 16.2F.ser";
    const auto irp = stackcards::MineralRefusalFor(ir, bayer, stackcards::ResolveTarget(std::nullopt, ir));
    REQUIRE(irp);
    CHECK(*irp == "Mineral moon needs a broadband filter; IR685 is IR-pass, so its colours come from the filter.");
    // An unrecognised filter token: allowed, as is a name without one.
    const std::string odd = "2026-10-23-030000-Moon-Bin1 Weird 16.2F.ser";
    CHECK_FALSE(stackcards::MineralRefusalFor(odd, bayer, stackcards::ResolveTarget(std::nullopt, odd)));
    // The Moon chosen by the user for a name without one: allowed (no filter known).
    CHECK_FALSE(stackcards::MineralRefusalFor("moon.ser", bayer, stackcards::ResolveTarget(ser::Target::Moon, "moon.ser")));
    // B1 m2: an AVI is Mono until OpenVideo resolves its Bayer pattern -- the info after resolution is a Bayer
    // capture and allowed; a capture that is still mono after it (no pattern known, or None chosen) is refused.
    ser::FileInfo avi = bayer;
    avi.container = ser::Container::Avi;
    CHECK_FALSE(stackcards::MineralRefusalFor(kMoonName, avi, stackcards::ResolveTarget(std::nullopt, kMoonName)));
    avi.colorId = ser::ColorId::Mono;
    const auto mono = stackcards::MineralRefusalFor(kMoonName, avi, stackcards::ResolveTarget(std::nullopt, kMoonName));
    REQUIRE(mono);
    CHECK(mono->find("this one is mono") != std::string::npos);
}

TEST_CASE("the progress text counts four steps when the mineral moon follows the stack", "[stackcards][window][mineral]") {
    CHECK(stackcards::ProgressText(stack::kPhaseMeasuring, 1298, 1564, 1500, 4) == "Step 1 of 4: Measuring frames 1234/1500");
    CHECK(stackcards::ProgressText(stack::kPhaseStacking, 312, 1400, 1500, 4) == "Step 3 of 4: Stacking 312/1400");
    CHECK(stackcards::ProgressText(stack::kPhaseMineral, 3, 7, 1500, 4) == "Step 4 of 4: Mineral moon 3/7");
    // The mineral moon of a stack already shown: its one step, unnumbered.
    CHECK(stackcards::ProgressText(stack::kPhaseMineral, 7, 7, 1500, 1) == "Mineral moon 7/7");
}

TEST_CASE("the info line reads the target from the same full path as the target itself (B8 review M5)", "[stackcards][window][target]") {
    ser::FileInfo i = BayerSer();
    i.header.width = 640;
    i.header.height = 480;
    i.header.depth = 16;
    i.frameCount = 24;
    // A directory named like a capture: ParseCaptureName on the full path reads its target, as ResolveTarget does.
    const std::string path = "/captures/2026-10-23-030000-Moon-Bin1 Lqef 64.6F.d/stack.ser";
    REQUIRE(stackcards::ResolveTarget(std::nullopt, path).target == ser::Target::Moon);
    CHECK(stackcards::StackInfoLine("stack.ser", i, path) ==
          "stack.ser: 24 frames, 640 x 480, 16-bit, Bayer RGGB, target from file name: Moon");
}

// ---- Planet processing (planet Task 6) -----------------------------------------------------------

#include "PlanetSynth.h"

namespace {

const std::string kSaturnName = "2026-10-23-041500-Saturn-Bin1 Lqef 17.6F.ser";

// A small ringed Saturn as a stack holds it: the camera's cast, seeing, noise.
ser::FloatImage SaturnStack() {
    testing::PlanetSceneSpec s;
    s.width = 128;
    s.height = 96;
    s.cx = 64.3;
    s.cy = 47.8;
    s.radius = 16;
    s.ring = true;
    s.ringTiltDeg = 15;
    s.tint = {1.3, 1, 0.68};
    s.cast = {1.0, 0.8, 0.55};
    s.pedestal = {0.01, 0.012, 0.015};
    s.blurSigma = 1.2;
    s.noiseStd = 0.004;
    s.seed = 31;
    return testing::MakePlanetScene(s).observed;
}

stack::MineralOutcome PlanetSevenSteps(const std::string& notice = "") {
    stack::MineralOutcome m;
    m.history = {"Planet disc: 812 px (equivalent radius 16.1 px), sky 91.0%", "Colour calibration: to Saturn's ...",
                 "Background: plane removed", "Display transfer: linear, white 0.81",
                 "Channel alignment: R 0.02 px, B 0.03 px; skipped",
                 "Sharpen (luminance): amount 1.50, starlet layers 1-4; gains 1.60 1.00 1.00 1.00; noise: the stack's model",
                 "Saturation: 1.00x the natural colour (the chroma layer's channel ratios on the sharp luminance), chroma "
                 "blur 1.00 px, eps 0.0011; no deviation clamp, no highlight fade; 0.0% of pixels held in gamut"};
    m.notice = notice;
    m.clipped = !notice.empty();  // a no-disc notice (the caution on a processed image: ruling Q13's test)
    return m;
}

}  // namespace

TEST_CASE("the post-processing row follows the target: the Moon's mineral moon, planet processing otherwise", "[stackcards][planet]") {
    CHECK(stackcards::PostKindFor(ser::Target::Moon) == stackcards::PostKind::Mineral);
    for (ser::Target t : {ser::Target::Mercury, ser::Target::Venus, ser::Target::Mars, ser::Target::Jupiter, ser::Target::Saturn,
                          ser::Target::Uranus, ser::Target::Neptune, ser::Target::Other})
        CHECK(stackcards::PostKindFor(t) == stackcards::PostKind::Planet);
    CHECK(stackcards::PostTitle(stackcards::PostKind::Mineral) == "Mineral moon");
    CHECK(stackcards::PostTitle(stackcards::PostKind::Planet) == "Planet processing");
    CHECK(stackcards::PostNoun(stackcards::PostKind::Mineral) == "mineral");
    CHECK(stackcards::PostNoun(stackcards::PostKind::Planet) == "planet");
    CHECK(stackcards::PostName(stackcards::PostKind::Mineral) == "mineral moon");
    CHECK(stackcards::PostName(stackcards::PostKind::Planet) == "planet processing");
    stackcards::PostParams p;
    p.kind = stackcards::PostKind::Planet;
    p.planet.saturation = 2;
    p.planet.chromaBlurSigma = 0;
    p.planet.sharpen = 3.5;
    CHECK(stackcards::PostParamsText(p) == "saturation 2.00, chroma blur 0.00 px, sharpen 3.50");
    p.kind = stackcards::PostKind::Mineral;
    CHECK(stackcards::PostParamsText(p) == "strength 8.00, chroma blur 2.50 px, sharpen " + stackcards::Fixed(stack::kSharpenDefault, 2));
}

TEST_CASE("planet processing is refused from the capture's name, the target and the reader's colour", "[stackcards][planet]") {
    const ser::FileInfo bayer = BayerSer();
    CHECK_FALSE(stackcards::PlanetRefusalFor(kSaturnName, bayer, stackcards::ResolveTarget(std::nullopt, kSaturnName)));
    const std::string hao3 = "/d/2026-10-05-031000-Saturn-Bin1 HaO3 9.0F.ser";
    const auto nb = stackcards::PlanetRefusalFor(hao3, bayer, stackcards::ResolveTarget(std::nullopt, hao3));
    REQUIRE(nb);
    CHECK(*nb == "Planet processing needs a broadband filter; HaO3 is narrowband, so its colours come from the filter.");
    const auto other = stackcards::PlanetRefusalFor(kSaturnName, bayer, stackcards::ResolveTarget(ser::Target::Other, kSaturnName));
    REQUIRE(other);
    CHECK(*other == "Planet processing needs a planet target; pick the planet in the Target list.");
    const auto moon = stackcards::PlanetRefusalFor(kSaturnName, bayer, stackcards::ResolveTarget(ser::Target::Moon, kSaturnName));
    REQUIRE(moon);
    CHECK(moon->find("use Mineral moon") != std::string::npos);
    ser::FileInfo mono = bayer;
    mono.colorId = ser::ColorId::Mono;
    const auto m = stackcards::PlanetRefusalFor(kSaturnName, mono, stackcards::ResolveTarget(std::nullopt, kSaturnName));
    REQUIRE(m);
    CHECK(m->find("Planet processing needs a colour capture; this one is mono") == 0);
    // PostRefusalFor picks the kind's gate.
    CHECK(stackcards::PostRefusalFor(stackcards::PostKind::Planet, kSaturnName, bayer,
                                     stackcards::ResolveTarget(ser::Target::Other, kSaturnName)) == other);
    CHECK(stackcards::PostRefusalFor(stackcards::PostKind::Mineral, kMoonName, bayer,
                                     stackcards::ResolveTarget(std::nullopt, kMoonName)) ==
          stackcards::MineralRefusalFor(kMoonName, bayer, stackcards::ResolveTarget(std::nullopt, kMoonName)));
}

TEST_CASE("the post-processing runs as its engine runs it, bit for bit, mineral moon and planet alike", "[stackcards][planet]") {
    const ser::FloatImage stacked = SaturnStack();
    stackcards::PostParams planet;
    planet.kind = stackcards::PostKind::Planet;
    planet.target = ser::Target::Saturn;
    planet.planet.saturation = 1.7;
    planet.planet.chromaBlurSigma = 0.5;
    planet.planet.sharpen = 2.5;
    const stack::MineralCache pc = stackcards::PreparePost(planet, stacked, 3, {}, nullptr);
    const stack::MineralOutcome po = stackcards::FinishPost(planet, pc, 3);
    const stack::MineralOutcome want = stack::FinishPlanet(stack::PreparePlanet(stacked, ser::Target::Saturn, 3, {}), planet.planet, 3);
    CHECK(po.image.data == want.image.data);
    CHECK(po.history == want.history);

    stackcards::PostParams mineral;
    mineral.kind = stackcards::PostKind::Mineral;
    mineral.mineral.strength = 5;
    const stack::MineralCache mc = stackcards::PreparePost(mineral, stacked, 3, {}, nullptr);
    const stack::MineralOutcome mo = stackcards::FinishPost(mineral, mc, 3);
    const stack::MineralOutcome mwant = stack::MakeMineralMoon(stacked, 3, {}, mineral.mineral);
    CHECK(mo.image.data == mwant.image.data);
    CHECK(mo.history == mwant.history);
}

TEST_CASE("the planet window's cards: OBJECT the planet, then one HISTORY line per step, prefixed", "[stackcards][planet]") {
    const stackcards::TargetChoice saturn = stackcards::ResolveTarget(std::nullopt, kSaturnName);
    const std::vector<Card> cards = stackcards::StackCards(kSaturnName, Stacked(ser::Container::Ser), Times(), saturn);
    const Card* object = Find(cards, "OBJECT");
    REQUIRE(object != nullptr);
    CHECK(object->value == "Saturn");
    CHECK(object->comment == "Target, from the file name");

    stackcards::PostParams p;
    p.kind = stackcards::PostKind::Planet;
    p.target = ser::Target::Saturn;
    const stack::MineralOutcome out = stackcards::FinishPost(p, stackcards::PreparePost(p, SaturnStack(), 2, {}, nullptr), 2);
    const std::vector<std::string> h = stackcards::PostHistory(stackcards::PostKind::Planet, out);
    REQUIRE(h.size() == 7);
    const char* steps[] = {"Planet disc", "Colour calibration", "Background", "Display transfer", "Channel alignment", "Sharpen",
                           "Saturation"};
    for (size_t i = 0; i < h.size(); ++i) {
        CHECK(h[i] == "PlanetaryStack planet: " + fits::FitsAscii(out.history[i]));
        CHECK(h[i].rfind(std::string("PlanetaryStack planet: ") + steps[i], 0) == 0);
    }
    // The mineral moon's are unchanged: its own prefix.
    CHECK(stackcards::PostHistory(stackcards::PostKind::Mineral, SevenSteps()) == stackcards::MineralHistory(SevenSteps()));
    CHECK(stackcards::MineralHistory(SevenSteps())[0] == "PlanetaryStack mineral: " + SevenSteps().history[0]);
}

TEST_CASE("the planet's result line: the Saturation step, the Sharpen step up to its noise note, a notice as a warning", "[stackcards][planet]") {
    CHECK(stackcards::PostNoticeText(stackcards::PostKind::Planet, "").empty());
    CHECK(stackcards::PostNoticeText(stackcards::PostKind::Planet, "no planet disc distinguished from the sky") ==
          "Planet processing: no planet disc distinguished from the sky, so the planet image is the stack clipped to [0, 1], "
          "with no colour or sharpening.");
    CHECK(stackcards::PostResultText(stackcards::PostKind::Planet, PlanetSevenSteps()) ==
          "Planet processing shown: " + PlanetSevenSteps().history[6] + "\n"
          "Sharpen (luminance): amount 1.50, starlet layers 1-4; gains 1.60 1.00 1.00 1.00");
    const std::string withNotice =
        stackcards::PostResultText(stackcards::PostKind::Planet, PlanetSevenSteps("no planet disc distinguished from the sky"));
    CHECK(withNotice.find("\nWarning: Planet processing: no planet disc distinguished from the sky, so the planet image") !=
          std::string::npos);
    // The mineral moon's, through the same code, as before.
    CHECK(stackcards::PostResultText(stackcards::PostKind::Mineral, SevenSteps()) == stackcards::MineralResultText(SevenSteps()));
    CHECK(stackcards::PostNoticeText(stackcards::PostKind::Mineral, "x") == stackcards::MineralNoticeText("x"));
}

TEST_CASE("a planet disc filling the frame reaches the console and the window as a caution on a processed image (ruling Q13)", "[stackcards][planet]") {
    const std::string caution =
        "the planet disc is 75% or more of the frame: the disc fills the frame, or the sky is curved more than a quadratic "
        "models; colour calibration, the background and the sharpening then treat the whole frame as the planet";
    stack::MineralOutcome post = PlanetSevenSteps(caution);
    post.clipped = false;
    // The console's warning (ShowPostWindow, the window's keepPost): the caution as the engine gives it, never "the
    // stack clipped".
    CHECK(stackcards::PostNoticeText(stackcards::PostKind::Planet, post) == "Planet processing: " + caution + ".");
    CHECK(stackcards::PostResultText(stackcards::PostKind::Planet, post).find("\nWarning: Planet processing: " + caution + ".") !=
          std::string::npos);
    CHECK(stackcards::PostResultText(stackcards::PostKind::Planet, post).find("clipped") == std::string::npos);
    CHECK(stackcards::PostImageText(stackcards::PostKind::Planet, post) ==
          "planet processing, display-ready, see the warning above");
    CHECK(stackcards::PostStatusText(stackcards::PostKind::Planet, post) == "Planet processing shown, with a warning; see below.");
    // No disc: as before (the stack clipped).
    stack::MineralOutcome none = PlanetSevenSteps("no planet disc distinguished from the sky");
    none.clipped = true;
    CHECK(stackcards::PostNoticeText(stackcards::PostKind::Planet, none) ==
          stackcards::PostNoticeText(stackcards::PostKind::Planet, none.notice));
    CHECK(stackcards::PostImageText(stackcards::PostKind::Planet, none) ==
          "the stack clipped to [0, 1]: no planet processing, see the warning above");
    CHECK(stackcards::PostStatusText(stackcards::PostKind::Planet, none) == "Planet processing: no planet disc; see below.");
    stack::MineralOutcome noLunar = SevenSteps("no lunar disk distinguished from the sky");
    noLunar.clipped = true;
    CHECK(stackcards::PostStatusText(stackcards::PostKind::Mineral, noLunar) == "Mineral moon: no lunar disk; see below.");
    // No notice: shown, display-ready.
    CHECK(stackcards::PostNoticeText(stackcards::PostKind::Planet, PlanetSevenSteps()).empty());
    CHECK(stackcards::PostImageText(stackcards::PostKind::Planet, PlanetSevenSteps()) == "planet processing, display-ready");
    CHECK(stackcards::PostStatusText(stackcards::PostKind::Planet, PlanetSevenSteps()) == "Planet processing shown.");
    CHECK(stackcards::PostStatusText(stackcards::PostKind::Mineral, SevenSteps()) == "Mineral moon shown.");
}

TEST_CASE("the console's context names the post-processing and its parameters, or its absence", "[stackcards][planet]") {
    stackcards::PostParams m;
    m.kind = stackcards::PostKind::Mineral;
    m.mineral.strength = 5;
    m.mineral.chromaBlurSigma = 2;
    m.mineral.sharpen = 2.5;
    CHECK(stackcards::PostContextText(ser::Target::Moon, &m) ==
          "; then the mineral moon: strength 5.00, chroma blur 2.00 px, sharpen 2.50");
    stackcards::PostParams p;
    p.kind = stackcards::PostKind::Planet;
    p.target = ser::Target::Saturn;
    p.planet.saturation = 1.25;
    CHECK(stackcards::PostContextText(ser::Target::Saturn, &p) ==
          "; then planet processing: saturation 1.25, chroma blur " + stackcards::Fixed(stack::kPlanetChromaBlurDefault, 2) +
              " px, sharpen " + stackcards::Fixed(stack::kPlanetSharpenDefault, 2));
    CHECK(stackcards::PostContextText(ser::Target::Moon, nullptr) == "; no mineral moon");
    CHECK(stackcards::PostContextText(ser::Target::Saturn, nullptr) == "; no planet processing");
    CHECK(stackcards::PostContextText(ser::Target::Other, nullptr) == "; no planet processing");
}

TEST_CASE("mineralMoon and planetProcess both on: refused, only one of the two can run", "[stackcards][planet]") {
    // Final review (Task 6 minor): a script setting both got only the mineral moon's refusal (or the planet's) and no
    // word that the two exclude each other; the run now fails with that reason, before any stacking.
    CHECK_FALSE(stackcards::PostConflict(false, false).has_value());
    CHECK_FALSE(stackcards::PostConflict(true, false).has_value());
    CHECK_FALSE(stackcards::PostConflict(false, true).has_value());
    const std::optional<std::string> why = stackcards::PostConflict(true, true);
    REQUIRE(why.has_value());
    CHECK(*why == "Mineral moon and Planet processing are both on; only one of the two can run (Mineral moon for the Moon, "
                  "Planet processing for a planet).");
}

TEST_CASE("post-processing parameters: equal values, and when a cache made for one serves the other", "[stackcards][planet]") {
    stackcards::PostParams a;
    a.kind = stackcards::PostKind::Planet;
    a.target = ser::Target::Saturn;
    stackcards::PostParams b = a;
    CHECK(stackcards::SamePostParams(a, b));
    b.planet.sharpen = 3;
    CHECK_FALSE(stackcards::SamePostParams(a, b));
    CHECK(stackcards::SamePostData(a, b));   // the sliders re-run only the finish
    b.target = ser::Target::Jupiter;
    CHECK_FALSE(stackcards::SamePostData(a, b));   // another planet's catalogue colour: prepared again
    stackcards::PostParams m;
    m.kind = stackcards::PostKind::Mineral;
    CHECK_FALSE(stackcards::SamePostData(a, m));
    stackcards::PostParams m2 = m;
    m2.target = ser::Target::Saturn;   // the mineral moon has no catalogue colour: the target does not enter
    CHECK(stackcards::SamePostData(m, m2));
    m2.mineral.strength = 3;
    CHECK_FALSE(stackcards::SamePostParams(m, m2));
}

TEST_CASE("the progress text counts planet processing as the mineral moon's step", "[stackcards][window][planet]") {
    CHECK(stackcards::ProgressText(stack::kPhasePlanet, 3, 7, 1500, 4) == "Step 4 of 4: Planet processing 3/7");
    CHECK(stackcards::ProgressText(stack::kPhasePlanet, 7, 7, 1500, 1) == "Planet processing 7/7");
}
