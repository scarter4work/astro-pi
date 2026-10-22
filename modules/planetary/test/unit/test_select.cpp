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

void CheckCountsAddUp(const Selection& sel, int64_t n);

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
    const std::regex re(R"(^used (\d+) of (\d+) frames: the frames judged sharpest after allowing for measurement noise, quality >= ([01]\.\d\d) of the best)");
    INFO(sel.explanation);
    REQUIRE(std::regex_search(sel.explanation, m, re));
    CHECK(std::stoll(m[1]) == static_cast<long long>(sel.frames.size()));
    CHECK(std::stoll(m[2]) == static_cast<long long>(stats.size()));
    CHECK(sel.explanation.find('\n') == std::string::npos);
    CheckCountsAddUp(sel, static_cast<int64_t>(stats.size()));
    return {sel.frames.begin(), sel.frames.end()};
}

// The explanation accounts for every frame: used + every "not used" group == n.
void CheckCountsAddUp(const Selection& sel, int64_t n) {
    INFO(sel.explanation);
    std::smatch m;
    REQUIRE(std::regex_search(sel.explanation, m, std::regex(R"(^used (\d+) of)")));
    int64_t total = std::stoll(m[1]);
    const auto pos = sel.explanation.find("not used: ");
    if (pos != std::string::npos) {
        const std::string tail = sel.explanation.substr(pos);
        const std::regex item(R"((\d+) (without a target|stray detections?|at the frame edge|clipped|outliers?|without detail|below the cut))");
        for (auto it = std::sregex_iterator(tail.begin(), tail.end(), item); it != std::sregex_iterator(); ++it)
            total += std::stoll((*it)[1]);
    }
    CHECK(total == n);
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
    CheckCountsAddUp(sel, 350);
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
    CHECK(Number(mono.diagnosis, "blur limit (pixels)") == Catch::Approx(60 * stack::kBlurLimit).epsilon(0.05));
    CHECK(mono.focusJudged);
    CHECK(Number(mono.diagnosis, "blur spread") < stack::kSteadySpread);
    // Clearly steady: the confident verdict, not "can't tell" -- even the upper bound is under 5%.
    CHECK(Number(mono.diagnosis, "blur spread upper bound") < stack::kSteadySpread);
    CHECK(mono.diagnosis.message.find("the optics, not the air") != std::string::npos);
    CHECK(mono.diagnosis.message.find("to tell") == std::string::npos);

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
    for (int64_t i = 0; i < 94; ++i) stats.push_back(Disc(i, 480, 360, 240, 180, 5.5, 2.0 + 0.01 * static_cast<double>(i % 9)));
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
    auto stats = DiscCapture(20000, [&](int64_t i) {
        return good(i) ? 1.0 * (1 + 0.03 * n01(rng)) : 1.7 * (1 + 0.05 * n01(rng));
    });
    // With 1% measurement noise reported, so the deconvolution (step 6) runs at full size.
    for (auto& s : stats) {
        s.quality *= 1 + 0.01 * n01(rng);
        s.qualitySigma = 0.01 * s.quality;
    }
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

// ---- review round 1 ------------------------------------------------------------------------------

namespace {
// A disc frame with a given quality and its 1-sigma measurement noise.
FrameStats DiscQ(int64_t i, double q, double qSigma) {
    FrameStats s = Disc(i, 640, 480, 320, 240, 60, 1.0);
    s.quality = q;
    s.qualitySigma = qSigma;
    return s;
}

// A satellite streak or plane: a bright line across the frame, its box spanning it edge to edge.
FrameStats Streak(int64_t i, int lw, int lh) {
    FrameStats s;
    s.index = i;
    s.targetFound = true;
    s.bx0 = 0;
    s.bx1 = lw;
    s.by0 = (static_cast<int>(i) * 37) % (lh - 20);
    s.by1 = s.by0 + 12;
    s.cx = lw / 2.0;
    s.cy = s.by0 + 6.0;
    s.quality = 2.5;
    s.qualitySigma = 0.01;
    return s;
}
}  // namespace

TEST_CASE("a small cluster of implausibly sharp frames is fenced off as outliers", "[select][outliers]") {
    std::mt19937 rng(41);
    std::normal_distribution<double> n01(0, 1);
    std::vector<FrameStats> stats;
    for (int64_t i = 0; i < 300; ++i) stats.push_back(DiscQ(i, 1.0 * (1 + 0.01 * n01(rng)), 0.0));
    for (int64_t i = 300; i < 305; ++i) stats.push_back(DiscQ(i, 1.3, 0.0));  // glitches
    const Selection sel = SelectFrames(stats, Info(640, 480));
    const auto kept = CheckSelected(sel, stats);
    for (int64_t f = 300; f < 305; ++f) CHECK(kept.count(f) == 0);
    CHECK(kept.size() >= 270);
    CHECK(sel.explanation.find("5 outliers") != std::string::npos);
}

TEST_CASE("a genuine lucky tail, or a real sharp mode, is not fenced", "[select][outliers]") {
    std::mt19937 rng(43);
    std::normal_distribution<double> n01(0, 1);
    // Poor night: log-normal blur, 30% spread -- its sharp tail is the point of lucky imaging.
    const auto poor = DiscCapture(3000, [&](int64_t) { return 1.5 * std::exp(0.3 * n01(rng)); });
    const Selection selPoor = SelectFrames(poor, Info(640, 480));
    CheckSelected(selPoor, poor);
    CHECK(selPoor.explanation.find("outlier") == std::string::npos);
    // The sharpest frame of the night is among those used.
    const auto best = std::max_element(poor.begin(), poor.end(),
                                       [](const FrameStats& a, const FrameStats& b) { return a.quality < b.quality; });
    CHECK(std::count(selPoor.frames.begin(), selPoor.frames.end(), best->index) == 1);
    // A sharp mode of 5% of the frames, far above the bulk (well past 6 MAD): it is the selection.
    const auto modes = DiscCapture(3000, [&](int64_t i) { return i % 20 == 0 ? 1.0 * (1 + 0.02 * n01(rng)) : 2.0 * (1 + 0.03 * n01(rng)); });
    const Selection selModes = SelectFrames(modes, Info(640, 480));
    const auto kept = CheckSelected(selModes, modes);
    for (int64_t f : kept) CHECK(f % 20 == 0);
    CHECK(kept.size() >= 135);
}

TEST_CASE("measurement noise alone does not drive the cut (Ruling P5)", "[select][noise]") {
    for (const double noise : {0.03, 0.05, 0.10}) {
        std::mt19937 rng(47);
        std::normal_distribution<double> n01(0, 1);
        // Identical blur in every frame; the quality scatter is the measurement noise it reports.
        std::vector<FrameStats> stats;
        for (int64_t i = 0; i < 2000; ++i) stats.push_back(DiscQ(i, 1.0 * (1 + noise * n01(rng)), noise));
        INFO("noise " << noise);
        const auto kept = CheckSelected(SelectFrames(stats, Info(640, 480)), stats);
        CHECK(kept.size() >= 1800);
    }
    // Neptune 2026-01-30: 94 frames, spread 0.059 of the mean, reported noise 0.070 -- all noise.
    {
        std::mt19937 rng(53);
        std::normal_distribution<double> n01(0, 1);
        std::vector<FrameStats> stats;
        for (int64_t i = 0; i < 94; ++i) stats.push_back(DiscQ(i, 0.58 * (1 + 0.059 * n01(rng)), 0.58 * 0.070));
        const auto kept = CheckSelected(SelectFrames(stats, Info(640, 480)), stats);
        CHECK(kept.size() >= 85);
    }
    // A real bimodal capture with the same 3% noise still keeps its sharp mode only.
    {
        std::mt19937 rng(59);
        std::normal_distribution<double> n01(0, 1);
        auto good = [](int64_t i) { return i % 10 < 3; };
        std::vector<FrameStats> stats;
        for (int64_t i = 0; i < 2000; ++i) {
            const double q = DiscQuality(60, good(i) ? 1.0 : 1.8);
            stats.push_back(DiscQ(i, q * (1 + 0.03 * n01(rng)), 0.03 * q));
        }
        const auto kept = CheckSelected(SelectFrames(stats, Info(640, 480)), stats);
        for (int64_t f : kept) CHECK(good(f));
        CHECK(kept.size() >= 540);
    }
}

TEST_CASE("small discs: sampling-corrected focus judgement with an absolute floor (Ruling P4)", "[select][diagnosis]") {
    const ser::FileInfo info = Info(480, 360);
    auto capture = [](double radius, double sigma, int64_t n, double jitter, uint32_t seed) {
        std::mt19937 rng(seed);
        std::normal_distribution<double> n01(0, 1);
        std::vector<FrameStats> v;
        for (int64_t i = 0; i < n; ++i) v.push_back(Disc(i, 480, 360, 240, 180, radius, sigma * (1 + jitter * n01(rng))));
        return v;
    };
    // A sharp small disc (R = 6 luma px, limb 0.6 px: mostly the sampling's own 0.5 px) is stacked.
    {
        const auto stats = capture(6, 0.6, 200, 0.02, 61);
        const Selection sel = SelectFrames(stats, info);
        CheckSelected(sel, stats);
        CHECK(sel.focusJudged);
    }
    // Over R/8 relative but under the absolute floor (1.5 px on R = 6): still stacked.
    {
        const auto stats = capture(6, 1.5, 200, 0.02, 67);
        CheckSelected(SelectFrames(stats, info), stats);
    }
    // A small disc smeared over 5 px in every frame: out of focus, numbers in sensor pixels.
    {
        const auto stats = capture(8, 5.0, 200, 0.005, 71);
        const Selection sel = SelectFrames(stats, info);
        CheckDiagnosed(sel, DiagnosisKind::OutOfFocus);
        CHECK(Number(sel.diagnosis, "limb blur (pixels)") == Catch::Approx(std::sqrt(25 - 0.25)).epsilon(0.05));
        CHECK(Number(sel.diagnosis, "blur limit (pixels)") == stack::kBlurFloorPixels);
        CHECK(sel.focusJudged);
    }
}

TEST_CASE("full-frame targets: focus is left to the region stage (Ruling P4)", "[select]") {
    // A blurred-looking full-frame quality is not judged here; focusJudged says so.
    std::vector<FrameStats> stats;
    for (int64_t i = 0; i < 300; ++i) {
        FrameStats s;
        s.index = i;
        s.targetFound = true;
        s.bx0 = 0;
        s.by0 = 0;
        s.bx1 = 640;
        s.by1 = 480;
        s.cx = 320;
        s.cy = 240;
        s.quality = 0.02;  // a texture scale of 50 px
        stats.push_back(s);
    }
    const Selection sel = SelectFrames(stats, Info(640, 480));
    CheckSelected(sel, stats);
    CHECK(sel.fullFrameTarget);
    CHECK_FALSE(sel.focusJudged);
}

TEST_CASE("count floors scale with the capture, stray detections do not count (review item 4)", "[select][diagnosis]") {
    // 20 000 frames of empty sky with 6 satellite streaks: no target, and not a full-frame one.
    {
        std::vector<FrameStats> stats;
        for (int64_t i = 0; i < 20000; ++i) stats.push_back(i % 3333 == 7 ? Streak(i, 640, 480) : NoTargetFrame(i));
        const Selection sel = SelectFrames(stats, Info(640, 480));
        CheckDiagnosed(sel, DiagnosisKind::NoTarget);
        CHECK_FALSE(sel.fullFrameTarget);
        CHECK(Number(sel.diagnosis, "frames") == 20000);
    }
    // 10 000 frames with a target in 60 (< sqrt(n) = 100): no target.
    {
        std::vector<FrameStats> stats;
        for (int64_t i = 0; i < 10000; ++i)
            stats.push_back(i % 166 == 0 ? Disc(i, 640, 480, 320, 240, 60, 1.0) : NoTargetFrame(i));
        CheckDiagnosed(SelectFrames(stats, Info(640, 480)), DiagnosisKind::NoTarget);
    }
    // 10 000 frames, the disc whole in only 60: the target leaves the frame.
    {
        std::vector<FrameStats> stats;
        for (int64_t i = 0; i < 10000; ++i) stats.push_back(Disc(i, 640, 480, i % 166 == 0 ? 320 : 30, 240, 60, 1.0));
        CheckDiagnosed(SelectFrames(stats, Info(640, 480)), DiagnosisKind::TargetLeavesFrame);
    }
    // 10 000 frames, unclipped in only 60: clipped.
    {
        std::vector<FrameStats> stats;
        for (int64_t i = 0; i < 10000; ++i) {
            FrameStats s = Disc(i, 640, 480, 320, 240, 60, 1.0);
            s.clippedFraction = i % 166 == 0 ? 0 : 0.5;
            stats.push_back(s);
        }
        CheckDiagnosed(SelectFrames(stats, Info(640, 480)), DiagnosisKind::Clipped);
    }
    // A planet capture with 3 streak frames: the streaks are stray detections, never selected.
    {
        std::vector<FrameStats> stats;
        for (int64_t i = 0; i < 500; ++i) stats.push_back(i % 150 == 1 ? Streak(i, 640, 480) : Disc(i, 640, 480, 320, 240, 60, 1.0));
        const Selection sel = SelectFrames(stats, Info(640, 480));
        const auto kept = CheckSelected(sel, stats);
        for (int64_t f : kept) CHECK(f % 150 != 1);
        CHECK(sel.explanation.find("4 stray detections") != std::string::npos);
        CHECK_FALSE(sel.fullFrameTarget);
    }
}

TEST_CASE("frames without detail (quality 0) are left out, not a reason to refuse (review item 5)", "[select]") {
    // Three frames (1, 1, 0): the two with detail.
    {
        auto stats = DiscCapture(3, [](int64_t) { return 1.0; });
        stats[2].quality = 0;
        const Selection sel = SelectFrames(stats, Info(640, 480));
        CHECK(sel.frames == std::vector<int64_t>{0, 1});
        CHECK(sel.explanation.find("1 without detail") != std::string::npos);
        CheckCountsAddUp(sel, 3);
    }
    // Ten frames, three with detail (< need = 5): out of focus.
    {
        auto stats = DiscCapture(10, [](int64_t) { return 1.0; });
        for (int64_t i = 3; i < 10; ++i) stats[static_cast<size_t>(i)].quality = 0;
        const Selection sel = SelectFrames(stats, Info(640, 480));
        CheckDiagnosed(sel, DiagnosisKind::OutOfFocus);
        CHECK(Number(sel.diagnosis, "frames with detail") == 3);
    }
}

TEST_CASE("few blurred frames: focus or seeing is worded as undetermined (review item 6)", "[select][diagnosis]") {
    std::mt19937 rng(73);
    std::normal_distribution<double> n01(0, 1);
    const auto stats = DiscCapture(20, [&](int64_t) { return 20.0 * std::exp(0.3 * n01(rng)); });
    const Selection sel = SelectFrames(stats, Info(640, 480));
    CheckDiagnosed(sel, DiagnosisKind::OutOfFocus);
    CHECK(sel.diagnosis.message.find("too few frames to tell") != std::string::npos);
    CHECK(Number(sel.diagnosis, "frames judged") == 20);
}

// ---- review round 2 ------------------------------------------------------------------------------

namespace {
// The reviewer's oracle: a capture whose true limb blur per frame is known. Score = the detail
// signal-to-noise S of a frame set computed with the TRUE blurs, relative to the best set an
// oracle (knowing the true blurs) would pick with the same rule.
struct Oracle {
    std::vector<double> sigma;  // true limb blur per frame
    std::vector<FrameStats> stats;
};

Oracle VaryingNight(double g, double noise, uint32_t seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> n01(0, 1);
    Oracle o;
    for (int64_t i = 0; i < 3000; ++i) {
        const double sig = 2.0 * std::exp(g * n01(rng));
        const double qt = DiscQuality(60, sig);
        o.sigma.push_back(sig);
        o.stats.push_back(DiscQ(i, qt * (1 + noise * n01(rng)), noise * qt));
    }
    return o;
}

double OracleRelativeScore(const Oracle& o, const std::vector<int64_t>& set) {
    const size_t n = o.sigma.size();
    std::vector<double> sorted = o.sigma;
    std::sort(sorted.begin(), sorted.end());
    const double ref = sorted[(n + 99) / 100 - 1];
    auto w = [&](double sig) { return std::exp(-3 * std::max(1.0, (sig / ref) * (sig / ref))); };
    double sum = 0, best = -1;
    for (size_t k = 1; k <= n; ++k) {
        sum += w(sorted[k - 1]);
        best = std::max(best, sum / std::sqrt(static_cast<double>(k)));
    }
    double s = 0;
    for (int64_t f : set) s += w(o.sigma[static_cast<size_t>(f)]);
    return s / std::sqrt(static_cast<double>(set.size())) / best;
}
}  // namespace

TEST_CASE("varying nights with measurement noise: close to the oracle's choice (N1)", "[select][noise]") {
    // Blur log-sd g, quality noise. Ten seeds per case: the bars are on the mean (measured 10-seed
    // mean minus 0.02) with a looser per-seed minimum (measured minimum minus 0.03), so the test
    // states a property of the rule, not one lucky draw. Measured 2026-10-21 (EM budget 300):
    // mean .967 / .841 / .644, minimum .951 / .815 / .602.
    struct Case {
        double g, noise, meanBar, minBar;
    };
    for (const Case c : {Case{0.2, 0.02, 0.947, 0.92}, Case{0.2, 0.05, 0.82, 0.785}, Case{0.3, 0.10, 0.62, 0.57}}) {
        double sum = 0, lowest = 1;
        for (uint32_t seed = 1; seed <= 10; ++seed) {
            const Oracle o = VaryingNight(c.g, c.noise, seed);
            const Selection sel = SelectFrames(o.stats, Info(640, 480));
            CheckSelected(sel, o.stats);
            const double score = OracleRelativeScore(o, sel.frames);
            sum += score;
            lowest = std::min(lowest, score);
        }
        INFO("g " << c.g << " noise " << c.noise << " mean " << sum / 10 << " min " << lowest);
        CHECK(sum / 10 >= c.meanBar);
        CHECK(lowest >= c.minBar);
    }
}

TEST_CASE("frames with detail must reach the count floor too (N2)", "[select][diagnosis]") {
    std::vector<FrameStats> stats;
    for (int64_t i = 0; i < 20000; ++i) {
        FrameStats s = Disc(i, 640, 480, 320, 240, 60, 1.0);
        if (i >= 5) s.quality = 0;
        stats.push_back(s);
    }
    const Selection sel = SelectFrames(stats, Info(640, 480));
    CheckDiagnosed(sel, DiagnosisKind::OutOfFocus);
    CHECK(Number(sel.diagnosis, "frames with detail") == 5);
}

TEST_CASE("streaks are never a frame-filling target, even as the majority of detections (N3)", "[select]") {
    std::vector<FrameStats> stats;
    for (int64_t i = 0; i < 2000; ++i) stats.push_back(i % 30 == 0 ? Streak(i, 640, 480) : NoTargetFrame(i));
    const Selection sel = SelectFrames(stats, Info(640, 480));
    CHECK(sel.frames.empty());
    CHECK_FALSE(sel.fullFrameTarget);
    CHECK(sel.diagnosis.kind != DiagnosisKind::None);
    // The per-frame rule shared with Align: a frame-wide thin box does not fill the frame.
    CHECK_FALSE(stack::FillsFrame(Streak(0, 640, 480), 640, 480));
    FrameStats moon;
    moon.targetFound = true;
    moon.bx0 = 330;
    moon.by0 = 0;
    moon.bx1 = 640;
    moon.by1 = 321;
    CHECK(stack::FillsFrame(moon, 640, 480));
}

TEST_CASE("short captures are not fenced: their sharpest frames stay (N4)", "[select][outliers]") {
    auto capture = [](const std::vector<double>& qs) {
        std::vector<FrameStats> v;
        for (size_t i = 0; i < qs.size(); ++i) v.push_back(DiscQ(static_cast<int64_t>(i), qs[i], 0.0));
        return v;
    };
    {
        const auto stats = capture({1.0, 1.0, 1.01, 1.3, 1.32});
        const Selection sel = SelectFrames(stats, Info(640, 480));
        const auto kept = CheckSelected(sel, stats);
        CHECK(kept.count(3) == 1);
        CHECK(kept.count(4) == 1);
        CHECK(sel.explanation.find("outlier") == std::string::npos);
    }
    {
        const auto stats = capture({1.0, 1.01, 1.02, 1.0, 1.01, 1.0, 1.02, 1.25, 1.26, 1.27});
        const Selection sel = SelectFrames(stats, Info(640, 480));
        const auto kept = CheckSelected(sel, stats);
        for (int64_t f : {7, 8, 9}) CHECK(kept.count(f) == 1);
    }
    // 100 frames, 99 identical and one 0.1% sharper: no outlier (differences under the floor).
    {
        std::vector<double> qs(100, 1.0);
        qs[42] = 1.001;
        const Selection sel = SelectFrames(capture(qs), Info(640, 480));
        CHECK(sel.explanation.find("outlier") == std::string::npos);
    }
}

TEST_CASE("focus or seeing: a verdict only when the noise allows one (N5)", "[select][diagnosis]") {
    // Blurred disc (limb 10 px on R = 60), 500 frames, true blur spread g, quality noise `noise`.
    auto capture = [](double g, double noise, uint32_t seed) {
        std::mt19937 rng(seed);
        std::normal_distribution<double> n01(0, 1);
        std::vector<FrameStats> v;
        for (int64_t i = 0; i < 500; ++i) {
            const double qt = DiscQuality(60, 10.0 * (1 + g * n01(rng)));
            v.push_back(DiscQ(i, qt * (1 + noise * n01(rng)), noise * qt));
        }
        return v;
    };
    auto cantTell = [](const Selection& sel) { return sel.diagnosis.message.find("to tell") != std::string::npos; };
    // Steady blur (g = 0) under 10% noise, 50 seeds: never called seeing (at most 5% of seeds).
    {
        int poorSeeing = 0;
        for (uint32_t seed = 1; seed <= 50; ++seed) {
            const Selection sel = SelectFrames(capture(0, 0.10, seed), Info(640, 480));
            REQUIRE(sel.diagnosis.kind != DiagnosisKind::None);
            poorSeeing += sel.diagnosis.kind == DiagnosisKind::PoorSeeing;
        }
        CHECK(poorSeeing <= 2);
    }
    // Clearly steady -- steady blur under 1% noise, 50 seeds: the confident "optics" verdict in
    // every seed, its upper bound under kSteadySpread. (At 3% noise and 500 frames the 2-SE bound
    // alone is ~5.5% of blur spread, so "can't tell" is the honest verdict there.)
    for (uint32_t seed = 1; seed <= 50; ++seed) {
        const Selection sel = SelectFrames(capture(0, 0.01, seed), Info(640, 480));
        INFO("seed " << seed << ": " << sel.diagnosis.message);
        REQUIRE(sel.diagnosis.kind == DiagnosisKind::OutOfFocus);
        CHECK(sel.diagnosis.message.find("the optics, not the air") != std::string::npos);
        CHECK(Number(sel.diagnosis, "blur spread upper bound") < stack::kSteadySpread);
    }
    // 6% seeing under 3% noise is measurable: PoorSeeing in (nearly) every seed.
    {
        int poorSeeing = 0;
        for (uint32_t seed = 1; seed <= 50; ++seed)
            poorSeeing += SelectFrames(capture(0.06, 0.03, seed), Info(640, 480)).diagnosis.kind == DiagnosisKind::PoorSeeing;
        CHECK(poorSeeing >= 48);
    }
    // 6% seeing under 10% noise (seed 13): the 2-SE bounds straddle the 5% limit -- "can't tell",
    // the honest verdict (the point estimate alone said seeing).
    {
        const Selection sel = SelectFrames(capture(0.06, 0.10, 13), Info(640, 480));
        CheckDiagnosed(sel, DiagnosisKind::OutOfFocus);
        INFO(sel.diagnosis.message);
        CHECK(cantTell(sel));
        CHECK(Number(sel.diagnosis, "blur spread lower bound") < stack::kSteadySpread);
        CHECK(Number(sel.diagnosis, "blur spread upper bound") >= stack::kSteadySpread);
    }

    // Heavy noise and no measurable spread (scatter exactly the reported 15%: tau^2 = 0), yet a
    // spread over 5% cannot be ruled out: neither "steady" (focus) nor "varying" -- can't tell.
    std::vector<FrameStats> noisy;
    const double qt = DiscQuality(60, 10.0);
    for (int64_t i = 0; i < 500; ++i) noisy.push_back(DiscQ(i, qt * (i % 2 ? 1.15 : 0.85), 0.15 * qt));
    const Selection selNoisy = SelectFrames(noisy, Info(640, 480));
    CheckDiagnosed(selNoisy, DiagnosisKind::OutOfFocus);
    INFO(selNoisy.diagnosis.message);
    CHECK(Number(selNoisy.diagnosis, "blur spread") < stack::kSteadySpread);
    CHECK(Number(selNoisy.diagnosis, "blur spread upper bound") >= stack::kSteadySpread);
    CHECK(selNoisy.diagnosis.message.find("too large to tell") != std::string::npos);
}

TEST_CASE("lunar limb and crescent slivers are frame-filling; streaks are not (A)", "[select]") {
    // Full height, touching the right edge, 9% / 16% / 23% of the width: kept as frame-filling.
    for (const int w : {60, 100, 150}) {
        std::mt19937 rng(3);
        std::normal_distribution<double> n01(0, 1);
        std::vector<FrameStats> stats;
        for (int64_t i = 0; i < 2000; ++i) {
            FrameStats s;
            s.index = i;
            s.targetFound = true;
            s.bx0 = 640 - w;
            s.bx1 = 640;
            s.by0 = 0;
            s.by1 = 480;
            s.cx = 640 - w / 2.0;
            s.cy = 240;
            s.quality = 0.3 * (1 + 0.01 * n01(rng));
            s.qualitySigma = 0.003;
            stats.push_back(s);
        }
        INFO("sliver width " << w);
        const Selection sel = SelectFrames(stats, Info(640, 480));
        const auto kept = CheckSelected(sel, stats);
        CHECK(sel.fullFrameTarget);
        CHECK(kept.size() >= 1800);
    }
    // A trail's box (2.5% of the frame across) never fills it.
    CHECK_FALSE(stack::FillsFrame(Streak(0, 640, 480), 640, 480));
    FrameStats sliver;
    sliver.targetFound = true;
    sliver.bx0 = 580;
    sliver.bx1 = 640;
    sliver.by0 = 0;
    sliver.by1 = 480;
    CHECK(stack::FillsFrame(sliver, 640, 480));
}

TEST_CASE("the fence never cuts the top of a real sharp mode (E)", "[select][outliers]") {
    // 600 frames 30% sharper than 2400 others (3% spread each, 2% noise): the sharp mode is the
    // selection, its sharpest frames included -- no outliers.
    std::mt19937 rng(5);
    std::normal_distribution<double> n01(0, 1);
    std::vector<FrameStats> stats;
    for (int64_t i = 0; i < 3000; ++i) {
        const double qt = (i < 600 ? 1.3 : 1.0) * (1 + 0.03 * n01(rng));
        stats.push_back(DiscQ(i, qt * (1 + 0.02 * n01(rng)), 0.02 * qt));
    }
    const Selection sel = SelectFrames(stats, Info(640, 480));
    const auto kept = CheckSelected(sel, stats);
    CHECK(sel.explanation.find("outlier") == std::string::npos);
    for (int64_t f : kept) CHECK(f < 600);
    CHECK(kept.size() >= 540);
    const auto best = std::max_element(stats.begin(), stats.end(),
                                       [](const FrameStats& x, const FrameStats& y) { return x.quality < y.quality; });
    CHECK(kept.count(best->index) == 1);
}

// ---- review round 3 ------------------------------------------------------------------------------

namespace {
// 3000 bulk frames (true quality 1, spread `spread`) and m glitch frames log-uniform in [lo, hi],
// all measured with relative noise `noise` (reported). Frames >= 3000 are the glitches.
std::vector<FrameStats> BulkAndGlitches(int m, double lo, double hi, double spread, double noise, uint32_t seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> n01(0, 1);
    std::uniform_real_distribution<double> u(std::log(lo), std::log(hi));
    std::vector<FrameStats> v;
    for (int64_t i = 0; i < 3000; ++i) {
        const double qt = 1.0 * (1 + spread * n01(rng));
        v.push_back(DiscQ(i, qt * (1 + noise * n01(rng)), noise * qt));
    }
    for (int64_t i = 0; i < m; ++i) {
        const double q = std::exp(u(rng));
        v.push_back(DiscQ(3000 + i, q, noise * q));
    }
    return v;
}
}  // namespace

TEST_CASE("glitch clusters that reach down to the bulk are fenced, never the selection (round 4)", "[select][outliers]") {
    // Glitches spread from just above the bulk to far above it shield one another from the gap
    // rule (no 2-sd gap anywhere); the frames under the fence must still outnumber those above it
    // for the latter to be the top of a real population. 20 seeds per case.
    struct Case {
        int m;
        double lo, hi, spread, noise;
    };
    for (const Case c : {Case{50, 1.05, 1.6, 0.01, 0.01}, Case{40, 1.1, 1.5, 0.02, 0.02}}) {
        for (uint32_t seed = 1; seed <= 20; ++seed) {
            const auto stats = BulkAndGlitches(c.m, c.lo, c.hi, c.spread, c.noise, seed);
            const Selection sel = SelectFrames(stats, Info(640, 480));
            INFO("glitches " << c.m << " in [" << c.lo << ", " << c.hi << "], seed " << seed << ": " << sel.explanation);
            const auto kept = CheckSelected(sel, stats);
            int64_t keptBulk = 0;
            for (int64_t f : kept) {
                keptBulk += f < 3000;
                CHECK(stats[static_cast<size_t>(f)].quality < 1.25);  // nothing far above the bulk
            }
            CHECK(keptBulk >= 2700);
            CHECK(sel.explanation.find("outlier") != std::string::npos);
        }
    }
}

TEST_CASE("a ladder of glitches starting under the fence is fenced (round 4)", "[select][outliers]") {
    // 300 frames (1% spread, no noise reported) and 10 frames climbing from 5 sd above the median in
    // steps of 1.9 sd: no step is a 2-sd gap, and the lowest rung sits under the fence.
    for (uint32_t seed = 41; seed <= 45; ++seed) {
        std::mt19937 rng(seed);
        std::normal_distribution<double> n01(0, 1);
        std::vector<FrameStats> stats;
        for (int64_t i = 0; i < 300; ++i) stats.push_back(DiscQ(i, 1.0 * (1 + 0.01 * n01(rng)), 0.0));
        for (int64_t i = 0; i < 10; ++i) stats.push_back(DiscQ(300 + i, std::exp(0.05 + 0.019 * static_cast<double>(i)), 0.0));
        const Selection sel = SelectFrames(stats, Info(640, 480));
        INFO("seed " << seed << ": " << sel.explanation);
        const auto kept = CheckSelected(sel, stats);
        int64_t keptBulk = 0, keptRungs = 0;
        for (int64_t f : kept) {
            keptBulk += f < 300;
            keptRungs += f >= 300;
            CHECK(f < 302);  // rungs 2.. (7 sd and up) are over any fence these 300 frames give
        }
        CHECK(keptBulk >= 270);
        CHECK(keptRungs <= 2);
        CHECK(sel.explanation.find("outlier") != std::string::npos);
    }
}

namespace {
// A satellite or plane trail tilted by `degrees` across the full width of a lw x lh luma frame:
// its box is the line's vertical run plus the line width (3 px) plus one detection block per side.
FrameStats TiltedTrail(int64_t i, int lw, int lh, double degrees) {
    const int d = stack::DetectionBlock(lw, lh);
    const int run = static_cast<int>(std::ceil(lw * std::tan(degrees * M_PI / 180))) + 3 + 2 * d;
    FrameStats s;
    s.index = i;
    s.targetFound = true;
    s.bx0 = 0;
    s.bx1 = lw;
    s.by0 = (static_cast<int>(i) * 37) % (lh - run);
    s.by1 = s.by0 + run;
    s.cx = lw / 2.0;
    s.cy = s.by0 + run / 2.0;
    s.quality = 2.5;
    s.qualitySigma = 0.01;
    return s;
}
}  // namespace

TEST_CASE("tilted trails are never a frame-filling target (round 4)", "[select]") {
    // Tilted 1.5-4 degrees, a trail's box is 5.5-12% of a 640 x 480 frame across -- as wide as a
    // thin lunar sliver's, so the box shape alone cannot refuse it.
    std::vector<FrameStats> stats;
    for (int64_t i = 0; i < 2000; ++i)
        stats.push_back(i % 30 == 0 ? TiltedTrail(i, 640, 480, 1.5 + 2.5 * static_cast<double>(i % 7) / 6) : NoTargetFrame(i));
    CHECK(stack::FillsFrame(TiltedTrail(0, 640, 480, 1.5), 640, 480));  // per frame it can pass
    const Selection sel = SelectFrames(stats, Info(640, 480));
    INFO(sel.explanation);
    CHECK_FALSE(sel.fullFrameTarget);
    CHECK(sel.frames.empty());
    CHECK(sel.diagnosis.kind != DiagnosisKind::None);
}
