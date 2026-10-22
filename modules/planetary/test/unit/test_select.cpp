#include <catch_amalgamated.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <random>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include "Measure.h"
#include "Select.h"

using namespace planetary;
using stack::DiagnosisKind;
using stack::FrameStats;
using stack::Selection;
using stack::SelectFrames;

namespace {

ser::FileInfo Info(int w, int h, ser::ColorId color = ser::ColorId::Mono) {
    ser::FileInfo info;
    info.header.width = w;
    info.header.height = h;
    info.header.depth = 8;
    info.colorId = color;
    info.normDepth = 8;
    return info;
}

// Quality of a disc of radius R (luma px) whose limb is blurred by sigma (luma px), by the disc
// model Select.h documents: quality^2 = 1 / (sqrt(pi) R sigma).
double DiscQuality(double radius, double sigma) { return 1 / std::sqrt(std::sqrt(M_PI) * radius * sigma); }

// A disc target centred at (cx, cy) in a luma image of lw x lh: the box is the disc grown by one
// detection block, as Measure makes it, clipped to the image.
FrameStats Disc(int64_t index, int lw, int lh, double cx, double cy, double radius, double sigma) {
    const int d = stack::DetectionBlock(lw, lh);
    FrameStats s;
    s.index = index;
    s.targetFound = true;
    s.cx = cx;
    s.cy = cy;
    s.bx0 = std::max(0, static_cast<int>(std::floor(cx - radius)) - d);
    s.by0 = std::max(0, static_cast<int>(std::floor(cy - radius)) - d);
    s.bx1 = std::min(lw, static_cast<int>(std::ceil(cx + radius)) + d);
    s.by1 = std::min(lh, static_cast<int>(std::ceil(cy + radius)) + d);
    s.quality = DiscQuality(radius, sigma);
    s.brightness = 0.5;
    s.clippedFraction = 0;
    return s;
}

FrameStats NoTargetFrame(int64_t index) {
    FrameStats s;
    s.index = index;
    s.brightness = 0.02;
    return s;
}

double Number(const stack::Diagnosis& d, const std::string& name) {
    for (const auto& [n, v] : d.numbers)
        if (n == name) return v;
    FAIL("diagnosis has no number named \"" << name << "\": " << d.message);
    return 0;
}

// Checks what every successful selection promises; returns the indices as a set.
std::set<int64_t> CheckSelected(const Selection& sel, const std::vector<FrameStats>& stats) {
    REQUIRE(sel.diagnosis.kind == DiagnosisKind::None);
    REQUIRE(sel.diagnosis.message.empty());
    REQUIRE_FALSE(sel.frames.empty());
    REQUIRE(std::is_sorted(sel.frames.begin(), sel.frames.end()));
    REQUIRE(std::adjacent_find(sel.frames.begin(), sel.frames.end()) == sel.frames.end());
    double lowest = 1e300;
    for (int64_t f : sel.frames) {
        const auto it = std::find_if(stats.begin(), stats.end(), [&](const FrameStats& s) { return s.index == f; });
        REQUIRE(it != stats.end());
        lowest = std::min(lowest, it->quality);
    }
    CHECK(sel.threshold == lowest);
    // One line, the spec's style, numbers matching.
    std::smatch m;
    const std::regex re(R"(^used (\d+) of (\d+) frames: the sharpest group, quality >= ([01]\.\d\d) of the best)");
    INFO(sel.explanation);
    REQUIRE(std::regex_search(sel.explanation, m, re));
    CHECK(std::stoll(m[1]) == static_cast<long long>(sel.frames.size()));
    CHECK(std::stoll(m[2]) == static_cast<long long>(stats.size()));
    CHECK(sel.explanation.find('\n') == std::string::npos);
    return {sel.frames.begin(), sel.frames.end()};
}

void CheckDiagnosed(const Selection& sel, DiagnosisKind kind) {
    INFO(sel.explanation);
    REQUIRE(sel.diagnosis.kind == kind);
    CHECK(sel.frames.empty());
    CHECK(sel.threshold == 0);
    CHECK_FALSE(sel.diagnosis.message.empty());
    CHECK(sel.explanation == sel.diagnosis.message);
    CHECK_FALSE(sel.diagnosis.numbers.empty());
    for (const auto& [n, v] : sel.diagnosis.numbers) CHECK(std::isfinite(v));
}

// A capture of n frames of one disc (R = 60 luma px in 640 x 480), sigma per frame from `sigma`.
template <class F>
std::vector<FrameStats> DiscCapture(int64_t n, F sigma) {
    std::vector<FrameStats> v;
    for (int64_t i = 0; i < n; ++i) v.push_back(Disc(i, 640, 480, 320, 240, 60, sigma(i)));
    return v;
}

}  // namespace

TEST_CASE("bimodal capture: keeps the steady mode, drops the poor-seeing mode", "[select]") {
    std::mt19937 rng(7);
    std::normal_distribution<double> n01(0, 1);
    // 30% steady-seeing frames (limb blur 1.0 luma px, 3% jitter) interleaved with poor ones (1.8).
    auto good = [](int64_t i) { return i % 10 < 3; };
    const auto stats = DiscCapture(2000, [&](int64_t i) {
        return good(i) ? 1.0 * (1 + 0.03 * n01(rng)) : 1.8 * (1 + 0.05 * n01(rng));
    });
    const Selection sel = SelectFrames(stats, Info(640, 480));
    const auto kept = CheckSelected(sel, stats);
    for (int64_t f : kept) CHECK(good(f));
    CHECK(kept.size() >= 540);  // >= 90% of the 600 good frames
    CHECK_FALSE(sel.fullFrameTarget);
}

TEST_CASE("uniformly good capture keeps most frames", "[select]") {
    std::mt19937 rng(11);
    std::normal_distribution<double> n01(0, 1);
    const auto stats = DiscCapture(2000, [&](int64_t) { return 1.0 * (1 + 0.02 * n01(rng)); });
    const auto kept = CheckSelected(SelectFrames(stats, Info(640, 480)), stats);
    CHECK(kept.size() >= 1800);
}

TEST_CASE("selection adapts: a steady night keeps many frames, a poor night few", "[select]") {
    std::mt19937 rng(13);
    std::normal_distribution<double> n01(0, 1);
    const auto steady = DiscCapture(3000, [&](int64_t) { return 1.0 * (1 + 0.03 * n01(rng)); });
    // Poor night: blur log-normal, 30% spread -- a few lucky frames, most blurred.
    const auto poor = DiscCapture(3000, [&](int64_t) { return 1.5 * std::exp(0.3 * n01(rng)); });
    const auto keptSteady = CheckSelected(SelectFrames(steady, Info(640, 480)), steady);
    const Selection selPoor = SelectFrames(poor, Info(640, 480));
    const auto keptPoor = CheckSelected(selPoor, poor);
    CHECK(keptSteady.size() >= 2700);
    CHECK(keptPoor.size() <= 900);
    CHECK(keptPoor.size() >= 5);
    // The kept frames of the poor night are its sharpest: none blurrier than any frame left out.
    double worstKept = 1e300, bestDropped = 0;
    for (const auto& s : poor) {
        if (keptPoor.count(s.index)) worstKept = std::min(worstKept, s.quality);
        else bestDropped = std::max(bestDropped, s.quality);
    }
    CHECK(worstKept >= bestDropped);
}

TEST_CASE("frames without a target, cut by the edge or clipped are never selected", "[select]") {
    // The excluded frames are given the sharpest qualities of all, so only the rule keeps them out.
    std::vector<FrameStats> stats;
    std::set<int64_t> bad;
    for (int64_t i = 0; i < 350; ++i) {
        FrameStats s = Disc(i, 640, 480, 320, 240, 60, 1.0 + 0.001 * static_cast<double>(i % 17));
        switch (i % 7) {
            case 0:
                s = NoTargetFrame(i);
                s.quality = 5;  // whatever it says, a frame without a target is out
                bad.insert(i);
                break;
            case 1: {  // the disc drifted onto one of the four edges (cycled), its box touching it
                const int e = static_cast<int>(i / 7) % 4;
                const double cx = e == 0 ? 40 : e == 1 ? 600 : 320, cy = e == 2 ? 40 : e == 3 ? 440 : 240;
                s = Disc(i, 640, 480, cx, cy, 60, 0.5);  // a cut limb reads sharp
                bad.insert(i);
                break;
            }
            case 2:
                s.quality = DiscQuality(60, 0.5);
                s.clippedFraction = 0.4;
                bad.insert(i);
                break;
            default:
                break;
        }
        stats.push_back(s);
    }
    const Selection sel = SelectFrames(stats, Info(640, 480));
    const auto kept = CheckSelected(sel, stats);
    for (int64_t f : kept) CHECK(bad.count(f) == 0);
    CHECK(kept.size() >= 180);  // nearly all of the 200 good frames
    CHECK(sel.explanation.find("not used: 50 without a target, 50 at the frame edge, 50 clipped") != std::string::npos);
}

TEST_CASE("the frame edge is the luma image's edge: Bayer luma is half size (LumaScale)", "[select]") {
    const ser::FileInfo bayer = Info(1920, 1080, ser::ColorId::BayerRGGB);
    REQUIRE(stack::LumaScale(bayer) == 2.0);
    REQUIRE(stack::LumaScale(Info(1920, 1080)) == 1.0);
    REQUIRE(stack::LumaScale(Info(1920, 1080, ser::ColorId::RGB)) == 1.0);
    // Luma 960 x 540. Frames 0..49 sit inside; frames 50..99 touch the luma right edge (x = 960),
    // which is not the frame edge in sensor pixels (1920) -- they must still be dropped.
    std::vector<FrameStats> stats;
    for (int64_t i = 0; i < 100; ++i)
        stats.push_back(Disc(i, 960, 540, i < 50 ? 480 : 930, 270, 40, 1.0));
    const auto kept = CheckSelected(SelectFrames(stats, bayer), stats);
    for (int64_t f : kept) CHECK(f < 50);
    CHECK(kept.size() >= 45);
}

TEST_CASE("full-frame target (lunar surface): edge contact is not drift, the cut is lenient", "[select]") {
    std::mt19937 rng(17);
    std::normal_distribution<double> n01(0, 1);
    struct Case {
        int w, h;
        ser::ColorId color;
        int bx0, by0, bx1, by1;  // in luma pixels
    };
    // The three real lunar framings measured 2026-10-21 (1, 2 and 3 edges touched) and a Bayer
    // capture whose box is the whole luma frame.
    const Case cases[] = {{1920, 1080, ser::ColorId::Mono, 152, 136, 1704, 1080},
                          {640, 480, ser::ColorId::Mono, 330, 0, 640, 321},
                          {480, 360, ser::ColorId::Mono, 0, 70, 480, 360},
                          {1920, 1080, ser::ColorId::BayerGRBG, 0, 0, 960, 540}};
    for (const Case& c : cases) {
        std::vector<FrameStats> stats;
        for (int64_t i = 0; i < 2120; ++i) {
            FrameStats s;
            s.index = i;
            s.targetFound = true;
            s.bx0 = c.bx0;
            s.by0 = c.by0;
            s.bx1 = c.bx1;
            s.by1 = c.by1;
            s.cx = (c.bx0 + c.bx1) / 2.0;
            s.cy = (c.by0 + c.by1) / 2.0;
            s.quality = 0.1608 * (1 + 0.002 * n01(rng));  // the measured lunar spread
            s.brightness = 0.4;
            stats.push_back(s);
        }
        const Selection sel = SelectFrames(stats, Info(c.w, c.h, c.color));
        INFO(c.w << "x" << c.h << " box " << c.bx0 << "," << c.by0 << "-" << c.bx1 << "," << c.by1);
        const auto kept = CheckSelected(sel, stats);
        CHECK(sel.fullFrameTarget);
        CHECK(kept.size() >= 2000);
        CHECK(sel.explanation.find("frame-filling target") != std::string::npos);
    }
}

TEST_CASE("a small disc sitting on the edge is drift, not a full-frame target", "[select]") {
    // 640 x 480, R = 20: every frame's box touches the left edge but spans far less than half.
    std::vector<FrameStats> stats;
    for (int64_t i = 0; i < 200; ++i) stats.push_back(Disc(i, 640, 480, 10, 240, 20, 1.0));
    const Selection sel = SelectFrames(stats, Info(640, 480));
    CheckDiagnosed(sel, DiagnosisKind::TargetLeavesFrame);
    CHECK_FALSE(sel.fullFrameTarget);
}

TEST_CASE("diagnosis: no target", "[select][diagnosis]") {
    std::vector<FrameStats> stats;
    for (int64_t i = 0; i < 21; ++i) stats.push_back(NoTargetFrame(i));
    const Selection sel = SelectFrames(stats, Info(1920, 1080));
    CheckDiagnosed(sel, DiagnosisKind::NoTarget);
    CHECK(Number(sel.diagnosis, "frames") == 21);
    CHECK(Number(sel.diagnosis, "frames with a target") == 0);

    // A target in 3 of 100 frames is still no capture of it.
    std::vector<FrameStats> few;
    for (int64_t i = 0; i < 100; ++i)
        few.push_back(i < 3 ? Disc(i, 640, 480, 320, 240, 60, 1.0) : NoTargetFrame(i));
    const Selection selFew = SelectFrames(few, Info(640, 480));
    CheckDiagnosed(selFew, DiagnosisKind::NoTarget);
    CHECK(Number(selFew.diagnosis, "frames with a target") == 3);

    // No frames at all.
    const Selection none = SelectFrames({}, Info(640, 480));
    CheckDiagnosed(none, DiagnosisKind::NoTarget);
    CHECK(Number(none.diagnosis, "frames") == 0);
}

TEST_CASE("diagnosis: target leaves the frame", "[select][diagnosis]") {
    std::vector<FrameStats> stats;
    for (int64_t i = 0; i < 100; ++i)
        stats.push_back(Disc(i, 640, 480, i < 3 ? 320.0 : 30.0 + static_cast<double>(i % 5), 240, 60, 1.0));
    const Selection sel = SelectFrames(stats, Info(640, 480));
    CheckDiagnosed(sel, DiagnosisKind::TargetLeavesFrame);
    CHECK(Number(sel.diagnosis, "frames with a target") == 100);
    CHECK(Number(sel.diagnosis, "frames at the frame edge") == 97);
    CHECK(Number(sel.diagnosis, "frames fully inside") == 3);
}

TEST_CASE("diagnosis: clipped", "[select][diagnosis]") {
    std::vector<FrameStats> stats;
    for (int64_t i = 0; i < 100; ++i) {
        FrameStats s = Disc(i, 640, 480, 320, 240, 60, 1.0);
        s.clippedFraction = i < 98 ? 0.37 : 0.1;
        stats.push_back(s);
    }
    const Selection sel = SelectFrames(stats, Info(640, 480));
    CheckDiagnosed(sel, DiagnosisKind::Clipped);
    CHECK(Number(sel.diagnosis, "frames considered") == 100);
    CHECK(Number(sel.diagnosis, "clipped frames") == 98);
    CHECK(Number(sel.diagnosis, "median clipped fraction") == Catch::Approx(0.37));
    CHECK(Number(sel.diagnosis, "clipped limit") == stack::kClippedFrame);
}

TEST_CASE("diagnosis: out of focus (steady blur over the limit)", "[select][diagnosis]") {
    std::mt19937 rng(19);
    std::normal_distribution<double> n01(0, 1);
    // R = 60 luma px, limb blur 10 luma px (1/6 of R) in every frame, 1% jitter.
    const auto stats = DiscCapture(500, [&](int64_t) { return 10.0 * (1 + 0.01 * n01(rng)); });
    const Selection mono = SelectFrames(stats, Info(640, 480));
    CheckDiagnosed(mono, DiagnosisKind::OutOfFocus);
    CHECK(Number(mono.diagnosis, "limb blur (pixels)") == Catch::Approx(10).epsilon(0.05));
    CHECK(Number(mono.diagnosis, "target radius (pixels)") == Catch::Approx(60).epsilon(0.05));
    CHECK(Number(mono.diagnosis, "blur / radius") == Catch::Approx(1.0 / 6).epsilon(0.05));
    CHECK(Number(mono.diagnosis, "blur limit / radius") == stack::kBlurLimit);
    CHECK(Number(mono.diagnosis, "blur spread") < stack::kSteadySpread);

    // The same luma statistics from a Bayer capture: pixels are reported at sensor scale.
    const Selection bayer = SelectFrames(stats, Info(1280, 960, ser::ColorId::BayerRGGB));
    CheckDiagnosed(bayer, DiagnosisKind::OutOfFocus);
    CHECK(Number(bayer.diagnosis, "limb blur (pixels)") == Catch::Approx(20).epsilon(0.05));
    CHECK(Number(bayer.diagnosis, "target radius (pixels)") == Catch::Approx(120).epsilon(0.05));

    // No detail at all in any frame (quality 0) is out of focus whatever the target's size.
    std::vector<FrameStats> flat;
    for (int64_t i = 0; i < 50; ++i) {
        FrameStats s = Disc(i, 640, 480, 320, 240, 5, 1.0);
        s.quality = 0;
        flat.push_back(s);
    }
    const Selection selFlat = SelectFrames(flat, Info(640, 480));
    CheckDiagnosed(selFlat, DiagnosisKind::OutOfFocus);
    CHECK(Number(selFlat.diagnosis, "best quality") == 0);
}

TEST_CASE("diagnosis: poor seeing (varying blur, best frames still over the limit)", "[select][diagnosis]") {
    std::mt19937 rng(23);
    std::normal_distribution<double> n01(0, 1);
    // Median limb blur 20 luma px on R = 60, 30% log-normal spread: the best 1% are near 10 px.
    const auto stats = DiscCapture(1000, [&](int64_t) { return 20.0 * std::exp(0.3 * n01(rng)); });
    const Selection sel = SelectFrames(stats, Info(640, 480));
    CheckDiagnosed(sel, DiagnosisKind::PoorSeeing);
    CHECK(Number(sel.diagnosis, "blur / radius") >= stack::kBlurLimit);
    CHECK(Number(sel.diagnosis, "blur spread") >= stack::kSteadySpread);
    CHECK(Number(sel.diagnosis, "limb blur (pixels)") > 0);
}

TEST_CASE("a disc too small to judge focus on is stacked, not diagnosed", "[select][diagnosis]") {
    // Neptune-like: R = 5.5 luma px with a 2 px limb (0.36 R) -- sharp for its size.
    std::vector<FrameStats> stats;
    for (int64_t i = 0; i < 94; ++i) stats.push_back(Disc(i, 480, 360, 240, 180, 5.5, 2.0 + 0.01 * (i % 9)));
    const auto kept = CheckSelected(SelectFrames(stats, Info(480, 360)), stats);
    CHECK(kept.size() >= 5);
}

TEST_CASE("very short captures behave sensibly", "[select][short]") {
    const ser::FileInfo info = Info(640, 480);
    // One frame: it is the stack.
    {
        const auto stats = DiscCapture(1, [](int64_t) { return 1.0; });
        CHECK(SelectFrames(stats, info).frames == std::vector<int64_t>{0});
    }
    // Three alike frames: all of them.
    {
        const auto stats = DiscCapture(3, [](int64_t i) { return 1.0 + 0.01 * static_cast<double>(i); });
        CHECK(CheckSelected(SelectFrames(stats, info), stats).size() == 3);
    }
    // Four frames, one without a target: the other three.
    {
        auto stats = DiscCapture(4, [](int64_t) { return 1.0; });
        stats[2] = NoTargetFrame(2);
        CHECK(SelectFrames(stats, info).frames == std::vector<int64_t>{0, 1, 3});
    }
    // Twenty frames, one badly blurred: it is left out, the rest kept.
    {
        const auto stats = DiscCapture(20, [](int64_t i) { return i == 7 ? 4.0 : 1.0 + 0.01 * static_cast<double>(i % 3); });
        const auto kept = CheckSelected(SelectFrames(stats, info), stats);
        CHECK(kept.count(7) == 0);
        CHECK(kept.size() == 19);
    }
    // Thirty frames of a poor night: never fewer than kMinStackFrames.
    {
        std::mt19937 rng(29);
        std::normal_distribution<double> n01(0, 1);
        const auto stats = DiscCapture(30, [&](int64_t) { return 1.5 * std::exp(0.5 * n01(rng)); });
        const auto kept = CheckSelected(SelectFrames(stats, info), stats);
        CHECK(kept.size() >= static_cast<size_t>(stack::kMinStackFrames));
        CHECK(kept.size() < 30);
    }
    // Three frames, two without a target: too little to call it a capture of the target.
    {
        auto stats = DiscCapture(3, [](int64_t) { return 1.0; });
        stats[0] = NoTargetFrame(0);
        stats[1] = NoTargetFrame(1);
        CheckDiagnosed(SelectFrames(stats, info), DiagnosisKind::NoTarget);
    }
}

TEST_CASE("20 000 frames: same rule, fast", "[select][long]") {
    std::mt19937 rng(31);
    std::normal_distribution<double> n01(0, 1);
    auto good = [](int64_t i) { return (i / 50) % 3 == 0; };  // steady spells of 50 frames
    const auto stats = DiscCapture(20000, [&](int64_t i) {
        return good(i) ? 1.0 * (1 + 0.03 * n01(rng)) : 1.7 * (1 + 0.05 * n01(rng));
    });
    const auto t0 = std::chrono::steady_clock::now();
    const Selection sel = SelectFrames(stats, Info(640, 480));
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const auto kept = CheckSelected(sel, stats);
    int64_t nGood = 0;
    for (const auto& s : stats) nGood += good(s.index);
    for (int64_t f : kept) CHECK(good(f));
    CHECK(static_cast<double>(kept.size()) >= 0.9 * static_cast<double>(nGood));
    CHECK(secs < 1.0);
}

TEST_CASE("input order does not matter; the result is in frame order", "[select]") {
    std::mt19937 rng(37);
    std::normal_distribution<double> n01(0, 1);
    auto stats = DiscCapture(500, [&](int64_t) { return 1.2 * std::exp(0.2 * n01(rng)); });
    const Selection a = SelectFrames(stats, Info(640, 480));
    std::shuffle(stats.begin(), stats.end(), rng);
    const Selection b = SelectFrames(stats, Info(640, 480));
    CHECK(a.frames == b.frames);
    CHECK(a.threshold == b.threshold);
    CHECK(a.explanation == b.explanation);
}
