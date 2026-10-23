#include <catch_amalgamated.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "AviWriter.h"
#include "Engine.h"
#include "StackSynth.h"
#include "TestUtil.h"

using namespace planetary;
using ser::FloatImage;
using stack::DiagnosisKind;
using stack::StackOutcome;
using testing::Capture;
using testing::FrameSpec;
using testing::Scene;

namespace {

testing::TempDir& Dir() {
    static testing::TempDir dir;
    return dir;
}

// The capture's frames (16-bit in memory) written as an 8-bit AVI with no sidecar.
std::string WriteAvi8(const Capture& cap) {
    const std::string path = Dir().NewPath(".avi");
    std::vector<std::vector<uint8_t>> frames;
    for (const auto& f : *cap.frames) {
        std::vector<uint8_t> b(f.size());
        for (size_t i = 0; i < f.size(); ++i) b[i] = static_cast<uint8_t>(std::lround(f[i] / 257.0));
        frames.push_back(std::move(b));
    }
    testing::AviSpec spec;
    spec.width = cap.info.header.width;
    spec.height = cap.info.header.height;
    testing::WriteAvi(path, spec, frames);
    return path;
}

// Frames of a scene at the sensor's sites: an RGGB mosaic of the scene times per-colour gains.
void RenderMosaic(Capture& cap, const Scene& s, int n, const std::function<FrameSpec(int)>& spec) {
    const int w = cap.info.header.width, h = cap.info.header.height;
    const double gain[3] = {1.0, 0.8, 0.55};  // R, G, B
    Capture mono = testing::NewCapture(w, h);
    testing::RenderCapture(mono, s, n, spec);
    for (const auto& f : *mono.frames) {
        std::vector<float> v(f.size());
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const int c = (y & 1) == 0 ? ((x & 1) == 0 ? 0 : 1) : ((x & 1) == 0 ? 1 : 2);
                const size_t i = static_cast<size_t>(y) * w + x;
                v[i] = static_cast<float>(gain[c] * f[i] / 65535.0);
            }
        cap.Add(v);
    }
}

// Disc capture with seeing (StackSynth's Seeing: drift, local displacement, blur field).
Capture DiscCapture(int w, int h, double cx, double cy, double radius, int n, unsigned seed, double drift = 2.0) {
    const Scene s = testing::MakeDisc(seed, cx, cy, radius);
    Capture cap = testing::NewCapture(w, h);
    std::mt19937 rng(seed + 1000);
    testing::RenderCapture(cap, s, n, [&](int) { return testing::Seeing(rng, w, h, drift, 28); });
    return cap;
}

// Readers that count themselves and their reads, optionally slowed down.
struct ReaderLedger {
    std::atomic<int> live{0}, peak{0};
    std::atomic<int64_t> reads{0};
    // Reads made on the thread that called RunStack (BuildReference's and the focus measure's) and on
    // the workers (every pass).
    std::atomic<int64_t> callerReads{0}, workerReads{0};
    std::thread::id caller = std::this_thread::get_id();
    int delayMs = 0;
};

class CountedSource : public ser::FrameSource {
public:
    CountedSource(std::unique_ptr<ser::FrameSource> inner, ReaderLedger& ledger) : inner_(std::move(inner)), ledger_(ledger) {
        const int now = ++ledger_.live;
        for (int p = ledger_.peak.load(); now > p && !ledger_.peak.compare_exchange_weak(p, now);) {
        }
    }
    ~CountedSource() override { --ledger_.live; }
    const ser::FileInfo& Info() const override { return inner_->Info(); }
    ser::Frame ReadFrame(int64_t i) const override {
        ++ledger_.reads;
        ++(std::this_thread::get_id() == ledger_.caller ? ledger_.callerReads : ledger_.workerReads);
        if (ledger_.delayMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ledger_.delayMs));
        return inner_->ReadFrame(i);
    }
    std::optional<int64_t> FrameTimestamp(int64_t i) const override { return inner_->FrameTimestamp(i); }

private:
    std::unique_ptr<ser::FrameSource> inner_;
    ReaderLedger& ledger_;
};

stack::SourceFactory Counted(const stack::SourceFactory& base, ReaderLedger& ledger) {
    return [base, &ledger] { return std::unique_ptr<ser::FrameSource>(new CountedSource(base(), ledger)); };
}

// Threads of this process (Linux: one entry per thread in /proc/self/task).
int ThreadCount() {
    int n = 0;
    for (const auto& e : std::filesystem::directory_iterator("/proc/self/task")) {
        (void)e;
        ++n;
    }
    return n;
}

// The thread count once it has stopped changing (the same over 100 ms, at most 3 s). pthread_join
// returns once the kernel clears the joined thread's tid during its exit, and /proc/self/task lists the
// task until it is reaped a moment later -- so a count taken straight after a join, or straight after
// the previous RunStack, can be high (under ctest -j8: 3 for 1 after a cancel, and a baseline taken
// right after a dry run that never came back). A worker still running would hold its reader open
// (ReaderLedger::live, checked first) and keep the count from settling at the baseline.
int SettledThreadCount() {
    int last = ThreadCount(), same = 0;
    for (int i = 0; i < 300 && same < 10; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const int now = ThreadCount();
        same = now == last ? same + 1 : 0;
        last = now;
    }
    return last;
}

FloatImage NormalizedFrame(const std::string& path, int64_t i) {
    auto src = ser::OpenVideo(path, ser::BayerChoice::None);
    return ser::Normalize(src->ReadFrame(i), src->Info().normDepth);
}

// The selection line must never be the provisional one (Ruling P6).
void CheckFinalLines(const StackOutcome& o) {
    CHECK(o.summary.selection.find("(alignment decides)") == std::string::npos);
    CHECK_FALSE(o.summary.selection.empty());
}

}  // namespace

TEST_CASE("RunStack: an end-to-end synthetic AVI stacks closer to the truth than its best single frame", "[engine]") {
    const int w = 128, h = 112;
    const double cx = 64.3, cy = 55.7, radius = 26;
    const Scene s = testing::MakeDisc(11, cx, cy, radius);
    const Capture cap = DiscCapture(w, h, cx, cy, radius, 60, 11);
    const std::string path = WriteAvi8(cap);

    struct Report {
        std::string phase;
        int64_t done, total;
    };
    std::vector<Report> reports;
    bool offThread = false;
    const auto caller = std::this_thread::get_id();
    stack::Progress progress;
    progress.report = [&](const std::string& phase, int64_t done, int64_t total) {
        if (std::this_thread::get_id() != caller) offThread = true;
        reports.push_back({phase, done, total});
    };
    progress.cancelled = [&] {
        if (std::this_thread::get_id() != caller) offThread = true;
        return false;
    };
    const StackOutcome o = stack::RunStack(path, ser::BayerChoice::None, 8, progress);

    REQUIRE(o.diagnosis.kind == DiagnosisKind::None);
    REQUIRE(o.image.has_value());
    CHECK(o.image->width == w);
    CHECK(o.image->height == h);
    CHECK(o.image->channels == 1);
    CHECK(o.info.colorId == ser::ColorId::Mono);
    CHECK(o.summary.measured == 60);
    CHECK(o.summary.used > 0);
    CHECK(o.summary.used <= 60);
    CHECK(o.summary.used == static_cast<int64_t>(o.frames.stacked.size()));
    CheckFinalLines(o);
    CHECK_FALSE(o.summary.alignment.empty());
    CHECK(o.summary.seconds > 0);
    CHECK(o.summary.bytesRead >= 60 * static_cast<int64_t>(w) * h);
    CHECK(o.summary.readMBps > 0);
    std::vector<std::string> steps;
    for (const auto& p : o.summary.phaseSeconds) steps.push_back(p.first);
    CHECK(steps == std::vector<std::string>{"Hot pixels", "Measuring noise", "Measuring frames", "Selecting", "Building reference",
                                            "Aligning frames", "Planning regions", "Judging focus", "Stacking"});
    for (const auto& wtext : o.summary.warnings) CHECK(wtext != stack::kUndeclaredMosaicWarning);

    // Closer to the truth than every single frame (the sharpest included), over the disc and its limb.
    auto inside = [&](int x, int y) { return std::hypot(x - cx, y - cy) < radius + 4; };
    const double ours = testing::RmsToTruth(*o.image, s, inside);
    double bestFrame = 1e9;
    for (int64_t i = 0; i < 60; ++i) bestFrame = std::min(bestFrame, testing::RmsToTruth(NormalizedFrame(path, i), s, inside));
    INFO("rms ours " << ours << ", best single frame " << bestFrame);
    CHECK(ours < 0.8 * bestFrame);  // measured 0.68 x (0.0316 vs 0.0467)

    // Progress: the three phases in order, each with one total, done growing to it, all on the caller.
    CHECK_FALSE(offThread);
    std::vector<std::string> order;
    for (const auto& r : reports)
        if (order.empty() || order.back() != r.phase) order.push_back(r.phase);
    CHECK(order == std::vector<std::string>{stack::kPhaseMeasuring, stack::kPhaseAligning, stack::kPhaseStacking});
    for (const std::string& phase : order) {
        int64_t total = -1, last = -1;
        for (const auto& r : reports) {
            if (r.phase != phase) continue;
            if (total < 0) total = r.total;
            CHECK(r.total == total);
            CHECK(r.done >= last);
            CHECK(r.done <= r.total);
            last = r.done;
        }
        INFO(phase);
        CHECK(last == total);
        CHECK(total > 0);
    }
}

TEST_CASE("RunStack: a capture with no target gives NoTarget and no image", "[engine]") {
    const int w = 96, h = 80;
    Capture cap = testing::NewCapture(w, h);
    Scene sky;  // a flat 2% pedestal and noise: the 2026-01-30 Saturn AVI in miniature
    sky.level = 0.02;
    testing::RenderCapture(cap, sky, 40, [](int) {
        FrameSpec f;
        f.noise = 0.004;
        return f;
    });
    std::vector<std::string> phases;
    stack::Progress progress;
    progress.report = [&](const std::string& phase, int64_t, int64_t) { phases.push_back(phase); };
    const StackOutcome o = stack::RunStack(WriteAvi8(cap), ser::BayerChoice::None, 4, progress);
    CHECK(o.diagnosis.kind == DiagnosisKind::NoTarget);
    CHECK_FALSE(o.image.has_value());
    CHECK_FALSE(o.diagnosis.message.empty());
    CHECK_FALSE(o.diagnosis.numbers.empty());
    CHECK(o.summary.used == 0);
    CHECK(o.summary.measured == 40);
    CHECK(o.summary.selection == o.diagnosis.message);
    CHECK(o.frames.stacked.empty());
    for (const auto& p : phases) CHECK(p == stack::kPhaseMeasuring);

    // The same frames as an undeclared mosaic (the 2026-01-30 Saturn AVI's case): no image, so no
    // "was stacked as mono" warning either.
    const StackOutcome raw = stack::RunStack(WriteAvi8(cap), ser::BayerChoice::Auto, 4, {});
    CHECK(stack::UndeclaredMosaic(raw.info));
    CHECK(raw.diagnosis.kind == DiagnosisKind::NoTarget);
    CHECK(std::count(raw.summary.warnings.begin(), raw.summary.warnings.end(), std::string(stack::kUndeclaredMosaicWarning)) == 0);
}

namespace {

// A disc on a steady night (blur 1.6 px +- 4%), so selection keeps most frames (>= kFocusFrames).
Capture SteadyDisc(int w, int h, int frames, unsigned seed) {
    const Scene s = testing::MakeDisc(seed, w / 2.0 + 0.2, h / 2.0 - 0.2, 22);
    Capture cap = testing::NewCapture(w, h);
    std::mt19937 rng(seed + 3);
    std::normal_distribution<double> n(0, 1);
    testing::RenderCapture(cap, s, frames, [&](int) {
        FrameSpec f;
        f.gx = 1.5 * n(rng);
        f.gy = 1.5 * n(rng);
        f.hasLocal = true;
        f.dx = testing::RandomField(rng, w, h, 28, [] { return 0.0; });
        f.dy = testing::RandomField(rng, w, h, 28, [] { return 0.0; });
        const double blur = 1.6 * std::exp(0.04 * n(rng));
        f.sigma = testing::RandomField(rng, w, h, 28, [blur] { return blur; });
        return f;
    });
    return cap;
}

}  // namespace

TEST_CASE("RunStack: cancel in each phase throws StackCancelled, joins every thread and closes every reader", "[engine]") {
    const Capture cap = SteadyDisc(112, 96, 48, 21);
    // The boundaries are read from who reads, not from progress counts: BuildReference reads its
    // kReferenceFrames frames on the calling thread, then the alignment pass reads on the workers, then
    // the focus measure reads its kFocusFrames on the calling thread again. That needs >= kFocusFrames
    // kept frames (so kReferenceFrames selected), checked on a dry run.
    {
        const StackOutcome dry = stack::RunStack(cap.Factory(), 6, {});
        REQUIRE(dry.frames.stacked.size() >= static_cast<size_t>(stack::kFocusFrames));
    }
    struct Case {
        const char* where;
        std::string phase;
    };
    for (const Case& c : {Case{"measuring", stack::kPhaseMeasuring}, Case{"building the reference", stack::kPhaseAligning},
                          Case{"the alignment pass", stack::kPhaseAligning}, Case{"the focus measure's reads", stack::kPhaseAligning},
                          Case{"judging focus", stack::kPhaseAligning}, Case{"stacking", stack::kPhaseStacking}}) {
        const std::string where = c.where;
        INFO("cancel in " << where);
        ReaderLedger ledger;
        ledger.delayMs = 2;
        const auto factory = Counted(cap.Factory(), ledger);
        const int threadsBefore = SettledThreadCount();
        std::string phase;
        int64_t done = 0;
        int64_t callerAtAligning = -1, workerAtAligning = -1, callerAtFire = -1, workerAtFire = -1, pollsAfterReads = 0;
        std::vector<std::string> seen;
        bool fired = false;
        stack::Progress progress;
        progress.report = [&](const std::string& p, int64_t d, int64_t) {
            phase = p;
            done = d;
            if (seen.empty() || seen.back() != p) seen.push_back(p);
            if (p == stack::kPhaseAligning && callerAtAligning < 0) {
                callerAtAligning = ledger.callerReads.load();
                workerAtAligning = ledger.workerReads.load();
            }
        };
        progress.cancelled = [&] {
            if (fired || phase != c.phase) return fired;
            const int64_t caller = ledger.callerReads.load() - callerAtAligning, worker = ledger.workerReads.load() - workerAtAligning;
            if (where == "building the reference") fired = caller >= 2;
            else if (where == "the alignment pass") fired = worker >= 3;
            else if (where == "the focus measure's reads") fired = caller >= stack::kReferenceFrames + 2;
            // Every focus frame read: without polls inside the focus measure there are only two more (the read
            // loop's after the last read, and the phase's end), so a 4th proves MeasureFocus / the inversion poll.
            else if (where == "judging focus") fired = caller >= stack::kReferenceFrames + stack::kFocusFrames && ++pollsAfterReads >= 4;
            else fired = done >= 2;
            if (fired) {
                callerAtFire = caller;
                workerAtFire = worker;
            }
            return fired;
        };
        CHECK_THROWS_AS(stack::RunStack(factory, 6, progress), stack::StackCancelled);
        CHECK(fired);
        REQUIRE_FALSE(seen.empty());
        CHECK(seen.back() == c.phase);  // nothing after the cancelled phase was started
        if (where == "building the reference") CHECK(workerAtFire == 0);
        if (where == "the alignment pass") CHECK(callerAtFire == stack::kReferenceFrames);  // no focus read yet
        if (where == "the focus measure's reads") CHECK(callerAtFire > stack::kReferenceFrames);
        if (where == "judging focus") CHECK(callerAtFire == stack::kReferenceFrames + stack::kFocusFrames);
        CHECK(ledger.live.load() == 0);
        CHECK(SettledThreadCount() == threadsBefore);
        const int64_t reads = ledger.reads.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        CHECK(ledger.reads.load() == reads);  // no worker is still reading
    }
}

TEST_CASE("RunStack: an AVI with an unknown Bayer pattern stacks the raw mosaic as mono, with a warning", "[engine]") {
    const int w = 192, h = 160;
    const Scene s = testing::MakeDisc(31, 96.4, 80.6, 40);
    Capture cap = testing::NewCapture(w, h);
    std::mt19937 rng(3100);
    RenderMosaic(cap, s, 40, [&](int) { return testing::Seeing(rng, w, h, 2.0, 40); });
    const std::string path = WriteAvi8(cap);

    const StackOutcome raw = stack::RunStack(path, ser::BayerChoice::Auto, 8, {});
    REQUIRE(raw.diagnosis.kind == DiagnosisKind::None);
    REQUIRE(raw.image.has_value());
    CHECK(stack::UndeclaredMosaic(raw.info));
    CHECK(raw.image->channels == 1);
    CHECK(raw.image->width == w);  // full resolution: the mosaic itself, not its 2 x 2 luma
    CHECK(raw.image->height == h);
    CHECK(std::count(raw.summary.warnings.begin(), raw.summary.warnings.end(), std::string(stack::kUndeclaredMosaicWarning)) == 1);
    const std::string warning = stack::kUndeclaredMosaicWarning;
    CHECK(warning.find("mono") != std::string::npos);
    CHECK(warning.find("cannot be debayered afterwards") != std::string::npos);
    // Neutral: anything reading summary.warnings directly (a script, stack-probe's JSON) gets the fact,
    // the consequence and the remedy, naming no tool; each window gives its own controls (ShownWarnings).
    CHECK(warning.find("choose a Bayer pattern and stack again") != std::string::npos);
    CHECK(warning.find("Save pattern") == std::string::npos);
    CHECK(warning.find("SERViewer") == std::string::npos);
    CheckFinalLines(raw);

    const StackOutcome colour = stack::RunStack(path, ser::BayerChoice::RGGB, 8, {});
    REQUIRE(colour.diagnosis.kind == DiagnosisKind::None);
    REQUIRE(colour.image.has_value());
    CHECK(colour.image->channels == 3);
    CHECK(colour.image->width == w);
    CHECK(std::count(colour.summary.warnings.begin(), colour.summary.warnings.end(), std::string(stack::kUndeclaredMosaicWarning)) == 0);
}

TEST_CASE("RunStack: 1 and 8 threads give the same result within 1e-6", "[engine]") {
    const Capture cap = DiscCapture(112, 96, 55.6, 48.3, 22, 50, 41);
    const StackOutcome a = stack::RunStack(cap.Factory(), 1, {}), b = stack::RunStack(cap.Factory(), 8, {});
    REQUIRE(a.image.has_value());
    REQUIRE(b.image.has_value());
    REQUIRE(a.image->data.size() == b.image->data.size());
    double worst = 0;
    for (size_t i = 0; i < a.image->data.size(); ++i) worst = std::max(worst, static_cast<double>(std::fabs(a.image->data[i] - b.image->data[i])));
    INFO("max |1 thread - 8 threads| " << worst);
    CHECK(worst <= 1e-6);
    CHECK(a.summary.used == b.summary.used);
    CHECK(a.summary.selection == b.summary.selection);
    CHECK(a.summary.alignment == b.summary.alignment);
    CHECK(a.frames.stacked == b.frames.stacked);
    CHECK(a.frames.regionsKeeping == b.frames.regionsKeeping);
    CHECK(a.summary.memory.observers == 1);
    CHECK(a.summary.focus.measure == b.summary.focus.measure);  // MeasureFocus is identical for any count
}

TEST_CASE("RunStack: a lucky suspect is admitted and carries region weight, a torn one is rejected (Rulings P6, P8)", "[engine]") {
    const int w = 112, h = 96;
    const double cx = 56.2, cy = 47.8;
    const Scene s = testing::MakeDisc(51, cx, cy, 24);
    Capture cap = testing::NewCapture(w, h);
    std::mt19937 rng(5100);
    std::normal_distribution<double> n(0, 1);
    const int lucky = 20, torn = 40;
    testing::RenderCapture(cap, s, 60, [&](int k) {
        FrameSpec f;
        f.gx = 1.5 * n(rng);
        f.gy = 1.5 * n(rng);
        f.hasLocal = true;
        f.dx = testing::RandomField(rng, w, h, 28, [] { return 0.0; });
        f.dy = testing::RandomField(rng, w, h, 28, [] { return 0.0; });
        // A steady night (blur 1.6 px +- 4%), one genuinely sharper frame and one sharp frame torn
        // through the middle by a shake during readout.
        const double blur = k == lucky || k == torn ? 0.5 : 1.6 * std::exp(0.04 * n(rng));
        f.sigma = testing::RandomField(rng, w, h, 28, [blur] { return blur; });
        if (k == torn) {
            f.tearRow = static_cast<int>(cy);
            f.tearDx = 9;
        }
        return f;
    });
    const StackOutcome o = stack::RunStack(cap.Factory(), 8, {});
    REQUIRE(o.diagnosis.kind == DiagnosisKind::None);
    INFO(o.summary.selection);
    INFO(o.summary.alignment);
    CHECK(o.frames.admittedSuspects == std::vector<int64_t>{lucky});
    CHECK(o.frames.rejectedSuspects == std::vector<int64_t>{torn});
    CHECK(o.summary.selection.find("1 admitted after alignment, 1 rejected") != std::string::npos);
    CheckFinalLines(o);
    const auto at = std::find(o.frames.stacked.begin(), o.frames.stacked.end(), lucky);
    REQUIRE(at != o.frames.stacked.end());
    REQUIRE(o.frames.regions > 0);
    const int keeping = o.frames.regionsKeeping[static_cast<size_t>(at - o.frames.stacked.begin())];
    INFO("the lucky frame is stacked by " << keeping << " of " << o.frames.regions << " regions");
    CHECK(keeping == o.frames.regions);  // P8: no region fences it off
    CHECK(std::find(o.frames.stacked.begin(), o.frames.stacked.end(), torn) == o.frames.stacked.end());
}

TEST_CASE("RunStack: a hot pixel fixed on the sensor of a perfectly tracked capture is repaired before every stage", "[engine]") {
    // In the sky beside the disc: this test is about the repair reaching every stage (the wrapper), not about
    // where HotPixelMap finds hot sites -- a site inside a textured disc is HotPixelMap's own tests' business.
    const int w = 112, h = 96, hx = 90, hy = 20;
    const double cx = 56.2, cy = 47.8;
    const Scene s = testing::MakeDisc(61, cx, cy, 24);
    Capture cap = testing::NewCapture(w, h);
    std::mt19937 rng(6100);
    testing::RenderCapture(cap, s, 50, [&](int) { return testing::Seeing(rng, w, h, 0.0, 28); });  // no drift
    Capture hot = testing::NewCapture(w, h);
    for (const auto& f : *cap.frames) {
        std::vector<float> v(f.size());
        for (size_t i = 0; i < f.size(); ++i) v[i] = static_cast<float>(f[i] / 65535.0);
        v[static_cast<size_t>(hy) * w + hx] = std::min(1.0f, v[static_cast<size_t>(hy) * w + hx] + 0.35f);
        hot.Add(v);
    }
    const StackOutcome clean = stack::RunStack(cap.Factory(), 4, {}), o = stack::RunStack(hot.Factory(), 4, {});
    REQUIRE(clean.image.has_value());
    REQUIRE(o.image.has_value());
    CHECK(clean.summary.hotPixels == 0);
    CHECK(o.summary.hotPixels == 1);
    CHECK(o.summary.alignment.find("1 hot pixel repaired") != std::string::npos);
    const size_t site = static_cast<size_t>(hy) * w + hx;
    INFO("stack at the hot site " << o.image->data[site] << ", without the hot pixel " << clean.image->data[site]);
    CHECK(std::fabs(o.image->data[site] - clean.image->data[site]) < 0.02);  // unrepaired: ~0.35 above
}

TEST_CASE("RepairingSource returns every frame repaired; RepairingFactory passes a null reader on", "[engine]") {
    const int w = 48, h = 40;
    Capture cap = testing::NewCapture(w, h);
    for (int k = 0; k < 20; ++k) {
        std::vector<float> v(static_cast<size_t>(w) * h, 0.1f + 0.001f * k);
        v[static_cast<size_t>(20) * w + 30] = 0.9f;
        cap.Add(v);
    }
    auto map = std::make_shared<stack::HotPixelMap>(cap.info);
    auto probe = cap.Factory()();
    for (int k = 0; k < 20; ++k) map->Observe(probe->ReadFrame(k));
    map->Finish();
    REQUIRE(map->Count() == 1);
    const stack::SourceFactory repaired = stack::RepairingFactory(cap.Factory(), map);
    auto src = repaired();
    for (int k = 0; k < 20; k += 7) {
        const ser::Frame f = src->ReadFrame(k);
        CHECK(f.samples[static_cast<size_t>(20) * w + 30] == (*cap.frames)[static_cast<size_t>(k)][static_cast<size_t>(20) * w + 31]);
    }
    CHECK(src->Info().frameCount == 20);
    const stack::SourceFactory none = stack::RepairingFactory([] { return std::unique_ptr<ser::FrameSource>(); }, map);
    CHECK(none() == nullptr);
}

TEST_CASE("RunStack: a frame-filling surface is judged by its detail scale -- clean, or warned and stacked, never refused (Ruling P15)", "[engine]") {
    const int w = 224, h = 176;
    const Scene s = testing::MakeSurface(71, w, h);
    auto capture = [&](double blur) {
        Capture cap = testing::NewCapture(w, h);
        std::mt19937 rng(7100);
        std::normal_distribution<double> n(0, 1);
        testing::RenderCapture(cap, s, 40, [&](int) {
            FrameSpec f;
            f.gx = 1.5 * n(rng);
            f.gy = 1.5 * n(rng);
            f.hasLocal = true;
            f.dx = testing::RandomField(rng, w, h, 40, [&] { return 0.3 * n(rng); });
            f.dy = testing::RandomField(rng, w, h, 40, [&] { return 0.3 * n(rng); });
            f.sigma = testing::RandomField(rng, w, h, 40, [&] { return blur * std::exp(0.1 * n(rng)); });
            return f;
        });
        return cap;
    };
    auto focusWarnings = [](const StackOutcome& o) {
        int k = 0;
        for (const auto& wtext : o.summary.warnings) k += wtext.find("may be out of focus") != std::string::npos;
        return k;
    };
    // Measured here (detail scale, sensor px = luma px): blur 1 -> 4.5, 12 -> 12.3.
    const StackOutcome sharp = stack::RunStack(capture(1.0).Factory(), 8, {});
    INFO("blur 1: detail " << sharp.summary.focus.detailScale << " | " << sharp.summary.alignment);
    CHECK(sharp.diagnosis.kind == DiagnosisKind::None);
    CHECK(sharp.image.has_value());
    CHECK_FALSE(sharp.summary.focus.disc);
    CHECK(sharp.summary.focus.outcome == stack::FocusOutcome::InFocus);
    CHECK(sharp.summary.focus.detailScale < stack::kSurfaceWarnAbove);
    CHECK(focusWarnings(sharp) == 0);
    const StackOutcome soft = stack::RunStack(capture(12.0).Factory(), 8, {});
    INFO("blur 12: detail " << soft.summary.focus.detailScale << " | " << soft.diagnosis.message);
    CHECK(soft.diagnosis.kind == DiagnosisKind::None);
    CHECK(soft.image.has_value());
    CHECK(soft.summary.focus.outcome == stack::FocusOutcome::Warned);
    CHECK(soft.summary.focus.detailScale > stack::kSurfaceWarnAbove);
    CHECK(focusWarnings(soft) == 1);
}

namespace {

// A Bayer (RGGB) capture of a textured disc of `radius` luma px (2 x in sensor px) blurred by `blur`
// sensor px, with a little jitter.
Capture BayerDisc(int w, int h, double radiusLuma, double blur, unsigned seed) {
    const double cx = w / 2.0 + 0.3, cy = h / 2.0 - 0.4;
    const Scene s = testing::MakeDisc(seed, cx, cy, 2 * radiusLuma);
    Capture cap = testing::NewCapture(w, h, ser::ColorId::BayerRGGB);
    std::mt19937 rng(seed + 7);
    std::normal_distribution<double> n(0, 1);
    RenderMosaic(cap, s, 40, [&](int) {
        FrameSpec f;
        f.gx = 1.2 * n(rng);
        f.gy = 1.2 * n(rng);
        f.hasLocal = true;
        f.dx = testing::RandomField(rng, w, h, 40, [] { return 0.0; });
        f.dy = testing::RandomField(rng, w, h, 40, [] { return 0.0; });
        f.sigma = testing::RandomField(rng, w, h, 40, [&] { return blur * std::exp(0.05 * n(rng)); });
        return f;
    });
    return cap;
}

}  // namespace

TEST_CASE("RunStack: a disc is judged by its limb blur against its radius -- clean or warned (review C1, Ruling P15)", "[engine]") {
    auto focusWarnings = [](const StackOutcome& o) {
        int k = 0;
        for (const auto& wtext : o.summary.warnings) k += wtext.find("out of focus") != std::string::npos;
        return k;
    };
    SECTION("a sharp Bayer disc of radius 14 luma px stacks clean, at 0.85 and at 1.25 luma px of blur (1.5 x Barlow)") {
        for (double blur : {1.7, 2.5}) {
            const StackOutcome o = stack::RunStack(BayerDisc(192, 160, 14, blur, 81).Factory(), 8, {});
            const stack::FocusCheck& f = o.summary.focus;
            INFO("blur " << blur << " sensor px: limb " << f.limbBlur << " px, " << f.measure << " of the radius " << f.radius
                         << " | " << o.diagnosis.message);
            CHECK(o.diagnosis.kind == DiagnosisKind::None);
            CHECK(o.image.has_value());
            CHECK(f.disc);
            CHECK(f.outcome == stack::FocusOutcome::InFocus);
            CHECK(f.radius == Catch::Approx(14).margin(1.5));  // the half-level radius, not the blurred box
            CHECK(focusWarnings(o) == 0);
        }
    }
    SECTION("the same disc blurred by 16 sensor px (0.57 of its radius) is stacked with a may-be-out-of-focus warning") {
        const StackOutcome o = stack::RunStack(BayerDisc(256, 224, 14, 16, 81).Factory(), 8, {});
        const stack::FocusCheck& f = o.summary.focus;
        INFO("limb " << f.limbBlur << " px, " << f.measure << " of the radius " << f.radius << " | " << o.diagnosis.message);
        CHECK(o.diagnosis.kind == DiagnosisKind::None);
        CHECK(o.image.has_value());
        CHECK(f.outcome == stack::FocusOutcome::Warned);
        CHECK(f.measure > stack::kDiscWarnAbove);
        int warned = 0;
        for (const auto& wtext : o.summary.warnings) warned += wtext == stack::FocusWarning(f);
        CHECK(warned == 1);
    }
    SECTION("a disc blurred by twice its radius is a blob: Select's gross-defocus rule refuses it before the region stage") {
        // The region stage never refuses a disc (Ruling P20; a blob's half-level radius grows with its blur, holding
        // its measure at 0.57-0.67 -- real Jupiter blurred by 16 px: 0.566; this synthetic: 0.61). Select does: the
        // blob's half-contrast radius (~15.6 luma px) puts it in the large-disc class, where sigma(q_ref) / R_h reads
        // 0.47 against kGrossBlurLimit 0.25 (Select.h step 7) -- the one refusal for a disc's gross defocus.
        // Steady or not is left open in the message: with qualitySigma calibrated to the frames' real scatter
        // (Ruling P26), the noise-corrected spread's upper bound is 8% here, over kSteadySpread (before, an
        // overstated qualitySigma made it read 0 -- "the same in every frame" -- with false confidence).
        const StackOutcome o = stack::RunStack(BayerDisc(320, 288, 8, 32, 82).Factory(), 8, {});
        INFO(o.diagnosis.message << " | " << o.summary.selection);
        CHECK(o.diagnosis.kind == DiagnosisKind::OutOfFocus);
        double blurOverRadius = 0;
        for (const auto& [name, v] : o.diagnosis.numbers)
            if (name == "blur / radius") blurOverRadius = v;
        CHECK(blurOverRadius >= stack::kGrossBlurLimit);
        CHECK_FALSE(o.image.has_value());
    }
}
TEST_CASE("RunStack: frames that do not show one scene are diagnosed Unaligned, with no image", "[engine]") {
    const int w = 160, h = 128;
    Capture cap = testing::NewCapture(w, h);
    for (int k = 0; k < 30; ++k) {  // every frame a different lunar field
        FrameSpec f;
        cap.Add(testing::RenderFrame(testing::MakeSurface(900u + k, w, h), w, h, f, 77u + k));
    }
    const StackOutcome o = stack::RunStack(cap.Factory(), 8, {});
    INFO(o.diagnosis.message);
    CHECK(o.diagnosis.kind == DiagnosisKind::Unaligned);
    CHECK(o.summary.alignment.empty());
    CHECK_FALSE(o.image.has_value());
    CHECK(o.summary.used == 0);
    CHECK_FALSE(o.diagnosis.numbers.empty());
}

TEST_CASE("PlanMemory caps ResidualScale observers and workers so the engine stays within its budget", "[engine]") {
    auto info = [](int w, int h, ser::ColorId c, int depth) {
        ser::FileInfo i;
        i.header.width = w;
        i.header.height = h;
        i.colorId = c;
        i.normDepth = depth;
        i.planes = c == ser::ColorId::RGB ? 3 : 1;
        i.bytesPerSample = depth > 8 ? 2 : 1;
        return i;
    };
    struct Format {
        ser::FileInfo info;
        const char* name;
    };
    const Format formats[] = {{info(1920, 1080, ser::ColorId::Mono, 8), "1080p mono"},
                              {info(1920, 1080, ser::ColorId::BayerRGGB, 8), "1080p Bayer"},
                              {info(3840, 2160, ser::ColorId::RGB, 16), "4K RGB 16-bit"},
                              {info(640, 480, ser::ColorId::Mono, 8), "VGA mono"}};
    for (const Format& f : formats)
        for (int threads : {1, 8, 32, 64, 256}) {
            INFO(f.name << ", " << threads << " threads");
            const stack::MemoryPlan p = stack::PlanMemory(f.info, threads);
            CHECK(p.observers >= 1);
            CHECK(p.observers <= p.alignWorkers);
            CHECK(p.observers * p.observerBytes <= std::max(stack::kResidualBudget, p.observerBytes));
            for (int wk : {p.measureWorkers, p.alignWorkers, p.stackWorkers}) {
                CHECK(wk >= 1);
                CHECK(wk <= threads);
            }
            CHECK(p.peakBytes <= stack::kMemoryBudget);
        }
    // 1080p mono, 32 threads: ResidualScale's 41 MB per instance would be 1.3 GB on every worker.
    const stack::MemoryPlan mono = stack::PlanMemory(formats[0].info, 32);
    CHECK(mono.observerBytes > 40000000);
    CHECK(mono.observers == 12);
    CHECK(mono.alignWorkers == 32);
    // 4K RGB: the stacking pass's workers are what the budget caps.
    CHECK(stack::PlanMemory(formats[2].info, 64).stackWorkers < 64);
    // The focus measure's kFocusFrames luma frames are counted (after the residual scale is released):
    // 4K mono, 16 x 33 MB = 531 MB.
    const stack::MemoryPlan mono4k = stack::PlanMemory(info(3840, 2160, ser::ColorId::Mono, 16), 32);
    CHECK(mono4k.focusBytes == static_cast<size_t>(stack::kFocusFrames) * 4 * 3840 * 2160);
    CHECK(mono4k.peakBytes >= mono4k.focusBytes);
    CHECK(mono4k.peakBytes <= stack::kMemoryBudget);
}

TEST_CASE("RunStack opens no more readers at once than the memory plan's workers", "[engine]") {
    const Capture cap = DiscCapture(112, 96, 55.6, 48.3, 22, 50, 43);
    for (int threads : {1, 3, 8}) {
        ReaderLedger ledger;
        const StackOutcome o = stack::RunStack(Counted(cap.Factory(), ledger), threads, {});
        REQUIRE(o.image.has_value());
        const stack::MemoryPlan& m = o.summary.memory;
        INFO(threads << " threads: peak live readers " << ledger.peak.load());
        CHECK(ledger.peak.load() <= std::max({m.measureWorkers, m.alignWorkers, m.stackWorkers}));
        CHECK(ledger.peak.load() >= 1);  // how many overlap depends on scheduling; the bound above does not
        CHECK(ledger.live.load() == 0);
    }
}

TEST_CASE("RunStack: a file that cannot be opened is an error, not a diagnosis", "[engine]") {
    CHECK_THROWS_AS(stack::RunStack(Dir().NewPath(".avi"), ser::BayerChoice::Auto, 2, {}), ser::SerError);
}

TEST_CASE("MeasureFocus: the detail scale of a blurred white texture is its blur, whatever the noise and sub-pixel shifts", "[engine]") {
    const int n = 256;
    std::mt19937 rng(9100);
    std::normal_distribution<double> g(0, 1);
    std::uniform_real_distribution<double> u(-3, 3);
    std::vector<stack::Complex> white(static_cast<size_t>(n) * n);
    for (auto& c : white) c = g(rng);
    const stack::Fft2D fft(n, n);
    fft.Forward(white);
    auto freq = [n](int k) { return (k <= n / 2 ? k : k - n) / static_cast<double>(n); };
    for (double sigma : {2.0, 4.0}) {
        for (double noiseShare : {0.0, 1.0}) {  // per-frame noise rms relative to the texture's rms
            std::vector<FloatImage> lumas;
            std::vector<stack::Shift> shifts;
            for (int k = 0; k < 16; ++k) {
                // Frame k shows the texture moved by d (exactly, by the shift theorem): frame(x + d) = texture(x).
                stack::Shift d;
                d.dx = u(rng);
                d.dy = u(rng);
                std::vector<stack::Complex> f = white;
                for (int ky = 0; ky < n; ++ky)
                    for (int kx = 0; kx < n; ++kx) {
                        const double fx = freq(kx), fy = freq(ky);
                        f[static_cast<size_t>(ky) * n + kx] *= std::exp(-2 * M_PI * M_PI * sigma * sigma * (fx * fx + fy * fy)) *
                                                               std::polar(1.0, -2 * M_PI * (fx * d.dx + fy * d.dy));
                    }
                fft.Inverse(f);
                FloatImage im{n, n, 1, std::vector<float>(static_cast<size_t>(n) * n)};
                double rms = 0;
                for (const auto& c : f) rms += c.real() * c.real();
                rms = std::sqrt(rms / f.size());
                for (size_t i = 0; i < f.size(); ++i)
                    im.data[i] = static_cast<float>(0.5 + 0.1 * (f[i].real() / rms + noiseShare * g(rng)));
                lumas.push_back(std::move(im));
                shifts.push_back(d);
            }
            std::vector<stack::FocusWindow> windows;
            for (int y : {32, 96, 160})
                for (int x : {32, 96, 160}) windows.push_back({x, y, 64});
            const auto one = stack::MeasureFocus(lumas, shifts, windows, 1), eight = stack::MeasureFocus(lumas, shifts, windows, 8);
            REQUIRE(one.size() == windows.size());
            for (size_t k = 0; k < one.size(); ++k) {
                CHECK(one[k].frames == 16);
                CHECK(one[k].c0 == eight[k].c0);  // the same for any thread count
                CHECK(one[k].c2 == eight[k].c2);
            }
            const stack::FocusVerdict v = stack::JudgeFocus(one, 1);
            INFO("sigma " << sigma << ", noise " << noiseShare << ": detail scale " << v.detailScale);
            CHECK(v.judged == 9);
            // Measured: sigma 2 -> 1.99 / 2.06, sigma 4 -> 3.81 / 3.53 (noise 0 / 1): the 64-px window resolves a
            // 4-px blur in ~2.5 frequency bins, and noise as strong as the texture scatters the moment.
            CHECK(v.detailScale == Catch::Approx(sigma).epsilon(0.15));
            CHECK(stack::JudgeFocus(one, 2).detailScale == Catch::Approx(2 * v.detailScale));  // sensor px
        }
    }
}

TEST_CASE("The disc focus measure recovers the limb blur of a blurred disc of any radius (review C1)", "[engine]") {
    // Discs rendered in real space -- each pixel the mean of 8 x 8 sub-samples of a uniform disc on sky --
    // then blurred exactly (Gaussian in the Fourier domain), moved by sub-pixel shifts, noise added.
    const int n = 96;
    const stack::Fft2D fft(n, n);
    auto freq = [n](int k) { return (k <= n / 2 ? k : k - n) / static_cast<double>(n); };
    std::mt19937 rng(9200);
    std::normal_distribution<double> g(0, 1);
    std::uniform_real_distribution<double> u(-2, 2);
    for (double radius : {2.7, 9.5, 14.0}) {
        std::vector<stack::Complex> disc(static_cast<size_t>(n) * n);
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                int in = 0;
                for (int j = 0; j < 8; ++j)
                    for (int i = 0; i < 8; ++i) in += std::hypot(x - 48 + (i + 0.5) / 8 - 0.5, y - 48 + (j + 0.5) / 8 - 0.5) < radius;
                disc[static_cast<size_t>(y) * n + x] = in / 64.0;
            }
        fft.Forward(disc);
        for (double sigma : {0.85, 1.25, 2.0, 4.0}) {
            std::vector<FloatImage> lumas;
            std::vector<stack::Shift> shifts;
            for (int k = 0; k < 16; ++k) {
                stack::Shift d;
                d.dx = u(rng);
                d.dy = u(rng);
                std::vector<stack::Complex> f = disc;
                for (int ky = 0; ky < n; ++ky)
                    for (int kx = 0; kx < n; ++kx) {
                        const double fx = freq(kx), fy = freq(ky);
                        f[static_cast<size_t>(ky) * n + kx] *= std::exp(-2 * M_PI * M_PI * sigma * sigma * (fx * fx + fy * fy)) *
                                                               std::polar(1.0, -2 * M_PI * (fx * d.dx + fy * d.dy));
                    }
                fft.Inverse(f);
                FloatImage im{n, n, 1, std::vector<float>(static_cast<size_t>(n) * n)};
                for (size_t i = 0; i < f.size(); ++i)
                    im.data[i] = static_cast<float>(0.03 + 0.6 * f[i].real() / (n * n) + 0.01 * g(rng));
                lumas.push_back(std::move(im));
                shifts.push_back(d);
            }
            const stack::FocusWindow win = stack::DiscFocusWindow(48, 48, radius, n, n);
            REQUIRE(win.side > 0);
            CHECK_FALSE(win.taper);
            const auto p = stack::MeasureFocus(lumas, shifts, {win}, 1);
            const double detail = stack::FocusDetailScale(p[0]);
            const double limb = stack::LimbBlurFromDetail(detail, radius, p[0].side);
            INFO("radius " << radius << ", sigma " << sigma << ": window " << win.side << ", detail " << detail << ", limb blur " << limb);
            CHECK(limb == Catch::Approx(sigma).epsilon(0.15));
            // The half-level radius of one frame (its disc sits at 48 + its shift) stays near the true one.
            const stack::FocusWindow w0 = stack::DiscFocusWindow(48 + shifts[0].dx, 48 + shifts[0].dy, radius, n, n);
            const double half = stack::DiscHalfRadius(lumas[0], 48 + shifts[0].dx, 48 + shifts[0].dy, radius, w0);
            INFO("half-level radius " << half);
            // Measured: within 0.13 sigma (inward) while sigma <= radius / 2; outward by 0.25 / 0.6 sigma at
            // sigma 2 / 4 on the 2.7-px disc (a blob).
            if (sigma <= radius / 2) CHECK(std::fabs(half - radius) <= 0.15 * sigma + 0.15);
            // DiscRadius iterates to its own estimate whatever radius it starts from (Select's, of either
            // definition: the blurred box or the half-contrast radius), so the verdict does not depend on it.
            const double fromBelow = stack::DiscRadius(lumas[0], 48 + shifts[0].dx, 48 + shifts[0].dy, 0.7 * radius);
            const double fromAbove = stack::DiscRadius(lumas[0], 48 + shifts[0].dx, 48 + shifts[0].dy, 1.8 * radius + 2 * sigma);
            INFO("DiscRadius from 0.7 R: " << fromBelow << ", from 1.8 R + 2 sigma: " << fromAbove);
            if (sigma <= radius / 2) CHECK(std::fabs(fromBelow - fromAbove) <= 0.05 * radius + 0.1);
        }
    }
}

TEST_CASE("MeasureFocus: noise-free frames are judged (no noise is infinite signal-to-noise, not none)", "[engine]") {
    const int n = 128;
    const Scene s = testing::MakeSurface(95, n, n);
    FloatImage im{n, n, 1, std::vector<float>(static_cast<size_t>(n) * n)};
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) im.data[static_cast<size_t>(y) * n + x] = static_cast<float>(s.Eval(x, y, 1.0));
    const std::vector<FloatImage> lumas(16, im);  // identical: the halves' power equals their cross-power
    const std::vector<stack::Shift> shifts(16);
    const auto p = stack::MeasureFocus(lumas, shifts, {{32, 32, 64}}, 1);
    double snr = 0;
    const double d = stack::FocusDetailScale(p[0], &snr);
    INFO("detail " << d << ", snr " << snr);
    CHECK(std::isinf(snr));
    CHECK(std::isfinite(d));
    CHECK(stack::JudgeFocus(p, 1).judged == 1);
}

TEST_CASE("The alignment line's region phrase never claims a region stacks 0 frames of its own", "[engine]") {
    auto detail = [](std::vector<int64_t> kept) {
        std::vector<stack::RegionDetail> d;
        for (int64_t k : kept) {
            stack::RegionDetail r;
            r.kept = k;
            r.candidates = k;
            d.push_back(r);
        }
        return d;
    };
    CHECK(stack::RegionPhrase(detail({262, 300, 290}), 300, 300) ==
          "3 regions placed on the target's detail, each stacking its own sharpest 262-300 of 300 frames (median 290)");
    CHECK(stack::RegionPhrase(detail({300}), 300, 300) ==
          "1 region placed on the target's detail, stacking its own sharpest 300 of 300 frames");
    // The 2026-01-30 Moon 193142: 7 of 326 regions (lunar-limb windows, mostly dark sky) measured no
    // detail above the noise; they take the plain mean of the stacked frames (Accumulator's fallback).
    std::vector<int64_t> moon(319, 2043);
    moon[0] = 5;
    moon[1] = 2102;
    moon.insert(moon.end(), 7, 0);
    CHECK(stack::RegionPhrase(detail(moon), 2102, 2102) ==
          "326 regions placed on the target's detail: 319 stacking their own sharpest 5-2102 of 2102 frames (median "
          "2043), 7 with no detail above the measurement noise taking the mean of the 2102 stacked frames");
    CHECK(stack::RegionPhrase({}, 40, 40) == "no region holds detail to rank on, so the 40 aligned frames are stacked whole");
}

namespace {


// The region-stage disc verdict on synthetic frames of the same optics at scale m: a disc of radius
// R0 m luma px blurred by sigma0 m luma px, 8 frames with sub-pixel jitter, pixel-integrated.
stack::FocusCheck DiscVerdictAtScale(bool bayer, double m, double radius0, double sigma0, unsigned seed) {
    // Sensor pixels integrate the image over their area: every pixel is the mean of 6 x 6 sub-samples of a
    // flat disc (sky 0.02, disc 0.32) whose edge is a Gaussian blur of sigma -- what a real sensor records,
    // so the model's removal of the pixel aperture is what is tested.
    const int ls = bayer ? 2 : 1;
    const double rS = radius0 * m * ls, sigmaS = sigma0 * m * ls;
    int side = static_cast<int>(std::ceil(2 * (rS + 4 * sigmaS) + 16 * ls + 24));
    side += side & 1;
    const int w = side, h = side;
    std::mt19937 rng(seed + 11);
    std::normal_distribution<double> n(0, 1);
    std::vector<stack::Shift> shifts;
    std::vector<double> gx, gy;
    for (int k = 0; k < 8; ++k) {
        gx.push_back(w / 2.0 + 0.8 * n(rng));
        gy.push_back(h / 2.0 + 0.8 * n(rng));
        shifts.push_back(stack::Shift{(gx.back() - w / 2.0) / ls, (gy.back() - h / 2.0) / ls, 1});
    }
    Capture cap = testing::NewCapture(w, h, bayer ? ser::ColorId::BayerRGGB : ser::ColorId::Mono);
    const double gain[3] = {1.0, 0.8, 0.55};
    std::vector<std::vector<float>> frames(8, std::vector<float>(static_cast<size_t>(w) * h));
    std::vector<std::thread> pool;
    for (int k = 0; k < 8; ++k)
        pool.emplace_back([&, k] {
            std::mt19937 noise(seed * 31 + k);
            std::normal_distribution<double> g(0, 1);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    double acc = 0;
                    for (int j = 0; j < 6; ++j)
                        for (int i = 0; i < 6; ++i) {
                            const double r = std::hypot(x - 0.5 + (i + 0.5) / 6 - gx[static_cast<size_t>(k)],
                                                        y - 0.5 + (j + 0.5) / 6 - gy[static_cast<size_t>(k)]);
                            acc += 0.5 * std::erfc((r - rS) / (std::sqrt(2.0) * std::max(sigmaS, 1e-3)));
                        }
                    const int c = !bayer ? 1 : (y & 1) == 0 ? ((x & 1) == 0 ? 0 : 1) : ((x & 1) == 0 ? 1 : 2);
                    frames[static_cast<size_t>(k)][static_cast<size_t>(y) * w + x] =
                        static_cast<float>(gain[c] * (0.02 + 0.3 * acc / 36) + 0.003 * g(noise));
                }
        });
    for (auto& t : pool) t.join();
    for (const auto& f : frames) cap.Add(f);
    auto src = cap.Factory()();
    std::vector<FloatImage> lumas;
    for (int k = 0; k < 8; ++k) lumas.push_back(stack::MeasurementLuma(src->ReadFrame(k), cap.info));
    // Luma pixel i covers sensor pixels ls i .. ls i + ls - 1: sensor X is luma (X - (ls - 1) / 2) / ls.
    const double cx = (w / 2.0 - 0.5 * (ls - 1)) / ls, cy = (h / 2.0 - 0.5 * (ls - 1)) / ls;
    // RunStack seeds DiscRadius with Select's radius; here the true one, inflated as Select's would be.
    const double half = stack::DiscRadius(lumas[0], cx + shifts[0].dx, cy + shifts[0].dy, radius0 * m + 2 * sigma0 * m);
    return stack::MeasureDiscFocus(lumas, shifts, cx, cy, half, ls, 4);
}

}  // namespace

TEST_CASE("The disc focus verdict is the same at 1x, 1.5x, 2x and 3x the image scale, Bayer and mono (review round 2, I2)", "[engine]") {
    // The 2026-01-30 Jupiter's in-focus limb (sigma / R = 0.16 at R 7.9 luma px), 3x it (warned) and
    // 12x it (a blob: warned): blur and radius both scaled, the same sensor noise, pixel-integrated.
    struct Level {
        double ratio;
        stack::FocusOutcome expect;
    };
    for (bool bayer : {true, false})
        for (const Level& lv : {Level{0.16, stack::FocusOutcome::InFocus}, Level{0.5, stack::FocusOutcome::Warned},
                                Level{2.0, stack::FocusOutcome::Warned}}) {
            for (double m : {1.0, 1.5, 2.0, 3.0}) {
                const stack::FocusCheck f = DiscVerdictAtScale(bayer, m, 7.9, lv.ratio * 7.9, 61);
                INFO((bayer ? "Bayer" : "mono") << " scale " << m << ", true sigma/R " << lv.ratio << ": measure " << f.measure
                                                << ", radius " << f.radius << ", limb " << f.limbBlur << ", note " << f.note);
                CHECK(f.outcome == lv.expect);
                // The reading tracks the true sigma / R -- the model strips the pixel aperture -- within 10%
                // up to the blob regime; past it the half-level radius inflates and the reading saturates.
                if (lv.ratio < 1) CHECK(f.measure == Catch::Approx(lv.ratio).epsilon(0.10));
                else CHECK(f.measure < 1.0);
            }
        }
}

TEST_CASE("The disc model inversion is fast for a large disc and cancellable (review round 2, I1)", "[engine]") {
    const double radius = 228;
    const int n = 1024;
    const double detail = stack::DiscDetailScale(radius, 6.0, n);
    const auto t0 = std::chrono::steady_clock::now();
    const double sigma = stack::LimbBlurFromDetail(detail, radius, n);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    INFO("R 228, n 1024: inverted in " << secs << " s, sigma " << sigma);
    CHECK(sigma == Catch::Approx(6.0).margin(0.01));
    CHECK(secs < 1.0);
    int polls = 0;
    const std::function<void()> poll = [&] {
        if (++polls >= 3) throw stack::StackCancelled("cancelled while judging focus");
    };
    CHECK_THROWS_AS(stack::LimbBlurFromDetail(detail, radius, n, poll), stack::StackCancelled);
}

TEST_CASE("A disc whose halo does not fit the frame is not judged, with the reason (review round 2, minor)", "[engine]") {
    // A disc of radius 30 luma px in a 96-px frame: 4 R + 16 = 136 does not fit; 2 R + 16 = 76 does, so it is
    // measured, but a blur whose 3-sigma halo leaves the window is not judged.
    const int n = 96;
    std::vector<FloatImage> lumas;
    std::vector<stack::Shift> shifts;
    for (int k = 0; k < 8; ++k) {
        FloatImage im{n, n, 1, std::vector<float>(static_cast<size_t>(n) * n)};
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
                im.data[static_cast<size_t>(y) * n + x] =
                    static_cast<float>(0.03 + 0.5 * 0.5 * std::erfc((std::hypot(x - 48, y - 48) - 30) / (std::sqrt(2.0) * 8)));
        lumas.push_back(std::move(im));
        shifts.push_back(stack::Shift{});
    }
    const stack::FocusWindow win = stack::DiscFocusWindow(48, 48, 30, n, n);
    CHECK(win.side < 4 * 30 + 16);
    const auto prof = stack::MeasureFocus(lumas, shifts, {win}, 1);
    const stack::FocusCheck f = stack::JudgeDiscFocus(prof.front(), 30, 1, win);
    INFO("outcome " << static_cast<int>(f.outcome) << ": " << f.note);
    CHECK(f.outcome == stack::FocusOutcome::NotJudged);
    CHECK(f.note.find("does not fit") != std::string::npos);
    // ... while a frame with no room even for the disc and 8 px of sky gives no window at all.
    CHECK(stack::DiscFocusWindow(48, 48, 44, n, n).side == 0);
}

namespace {

// Mono luma frames of a disc of radius R blurred by s (erfc edge, 4 x 4 pixel-integrated), 16 frames
// with jitter, sky 0.02 and noise -- the review's scan (round 3).
stack::FocusCheck ScanDisc(int n, double R, double s, double noise) {
    std::mt19937 rng(5);
    std::normal_distribution<double> nd(0, 1);
    std::vector<FloatImage> lumas;
    std::vector<stack::Shift> shifts;
    const double c = n / 2.0;
    for (int k = 0; k < 16; ++k) {
        const double dx = 0.7 * nd(rng), dy = 0.7 * nd(rng);
        FloatImage im{n, n, 1, std::vector<float>(static_cast<size_t>(n) * n)};
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                double acc = 0;
                for (int j = 0; j < 4; ++j)
                    for (int i = 0; i < 4; ++i) {
                        const double r = std::hypot(x - 0.5 + (i + 0.5) / 4 - dx - c, y - 0.5 + (j + 0.5) / 4 - dy - c);
                        acc += 0.25 * std::erfc((r - R) / (std::sqrt(2.0) * std::max(s, 1e-3)));
                    }
                im.data[static_cast<size_t>(y) * n + x] = static_cast<float>(0.02 + acc / 16 + noise * nd(rng));
            }
        lumas.push_back(std::move(im));
        shifts.push_back(stack::Shift{dx, dy, 1});
    }
    const double rad = stack::DiscRadius(lumas[0], c + shifts[0].dx, c + shifts[0].dy, R + 2 * s);
    return stack::MeasureDiscFocus(lumas, shifts, c, c, rad, 1, 8);
}

}  // namespace

TEST_CASE("No region-stage disc verdict refuses: faint blobs and a clamped inversion are warned or not judged (Ruling P20)", "[engine]") {
    CHECK(std::isinf(stack::kDiscRefuseFrom));
    struct Case {
        int n;
        double R, s, noise;
    };
    // The review's refusals (round 3): tiny discs blurred past their radius, read 0.78-2.8.
    for (const Case& c : {Case{106, 0.8, 8, 0.005}, Case{106, 1.0, 8, 0.005}, Case{107, 1.5, 8, 0.005}, Case{92, 2.0, 6, 0.005},
                          Case{90, 0.8, 6, 0.08}}) {
        const stack::FocusCheck f = ScanDisc(c.n, c.R, c.s, c.noise);
        INFO("R " << c.R << " s " << c.s << " noise " << c.noise << ": outcome " << static_cast<int>(f.outcome) << ", measure " << f.measure
                  << ", limb " << f.limbBlur << ", note " << f.note);
        CHECK((f.outcome == stack::FocusOutcome::Warned || f.outcome == stack::FocusOutcome::NotJudged));
    }
    // ... and the one that hit the inversion's limit (limb 23.9 px in a 32-px window): not judged, with why.
    const stack::FocusCheck clamped = ScanDisc(71, 1.5, 3.5, 0.08);
    INFO("clamped: outcome " << static_cast<int>(clamped.outcome) << ", measure " << clamped.measure << ", note " << clamped.note);
    CHECK(clamped.outcome == stack::FocusOutcome::NotJudged);
    CHECK_FALSE(clamped.note.empty());
}
