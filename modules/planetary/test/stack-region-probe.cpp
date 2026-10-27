// Test-only CLI over the region stage (Task 5), for the real-data check. Never shipped, not a ctest
// case. Reads the video only.
//
//   stack-region-probe <video> [--frames N] [--threads T] [--out DIR] [--bayer auto|none|RGGB|GRBG|GBRG|BGGR]
//                      [--keff-box Y0,Y1,X0,X1 [--keff-origin TOP,LEFT] --json-out FILE] [--plan-only] [--fits-out DIR]
//                      [--mask-out FILE]
//
// Runs the engine's flow on the first N frames (default: all) on T threads (default: hardware):
// FitNoiseModel -> MeasureAll -> SelectFrames -> BuildReference -> pass A (every selected frame: luma, GlobalAligner,
// RegionMeasurer, ResidualScale) -> PlanRegions -> pass B (every planned frame: DemosaicHQ,
// Accumulator::Add) -> Finish, plus the same frames stacked with the global shift only (a layout
// without regions: the fallback stack alone) as the baseline. Prints the selection, the layout, the
// per-region kept counts, rejection, memory and the time of every phase. With --out, writes
// DIR/ours.ppm, DIR/global.ppm, DIR/best-frame.ppm (the sharpest single frame, demosaiced) with one
// shared display stretch (8-bit, black at the 0.1th percentile of ours, white at its 99.95th, gamma
// 1/2.2) for a side-by-side. Exit 0 on success, 2 when the capture is diagnosed, 1 on error.
//
// --keff-box (Ruling P28, the bench's END noise model): our stack's effective frame count over a box
// of the output (full-resolution) grid, from the plan's own per-frame kept flags and the blend
// weights the Accumulator uses (AxisWeightAt times the layout's blend mask m, RegionLayout::mask; w_r(p) below
// includes m; alpha_f(p) = sum over the regions keeping f of
// w_r(p) / K_r + (1 - cover(p)) / K_fallback). Per pixel the stack's noise power is
// sigma^2 sum_f alpha_f^2 / (sum_f alpha_f)^2; over the box it is averaged with the metric's Hann
// window squared, and k_eff = 1 / that average (in units of sigma^2). Frame gains, rejected samples
// and the outside-frame weight are left out. --json-out writes it with the plan's per-region kept
// counts (the bench checks them against stack-probe's, the engine's own run). This probe does not crop
// (Engine.h "The crop"): its grid is the reference grid, while RunStack's stack -- stack-probe's --out,
// where the bench takes its boxes -- is that grid cropped. --keff-origin TOP,LEFT (Ruling R14; default
// 0,0) says the box is on such a cropped grid, whose pixel (0, 0) is this grid's (TOP, LEFT) -- stack-probe's
// JSON "crop" top and left: the box is read here at (Y + TOP, X + LEFT). The JSON then records "box" as
// given and "origin" [TOP, LEFT], so the bench can check both against the stack it measures. A box reaching
// so far into the frames' guard band or past their edge that BoxKeff (which counts every kept frame everywhere) could
// over-count its k by more than 1e-4 is refused with the reason (exit 1; Ruling R15, BoxReachError). --plan-only stops
// after the plan (no pass B). --bayer as stack-probe's (default Auto: the sidecar or the header).
// The JSON also carries keffBoxGlobal, the same over the global-only plan (every planned frame, equal
// weight). --fits-out DIR writes DIR/ours.fits (the region stack) and DIR/global.fits (the global-only
// stack of the same frames), 32-bit float as stack-probe's --out: for the bench's split-half triage
// (P28 fix round 3, review I-E: is a half-to-half excess the region stage's or the frames'?). --mask-out FILE
// writes the blend mask on the luma grid (1 everywhere when the layout has none) to FILE and the reference
// to FILE.ref.fits (E1: where the regions blend). The "mask" line prints its extent and how much of it is 1; the
// "masksky" line what it was judged on (BlendMask::skyLevel, targetLevel, shareUnder) and the sky-only regions.
#include <algorithm>
#include <array>
#include <map>
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
#include "ProbeFits.h"

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

ser::BayerChoice ParseBayer(const std::string& s) {
    if (s == "auto" || s == "Auto") return ser::BayerChoice::Auto;
    if (s == "none" || s == "None") return ser::BayerChoice::None;
    if (s == "RGGB") return ser::BayerChoice::RGGB;
    if (s == "GRBG") return ser::BayerChoice::GRBG;
    if (s == "GBRG") return ser::BayerChoice::GBRG;
    if (s == "BGGR") return ser::BayerChoice::BGGR;
    throw std::invalid_argument("unknown --bayer " + s);
}

double Hann(int i, int n) { return n > 1 ? 0.5 - 0.5 * std::cos(2 * M_PI * i / (n - 1)) : 1.0; }

// Ruling R15, BoxKeff's precondition. BoxKeff counts every kept frame at every pixel of the box; a frame whose sample
// there falls off the frame or in its guard band (Accumulate.h "The frame's border") adds nothing to that pixel, so near
// the edge of the frames' reach BoxKeff's k is too high. This bounds by how much, and refuses the box when the bound
// passes kMaxBoxOverCount.
//  - Reach. The Accumulator samples frame f at x + s (global.dx + the local warp there) -- LocalWarpAt, the blend of the
//    frame's usable regions' shifts around that point -- and counts the sample outside below `guard` or above
//    W - 1 - guard (Accumulate.cpp). The warp is smooth (sin^2 blends over regions of >= 32 luma px), so each frame's
//    offset is taken, exactly as the Accumulator computes it, on the box's border (left / right columns, top / bottom
//    rows), its extreme over the border applied to the columns / rows next to it; 0.05 px covers the Accumulator's warp
//    grid (interpolation error under 0.02 px).
//  - Bound. A frame missing from a pixel changes that pixel's noise-power count by at most 1 / K (K the smallest count
//    any region, or the fallback, stacks: its share of the blend there is at most 1 / K). BoxKeff averages over the box
//    with Hann^2 weights, separable in x and y; a frame that misses c columns at one side misses at most the Hann^2 share
//    of those columns (summed over the whole box height). So k is over-counted by at most sum_f share_f / K, relative.
//  - Why not "guard band + largest drift from the edge": the box's outermost rows and columns carry Hann^2 weight 0 and
//    the next ones ~1e-8 of the centre's, so such a rule refuses boxes whose omission is negligible -- the held-out
//    lunar engine test's box, 6 px inside the crop, where one frame warped ~9 px misses one column by 0.06 px (bound
//    ~1e-10).
// Empty when the bound is at most kMaxBoxOverCount, else the reason with the numbers.
constexpr double kMaxBoxOverCount = 1e-4;  // the calibration's own reproduction bound on R (test_end), which k enters linearly
std::string BoxReachError(const stack::RegionPlan& plan, int y0, int y1, int x0, int x1, double s, int width, int height,
                          int guard) {
    const stack::RegionLayout& lay = plan.layout;
    const size_t nr = plan.RegionCount();
    const double slack = 0.05;
    const int bw = x1 - x0, bh = y1 - y0;
    // Hann^2 shares of each column / row of the box (BoxKeff's weights), and their cumulative sums from each side.
    std::vector<double> cx(static_cast<size_t>(bw) + 1, 0.0), cy(static_cast<size_t>(bh) + 1, 0.0);
    for (int i = 0; i < bw; ++i) cx[static_cast<size_t>(i) + 1] = cx[static_cast<size_t>(i)] + std::pow(Hann(i, bw), 2);
    for (int i = 0; i < bh; ++i) cy[static_cast<size_t>(i) + 1] = cy[static_cast<size_t>(i)] + std::pow(Hann(i, bh), 2);
    auto share = [](const std::vector<double>& c, int fromLow, int fromHigh) {  // columns [0, fromLow) and the last fromHigh
        const int n = static_cast<int>(c.size()) - 1;
        fromLow = std::clamp(fromLow, 0, n);
        fromHigh = std::clamp(fromHigh, 0, n - fromLow);
        return c.back() > 0 ? (c[static_cast<size_t>(fromLow)] + c.back() - c[static_cast<size_t>(n - fromHigh)]) / c.back() : 1.0;
    };
    int64_t kMin = plan.stackedCount;
    for (const auto& d : plan.detail)
        if (d.kept > 0) kMin = std::min<int64_t>(kMin, d.kept);
    std::vector<stack::AxisWeight> col(static_cast<size_t>(width)), row(static_cast<size_t>(height));
    for (int x = x0; x < x1; ++x) col[static_cast<size_t>(x)] = stack::AxisWeightAt(lay, false, (x + 0.5) / s - 0.5);
    for (int y = y0; y < y1; ++y) row[static_cast<size_t>(y)] = stack::AxisWeightAt(lay, true, (y + 0.5) / s - 0.5);
    std::vector<float> ldx(nr), ldy(nr);
    std::vector<uint8_t> usable(nr);
    double total = 0, worst = 0;  // sum over frames of the missed share; the largest one frame misses
    int64_t missing = 0;          // frames that miss some of the box
    double loX = 1e300, hiX = -1e300, loY = 1e300, hiY = -1e300;
    for (size_t p = 0; p < plan.frames.size(); ++p) {
        if (!plan.stacked[p]) continue;
        for (size_t r = 0; r < nr; ++r) {
            const size_t k = p * nr + r;
            ldx[r] = plan.dx[k];
            ldy[r] = plan.dy[k];
            usable[r] = plan.flags[k] & stack::RegionPlan::kShiftUsable ? 1 : 0;
        }
        auto offset = [&](int x, int y, double& ox, double& oy) {  // the sample position minus (x, y)
            double lx = 0, ly = 0;
            if (nr > 0)
                stack::LocalWarpAt(lay, col[static_cast<size_t>(x)], row[static_cast<size_t>(y)],
                                   lay.mask.At((x + 0.5) / s - 0.5, (y + 0.5) / s - 0.5), ldx.data(), ldy.data(),
                                   usable.data(), lx, ly);
            ox = s * (plan.global[p].dx + lx);
            oy = s * (plan.global[p].dy + ly);
        };
        double lo = 1e300, hi = -1e300, top = 1e300, bot = -1e300, ox, oy;
        for (int y = y0; y < y1; ++y) {
            offset(x0, y, ox, oy);
            lo = std::min(lo, ox);
            offset(x1 - 1, y, ox, oy);
            hi = std::max(hi, ox);
        }
        for (int x = x0; x < x1; ++x) {
            offset(x, y0, ox, oy);
            top = std::min(top, oy);
            offset(x, y1 - 1, ox, oy);
            bot = std::max(bot, oy);
        }
        loX = std::min(loX, x0 + lo);
        hiX = std::max(hiX, x1 - 1 + hi);
        loY = std::min(loY, y0 + top);
        hiY = std::max(hiY, y1 - 1 + bot);
        // Columns x with x + lo - slack < guard (left) or x + hi + slack > W - 1 - guard (right) are missed; rows likewise.
        auto missedLow = [&](int a0, double off) { return static_cast<int>(std::ceil(guard - off + slack - a0)); };
        auto missedHigh = [&](int a1, double off, int extent) {
            return static_cast<int>(std::ceil(a1 - 1 + off + slack - (extent - 1 - guard)));
        };
        const double f = share(cx, missedLow(x0, lo), missedHigh(x1, hi, width)) +
                         share(cy, missedLow(y0, top), missedHigh(y1, bot, height));
        if (f > 0) ++missing;
        total += f;
        worst = std::max(worst, f);
    }
    const double bound = kMin > 0 ? total / static_cast<double>(kMin) : 1.0;
    if (bound > kMaxBoxOverCount) {
        char why[640];
        std::snprintf(why, sizeof why,
                      "the box y %d-%d, x %d-%d (uncropped grid) reaches into the frames' guard band (%d px) or past their "
                      "edge: %lld stacked frames miss part of it (their samples run x %.2f..%.2f, y %.2f..%.2f against the "
                      "stacked part x %d..%d, y %d..%d), so BoxKeff, which counts every kept frame everywhere, may "
                      "over-count k by up to %.2e relative (over %.0e; the largest one-frame share missed %.2e, smallest "
                      "kept count %lld; Ruling R15)",
                      y0, y1, x0, x1, guard, static_cast<long long>(missing), loX, hiX, loY, hiY, guard, width - 1 - guard,
                      guard, height - 1 - guard, bound, kMaxBoxOverCount, worst, static_cast<long long>(kMin));
        return why;
    }
    return "";
}

// See --keff-box in the header. The layout's blend mask (RegionLayout::mask; empty for a frame-filling layout)
// scales every region weight, as in the Accumulator -- the fallback takes 1 - cover where it is under 1.
// The model's limit (Ruling R15): it counts every kept frame at every pixel of the box. A frame whose sample there
// falls off the frame or in its guard band (Accumulate.h "The frame's border") adds nothing to that pixel, so near the
// edge of the frames' reach the real k is lower than this one. BoxReachError bounds that and refuses the box first.
double BoxKeff(const stack::RegionPlan& plan, int y0, int y1, int x0, int x1, double s) {
    const stack::BlendMask& mask = plan.layout.mask;
    const stack::RegionLayout& lay = plan.layout;
    const size_t nr = plan.RegionCount();
    std::vector<size_t> stacked;
    for (size_t p = 0; p < plan.frames.size(); ++p)
        if (plan.stacked[p]) stacked.push_back(p);
    if (stacked.empty() || plan.stackedCount <= 0) throw std::runtime_error("plan stacks no frame");
    const double fb = 1.0 / static_cast<double>(plan.stackedCount);
    std::map<std::array<int, 4>, std::array<int64_t, 16>> cache;
    double num = 0, den = 0;
    const int h = y1 - y0, w = x1 - x0;
    for (int y = y0; y < y1; ++y) {
        const stack::AxisWeight ay = stack::AxisWeightAt(lay, true, (y + 0.5) / s - 0.5);
        const double hy = Hann(y - y0, h);
        for (int x = x0; x < x1; ++x) {
            const stack::AxisWeight ax = stack::AxisWeightAt(lay, false, (x + 0.5) / s - 0.5);
            const double mk = mask.At((x + 0.5) / s - 0.5, (y + 0.5) / s - 0.5);
            std::array<int, 4> ids{-1, -1, -1, -1};
            std::array<double, 4> wts{0, 0, 0, 0};
            int m = 0;
            double cover = 0;
            for (int b = 0; b < 2; ++b) {
                const int j = ay.i0 + b;
                const double wy = b ? ay.w1 : ay.w0;
                if (j < 0 || j >= lay.ny || wy == 0) continue;
                for (int c = 0; c < 2; ++c) {
                    const int i = ax.i0 + c;
                    const double wx = c ? ax.w1 : ax.w0;
                    if (i < 0 || i >= lay.nx || wx == 0) continue;
                    const int r = lay.node[static_cast<size_t>(j) * lay.nx + i];
                    if (r < 0 || plan.detail[static_cast<size_t>(r)].kept <= 0) continue;
                    ids[static_cast<size_t>(m)] = r;
                    wts[static_cast<size_t>(m)] = wx * wy * mk;
                    cover += wx * wy * mk;
                    ++m;
                }
            }
            auto it = cache.find(ids);
            if (it == cache.end()) {
                std::array<int64_t, 16> counts{};
                for (size_t p : stacked) {
                    int bits = 0;
                    for (int k = 0; k < m; ++k)
                        if (plan.flags[p * nr + static_cast<size_t>(ids[static_cast<size_t>(k)])] & stack::RegionPlan::kKept) bits |= 1 << k;
                    ++counts[static_cast<size_t>(bits)];
                }
                it = cache.emplace(ids, counts).first;
            }
            const double fbw = std::max(0.0, 1 - cover) * fb;
            double s1 = 0, s2 = 0;
            for (int bits = 0; bits < (1 << m); ++bits) {
                const int64_t n = it->second[static_cast<size_t>(bits)];
                if (!n) continue;
                double a = fbw;
                for (int k = 0; k < m; ++k)
                    if (bits & (1 << k))
                        a += wts[static_cast<size_t>(k)] / static_cast<double>(plan.detail[static_cast<size_t>(ids[static_cast<size_t>(k)])].kept);
                s1 += static_cast<double>(n) * a;
                s2 += static_cast<double>(n) * a * a;
            }
            if (!(s1 > 0)) continue;
            const double hh = std::pow(hy * Hann(x - x0, w), 2);
            num += hh * s2 / (s1 * s1);
            den += hh;
        }
    }
    if (!(num > 0)) throw std::runtime_error("the box holds no stacked pixel");
    return den / num;
}

// --keff-selftest: BoxKeff against analytic layouts (review M2 / N-m6). Returns the number of failures.
int KeffSelfTest() {
    auto plan = [](int nx, int keptLeft, int keptRight, int n) {
        stack::RegionPlan p;
        stack::RegionLayout& L = p.layout;
        L.width = 64 * nx;
        L.height = 64;
        L.size = 32;
        L.step = 16;
        L.nx = nx;
        L.ny = 1;
        L.flatEdges = true;
        for (int i = 0; i < nx; ++i) {
            stack::Region r;
            r.x0 = i * 16;
            r.x1 = r.x0 + 32;
            r.y1 = 32;
            r.gx = i;
            L.regions.push_back(r);
            L.node.push_back(i);
        }
        for (int f = 0; f < n; ++f) {
            p.frames.push_back(f);
            p.stacked.push_back(1);
        }
        p.stackedCount = n;
        p.flags.assign(static_cast<size_t>(n) * nx, 0);
        p.detail.resize(static_cast<size_t>(nx));
        const int kept[2] = {keptLeft, keptRight};
        for (int i = 0; i < nx; ++i) {
            p.detail[static_cast<size_t>(i)].kept = kept[i];
            for (int f = 0; f < kept[i]; ++f) p.flags[static_cast<size_t>(f) * nx + i] = stack::RegionPlan::kKept;
        }
        return p;
    };
    int fails = 0;
    auto expect = [&](const char* what, double got, double want) {
        const bool ok = std::abs(got - want) <= 1e-6 * want;
        std::printf("selftest %-48s k_eff %.6f, expected %.6f: %s\n", what, got, want, ok ? "ok" : "FAIL");
        fails += ok ? 0 : 1;
    };
    // 1. One node, flat edges: weight 1 everywhere; the region keeps 37 of 100 -> k_eff = 37.
    expect("one region keeping 37 of 100", BoxKeff(plan(1, 37, 0, 100), 0, 64, 0, 64, 1.0), 37.0);
    // 2. The region keeps no frame: the fallback (every stacked frame) everywhere -> 100.
    expect("a region keeping none (the fallback)", BoxKeff(plan(1, 0, 0, 100), 0, 64, 0, 64, 1.0), 100.0);
    // 3. Two nodes blended (sin^2): left keeps frames 0..39, right all 100 (nested). The weights are
    //    written out from the layout's geometry, not from AxisWeightAt (review N2-m8): step 16, window 32,
    //    flat edges, so for pixel centre t = x + 0.5 the right node's weight is 0 below t = 16,
    //    sin^2(pi (t - 16) / 32) over 16..32 and 1 above, the left's its complement. (This restates the
    //    engine's documented blend rule; it checks BoxKeff's sums over that rule, not the rule itself.)
    //    Per pixel frames 0..39 carry wl / 40 + wr / 100, the other 60 wr / 100; noise power
    //    40 a^2 + 60 b^2 (the weights sum to 1). Averaged with Hann^2 over the box.
    {
        const stack::RegionPlan p3 = plan(2, 40, 100, 100);
        const int y0 = 0, y1 = 32, x0 = 0, x1 = 48;     // the left node, the blend, the start of the right
        double num = 0, den = 0;
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                const double t = x + 0.5;
                const double sr = std::sin(M_PI * (t - 16) / 32);
                const double wr = t < 16 ? 0.0 : t < 32 ? sr * sr : 1.0, wl = 1 - wr;
                const double a = wl / 40 + wr / 100, b = wr / 100;
                const double hh = std::pow(Hann(y - y0, y1 - y0) * Hann(x - x0, x1 - x0), 2);
                num += hh * (40 * a * a + 60 * b * b);
                den += hh;
            }
        expect("two blended regions, nested 40 / 100", BoxKeff(p3, y0, y1, x0, x1, 1.0), den / num);
    }
    return fails;
}

}  // namespace

int main(int argc, char** argv) try {
    if (argc >= 2 && std::string(argv[1]) == "--keff-selftest") return KeffSelfTest() ? 1 : 0;
    if (argc < 2) {
        std::fprintf(stderr, "usage: stack-region-probe <video> [--frames N] [--threads T] [--out DIR]\n");
        return 1;
    }
    const std::string path = argv[1];
    int64_t limit = INT64_MAX;
    int threads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    std::string out, jsonOut, fitsOut, maskOut;
    ser::BayerChoice bayer = ser::BayerChoice::Auto;
    bool planOnly = false;
    std::optional<std::array<int, 4>> keffBox;
    std::array<int, 2> keffOrigin{0, 0};  // --keff-origin TOP,LEFT: the box's grid is this one cropped there
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--bayer" && i + 1 < argc) bayer = ParseBayer(argv[++i]);
        else if (a == "--plan-only") planOnly = true;
        else if (a == "--json-out" && i + 1 < argc) jsonOut = argv[++i];
        else if (a == "--keff-box" && i + 1 < argc) {
            std::array<int, 4> b{};
            if (std::sscanf(argv[++i], "%d,%d,%d,%d", &b[0], &b[1], &b[2], &b[3]) != 4 || b[1] <= b[0] || b[3] <= b[2])
                throw std::invalid_argument("--keff-box Y0,Y1,X0,X1");
            keffBox = b;
        } else if (a == "--keff-origin" && i + 1 < argc) {
            if (std::sscanf(argv[++i], "%d,%d", &keffOrigin[0], &keffOrigin[1]) != 2 || keffOrigin[0] < 0 || keffOrigin[1] < 0)
                throw std::invalid_argument("--keff-origin TOP,LEFT (both >= 0)");
        } else if (a == "--frames" && i + 1 < argc) limit = std::stoll(argv[++i]);
        else if (a == "--threads" && i + 1 < argc) threads = std::max(1, std::atoi(argv[++i]));
        else if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--fits-out" && i + 1 < argc) fitsOut = argv[++i];
        else if (a == "--mask-out" && i + 1 < argc) maskOut = argv[++i];
        else throw std::invalid_argument("unknown argument " + a);
    }
    const stack::SourceFactory factory = [&] {
        return std::unique_ptr<ser::FrameSource>(new Truncated(ser::OpenVideo(path, bayer), limit));
    };
    const ser::FileInfo info = factory()->Info();
    const double scale = stack::LumaScale(info);
    std::printf("capture  %s\n", path.c_str());
    std::printf("frames   %lld, %dx%d, %s, luma scale %.0f, %d threads\n", static_cast<long long>(info.frameCount),
                info.header.width, info.header.height, ser::IsBayer(info.colorId) ? "Bayer" : "mono/RGB", scale, threads);

    auto t0 = std::chrono::steady_clock::now();
    const stack::NoiseModel noise = stack::FitNoiseModel(factory, info, threads, {});
    int clippedKnots = 0;
    for (const auto& k : noise.knots) clippedKnots += k.clipped;
    std::printf("noise    var = %.4g + %.4g t (luma units^2), %zu bins (%d clipped), %d frame triples\n", noise.a, noise.b,
                noise.knots.size(), clippedKnots, noise.frames);
    const std::vector<stack::FrameStats> stats = stack::MeasureAll(factory, info, threads, {}, noise);
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
    const stack::GlobalAligner aligner(ref, sel.fullFrameTarget, stack::LumaScale(info));
    const stack::RegionLayout layout = stack::PlaceRegions(ref, stack::MeasureFrame(ref, 0, stack::LumaScale(info)), sel.fullFrameTarget, noise);
    const stack::RegionMeasurer measurer(ref, layout);
    // RunStack's own flow (Engine.cpp): the selected frames AND the suspects are aligned; a frame under
    // the absolute floor is not region-measured; the clean frames' cut; AdmitSuspects decides the
    // suspects against it; the kept frames are the clean ones over the cut plus the admitted suspects.
    std::vector<int64_t> alignList = sel.frames;
    for (const stack::Suspect& s : sel.suspects) alignList.push_back(s.index);
    std::sort(alignList.begin(), alignList.end());
    const int64_t n = static_cast<int64_t>(alignList.size());
    std::vector<stack::Shift> shifts(static_cast<size_t>(n));
    std::vector<stack::FrameRegions> measured(static_cast<size_t>(n));
    std::vector<uint8_t> hasRegions(static_cast<size_t>(n), 0);
    std::vector<std::unique_ptr<stack::ResidualScale>> residuals;
    for (int w = 0; w < threads; ++w) residuals.push_back(std::make_unique<stack::ResidualScale>(layout, ref));
    std::atomic<int64_t> regionNs{0}, observeNs{0}, readNs{0}, demosaicNs{0}, addNs{0};
    ParallelFrames(n, threads, factory, [&](int w, ser::FrameSource& src, int64_t i) {
        const int64_t f = alignList[static_cast<size_t>(i)];
        ser::Frame rawA = src.ReadFrame(f);
        hot.Repair(rawA);
        const ser::FloatImage luma = stack::MeasurementLuma(rawA, info);
        shifts[static_cast<size_t>(i)] = aligner.Align(luma, stats[static_cast<size_t>(f)]);
        if (!(shifts[static_cast<size_t>(i)].confidence >= stack::kMinAlignConfidence)) return;
        const auto r0 = std::chrono::steady_clock::now();
        measured[static_cast<size_t>(i)] = measurer.Measure(luma, f, shifts[static_cast<size_t>(i)]);
        hasRegions[static_cast<size_t>(i)] = 1;
        regionNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - r0).count();
        const auto o0 = std::chrono::steady_clock::now();
        residuals[static_cast<size_t>(w)]->Observe(luma, measured[static_cast<size_t>(i)]);
        observeNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - o0).count();
    });
    std::vector<uint8_t> isSuspect(static_cast<size_t>(n), 0);
    std::vector<stack::Shift> clean, suspectShifts;
    for (const stack::Suspect& s : sel.suspects) {
        const auto it = std::lower_bound(alignList.begin(), alignList.end(), s.index);
        const size_t p = static_cast<size_t>(it - alignList.begin());
        isSuspect[p] = 1;
        suspectShifts.push_back(shifts[p]);
    }
    for (size_t p = 0; p < alignList.size(); ++p)
        if (!isSuspect[p]) clean.push_back(shifts[p]);
    const double cut = stack::AlignmentCut(clean);
    const stack::Admission adm = stack::AdmitSuspects(sel, suspectShifts, cut);
    std::vector<stack::FrameRegions> kept;
    std::vector<stack::FrameRegions> keptGlobal;
    for (size_t p = 0; p < alignList.size(); ++p) {
        const bool pass = stack::PassesAlignment(shifts[p], cut);
        const bool use = isSuspect[p] ? std::binary_search(adm.admitted.begin(), adm.admitted.end(), alignList[p]) : pass;
        if (!use || !hasRegions[p]) continue;
        kept.push_back(measured[p]);
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
        std::vector<double> ratio;
        for (size_t i = 0; i < q.size(); ++i) ratio.push_back(s[i] > 0 ? q[i] / s[i] : 1e9);
        std::sort(ratio.begin(), ratio.end());
        std::printf("detail   %3zu: median q / qualitySigma %.2f%s\n", r, ratio[ratio.size() / 2],
                    plan.detail[r].noiseOnly ? " (noise only: plain mean)" : "");
        std::sort(q.begin(), q.end());
        std::sort(s.begin(), s.end());
        const auto& d = plan.detail[r];
        const auto& reg = layout.regions[r];
        std::printf("region   %3zu at %4d,%4d: q p10 %.3f p50 %.3f p90 %.3f max %.3f, sigma %.5f; best %.3f, kept %lld of %lld\n", r, reg.x0,
                    reg.y0, q[q.size() / 10], q[q.size() / 2], q[q.size() * 9 / 10], q.back(), s[s.size() / 2], d.bestQuality,
                    static_cast<long long>(d.kept), static_cast<long long>(d.candidates));
    }
    // The layout's blend mask (Regions.h, RegionBlendMask): the stack, and BoxKeff, weigh the regions by it.
    const stack::BlendMask& mask = layout.mask;
    if (mask.Empty()) {
        std::printf("mask     none (every region weight as laid out)\n");
    } else {
        double full = 0, part = 0;
        for (float v : *mask.m) {
            full += v >= 1;
            part += v > 0 && v < 1;
        }
        const double n = static_cast<double>(mask.m->size());
        std::printf("mask     over luma x %d-%d, y %d-%d: %.1f%% of it 1, %.1f%% tapering, the rest 0 (the fallback alone)\n", mask.x0,
                    mask.x0 + mask.width, mask.y0, mask.y0 + mask.height, 100 * full / n, 100 * part / n);
    }
    {
        // What the mask was judged on, and the regions whose window it leaves at 0 throughout (sky only).
        int skyOnly = 0;
        if (!mask.Empty())
            for (const stack::Region& g : layout.regions) {
                double mx = 0;
                for (int y = g.y0; y < g.y1; ++y)
                    for (int x = g.x0; x < g.x1; ++x) mx = std::max(mx, mask.At(x, y));
                skyOnly += mx == 0;
            }
        std::printf("masksky  sky level %.5f, target level %.5f (sky / target %.4f), share under the seed threshold %.4f; %d of %zu regions sky only\n",
                    mask.skyLevel, mask.targetLevel, mask.targetLevel > 0 ? mask.skyLevel / mask.targetLevel : 0.0, mask.shareUnder, skyOnly,
                    layout.regions.size());
    }
    if (!maskOut.empty()) {
        ser::FloatImage mi{ref.width, ref.height, 1, std::vector<float>(ref.data.size(), mask.Empty() ? 1.f : 0.f)};
        for (int y = 0; y < mask.height; ++y)
            for (int x = 0; x < mask.width; ++x)
                mi.data[static_cast<size_t>(y + mask.y0) * ref.width + (x + mask.x0)] = (*mask.m)[static_cast<size_t>(y) * mask.width + x];
        probe::WriteFits(maskOut, mi);
        probe::WriteFits(maskOut + ".ref.fits", ref);
        std::printf("wrote    %s (the mask, luma grid) and %s.ref.fits (the reference)\n", maskOut.c_str(), maskOut.c_str());
    }
    if (keffBox) {
        const auto& b = *keffBox;
        const int oy = keffOrigin[0], ox = keffOrigin[1];  // the box on this (uncropped) grid: + origin
        const int width = info.header.width, height = info.header.height;
        if (b[1] + oy > height || b[3] + ox > width)
            throw std::invalid_argument("--keff-box + --keff-origin runs past the " + std::to_string(width) + "x" +
                                        std::to_string(height) + " grid");
        const std::string unreached = BoxReachError(plan, b[0] + oy, b[1] + oy, b[2] + ox, b[3] + ox, scale, width, height,
                                                    stack::FrameGuard(info));
        if (!unreached.empty()) throw std::runtime_error(unreached);
        const double keff = BoxKeff(plan, b[0] + oy, b[1] + oy, b[2] + ox, b[3] + ox, scale);
        const double keffGlobal = BoxKeff(global, b[0] + oy, b[1] + oy, b[2] + ox, b[3] + ox, scale);
        std::printf("keff     %.2f over the box y %d-%d, x %d-%d of the grid cropped at (%d, %d) (stacked %lld)\n", keff, b[0], b[1],
                    b[2], b[3], oy, ox, static_cast<long long>(plan.stackedCount));
        if (!jsonOut.empty()) {
            std::FILE* f = std::fopen(jsonOut.c_str(), "w");
            if (!f) throw std::runtime_error("cannot write " + jsonOut);
            std::fprintf(f, "{\"stacked\": %lld, \"keffBox\": %.6f, \"keffBoxGlobal\": %.6f, \"box\": [%d, %d, %d, %d], "
                            "\"origin\": [%d, %d], \"regionKept\": [",
                         static_cast<long long>(plan.stackedCount), keff, keffGlobal, b[0], b[1], b[2], b[3], oy, ox);
            for (size_t r = 0; r < plan.detail.size(); ++r)
                std::fprintf(f, "%s%lld", r ? ", " : "", static_cast<long long>(plan.detail[r].kept));
            std::fprintf(f, "], \"regionSize\": %d, \"nodes\": [%d, %d], \"flatEdges\": %s}\n", layout.size, layout.nx,
                         layout.ny, layout.flatEdges ? "true" : "false");
            std::fclose(f);
        }
    }
    if (planOnly) return 0;
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
    size_t zeros = 0, zerosGlobal = 0;
    for (float v : ours.data) zeros += v == 0.f;
    for (float v : base.data) zerosGlobal += v == 0.f;
    std::printf("stack    %lld of %zu planned frames stacked; rejected cells %lld (%.4f%% of stacked frame cells); zero samples: ours %zu, global %zu; held %.1f MB\n",
                static_cast<long long>(plan.stackedCount), plan.frames.size(), static_cast<long long>(acc.RejectedCells()),
                100.0 * acc.RejectedCells() / (static_cast<double>(plan.stackedCount) * (info.header.width / scale) * (info.header.height / scale)),
                zeros, zerosGlobal, acc.BytesHeld() / 1e6);
    std::printf("pass B   per frame and thread: read %.1f ms, demosaic %.1f ms, Add (regions) %.1f ms\n", readNs.load() / 1e6 / plan.frames.size(), demosaicNs.load() / 1e6 / plan.frames.size(), addNs.load() / 1e6 / plan.frames.size());
    std::printf("time     measure %.2f s, select+reference %.2f s, pass A %.2f s (regions %.1f, residual %.1f ms/frame/thread), plan %.2f s, "
                "pass B (both stacks) %.2f s\n",
                tMeasure, tRef, tPassA, regionNs.load() / 1e6 / std::max<int64_t>(1, n), observeNs.load() / 1e6 / std::max<int64_t>(1, n), tPlan, tPassB);

    if (!fitsOut.empty()) {
        probe::WriteFits(fitsOut + "/ours.fits", ours);
        probe::WriteFits(fitsOut + "/global.fits", base);
        std::printf("wrote    %s/{ours,global}.fits\n", fitsOut.c_str());
    }
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
