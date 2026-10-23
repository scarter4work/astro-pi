#include <catch_amalgamated.hpp>

#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <functional>
#include <limits>
#include <random>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include "Align.h"
#include "Measure.h"
#include "Select.h"
#include "StackSynth.h"

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
    s.halfRadius = radius;  // the disc's own radius: its limb crosses half contrast there
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

// Instructions retired by this thread, user space only, while `f` runs (perf_event_open; Linux allows
// it for one's own thread at perf_event_paranoid <= 2). available false, with the reason, where the
// kernel or a container gives no hardware counter.
struct InstructionCount {
    bool available = false;
    uint64_t count = 0;
    std::string error;
};
InstructionCount Instructions(const std::function<void()>& f) {
    perf_event_attr attr{};
    attr.type = PERF_TYPE_HARDWARE;
    attr.size = sizeof attr;
    attr.config = PERF_COUNT_HW_INSTRUCTIONS;
    attr.disabled = 1;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    InstructionCount out;
    const int fd = static_cast<int>(syscall(SYS_perf_event_open, &attr, 0, -1, -1, 0));
    if (fd < 0) {
        out.error = std::strerror(errno);
        f();
        return out;
    }
    ioctl(fd, PERF_EVENT_IOC_RESET, 0);
    ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
    f();
    ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
    uint64_t v = 0;
    out.available = read(fd, &v, sizeof v) == static_cast<ssize_t>(sizeof v);
    if (!out.available) out.error = "short read of the counter";
    out.count = v;
    close(fd);
    return out;
}
// The 20 000- over the 5 000-frame instruction count of the long-capture test may not exceed this
// (measured 4.003, idle or under 32 memcpy-bandwidth processes; the counts repeat to within ~20
// instructions in 4.2e9 idle and ~3 000 under that load: deterministic, so the
// margin is small).
constexpr double kLinearInstructionRatio = 4.2;

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

// The explanation accounts for every frame: used + every "not used" group == n. The "not used"
// list ends at the next "; " (the suspects' outcome and the frame-filling note follow it).
void CheckCountsAddUp(const std::string& explanation, int64_t n) {
    INFO(explanation);
    std::smatch m;
    REQUIRE(std::regex_search(explanation, m, std::regex(R"(^used (\d+) of)")));
    int64_t total = std::stoll(m[1]);
    const auto pos = explanation.find("not used: ");
    if (pos != std::string::npos) {
        std::string tail = explanation.substr(pos);
        tail = tail.substr(0, tail.find("; "));
        const std::regex item(
            R"((\d+) (without a target|stray detections?|at the frame edge|clipped|without detail|below the cut|suspect frames? \(alignment decides\)|rejected by alignment))");
        for (auto it = std::sregex_iterator(tail.begin(), tail.end(), item); it != std::sregex_iterator(); ++it)
            total += std::stoll((*it)[1]);
    }
    CHECK(total == n);
}

void CheckCountsAddUp(const Selection& sel, int64_t n) { CheckCountsAddUp(sel.explanation, n); }

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
    // R = 60 luma px, limb blur 20 luma px (1/3 of R: over kGrossBlurLimit) in every frame, 1% jitter.
    const auto stats = DiscCapture(500, [&](int64_t) { return 20.0 * (1 + 0.01 * n01(rng)); });
    const Selection mono = SelectFrames(stats, Info(640, 480));
    CheckDiagnosed(mono, DiagnosisKind::OutOfFocus);
    CHECK(Number(mono.diagnosis, "limb blur (pixels)") == Catch::Approx(20).epsilon(0.05));
    CHECK(Number(mono.diagnosis, "target radius (pixels)") == Catch::Approx(60).epsilon(0.05));
    CHECK(Number(mono.diagnosis, "blur / radius") == Catch::Approx(1.0 / 3).epsilon(0.05));
    CHECK(Number(mono.diagnosis, "blur limit (pixels)") == Catch::Approx(60 * stack::kGrossBlurLimit).epsilon(0.05));
    CHECK(mono.focusJudged);
    CHECK(Number(mono.diagnosis, "blur spread") < stack::kSteadySpread);
    // Clearly steady: the confident verdict, not "can't tell" -- even the upper bound is under 5%.
    CHECK(Number(mono.diagnosis, "blur spread upper bound") < stack::kSteadySpread);
    CHECK(mono.diagnosis.message.find("the optics, not the air") != std::string::npos);
    CHECK(mono.diagnosis.message.find("to tell") == std::string::npos);

    // The same luma statistics from a Bayer capture: pixels are reported at sensor scale.
    const Selection bayer = SelectFrames(stats, Info(1280, 960, ser::ColorId::BayerRGGB));
    CheckDiagnosed(bayer, DiagnosisKind::OutOfFocus);
    CHECK(Number(bayer.diagnosis, "limb blur (pixels)") == Catch::Approx(40).epsilon(0.05));
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
    // Median limb blur 40 luma px on R = 60, 30% log-normal spread: the best 1% are near 20 px.
    const auto stats = DiscCapture(1000, [&](int64_t) { return 40.0 * std::exp(0.3 * n01(rng)); });
    const Selection sel = SelectFrames(stats, Info(640, 480));
    CheckDiagnosed(sel, DiagnosisKind::PoorSeeing);
    CHECK(Number(sel.diagnosis, "blur / radius") >= stack::kGrossBlurLimit);
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

TEST_CASE("20 000 frames: same rule, linear cost", "[select][long]") {
    auto good = [](int64_t i) { return (i / 50) % 3 == 0; };  // steady spells of 50 frames
    auto capture = [&](int64_t n) {
        std::mt19937 rng(31);
        std::normal_distribution<double> n01(0, 1);
        auto stats = DiscCapture(n, [&](int64_t i) {
            return good(i) ? 1.0 * (1 + 0.03 * n01(rng)) : 1.7 * (1 + 0.05 * n01(rng));
        });
        // With 1% measurement noise reported, so the deconvolution (step 6) runs at full size.
        for (auto& s : stats) {
            s.quality *= 1 + 0.01 * n01(rng);
            s.qualitySigma = 0.01 * s.quality;
        }
        return stats;
    };
    const auto stats = capture(20000), quarter = capture(5000);
    const Selection sel = SelectFrames(stats, Info(640, 480));
    const auto kept = CheckSelected(sel, stats);
    int64_t nGood = 0;
    for (const auto& s : stats) nGood += good(s.index);
    for (int64_t f : kept) CHECK(good(f));
    CHECK(static_cast<double>(kept.size()) >= 0.9 * static_cast<double>(nGood));

    CHECK(SelectFrames(stats, Info(640, 480)).frames == sel.frames);  // deterministic

    // Cost, without a clock that load can stretch: CPU time includes memory stalls, and under
    // memory-bandwidth load (review: 32 processes copying 64 MB) the 20 000-frame working set spills
    // cache where the 5 000 one does not -- 2.6-3.1 s CPU against 0.40-0.43 s idle, ratio 8.6-12.1
    // against 3.9-4.1.
    // (1) RankByDetail's own count of its grid evaluations (steps 6 and 8, where nearly all the cost
    // is), on every frame's quality passed to it directly: exactly the likelihood table plus
    // kPriorIterations EM steps plus the posteriors, kPriorGrid each per sample -- 4x the frames, 4x.
    // It is counted inside the loops it measures, so it pins those loops' shape, nothing else.
    auto work = [](const std::vector<FrameStats>& s) {
        std::vector<double> q, sg;
        for (const FrameStats& f : s) {
            q.push_back(f.quality);
            sg.push_back(f.qualitySigma);
        }
        const auto n = static_cast<int64_t>(s.size());
        const auto floorCount = static_cast<int64_t>(std::ceil(std::sqrt(static_cast<double>(n))));
        const stack::DetailRanking r = stack::RankByDetail(q, sg, stack::kMinStackFrames, floorCount);
        return std::make_pair(r.work, r.left.size());
    };
    const auto [full, fullLeft] = work(stats);
    const auto [part, partLeft] = work(quarter);
    INFO("grid evaluations: 20 000 frames " << full << " (" << fullLeft << " ranked), 5 000 frames " << part << " ("
                                            << partLeft << ")");
    const auto perSample = static_cast<uint64_t>(stack::kPriorGrid) * (2 * stack::kPriorIterations + 2);
    CHECK(full == perSample * fullLeft);
    CHECK(part == perSample * partLeft);
    CHECK(static_cast<double>(full) / static_cast<double>(part) == Catch::Approx(4).epsilon(0.01));

    // (2) The whole of SelectFrames, by the instructions this thread retires (a hardware counter,
    // user space only): the same input runs the same code, so the count is the same under any load --
    // memory stalls, other processes and cache misses retire no instructions. 4x the frames retires
    // 4.003x the instructions; any step superlinear in N anywhere in SelectFrames raises it
    // (an O(N^2) pass of a few instructions per pair adds ~1.6e9 at 20 000 frames). The bound is the
    // measured ratio plus a small margin, safe because the count is deterministic.
    const InstructionCount full20 = Instructions([&] { (void)SelectFrames(stats, Info(640, 480)); });
    const InstructionCount part5 = Instructions([&] { (void)SelectFrames(quarter, Info(640, 480)); });
    if (full20.available && part5.available) {
        const double ratio = static_cast<double>(full20.count) / static_cast<double>(part5.count);
        INFO("instructions retired: 20 000 frames " << full20.count << ", 5 000 frames " << part5.count << " (ratio "
                                                    << ratio << ")");
        CHECK(ratio < kLinearInstructionRatio);
    } else {
        WARN("no hardware instruction counter here (perf_event_open: " << full20.error
                                                                       << "); the superlinear check is the CPU ceiling alone");
    }
    // (3) A ceiling on this thread's CPU for the 20 000-frame selection, ~10x the most measured under
    // memory-bandwidth load (2.6-3.1 s; 0.40-0.43 s idle): never tripped by load, only by a change
    // that costs an order of magnitude -- the backstop where (2) has no counter.
    timespec a{}, b{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &a);
    (void)SelectFrames(stats, Info(640, 480));
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &b);
    const double cpu = static_cast<double>(b.tv_sec - a.tv_sec) + 1e-9 * static_cast<double>(b.tv_nsec - a.tv_nsec);
    INFO("CPU for 20 000 frames: " << cpu << " s");
    CHECK(cpu < 30.0);
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

// ---- fix round 1: review findings 1-9 ------------------------------------------------------------

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

TEST_CASE("a small cluster of implausibly sharp frames is fenced off as suspects", "[select][outliers]") {
    // Round 5 (Ruling P6): fenced frames are suspects held for the alignment check, not "outliers".
    std::mt19937 rng(41);
    std::normal_distribution<double> n01(0, 1);
    std::vector<FrameStats> stats;
    for (int64_t i = 0; i < 300; ++i) stats.push_back(DiscQ(i, 1.0 * (1 + 0.01 * n01(rng)), 0.0));
    for (int64_t i = 300; i < 305; ++i) stats.push_back(DiscQ(i, 1.3, 0.0));  // glitches
    const Selection sel = SelectFrames(stats, Info(640, 480));
    const auto kept = CheckSelected(sel, stats);
    for (int64_t f = 300; f < 305; ++f) CHECK(kept.count(f) == 0);
    CHECK(kept.size() >= 270);
    CHECK(sel.explanation.find("5 suspect frames (alignment decides)") != std::string::npos);
    REQUIRE(sel.suspects.size() == 5);
    for (size_t k = 0; k < 5; ++k) {
        CHECK(sel.suspects[k].index == 300 + static_cast<int64_t>(k));
        CHECK(sel.suspects[k].quality == 1.3);
        CHECK_FALSE(sel.suspects[k].reason.empty());
    }
}

TEST_CASE("a genuine lucky tail, or a real sharp mode, is not fenced", "[select][outliers]") {
    std::mt19937 rng(43);
    std::normal_distribution<double> n01(0, 1);
    // Poor night: log-normal blur, 30% spread -- its sharp tail is the point of lucky imaging.
    const auto poor = DiscCapture(3000, [&](int64_t) { return 1.5 * std::exp(0.3 * n01(rng)); });
    const Selection selPoor = SelectFrames(poor, Info(640, 480));
    CheckSelected(selPoor, poor);
    CHECK(selPoor.suspects.empty());
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
    const auto stats = DiscCapture(20, [&](int64_t) { return 40.0 * std::exp(0.3 * n01(rng)); });
    const Selection sel = SelectFrames(stats, Info(640, 480));
    CheckDiagnosed(sel, DiagnosisKind::OutOfFocus);
    CHECK(sel.diagnosis.message.find("too few frames to tell") != std::string::npos);
    CHECK(Number(sel.diagnosis, "frames judged") == 20);
}

// ---- fix round 2: re-review 1 (N1-N6) ------------------------------------------------------------

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
        CHECK(sel.suspects.empty());
    }
    {
        const auto stats = capture({1.0, 1.01, 1.02, 1.0, 1.01, 1.0, 1.02, 1.25, 1.26, 1.27});
        const Selection sel = SelectFrames(stats, Info(640, 480));
        const auto kept = CheckSelected(sel, stats);
        for (int64_t f : {7, 8, 9}) CHECK(kept.count(f) == 1);
    }
    // 100 frames, 99 identical and one 0.1% sharper: no suspect (differences under the floor).
    {
        std::vector<double> qs(100, 1.0);
        qs[42] = 1.001;
        const Selection sel = SelectFrames(capture(qs), Info(640, 480));
        CHECK(sel.suspects.empty());
    }
}

TEST_CASE("focus or seeing: a verdict only when the noise allows one (N5)", "[select][diagnosis]") {
    // Blurred disc (limb 20 px on R = 60), 500 frames, true blur spread g, quality noise `noise`.
    auto capture = [](double g, double noise, uint32_t seed) {
        std::mt19937 rng(seed);
        std::normal_distribution<double> n01(0, 1);
        std::vector<FrameStats> v;
        for (int64_t i = 0; i < 500; ++i) {
            const double qt = DiscQuality(60, 20.0 * (1 + g * n01(rng)));
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
    const double qt = DiscQuality(60, 20.0);
    for (int64_t i = 0; i < 500; ++i) noisy.push_back(DiscQ(i, qt * (i % 2 ? 1.15 : 0.85), 0.15 * qt));
    const Selection selNoisy = SelectFrames(noisy, Info(640, 480));
    CheckDiagnosed(selNoisy, DiagnosisKind::OutOfFocus);
    INFO(selNoisy.diagnosis.message);
    CHECK(Number(selNoisy.diagnosis, "blur spread") < stack::kSteadySpread);
    CHECK(Number(selNoisy.diagnosis, "blur spread upper bound") >= stack::kSteadySpread);
    CHECK(selNoisy.diagnosis.message.find("too large to tell") != std::string::npos);
}

// ---- fix round 3: re-review 2 (A-E) --------------------------------------------------------------

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
    // selection, its sharpest frames included -- no suspects.
    std::mt19937 rng(5);
    std::normal_distribution<double> n01(0, 1);
    std::vector<FrameStats> stats;
    for (int64_t i = 0; i < 3000; ++i) {
        const double qt = (i < 600 ? 1.3 : 1.0) * (1 + 0.03 * n01(rng));
        stats.push_back(DiscQ(i, qt * (1 + 0.02 * n01(rng)), 0.02 * qt));
    }
    const Selection sel = SelectFrames(stats, Info(640, 480));
    const auto kept = CheckSelected(sel, stats);
    CHECK(sel.suspects.empty());
    for (int64_t f : kept) CHECK(f < 600);
    CHECK(kept.size() >= 540);
    const auto best = std::max_element(stats.begin(), stats.end(),
                                       [](const FrameStats& x, const FrameStats& y) { return x.quality < y.quality; });
    CHECK(kept.count(best->index) == 1);
}

// ---- fix round 4: re-review 3 (glitch clusters, tilted trails) -----------------------------------

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
            CHECK(sel.explanation.find("suspect frame") != std::string::npos);
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
        CHECK(sel.explanation.find("suspect frame") != std::string::npos);
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

// ---- fix round 5: re-review 4 (Rulings P6 suspects, P7 cloudy full-frame targets) ---------------

namespace {
// Global alignment results with the given confidences (only the confidence matters to admission).
std::vector<stack::Shift> Confidences(size_t n, double lo, double hi, std::mt19937& rng) {
    std::uniform_real_distribution<double> u(lo, hi);
    std::vector<stack::Shift> v(n);
    for (auto& s : v) s.confidence = u(rng);
    return v;
}

// The clean frames align as steady frames do (Align.h: 0.96-0.99); returns AlignmentCut over them.
double CleanCut(const Selection& sel, std::mt19937& rng) {
    return stack::AlignmentCut(Confidences(sel.frames.size(), 0.96, 0.99, rng));
}

// What every admission promises; returns the final frames as a set.
std::set<int64_t> CheckAdmitted(const Selection& sel, const stack::Admission& adm, int64_t n) {
    INFO(adm.explanation);
    REQUIRE(std::is_sorted(adm.frames.begin(), adm.frames.end()));
    REQUIRE(std::adjacent_find(adm.frames.begin(), adm.frames.end()) == adm.frames.end());
    CHECK(adm.admitted.size() + adm.rejected.size() == sel.suspects.size());
    std::set<int64_t> fin(adm.frames.begin(), adm.frames.end());
    for (int64_t f : sel.frames) CHECK(fin.count(f) == 1);
    for (int64_t f : adm.admitted) CHECK(fin.count(f) == 1);
    for (int64_t f : adm.rejected) CHECK(fin.count(f) == 0);
    CHECK(adm.frames.size() == sel.frames.size() + adm.admitted.size());
    std::smatch m;
    REQUIRE(std::regex_search(adm.explanation, m, std::regex(R"(^used (\d+) of (\d+) frames: )")));
    CHECK(std::stoll(m[1]) == static_cast<long long>(adm.frames.size()));
    CHECK(std::stoll(m[2]) == n);
    CHECK(adm.explanation.find("(alignment decides)") == std::string::npos);
    CheckCountsAddUp(adm.explanation, n);
    if (!sel.suspects.empty()) {
        const std::string outcome = std::to_string(sel.suspects.size()) +
                                    (sel.suspects.size() == 1 ? " suspect frame" : " suspect frames") +
                                    " (sharper than the rest): " + std::to_string(adm.admitted.size()) +
                                    " admitted after alignment, " + std::to_string(adm.rejected.size()) + " rejected";
        CHECK(adm.explanation.find(outcome) != std::string::npos);
    } else {
        CHECK(adm.explanation == sel.explanation);
    }
    return fin;
}

// The review's lucky-group capture: n - m bulk frames (true log-quality sd 1%) and m frames centred
// c sd above it with width w sd, measured with relative noise ns (reported). Frames >= n - m are the
// group.
std::vector<FrameStats> LuckyGroup(int n, int m, double c, double w, double ns, uint32_t seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> n01(0, 1);
    std::vector<FrameStats> v;
    for (int i = 0; i < n - m; ++i) {
        const double qt = std::exp(0.01 * n01(rng));
        v.push_back(DiscQ(i, qt * (1 + ns * n01(rng)), ns * qt));
    }
    for (int i = 0; i < m; ++i) {
        const double qt = std::exp(0.01 * (c + w * n01(rng)));
        v.push_back(DiscQ(n - m + i, qt * (1 + ns * n01(rng)), ns * qt));
    }
    return v;
}
}  // namespace

TEST_CASE("a small genuine lucky group is held as suspect, then admitted when it aligns (P6)", "[select][suspects]") {
    // The review's cases: 15 of 300, 30 of 1000, 50 of 3000 frames 8 sd above the rest (width 1.5
    // sd), with no noise reported (the band rule then holds most of the group as suspect: 50 of 3000
    // kept only 5.9 of the group in round 4) and with 1% noise. Lucky frames show the target whole,
    // so they align like the clean frames.
    struct Case {
        int n, m;
    };
    for (const Case c : {Case{300, 15}, Case{1000, 30}, Case{3000, 50}}) {
        for (const double ns : {0.0, 0.01}) {
            int64_t suspects = 0;
            for (uint32_t seed = 1; seed <= 5; ++seed) {
                const auto stats = LuckyGroup(c.n, c.m, 8, 1.5, ns, seed);
                const Selection sel = SelectFrames(stats, Info(640, 480));
                INFO(c.m << " of " << c.n << ", noise " << ns << ", seed " << seed << ": " << sel.explanation);
                const auto kept = CheckSelected(sel, stats);
                suspects += static_cast<int64_t>(sel.suspects.size());
                for (size_t k = 0; k < sel.suspects.size(); ++k) {
                    const stack::Suspect& s = sel.suspects[k];
                    CHECK(s.index >= c.n - c.m);  // only the group is that sharp
                    CHECK(kept.count(s.index) == 0);
                    CHECK(s.quality == stats[static_cast<size_t>(s.index)].quality);
                    CHECK_FALSE(s.reason.empty());
                    if (k > 0) CHECK(sel.suspects[k - 1].index < s.index);
                }
                std::mt19937 rng(seed + 100);
                const double cut = CleanCut(sel, rng);
                const stack::Admission adm = AdmitSuspects(sel, Confidences(sel.suspects.size(), 0.96, 0.99, rng), cut);
                const auto fin = CheckAdmitted(sel, adm, c.n);
                CHECK(adm.rejected.empty());
                for (int64_t f = c.n - c.m; f < c.n; ++f) CHECK(fin.count(f) == 1);  // the whole group stacks
            }
            INFO(c.m << " of " << c.n << ", noise " << ns);
            if (ns == 0) CHECK(suspects > 0);  // the cases do exercise admission
        }
    }
}

TEST_CASE("glitch suspects are rejected by alignment: torn or foreign frames never stack (P6)", "[select][suspects]") {
    // 5 glitches at 1.3 on 300 frames, torn (confidence 0.50-0.77, under any steady capture's cut).
    {
        std::mt19937 rng(41);
        std::normal_distribution<double> n01(0, 1);
        std::vector<FrameStats> stats;
        for (int64_t i = 0; i < 300; ++i) stats.push_back(DiscQ(i, 1.0 * (1 + 0.01 * n01(rng)), 0.0));
        for (int64_t i = 300; i < 305; ++i) stats.push_back(DiscQ(i, 1.3, 0.0));
        const Selection sel = SelectFrames(stats, Info(640, 480));
        REQUIRE(sel.suspects.size() == 5);
        const double cut = CleanCut(sel, rng);
        const stack::Admission adm = AdmitSuspects(sel, Confidences(5, 0.50, 0.77, rng), cut);
        CheckAdmitted(sel, adm, 305);
        CHECK(adm.frames == sel.frames);
        CHECK(adm.rejected == std::vector<int64_t>{300, 301, 302, 303, 304});
        CHECK(adm.explanation.find("5 rejected by alignment") != std::string::npos);

        // Two of them align (a sharp frame that happens to be whole), three do not.
        std::vector<stack::Shift> mixed = Confidences(5, 0.50, 0.77, rng);
        mixed[0].confidence = 0.98;
        mixed[1].confidence = 0.97;
        const stack::Admission part = AdmitSuspects(sel, mixed, cut);
        CheckAdmitted(sel, part, 305);
        CHECK(part.admitted == std::vector<int64_t>{300, 301});
        CHECK(part.rejected == std::vector<int64_t>{302, 303, 304});
        CHECK(part.explanation.find("3 rejected by alignment") != std::string::npos);

        // A confidence that is not a number is not a pass; the shifts must match the suspects.
        std::vector<stack::Shift> nan = Confidences(5, 0.96, 0.99, rng);
        nan[2].confidence = std::numeric_limits<double>::quiet_NaN();
        const stack::Admission withNan = AdmitSuspects(sel, nan, cut);
        CHECK(withNan.rejected == std::vector<int64_t>{302});
        CHECK_THROWS_AS(AdmitSuspects(sel, Confidences(4, 0.96, 0.99, rng), cut), std::invalid_argument);
    }
    // Spread glitch clusters (round 4's shapes): foreign content (0.05-0.27), 5 seeds each.
    struct Case {
        int m;
        double lo, hi, spread, noise;
    };
    for (const Case c : {Case{50, 1.05, 1.6, 0.01, 0.01}, Case{40, 1.1, 1.5, 0.02, 0.02}}) {
        for (uint32_t seed = 1; seed <= 5; ++seed) {
            const auto stats = BulkAndGlitches(c.m, c.lo, c.hi, c.spread, c.noise, seed);
            const Selection sel = SelectFrames(stats, Info(640, 480));
            INFO("glitches " << c.m << ", seed " << seed << ": " << sel.explanation);
            REQUIRE_FALSE(sel.suspects.empty());
            for (const stack::Suspect& s : sel.suspects) CHECK(s.index >= 3000);
            std::mt19937 rng(seed);
            const double cut = CleanCut(sel, rng);
            const stack::Admission adm = AdmitSuspects(sel, Confidences(sel.suspects.size(), 0.05, 0.27, rng), cut);
            CheckAdmitted(sel, adm, 3000 + c.m);
            CHECK(adm.admitted.empty());
            for (int64_t f : adm.frames) CHECK(stats[static_cast<size_t>(f)].quality < 1.25);
        }
    }
    // The ladder: torn rungs.
    {
        std::mt19937 rng(41);
        std::normal_distribution<double> n01(0, 1);
        std::vector<FrameStats> stats;
        for (int64_t i = 0; i < 300; ++i) stats.push_back(DiscQ(i, 1.0 * (1 + 0.01 * n01(rng)), 0.0));
        for (int64_t i = 0; i < 10; ++i) stats.push_back(DiscQ(300 + i, std::exp(0.05 + 0.019 * static_cast<double>(i)), 0.0));
        const Selection sel = SelectFrames(stats, Info(640, 480));
        REQUIRE_FALSE(sel.suspects.empty());
        const double cut = CleanCut(sel, rng);
        const stack::Admission adm = AdmitSuspects(sel, Confidences(sel.suspects.size(), 0.5, 0.77, rng), cut);
        CheckAdmitted(sel, adm, 310);
        for (int64_t f : adm.frames) CHECK(f < 302);
    }
}

TEST_CASE("no suspects: admission changes nothing; a diagnosed selection has none (P6)", "[select][suspects]") {
    std::mt19937 rng(11);
    std::normal_distribution<double> n01(0, 1);
    const auto stats = DiscCapture(500, [&](int64_t) { return 1.0 * (1 + 0.02 * n01(rng)); });
    const Selection sel = SelectFrames(stats, Info(640, 480));
    CheckSelected(sel, stats);
    REQUIRE(sel.suspects.empty());
    const stack::Admission adm = AdmitSuspects(sel, {}, 0.85);
    CHECK(adm.frames == sel.frames);
    CHECK(adm.explanation == sel.explanation);

    std::vector<FrameStats> none;
    for (int64_t i = 0; i < 21; ++i) none.push_back(NoTargetFrame(i));
    const Selection diag = SelectFrames(none, Info(640, 480));
    CHECK(diag.suspects.empty());
    const stack::Admission admDiag = AdmitSuspects(diag, {}, 0.85);
    CHECK(admDiag.frames.empty());
    CHECK(admDiag.explanation == diag.explanation);
}

TEST_CASE("AdmitSuspects refuses a cut that AlignmentCut cannot return (controller follow-up)", "[select][suspects]") {
    // AlignmentCut is never below kMinAlignConfidence; a lower, zero, negative or NaN cut is a caller
    // bug that would admit torn or foreign frames, so it throws -- with or without suspects.
    std::mt19937 rng(41);
    std::normal_distribution<double> n01(0, 1);
    std::vector<FrameStats> stats;
    for (int64_t i = 0; i < 300; ++i) stats.push_back(DiscQ(i, 1.0 * (1 + 0.01 * n01(rng)), 0.0));
    for (int64_t i = 300; i < 305; ++i) stats.push_back(DiscQ(i, 1.3, 0.0));
    const Selection sel = SelectFrames(stats, Info(640, 480));
    REQUIRE(sel.suspects.size() == 5);
    const std::vector<stack::Shift> shifts = Confidences(5, 0.96, 0.99, rng);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double below = std::nextafter(stack::kMinAlignConfidence, 0.0);
    for (double bad : {below, 0.3, 0.0, -0.85, nan, -std::numeric_limits<double>::infinity()}) {
        INFO("cut " << bad);
        CHECK_THROWS_AS(AdmitSuspects(sel, shifts, bad), std::invalid_argument);
        CHECK_THROWS_AS(AdmitSuspects(Selection{}, {}, bad), std::invalid_argument);
    }
    // The floor itself and anything above it are accepted.
    CHECK(AdmitSuspects(sel, shifts, stack::kMinAlignConfidence).admitted.size() == 5);
    CHECK(AdmitSuspects(sel, shifts, 0.995).rejected.size() == 5);
    CHECK_NOTHROW(AdmitSuspects(Selection{}, {}, stack::kMinAlignConfidence));
}

namespace {
// The review's cloudy Moon: n frames of 640 x 480; with probability p a frame shows a frame-filling
// box (0, 0)-(320, 200..206), quality log-normal 3%, noise 1%; otherwise cloud (no target).
std::vector<FrameStats> CloudyMoon(int n, double p) {
    std::mt19937 rng(7);
    std::normal_distribution<double> n01(0, 1);
    std::bernoulli_distribution clear(p);
    std::vector<FrameStats> v;
    for (int i = 0; i < n; ++i) {
        FrameStats s;
        s.index = i;
        if (clear(rng)) {
            s.targetFound = true;
            s.bx0 = 0;
            s.by0 = 0;
            s.bx1 = 320;
            s.by1 = 200 + i % 7;
            s.cx = 160;
            s.cy = 100;
            s.quality = std::exp(0.03 * n01(rng));
            s.qualitySigma = 0.01;
            s.brightness = 0.5;
        }
        v.push_back(s);
    }
    return v;
}
}  // namespace

TEST_CASE("a cloudy Moon stacks from its clear frames (P7)", "[select]") {
    struct Case {
        int n;
        double p;
    };
    for (const Case c : {Case{2000, 0.48}, Case{2000, 0.30}, Case{200, 0.45}, Case{60, 0.40}}) {
        const auto stats = CloudyMoon(c.n, c.p);
        int64_t clear = 0;
        for (const auto& s : stats) clear += s.targetFound;
        const Selection sel = SelectFrames(stats, Info(640, 480));
        INFO(c.n << " frames, clear " << clear << ": " << sel.explanation);
        const auto kept = CheckSelected(sel, stats);
        CHECK(sel.fullFrameTarget);
        for (int64_t f : kept) CHECK(stats[static_cast<size_t>(f)].targetFound);
        CHECK(static_cast<double>(kept.size()) >= 0.6 * static_cast<double>(clear));
        CHECK(sel.explanation.find(std::to_string(c.n - clear) + " without a target (cloud or gaps?)") != std::string::npos);
    }
}

TEST_CASE("a frame-filling surface that drifts is still frame-filling (P7)", "[select]") {
    // The box (full height, 400 px wide) drifts steadily by 20% of the width over 2000 frames.
    std::mt19937 rng(9);
    std::normal_distribution<double> n01(0, 1);
    std::vector<FrameStats> stats;
    for (int64_t i = 0; i < 2000; ++i) {
        FrameStats s;
        s.index = i;
        s.targetFound = true;
        s.bx0 = static_cast<int>(128 * i / 2000);
        s.bx1 = s.bx0 + 400;
        s.by0 = 0;
        s.by1 = 480;
        s.cx = s.bx0 + 200;
        s.cy = 240;
        s.quality = 0.3 * (1 + 0.01 * n01(rng));
        s.qualitySigma = 0.003;
        stats.push_back(s);
    }
    const Selection sel = SelectFrames(stats, Info(640, 480));
    const auto kept = CheckSelected(sel, stats);
    CHECK(sel.fullFrameTarget);
    CHECK(kept.size() >= 1800);
}

TEST_CASE("trails over 10% of the frames sweep: never a frame-filling target (P7)", "[select]") {
    // Tilted trails in every 5th frame (20%) and in every frame: each box passes FillsFrame, but the
    // boxes cross wherever they happen to.
    for (const int every : {5, 1}) {
        std::vector<FrameStats> stats;
        for (int64_t i = 0; i < 2000; ++i)
            stats.push_back(i % every == 0 ? TiltedTrail(i, 640, 480, 1.5 + 2.5 * static_cast<double>(i % 7) / 6) : NoTargetFrame(i));
        const Selection sel = SelectFrames(stats, Info(640, 480));
        INFO("a trail in every " << every << ": " << sel.explanation);
        CHECK_FALSE(sel.fullFrameTarget);
        CHECK(sel.frames.empty());
        CHECK(sel.diagnosis.kind != DiagnosisKind::None);
    }
}

TEST_CASE("a frame-filling surface seen in under 10% of the frames: no target, cloud or gaps (P7)", "[select][diagnosis]") {
    const auto stats = CloudyMoon(2000, 0.05);
    int64_t clear = 0;
    for (const auto& s : stats) clear += s.targetFound;
    const Selection sel = SelectFrames(stats, Info(640, 480));
    CheckDiagnosed(sel, DiagnosisKind::NoTarget);
    CHECK_FALSE(sel.fullFrameTarget);
    CHECK(sel.diagnosis.message.find("cloud or gaps?") != std::string::npos);
    CHECK(sel.diagnosis.message.find("keep it centred") == std::string::npos);
    CHECK(Number(sel.diagnosis, "frames") == 2000);
    CHECK(Number(sel.diagnosis, "frames with a target") == static_cast<double>(clear));
    CHECK(Number(sel.diagnosis, "presence needed") == stack::kFullFramePresence);
}

// ---- gross defocus on large discs (Task 6 finding) ------------------------------------------------

namespace {
// A measured capture of a textured disc of radius R (MakeDisc, 60 frames, drift 1.5 px) blurred by
// `blur` px: as StackSynth renders a blur (the limb widened in quadrature, the texture smoothed; 4%
// jitter from frame to frame, noise 0.01: Task 6's case), or -- `convolve` -- as defocus acts on the
// whole image: each sharp frame (seeing 0.6 px, 4% jitter) convolved with a Gaussian of sigma `blur`
// (steady: the optics), pixel noise `noise` added after. Frames reach R + 3 blur + 30 px out from the
// centre each way, so the blurred halo stays inside. `sky`: the sky level when >= 0 (MakeDisc's is 0.02,
// 2 noise sd above black at noise 0.01: the 16-bit capture clips 2.3% of it at 0).
Selection MeasuredDisc(double radius, double blur, bool convolve, double noise, unsigned seed, double sky = -1) {
    const int half = static_cast<int>(radius + 3 * blur + 30), w = 2 * half, h = 2 * half;
    testing::Scene s = testing::MakeDisc(seed, half + 0.3, half - 0.2, radius);
    if (sky >= 0) s.sky = sky;
    testing::Capture cap = testing::NewCapture(w, h);
    std::mt19937 rng(seed + 1);
    std::normal_distribution<double> n01(0, 1);
    std::vector<testing::FrameSpec> specs;
    for (int k = 0; k < 60; ++k) {
        testing::FrameSpec f;
        f.gx = 1.5 * n01(rng);
        f.gy = 1.5 * n01(rng);
        f.hasLocal = true;
        f.dx = testing::RandomField(rng, w, h, 28, [] { return 0.0; });
        f.dy = f.dx;
        const double b = (convolve ? 0.6 : blur) * std::exp(0.04 * n01(rng));
        f.sigma = testing::RandomField(rng, w, h, 28, [b] { return b; });
        f.noise = convolve ? 0.0 : noise;
        specs.push_back(f);
    }
    for (int k = 0; k < 60; ++k) {
        std::vector<float> v = testing::RenderFrame(s, w, h, specs[static_cast<size_t>(k)], 500u + k);
        if (convolve) {
            const double sg = blur;
            const int r = static_cast<int>(std::ceil(3 * sg));
            std::vector<double> kern;
            double sum = 0;
            for (int i = -r; i <= r; ++i) sum += kern.emplace_back(std::exp(-0.5 * i * i / (sg * sg)));
            for (double& kv : kern) kv /= sum;
            auto pass = [&](std::vector<float>& img, bool alongX) {
                std::vector<float> out(img.size());
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x) {
                        double acc = 0;
                        for (int i = -r; i <= r; ++i) {
                            const int xx = alongX ? std::clamp(x + i, 0, w - 1) : x;
                            const int yy = alongX ? y : std::clamp(y + i, 0, h - 1);
                            acc += kern[static_cast<size_t>(i + r)] * img[static_cast<size_t>(yy) * w + xx];
                        }
                        out[static_cast<size_t>(y) * w + x] = static_cast<float>(acc);
                    }
                img.swap(out);
            };
            pass(v, true);
            pass(v, false);
            for (float& p : v) p = static_cast<float>(p + noise * n01(rng));
        }
        cap.Add(v);
    }
    return SelectFrames(stack::MeasureAll(cap.Factory(), cap.info, 8, {}), cap.info);
}
}  // namespace

TEST_CASE("a grossly defocused large disc is out of focus; sharp and mildly soft ones are stacked", "[select][diagnosis][focus]") {
    // Task 6 found the R 24 / blur 12 case stacked: the box radius grows with the blur (24 -> 41 luma
    // px here), so sigma(q) / R_box never reached R / 8 on any measured disc. Readings (Select.h step 7)
    // are sigma(q_ref) over the half-contrast radius; limit kGrossBlurLimit.
    struct Case {
        double radius, blur;
        bool convolve;
        double noise;
        bool diagnosed;
        double sky = -1;  // MakeDisc's 0.02 unless given
    };
    // Ruling P19 (an unbiased pixel noise): R 24 blurred 10 px at noise 0.01 read 0.245 and was stacked
    // while the noise estimate read s^2 ~5% low (0.263 then, 0.269 with Ruling P26), and the convolved
    // R 24 / 12 px at noise 0.01 sat on the limit (0.267; 0.312 after P19; 0.363 after P26; noise-free
    // 0.334). The faint blob R 40 / 20 px at noise 0.01 over a sky of 0.06 that does not clip was stacked
    // and is refused (0.377 after P19, 0.325 after P26; noise-free 0.342). Over the default sky of 0.02 the
    // capture clips 2.3% of the sky at 0: with one noise level, measured on that sky, the target's noise
    // read low (the sky's variance is 0.960 of the target's) and R 40 / 20 px was stacked (0.240); with
    // the capture's noise model (Ruling P26: each level's own noise, the clipped sky's included) it reads
    // 0.312 and is refused -- in every one of seeds 7, 107, ..., 907 (0.304-0.375).
    for (const Case& c : {Case{24, 0.6, false, 0.01, false}, Case{24, 2, false, 0.01, false}, Case{24, 3, false, 0.01, false},
                          Case{40, 4, false, 0.01, false}, Case{24, 2, true, 0.005, false}, Case{40, 3, true, 0.005, false},
                          Case{24, 2, true, 0.01, false}, Case{40, 3, true, 0.01, false},
                          Case{24, 12, false, 0.01, true}, Case{24, 12, true, 0.005, true}, Case{40, 20, true, 0.005, true},
                          Case{24, 10, false, 0.01, true}, Case{24, 12, true, 0.01, true},
                          Case{40, 20, true, 0.01, true, 0.06}, Case{40, 3, true, 0.01, false, 0.06},
                          Case{40, 20, true, 0.01, true}}) {
        const Selection sel = MeasuredDisc(c.radius, c.blur, c.convolve, c.noise, 7, c.sky);
        std::string numbers;
        for (const auto& [name, v] : sel.diagnosis.numbers) numbers += name + " " + std::to_string(v) + "; ";
        INFO("R " << c.radius << ", blur " << c.blur << (c.convolve ? " (convolved)" : " (StackSynth limb)") << ", noise "
                  << c.noise << ", sky " << (c.sky >= 0 ? c.sky : 0.02) << ": "
                  << sel.explanation << " | " << numbers);
        CHECK(sel.focusJudged);
        CHECK(sel.targetRadius >= stack::kMinJudgedRadius);  // judged by the large-disc rule
        if (!c.diagnosed) {
            CHECK(sel.diagnosis.kind == DiagnosisKind::None);
            continue;
        }
        CHECK(sel.diagnosis.kind == DiagnosisKind::OutOfFocus);
        if (sel.diagnosis.kind != DiagnosisKind::OutOfFocus) continue;
        // The radius the judgement used is the disc's, not the blurred halo's -- up to a blur of R / 2;
        // blurred by R the centre dims and the half-contrast radius moves out (R 24 by 24 px: 33.5).
        if (c.blur <= 0.5 * c.radius) CHECK(Number(sel.diagnosis, "target radius (pixels)") == Catch::Approx(c.radius).epsilon(0.15));
        CHECK(Number(sel.diagnosis, "limb blur (pixels)") >= Number(sel.diagnosis, "blur limit (pixels)"));
    }
    // The faintest: R 24 convolved by 24 px over the unclipped sky (noise-free 0.400), each frame's quality
    // at a median 3.35-3.57 qualitySigma (seeds 7 / 107 / 207). Its reading is the top of a deconvolution of
    // 53-58 noisy frames, which a few high draws can lift: 0.222 / 0.333 / 0.385 / 0.368 / 0.341 / 0.473 /
    // 0.511 / 0.379 / 0.343 / 0.295 for seeds 7, 107, ..., 907 with Ruling P26's noise. A miss stacks a
    // soft image, which the region stage warns about -- the failure the rule prefers (Ruling P15).
    int refused = 0, refusedClipped = 0;
    for (unsigned seed = 7; seed < 1000; seed += 100) {
        refused += MeasuredDisc(24, 24, true, 0.01, seed, 0.06).diagnosis.kind == DiagnosisKind::OutOfFocus;
        refusedClipped += MeasuredDisc(40, 20, true, 0.01, seed).diagnosis.kind == DiagnosisKind::OutOfFocus;
    }
    INFO("R 24 / 24 px over sky 0.06 refused in " << refused << " of 10 seeds, R 40 / 20 px over the clipped sky in "
                                                  << refusedClipped);
    CHECK(refused >= 8);
    CHECK(refusedClipped == 10);
}

TEST_CASE("a small disc blurred into a large box stays small: the region stage judges it (P4 and P20)", "[select][diagnosis][focus][class]") {
    // Classed by the box, these turned "large" (boxes of 16-22 luma px) and were refused here before
    // the region stage could judge them (R 11 blurred 4 px as PoorSeeing): the size class moved with
    // the blur being judged. Classed by the half-contrast radius they stay small, and the small-disc
    // rule (box-inflated: S <= 0.85 on these) leaves them to the region stage, which warns (Ruling P20).
    for (const auto& [radius, blur] : std::vector<std::pair<double, double>>{{8, 6}, {11, 4}, {11, 6}, {14, 6}}) {
        const Selection sel = MeasuredDisc(radius, blur, false, 0.01, 7);
        INFO("R " << radius << ", blur " << blur << ": " << sel.explanation);
        CHECK(sel.focusJudged);
        CHECK(sel.targetRadius < stack::kMinJudgedRadius);
        CHECK(sel.targetRadius == Catch::Approx(radius).margin(2.5));
        CHECK(sel.diagnosis.kind == DiagnosisKind::None);
    }
}
