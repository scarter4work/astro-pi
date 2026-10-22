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
    std::atomic<int> live{0};
    std::atomic<int64_t> reads{0};
    int delayMs = 0;
};

class CountedSource : public ser::FrameSource {
public:
    CountedSource(std::unique_ptr<ser::FrameSource> inner, ReaderLedger& ledger) : inner_(std::move(inner)), ledger_(ledger) {
        ++ledger_.live;
    }
    ~CountedSource() override { --ledger_.live; }
    const ser::FileInfo& Info() const override { return inner_->Info(); }
    ser::Frame ReadFrame(int64_t i) const override {
        ++ledger_.reads;
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
    CHECK(steps == std::vector<std::string>{"Hot pixels", "Measuring frames", "Selecting", "Building reference",
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

    const Capture cap = DiscCapture(112, 96, 56.2, 47.8, 22, 48, 21);
TEST_CASE("RunStack: cancel in each phase throws StackCancelled, joins every thread and closes every reader", "[engine]") {
    struct Case {
        std::string phase;
        // Fire once done >= 2, or (fromEnd > 0) once done >= total - fromEnd: in Aligning the last
        // kFocusFrames units are the focus measure's reads, the ones before them the alignment pass.
        int fromEnd;
        const char* where;
    };
    for (const Case& c : {Case{stack::kPhaseMeasuring, 0, "measuring"}, Case{stack::kPhaseAligning, 0, "building the reference"},
                          Case{stack::kPhaseAligning, stack::kFocusFrames + 4, "the alignment pass"},
                          Case{stack::kPhaseAligning, 3, "the focus measure's reads"}, Case{stack::kPhaseStacking, 0, "stacking"}}) {
        INFO("cancel in " << c.where);
        ReaderLedger ledger;
        ledger.delayMs = 2;
        const auto factory = Counted(cap.Factory(), ledger);
        const int threadsBefore = ThreadCount();
        std::string phase;
        int64_t done = 0, total = 0;
        std::vector<std::string> seen;
        bool fired = false;
        int64_t firedAt = -1;
        stack::Progress progress;
        progress.report = [&](const std::string& p, int64_t d, int64_t t) {
            phase = p;
            done = d;
            total = t;
            if (seen.empty() || seen.back() != p) seen.push_back(p);
        };
        progress.cancelled = [&] {
            if (!fired && phase == c.phase && (c.fromEnd > 0 ? done >= total - c.fromEnd : done >= 2)) {
                fired = true;
                firedAt = done;
            }
            return fired;
        };
        CHECK_THROWS_AS(stack::RunStack(factory, 6, progress), stack::StackCancelled);
        CHECK(fired);
        if (c.fromEnd > stack::kFocusFrames) CHECK(firedAt <= total - stack::kFocusFrames);  // before the focus reads
        if (c.fromEnd > 0 && c.fromEnd < stack::kFocusFrames) CHECK(firedAt > total - stack::kFocusFrames);
        REQUIRE_FALSE(seen.empty());
        CHECK(seen.back() == c.phase);  // nothing after the cancelled phase was started
        CHECK(ledger.live.load() == 0);
        CHECK(ThreadCount() == threadsBefore);
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
    CHECK(warning.find("Save pattern...") != std::string::npos);
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
    CHECK(a.summary.detailScale == b.summary.detailScale);  // MeasureFocus is identical for any count
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
    // In the sky beside the disc: inside a textured disc HotPixelMap's ring rule passes a +0.35 site in only
    // 27 of these 50 frames (under kHotShare; on a flat disc 50 of 50) -- a Task 5 limit, measured 2026-10-22.
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

TEST_CASE("RunStack: a defocused frame-filling surface is diagnosed out of focus from its regions (Ruling P4)", "[engine]") {
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
    // Measured here (detail scale, sensor px = luma px): blur 1 -> 4.5, 3 -> 7.3, 12 -> 12.3.
    for (double blur : {1.0, 3.0}) {
        const StackOutcome o = stack::RunStack(capture(blur).Factory(), 8, {});
        INFO("blur " << blur << ": detail " << o.summary.detailScale << " | " << o.summary.alignment);
        CHECK(o.diagnosis.kind == DiagnosisKind::None);
        CHECK(o.image.has_value());
        CHECK(o.summary.detailScale > 0);
        CHECK(o.summary.detailScale < stack::kMaxDetailScale);
    }
    const StackOutcome o = stack::RunStack(capture(12.0).Factory(), 8, {});
    INFO("blur 12: " << o.diagnosis.message << " | detail " << o.summary.detailScale << " | " << o.summary.selection);
    REQUIRE(o.diagnosis.kind == DiagnosisKind::OutOfFocus);
    CHECK_FALSE(o.image.has_value());
    CHECK(o.summary.used == 0);
    CHECK(o.summary.detailScale > stack::kMaxDetailScale);
    bool regionNumbers = false;  // the region stage's verdict, not Select's
    for (const auto& nb : o.diagnosis.numbers) regionNumbers |= nb.first == "detail scale (pixels)";
    CHECK(regionNumbers);
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
