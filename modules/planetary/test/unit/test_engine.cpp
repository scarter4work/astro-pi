#include <catch_amalgamated.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "AviWriter.h"
#include "Engine.h"
#include "Noise.h"
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
    // A mono capture has no demosaic border, so no guard band (Ruling R13; Accumulate.h, "The frame's border"): every
    // pixel some frame reaches in-frame -- all of them, at 2 px rms of drift -- and the full frame, uncropped.
    const stack::StackCrop& crop = o.summary.crop;
    CHECK(crop.guard == 0);
    CHECK(crop.left + crop.top + crop.right + crop.bottom == 0);
    CHECK(crop.uncovered == 0);
    CHECK(o.image->width == w);
    CHECK(o.image->height == h);
    CHECK(o.summary.alignment.find("cropped") == std::string::npos);
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
    // The dry run also counts the alignment pass's reads (the worker reads of the aligning phase): PlanRegions
    // runs after them and before the focus measure's reads, reporting past refUnits + those frames.
    int64_t alignReads = 0;
    {
        ReaderLedger ledger;
        int64_t workerAtAligning = -1;
        stack::Progress p;
        p.report = [&](const std::string& phase, int64_t, int64_t) {
            if (phase == stack::kPhaseAligning && workerAtAligning < 0) workerAtAligning = ledger.workerReads.load();
            if (phase == stack::kPhaseStacking && alignReads == 0) alignReads = ledger.workerReads.load() - workerAtAligning;
        };
        const StackOutcome dry = stack::RunStack(Counted(cap.Factory(), ledger), 6, p);
        REQUIRE(dry.frames.stacked.size() >= static_cast<size_t>(stack::kFocusFrames));
        REQUIRE(alignReads >= static_cast<int64_t>(dry.frames.stacked.size()));
    }
    struct Case {
        const char* where;
        std::string phase;
    };
    for (const Case& c : {Case{"measuring", stack::kPhaseMeasuring}, Case{"building the reference", stack::kPhaseAligning},
                          Case{"the alignment pass", stack::kPhaseAligning}, Case{"planning regions", stack::kPhaseAligning},
                          Case{"the focus measure's reads", stack::kPhaseAligning},
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
            // Final review, engine M4: PlanRegions polls -- its progress has moved past the alignment pass's units
            // (3 per reference frame, 1 per aligned frame) and no focus frame has been read.
            else if (where == "planning regions")
                fired = worker >= alignReads && caller == stack::kReferenceFrames && done > 3 * stack::kReferenceFrames + alignReads;
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
        std::string message;
        try {
            stack::RunStack(factory, 6, progress);
        } catch (const stack::StackCancelled& e) {
            message = e.what();
        }
        CHECK_FALSE(message.empty());  // StackCancelled was thrown
        CHECK(fired);
        if (where == "planning regions") {
            CHECK(message == "cancelled while planning regions");  // PlanRegions' own poll, not the phase's next check
            CHECK(callerAtFire == stack::kReferenceFrames);       // no focus read yet
        }
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

TEST_CASE("PlanRegions reports its progress and honours cancel from the calling thread (final review, engine M4)", "[engine]") {
    // 64 regions on an 8 x 8 grid, 400 frames whose quality varies (each region ranks its own frames).
    stack::RegionLayout lay;
    lay.width = lay.height = 288;
    lay.size = 64;
    lay.step = 32;
    lay.nx = lay.ny = 8;
    for (int j = 0; j < 8; ++j)
        for (int i = 0; i < 8; ++i) {
            lay.regions.push_back({i * 32, j * 32, i * 32 + 64, j * 32 + 64, 0.01, i, j});
            lay.node.push_back(j * 8 + i);
        }
    std::mt19937 rng(17);
    std::normal_distribution<double> n(0, 1);
    std::vector<stack::FrameRegions> measured(400);
    for (int k = 0; k < 400; ++k) {
        measured[static_cast<size_t>(k)].frameIndex = k;
        for (int r = 0; r < 64; ++r) {
            stack::RegionMeasure q;
            q.aligned = true;
            q.confidence = 0.95f;
            q.quality = static_cast<float>(0.3 * std::exp(0.3 * n(rng)));
            q.qualitySigma = 0.01f;
            q.quality2Sigma = 2 * q.quality * q.qualitySigma;
            measured[static_cast<size_t>(k)].regions.push_back(q);
        }
    }
    const auto caller = std::this_thread::get_id();
    std::vector<int64_t> done;
    bool offThread = false, badTotal = false;
    stack::Progress progress;
    progress.report = [&](const std::string&, int64_t d, int64_t t) {
        offThread |= std::this_thread::get_id() != caller;
        badTotal |= t != 64;
        done.push_back(d);
    };
    progress.cancelled = [&] {
        offThread |= std::this_thread::get_id() != caller;
        return false;
    };
    const stack::RegionPlan plan = stack::PlanRegions(lay, measured, 4, progress);
    CHECK_FALSE(offThread);
    CHECK_FALSE(badTotal);
    INFO("reports " << done.size());
    REQUIRE(done.size() >= 3);  // the start, at least one poll while ranking, the end
    CHECK(done.front() == 0);
    CHECK(done.back() == 64);
    CHECK(std::is_sorted(done.begin(), done.end()));
    const stack::RegionPlan one = stack::PlanRegions(lay, measured, 1);  // the same for any count, with or without hooks
    CHECK(plan.flags == one.flags);
    CHECK(plan.stackedCount == one.stackedCount);

    // A cancel at the first poll while the workers rank: StackCancelled, every worker joined.
    const int threadsBefore = SettledThreadCount();
    int reports = 0;
    stack::Progress cancel;
    cancel.report = [&](const std::string&, int64_t, int64_t) { ++reports; };
    cancel.cancelled = [&] { return reports >= 2; };  // the first report is (0, 64), before the workers start
    CHECK_THROWS_AS(stack::PlanRegions(lay, measured, 4, cancel), stack::StackCancelled);
    CHECK(reports >= 2);
    CHECK(SettledThreadCount() == threadsBefore);
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
    CHECK(raw.summary.crop.guard == 0);  // stacked as mono: no demosaic border, no guard band (Ruling R13)
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
    CHECK(colour.image->width == w - colour.summary.crop.left - colour.summary.crop.right);  // less the crop (Task A2)
    CHECK(colour.summary.crop.guard == stack::kFrameGuard);  // declared Bayer: the demosaic's border is guarded (R13)
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
// sensor px, `frames` frames, the blur jittering by a log-sd of `jitter` per 40-px cell of each frame.
Capture BayerDisc(int w, int h, double radiusLuma, double blur, unsigned seed, double jitter = 0.05, int frames = 40) {
    const double cx = w / 2.0 + 0.3, cy = h / 2.0 - 0.4;
    const Scene s = testing::MakeDisc(seed, cx, cy, 2 * radiusLuma);
    Capture cap = testing::NewCapture(w, h, ser::ColorId::BayerRGGB);
    std::mt19937 rng(seed + 7);
    std::normal_distribution<double> n(0, 1);
    RenderMosaic(cap, s, frames, [&](int) {
        FrameSpec f;
        f.gx = 1.2 * n(rng);
        f.gy = 1.2 * n(rng);
        f.hasLocal = true;
        f.dx = testing::RandomField(rng, w, h, 40, [] { return 0.0; });
        f.dy = testing::RandomField(rng, w, h, 40, [] { return 0.0; });
        f.sigma = testing::RandomField(rng, w, h, 40, [&] { return blur * std::exp(jitter * n(rng)); });
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
        // over kGrossBlurLimit (Select.h step 7) -- the one refusal for a disc's gross defocus.
        auto blurOverRadius = [](const StackOutcome& o) {
            for (const auto& [name, v] : o.diagnosis.numbers)
                if (name == "blur / radius") return v;
            return 0.0;
        };
        // The capture as it comes: 40 frames, its blur jittering 5% per 40-px cell. Refused, and worded as the
        // data allow: with qualitySigma the frames' real scatter (Ruling P26) 40 frames cannot rule out a
        // spread over kSteadySpread (6-8%), so "focus or seeing" -- the hedged verdict, ruled acceptable
        // (P26 fix round 2; the "optics" wording before P26 came from an overstated qualitySigma).
        {
            const StackOutcome o = stack::RunStack(BayerDisc(320, 288, 8, 32, 82).Factory(), 8, {});
            INFO(o.diagnosis.message << " | " << o.summary.selection);
            CHECK(o.diagnosis.kind == DiagnosisKind::OutOfFocus);
            CHECK(o.diagnosis.message.find("Too blurred: even the sharpest frames blur the limb by") == 0);
            CHECK(o.diagnosis.message.find("focus or seeing, the measurement noise is too large to tell") != std::string::npos);
            CHECK(blurOverRadius(o) >= stack::kGrossBlurLimit);
            CHECK_FALSE(o.image.has_value());
        }
        // A steady defocus over enough frames to show it: 200 frames of one blur read a spread of 0%, at most 3%
        // -- the confident verdict.
        {
            const StackOutcome o = stack::RunStack(BayerDisc(320, 288, 8, 32, 82, 0.0, 200).Factory(), 8, {});
            INFO(o.diagnosis.message << " | " << o.summary.selection);
            CHECK(o.diagnosis.kind == DiagnosisKind::OutOfFocus);
            CHECK(o.diagnosis.message.find("Out of focus") == 0);
            CHECK(o.diagnosis.message.find("the optics, not the air") != std::string::npos);
            CHECK(blurOverRadius(o) >= stack::kGrossBlurLimit);
            CHECK_FALSE(o.image.has_value());
        }
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

TEST_CASE("RunStack: a small selection whose intact frames align is stacked, not refused as Unaligned (final review I1)",
          "[engine]") {
    // A poor night in miniature: 10 frames of one lunar field, 5 sharp and 5 blurred, so Select keeps as few as
    // need (5). Two of the sharp frames are torn through the middle by 8 px: they fail alignment. The 3
    // intact frames align at ~1.00 -- the frames show one scene, so the 3 that align are stacked (Ruling P15: a
    // false refusal leaves the user nothing). 3 < need = 5 refused it as "Unaligned".
    const int w = 160, h = 128;
    const Scene s = testing::MakeSurface(4242, w, h);
    Capture cap = testing::NewCapture(w, h);
    std::mt19937 rng(4243);
    std::normal_distribution<double> n(0, 1);
    const std::set<int64_t> torn{1, 3};
    testing::RenderCapture(cap, s, 10, [&](int k) {
        FrameSpec f;
        f.gx = 1.0 * n(rng);
        f.gy = 1.0 * n(rng);
        f.hasLocal = true;
        f.dx = testing::RandomField(rng, w, h, 40, [] { return 0.0; });
        f.dy = testing::RandomField(rng, w, h, 40, [] { return 0.0; });
        const double blur = k < 5 ? 0.7 : 4.0;
        f.sigma = testing::RandomField(rng, w, h, 40, [blur] { return blur; });
        if (torn.count(k)) {
            f.tearRow = h / 2;
            f.tearDx = k == 1 ? 8 : -8;
        }
        return f;
    });
    const StackOutcome o = stack::RunStack(cap.Factory(), 8, {});
    INFO(o.diagnosis.message << " | " << o.summary.selection << " | " << o.summary.alignment);
    REQUIRE(o.diagnosis.kind == DiagnosisKind::None);
    REQUIRE(o.image.has_value());
    CHECK(o.frames.alignmentExcluded == std::vector<int64_t>{1, 3});
    for (int64_t f : o.frames.stacked) {
        CHECK(torn.count(f) == 0);
        CHECK(f < 5);  // only the sharp selection
    }
    CHECK(o.summary.used == 3);
    CheckFinalLines(o);
    // Engine M1: the selection line counts the frames SELECTED (5); the frames stacked (3) are summary.used.
    CHECK(o.summary.selection.rfind("selected 5 of 10 frames: ", 0) == 0);
    CHECK(o.summary.selection.find("used") == std::string::npos);
}

namespace {
// A lunar capture of n frames, the first nSharp sharp (0.7 px) and the rest blurred (4 px), so Select keeps the sharp
// ones; frame k in `tears` is torn: its rows from tears[k].first down are displaced by tears[k].second px. The tears
// differ in row and size, as a shake during readout does, so no two torn frames agree with each other.
Capture TornCapture(int n, int nSharp, const std::map<int, std::pair<int, double>>& tears, unsigned seed) {
    const int w = 160, h = 128;
    const Scene s = testing::MakeSurface(seed, w, h);
    Capture cap = testing::NewCapture(w, h);
    std::mt19937 rng(seed + 1);
    std::normal_distribution<double> nd(0, 1);
    testing::RenderCapture(cap, s, n, [&](int k) {
        FrameSpec f;
        f.gx = 1.0 * nd(rng);
        f.gy = 1.0 * nd(rng);
        f.hasLocal = true;
        f.dx = testing::RandomField(rng, w, h, 40, [] { return 0.0; });
        f.dy = testing::RandomField(rng, w, h, 40, [] { return 0.0; });
        const double blur = k < nSharp ? 0.7 : 4.0;
        f.sigma = testing::RandomField(rng, w, h, 40, [blur] { return blur; });
        const auto t = tears.find(k);
        if (t != tears.end()) {
            f.tearRow = t->second.first;
            f.tearDx = t->second.second;
        }
        return f;
    });
    return cap;
}
}  // namespace

TEST_CASE("RunStack: torn frames are excluded even when they are most of the selection (bench6 step 1)", "[engine]") {
    // AlignmentCut is relative to the selection's median match. When most selected frames are torn, the reference
    // averages their displaced parts and the median falls with them (measured at 9cf7f5b6: 3 of 5 torn -> median 0.87,
    // cut 0.74; 7 of 12 -> median 0.84), so every torn frame passed and was stacked. A torn frame has to be refused on
    // its own evidence, not by comparison with the rest.
    SECTION("most of a small selection torn: the rest are too few, the capture is Unaligned (I1's rule)") {
        const std::map<int, std::pair<int, double>> tears{{1, {64, 8.0}}, {2, {52, -7.0}}, {3, {76, 9.0}}};
        const StackOutcome o = stack::RunStack(TornCapture(10, 5, tears, 5151).Factory(), 8, {});
        INFO(o.diagnosis.message << " | " << o.summary.selection << " | " << o.summary.alignment);
        for (int64_t f : o.frames.stacked) CHECK(tears.count(static_cast<int>(f)) == 0);
        CHECK(o.diagnosis.kind == DiagnosisKind::Unaligned);
        CHECK(o.diagnosis.message.find("only 2 of 5 selected frames") != std::string::npos);
        CHECK(o.diagnosis.message.find(" torn: part of the frame displaced by more than 3 px)") != std::string::npos);
        CHECK_FALSE(o.image.has_value());
    }
    SECTION("most of a larger selection torn: the intact frames that remain are stacked, the torn ones are not") {
        // 13 sharp frames, 7 of them torn; the 6 intact are over need (5), so they are stacked (I1: Unaligned only
        // when most of the selection fails AND fewer than need align).
        const std::map<int, std::pair<int, double>> tears{{0, {64, 8.0}},  {1, {50, -8.0}}, {3, {70, 7.0}},
                                                          {4, {58, -9.0}}, {6, {44, 8.0}},  {8, {80, -7.0}},
                                                          {11, {66, 10.0}}};
        const StackOutcome o = stack::RunStack(TornCapture(26, 13, tears, 6161).Factory(), 8, {});
        INFO(o.diagnosis.message << " | " << o.summary.selection << " | " << o.summary.alignment);
        REQUIRE(o.diagnosis.kind == DiagnosisKind::None);
        REQUIRE(o.image.has_value());
        CHECK(o.summary.selection.rfind("selected 13 of 26 frames: ", 0) == 0);
        CHECK(o.frames.stacked == std::vector<int64_t>{2, 5, 7, 9, 10, 12});
        CHECK(o.frames.alignmentExcluded == std::vector<int64_t>{0, 1, 3, 4, 6, 8, 11});
        CHECK(o.summary.used == 6);
        CheckFinalLines(o);
    }
}

TEST_CASE("RunStack KNOWN LIMIT (Ruling F11): a majority torn alike becomes the reference and the intact frames are "
          "dropped",
          "[engine]") {
    // This pins a limit, not a wanted result. BuildReference keeps the largest group of frames that agree. Torn
    // frames agree with each other within kMaxTear when they are torn the same way -- the same direction by a
    // similar amount -- even through different rows, so a majority torn alike outvotes the intact frames: it becomes
    // the reference, and the intact frames read torn against it and are left out. Which group is "the scene" is a
    // design choice the spec does not settle; never seen in real data (0 of 37,773 frames, bench6 re-review N1).
    // If the engine learns to tell the intact group apart, this test must fail and be rewritten as a fix.
    const std::set<int64_t> torn{1, 2, 3};
    SECTION("3 of 5 sharp frames torn by +8 px through the same row") {
        const std::map<int, std::pair<int, double>> tears{{1, {64, 8.0}}, {2, {64, 8.0}}, {3, {64, 8.0}}};
        const StackOutcome o = stack::RunStack(TornCapture(10, 5, tears, 5151).Factory(), 8, {});
        INFO(o.diagnosis.message << " | " << o.summary.selection << " | " << o.summary.alignment);
        REQUIRE(o.diagnosis.kind == DiagnosisKind::None);
        REQUIRE(o.image.has_value());
        CHECK(o.summary.selection.rfind("selected 5 of 10 frames: ", 0) == 0);
        CHECK(o.frames.stacked == std::vector<int64_t>{1, 2, 3});
        CHECK(o.frames.alignmentExcluded == std::vector<int64_t>{0, 4});
        CHECK(o.summary.used == 3);
    }
    SECTION("3 of 5 sharp frames torn by +8 px through different rows") {
        const std::map<int, std::pair<int, double>> tears{{1, {60, 8.0}}, {2, {64, 8.0}}, {3, {70, 8.0}}};
        const StackOutcome o = stack::RunStack(TornCapture(10, 5, tears, 5151).Factory(), 8, {});
        INFO(o.diagnosis.message << " | " << o.summary.selection << " | " << o.summary.alignment);
        REQUIRE(o.diagnosis.kind == DiagnosisKind::None);
        REQUIRE(o.image.has_value());
        CHECK(o.summary.selection.rfind("selected 5 of 10 frames: ", 0) == 0);
        CHECK(o.frames.stacked == std::vector<int64_t>{1, 2, 3});
        CHECK(o.frames.alignmentExcluded == std::vector<int64_t>{0, 4});
        CHECK(o.summary.used == 3);
    }
}

TEST_CASE("UnalignedBelow: half of a small selection, need for a large one; never reached with every clean frame over the floor",
          "[engine]") {
    CHECK(stack::UnalignedBelow(10, 5) == 3);       // the case above: 3 of 5 align -> stacked
    CHECK(stack::UnalignedBelow(10, 6) == 3);
    CHECK(stack::UnalignedBelow(2000, 9) == 5);     // ceil(9 / 2) = 5 = need
    CHECK(stack::UnalignedBelow(2000, 1500) == 5);  // need = kMinStackFrames
    CHECK(stack::UnalignedBelow(3, 2) == 1);        // need = 2, ceil(2 / 2) = 1
    CHECK(stack::UnalignedBelow(1, 1) == 1);
    CHECK(stack::UnalignedBelow(0, 0) == 1);
    // Engine.h's claim: with every clean confidence >= kMinAlignConfidence, at least ceil(S / 2) pass AlignmentCut,
    // so the diagnosis cannot fire (random confidences over the floor, 1-40 frames).
    std::mt19937 rng(77);
    std::uniform_real_distribution<double> u(stack::kMinAlignConfidence, 1.0);
    for (int trial = 0; trial < 2000; ++trial) {
        const int s = 1 + trial % 40;
        std::vector<stack::Shift> shifts(static_cast<size_t>(s));
        for (auto& sh : shifts) sh.confidence = u(rng);
        const double cut = stack::AlignmentCut(shifts);
        int64_t pass = 0;
        for (const auto& sh : shifts) pass += sh.confidence >= cut;
        INFO("trial " << trial << ": " << pass << " of " << s << " pass cut " << cut);
        REQUIRE(pass >= (s + 1) / 2);
        REQUIRE(pass >= stack::UnalignedBelow(2000, s));
    }
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

TEST_CASE("PlanMemory counts the noise model's scratch: the quality's variance and the noise phase (Ruling P26 fix round 1)",
          "[engine]") {
    auto info = [](int w, int h) {
        ser::FileInfo i;
        i.header.width = w;
        i.header.height = h;
        i.colorId = ser::ColorId::Mono;
        i.normDepth = 16;
        i.planes = 1;
        i.bytesPerSample = 2;
        return i;
    };
    // 4K mono (8 294 400 luma px): measuring holds 4 + 17 per luma px (16 until E2 counted the luma's clipped
    // samples, 1 per px) and PixelVariance's rows of doubles
    // (the quality's variance over the box: 4 per px); aligning 2 + 12 and the rows (the regions'
    // variance over their extent), plus the correlation scratch. 941ab4e7 counted 4 + 12 and 2 + 8, and
    // held a whole-frame variance (4 per px) and a double plane (8 per px) on every worker.
    const size_t px = 3840ull * 2160, rows = 3 * 8 * 3840;
    const stack::MemoryPlan p = stack::PlanMemory(info(3840, 2160), 32);
    CHECK(p.measureWorkerBytes == 4 * px + 17 * px + rows);
    CHECK(p.alignWorkerBytes == 2 * px + 12 * px + rows + stack::kWorkerScratchBytes);
    // The noise phase samples every 3rd centre there: four NoisePlanes of 8 bytes per sampled centre.
    const size_t sampled = 1279ull * 719;
    CHECK(p.noiseWorkerBytes == 2 * px + 4 * px + 37 * sampled + 65536);
    CHECK(p.noiseWorkerBytes <= p.measureWorkerBytes);
    const stack::NoisePlane plane(ser::FloatImage{3840, 2160, 1, std::vector<float>(px, 0.3f)});
    CHECK(plane.response.size() + plane.level.size() == 2 * sampled);
    // A small frame: the noise phase's planes are what a measuring worker holds at most.
    const stack::MemoryPlan s = stack::PlanMemory(info(64, 64), 32);
    CHECK(s.measureWorkerBytes == s.noiseWorkerBytes);
    CHECK(s.noiseWorkerBytes == 2 * 4096 + 4 * 4096 + 37 * 60 * 60 + 65536);
}

TEST_CASE("PlanMemory counts what grows with frames x regions: the measurements, the plan (held once), PlanRegions' scratch (final review, engine M3)",
          "[engine]") {
    ser::FileInfo info;
    info.header.width = 3840;
    info.header.height = 2160;
    info.colorId = ser::ColorId::Mono;
    info.normDepth = 16;
    info.planes = 1;
    info.bytesPerSample = 2;
    info.frameCount = 10000;
    // The largest 4K mono layout: kMaxRegionSize windows at half overlap over the whole frame.
    const size_t regions = 59 * 33;
    const stack::MemoryPlan before = stack::PlanMemory(info, 32), p = stack::PlanMemory(info, 32, regions);
    // Before the layout is placed only the per-frame parts are counted.
    CHECK(before.measureStoreBytes == 10000 * (2 * sizeof(stack::FrameRegions) + sizeof(stack::Shift) + 2));
    CHECK(before.planBytes == 10000 * (sizeof(int64_t) + sizeof(stack::Shift) + sizeof(float) + 1));
    CHECK(before.planScratchBytes == 0);
    // The per-entry sizes the comments quote, measured.
    CHECK(sizeof(stack::RegionMeasure) == 28);
    CHECK(p.measureStoreBytes ==
          10000 * (regions * sizeof(stack::RegionMeasure) + 2 * sizeof(stack::FrameRegions) + sizeof(stack::Shift) + 2));
    CHECK(p.measureStoreBytes >= 545000000);  // 10 000 x 1947 x 28 = 545 MB
    CHECK(p.planBytes >= 10000 * regions * 9);  // 175 MB
    CHECK(p.planScratchBytes == stack::PlanRegionsWorkerBytes(10000));  // 664 per frame (test_plan_memory measures it)
    CHECK(p.planScratchBytes == 664 * 10000);
    CHECK(p.planWorkers >= 1);
    CHECK(p.planWorkers <= p.alignWorkers);
    // The real storage of one frame's measurements: its FrameRegions and the region array it owns.
    std::vector<stack::FrameRegions> real(2);
    for (auto& f : real) f.regions.resize(regions);
    size_t realBytes = real.capacity() * sizeof(stack::FrameRegions);
    for (const auto& f : real) realBytes += f.regions.capacity() * sizeof(stack::RegionMeasure);
    CHECK(realBytes <= 2 * (regions * sizeof(stack::RegionMeasure) + 2 * sizeof(stack::FrameRegions)));
    // Counted, they cap the alignment pass's workers and keep the peak within the budget; measuring is unchanged.
    CHECK(p.measureWorkers == before.measureWorkers);
    CHECK(p.alignWorkers < before.alignWorkers);
    // Where the budget binds (32 threads) the counted bytes show as fewer workers (above), and the peak -- the
    // largest pass, each filled to the budget a worker at a time -- need not rise. PlanMemory at fix round 2 of
    // fix/planetary-edge (per-channel sums, Accumulate.h "Clipping"): with regions the peak is the alignment pass's
    // 3.903 GB (21 workers), under its 3.981 GB without (26 workers); at 3123eea8 it passed by
    // 3.4 MB of the stacking pass's worker rounding (review, Q3). On one thread nothing is capped and the peak rises
    // by what the regions add (0.773 -> 1.137 GB, same method).
    CHECK(stack::PlanMemory(info, 1, regions).peakBytes > stack::PlanMemory(info, 1).peakBytes);
    CHECK(p.peakBytes <= stack::kMemoryBudget);
    for (int threads : {1, 8, 64, 256}) {
        const stack::MemoryPlan q = stack::PlanMemory(info, threads, regions);
        INFO(threads << " threads: peak " << q.peakBytes);
        CHECK(q.peakBytes <= stack::kMemoryBudget);
    }
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
    // A region keeping no frame had `candidates` frames aligned there with detail: the noise gate's (P27) when
    // there were enough to rank, too few otherwise (PlanRegions sets tooFewFrames the same way).
    auto detail = [](std::vector<int64_t> kept, int64_t candidates = 300) {
        std::vector<stack::RegionDetail> d;
        for (int64_t k : kept) {
            stack::RegionDetail r;
            r.kept = k;
            r.candidates = k > 0 ? k : candidates;
            r.noiseOnly = k == 0;
            r.tooFewFrames = k == 0 && r.candidates < stack::kMinStackFrames;
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
    CHECK(stack::RegionPhrase({}, 1, 1) == "no region holds detail to rank on, so the 1 aligned frame is stacked whole");
    // Final review, engine M2: a region with fewer than kMinStackFrames candidates is not blamed on noise.
    CHECK(stack::RegionPhrase(detail({0, 0, 0}, 3), 3, 3) ==
          "3 regions placed on the target's detail: 3 with too few frames aligned there to rank (under 5) taking the mean "
          "of the 3 stacked frames");
    CHECK(stack::RegionPhrase(detail({0}, 1), 1, 1) ==
          "1 region placed on the target's detail: 1 with too few frames aligned there to rank (under 5) taking the mean "
          "of the 1 stacked frame");
    std::vector<stack::RegionDetail> both = detail({120, 0}, 300);
    const std::vector<stack::RegionDetail> few = detail({0}, 2);
    both.insert(both.end(), few.begin(), few.end());
    CHECK(stack::RegionPhrase(both, 300, 300) ==
          "3 regions placed on the target's detail: 1 stacking their own sharpest 120 of 300 frames, 1 with no detail above "
          "the measurement noise and 1 with too few frames aligned there to rank (under 5) taking the mean of the 300 "
          "stacked frames");
}

TEST_CASE("RunStack: a 3-frame capture's regions are too few frames to rank, not noise (final review, engine M2)", "[engine]") {
    const Capture cap = DiscCapture(160, 128, 80.3, 63.7, 30, 3, 91, 1.0);
    const StackOutcome o = stack::RunStack(cap.Factory(), 4, {});
    INFO(o.diagnosis.message << " | " << o.summary.alignment);
    REQUIRE(o.image.has_value());
    REQUIRE(o.frames.regions > 0);
    for (const stack::RegionDetail& d : o.frames.regionDetail) {
        CHECK(d.kept == 0);
        CHECK((d.skyOnly || d.tooFewFrames));
    }
    CHECK(o.summary.alignment.find("with too few frames aligned there to rank (under 5) taking the mean of the 3 stacked frames") !=
          std::string::npos);
    CHECK(o.summary.alignment.find("measurement noise") == std::string::npos);
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

TEST_CASE("The region phrase does not count sky-only regions as placed on the target's detail (E1 fix round 2)", "[engine]") {
    // Review N4: a region whose whole window lies where the blend mask is 0 still ranks and keeps frames (they
    // join the fallback's), but its stack never reaches the image. Three regions in a row; the mask is 0 over
    // the first one's window and 1 elsewhere.
    stack::RegionLayout lay;
    lay.width = 64;
    lay.height = 32;
    lay.size = 32;
    lay.step = 16;
    lay.nx = 3;
    lay.ny = 1;
    lay.flatEdges = true;
    for (int i = 0; i < 3; ++i) {
        lay.regions.push_back({i * 16, 0, i * 16 + 32, 32, 0.01, i, 0});
        lay.node.push_back(i);
    }
    auto m = std::make_shared<std::vector<float>>(64 * 32, 1.f);
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) (*m)[static_cast<size_t>(y) * 64 + x] = 0.f;
    lay.mask.width = 64;
    lay.mask.height = 32;
    lay.mask.m = m;
    std::mt19937 rng(9);
    std::normal_distribution<double> n(0, 1);
    std::vector<stack::FrameRegions> measured;
    for (int k = 0; k < 40; ++k) {
        stack::FrameRegions f;
        f.frameIndex = k;
        for (int r = 0; r < 3; ++r) {
            stack::RegionMeasure q;
            q.aligned = true;
            q.confidence = 0.95f;
            q.quality = static_cast<float>(0.3 * std::exp(0.4 * n(rng)));
            q.qualitySigma = 0.001f;
            f.regions.push_back(q);
        }
        measured.push_back(f);
    }
    const stack::RegionPlan plan = stack::PlanRegions(lay, measured);
    for (const auto& d : plan.detail) REQUIRE(d.kept > 0);  // all three rank and keep frames
    const std::string phrase = stack::RegionPhrase(plan.detail, 40, plan.stackedCount);
    INFO(phrase);
    CHECK(phrase.rfind("2 regions placed on the target's detail", 0) == 0);
    CHECK(phrase.find("1 over the sky only") != std::string::npos);
}

TEST_CASE("RunStack: a frame-filling surface drifting off one edge is stacked there from the frames that saw it (field defect D1, "
          "2026-10-23 Moon)",
          "[engine]") {
    // The 2026-10-23 Moon (Scott, PlanetaryStack 0.3.0.0): the capture drifted 30 luma px left, the limb runs off the
    // frame's left edge, and the regions of the left column -- sky on their left, surface on their right -- kept
    // only frames whose window had mostly left the frame (ranked on the part still inside, a different part of the
    // scene per frame). Their pixels near the edge were then made of samples from outside every frame they kept:
    // Moon-coloured staircase triangles in the sky and a smeared bright block on the surface. Here: a surface whose
    // limb leaves sky in the left corners, drifting from +2 to -26 px over the capture.
    const int w = 320, h = 240, n = 60;
    Scene s = testing::MakeSurface(91, w, h);
    s.cx = 300;
    s.cy = 120;
    s.radius = 300;  // touches the left edge at mid-height; sky left of x ~25 at the top and bottom rows
    Capture cap = testing::NewCapture(w, h);
    std::mt19937 rng(9100);
    testing::RenderCapture(cap, s, n, [&](int k) {
        FrameSpec f = testing::Seeing(rng, w, h, 0.5, 40, 1.0);
        f.gx += 2.0 - 28.0 * k / (n - 1);
        return f;
    });
    const StackOutcome o = stack::RunStack(cap.Factory(), 8, {});
    REQUIRE(o.diagnosis.kind == DiagnosisKind::None);
    REQUIRE(o.image.has_value());
    INFO(o.summary.selection << " | " << o.summary.alignment);
    // The band the drift takes out of most frames, against the interior, both over the whole height.
    const double edge = testing::RmsToTruth(*o.image, s, [](int x, int) { return x < 28; });
    const double mid = testing::RmsToTruth(*o.image, s, [](int x, int) { return x >= 140 && x < 168; });
    // The largest error in the band, registered as RmsToTruth registers.
    // The stack is cropped (Engine.h, "The crop"; Task A2): its pixel (x, y) is the reference grid's (x + left, y + top).
    const FloatImage& im = *o.image;
    const int iw = im.width, ih = im.height, ox = o.summary.crop.left, oy = o.summary.crop.top;
    FloatImage truth{iw, ih, 1, std::vector<float>(static_cast<size_t>(iw) * ih)};
    for (int y = 0; y < ih; ++y)
        for (int x = 0; x < iw; ++x) truth.data[static_cast<size_t>(y) * iw + x] = static_cast<float>(s.Eval(x + ox, y + oy, 0));
    const stack::Shift d = stack::GlobalShift(truth, im, stack::MeasureFrame(im, 0, 1), 1);
    double worst = 0;
    for (int y = 0; y < ih; ++y)
        for (int x = 0; x < 28; ++x)
            worst = std::max(worst, std::fabs(im.data[static_cast<size_t>(y) * iw + x] - s.Eval(x + ox - d.dx, y + oy - d.dy, 0)));
    INFO("rms to the truth: left band " << edge << ", interior band " << mid << "; largest error in the band " << worst);
    // Measured at c59e9fd4 (released engine): left band 0.0249 rms against the interior's 0.0064, largest error 0.362
    // -- surface in the sky. Fixed: 0.0095 against 0.0065 (fewer frames reach the band), largest 0.120 -- the limb,
    // blurred by the seeing in the stack and sharp in the truth. Re-measured after Task A2 (fix round 1): 0.00951 against
    // 0.00647, largest 0.120 -- the same, as a mono capture has no guard band and is not cropped here (Ruling R13).
    CHECK(edge < 2 * mid);
    CHECK(worst < 0.2);
}

namespace {

// The 2026-10-23 64.6F Moon's drift (Task A1): (ox, oy) of every 40th of its 2324 raw frames (scratchpad
// raw10.bin.shifts.npy: phase correlation of every 10th raw frame against the 0.3.0.1 stack, every 4th of those kept,
// rounded to 0.01 px); stack column d sits at frame column d + ox. The reference is at the end, so only the late
// frames reach the stack's left and top edges.
constexpr double kDrift646[][2] = {
    {-55.84, -29.27}, {-54.87, -28.52}, {-54.84, -28.09}, {-54.23, -28.47}, {-53.15, -28.04}, {-52.09, -26.04},
    {-52.00, -26.41}, {-50.20, -24.73}, {-49.41, -23.84}, {-48.48, -24.37}, {-48.04, -24.25}, {-46.28, -23.44},
    {-46.21, -23.75}, {-44.98, -22.47}, {-44.23, -21.66}, {-43.67, -21.05}, {-44.13, -20.17}, {-41.83, -20.42},
    {-41.40, -20.13}, {-40.27, -19.99}, {-39.87, -18.07}, {-38.19, -18.30}, {-37.63, -18.08}, {-37.54, -16.82},
    {-35.50, -16.71}, {-33.79, -16.13}, {-32.99, -15.78}, {-30.54, -14.26}, {-29.89, -15.63}, {-29.99, -14.20},
    {-28.90, -12.96}, {-27.51, -13.21}, {-25.64, -12.58}, {-24.94, -11.80}, {-23.99, -10.62}, {-21.87, -11.42},
    {-21.60, -9.89},  {-20.34, -10.21}, {-19.23, -9.01},  {-17.98, -8.47},  {-17.74, -8.02},  {-15.60, -7.06},
    {-13.90, -7.20},  {-13.58, -6.38},  {-13.26, -5.66},  {-11.66, -4.73},  {-10.26, -5.85},  {-10.37, -4.26},
    {-7.97, -3.88},   {-6.12, -3.53},   {-4.97, -2.44},   {-4.44, -2.53},   {-3.22, -1.87},   {-3.03, -1.46},
    {-1.81, -0.63},   {-0.04, -0.07},   {0.43, 1.16},     {1.59, 0.64},     {2.27, 1.73}};

// A chroma-free, white-balanced capture (Task A2, Rulings R8 corrected 2 and R10), framed like the 64.6F Moon: a
// MakeSurface texture L inside a disc of radius 170 centred at (100, 120) of the 320 x 240 frame at the reference --
// it runs off the left, top and bottom edges, sky on the right (a frame-filling target, found the same in every
// frame) -- mosaiced RGGB as R 0.93 L, G L, B 0.70 L (the Moon's colour differences, uniform, so DemosaicHQ is not
// exact on it), plus N(0, 1.25 / 255) per site (the 64.6F's 3.5 DN, scaled to the stack noise of its 2297 frames at
// these few). Seeing blur `blur` px, the last 4 frames `refBlur` (the sharpest, so the reference is at the end, as on
// the 64.6F). drift: kDrift646 interpolated over the capture; else every frame at its last point. Both plus
// N(0, 0.25 px) of jitter. Any chroma in its stack is made by the CFA sampling, the demosaic and the engine.
Capture ChromaFreeCapture(int n, bool drift, double blur, double refBlur) {
    const int w = 320, h = 240;
    Scene s = testing::MakeSurface(646, w + 128, h + 72);
    s.cx = 156;  // scene = frame + (58, 32) - the reference's (2.27, 1.73)
    s.cy = 150;
    s.radius = 170;
    Capture cap = testing::NewCapture(w, h, ser::ColorId::BayerRGGB);
    const int nd = static_cast<int>(sizeof(kDrift646) / sizeof(kDrift646[0]));
    std::mt19937 rng(6460);
    std::normal_distribution<double> jitter(0, 0.25);
    std::vector<double> gx(static_cast<size_t>(n)), gy(gx.size());
    for (int k = 0; k < n; ++k) {
        const double t = drift ? static_cast<double>(k) * (nd - 1) / (n - 1) : nd - 1;
        const int i = std::min(static_cast<int>(t), nd - 2);
        const double f = t - i;
        gx[static_cast<size_t>(k)] = kDrift646[i][0] + f * (kDrift646[i + 1][0] - kDrift646[i][0]) + jitter(rng);
        gy[static_cast<size_t>(k)] = kDrift646[i][1] + f * (kDrift646[i + 1][1] - kDrift646[i][1]) + jitter(rng);
    }
    const double colour[3] = {0.93, 1.0, 0.70};  // R, G, B
    std::vector<std::vector<float>> frames(static_cast<size_t>(n));
    std::vector<std::thread> pool;
    for (int t = 0; t < 16; ++t)
        pool.emplace_back([&, t] {
            for (int k = t; k < n; k += 16) {
                std::mt19937 noise(64600u + k);
                std::normal_distribution<double> nz(0, 1.25 / 255);
                const double b = k >= n - 4 ? refBlur : blur;
                std::vector<float>& v = frames[static_cast<size_t>(k)];
                v.resize(static_cast<size_t>(w) * h);
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x) {
                        const int c = (y & 1) == 0 ? ((x & 1) == 0 ? 0 : 1) : ((x & 1) == 0 ? 1 : 2);
                        // frame(p) = scene(p - g); the scene's origin 58 px left of and 32 px above the frame's, so
                        // every frame of the drift sees texture.
                        const double l = s.Eval(x - gx[static_cast<size_t>(k)] + 58, y - gy[static_cast<size_t>(k)] + 32, b);
                        v[static_cast<size_t>(y) * w + x] = static_cast<float>(colour[c] * l + nz(noise));
                    }
            }
        });
    for (auto& th : pool) th.join();
    for (const auto& f : frames) cap.Add(f);
    return cap;
}

// test/edge_chroma.py's measure on an RGB stack: per pixel B/G and R/G (G >= 0.05), their std over the band [d0, d1)
// px in from `side` (L, T, B), along the side over [t0, t1) (rows for L, columns for T and B). {B/G, R/G}.
std::pair<double, double> BandChroma(const FloatImage& im, char side, int d0, int d1, int t0, int t1) {
    double s[2] = {0, 0}, s2[2] = {0, 0};
    int64_t n = 0;
    for (int d = d0; d < d1; ++d)
        for (int t = t0; t < t1; ++t) {
            const int x = side == 'L' ? d : t;
            const int y = side == 'T' ? d : side == 'B' ? im.height - 1 - d : t;
            const float* p = im.data.data() + (static_cast<size_t>(y) * im.width + x) * 3;
            if (!(p[1] >= 0.05f)) continue;
            for (int c = 0; c < 2; ++c) {
                const double v = static_cast<double>(p[c == 0 ? 2 : 0]) / p[1];
                s[c] += v;
                s2[c] += v * v;
            }
            ++n;
        }
    auto sd = [&](int c) {
        const double m = s[c] / static_cast<double>(n);
        return n < 2 ? std::numeric_limits<double>::quiet_NaN() : std::sqrt(std::max(0.0, s2[c] / static_cast<double>(n) - m * m));
    };
    return {sd(0), sd(1)};
}

// The sides the disc runs off, each with its span on the disc clear of the limb and the corners (the limb, in a band,
// reads as colour: edge_chroma.py's caveat).
struct ChromaSide {
    char side;
    int t0, t1;
};
constexpr ChromaSide kChromaSides[] = {{'L', 40, 200}, {'T', 40, 190}, {'B', 40, 190}};

}  // namespace

TEST_CASE("RunStack: the demosaic's frame border does not reach the stack -- a still capture's edge columns are no more "
          "colourful than its interior (Ruling R10)",
          "[engine]") {
    // DemosaicHQ fills a frame's outermost sites (columns and rows 0-2) with less information than inside: on a textured
    // frame its chroma there departs from the interior's (Task A1 review: crop vs full single real frames, B/G 0.0155 at
    // column 0, 0.004-0.007 at 1-2, 0 from 3). A capture held still puts every frame's border on the stack's edge, so
    // the stack kept it. The chroma-free capture held at one point: no chroma in the light, so each edge column's must
    // be the interior's.
    const Capture cap = ChromaFreeCapture(120, false, 0.45, 0.3);
    const StackOutcome o = stack::RunStack(cap.Factory(), 8, {});
    REQUIRE(o.diagnosis.kind == DiagnosisKind::None);
    REQUIRE(o.image.has_value());
    REQUIRE(o.image->channels == 3);
    const FloatImage& im = *o.image;
    INFO(o.summary.selection << " | " << o.summary.alignment);
    INFO("output " << im.width << " x " << im.height);
    for (const ChromaSide& s : kChromaSides) {
        const auto ref = BandChroma(im, s.side, 20, 40, s.t0, s.t1);
        for (int d = 0; d < 3; ++d) {
            const auto c = BandChroma(im, s.side, d, d + 1, s.t0, s.t1);
            INFO("side " << s.side << ", " << d << " px in: B/G " << c.first << " vs " << ref.first << " ("
                         << c.first / ref.first << "x), R/G " << c.second << " vs " << ref.second << " ("
                         << c.second / ref.second << "x)");
            CHECK(c.first <= 1.5 * ref.first);
            CHECK(c.second <= 1.5 * ref.second);
        }
    }
}

TEST_CASE("RunStack: a chroma-free capture under the 64.6F drift has no more chroma at the stack's edges than inside "
          "(Ruling R10, no regression)",
          "[engine]") {
    // Rulings R8 corrected 2 and R10: under the real drift the engine adds no edge chroma of its own. The guard band and
    // the crop (Accumulate.h, Engine.h) must keep it so: edge_chroma's bands -- the outermost 6 px against 20-40 px in,
    // measured from the output's edges -- on every side the disc runs off. B/G left / top / bottom: 0.89x / 1.00x / 1.10x
    // at 2c406742; 0.76x / 0.88x / 1.17x with the guard band (4 px) and the crop. The ratio follows which texture the
    // bands land on (R8 corrected 2), so it moves by a few tenths as the crop moves the bands (guard 6: top R/G 1.69x).
    const Capture cap = ChromaFreeCapture(300, true, 0.45, 0.3);
    const StackOutcome o = stack::RunStack(cap.Factory(), 8, {});
    REQUIRE(o.diagnosis.kind == DiagnosisKind::None);
    REQUIRE(o.image.has_value());
    REQUIRE(o.image->channels == 3);
    const FloatImage& im = *o.image;
    INFO(o.summary.selection << " | " << o.summary.alignment);
    INFO("output " << im.width << " x " << im.height);
    for (const ChromaSide& s : kChromaSides) {
        const auto edge = BandChroma(im, s.side, 0, 6, s.t0, s.t1), ref = BandChroma(im, s.side, 20, 40, s.t0, s.t1);
        INFO("side " << s.side << ": B/G " << edge.first << " / " << ref.first << " = " << edge.first / ref.first << ", R/G "
                     << edge.second << " / " << ref.second << " = " << edge.second / ref.second);
        CHECK(edge.first <= 1.5 * ref.first);
        CHECK(edge.second <= 1.5 * ref.second);
    }
}

namespace {

// The sky's noise along one outer line of a stack, `d` lines in from `side` (L, T, R, B): the std of the luminance (mean
// of the channels) over the line's pixels farther than `clear` from (cx, cy); NaN with under 50 such pixels.
double LineSkyNoise(const FloatImage& im, char side, int d, double cx, double cy, double clear) {
    const bool column = side == 'L' || side == 'R';
    const int len = column ? im.height : im.width;
    const int at = side == 'L' || side == 'T' ? d : (column ? im.width : im.height) - 1 - d;
    double s = 0, s2 = 0;
    int64_t n = 0;
    for (int t = 0; t < len; ++t) {
        const int x = column ? at : t, y = column ? t : at;
        if (std::hypot(x - cx, y - cy) <= clear) continue;
        const float* p = im.data.data() + (static_cast<size_t>(y) * im.width + x) * im.channels;
        double l = 0;
        for (int c = 0; c < im.channels; ++c) l += p[c];
        l /= im.channels;
        s += l;
        s2 += l * l;
        ++n;
    }
    if (n < 50) return std::numeric_limits<double>::quiet_NaN();
    const double m = s / static_cast<double>(n);
    return std::sqrt(std::max(0.0, s2 / static_cast<double>(n) - m * m));
}

}  // namespace

TEST_CASE("RunStack: a near-still Bayer capture keeps no outer line that only a few stray frames reach (Ruling R19)",
          "[engine]") {
    // Final review of Part A, I1: the guard band moves each frame's stacked edge 4 px in, and a crop of only the pixels
    // no frame reached keeps every line that some frame reaches beyond its guard. On a capture held nearly still, those
    // outer lines are held by the tail of the shift distribution -- a handful of frames -- and are several times
    // noisier than the interior (2026-09-07 Saturn: the outermost 1-4 lines 2-4.5x the interior sky noise, against
    // 1.1-1.2x on 0.3.0.1). Here: a small planet on flat sky, every frame within 0.3 px rms of the reference, but 20
    // stray frames shifted 6 px diagonally, 5 towards each corner, so each side's outer 4 lines are reached by 10 of the
    // 160 frames.
    const int w = 256, h = 192, n = 160;
    Scene s = testing::MakeDisc(1907, w / 2.0, h / 2.0, 34);
    s.sky = 0.1;  // well above 0, so the frames' noise is not clipped
    Capture cap = testing::NewCapture(w, h, ser::ColorId::BayerRGGB);
    std::mt19937 rng(19070);
    std::normal_distribution<double> jitter(0, 0.3);
    RenderMosaic(cap, s, n, [&](int k) {
        FrameSpec f;
        f.gx = jitter(rng);
        f.gy = jitter(rng);
        f.noise = 0.01;
        // frame(p) = scene(p - g): a frame shifted +6 px in x reaches 6 px further left on the reference's grid. Each
        // stray goes diagonally, so every outer line, corners included, is reached by two of the four sets (10 frames).
        if (k % 32 == 3 || k % 32 == 19) f.gx += 6;
        if (k % 32 == 11 || k % 32 == 27) f.gx -= 6;
        if (k % 32 == 3 || k % 32 == 11) f.gy += 6;
        if (k % 32 == 19 || k % 32 == 27) f.gy -= 6;
        return f;
    });
    const StackOutcome o = stack::RunStack(cap.Factory(), 8, {});
    REQUIRE(o.diagnosis.kind == DiagnosisKind::None);
    REQUIRE(o.image.has_value());
    const FloatImage& im = *o.image;
    const stack::StackCrop& c = o.summary.crop;
    INFO(o.summary.selection << " | " << o.summary.alignment);
    INFO("output " << im.width << " x " << im.height << ", crop " << c.left << "/" << c.top << "/" << c.right << "/"
                   << c.bottom);
    REQUIRE(o.summary.used == n);  // the strays are stacked: they are as sharp as the rest
    // The disc's centre on the output grid (the reference sits within a fraction of a px of the still frames), and the
    // sky clear of its limb by 16 px.
    const double cx = w / 2.0 - c.left, cy = h / 2.0 - c.top, clear = 34 + 16;
    for (const char side : {'L', 'T', 'R', 'B'}) {
        // The interior: the sky of lines 12-23 in from the same side, pooled.
        double ref = 0;
        for (int d = 12; d < 24; ++d) ref += LineSkyNoise(im, side, d, cx, cy, clear) / 12;
        for (int d = 0; d < 4; ++d) {
            const double e = LineSkyNoise(im, side, d, cx, cy, clear);
            INFO("side " << side << ", line " << d << ": sky noise " << e << " against " << ref << " (" << e / ref << "x)");
            CHECK(e <= 1.5 * ref);
        }
    }
}

TEST_CASE("UncoveredCrop cuts the edge rows and columns no frame reached, counts a hole, and says so (Ruling R11)", "[engine]") {
    const int w = 12, h = 9;
    std::vector<float> in(static_cast<size_t>(w) * h, 1.f);
    // Nothing unreached: no crop, no phrase.
    stack::StackCrop c = stack::UncoveredCrop(in, w, h, 4);
    CHECK(c.left + c.top + c.right + c.bottom == 0);
    CHECK(c.uncovered == 0);
    CHECK(stack::CropPhrase(c).empty());
    // Columns 0-1 and row 0 unreached, plus the corner pixel (11, 8) and an interior hole at (6, 4).
    for (int y = 0; y < h; ++y) in[static_cast<size_t>(y) * w] = in[static_cast<size_t>(y) * w + 1] = 0;
    for (int x = 0; x < w; ++x) in[static_cast<size_t>(x)] = 0;
    in[static_cast<size_t>(8) * w + 11] = 0;
    in[static_cast<size_t>(4) * w + 6] = 0;
    c = stack::UncoveredCrop(in, w, h, 4);
    CHECK(c.left == 2);
    CHECK(c.top == 1);
    CHECK(c.right + c.bottom == 1);  // the corner goes with one line, whichever holds the larger share
    CHECK(c.uncovered == 1);         // the hole: no outer line reaches it, so it is counted, not peeled
    const std::string p = stack::CropPhrase(c);
    CHECK(p.find("cropped 2 px left, 1 px top") == 0);
    CHECK(p.find("each frame's outer 4 pixels are not stacked") != std::string::npos);
    CHECK(p.find("1 pixel inside the image") != std::string::npos);
    CHECK_THROWS_AS(stack::UncoveredCrop(in, w, h - 1, 4), std::invalid_argument);
    // Without a guard band (guard 0: not a declared Bayer capture, or a frame too small for it) the phrase names none.
    stack::StackCrop bare = stack::UncoveredCrop(in, w, h, 0);
    CHECK(bare.left == 2);
    const std::string q = stack::CropPhrase(bare);
    CHECK(q.find("cropped 2 px left, 1 px top") == 0);
    CHECK(q.find("no frame reached them") != std::string::npos);
    CHECK(q.find("outer") == std::string::npos);
    CHECK(q.find("stacked part") == std::string::npos);
}

TEST_CASE("RunStack: a still mono capture has no guard band -- no crop, its full size (Ruling R13)", "[engine]") {
    // The guard band guards DemosaicHQ's border (Accumulate.h, "The frame's border"); a mono frame has none, so every
    // frame's edge pixels are stacked and a capture held still keeps every pixel. With the guard applied, every side
    // would lose ~4 px here.
    const int w = 112, h = 96;
    const Capture cap = DiscCapture(w, h, 55.6, 48.3, 22, 40, 47, 0.0);  // no global drift: only the seeing's local motion
    const StackOutcome o = stack::RunStack(cap.Factory(), 4, {});
    REQUIRE(o.diagnosis.kind == DiagnosisKind::None);
    REQUIRE(o.image.has_value());
    INFO(o.summary.alignment);
    CHECK(o.summary.crop.guard == 0);
    CHECK(o.summary.crop.left + o.summary.crop.top + o.summary.crop.right + o.summary.crop.bottom == 0);
    CHECK(o.image->width == w);
    CHECK(o.image->height == h);
}
