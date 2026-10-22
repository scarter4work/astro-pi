#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>

#include "Accumulate.h"
#include "Align.h"
#include "Demosaic.h"
#include "Measure.h"
#include "Regions.h"
#include "Select.h"
#include "StackSynth.h"

using namespace planetary;
using ser::FloatImage;
using stack::FrameRegions;
using stack::Shift;
using testing::Capture;
using testing::FrameSpec;
using testing::Scene;

namespace {


// Renders `n` frames on `threads` threads: spec(k) describes frame k.
void RenderCapture(Capture& cap, const Scene& s, int n, const std::function<FrameSpec(int)>& spec) {
    const int w = cap.info.header.width, h = cap.info.header.height;
    std::vector<std::vector<float>> frames(static_cast<size_t>(n));
    std::vector<FrameSpec> specs;
    for (int k = 0; k < n; ++k) specs.push_back(spec(k));
    std::vector<std::thread> pool;
    const int t = 16;
    for (int i = 0; i < t; ++i)
        pool.emplace_back([&, i] {
            for (int k = i; k < n; k += t)
                frames[static_cast<size_t>(k)] = testing::RenderFrame(s, w, h, specs[static_cast<size_t>(k)], 1000u + k);
        });
    for (auto& th : pool) th.join();
    for (auto& f : frames) cap.Add(f);
}

// Catmull-Rom sample, clamped (the plain global-aligned mean's interpolation).
double Cubic(const FloatImage& im, double x, double y, int c = 0) {
    const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
    auto wts = [](double t, double* w) {
        const double t2 = t * t, t3 = t2 * t;
        w[0] = 0.5 * (-t3 + 2 * t2 - t);
        w[1] = 0.5 * (3 * t3 - 5 * t2 + 2);
        w[2] = 0.5 * (-3 * t3 + 4 * t2 + t);
        w[3] = 0.5 * (t3 - t2);
    };
    double wx[4], wy[4];
    wts(x - x0, wx);
    wts(y - y0, wy);
    double acc = 0;
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i) {
            const int xx = std::clamp(x0 - 1 + i, 0, im.width - 1), yy = std::clamp(y0 - 1 + j, 0, im.height - 1);
            acc += wx[i] * wy[j] * im.data[(static_cast<size_t>(yy) * im.width + xx) * im.channels + c];
        }
    return acc;
}

struct Pipeline {
    ser::FileInfo info;
    stack::Selection sel;
    std::vector<stack::FrameStats> stats;
    FloatImage reference;
    std::vector<int64_t> used;          // frames stacked (selected and aligned)
    std::vector<Shift> shifts;          // their global shifts
    stack::RegionPlan plan;
    FloatImage ours;                    // the region stack
    FloatImage sigma;                   // ResidualScale::Sigma
    std::vector<FrameRegions> measured; // per used frame
    std::unique_ptr<stack::Accumulator> acc;
};

// The engine's flow, as Task 6 will run it (single pass of each stage, frames read from memory).
// allFrames: the regions get every frame with a target instead of the global selection. Real lunar
// captures keep 100% globally (Task 3: whole-frame quality spread 0.160-0.161 -- seeing averages out
// over hundreds of isoplanatic patches); the synthetic frames hold only ~20 patches, so their
// whole-frame quality varies far more and the global cut would keep a handful. Handing the regions
// every frame reproduces the real full-frame case, where the lucky selection is the regions' alone.
// keepTorn: no global alignment cut (Align.h keeps half-frame tears of 4-6 px in its own tests; the
// synthetic seeing here lowers their match below the cut, so the tear test lifts it to reach the regions).
Pipeline Run(const Capture& cap, bool allFrames = false, bool keepTorn = false, int threads = 8) {
    Pipeline p;
    p.info = cap.info;
    const auto factory = cap.Factory();
    p.stats = stack::MeasureAll(factory, p.info, threads, {});
    p.sel = stack::SelectFrames(p.stats, p.info);
    REQUIRE(p.sel.diagnosis.kind == stack::DiagnosisKind::None);
    p.reference = stack::BuildReference(factory, p.info, p.sel, p.stats, threads, {});
    const stack::GlobalAligner aligner(p.reference, p.sel.fullFrameTarget);
    auto src = factory();
    std::vector<FloatImage> lumas;
    std::vector<Shift> all;
    std::vector<int64_t> cand = p.sel.frames;
    if (allFrames) {
        cand.clear();
        for (const auto& st : p.stats)
            if (st.targetFound) cand.push_back(st.index);
    }
    for (int64_t f : cand) {
        lumas.push_back(stack::LumaForMeasure(src->ReadFrame(f), p.info.colorId, p.info.normDepth));
        all.push_back(aligner.Align(lumas.back(), p.stats[static_cast<size_t>(f)]));
    }
    const double cut = keepTorn ? 0.0 : stack::AlignmentCut(all);
    const stack::RegionLayout layout =
        stack::PlaceRegions(p.reference, stack::MeasureFrame(p.reference, 0), p.sel.fullFrameTarget);
    const stack::RegionMeasurer measurer(p.reference, layout);
    stack::ResidualScale residual(layout, p.reference);
    std::vector<FrameRegions> measured;
    for (size_t i = 0; i < cand.size(); ++i) {
        if (all[i].confidence < cut) continue;
        p.used.push_back(cand[i]);
        p.shifts.push_back(all[i]);
        measured.push_back(measurer.Measure(lumas[i], cand[i], all[i]));
        p.measured.push_back(measured.back());
        residual.Observe(lumas[i], measured.back());
    }
    p.plan = stack::PlanRegions(layout, measured);
    p.sigma = residual.Sigma(p.info.normDepth);
    p.acc = std::make_unique<stack::Accumulator>(p.plan, p.reference, p.sigma, p.info);
    for (int64_t f : p.used)  // frames no region keeps are refused (not stacked anywhere)
        CHECK(p.acc->Add(f, stack::DemosaicHQ(src->ReadFrame(f), p.info.colorId, p.info.normDepth)) == p.plan.Uses(f));
    p.ours = p.acc->Finish();
    return p;
}

// Mean of `frames` sampled at x + their global shift (Catmull-Rom), on the reference grid.
FloatImage PlainMean(const Capture& cap, const Pipeline& p, const std::vector<int64_t>& frames) {
    const int w = cap.info.header.width, h = cap.info.header.height;
    FloatImage out{w, h, 1, std::vector<float>(static_cast<size_t>(w) * h, 0.f)};
    auto src = cap.Factory()();
    for (int64_t f : frames) {
        const auto it = std::find(p.used.begin(), p.used.end(), f);
        REQUIRE(it != p.used.end());
        const Shift s = p.shifts[static_cast<size_t>(it - p.used.begin())];
        const FloatImage im = stack::DemosaicHQ(src->ReadFrame(f), cap.info.colorId, cap.info.normDepth);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                out.data[static_cast<size_t>(y) * w + x] += static_cast<float>(Cubic(im, x + s.dx, y + s.dy) / frames.size());
    }
    return out;
}

// RMS difference between a stack and the sharp scene, after registering the stack to the scene
// (GlobalShift: stack(x + d) ~ scene(x)), over `inside` pixels.
double RmsToTruth(const FloatImage& stackIm, const Scene& s, const std::function<bool(int, int)>& inside) {
    const int w = stackIm.width, h = stackIm.height;
    FloatImage truth{w, h, 1, std::vector<float>(static_cast<size_t>(w) * h)};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) truth.data[static_cast<size_t>(y) * w + x] = static_cast<float>(s.Eval(x, y, 0));
    const Shift d = stack::GlobalShift(truth, stackIm, stack::MeasureFrame(stackIm, 0));
    double sum = 0;
    int64_t n = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (!inside(x, y)) continue;
            const double e = stackIm.data[static_cast<size_t>(y) * w + x] - s.Eval(x - d.dx, y - d.dy, 0);
            sum += e * e;
            ++n;
        }
    REQUIRE(n > 0);
    return std::sqrt(sum / static_cast<double>(n));
}

// A surface capture with seeing: per-frame drift, a smooth local displacement field (rms ~0.7 px,
// cells of 40 px) and a smooth blur field (0.4-3 px, lognormal per 40-px cell).
FrameSpec Seeing(std::mt19937& rng, int w, int h, double drift, double cell, double blurMedian = 1.2) {
    std::normal_distribution<double> n(0, 1);
    FrameSpec f;
    f.gx = drift * n(rng);
    f.gy = drift * n(rng);
    f.hasLocal = true;
    f.dx = testing::RandomField(rng, w, h, cell, [&] { return 0.7 * n(rng); });
    f.dy = testing::RandomField(rng, w, h, cell, [&] { return 0.7 * n(rng); });
    f.sigma = testing::RandomField(rng, w, h, cell, [&] { return std::clamp(blurMedian * std::exp(0.6 * n(rng)), std::min(0.4, blurMedian), 3.0); });
    f.noise = 0.01;
    return f;
}

}  // namespace

TEST_CASE("Lucky regions beat the plain global-aligned mean on a frame-filling surface", "[accumulate]") {
    const int w = 224, h = 176;
    const Scene s = testing::MakeSurface(31, w, h);
    Capture cap = testing::NewCapture(w, h);
    std::mt19937 rng(77);
    RenderCapture(cap, s, 80, [&](int) { return Seeing(rng, w, h, 2.0, 40); });
    const Pipeline p = Run(cap, true);
    REQUIRE(p.sel.fullFrameTarget);
    const FloatImage plain = PlainMean(cap, p, p.used);
    auto inside = [&](int x, int y) { return x >= 16 && y >= 16 && x < w - 16 && y < h - 16; };
    const double ours = RmsToTruth(p.ours, s, inside), base = RmsToTruth(plain, s, inside);
    INFO("rms ours " << ours << " plain " << base);
    CHECK(ours < 0.75 * base);
}

TEST_CASE("Lucky regions beat the plain global-aligned mean on a 40 px disc", "[accumulate]") {
    const int w = 112, h = 96;
    const Scene s = testing::MakeDisc(8, 55.5, 47.2, 20);
    Capture cap = testing::NewCapture(w, h);
    std::mt19937 rng(78);
    RenderCapture(cap, s, 100, [&](int) {
        FrameSpec f = Seeing(rng, w, h, 3.0, 28);
        return f;
    });
    const Pipeline p = Run(cap);
    REQUIRE_FALSE(p.sel.fullFrameTarget);
    REQUIRE(!p.plan.layout.regions.empty());
    const FloatImage plain = PlainMean(cap, p, p.used);
    auto inside = [&](int x, int y) { return std::hypot(x - 55.5, y - 47.2) < 26; };
    const double ours = RmsToTruth(p.ours, s, inside), base = RmsToTruth(plain, s, inside);
    INFO("rms ours " << ours << " plain " << base);
    CHECK(ours < 0.8 * base);
}

TEST_CASE("Per-region best beats per-frame best when each frame is sharp only in places", "[accumulate]") {
    const int w = 224, h = 176;
    const Scene s = testing::MakeSurface(32, w, h);
    Capture cap = testing::NewCapture(w, h);
    std::mt19937 rng(79);
    std::uniform_real_distribution<double> uni(0, 1);
    RenderCapture(cap, s, 80, [&](int) {
        FrameSpec f = Seeing(rng, w, h, 2.0, 40);
        // Sharp (0.4 px) in about a third of the 40-px cells, blurred (2.6 px) elsewhere.
        for (double& v : f.sigma.v) v = uni(rng) < 0.33 ? 0.4 : 2.6;
        return f;
    });
    const Pipeline p = Run(cap, true);
    // Per-frame best: the same number of frames as a typical region keeps, chosen by whole-frame
    // quality (the best a whole-frame selection can do).
    std::vector<int64_t> kept;
    for (const auto& d : p.plan.detail) kept.push_back(d.kept);
    std::sort(kept.begin(), kept.end());
    const int64_t k = std::max<int64_t>(1, kept[kept.size() / 2]);
    std::vector<int64_t> byQuality = p.used;
    std::sort(byQuality.begin(), byQuality.end(), [&](int64_t a, int64_t b) {
        return p.stats[static_cast<size_t>(a)].quality > p.stats[static_cast<size_t>(b)].quality;
    });
    byQuality.resize(static_cast<size_t>(k));
    const FloatImage frameBest = PlainMean(cap, p, byQuality);
    auto inside = [&](int x, int y) { return x >= 16 && y >= 16 && x < w - 16 && y < h - 16; };
    const double ours = RmsToTruth(p.ours, s, inside), best = RmsToTruth(frameBest, s, inside);
    INFO("rms ours " << ours << " per-frame best (" << k << " frames) " << best);
    CHECK(ours < 0.8 * best);
}

TEST_CASE("Tears up to 6 px are absorbed locally: no doubled features", "[accumulate]") {
    const int w = 224, h = 176, tearRow = 117;  // the lower third
    const Scene s = testing::MakeSurface(33, w, h);
    std::vector<double> tears(60, 0.0);
    auto make = [&](bool torn) {
        Capture cap = testing::NewCapture(w, h);
        std::mt19937 rng(80);
        std::uniform_real_distribution<double> uni(0, 1);
        RenderCapture(cap, s, 60, [&](int k) {
            FrameSpec f = Seeing(rng, w, h, 2.0, 40);
            const double t = 4 + 2 * uni(rng);
            if (torn && k % 10 == 3) {  // one frame in ten: the lower part displaced 4-6 px
                f.tearRow = tearRow;
                f.tearDx = t;
                f.tearDy = -0.5 * t;
                tears[static_cast<size_t>(k)] = t;
            }
            return f;
        });
        return cap;
    };
    const Capture clean = make(false), torn = make(true);
    const Pipeline a = Run(clean, true, true), b = Run(torn, true, true);
    // The torn frames reach the regions ...
    int tornUsed = 0;
    for (int64_t f : b.used) tornUsed += tears[static_cast<size_t>(f)] > 0;
    REQUIRE(tornUsed >= 3);
    // ... and the regions wholly below the tear stacked them with the tear as their local shift.
    const size_t nr = b.plan.RegionCount();
    int keptBelow = 0, absorbed = 0;
    for (size_t r = 0; r < nr; ++r) {
        const auto& reg = b.plan.layout.regions[r];
        if (reg.y0 < tearRow + 8 || reg.x0 < 12 || reg.x1 > w - 12 || reg.y1 > h - 4) continue;
        for (size_t f = 0; f < b.plan.frames.size(); ++f) {
            const double t = tears[static_cast<size_t>(b.plan.frames[f])];
            if (t == 0 || !(b.plan.flags[f * nr + r] & stack::RegionPlan::kKept)) continue;
            ++keptBelow;
            // The region's total shift (global + local) against the same frame's in the clean capture
            // (same seeing): the difference is the tear, to within the two references' own difference
            // (0.1-0.3 px: the torn capture's reference averages a few torn frames in).
            const int64_t fa = a.plan.Find(b.plan.frames[f]);
            REQUIRE(fa >= 0);
            const size_t ka = static_cast<size_t>(fa) * nr + r, kb = f * nr + r;
            REQUIRE(a.plan.RegionCount() == nr);
            const double ddx = (b.plan.global[f].dx + b.plan.dx[kb]) - (a.plan.global[static_cast<size_t>(fa)].dx + a.plan.dx[ka]);
            const double ddy = (b.plan.global[f].dy + b.plan.dy[kb]) - (a.plan.global[static_cast<size_t>(fa)].dy + a.plan.dy[ka]);
            absorbed += std::fabs(ddx - t) < 0.35 && std::fabs(ddy + 0.5 * t) < 0.35;
        }
    }
    INFO("torn frames used " << tornUsed << ", kept below the tear " << keptBelow << ", absorbed " << absorbed);
    CHECK(keptBelow > 0);
    CHECK(absorbed == keptBelow);
    // No doubled features: the torn capture stacks as well as the clean one.
    auto inside = [&](int x, int y) { return x >= 16 && y >= 16 && x < w - 16 && y < h - 16; };
    const double rc = RmsToTruth(a.ours, s, inside), rt = RmsToTruth(b.ours, s, inside);
    INFO("rms clean " << rc << " torn " << rt);
    CHECK(rt < 1.1 * rc);
}

TEST_CASE("A flat field stacks without seams, whatever frames each region keeps", "[accumulate]") {
    // A surface reference places regions; the frames are a flat field with noise and a gain that
    // varies from frame to frame (haze); each region keeps a different random subset.
    const int w = 192, h = 160;
    const Scene s = testing::MakeSurface(40, w, h);
    FloatImage ref{w, h, 1, std::vector<float>(static_cast<size_t>(w) * h)};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) ref.data[static_cast<size_t>(y) * w + x] = static_cast<float>(s.Eval(x, y, 0.6));
    const stack::RegionLayout lay = stack::PlaceRegions(ref, stack::MeasureFrame(ref, 0), true);
    REQUIRE(lay.regions.size() > 10);
    // The flat field the frames show; the reference for rejection is the same flat level.
    FloatImage flatRef{w, h, 1, std::vector<float>(static_cast<size_t>(w) * h, 0.5f)};
    const int frames = 60;
    std::mt19937 rng(5);
    std::uniform_real_distribution<double> uni(0, 1);
    std::normal_distribution<double> n(0, 1);
    std::vector<FrameRegions> measured;
    std::vector<FloatImage> imgs;
    const double noise = 0.02;
    for (int k = 0; k < frames; ++k) {
        FrameRegions m;
        m.frameIndex = k;
        m.gain = 0.7 + 0.6 * uni(rng);
        for (size_t r = 0; r < lay.regions.size(); ++r) {
            stack::RegionMeasure q;
            q.aligned = true;
            q.confidence = 0.95f;
            q.quality = static_cast<float>(0.3 * std::exp(0.4 * n(rng)));  // random: regions disagree
            q.qualitySigma = 0.001f;
            m.regions.push_back(q);
        }
        measured.push_back(m);
        FloatImage im{w, h, 1, std::vector<float>(static_cast<size_t>(w) * h)};
        for (float& v : im.data) v = static_cast<float>(m.gain * 0.5 + noise * n(rng));
        imgs.push_back(std::move(im));
    }
    const stack::RegionPlan plan = stack::PlanRegions(lay, measured);
    // Regions kept different subsets of frames.
    int differ = 0;
    for (size_t r = 1; r < plan.RegionCount(); ++r)
        for (size_t f = 0; f < plan.frames.size(); ++f)
            differ += (plan.flags[f * plan.RegionCount() + r] ^ plan.flags[f * plan.RegionCount()]) & stack::RegionPlan::kKept;
    REQUIRE(differ > 0);
    stack::ResidualScale residual(lay, flatRef);
    for (int k = 0; k < frames; ++k) residual.Observe(imgs[static_cast<size_t>(k)], measured[static_cast<size_t>(k)]);
    ser::FileInfo info = testing::NewCapture(w, h).info;
    stack::Accumulator acc(plan, flatRef, residual.Sigma(16), info);
    for (int k = 0; k < frames; ++k) acc.Add(k, imgs[static_cast<size_t>(k)]);
    const FloatImage out = acc.Finish();
    // Mean level restored by the gain correction; block means of step x step agree within noise.
    double mean = 0;
    for (float v : out.data) mean += v;
    mean /= static_cast<double>(out.data.size());
    CHECK(mean == Catch::Approx(0.5).margin(0.002));
    // Pixel noise of the stack: at most noise / sqrt(min kept) / 0.7 (the dimmest gain).
    int64_t minKept = frames;
    for (const auto& d : plan.detail) minKept = std::min(minKept, d.kept);
    const double pixNoise = noise / 0.7 / std::sqrt(static_cast<double>(minKept));
    // Seams would show as steps in the local mean at region borders: compare the means of 4-px-wide
    // strips on either side of every region window edge (each strip's mean has noise ~pixNoise / sqrt(4 L)).
    double worst = 0;
    for (const auto& r : lay.regions)
        for (int edge : {r.x0, r.x1}) {
            if (edge < 8 || edge > w - 8) continue;
            double a = 0, b = 0;
            int na = 0;
            for (int y = std::max(0, r.y0); y < std::min(h, r.y1); ++y)
                for (int dx = 1; dx <= 4; ++dx) {
                    a += out.data[static_cast<size_t>(y) * w + edge - dx];
                    b += out.data[static_cast<size_t>(y) * w + edge + dx - 1];
                    ++na;
                }
            worst = std::max(worst, std::fabs(a - b) / na / (pixNoise * std::sqrt(2.0 / na)));
        }
    INFO("worst seam step in noise units " << worst);
    CHECK(worst < 5);
    // Largest gradient between neighbouring pixels: no more than the noise allows (6 sigma of a
    // difference of two independent noisy pixels).
    double maxGrad = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x + 1 < w; ++x)
            maxGrad = std::max(maxGrad, static_cast<double>(std::fabs(out.data[static_cast<size_t>(y) * w + x + 1] -
                                                                        out.data[static_cast<size_t>(y) * w + x])));
    CHECK(maxGrad < 6 * std::sqrt(2.0) * pixNoise);
    CHECK(acc.RejectedCells() < 0.001 * frames * w * h);
}

TEST_CASE("A single-frame streak is rejected", "[accumulate]") {
    for (bool bayer : {false, true}) {
        INFO((bayer ? "Bayer" : "mono"));
        const int w = 192, h = 160;
        const ser::ColorId color = bayer ? ser::ColorId::BayerRGGB : ser::ColorId::Mono;
        const Scene s = testing::MakeSurface(41, w, h);
        // Bayer: a grey scene (R = G = B), so the CFA is the scene at full resolution.
        Capture clean = testing::NewCapture(w, h, color);
        std::mt19937 rng(81);
        std::vector<FrameSpec> specs;
        for (int k = 0; k < 40; ++k) {
            FrameSpec f = Seeing(rng, w, h, 2.0, 48);
            if (k == 17)  // the sharpest frame everywhere, so every region stacks it
                for (double& v : f.sigma.v) v = 0.4;
            specs.push_back(f);
        }
        RenderCapture(clean, s, 40, [&](int k) { return specs[static_cast<size_t>(k)]; });
        const Pipeline q = Run(clean, true);
        REQUIRE(std::find(q.used.begin(), q.used.end(), 17) != q.used.end());
        int keptBy = 0;
        const int64_t pos = q.plan.Find(17);
        for (size_t r = 0; r < q.plan.RegionCount(); ++r)
            keptBy += (q.plan.flags[static_cast<size_t>(pos) * q.plan.RegionCount() + r] & stack::RegionPlan::kKept) != 0;
        REQUIRE(keptBy == static_cast<int>(q.plan.RegionCount()));
        // The same stack with frame 17 crossed by a bright trail (+0.5 over a ~2 px wide line: a
        // satellite) and, separately, by a dark one (-0.5: a bird's silhouette). Rejection drops frame
        // 17's samples along the trail either way, which changes the stack there a little (17 is a
        // good frame) by the same amount in both runs; whatever of the trail leaks through does not
        // cancel. So the leak is half the difference of the two runs.
        auto run = [&](double level, int64_t& rejected) {
            FrameSpec streaked = specs[17];
            streaked.streak = true;
            streaked.streakLevel = level;
            streaked.sx0 = 10;
            streaked.sy0 = 20;
            streaked.sx1 = w - 10;
            streaked.sy1 = h - 30;
            Capture one = testing::NewCapture(w, h, color);
            one.Add(testing::RenderFrame(s, w, h, streaked, 1000u + 17));
            // The residual scale is observed with the streaked frame among the others (as Task 6 will).
            stack::ResidualScale rs(q.plan.layout, q.reference);
            {
                auto src = clean.Factory()();
                for (size_t i = 0; i < q.used.size(); ++i) {
                    const ser::Frame fr = q.used[i] == 17 ? one.Factory()()->ReadFrame(0) : src->ReadFrame(q.used[i]);
                    rs.Observe(stack::LumaForMeasure(fr, q.info.colorId, q.info.normDepth), q.measured[i]);
                }
            }
            stack::Accumulator acc(q.plan, q.reference, rs.Sigma(q.info.normDepth), q.info);
            auto src = clean.Factory()();
            for (int64_t f : q.used) {
                const ser::Frame fr = f == 17 ? one.Factory()()->ReadFrame(0) : src->ReadFrame(f);
                acc.Add(f, stack::DemosaicHQ(fr, q.info.colorId, q.info.normDepth));
            }
            rejected = acc.RejectedCells();
            return acc.Finish();
        };
        int64_t rejBright = 0, rejDark = 0;
        const FloatImage bright = run(0.5, rejBright), dark = run(-0.5, rejDark);
        const int c = bright.channels;
        double leak = 0, worstAway = 0;
        // Beyond the rejection's reach: the trail's strong cells (up to ~3 luma cells out after the
        // demosaic) plus kRejectReach + 1 luma cells.
        const double away = (bayer ? 2 : 1) * (stack::kRejectReach + 4);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const double dist = std::fabs((x - 10.0) * (h - 50) - (y - 20.0) * (w - 20)) / std::hypot(w - 20, h - 50);
                for (int ch = 0; ch < c; ++ch) {
                    const size_t i = (static_cast<size_t>(y) * w + x) * c + ch;
                    if (dist <= 4 && y > 20 && y < h - 30) leak = std::max(leak, 0.5 * std::fabs(bright.data[i] - dark.data[i]));
                    if (dist > away) worstAway = std::max(worstAway, static_cast<double>(std::fabs(bright.data[i] - q.ours.data[i])));
                }
            }
        INFO("trail leak " << leak << ", change away from it " << worstAway << ", rejected cells " << rejBright
                           << " bright / " << rejDark << " dark");
        // Unrejected, the trail would add 0.5 / (frames kept) -- at least 0.5 / 40 = 0.0125 -- at its centre.
        CHECK(leak < 0.001);
        // Away from the trail nothing changes beyond the spline prefilter's exponentially decaying
        // reach (|z|^k, z = 2 - sqrt 3: 0.5 x 0.27^9 ~ 4e-6 at 9 px).
        CHECK(worstAway < 1e-4);
        CHECK(rejBright > 0);
        CHECK(rejDark > 0);
    }
}

TEST_CASE("Accumulator memory is bounded by one frame, not by the frames added", "[accumulate]") {
    const int w = 160, h = 128;
    const Scene s = testing::MakeSurface(42, w, h);
    Capture cap = testing::NewCapture(w, h);
    std::mt19937 rng(82);
    RenderCapture(cap, s, 30, [&](int) { return Seeing(rng, w, h, 1.0, 40); });
    Pipeline p = Run(cap, true);
    const size_t held = p.acc->BytesHeld();
    const size_t frameBytes = static_cast<size_t>(w) * h * sizeof(float);
    // Float sums of the kept and of the rejected samples ((channels + 1) of each per pixel), the luma
    // maps and the plan: a few frames' worth.
    const size_t planBytes = p.plan.frames.size() * p.plan.RegionCount() * 9 + p.plan.frames.size() * 40;
    CHECK(held <= 2 * 2 * frameBytes + 2 * frameBytes + 2 * frameBytes + planBytes + 64 * 1024);
    // Adding more frames does not grow it (re-adding is refused, so build a second accumulator and
    // compare after a partial run).
    CHECK(p.acc->FramesAdded() == p.plan.stackedCount);
    CHECK_THROWS_AS(p.acc->Add(*std::find_if(p.used.begin(), p.used.end(), [&](int64_t f) { return p.plan.Uses(f); }), FloatImage{w, h, 1, std::vector<float>(static_cast<size_t>(w) * h)}),
                    std::logic_error);
    CHECK_FALSE(p.acc->Add(999999, FloatImage{w, h, 1, std::vector<float>(static_cast<size_t>(w) * h)}));
    CHECK(p.acc->BytesHeld() == held);
    CHECK_THROWS_AS(p.acc->Add(p.used.back() + 0, FloatImage{w / 2, h, 1, std::vector<float>(static_cast<size_t>(w / 2) * h)}),
                    std::invalid_argument);
}
TEST_CASE("Concurrent Add gives the sequential result", "[accumulate]") {
    const int w = 160, h = 128;
    const Scene s = testing::MakeSurface(43, w, h);
    Capture cap = testing::NewCapture(w, h);
    std::mt19937 rng(83);
    RenderCapture(cap, s, 40, [&](int) { return Seeing(rng, w, h, 1.5, 40); });
    const Pipeline p = Run(cap, true);  // sequential Adds, frame order
    stack::Accumulator acc(p.plan, p.reference, p.sigma, p.info);
    std::vector<FloatImage> frames;
    auto src = cap.Factory()();
    for (int64_t f : p.used) frames.push_back(stack::DemosaicHQ(src->ReadFrame(f), p.info.colorId, p.info.normDepth));
    std::vector<std::thread> pool;
    for (int t = 0; t < 8; ++t)
        pool.emplace_back([&, t] {
            for (size_t i = frames.size(); i-- > 0;)  // reverse order, interleaved over threads
                if (static_cast<int>(i % 8) == t) acc.Add(p.used[i], frames[i]);
        });
    for (auto& th : pool) th.join();
    const FloatImage out = acc.Finish();
    REQUIRE(out.data.size() == p.ours.data.size());
    double worst = 0;
    for (size_t i = 0; i < out.data.size(); ++i) worst = std::max(worst, static_cast<double>(std::fabs(out.data[i] - p.ours.data[i])));
    CHECK(worst < 1e-6);
    CHECK(acc.FramesAdded() == p.plan.stackedCount);
}

TEST_CASE("ResidualScale and Accumulator argument errors", "[accumulate]") {
    const int w = 96, h = 80;
    const Scene s = testing::MakeSurface(44, w, h);
    FloatImage ref{w, h, 1, std::vector<float>(static_cast<size_t>(w) * h)};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) ref.data[static_cast<size_t>(y) * w + x] = static_cast<float>(s.Eval(x, y, 0.6));
    const stack::RegionLayout lay = stack::PlaceRegions(ref, stack::MeasureFrame(ref, 0), true);
    stack::ResidualScale rs(lay, ref);
    CHECK_THROWS_AS(rs.Sigma(16), std::logic_error);
    FrameRegions m;
    m.regions.resize(lay.regions.size() + 1);
    CHECK_THROWS_AS(rs.Observe(ref, m), std::invalid_argument);
    FloatImage small{w / 2, h, 1, std::vector<float>(static_cast<size_t>(w / 2) * h)};
    CHECK_THROWS_AS(stack::ResidualScale(lay, small), std::invalid_argument);
    const stack::RegionPlan empty = stack::PlanRegions(lay, {});
    CHECK_THROWS_AS(stack::Accumulator(empty, ref, ref, testing::NewCapture(w, h).info), std::invalid_argument);
}

TEST_CASE("B-spline resampling: exact at integer shifts, little softening at half a pixel", "[accumulate]") {
    const int w = 96, h = 40;
    auto stackOne = [&](double period, double shift) {
        FloatImage im{w, h, 1, std::vector<float>(static_cast<size_t>(w) * h)};
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                im.data[static_cast<size_t>(y) * w + x] = static_cast<float>(0.5 + 0.2 * std::sin(2 * M_PI * x / period + 0.3));
        stack::RegionLayout none;  // no regions: the fallback stack alone, global shift only
        none.width = w;
        none.height = h;
        FrameRegions m;
        m.frameIndex = 0;
        m.global = Shift{shift, 0, 1};
        const stack::RegionPlan plan = stack::PlanRegions(none, {m});
        const FloatImage noRejection{w, h, 1, std::vector<float>(static_cast<size_t>(w) * h, 1.f)};
        stack::Accumulator acc(plan, im, noRejection, testing::NewCapture(w, h).info);
        acc.Add(0, im);
        return acc.Finish();
    };
    // Integer shift: out(x) = in(x + 2) exactly (away from the mirrored edges).
    const FloatImage one = stackOne(7.3, 2.0);
    double worst = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 8; x < w - 8; ++x)
            worst = std::max(worst, std::fabs(one.data[static_cast<size_t>(y) * w + x] -
                                              (0.5 + 0.2 * std::sin(2 * M_PI * (x + 2) / 7.3 + 0.3))));
    CHECK(worst < 2e-5);
    // Half-pixel shift: the amplitude kept at 0.25 and 0.4 cycles/px (Accumulate.h: 0.97 and 0.66;
    // Catmull-Rom would keep 0.88 and 0.45).
    for (auto [period, expect] : {std::pair<double, double>{4.0, 0.972}, {2.5, 0.660}}) {
        const FloatImage half = stackOne(period, 0.5);
        double c = 0, sn = 0;
        int n = 0;
        for (int y = 0; y < h; ++y)
            for (int x = 10; x < w - 10; ++x) {
                const double ph = 2 * M_PI * (x + 0.5) / period + 0.3, v = half.data[static_cast<size_t>(y) * w + x] - 0.5;
                c += v * std::cos(ph);
                sn += v * std::sin(ph);
                ++n;
            }
        const double amp = 2 * std::hypot(c, sn) / n / 0.2;
        INFO("period " << period << ": amplitude kept " << amp);
        CHECK(amp == Catch::Approx(expect).margin(0.01));
    }
}

TEST_CASE("Hot pixels fixed on the sensor are found and repaired; a moving moon's peak is not", "[accumulate]") {
    const int w = 128, h = 96;
    ser::FileInfo info = testing::NewCapture(w, h, ser::ColorId::BayerRGGB).info;
    // Hot sites (same-colour neighbours 2 px away): red, blue, green, in the sky and one on the planet.
    const std::vector<std::pair<int, int>> hot = {{20, 14}, {101, 77}, {64, 48}, {33, 70}, {90, 21}};
    std::mt19937 rng(7);
    std::normal_distribution<double> n(0, 1);
    std::uniform_real_distribution<double> uni(-3, 3);
    auto frame = [&](double jx, double jy) {
        ser::Frame f;
        f.width = w;
        f.height = h;
        f.channels = 1;
        f.samples.resize(static_cast<size_t>(w) * h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const double dx = x - (64 + jx), dy = y - (48 + jy);
                double v = 0.05 + 0.5 * 0.5 * std::erfc((std::hypot(dx, dy) - 20) / (std::sqrt(2.0) * 1.5));
                // A moon (PSF sigma 1.6 px) perfectly still on one sensor site in every frame (the hardest
                // case): only its shape tells it from a hot pixel.
                const double mx = x - 100.0, my = y - 30.0;
                v += 0.6 * std::exp(-(mx * mx + my * my) / (2 * 1.6 * 1.6));
                v += 0.004 * n(rng);
                for (const auto& [hx, hy] : hot)
                    if (x == hx && y == hy) v += 0.3;
                f.samples[static_cast<size_t>(y) * w + x] = static_cast<uint16_t>(std::lround(std::clamp(v, 0.0, 1.0) * 65535));
            }
        return f;
    };
    stack::HotPixelMap few(info);
    for (int k = 0; k < stack::kMinHotFrames - 1; ++k) few.Observe(frame(uni(rng), uni(rng)));
    few.Finish();
    CHECK(few.Count() == 0);  // too few frames to tell

    stack::HotPixelMap a(info), b(info);
    for (int k = 0; k < 40; ++k) (k % 2 ? a : b).Observe(frame(uni(rng), uni(rng)));
    a.Merge(b);
    CHECK(a.FramesObserved() == 40);
    a.Finish();
    CHECK(a.Count() == hot.size());

    ser::Frame f = frame(0.7, -1.2);
    const ser::Frame before = f;
    a.Repair(f);
    size_t changed = 0;
    for (size_t k = 0; k < f.samples.size(); ++k) changed += f.samples[k] != before.samples[k];
    CHECK(changed == hot.size());
    for (const auto& [hx, hy] : hot) {
        const double v = f.samples[static_cast<size_t>(hy) * w + hx] / 65535.0;
        const double left = before.samples[static_cast<size_t>(hy) * w + hx - 2] / 65535.0;
        const double right = before.samples[static_cast<size_t>(hy) * w + hx + 2] / 65535.0;
        INFO("hot site " << hx << "," << hy << ": repaired " << v << ", neighbours " << left << " / " << right);
        CHECK(std::fabs(v - 0.5 * (left + right)) < 0.03);
    }
}

TEST_CASE("Rejection leaves lucky detail alone: sharp point features, good seeing, low noise", "[accumulate]") {
    // Review finding: thin structure (crater rims, spots, gaps) has a large residual at its own pixels
    // only; a spatially smoothed scale tolerated the surrounding noise there, so the sharpest frames
    // were rejected exactly where they are sharpest, and pixels emptied.
    const int w = 224, h = 176;
    Scene s = testing::MakeSurface(31, w, h);
    std::mt19937 r(9);
    std::uniform_real_distribution<double> u(0, 1);
    for (int i = 0; i < 150; ++i) s.blobs.push_back({20 + (w - 40) * u(r), 20 + (h - 40) * u(r), 0.6, u(r) < 0.5 ? 0.3 : -0.25});
    s.Bin(-40, -40, w + 40, h + 40);
    Capture cap = testing::NewCapture(w, h);
    std::mt19937 rng(77);
    RenderCapture(cap, s, 80, [&](int) {
        FrameSpec f = Seeing(rng, w, h, 2.0, 40, 0.7);
        f.noise = 0.003;
        return f;
    });
    const Pipeline p = Run(cap, true);
    FloatImage noRejection = p.sigma;
    for (float& v : noRejection.data) v = 1e9f;
    stack::Accumulator open(p.plan, p.reference, noRejection, p.info);
    auto src = cap.Factory()();
    for (int64_t f : p.used) open.Add(f, stack::DemosaicHQ(src->ReadFrame(f), p.info.colorId, p.info.normDepth));
    const FloatImage without = open.Finish();
    auto inside = [&](int x, int y) { return x >= 16 && y >= 16 && x < w - 16 && y < h - 16; };
    const double with = RmsToTruth(p.ours, s, inside), base = RmsToTruth(without, s, inside);
    int zeros = 0;
    for (float v : p.ours.data) zeros += v == 0.f;
    const double share = static_cast<double>(p.acc->RejectedCells()) / (static_cast<double>(w) * h * p.used.size());
    INFO("rms with rejection " << with << ", without " << base << "; rejected " << 100 * share << "% of cells; zero pixels "
                                << zeros);
    CHECK(with < 1.01 * base);
    CHECK(share < 2e-4);
    CHECK(zeros == 0);
}

TEST_CASE("Rejection never empties a pixel", "[accumulate]") {
    // A region keeps only `need` frames and every one of them carries a feature the reference lacks
    // (here forced: a residual scale far too small), so all its samples there are rejected. The pixel
    // must still get a value -- from the rejected samples -- never 0.
    const int w = 160, h = 128;
    const Scene s = testing::MakeSurface(45, w, h);
    FloatImage ref{w, h, 1, std::vector<float>(static_cast<size_t>(w) * h)};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) ref.data[static_cast<size_t>(y) * w + x] = static_cast<float>(s.Eval(x, y, 0.6));
    const stack::RegionLayout lay = stack::PlaceRegions(ref, stack::MeasureFrame(ref, 0), true);
    std::vector<FrameRegions> measured;
    std::mt19937 rng(3);
    std::normal_distribution<double> n(0, 1);
    std::vector<FloatImage> frames;
    for (int k = 0; k < 10; ++k) {
        FrameRegions m;
        m.frameIndex = k;
        for (size_t r = 0; r < lay.regions.size(); ++r) {
            stack::RegionMeasure q;
            q.aligned = true;
            q.confidence = 0.95f;
            q.quality = static_cast<float>(k < 5 ? 1.0 : 0.2);  // frames 0-4 are the sharp ones everywhere
            q.qualitySigma = 0.001f;
            m.regions.push_back(q);
        }
        measured.push_back(m);
        FloatImage im = ref;
        for (int y = 60; y < 64; ++y)
            for (int x = 70; x < 74; ++x) im.data[static_cast<size_t>(y) * w + x] += 0.4f;  // the feature
        for (float& v : im.data) v += static_cast<float>(0.002 * n(rng));
        frames.push_back(std::move(im));
    }
    const stack::RegionPlan plan = stack::PlanRegions(lay, measured);
    FloatImage tiny{w, h, 1, std::vector<float>(static_cast<size_t>(w) * h, 1e-4f)};
    stack::Accumulator acc(plan, ref, tiny, testing::NewCapture(w, h).info);
    for (int k = 0; k < 10; ++k) acc.Add(k, frames[static_cast<size_t>(k)]);
    const FloatImage out = acc.Finish();
    CHECK(acc.RejectedCells() > 0);
    int zeros = 0;
    for (float v : out.data) zeros += v == 0.f;
    CHECK(zeros == 0);
    // Where everything was rejected the rejected samples stand in: the feature, not a hole.
    const double v = out.data[static_cast<size_t>(62) * w + 72], expect = ref.data[static_cast<size_t>(62) * w + 72] + 0.4;
    CHECK(v == Catch::Approx(expect).margin(0.02));
}
