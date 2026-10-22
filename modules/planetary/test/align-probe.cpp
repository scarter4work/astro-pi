// Test-only CLI over the stack engine's global alignment, for the real-data check
// (test/run-align-real.sh). Never shipped, not a ctest case. Reads the video only.
//
//   align-probe <video> [--frames N] [--threads T] [--split]
//
// Runs MeasureAll -> SelectFrames -> BuildReference over the first N frames (default: all), then
// aligns every selected frame to the reference on T threads (default: hardware threads) and prints
// one table row:
//   capture | luma WxH | mode window | frames | dx range | dy range | conf min / p1 / median | cut | below
// plus, for a disc, |centroid offset - shift| (median / p99), and the timings. --split (full-frame
// targets) also aligns each frame's top and bottom halves separately against the reference's and
// lists the frames whose halves disagree by more than 1 px or that fall below the cut -- a torn frame
// shows both. Exit 0 on success, 2 when the capture is diagnosed (no frames to align), 1 on error.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "Align.h"
#include "Demosaic.h"
#include "Measure.h"
#include "Select.h"

using namespace planetary;

namespace {

// The first `n` frames of a source.
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

double Quantile(std::vector<double> v, double p) {
    std::sort(v.begin(), v.end());
    return v[static_cast<size_t>(p * static_cast<double>(v.size() - 1))];
}

ser::FloatImage Rows(const ser::FloatImage& im, int y0, int y1) {
    ser::FloatImage o{im.width, y1 - y0, 1, {}};
    o.data.assign(im.data.begin() + static_cast<std::ptrdiff_t>(y0) * im.width,
                  im.data.begin() + static_cast<std::ptrdiff_t>(y1) * im.width);
    return o;
}

}  // namespace

int main(int argc, char** argv) try {
    if (argc < 2) {
        std::fprintf(stderr, "usage: align-probe <video> [--frames N] [--threads T] [--split]\n");
        return 1;
    }
    const std::string path = argv[1];
    int64_t limit = INT64_MAX;
    int threads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    bool split = false;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--frames" && i + 1 < argc) limit = std::stoll(argv[++i]);
        else if (a == "--threads" && i + 1 < argc) threads = std::max(1, std::atoi(argv[++i]));
        else if (a == "--split") split = true;
        else throw std::invalid_argument("unknown argument " + a);
    }
    using Clock = std::chrono::steady_clock;
    auto secs = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double>(b - a).count(); };
    const stack::SourceFactory factory = [&] {
        return std::make_unique<Truncated>(ser::OpenVideo(path, ser::BayerChoice::Auto), limit);
    };
    const ser::FileInfo info = factory()->Info();
    const std::string name = path.substr(path.find_last_of('/') + 1);

    const auto t0 = Clock::now();
    const std::vector<stack::FrameStats> stats = stack::MeasureAll(factory, info, threads, {});
    const stack::Selection sel = stack::SelectFrames(stats, info);
    const auto t1 = Clock::now();
    if (sel.frames.empty()) {
        std::printf("%s | diagnosed: %s\n", name.c_str(), sel.explanation.c_str());
        return 2;
    }
    const ser::FloatImage ref = stack::BuildReference(factory, info, sel, stats, threads, {});
    const auto t2 = Clock::now();
    const stack::GlobalAligner aligner(ref, sel.fullFrameTarget);

    std::vector<stack::Shift> shifts(sel.frames.size());
    std::vector<double> cpu(static_cast<size_t>(threads), 0.0);
    std::atomic<size_t> next{0};
    std::exception_ptr error;
    std::atomic<bool> failed{false};
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t)
        pool.emplace_back([&, t] {
            try {
                const auto src = factory();
                for (size_t k; !failed && (k = next.fetch_add(1)) < sel.frames.size();) {
                    const int64_t i = sel.frames[k];
                    const ser::FloatImage luma = stack::LumaForMeasure(src->ReadFrame(i), info.colorId, info.normDepth);
                    const auto a = Clock::now();
                    shifts[k] = aligner.Align(luma, stats[static_cast<size_t>(i)]);
                    cpu[static_cast<size_t>(t)] += std::chrono::duration<double>(Clock::now() - a).count();
                }
            } catch (...) {
                if (!failed.exchange(true)) error = std::current_exception();
            }
        });
    for (std::thread& t : pool) t.join();
    if (error) std::rethrow_exception(error);
    const auto t3 = Clock::now();

    std::vector<double> conf, dx, dy;
    for (const stack::Shift& s : shifts) {
        conf.push_back(s.confidence);
        dx.push_back(s.dx);
        dy.push_back(s.dy);
    }
    const double cut = stack::AlignmentCut(shifts);
    int below = 0;
    for (double c : conf) below += c < cut ? 1 : 0;
    double cpuSum = 0;
    for (double c : cpu) cpuSum += c;
    std::printf("%s | %dx%d %s | %s %dx%d | %zu of %lld | dx %.2f..%.2f | dy %.2f..%.2f | %.3f / %.3f / %.3f | %.3f | %d\n",
                name.c_str(), ref.width, ref.height, ser::ColorIdName(info.colorId),
                sel.fullFrameTarget ? "full" : "disc", aligner.WindowWidth(), aligner.WindowHeight(), sel.frames.size(),
                static_cast<long long>(info.frameCount), Quantile(dx, 0), Quantile(dx, 1), Quantile(dy, 0), Quantile(dy, 1),
                Quantile(conf, 0), Quantile(conf, 0.01), Quantile(conf, 0.5), cut, below);
    if (!sel.fullFrameTarget) {
        const stack::FrameStats rs = stack::MeasureFrame(ref, 0);
        std::vector<double> d;
        for (size_t k = 0; k < shifts.size(); ++k) {
            const stack::FrameStats& s = stats[static_cast<size_t>(sel.frames[k])];
            d.push_back(std::hypot(s.cx - rs.cx - shifts[k].dx, s.cy - rs.cy - shifts[k].dy));
        }
        std::printf("    |centroid offset - shift|: median %.3f, p99 %.3f px\n", Quantile(d, 0.5), Quantile(d, 0.99));
    }
    std::printf("    measure+select %.2f s, reference %.2f s, align %.2f s wall (%d threads, %.1f ms/frame in Align)\n",
                secs(t0, t1), secs(t1, t2), secs(t2, t3), threads, 1000 * cpuSum / static_cast<double>(shifts.size()));

    if (split && sel.fullFrameTarget) {
        const int h2 = ref.height / 2;
        const stack::GlobalAligner top(Rows(ref, 0, h2), true), bottom(Rows(ref, h2, 2 * h2), true);
        const auto src = factory();
        std::vector<double> dist;
        int torn = 0, both = 0;
        for (size_t k = 0; k < shifts.size(); ++k) {
            const int64_t i = sel.frames[k];
            const ser::FloatImage luma = stack::LumaForMeasure(src->ReadFrame(i), info.colorId, info.normDepth);
            const stack::Shift a = top.Align(Rows(luma, 0, h2), stats[static_cast<size_t>(i)]);
            const stack::Shift b = bottom.Align(Rows(luma, h2, 2 * h2), stats[static_cast<size_t>(i)]);
            const double d = std::hypot(a.dx - b.dx, a.dy - b.dy);
            dist.push_back(d);
            torn += d > 1 ? 1 : 0;
            both += d > 1 && shifts[k].confidence < cut ? 1 : 0;
            if (d > 2 || shifts[k].confidence < cut)
                std::printf("    frame %lld: halves differ %.2f px, confidence %.3f%s\n", static_cast<long long>(i), d,
                            shifts[k].confidence, shifts[k].confidence < cut ? " (below the cut)" : "");
        }
        std::printf("    halves: median %.3f px, p99 %.3f px; %d frames differ > 1 px, %d of them below the cut\n",
                    Quantile(dist, 0.5), Quantile(dist, 0.99), torn, both);
    }
    return 0;
} catch (const std::exception& e) {
    std::printf("error: %s\n", e.what());
    return 1;
}
