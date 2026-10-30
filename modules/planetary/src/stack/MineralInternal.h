#pragma once
// Mineral.cpp's helpers that Planet.cpp shares (plan Task 2): moved here unchanged from Mineral.cpp's anonymous
// namespaces (only `inline` added), so the mineral moon compiles the same code and stays bit-identical. Internal to
// the stack engine: not part of its interface.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "Mineral.h"
#include "Parallel.h"

namespace planetary::stack::mineral_internal {

inline size_t Pixels(const ser::FloatImage& im) { return static_cast<size_t>(im.width) * static_cast<size_t>(im.height); }

inline void RequireRgb(const ser::FloatImage& rgb, const char* who) {
    if (rgb.channels != 3 || rgb.width <= 0 || rgb.height <= 0 || rgb.data.size() != Pixels(rgb) * 3)
        throw std::invalid_argument(std::string(who) + ": needs a 3-channel image whose data matches its size");
}

// Median of v (v is reordered). 0 for an empty vector.
inline double Median(std::vector<float>& v) {
    if (v.empty()) return 0;
    const auto mid = v.begin() + static_cast<long>(v.size() / 2);
    std::nth_element(v.begin(), mid, v.end());
    return *mid;
}

// Robust std: 1.4826 x the median absolute deviation from the median (v is reordered).
inline double MadStd(std::vector<float>& v) {
    if (v.size() < 2) return 0;
    const double med = Median(v);
    for (float& x : v) x = static_cast<float>(std::fabs(x - med));
    return 1.4826 * Median(v);
}

// printf-style formatting of numbers into a HISTORY fragment (ASCII: the formats are). Every argument is passed
// as a double, so every conversion is a floating one (%f, %g, %e).
template <class... A>
std::string Fmt(const char* format, A... args) {
    char buf[256];
    std::snprintf(buf, sizeof buf, format, static_cast<double>(args)...);
    return buf;
}

inline std::string Percent(size_t part, size_t whole) { return Fmt("%.2f%%", 100.0 * part / std::max<size_t>(1, whole)); }

// Chebyshev dilation of `mask` by r px (separable max), in place.
inline void Dilate(std::vector<uint8_t>& mask, int w, int h, int r) {
    if (r <= 0) return;
    std::vector<uint8_t> tmp(mask.size(), 0);
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = &mask[static_cast<size_t>(y) * w];
        int last = -1 - r;  // the most recent member at or left of x (none yet: further than r)
        // two sweeps: a member within r to the left, then to the right
        for (int x = 0; x < w; ++x) {
            if (row[x]) last = x;
            if (x - last <= r) tmp[static_cast<size_t>(y) * w + x] = 1;
        }
        last = w + r;
        for (int x = w - 1; x >= 0; --x) {
            if (row[x]) last = x;
            if (last - x <= r) tmp[static_cast<size_t>(y) * w + x] = 1;
        }
    }
    std::fill(mask.begin(), mask.end(), 0);
    for (int x = 0; x < w; ++x) {
        int last = -1 - r;
        for (int y = 0; y < h; ++y) {
            if (tmp[static_cast<size_t>(y) * w + x]) last = y;
            if (y - last <= r) mask[static_cast<size_t>(y) * w + x] = 1;
        }
        last = h + r;
        for (int y = h - 1; y >= 0; --y) {
            if (tmp[static_cast<size_t>(y) * w + x]) last = y;
            if (last - y <= r) mask[static_cast<size_t>(y) * w + x] = 1;
        }
    }
}

inline void RequireMasks(const ser::FloatImage& rgb, const LunarMasks& m, const char* who) {
    RequireRgb(rgb, who);
    if (m.width != rgb.width || m.height != rgb.height || m.disk.size() != Pixels(rgb) || m.sky.size() != Pixels(rgb))
        throw std::invalid_argument(std::string(who) + ": masks do not match the image");
}

// Otsu's threshold over a 1024-bin histogram of v between its smallest and largest finite values (NaN left out,
// +-Inf in the end bins). The full range, not a percentile clip: a disc under 0.1% of the frame lies above the
// 99.9th percentile, and with that clip it shared the top bin with the sky's noise tail, so Otsu split the sky
// (task ENG review I2). +inf when v has no finite value.
inline double Otsu(const std::vector<float>& v) {
    double a = std::numeric_limits<double>::infinity(), b = -a;
    for (float x : v)
        if (std::isfinite(x)) {
            a = std::min(a, static_cast<double>(x));
            b = std::max(b, static_cast<double>(x));
        }
    if (!(b > a)) return a;
    constexpr int kBins = 1024;
    std::vector<double> hist(kBins, 0);
    for (float x : v)
        if (!std::isnan(x)) hist[static_cast<size_t>(std::clamp((x - a) / (b - a) * kBins, 0.0, kBins - 1.0))] += 1;
    double total = 0, sumAll = 0;
    for (int i = 0; i < kBins; ++i) {
        total += hist[i];
        sumAll += i * hist[i];
    }
    double w0 = 0, sum0 = 0, best = -1;
    int bestI = 0;
    for (int i = 0; i < kBins - 1; ++i) {
        w0 += hist[i];
        sum0 += i * hist[i];
        const double w1 = total - w0;
        if (w0 <= 0 || w1 <= 0) continue;
        const double m0 = sum0 / w0, m1 = (sumAll - sum0) / w1;
        const double between = w0 * w1 * (m0 - m1) * (m0 - m1);
        if (between > best) {
            best = between;
            bestI = i;
        }
    }
    return a + (bestI + 1.0) / kBins * (b - a);
}

// A plane a + b u + c v over the frame, u = x / (w - 1) - 0.5, v = y / (h - 1) - 0.5 (so b and c are the
// rise across the frame and a the level at its centre).
struct Plane {
    double a = 0, b = 0, c = 0;
    double At(int x, int y, int w, int h) const {
        return a + b * (static_cast<double>(x) / std::max(1, w - 1) - 0.5) + c * (static_cast<double>(y) / std::max(1, h - 1) - 0.5);
    }
};

// Least-squares plane of channel ch over the sky, three rounds of rejecting residuals beyond 3 robust sigma (a
// star, a hot pixel, a satellite trail).
inline Plane SkyPlane(const ser::FloatImage& rgb, const LunarMasks& m, int ch) {
    const int w = rgb.width, h = rgb.height;
    std::vector<uint8_t> use = m.sky;
    Plane p;
    for (int round = 0; round < 4; ++round) {
        double S[3][3] = {}, r[3] = {};
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const size_t i = static_cast<size_t>(y) * w + x;
                if (!use[i]) continue;
                const double f[3] = {1, static_cast<double>(x) / std::max(1, w - 1) - 0.5,
                                     static_cast<double>(y) / std::max(1, h - 1) - 0.5};
                const double v = rgb.data[i * 3 + ch];
                for (int a = 0; a < 3; ++a) {
                    r[a] += f[a] * v;
                    for (int b = 0; b < 3; ++b) S[a][b] += f[a] * f[b];
                }
            }
        // Solve S q = r (Gaussian elimination with partial pivoting; S is symmetric positive definite unless
        // the sky is a line, in which case the slope along the degenerate axis is left 0).
        double M[3][4];
        for (int a = 0; a < 3; ++a) {
            for (int b = 0; b < 3; ++b) M[a][b] = S[a][b];
            M[a][3] = r[a];
        }
        double q[3] = {0, 0, 0};
        int rank = 0;
        for (int col = 0; col < 3; ++col) {
            int piv = col;
            for (int a = col + 1; a < 3; ++a)
                if (std::fabs(M[a][col]) > std::fabs(M[piv][col])) piv = a;
            if (std::fabs(M[piv][col]) < 1e-9 * std::max(1.0, S[0][0])) continue;
            std::swap(M[piv], M[col]);
            for (int a = 0; a < 3; ++a)
                if (a != col) {
                    const double f = M[a][col] / M[col][col];
                    for (int b = col; b < 4; ++b) M[a][b] -= f * M[col][b];
                }
            ++rank;
        }
        for (int a = 0; a < 3; ++a)
            if (std::fabs(M[a][a]) >= 1e-9 * std::max(1.0, S[0][0])) q[a] = M[a][3] / M[a][a];
        if (rank == 0) break;
        p = {q[0], q[1], q[2]};
        if (round == 3) break;
        std::vector<float> res;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const size_t i = static_cast<size_t>(y) * w + x;
                if (use[i]) res.push_back(static_cast<float>(rgb.data[i * 3 + ch] - p.At(x, y, w, h)));
            }
        const double sd = MadStd(res);
        if (!(sd > 0)) break;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const size_t i = static_cast<size_t>(y) * w + x;
                if (m.sky[i]) use[i] = std::fabs(rgb.data[i * 3 + ch] - p.At(x, y, w, h)) <= 3 * sd;
            }
    }
    return p;
}

// Solves the n x n system A x = b in place (Gaussian elimination, partial pivoting). False when singular.
inline bool Solve(int n, double* A, double* b, double* x) {
    for (int col = 0; col < n; ++col) {
        int piv = col;
        for (int r = col + 1; r < n; ++r)
            if (std::fabs(A[r * n + col]) > std::fabs(A[piv * n + col])) piv = r;
        if (!(std::fabs(A[piv * n + col]) > 1e-300)) return false;
        if (piv != col) {
            for (int k = 0; k < n; ++k) std::swap(A[piv * n + k], A[col * n + k]);
            std::swap(b[piv], b[col]);
        }
        for (int r = col + 1; r < n; ++r) {
            const double f = A[r * n + col] / A[col * n + col];
            for (int k = col; k < n; ++k) A[r * n + k] -= f * A[col * n + k];
            b[r] -= f * b[col];
        }
    }
    for (int r = n - 1; r >= 0; --r) {
        double s = b[r];
        for (int k = r + 1; k < n; ++k) s -= A[r * n + k] * x[k];
        x[r] = s / A[r * n + r];
    }
    return true;
}

// Catmull-Rom (Keys a = -0.5) weights for a fractional offset f in [0, 1), taps at -1, 0, 1, 2: the engine's
// own interpolator (Accumulate's bicubic samples). It interpolates (f = 0 is the identity).
inline void CubicWeights(double f, double wt[4]) {
    const double f2 = f * f, f3 = f2 * f;
    wt[0] = -0.5 * f3 + f2 - 0.5 * f;
    wt[1] = 1.5 * f3 - 2.5 * f2 + 1;
    wt[2] = -1.5 * f3 + 2 * f2 + 0.5 * f;
    wt[3] = 0.5 * f3 - 0.5 * f2;
}

// Sample k of a row or column of n samples (at(i), 0 <= i < n), extended point-symmetrically past its ends: at(-k) =
// 2 at(0) - at(k), so a sub-pixel shift at the frame's edge extrapolates the local slope (Ruling R32). Replicating the
// edge sample instead left the outermost pixel where dispersion put it while the rest was aligned (real 64.6F:
// split-half edge chroma noise 3.5x in that column, against 1.0x unaligned; Task B9).
template <typename At>
double Extended(At at, int k, int n) {
    if (k >= 0 && k < n) return at(k);
    const int e = k < 0 ? 0 : n - 1, m = std::clamp(2 * e - k, 0, n - 1);
    return 2.0 * at(e) - at(m);
}

// Channel c of `src` sampled at (x + sx, y + sy) at every pixel, edges extended point-symmetrically (Extended): a
// 1-channel plane.
inline std::vector<float> ShiftedPlane(const ser::FloatImage& src, int c, double sx, double sy, int threads) {
    const int w = src.width, h = src.height, nc = src.channels;
    const int ix = static_cast<int>(std::floor(sx)), iy = static_cast<int>(std::floor(sy));
    double wx[4], wy[4];
    CubicWeights(sx - ix, wx);
    CubicWeights(sy - iy, wy);
    const size_t n = Pixels(src);
    std::vector<float> tmp(n), out(n);
    ParallelBands(h, threads, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const float* row = &src.data[static_cast<size_t>(y) * w * nc + c];
            auto at = [&](int i) { return static_cast<double>(row[static_cast<size_t>(i) * nc]); };
            for (int x = 0; x < w; ++x) {
                double s = 0;
                for (int k = 0; k < 4; ++k) s += wx[k] * Extended(at, x + ix - 1 + k, w);
                tmp[static_cast<size_t>(y) * w + x] = static_cast<float>(s);
            }
        }
    });
    ParallelBands(h, threads, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x) {
                auto at = [&](int i) { return static_cast<double>(tmp[static_cast<size_t>(i) * w + x]); };
                double s = 0;
                for (int k = 0; k < 4; ++k) s += wy[k] * Extended(at, y + iy - 1 + k, h);
                out[static_cast<size_t>(y) * w + x] = static_cast<float>(s);
            }
    });
    return out;
}

// The frame's edge after ShiftedPlane (Ruling R32, Task B9). Where a pixel's Catmull-Rom taps for the shift (sx, sy)
// leave the frame, its shifted sample is extrapolated, and no extension rule recovers light the frame never held:
// on MoonSynth (0.5 px shift, texture to 1/4 cycle/px) the point-symmetric extension still left the outermost pixel
// at 8x the interior's error and the next at 3x. The light's colour varies far more slowly than its brightness, so
// there the plane takes G's own sample (the reference channel, measured at that pixel) times the channel's ratio to
// G over the nearest 3 x 3 pixels whose taps stay inside. P: the shifted plane of `rgb`'s channel, in place.
inline void FillFrameEdge(std::vector<float>& P, const ser::FloatImage& rgb, double sx, double sy) {
    const int w = rgb.width, h = rgb.height;
    const int ix = static_cast<int>(std::floor(sx)), iy = static_cast<int>(std::floor(sy));
    // Taps x + ix - 1 .. x + ix + 2 stay inside for x in [x0, x1]; likewise y.
    const int x0 = std::max(0, 1 - ix), x1 = std::min(w - 1, w - 3 - ix);
    const int y0 = std::max(0, 1 - iy), y1 = std::min(h - 1, h - 3 - iy);
    if (x1 - x0 < 2 || y1 - y0 < 2) return;  // no 3 x 3 inside to take a ratio from
    auto g = [&](int x, int y) { return static_cast<double>(rgb.data[(static_cast<size_t>(y) * w + x) * 3 + 1]); };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (x >= x0 && x <= x1 && y >= y0 && y <= y1) {
                x = x1;  // the interior of this row needs nothing
                continue;
            }
            const int xa = std::clamp(x - 1, x0, x1 - 2), ya = std::clamp(y - 1, y0, y1 - 2);
            double sp = 0, sg = 0;
            for (int v = ya; v < ya + 3; ++v)
                for (int u = xa; u < xa + 3; ++u) {
                    sp += P[static_cast<size_t>(v) * w + u];
                    sg += g(u, v);
                }
            if (sg > 0) P[static_cast<size_t>(y) * w + x] = static_cast<float>(g(x, y) * sp / sg);
        }
}

// ---- The prepare/finish pipeline, shared by the mineral moon and the planets (plan Task 4) ----

// Runs a pipeline's steps, timing each and reporting (phase, step, total); a cancel seen at any report throws
// StackCancelled ("<product> cancelled") before any image is returned. Moved from Mineral.cpp (the mineral moon's
// MineralSteps), its phase and product made parameters.
class PipelineSteps {
public:
    PipelineSteps(const Progress& progress, const char* phase, const char* product, int total)
        : progress_(progress), phase_(phase), product_(product), total_(total) {}
    template <class Fn>
    void Run(const std::string& name, std::vector<std::pair<std::string, double>>& seconds, Fn fn) {
        Tick();
        const auto t0 = std::chrono::steady_clock::now();
        fn();
        seconds.push_back({name, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count()});
        ++done_;
    }
    void Tick() {
        if (progress_.report) progress_.report(phase_, done_, total_);
        if (progress_.cancelled && progress_.cancelled()) throw StackCancelled(std::string(product_) + " cancelled");
    }
    void Report(int done) {
        done_ = done;
        Tick();
    }

private:
    const Progress& progress_;
    const char* phase_;
    const char* product_;
    int total_, done_ = 0;
};

// Seven steps: the disc, colour calibration, background, display transfer, channel alignment (PrepareDisc's five),
// then the sharpening and the colour (the finish).
constexpr int kPipelineSteps = 7;

// What differs between the mineral moon's prepare and the planet's (spec call 2: one pipeline, parameterised by the
// target kind, not forked).
struct DiscPipeline {
    const char* phase;    // the Progress phase (kPhaseMineral, kPhasePlanet)
    const char* product;  // "Mineral moon", "Planet processing": refusal and cancel messages
    const char* discStep; // the first step's name and HISTORY prefix ("Lunar disk", "Planet disc")
    const char* noDisc;   // the notice, and every later step's skip reason, when the first step finds no disc
    bool discSize;        // the first step's line also gives the disc's px and equivalent radius
    std::function<StepRecord(ser::FloatImage&, const LunarMasks&)> calibrate, align;
    double displayMedian; // DisplayTransfer's medianTarget (<= 0: linear)
    bool highlightThreshold;  // measure cache.satThreshold (the mineral colour's highlight fade)
    // The first step's masks: SegmentLunar when empty (the Moon); the planet's SegmentPlanet.
    std::function<LunarMasks(const ser::FloatImage&, int)> segment = {};
};
// PrepareMineral's body (Mineral.h gives the contract), for either kind.
MineralCache PrepareDisc(const ser::FloatImage& stack, int threads, const Progress& progress, const StackNoise* stackNoise,
                         const DiscPipeline& kind);

// MineralColour's transfer (Mineral.h gives the contract) without its HISTORY line: what the line reports.
struct ColourCounts {
    double sigma = 0, threshold = 0;  // the chroma blur applied (px, scaled to the width) and the fade's threshold
    size_t faded = 0, held = 0;       // pixels with w < 1 by the fade; pixels whose w the gamut guard lowered
};
ColourCounts ColourTransfer(ser::FloatImage& rgb, const ser::FloatImage& lumSharp, const MineralParams& p, int threads);

}  // namespace planetary::stack::mineral_internal
