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

using testing::RenderCapture;
using testing::RmsToTruth;
using testing::Seeing;

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
    const stack::NoiseModel noise = stack::FitNoiseModel(factory, p.info, threads, {});
    p.stats = stack::MeasureAll(factory, p.info, threads, {}, noise);
    p.sel = stack::SelectFrames(p.stats, p.info);
    REQUIRE(p.sel.diagnosis.kind == stack::DiagnosisKind::None);
    p.reference = stack::BuildReference(factory, p.info, p.sel, p.stats, threads, {});
    const stack::GlobalAligner aligner(p.reference, p.sel.fullFrameTarget, stack::LumaScale(p.info));
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
        lumas.push_back(stack::MeasurementLuma(src->ReadFrame(f), p.info));
        all.push_back(aligner.Align(lumas.back(), p.stats[static_cast<size_t>(f)]));
    }
    const double cut = keepTorn ? 0.0 : stack::AlignmentCut(all);
    const stack::RegionLayout layout =
        stack::PlaceRegions(p.reference, stack::MeasureFrame(p.reference, 0, stack::LumaScale(p.info)), p.sel.fullFrameTarget, noise);
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

// The fallback alone: the same stacked frames with their gains, on the global shift, no region (the stack
// where the regions' blend mask is 0).
FloatImage FallbackAlone(const Capture& cap, const Pipeline& p) {
    stack::RegionLayout none = p.plan.layout;
    none.regions.clear();
    std::fill(none.node.begin(), none.node.end(), -1);
    std::vector<FrameRegions> bare;
    for (const FrameRegions& m : p.measured)
        if (p.plan.Uses(m.frameIndex)) {
            FrameRegions b = m;
            b.regions.clear();
            bare.push_back(b);
        }
    const stack::RegionPlan plan = stack::PlanRegions(none, bare);
    stack::Accumulator fb(plan, p.reference, p.sigma, p.info);
    auto src = cap.Factory()();
    for (const FrameRegions& b : bare) fb.Add(b.frameIndex, stack::DemosaicHQ(src->ReadFrame(b.frameIndex), p.info.colorId, p.info.normDepth));
    return fb.Finish();
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

// taper: the layout also gets a blend mask (RegionLayout::mask) falling from 1 to 0 across the frame through
// several region windows (E1 fix round 1, review M2): the weights' partition of unity where it tapers.
static void FlatFieldStacksWithoutSeams(bool taper) {
    // A surface reference places regions; the frames are a flat field with noise and a gain that
    // varies from frame to frame (haze); each region keeps a different random subset.
    const int w = 192, h = 160;
    const Scene s = testing::MakeSurface(40, w, h);
    FloatImage ref{w, h, 1, std::vector<float>(static_cast<size_t>(w) * h)};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) ref.data[static_cast<size_t>(y) * w + x] = static_cast<float>(s.Eval(x, y, 0.6));
    stack::RegionLayout lay = stack::PlaceRegions(ref, stack::MeasureFrame(ref, 0, 1), true, stack::NoiseModel{});
    REQUIRE(lay.mask.Empty());  // a surface without sky
    if (taper) {
        auto m = std::make_shared<std::vector<float>>(static_cast<size_t>(w) * h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const double t = std::clamp((x - 48) / 96.0, 0.0, 1.0);
                (*m)[static_cast<size_t>(y) * w + x] = static_cast<float>(0.5 * (1 + std::cos(M_PI * t)));
            }
        lay.mask.width = w;
        lay.mask.height = h;
        lay.mask.m = m;
        REQUIRE(lay.size <= 48);  // the taper crosses several windows
    }
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

TEST_CASE("A flat field stacks without seams, whatever frames each region keeps", "[accumulate]") {
    FlatFieldStacksWithoutSeams(false);
}

TEST_CASE("A flat field stacks without seams where the blend mask tapers (E1 fix round 1)", "[accumulate]") {
    FlatFieldStacksWithoutSeams(true);
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
                    rs.Observe(stack::MeasurementLuma(fr, q.info), q.measured[i]);
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
        // Unrejected, the trail would add 0.5 / (frames kept) -- at least 0.5 over the most any region
        // keeps -- at its centre; rejection must remove at least 90% of that. What gets through is the
        // trail's fringe under kGrowSigmas, spread over the frames the regions there keep, so it moves with
        // their counts: measured mono 0.00099 with the region noise of Task 5, 0.00101 with Ruling P25's,
        // 0.00131 with Ruling P26's noise model, where the mono regions keep at most 31 of the 40 frames
        // (sharper subsets: 92% of the unrejected 0.5 / 31 removed; the bound moved with the counts, from
        // 0.5 / 40 to 0.5 / the most kept); Bayer 0 with all three (at most 22 kept now).
        int64_t keptMost = 0;
        for (const stack::RegionDetail& d : q.plan.detail) keptMost = std::max(keptMost, d.kept);
        INFO("most frames kept by a region " << keptMost);
        REQUIRE(keptMost > 0);
        CHECK(leak < 0.1 * 0.5 / static_cast<double>(keptMost));
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

TEST_CASE("Concurrent Add on a Bayer capture gives the sequential result: odd luma height and a trail on the row seam", "[accumulate]") {
    // Review round 2 (M3): the 12-lane 3-channel sampling, a last luma row holding 3 sensor rows
    // (127 rows: luma 63, maxRows 3) and rejection across the seam where a frame's two row segments
    // meet (luma row pos * 97 % lumaH), on 1 thread in frame order and on 8 in reverse, interleaved.
    const int w = 160, h = 127;
    const Scene s = testing::MakeSurface(46, w, h);
    Capture clean = testing::NewCapture(w, h, ser::ColorId::BayerRGGB);
    std::mt19937 rng(84);
    std::vector<FrameSpec> specs;
    for (int k = 0; k < 40; ++k) {
        FrameSpec f = Seeing(rng, w, h, 1.5, 48);
        if (k == 17)  // the sharpest frame everywhere, so every region stacks it
            for (double& v : f.sigma.v) v = 0.4;
        specs.push_back(f);
    }
    RenderCapture(clean, s, 40, [&](int k) { return specs[static_cast<size_t>(k)]; });
    const Pipeline q = Run(clean, true);
    const int lumaH = q.reference.height;
    REQUIRE(lumaH == 63);
    const int64_t pos = q.plan.Find(17);
    REQUIRE(pos >= 0);
    for (size_t r = 0; r < q.plan.RegionCount(); ++r)
        REQUIRE((q.plan.flags[static_cast<size_t>(pos) * q.plan.RegionCount() + r] & stack::RegionPlan::kKept) != 0);
    const int seam = static_cast<int>((pos * 97) % lumaH);
    INFO("frame 17's seam at luma row " << seam);
    REQUIRE(seam > stack::kRejectReach + 2);
    REQUIRE(seam < lumaH - stack::kRejectReach - 2);
    // Frame 17 crossed by a trail lying along its seam (luma rows seam - 1 .. seam + 1) and, for
    // comparison, by a second one 26 luma rows away; bright and dark (the leak is half their
    // difference, as in the streak test).
    const int other = seam + 26;
    REQUIRE(other < lumaH - stack::kRejectReach - 2);
    auto trailDist = [&](int x, int y, int lumaRow) {
        const double y0 = 2 * lumaRow - 1.5, lx = w, ly = 4.0;
        return std::fabs(x * ly - (y - y0) * lx) / std::hypot(lx, ly);
    };
    auto run = [&](double level, int threads, int64_t& rejected) {
        std::vector<float> v = testing::RenderFrame(s, w, h, specs[17], 1000u + 17);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                for (int row : {seam, other}) {
                    const double d = trailDist(x, y, row);
                    v[static_cast<size_t>(y) * w + x] += static_cast<float>(level * std::exp(-d * d / (2 * 0.75 * 0.75)));
                }
        Capture one = testing::NewCapture(w, h, ser::ColorId::BayerRGGB);
        one.Add(v);
        auto read = [&](int64_t f) { return f == 17 ? one.Factory()()->ReadFrame(0) : clean.Factory()()->ReadFrame(f); };
        stack::ResidualScale rs(q.plan.layout, q.reference);
        for (size_t i = 0; i < q.used.size(); ++i) rs.Observe(stack::MeasurementLuma(read(q.used[i]), q.info), q.measured[i]);
        stack::Accumulator acc(q.plan, q.reference, rs.Sigma(q.info.normDepth), q.info);
        std::vector<FloatImage> frames;
        for (int64_t f : q.used) frames.push_back(stack::DemosaicHQ(read(f), q.info.colorId, q.info.normDepth));
        std::vector<std::thread> pool;
        for (int t = 0; t < threads; ++t)
            pool.emplace_back([&, t] {
                if (threads == 1)
                    for (size_t i = 0; i < frames.size(); ++i) acc.Add(q.used[i], frames[i]);
                else
                    for (size_t i = frames.size(); i-- > 0;)
                        if (static_cast<int>(i % threads) == t) acc.Add(q.used[i], frames[i]);
            });
        for (auto& th : pool) th.join();
        CHECK(acc.FramesAdded() == q.plan.stackedCount);
        rejected = acc.RejectedCells();
        return acc.Finish();
    };
    int64_t rej1 = 0, rej8 = 0, rejDark = 0;
    const FloatImage bright1 = run(0.5, 1, rej1), bright8 = run(0.5, 8, rej8), dark = run(-0.5, 8, rejDark);
    REQUIRE(bright1.channels == 3);
    REQUIRE(bright1.data.size() == bright8.data.size());
    double worst = 0;
    for (size_t i = 0; i < bright1.data.size(); ++i)
        worst = std::max(worst, static_cast<double>(std::fabs(bright1.data[i] - bright8.data[i])));
    // Float sums in a different order (Accumulate.h "Thread safety").
    CHECK(worst < 1e-6);
    CHECK(rej1 == rej8);
    CHECK(rej1 > 0);
    CHECK(rejDark > 0);
    // Rejected across the seam as anywhere: each trail's leak within 4 sensor px of its centre line,
    // against 0.5 / 40 unrejected (the Bayer leak is ~0.001 anywhere, "A single-frame streak").
    double leakSeam = 0, leakOther = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 8; x < w - 8; ++x)
            for (int c = 0; c < 3; ++c) {
                const size_t i = (static_cast<size_t>(y) * w + x) * 3 + c;
                const double l = 0.5 * std::fabs(bright8.data[i] - dark.data[i]);
                if (trailDist(x, y, seam) <= 4) leakSeam = std::max(leakSeam, l);
                if (trailDist(x, y, other) <= 4) leakOther = std::max(leakOther, l);
            }
    INFO("thread order max diff " << worst << "; rejected cells " << rej1 << " / " << rej8 << "; trail leak on the seam "
                                  << leakSeam << ", away from it " << leakOther);
    CHECK(leakSeam < 0.002);
    CHECK(leakOther < 0.002);
    CHECK(leakSeam < 1.5 * leakOther + 2e-4);
}

TEST_CASE("ResidualScale and Accumulator argument errors", "[accumulate]") {
    const int w = 96, h = 80;
    const Scene s = testing::MakeSurface(44, w, h);
    FloatImage ref{w, h, 1, std::vector<float>(static_cast<size_t>(w) * h)};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) ref.data[static_cast<size_t>(y) * w + x] = static_cast<float>(s.Eval(x, y, 0.6));
    const stack::RegionLayout lay = stack::PlaceRegions(ref, stack::MeasureFrame(ref, 0, 1), true, stack::NoiseModel{});
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

TEST_CASE("The Accumulator holds a moved-in plan without copying it; HotPixelMap releases its counts at Finish (final review, engine M3)",
          "[accumulate]") {
    const int w = 64, h = 48;
    FloatImage im{w, h, 1, std::vector<float>(static_cast<size_t>(w) * h, 0.4f)};
    stack::RegionLayout none;
    none.width = w;
    none.height = h;
    std::vector<FrameRegions> measured(3);
    for (int k = 0; k < 3; ++k) measured[static_cast<size_t>(k)].frameIndex = k;
    stack::RegionPlan plan = stack::PlanRegions(none, measured);
    const int64_t* frames = plan.frames.data();
    const stack::Accumulator moved(std::move(plan), im, im, testing::NewCapture(w, h).info);
    CHECK(moved.Plan().frames.data() == frames);  // the same storage: moved, not copied
    CHECK(moved.Plan().frames.size() == 3);
    const stack::RegionPlan kept = stack::PlanRegions(none, measured);
    const stack::Accumulator copied(kept, im, im, testing::NewCapture(w, h).info);  // an lvalue is copied
    CHECK(copied.Plan().frames == kept.frames);
    CHECK(copied.Plan().frames.data() != kept.frames.data());

    const ser::FileInfo info = testing::NewCapture(w, h).info;
    stack::HotPixelMap map(info);
    CHECK(map.BytesHeld() >= static_cast<size_t>(w) * h * 2);  // 2 bytes per sample while observing
    ser::Frame f;
    f.width = w;
    f.height = h;
    f.channels = 1;
    f.samples.assign(static_cast<size_t>(w) * h, 6000);
    f.samples[static_cast<size_t>(20) * w + 30] = 40000;
    for (int k = 0; k < 20; ++k) map.Observe(f);
    map.Finish();
    REQUIRE(map.Count() == 1);
    CHECK(map.BytesHeld() < 64);  // only the site
    ser::Frame g = f;
    map.Repair(g);
    CHECK(g.samples[static_cast<size_t>(20) * w + 30] == 6000);  // still repairs
    CHECK_THROWS_AS(map.Observe(f), std::logic_error);
    stack::HotPixelMap other(info);
    CHECK_THROWS_AS(map.Merge(other), std::logic_error);
    map.Finish();  // a second call does nothing
    CHECK(map.Count() == 1);
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

TEST_CASE("The stack stays inside the frames' range: no overshoot at a clipped edge, no gain lift of a saturated sample "
          "(field defect D2)",
          "[accumulate]") {
    // The 2026-10-23 Moon (PlanetaryStack 0.3.0.0) read up to 1.00265 in blue at 17 pixels of its brightest craters,
    // where the sensor saturated: PixInsight images are [0, 1]. One frame here, no regions, as above: a hard edge
    // from full scale (1) down to 0.3, and one from black (0) up to 0.7, each at whole and fractional shifts, at
    // the reference's brightness (gain 1) and dimmed by haze (gain 0.98, which the stack divides out).
    const int w = 64, h = 24;
    auto edge = [&](float left, float right) {
        FloatImage im{w, h, 1, std::vector<float>(static_cast<size_t>(w) * h)};
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) im.data[static_cast<size_t>(y) * w + x] = x < w / 2 ? left : right;
        return im;
    };
    auto stackOne = [&](const FloatImage& im, double shift, double gain) {
        stack::RegionLayout none;
        none.width = w;
        none.height = h;
        FrameRegions m;
        m.frameIndex = 0;
        m.global = Shift{shift, 0, 1};
        m.gain = gain;
        const stack::RegionPlan plan = stack::PlanRegions(none, {m});
        const FloatImage noRejection{w, h, 1, std::vector<float>(static_cast<size_t>(w) * h, 1.f)};
        stack::Accumulator acc(plan, im, noRejection, testing::NewCapture(w, h).info);
        acc.Add(0, im);
        return acc.Finish();
    };
    const FloatImage saturated = edge(1.f, 0.3f), black = edge(0.f, 0.7f);
    double hi = 0, lo = 1;
    for (double shift : {0.0, 0.25, 0.5})
        for (double gain : {1.0, 0.98}) {
            for (float v : stackOne(saturated, shift, gain).data) hi = std::max(hi, static_cast<double>(v));
            for (float v : stackOne(black, shift, gain).data) lo = std::min(lo, static_cast<double>(v));
        }
    INFO("largest sample " << hi << ", smallest " << lo);
    CHECK(hi <= 1.0);
    CHECK(lo >= 0.0);
}

namespace {
// Bounds the behaviour before Ruling R2 (3123eea8) meets in the tests below, measured there with these tests.
// The R2 test's largest error off its plateau (identical frames, no shift): mono and RGB 5.8e-6; RGGB 0.065, the
// demosaic's own error next to the hard plateau edge.
constexpr double kRimTolerance[2] = {1e-3, 0.07};
// The crater test's largest error where the truth is under 0.95: mono 0.044, RGGB 0.050 -- the +-0.5 px jitter's blur
// of the 1 px ramp.
constexpr double kCraterRimBlur = 0.06;
}  // namespace

TEST_CASE("A clipped sample is no measurement: a saturated plateau stays at the reference's clipped level whatever the "
          "frames' gains (Ruling R2)",
          "[accumulate]") {
    // The 2023-02-12 Moon: frames 0-260 are 1.257x brighter than the rest (FrameRegions::gain) and saturated over the
    // south limb; divided by their gain, their clipped samples read 0.796 and dimmed the limb wherever a region kept
    // them. Two frame groups here, no regions, both saturated on a plateau: one at the reference's brightness, one
    // 1.25x brighter. Off the plateau both see the same scene (the brighter group's samples 1.25x the reference's).
    const int w = 64, h = 40, n = 6;
    auto plateau = [&](int x, int y) { return x >= 20 && x < 44 && y >= 12 && y < 28; };
    auto scene = [&](int x, int y) { return 0.3 + 0.004 * x; };  // up to 0.55, unclipped in both groups
    for (ser::ColorId color : {ser::ColorId::Mono, ser::ColorId::RGB, ser::ColorId::BayerRGGB}) {
        const bool mosaic = color == ser::ColorId::BayerRGGB;
        const int c = color == ser::ColorId::Mono ? 1 : 3;
        INFO((color == ser::ColorId::Mono ? "mono" : mosaic ? "Bayer" : "RGB"));
        const ser::FileInfo info = testing::NewCapture(w, h, color).info;
        // RGB: only blue saturates on the plateau (red and green read the scene); mono and Bayer: every sample.
        auto frame = [&](double gain) {
            ser::Frame raw;
            raw.width = w;
            raw.height = h;
            raw.channels = mosaic ? 1 : c;
            raw.samples.resize(static_cast<size_t>(w) * h * raw.channels);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    for (int k = 0; k < raw.channels; ++k) {
                        const bool clip = plateau(x, y) && (c == 1 || mosaic || k == 2);
                        const double v = clip ? 1.0 : std::min(1.0, gain * scene(x, y));
                        raw.samples[(static_cast<size_t>(y) * w + x) * raw.channels + k] =
                            static_cast<uint16_t>(std::lround(v * 65535));
                    }
            return stack::DemosaicHQ(raw, color, 16);
        };
        stack::RegionLayout none;
        none.width = w / (mosaic ? 2 : 1);
        none.height = h / (mosaic ? 2 : 1);
        std::vector<FrameRegions> ms;
        for (int k = 0; k < n; ++k) {
            FrameRegions m;
            m.frameIndex = k;
            m.gain = k < n / 2 ? 1.0 : 1.25;
            ms.push_back(m);
        }
        const stack::RegionPlan plan = stack::PlanRegions(none, ms);
        FloatImage ref{none.width, none.height, 1, std::vector<float>(static_cast<size_t>(none.width) * none.height, 0.5f)};
        const FloatImage noRejection{none.width, none.height, 1, std::vector<float>(ref.data.size(), 1.f)};
        stack::Accumulator acc(plan, ref, noRejection, info);
        for (int k = 0; k < n; ++k) acc.Add(k, frame(ms[static_cast<size_t>(k)].gain));
        const FloatImage out = acc.Finish();
        // Inside the plateau, 2 px clear of its edge (the demosaic's and the interpolation's reach).
        double lo = 1, hi = 0, worstScene = 0;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                for (int k = 0; k < c; ++k) {
                    const float v = out.data[(static_cast<size_t>(y) * w + x) * c + k];
                    const bool clipped = c == 1 || mosaic || k == 2;
                    if (clipped && x >= 22 && x < 42 && y >= 14 && y < 26) {
                        lo = std::min(lo, static_cast<double>(v));
                        hi = std::max(hi, static_cast<double>(v));
                    }
                    // Everywhere off the plateau, its rim included (review I2: a sample next to a clip is no clipped
                    // sample), and in every unclipped channel, 3 px clear of the frame's edge.
                    if ((!plateau(x, y) || !clipped) && x >= 3 && x < w - 3 && y >= 3 && y < h - 3)
                        worstScene = std::max(worstScene, std::fabs(v - scene(x, y)));
                }
        INFO("plateau " << lo << " - " << hi << "; largest error off it (or in an unclipped channel) " << worstScene);
        CHECK(lo >= 0.999);  // the reference's clipped level, not dimmed by the brighter group
        CHECK(hi <= 1.0);
        CHECK(worstScene < kRimTolerance[mosaic ? 1 : 0]);  // unclipped samples are still gain-corrected and stacked
    }
}

namespace {

// Mono frames of a scene given at sensor resolution, or a grey RGGB mosaic of it through DemosaicHQ, 16-bit.
FloatImage SceneFrame(const std::function<double(int, int)>& scene, int w, int h, bool mosaic) {
    ser::Frame raw;
    raw.width = w;
    raw.height = h;
    raw.channels = 1;
    raw.samples.resize(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            raw.samples[static_cast<size_t>(y) * w + x] =
                static_cast<uint16_t>(std::lround(std::clamp(scene(x, y), 0.0, 1.0) * 65535));
    return stack::DemosaicHQ(raw, mosaic ? ser::ColorId::BayerRGGB : ser::ColorId::Mono, 16);
}

// The reference (luma) of a scene: itself in mono, the 2 x 2 site mean of a mosaic.
FloatImage SceneLuma(const std::function<double(int, int)>& scene, int w, int h, bool mosaic) {
    const int s = mosaic ? 2 : 1;
    FloatImage im{w / s, h / s, 1, std::vector<float>(static_cast<size_t>(w / s) * (h / s))};
    for (int y = 0; y < h / s; ++y)
        for (int x = 0; x < w / s; ++x) {
            double v = 0;
            for (int j = 0; j < s; ++j)
                for (int i = 0; i < s; ++i) v += std::clamp(scene(x * s + i, y * s + j), 0.0, 1.0);
            im.data[static_cast<size_t>(y) * (w / s) + x] = static_cast<float>(v / (s * s));
        }
    return im;
}

// One Accumulator over frames that each have a global shift (luma px) and a gain, no regions.
FloatImage StackShifted(const std::vector<FloatImage>& frames, const std::vector<Shift>& shifts, const std::vector<double>& gains,
                        const FloatImage& ref, float sigma, ser::ColorId color, int64_t* rejected = nullptr) {
    stack::RegionLayout none;
    none.width = ref.width;
    none.height = ref.height;
    std::vector<FrameRegions> ms;
    for (size_t k = 0; k < frames.size(); ++k) {
        FrameRegions m;
        m.frameIndex = static_cast<int64_t>(k);
        m.global = shifts[k];
        m.gain = gains[k];
        ms.push_back(m);
    }
    const stack::RegionPlan plan = stack::PlanRegions(none, ms);
    const FloatImage sig{ref.width, ref.height, 1, std::vector<float>(ref.data.size(), sigma)};
    stack::Accumulator acc(plan, ref, sig, testing::NewCapture(frames[0].width, frames[0].height, color).info);
    for (size_t k = 0; k < frames.size(); ++k) acc.Add(static_cast<int64_t>(k), frames[k]);
    if (rejected) *rejected = acc.RejectedCells();
    return acc.Finish();
}

}  // namespace

TEST_CASE("The rim of a saturated crater under jitter keeps its values: no bloat, no moat (review C1)", "[accumulate]") {
    // Fix round 1 (Ruling R2) dropped every sample next to a clip as no measurement: where every frame's rim sample was
    // flagged the rim read full scale (bloat), where only some were the stack averaged the frames the clip had not
    // reached -- the dimmer ones -- and read low (moat; the 2023-02-12 Moon's limb, G 0.916 -> 0.729). A crater of
    // 1.3 over 0.6 with a 1 px ramp, area-sampled 4 x 4 per pixel and clipped, 40 frames jittered +-0.5 px.
    const int w = 64, h = 40, n = 40;
    auto crater = [](double x, double y) { return 0.6 + 0.7 * std::clamp(9.0 - std::hypot(x - 32, y - 20), 0.0, 1.0); };
    auto area = [&](double dx, double dy) {
        return [=](int x, int y) {
            double v = 0;
            for (int j = 0; j < 4; ++j)
                for (int i = 0; i < 4; ++i) v += crater(x + (i + 0.5) / 4 - 0.5 - dx, y + (j + 0.5) / 4 - 0.5 - dy);
            return std::min(1.0, v / 16);
        };
    };
    const auto truth = area(0, 0);
    for (bool mosaic : {false, true}) {
        INFO((mosaic ? "Bayer" : "mono"));
        std::mt19937 rng(5);
        std::uniform_real_distribution<double> jit(-0.5, 0.5);
        std::vector<FloatImage> frames;
        std::vector<Shift> shifts;
        for (int k = 0; k < n; ++k) {
            const double dx = jit(rng), dy = jit(rng);  // the scene's point x shows at x + d: frame(x + g) ~ ref(x)
            frames.push_back(SceneFrame(area(dx, dy), w, h, mosaic));
            shifts.push_back(Shift{dx / (mosaic ? 2 : 1), dy / (mosaic ? 2 : 1), 1});
        }
        const int c = mosaic ? 3 : 1;
        const FloatImage out = StackShifted(frames, shifts, std::vector<double>(n, 1.0), SceneLuma(truth, w, h, mosaic), 1.f,
                                            mosaic ? ser::ColorId::BayerRGGB : ser::ColorId::Mono);
        double worst = 0, plateauLo = 1;
        int bloated = 0;
        for (int y = 3; y < h - 3; ++y)
            for (int x = 3; x < w - 3; ++x)
                for (int k = 0; k < c; ++k) {
                    const double t = truth(x, y), v = out.data[(static_cast<size_t>(y) * w + x) * c + k];
                    if (t < 0.95) worst = std::max(worst, std::fabs(v - t));
                    if (t < 0.9 && v >= 0.99) ++bloated;
                    if (std::hypot(x - 32, y - 20) < 6) plateauLo = std::min(plateauLo, v);
                }
        INFO("largest error where the truth is under 0.95: " << worst << "; samples of truth < 0.9 reading >= 0.99: " << bloated
                                                             << "; plateau's lowest " << plateauLo);
        // Measured at 3123eea8 (before R2): the jitter's blur of the 1 px ramp, mono and Bayer alike (this test).
        CHECK(worst < kCraterRimBlur);
        CHECK(bloated == 0);
        CHECK(plateauLo >= 0.999);
    }
}

TEST_CASE("Clipped samples are present: an unclipped outlier among them and an out-of-frame mirror do not set the pixel "
          "(review C2)",
          "[accumulate]") {
    // Fix round 1 counted no clipped weight as presence: one unclipped sample, a rejected outlier or a 1e-6 out-of-frame
    // mirror, was all the pixel had.
    const int w = 64, h = 40, n = 20;
    SECTION("a dark outlier in one frame over a plateau saturated in the others, with and without rejection") {
        auto plateau = [](int x, int y) { return x >= 20 && x < 44 && y >= 12 && y < 28; };
        auto scene = [&](int x, int y) { return plateau(x, y) ? 1.0 : 0.5; };
        auto outlier = [&](int x, int y) { return x >= 29 && x < 35 && y >= 17 && y < 23 ? 0.6 : scene(x, y); };
        std::vector<FloatImage> frames;
        for (int k = 0; k < n; ++k) frames.push_back(SceneFrame(k == 0 ? std::function<double(int, int)>(outlier) : scene, w, h, false));
        for (float sigma : {1.f, 0.01f}) {
            INFO("residual scale " << sigma);
            const FloatImage out = StackShifted(frames, std::vector<Shift>(n), std::vector<double>(n, 1.0), SceneLuma(scene, w, h, false),
                                                sigma, ser::ColorId::Mono);
            double lo = 1;
            for (int y = 17; y < 23; ++y)
                for (int x = 29; x < 35; ++x) lo = std::min(lo, static_cast<double>(out.data[static_cast<size_t>(y) * w + x]));
            INFO("the outlier patch reads at least " << lo);
            CHECK(lo > 0.95);  // 3123eea8: 0.98 (sigma 1), 1.0 (sigma 0.01)
        }
    }
    SECTION("a plateau saturated in every frame at the frame's edge, one frame drifted 30 px off it") {
        auto scene = [](int x, int) { return x < 12 ? 1.0 : 0.2; };
        std::vector<FloatImage> frames;
        std::vector<Shift> shifts(n);
        frames.push_back(SceneFrame([&](int x, int y) { return scene(x + 30, y); }, w, h, false));
        shifts[0] = Shift{-30, 0, 1};
        for (int k = 1; k < n; ++k) frames.push_back(SceneFrame(scene, w, h, false));
        const FloatImage out = StackShifted(frames, shifts, std::vector<double>(n, 1.0), SceneLuma(scene, w, h, false), 1.f,
                                            ser::ColorId::Mono);
        double lo = 1;
        for (int y = 0; y < h; ++y)
            for (int x = 1; x < 10; ++x) lo = std::min(lo, static_cast<double>(out.data[static_cast<size_t>(y) * w + x]));
        INFO("the plateau reads at least " << lo);
        CHECK(lo > 0.95);  // 3123eea8: 1.0
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
    std::string sites;
    for (size_t k : a.Sites()) sites += std::to_string(k % w) + "," + std::to_string(k / w) + " ";
    INFO("sites " << sites);
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
    const stack::RegionLayout lay = stack::PlaceRegions(ref, stack::MeasureFrame(ref, 0, 1), true, stack::NoiseModel{});
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

TEST_CASE("A streak in a border band a tenth of the frames reach is rejected as in the interior", "[accumulate]") {
    // Review round 2 (M1): drift moves most frames' edge out of the output's border band, so only a
    // tenth of the frames reach it in-bounds (lunar drift up to 51 px). One of those carries a bright
    // streak. Rejection must take it out there as it does where every frame reaches.
    const int w = 96, h = 64, frames = 100, streaked = 95;
    stack::RegionLayout none;  // the fallback stack alone: every frame equally
    none.width = w;
    none.height = h;
    std::vector<FrameRegions> measured;
    std::vector<FloatImage> imgs;
    std::mt19937 rng(12);
    std::normal_distribution<double> n(0, 1);
    for (int k = 0; k < frames; ++k) {
        FrameRegions m;
        m.frameIndex = k;
        // 90 frames sampled 30 px to the right: x >= 66 is outside them; 10 frames not shifted.
        m.global = Shift{k < 90 ? 30.0 : 0.0, 0, 1};
        measured.push_back(m);
        FloatImage im{w, h, 1, std::vector<float>(static_cast<size_t>(w) * h)};
        for (float& v : im.data) v = static_cast<float>(0.5 + 0.001 * n(rng));
        if (k == streaked)
            for (int y = 30; y < 32; ++y)
                for (int x = 0; x < w; ++x) im.data[static_cast<size_t>(y) * w + x] += 0.5f;
        imgs.push_back(std::move(im));
    }
    const stack::RegionPlan plan = stack::PlanRegions(none, measured);
    const FloatImage flat{w, h, 1, std::vector<float>(static_cast<size_t>(w) * h, 0.5f)};
    const FloatImage sigma{w, h, 1, std::vector<float>(static_cast<size_t>(w) * h, 0.01f)};
    stack::Accumulator acc(plan, flat, sigma, testing::NewCapture(w, h).info);
    for (int k = 0; k < frames; ++k) acc.Add(k, imgs[static_cast<size_t>(k)]);
    const FloatImage out = acc.Finish();
    // The streak's rows against rows far from it, over the same columns: what of the streak is left.
    auto leak = [&](int x0, int x1) {
        double on = 0, off = 0;
        int non = 0, noff = 0;
        for (int x = x0; x < x1; ++x) {
            for (int y = 30; y < 32; ++y, ++non) on += out.data[static_cast<size_t>(y) * w + x];
            for (int y = 44; y < 56; ++y, ++noff) off += out.data[static_cast<size_t>(y) * w + x];
        }
        return on / non - off / noff;
    };
    const double interior = leak(8, 58), band = leak(72, 90);
    INFO("streak left in the interior " << interior << ", in the border band " << band << "; rejected cells "
                                        << acc.RejectedCells());
    // Unrejected: +0.5 / 100 in the interior, +0.5 / 10 = 0.05 in the band.
    CHECK(std::fabs(interior) < 0.001);
    CHECK(std::fabs(band) < 0.001);
    int zeros = 0;
    for (float v : out.data) zeros += v == 0.f;
    CHECK(zeros == 0);
}

TEST_CASE("Hot segments are repaired whole on Bayer and mono sensors; still moons lines and a limb are not flagged", "[accumulate]") {
    // Review round 2 (M2): in a mosaic the same-colour neighbours are 2 px apart, so along a hot
    // column or row segment of 3 px or more the same-colour members are each other's neighbours; on a
    // mono sensor an adjacent pair already is. The optical features are held perfectly still on the
    // sensor in every frame (the hardest case: only their shape tells them from hot pixels), at PSF
    // sigma 1.6 px; the limb is sharper (1.0). Mono: a still point between two pixels and a still line
    // look like a hot pair or segment on the 1-px ring (the third-largest-neighbour rule of round 1
    // flagged 8 and 192 such sites here).
    const int w = 160, h = 128;
    std::vector<std::pair<int, int>> hot;
    auto column = [&](int x, int y0, int n) { for (int y = y0; y < y0 + n; ++y) hot.push_back({x, y}); };
    auto row = [&](int y, int x0, int n) { for (int x = x0; x < x0 + n; ++x) hot.push_back({x, y}); };
    column(20, 90, 3);   // even column, starting on an even row
    column(27, 101, 3);  // odd column, odd row
    column(40, 84, 5);
    column(47, 95, 5);
    row(100, 60, 3);
    row(107, 65, 3);
    row(112, 75, 5);
    row(119, 86, 5);
    for (int i = 0; i < 3; ++i) hot.push_back({100 + i, 100 + i});  // diagonal
    for (int i = 0; i < 3; ++i) hot.push_back({102 - i, 120 + i});  // anti-diagonal
    // Moons centred on an R, a G (R row), a G (B row) and a B site, and between sites.
    const std::vector<std::pair<double, double>> moons = {{30, 30}, {51, 30}, {70, 31}, {91, 31}, {110.5, 30.5}, {130.5, 30}};
    for (ser::ColorId color : {ser::ColorId::BayerRGGB, ser::ColorId::Mono}) {
        INFO((color == ser::ColorId::Mono ? "mono" : "Bayer"));
        const ser::FileInfo info = testing::NewCapture(w, h, color).info;
        std::mt19937 rng(13);
        std::normal_distribution<double> n(0, 1);
        auto frame = [&]() {
            ser::Frame f;
            f.width = w;
            f.height = h;
            f.channels = 1;
            f.samples.resize(static_cast<size_t>(w) * h);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    double v = 0.05;
                    for (const auto& [mx, my] : moons) {
                        const double dx = x - mx, dy = y - my;
                        v += 0.6 * std::exp(-(dx * dx + dy * dy) / (2 * 1.6 * 1.6));
                    }
                    // Thin lines (a ring seen edge-on, a rille's lit wall): along a row, along a column, a
                    // row between two sensor rows, and a diagonal.
                    auto line = [&](double d, double along, double a0, double a1) {
                        return along < a0 || along > a1 ? 0.0 : 0.5 * std::exp(-d * d / (2 * 1.6 * 1.6));
                    };
                    v += line(y - 55.0, x, 10, 70) + line(x - 100.0, y, 45, 75) + line(y - 68.5, x, 10, 70) +
                         line((x - y - 32.0) / std::sqrt(2.0), x, 75, 92);
                    // A still disc, limb sigma 1.0 px.
                    v += 0.45 * 0.5 * std::erfc((std::hypot(x - 132.0, y - 92.0) - 20) / (std::sqrt(2.0) * 1.0));
                    v += 0.004 * n(rng);
                    for (const auto& [hx, hy] : hot)
                        if (x == hx && y == hy) v += 0.3;
                    f.samples[static_cast<size_t>(y) * w + x] = static_cast<uint16_t>(std::lround(std::clamp(v, 0.0, 1.0) * 65535));
                }
            return f;
        };
        stack::HotPixelMap map(info);
        for (int k = 0; k < 20; ++k) map.Observe(frame());
        map.Finish();
        std::vector<size_t> expect;
        for (const auto& [hx, hy] : hot) expect.push_back(static_cast<size_t>(hy) * w + hx);
        std::sort(expect.begin(), expect.end());
        std::string missed, extra;
        size_t extras = 0;
        for (size_t k : expect)
            if (!std::binary_search(map.Sites().begin(), map.Sites().end(), k)) missed += std::to_string(k % w) + "," + std::to_string(k / w) + " ";
        for (size_t k : map.Sites())
            if (!std::binary_search(expect.begin(), expect.end(), k) && ++extras <= 12)
                extra += std::to_string(k % w) + "," + std::to_string(k / w) + " ";
        INFO("hot sites missed: " << missed << "; flagged that are not hot: " << extras << " (" << extra << ")");
        CHECK(missed.empty());
        CHECK(extras == 0);
        ser::Frame f = frame();
        map.Repair(f);
        for (const auto& [hx, hy] : hot) {
            INFO("site " << hx << "," << hy);
            CHECK(f.samples[static_cast<size_t>(hy) * w + hx] / 65535.0 < 0.07);
        }
    }
}

TEST_CASE("Hot pixel clusters on a mono sensor: an adjacent pair and a column segment", "[accumulate]") {
    const int w = 96, h = 80;
    ser::FileInfo info = testing::NewCapture(w, h).info;  // mono: neighbours 1 px away
    const std::vector<std::pair<int, int>> hot = {{20, 20}, {21, 20}, {60, 30}, {60, 31}, {60, 32}};
    std::mt19937 rng(11);
    std::normal_distribution<double> n(0, 1);
    auto frame = [&]() {
        ser::Frame f;
        f.width = w;
        f.height = h;
        f.channels = 1;
        f.samples.resize(static_cast<size_t>(w) * h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                double v = 0.05 + 0.004 * n(rng);
                const double mx = x - 40.0, my = y - 60.0;  // a still moon, PSF sigma 1.6 px
                v += 0.6 * std::exp(-(mx * mx + my * my) / (2 * 1.6 * 1.6));
                for (const auto& [hx, hy] : hot)
                    if (x == hx && y == hy) v += 0.3;
                f.samples[static_cast<size_t>(y) * w + x] = static_cast<uint16_t>(std::lround(std::clamp(v, 0.0, 1.0) * 65535));
            }
        return f;
    };
    stack::HotPixelMap map(info);
    for (int k = 0; k < 20; ++k) map.Observe(frame());
    map.Finish();
    CHECK(map.Count() == hot.size());
    ser::Frame f = frame();
    const ser::Frame before = f;
    map.Repair(f);
    for (const auto& [hx, hy] : hot) CHECK(f.samples[static_cast<size_t>(hy) * w + hx] / 65535.0 < 0.08);
    size_t changed = 0;
    for (size_t k = 0; k < f.samples.size(); ++k) changed += f.samples[k] != before.samples[k];
    CHECK(changed == hot.size());
}

TEST_CASE("Hot pixels inside a tracked textured disc are found and repaired; still points and lines on it are not", "[accumulate]") {
    // Task 6 found it: on a perfectly tracked disc with texture (MakeDisc's belts and spots, seeing
    // blur and local jitter, no drift) the ring rule passed a +0.35 site in only 27 of 50 frames, under
    // kHotShare -- the texture puts a ring neighbour within a tenth of the hot pixel's excess of it.
    // Measured over every site within 21 px of the centre (1386), a hot pixel found = isolated in >= 0.8
    // of the 50 frames, HotPixelMap without the smooth-surface rule: mono +0.35 573, +0.2 6, +0.15 0;
    // Bayer +0.35 177, +0.5 411. With it (Accumulate.h; gated in a mosaic): mono 1386, 1357, 1213;
    // Bayer 1160, 1228.
    const int w = 112, h = 96;
    const double cx = 56.2, cy = 47.8;
    const Scene s = testing::MakeDisc(61, cx, cy, 24);
    Capture base = testing::NewCapture(w, h);
    std::mt19937 rng(6100);
    RenderCapture(base, s, 50, [&](int) { return Seeing(rng, w, h, 0.0, 28); });
    // The same frames with something added at fixed sensor positions in every frame.
    auto with = [&](const std::function<double(int, int)>& add) {
        std::vector<ser::Frame> out;
        for (const auto& f : *base.frames) {
            ser::Frame fr;
            fr.width = w;
            fr.height = h;
            fr.channels = 1;
            fr.samples.resize(f.size());
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    const size_t i = static_cast<size_t>(y) * w + x;
                    fr.samples[i] = static_cast<uint16_t>(std::lround(std::clamp(f[i] / 65535.0 + add(x, y), 0.0, 1.0) * 65535));
                }
            out.push_back(std::move(fr));
        }
        return out;
    };
    // Hot sites on a 5 px grid over the disc (every colour of a mosaic), +0.35 and +0.2 alternately.
    std::vector<std::pair<int, int>> hot;
    std::vector<double> excess;
    for (int y = 30; y <= 66; y += 5)
        for (int x = 38; x <= 75; x += 5)
            if (std::hypot(x - cx, y - cy) <= 19) {
                hot.push_back({x, y});
                excess.push_back(hot.size() % 2 ? 0.35 : 0.2);
            }
    REQUIRE(hot.size() >= 40);
    const auto hotFrames = with([&](int x, int y) {
        for (size_t i = 0; i < hot.size(); ++i)
            if (hot[i].first == x && hot[i].second == y) return excess[i];
        return 0.0;
    });
    // Still optical detail on the disc, PSF sigma 1.6 px, held perfectly still on the sensor in every
    // frame (only its shape tells it from hot pixels): points on a site and between sites, and thin
    // lines along a row, a column and at 35 degrees (the predictor's worst angle, below), +0.25.
    // Optical features have PSF-smooth ends: a segment is a line through (x0, y0) of length len whose
    // ends fall off as erf at the PSF's sigma (a hard-ended one is not an image an optic can form).
    auto gauss = [](double d, double sg) { return std::exp(-d * d / (2 * sg * sg)); };
    auto segment = [&](double x, double y, double x0, double y0, double deg, double len, double sg) {
        const double c = std::cos(deg * M_PI / 180), sn = std::sin(deg * M_PI / 180);
        const double along = (x - x0) * c + (y - y0) * sn, across = -(x - x0) * sn + (y - y0) * c;
        return gauss(across, sg) * 0.5 *
               (std::erf((len / 2 - along) / (M_SQRT2 * sg)) + std::erf((len / 2 + along) / (M_SQRT2 * sg)));
    };
    const auto stillFrames = with([&](int x, int y) {
        double v = 0;
        for (const auto& [px, py] : std::vector<std::pair<double, double>>{{46, 40}, {64.5, 55.5}, {51, 57.5}, {67, 42}})
            v += gauss(std::hypot(x - px, y - py), 1.6);
        v += segment(x, y, 49, 33, 0, 14, 1.6) + segment(x, y, 40, 50, 90, 12, 1.6) +
             segment(x, y, 61.7, 48, 35, 14, 1.6) + segment(x, y, 56.9, 59.4, -35, 12, 1.6);
        return 0.25 * v;
    });
    // Sharper still detail, as native sampling gives on a 2.9 um sensor at f/9-f/12 (PSF sigma 1.0-1.3 px):
    // points, or 10-px lines at a spread of angles, on a grid over the disc at sub-pixel offsets, +0.25.
    // On a mosaic the smooth-surface rule alone flagged such detail (review round 1: sigma 1.0 lines on
    // a disc 0 -> 8 over 6 seeds); the different-colour gate (Accumulate.h) stops it.
    auto sharpFrames = [&](double sg, bool lines) {
        return with([&, sg, lines](int x, int y) {
            double v = 0;
            int k = 0;
            for (int j = -1; j <= 1; ++j)
                for (int i = -1; i <= 1; ++i, ++k) {
                    const double fx = cx + 12 * i + 0.13 * k - 0.5, fy = cy + 12 * j + 0.29 * k - 1.2;
                    if (std::fabs(x - fx) > 12 || std::fabs(y - fy) > 12) continue;
                    v += lines ? segment(x, y, fx, fy, 7 + 19 * k, 10, sg) : gauss(std::hypot(x - fx, y - fy), sg);
                }
            return 0.25 * v;
        });
    };
    for (ser::ColorId color : {ser::ColorId::Mono, ser::ColorId::BayerRGGB}) {
        const bool mono = color == ser::ColorId::Mono;
        INFO((mono ? "mono" : "Bayer"));
        const ser::FileInfo info = testing::NewCapture(w, h, color).info;
        auto mapOf = [&](const std::vector<ser::Frame>& frames) {
            stack::HotPixelMap map(info);
            for (const auto& f : frames) map.Observe(f);
            map.Finish();
            return map;
        };
        // Nothing on the clean disc, nothing on the still detail.
        CHECK(mapOf(stillFrames).Count() == 0);
        for (double sg : {1.0, 1.1, 1.2, 1.3})
            for (bool lines : {false, true}) {
                INFO("still " << (lines ? "lines" : "points") << " at PSF sigma " << sg);
                CHECK(mapOf(sharpFrames(sg, lines)).Count() == 0);
            }
        {
            std::vector<ser::Frame> clean;
            for (const auto& f : with([](int, int) { return 0.0; })) clean.push_back(f);
            CHECK(mapOf(clean).Count() == 0);
        }
        const stack::HotPixelMap map = mapOf(hotFrames);
        size_t found = 0, extras = 0;
        std::string missed;
        for (const auto& [hx, hy] : hot) {
            const size_t k = static_cast<size_t>(hy) * w + hx;
            if (std::binary_search(map.Sites().begin(), map.Sites().end(), k)) ++found;
            else missed += std::to_string(hx) + "," + std::to_string(hy) + " ";
        }
        extras = map.Count() - found;
        INFO("found " << found << " of " << hot.size() << "; missed " << missed << "; " << extras << " flagged that are not hot");
        CHECK(extras == 0);
        // Mono: every site. Bayer: the same-colour ring is 2 px out, where the texture is rougher (over
        // all 1386 sites: +0.35 found at 1160, +0.5 at 1228, 84-89%) -- most, not all; here 36 of 47.
        if (mono) CHECK(found == hot.size());
        else CHECK(found >= hot.size() * 7 / 10);
        // Repaired to the disc around them, not left 0.2-0.35 above it.
        ser::Frame f = hotFrames[17];
        map.Repair(f);
        const auto& clean = (*base.frames)[17];
        double worst = 0;
        for (const auto& [hx, hy] : hot) {
            const size_t k = static_cast<size_t>(hy) * w + hx;
            if (std::binary_search(map.Sites().begin(), map.Sites().end(), k))
                worst = std::max(worst, std::fabs(f.samples[k] - static_cast<double>(clean[k])) / 65535.0);
        }
        INFO("largest repaired-site error against the frame without hot pixels: " << worst);
        CHECK(worst < 0.03);  // measured 0.025 (mono) / 0.022 (Bayer)
    }
}

TEST_CASE("A raw colour mosaic read as mono is measured binned, stacked raw (Ruling P9)", "[accumulate]") {
    const int w = 224, h = 176;
    const Scene s = testing::MakeSurface(51, w, h);
    // The mosaic: the scene times a colour factor per CFA site (R 0.6, G 1.0, B 0.4 on RGGB).
    auto mosaic = [&](const FrameSpec& f, unsigned seed) {
        std::vector<float> v = testing::RenderFrame(s, w, h, f, seed);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const double k = (y % 2 == 0 && x % 2 == 0) ? 0.6 : (y % 2 == 1 && x % 2 == 1) ? 0.4 : 1.0;
                v[static_cast<size_t>(y) * w + x] = static_cast<float>(v[static_cast<size_t>(y) * w + x] * k);
            }
        return v;
    };
    Capture cap = testing::NewCapture(w, h);
    cap.info.avi = ser::AviDetails{};
    cap.info.avi->bayerSource = "none";
    REQUIRE(stack::UndeclaredMosaic(cap.info));
    CHECK(stack::LumaScale(cap.info) == 2.0);

    // Region quality: a sharp and a blurred frame of the same scene, scored on the raw mosaic and on
    // the binned luma. The fixed pattern swamps the raw score; the binned one sees the blur.
    FrameSpec sharp, blurred;
    sharp.hasLocal = blurred.hasLocal = true;
    sharp.dx = sharp.dy = blurred.dx = blurred.dy = testing::Field{};
    sharp.dx.v = sharp.dy.v = blurred.dx.v = blurred.dy.v = {0.0};
    sharp.sigma.v = {0.5};
    blurred.sigma.v = {2.0};
    sharp.noise = blurred.noise = 0.003;
    Capture two = cap;
    two.frames = std::make_shared<std::vector<std::vector<uint16_t>>>();
    two.Add(mosaic(sharp, 1));
    two.Add(mosaic(blurred, 2));
    auto ratio = [&](bool binned) {
        auto src = two.Factory()();
        const FloatImage a = binned ? stack::MeasurementLuma(src->ReadFrame(0), two.info)
                                    : stack::LumaForMeasure(src->ReadFrame(0), ser::ColorId::Mono, 16);
        const FloatImage b = binned ? stack::MeasurementLuma(src->ReadFrame(1), two.info)
                                    : stack::LumaForMeasure(src->ReadFrame(1), ser::ColorId::Mono, 16);
        const stack::RegionLayout lay = stack::PlaceRegions(a, stack::MeasureFrame(a, 0, binned ? 2.0 : 1.0), true,
                                                            testing::WhiteNoise(binned ? 0.0015 : 0.003));
        const stack::RegionMeasurer m(a, lay);
        const FrameRegions qa = m.Measure(a, 0, Shift{}), qb = m.Measure(b, 1, Shift{});
        std::vector<double> rs;
        for (size_t r = 0; r < lay.regions.size(); ++r)
            if (qa.regions[r].quality > 0 && qb.regions[r].quality > 0) rs.push_back(qa.regions[r].quality / qb.regions[r].quality);
        REQUIRE(!rs.empty());
        std::sort(rs.begin(), rs.end());
        return rs[rs.size() / 2];
    };
    const double raw = ratio(false), binned = ratio(true);
    INFO("median sharp / blurred region quality: raw mosaic " << raw << ", binned " << binned);
    CHECK(raw < 1.15);
    CHECK(binned > 1.3);

    // The whole flow: measured on bins, stacked at full resolution as mono.
    std::mt19937 rng(91);
    for (int k = 0; k < 40; ++k) cap.Add(mosaic(Seeing(rng, w, h, 2.0, 48), 3000u + k));
    const Pipeline p = Run(cap, true);
    CHECK(p.reference.width == w / 2);
    CHECK(p.ours.width == w);
    CHECK(p.ours.height == h);
    CHECK(p.ours.channels == 1);
    REQUIRE(p.plan.RegionCount() > 0);
    int zeros = 0;
    for (float v : p.ours.data) zeros += v == 0.f;
    CHECK(zeros == 0);
}

TEST_CASE("The sky under a region keeps the fallback's noise (Task 9: the 2023-09-29 Saturn patch)", "[accumulate]") {
    // The real case: on the 2023-09-29 Saturn (3686 frames stacked) the region at luma (341, 183) held
    // the tip of the rings in a mostly-sky window; ranked on it, it kept 64 of 3676 candidates (the
    // other regions 644-3686), and over the sky of its window the stack's high-pass noise was 1.65x the
    // sky around it (0.00038 against 0.00023-0.00026): a visible square of speckle with a seam, absent
    // from the global-only stack of the same frames (stack-region-probe: 2.77 against 2.70-3.00 DN).
    // A region's own frame choice buys detail only where there is detail; over sky it only buys noise,
    // sqrt(fallback frames / region frames) of it. Here: a 16 px disc, seeing that varies by cell;
    // the sky under the central half of a region that keeps under half its candidates must carry the
    // noise of the sky no region reaches (every stacked frame), not more.
    const int w = 160, h = 128;
    const double cx = 62.4, cy = 70.3, R = 16;
    const Scene s = testing::MakeDisc(11, cx, cy, R);
    Capture cap = testing::NewCapture(w, h);
    std::mt19937 rng(91);
    RenderCapture(cap, s, 300, [&](int) { return Seeing(rng, w, h, 2.0, 28); });
    const Pipeline p = Run(cap);
    REQUIRE_FALSE(p.sel.fullFrameTarget);
    const auto& lay = p.plan.layout;
    auto sky = [&](int x, int y) { return x >= 8 && y >= 8 && x < w - 8 && y < h - 8 && std::hypot(x - cx, y - cy) > R + 6; };
    std::vector<double> lucky, fallback;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (!sky(x, y)) continue;
            bool inLucky = false, inAny = false;
            for (size_t r = 0; r < lay.regions.size(); ++r) {
                const auto& g = lay.regions[r];
                if (x >= g.x0 && x < g.x1 && y >= g.y0 && y < g.y1) inAny = true;
                const int q = (g.x1 - g.x0) / 4;
                const auto& d = p.plan.detail[r];
                if (d.kept > 0 && 2 * d.kept < d.candidates && x >= g.x0 + q && x < g.x1 - q && y >= g.y0 + q && y < g.y1 - q)
                    inLucky = true;
            }
            const double v = p.ours.data[static_cast<size_t>(y) * w + x];
            if (inLucky) lucky.push_back(v);
            else if (!inAny) fallback.push_back(v);
        }
    REQUIRE(lucky.size() >= 50);     // the case: a region keeping a lucky few covers sky
    REQUIRE(fallback.size() >= 500);
    auto sd = [](const std::vector<double>& v) {
        double m = 0, q = 0;
        for (double x : v) m += x;
        m /= static_cast<double>(v.size());
        for (double x : v) q += (x - m) * (x - m);
        return std::sqrt(q / static_cast<double>(v.size() - 1));
    };
    const double a = sd(lucky), b = sd(fallback);
    INFO("sky noise under lucky regions " << a << " (" << lucky.size() << " px), sky no region reaches " << b << " ("
                                          << fallback.size() << " px), ratio " << a / b << "; stacked " << p.plan.stackedCount);
    CHECK(a < 1.25 * b);
}

TEST_CASE("The sky beside a frame-filling target keeps the fallback's noise (E1 fix round 1: the 2023-08-22 Moon)", "[accumulate]") {
    // The real case: the 2023-08-22 and 2023-03-05 Moons run as frame-filling targets, whose regions cover the
    // whole frame (flat edges), sky included; on the 2023-08-22 one the sky's high-pass RMS reached 1.91x the
    // global-only stack's (27 of 784 sky tiles over 1.15; stack-region-probe at 149178d7). Here: a disc
    // larger than the frame is high, cut by the bottom edge (frame-filling), sky above it; every frame with a
    // target goes to the regions, as for a real lunar capture. The sky past R + 6 under the central half of
    // the regions keeping under half their candidates must carry the noise of the fallback alone -- the same
    // stacked frames on the global shift, the same gains.
    const int w = 224, h = 176;
    const double cx = 112.3, cy = 150.4, R = 110;
    const Scene s = testing::MakeDisc(23, cx, cy, R);
    Capture cap = testing::NewCapture(w, h);
    std::mt19937 rng(57);
    RenderCapture(cap, s, 120, [&](int) { return Seeing(rng, w, h, 2.0, 40); });
    const Pipeline p = Run(cap, true);
    REQUIRE(p.sel.fullFrameTarget);
    REQUIRE(p.plan.layout.flatEdges);
    const FloatImage base = FallbackAlone(cap, p);
    const auto& lay = p.plan.layout;
    // High-pass (pixel minus its 3 x 3 mean) RMS, so the glow-free sky's level does not count.
    auto hp = [&](const FloatImage& im, int x, int y) {
        double m = 0;
        for (int j = -1; j <= 1; ++j)
            for (int i = -1; i <= 1; ++i) m += im.data[static_cast<size_t>(y + j) * w + x + i];
        return im.data[static_cast<size_t>(y) * w + x] - m / 9;
    };
    double a = 0, b = 0;
    int64_t n = 0;
    for (int y = 2; y < h - 2; ++y)
        for (int x = 2; x < w - 2; ++x) {
            if (std::hypot(x - cx, y - cy) <= R + 6 + stack::kMaskGrow + stack::kMaskTaper) continue;
            bool lucky = false;
            for (size_t r = 0; r < lay.regions.size() && !lucky; ++r) {
                const auto& g = lay.regions[r];
                const int q = (g.x1 - g.x0) / 4;
                const auto& d = p.plan.detail[r];
                lucky = d.kept > 0 && 2 * d.kept < d.candidates && x >= g.x0 + q && x < g.x1 - q && y >= g.y0 + q && y < g.y1 - q;
            }
            if (!lucky) continue;
            const double u = hp(p.ours, x, y), v = hp(base, x, y);
            a += u * u;
            b += v * v;
            ++n;
        }
    REQUIRE(n >= 200);  // the case: regions keeping a lucky few over sky
    const double ratio = std::sqrt(a / b);
    INFO("sky high-pass RMS under lucky regions over the fallback alone's: " << ratio << " (" << n << " px); stacked "
                                                                            << p.plan.stackedCount);
    CHECK(ratio < 1.25);
}

TEST_CASE("Where the blend mask is 0 the stack is the fallback alone -- warp included (E1 fix round 1)", "[accumulate]") {
    // Review M1: the mask scales the region weights in the warp as well as in the blend; over sky a region's
    // local shift is noise. LocalWarpAt itself: mask 0 gives no local shift, mask 1 the unmasked one.
    {
        stack::RegionLayout lay;
        lay.width = lay.height = 64;
        lay.size = 32;
        lay.step = 16;
        lay.nx = lay.ny = 1;
        lay.flatEdges = true;
        lay.regions.push_back({0, 0, 32, 32, 0.01, 0, 0});
        lay.node = {0};
        const float dx[1] = {1.5f}, dy[1] = {-0.75f};
        const uint8_t usable[1] = {1};
        const stack::AxisWeight a = stack::AxisWeightAt(lay, false, 10), b = stack::AxisWeightAt(lay, true, 10);
        double ox = 9, oy = 9;
        stack::LocalWarpAt(lay, a, b, 0.0, dx, dy, usable, ox, oy);
        CHECK(ox == 0.0);
        CHECK(oy == 0.0);
        stack::LocalWarpAt(lay, a, b, 1.0, dx, dy, usable, ox, oy);
        CHECK(ox == Catch::Approx(1.5));
        CHECK(oy == Catch::Approx(-0.75));
    }
    // The stack: the red test's disc. Where the mask is 0 every frame enters as in the fallback alone -- the
    // same weight and the global shift -- so the two images agree there to float rounding; with the warp
    // unmasked the sky's resampling at the regions' noisy shifts differed by about the stack's noise.
    const int w = 160, h = 128;
    const double cx = 62.4, cy = 70.3, R = 16;
    const Scene s = testing::MakeDisc(11, cx, cy, R);
    Capture cap = testing::NewCapture(w, h);
    std::mt19937 rng(91);
    RenderCapture(cap, s, 300, [&](int) { return Seeing(rng, w, h, 2.0, 28); });
    const Pipeline p = Run(cap);
    REQUIRE_FALSE(p.plan.layout.mask.Empty());
    const FloatImage base = FallbackAlone(cap, p);
    double worst = 0;
    int64_t n = 0, covered = 0;
    const auto& lay = p.plan.layout;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            // Only where the mask is 0 at every warp-grid point around the pixel (the stack samples it there).
            bool zero = true;
            for (int j = -4; j <= 4 && zero; j += 4)
                for (int i = -4; i <= 4 && zero; i += 4) zero = lay.mask.At(x + i, y + j) == 0.0;
            if (!zero) continue;
            const stack::AxisWeight ax = stack::AxisWeightAt(lay, false, x), ay = stack::AxisWeightAt(lay, true, y);
            covered += (ax.w0 + ax.w1) * (ay.w0 + ay.w1) > 0;  // under some region window
            ++n;
            worst = std::max(worst, static_cast<double>(std::fabs(p.ours.data[static_cast<size_t>(y) * w + x] - base.data[static_cast<size_t>(y) * w + x])));
        }
    INFO(n << " px where the mask is 0, " << covered << " of them under a region window; worst |stack - fallback alone| " << worst);
    REQUIRE(covered >= 500);
    CHECK(worst < 1e-5);
}

TEST_CASE("A blend mask of 1 changes nothing up to the frame's edges (E1 fix round 2)", "[accumulate]") {
    // Review N2: the stack samples the mask at its warp grid, whose points reach past the luma grid -- the first
    // at luma -0.25 on Bayer input, the last up to kWarpGrid px past the bottom and right edges. A mask that
    // read 0 there (the form before 85d5c8df) dimmed those points' region weights: a frame-filling Moon's bottom
    // rows went dark (2023-03-05), and Bayer input's top and left edge points read 0.75 of the edge value.
    stack::BlendMask ones;
    ones.width = 64;
    ones.height = 48;
    ones.m = std::make_shared<std::vector<float>>(64 * 48, 1.f);
    for (double v : {0.0, 23.5, 47.0}) {
        CHECK(ones.At(-3, v) == 1.0);
        CHECK(ones.At(-0.25, v) == 1.0);
        CHECK(ones.At(66.5, v) == 1.0);
    }
    for (double u : {0.0, 31.2, 63.0}) {
        CHECK(ones.At(u, -3) == 1.0);
        CHECK(ones.At(u, -0.25) == 1.0);
        CHECK(ones.At(u, 50.75) == 1.0);
    }
    CHECK(ones.At(-0.25, -0.25) == 1.0);
    // The stack: a frame-filling layout whose regions keep different frames and shift locally; with a mask of 1
    // over the whole luma grid it must be the stack without one, edges included -- Bayer (luma scale 2) and mono.
    for (const ser::ColorId colour : {ser::ColorId::BayerRGGB, ser::ColorId::Mono}) {
        const int W = 128, H = 96, s = colour == ser::ColorId::Mono ? 1 : 2, lw = W / s, lh = H / s;
        const Scene sc = testing::MakeSurface(5, W, H);
        FloatImage ref{lw, lh, 1, std::vector<float>(static_cast<size_t>(lw) * lh)};
        for (int y = 0; y < lh; ++y)
            for (int x = 0; x < lw; ++x) ref.data[static_cast<size_t>(y) * lw + x] = static_cast<float>(sc.Eval(s * x + 0.5 * (s - 1), s * y + 0.5 * (s - 1), 0.8));
        const stack::RegionLayout lay = stack::PlaceRegions(ref, stack::MeasureFrame(ref, 0, s), true, stack::NoiseModel{});
        REQUIRE(lay.flatEdges);
        REQUIRE(lay.mask.Empty());
        REQUIRE(lay.regions.size() > 2);
        std::mt19937 rng(3);
        std::normal_distribution<double> nd(0, 1);
        std::vector<FrameRegions> measured;
        const int frames = 24;
        for (int k = 0; k < frames; ++k) {
            FrameRegions m;
            m.frameIndex = k;
            m.global = Shift{0.3 * nd(rng), 0.3 * nd(rng), 1};
            for (size_t r = 0; r < lay.regions.size(); ++r) {
                stack::RegionMeasure q;
                q.aligned = true;
                q.confidence = 0.95f;
                q.dx = static_cast<float>(0.4 * nd(rng));
                q.dy = static_cast<float>(0.4 * nd(rng));
                q.quality = static_cast<float>(0.3 * std::exp(0.4 * nd(rng)));
                q.qualitySigma = 0.001f;
                m.regions.push_back(q);
            }
            measured.push_back(m);
        }
        const stack::RegionPlan plain = stack::PlanRegions(lay, measured);
        stack::RegionPlan masked = plain;
        masked.layout.mask = ones;
        masked.layout.mask.width = lw;
        masked.layout.mask.height = lh;
        masked.layout.mask.m = std::make_shared<std::vector<float>>(static_cast<size_t>(lw) * lh, 1.f);
        ser::FileInfo info = testing::NewCapture(W, H, colour).info;
        const FloatImage noRejection{lw, lh, 1, std::vector<float>(static_cast<size_t>(lw) * lh, 1.f)};
        stack::Accumulator a(plain, ref, noRejection, info), b(masked, ref, noRejection, info);
        const int C = colour == ser::ColorId::Mono ? 1 : 3;
        for (int k = 0; k < frames; ++k) {
            FloatImage f{W, H, C, std::vector<float>(static_cast<size_t>(W) * H * C)};
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x)
                    for (int c = 0; c < C; ++c)
                        f.data[(static_cast<size_t>(y) * W + x) * C + c] = static_cast<float>(sc.Eval(x, y, 0.8) + 0.01 * nd(rng));
            a.Add(k, f);
            b.Add(k, f);
        }
        const FloatImage pa = a.Finish(), pb = b.Finish();
        double worst = 0;
        int wx = 0, wy = 0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                for (int c = 0; c < C; ++c) {
                    const double d = std::fabs(pa.data[(static_cast<size_t>(y) * W + x) * C + c] - pb.data[(static_cast<size_t>(y) * W + x) * C + c]);
                    if (d > worst) {
                        worst = d;
                        wx = x;
                        wy = y;
                    }
                }
        INFO((C == 1 ? "mono" : "Bayer") << ": worst |masked - unmasked| " << worst << " at " << wx << ", " << wy);
        CHECK(worst < 1e-6);
    }
}
