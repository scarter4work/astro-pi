// Test-only CLI over the region stage (Task 5), for the real-data check. Never shipped, not a ctest
// case. Reads the video only.
//
//   stack-region-probe <video> [--frames N] [--threads T] [--out DIR]
//
// Runs the engine's flow on the first N frames (default: all) on T threads (default: hardware):
// MeasureAll -> SelectFrames -> BuildReference -> pass A (every selected frame: luma, GlobalAligner,
// RegionMeasurer, ResidualScale) -> PlanRegions -> pass B (every planned frame: DemosaicHQ,
// Accumulator::Add) -> Finish, plus the same frames stacked with the global shift only (a layout
// without regions: the fallback stack alone) as the baseline. Prints the selection, the layout, the
// per-region kept counts, rejection, memory and the time of every phase. With --out, writes
// DIR/ours.ppm, DIR/global.ppm, DIR/best-frame.ppm (the sharpest single frame, demosaiced) with one
// shared display stretch (8-bit, black at the 0.1th percentile of ours, white at its 99.95th, gamma
// 1/2.2) for a side-by-side. Exit 0 on success, 2 when the capture is diagnosed, 1 on error.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "Accumulate.h"
#include "Align.h"
#include "Demosaic.h"
#include "Measure.h"
#include "Regions.h"
#include "Select.h"

using namespace planetary;

namespace {

class Truncated : public ser::FrameSource {
public:
    Truncated(std::unique_ptr<ser::FrameSource> s, int64_t n) : s_(std::move(s)), info_(s_->Info()) {
        info_.frameCount = std::min(info_.frameCount, n);
    }
    const ser::FileInfo& Info() const override { return info_; }
    ser::Frame ReadFrame(int64_t i) const override { return s_->ReadFrame(i); }
    std::optional<int64_t> FrameTimestamp(int64_t i) const override { return s_->FrameTimestamp(i); }

private:
    std::unique_ptr<ser::FrameSource> s_;
    ser::FileInfo info_;
};

double Seconds(std::chrono::steady_clock::time_point a) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - a).count();
}

// Runs fn(i) for i in [0, n) on `threads` workers, each with its own reader; contiguous chunks.
template <class Fn>
void ParallelFrames(int64_t n, int threads, const stack::SourceFactory& factory, Fn fn) {
    std::vector<std::thread> pool;
    std::exception_ptr err;
    std::mutex m;
    const int t = static_cast<int>(std::max<int64_t>(1, std::min<int64_t>(threads, n)));
    for (int w = 0; w < t; ++w)
        pool.emplace_back([&, w] {
            try {
                auto src = factory();
                const int64_t a = n * w / t, b = n * (w + 1) / t;
                for (int64_t i = a; i < b; ++i) fn(w, *src, i);
            } catch (...) {
                std::lock_guard<std::mutex> l(m);
                if (!err) err = std::current_exception();
            }
        });
    for (auto& th : pool) th.join();
    if (err) std::rethrow_exception(err);
}

void WritePpm(const std::string& path, const ser::FloatImage& im, double lo, double hi) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) throw std::runtime_error("cannot write " + path);
    std::fprintf(f, "P6\n%d %d\n255\n", im.width, im.height);
    std::vector<unsigned char> row(static_cast<size_t>(im.width) * 3);
    for (int y = 0; y < im.height; ++y) {
        for (int x = 0; x < im.width; ++x)
            for (int c = 0; c < 3; ++c) {
                const int ch = im.channels == 3 ? c : 0;
                const double v = im.data[(static_cast<size_t>(y) * im.width + x) * im.channels + ch];
                const double t = std::clamp((v - lo) / (hi - lo), 0.0, 1.0);
                row[static_cast<size_t>(x) * 3 + c] = static_cast<unsigned char>(std::lround(255 * std::pow(t, 1 / 2.2)));
            }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
}

}  // namespace

int main(int argc, char** argv) try {
    if (argc < 2) {
        std::fprintf(stderr, "usage: stack-region-probe <video> [--frames N] [--threads T] [--out DIR]\n");
        return 1;
    }
    const std::string path = argv[1];
    int64_t limit = INT64_MAX;
    int threads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    std::string out;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--frames" && i + 1 < argc) limit = std::stoll(argv[++i]);
        else if (a == "--threads" && i + 1 < argc) threads = std::max(1, std::atoi(argv[++i]));
        else if (a == "--out" && i + 1 < argc) out = argv[++i];
        else throw std::invalid_argument("unknown argument " + a);
    }
    const stack::SourceFactory factory = [&] {
        return std::unique_ptr<ser::FrameSource>(new Truncated(ser::OpenVideo(path, ser::BayerChoice::Auto), limit));
    };
    const ser::FileInfo info = factory()->Info();
    const double scale = stack::LumaScale(info);
    std::printf("capture  %s\n", path.c_str());
    std::printf("frames   %lld, %dx%d, %s, luma scale %.0f, %d threads\n", static_cast<long long>(info.frameCount),
                info.header.width, info.header.height, ser::IsBayer(info.colorId) ? "Bayer" : "mono/RGB", scale, threads);

    auto t0 = std::chrono::steady_clock::now();
    const std::vector<stack::FrameStats> stats = stack::MeasureAll(factory, info, threads, {});
    const double tMeasure = Seconds(t0);
    t0 = std::chrono::steady_clock::now();
    const stack::Selection sel = stack::SelectFrames(stats, info);
    std::printf("select   %s\n", sel.explanation.c_str());
    if (sel.diagnosis.kind != stack::DiagnosisKind::None) return 2;
    const ser::FloatImage ref = stack::BuildReference(factory, info, sel, stats, threads, {});
    const double tRef = Seconds(t0);
    stack::HotPixelMap hot(info);
    {
        auto src = factory();
        const size_t ns = sel.frames.size(), take = std::min<size_t>(64, ns);
        for (size_t i = 0; i < take; ++i) hot.Observe(src->ReadFrame(sel.frames[i * ns / take]));
        hot.Finish();
    }
    std::printf("hot      %zu hot sites from %lld frames\n", hot.Count(), static_cast<long long>(hot.FramesObserved()));

    t0 = std::chrono::steady_clock::now();
    const stack::GlobalAligner aligner(ref, sel.fullFrameTarget);
    const stack::RegionLayout layout = stack::PlaceRegions(ref, stack::MeasureFrame(ref, 0), sel.fullFrameTarget);
    const stack::RegionMeasurer measurer(ref, layout);
    const int64_t n = static_cast<int64_t>(sel.frames.size());
    std::vector<stack::Shift> shifts(static_cast<size_t>(n));
    std::vector<stack::FrameRegions> measured(static_cast<size_t>(n));
    std::vector<std::unique_ptr<stack::ResidualScale>> residuals;
    for (int w = 0; w < threads; ++w) residuals.push_back(std::make_unique<stack::ResidualScale>(layout, ref));
    std::atomic<int64_t> regionNs{0}, observeNs{0}, readNs{0}, demosaicNs{0}, addNs{0};
    ParallelFrames(n, threads, factory, [&](int w, ser::FrameSource& src, int64_t i) {
        const int64_t f = sel.frames[static_cast<size_t>(i)];
        ser::Frame rawA = src.ReadFrame(f);
        hot.Repair(rawA);
        const ser::FloatImage luma = stack::LumaForMeasure(rawA, info.colorId, info.normDepth);
        shifts[static_cast<size_t>(i)] = aligner.Align(luma, stats[static_cast<size_t>(f)]);
        const auto r0 = std::chrono::steady_clock::now();
        measured[static_cast<size_t>(i)] = measurer.Measure(luma, f, shifts[static_cast<size_t>(i)]);
        regionNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - r0).count();
        const auto o0 = std::chrono::steady_clock::now();
        residuals[static_cast<size_t>(w)]->Observe(luma, measured[static_cast<size_t>(i)]);
        observeNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - o0).count();
    });
    const double cut = stack::AlignmentCut(shifts);
    std::vector<stack::FrameRegions> kept;
    std::vector<stack::FrameRegions> keptGlobal;
    for (int64_t i = 0; i < n; ++i)
        if (shifts[static_cast<size_t>(i)].confidence >= cut) {
            kept.push_back(measured[static_cast<size_t>(i)]);
            stack::FrameRegions g = kept.back();
            g.regions.clear();
            g.gain = 1;
            keptGlobal.push_back(g);
        }
    for (int w = 1; w < threads; ++w) residuals[0]->Merge(*residuals[static_cast<size_t>(w)]);
    const ser::FloatImage sigma = residuals[0]->Sigma(info.normDepth);
    const double tPassA = Seconds(t0);

    t0 = std::chrono::steady_clock::now();
    const stack::RegionPlan plan = stack::PlanRegions(layout, kept, threads);
    stack::RegionLayout none = layout;
    none.regions.clear();
    std::fill(none.node.begin(), none.node.end(), -1);
    const stack::RegionPlan global = stack::PlanRegions(none, keptGlobal);
    const double tPlan = Seconds(t0);
    std::vector<int64_t> keptCounts;
    for (const auto& d : plan.detail) keptCounts.push_back(d.kept);
    std::sort(keptCounts.begin(), keptCounts.end());
    std::printf("regions  %zu of %dx%d nodes, size %d luma px%s; aligned frames %zu (cut %.3f)\n", layout.regions.size(),
                layout.nx, layout.ny, layout.size, layout.flatEdges ? ", full frame" : ", disc", kept.size(), cut);
    if (!keptCounts.empty())
        std::printf("kept     per region min %lld / median %lld / max %lld of %zu; best region q %.3f (+- %.3f), detail scale %.2f luma px\n",
                    static_cast<long long>(keptCounts.front()), static_cast<long long>(keptCounts[keptCounts.size() / 2]),
                    static_cast<long long>(keptCounts.back()), kept.size(), plan.detail[static_cast<size_t>(plan.bestRegion)].bestQuality,
                    plan.detail[static_cast<size_t>(plan.bestRegion)].keptQualitySigma,
                    plan.detail[static_cast<size_t>(plan.bestRegion)].detailScale);

    // Per region: the quality distribution over the aligned frames and what was kept.
    for (size_t r = 0; r < plan.RegionCount(); ++r) {
        std::vector<double> q, s;
        for (const auto& m : kept)
            if (m.regions[r].aligned && m.regions[r].quality > 0) {
                q.push_back(m.regions[r].quality);
                s.push_back(m.regions[r].qualitySigma);
            }
        int al = 0;
        std::vector<double> cf;
        for (const auto& m : kept)
            if (m.regions[r].aligned) {
                ++al;
                cf.push_back(m.regions[r].confidence);
            }
        std::sort(cf.begin(), cf.end());
        std::printf("align    %3zu: aligned %d, confidence median %.3f, cut %.3f, candidates %lld, with detail %zu\n", r, al,
                    cf.empty() ? 0.0 : cf[cf.size() / 2], plan.detail[r].confidenceCut, static_cast<long long>(plan.detail[r].candidates), q.size());
        if (q.empty()) continue;
        std::sort(q.begin(), q.end());
        std::sort(s.begin(), s.end());
        const auto& d = plan.detail[r];
        const auto& reg = layout.regions[r];
        std::printf("region   %3zu at %4d,%4d: q p10 %.3f p50 %.3f p90 %.3f max %.3f, sigma %.5f; best %.3f, kept %lld of %lld\n", r, reg.x0,
                    reg.y0, q[q.size() / 10], q[q.size() / 2], q[q.size() * 9 / 10], q.back(), s[s.size() / 2], d.bestQuality,
                    static_cast<long long>(d.kept), static_cast<long long>(d.candidates));
    }
    t0 = std::chrono::steady_clock::now();
    stack::Accumulator acc(plan, ref, sigma, info), accGlobal(global, ref, sigma, info);
    ParallelFrames(static_cast<int64_t>(plan.frames.size()), threads, factory, [&](int, ser::FrameSource& src, int64_t i) {
        const int64_t f = plan.frames[static_cast<size_t>(i)];
        const auto r0 = std::chrono::steady_clock::now();
        ser::Frame raw = src.ReadFrame(f);
        hot.Repair(raw);
        const auto r1 = std::chrono::steady_clock::now();
        ser::FloatImage im = stack::DemosaicHQ(raw, info.colorId, info.normDepth);
        const auto r2 = std::chrono::steady_clock::now();
        accGlobal.Add(f, im);
        const auto r3 = std::chrono::steady_clock::now();
        acc.Add(f, std::move(im));
        const auto r4 = std::chrono::steady_clock::now();
        auto ns = [](auto a, auto b) { return std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count(); };
        readNs += ns(r0, r1);
        demosaicNs += ns(r1, r2);
        addNs += ns(r3, r4);
    });
    const ser::FloatImage ours = acc.Finish(), base = accGlobal.Finish();
    const double tPassB = Seconds(t0);
    std::printf("stack    rejected cells %lld (%.4f%% of frame cells), held %.1f MB\n",
                static_cast<long long>(acc.RejectedCells()),
                100.0 * acc.RejectedCells() / (static_cast<double>(plan.frames.size()) * (info.header.width / scale) * (info.header.height / scale)),
                acc.BytesHeld() / 1e6);
    std::printf("pass B   per frame and thread: read %.1f ms, demosaic %.1f ms, Add (regions) %.1f ms\n", readNs.load() / 1e6 / plan.frames.size(), demosaicNs.load() / 1e6 / plan.frames.size(), addNs.load() / 1e6 / plan.frames.size());
    std::printf("time     measure %.2f s, select+reference %.2f s, pass A %.2f s (regions %.1f, residual %.1f ms/frame/thread), plan %.2f s, "
                "pass B (both stacks) %.2f s\n",
                tMeasure, tRef, tPassA, regionNs.load() / 1e6 / std::max<int64_t>(1, n), observeNs.load() / 1e6 / std::max<int64_t>(1, n), tPlan, tPassB);

    if (!out.empty()) {
        std::vector<float> v(ours.data.begin(), ours.data.end());
        std::sort(v.begin(), v.end());
        const double lo = v[static_cast<size_t>(0.001 * (v.size() - 1))], hi = v[static_cast<size_t>(0.9995 * (v.size() - 1))];
        WritePpm(out + "/ours.ppm", ours, lo, hi);
        WritePpm(out + "/global.ppm", base, lo, hi);
        int64_t best = sel.frames.front();
        for (int64_t f : sel.frames)
            if (stats[static_cast<size_t>(f)].quality > stats[static_cast<size_t>(best)].quality) best = f;
        WritePpm(out + "/best-frame.ppm", stack::DemosaicHQ(factory()->ReadFrame(best), info.colorId, info.normDepth), lo, hi);
        std::printf("wrote    %s/{ours,global,best-frame}.ppm (best frame %lld)\n", out.c_str(), static_cast<long long>(best));
    }
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
}
