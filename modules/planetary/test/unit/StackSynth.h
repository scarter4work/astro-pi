#pragma once
// Synthetic captures for the region / stacking tests (Task 5): analytic scenes whose shifted and
// blurred versions are exact (no resampling in the generator), per-frame displacement and blur fields
// that vary across the frame, and an in-memory FrameSource so the real pipeline (MeasureAll ->
// SelectFrames -> BuildReference -> GlobalAligner -> regions -> Accumulator) runs on them.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <vector>

#include "Align.h"
#include "Demosaic.h"
#include "FrameSource.h"
#include "Measure.h"
#include "Select.h"

namespace planetary::testing {

// A scene evaluable at any point with any extra Gaussian blur sigma (exact for its components:
// a sinusoid's amplitude falls by exp(-2 pi^2 sigma^2 f^2); a Gaussian blob of width s becomes one of
// width sqrt(s^2 + sigma^2) with the same flux; an erf disc edge widens in quadrature).
struct Scene {
    struct Wave { double u, v, a, phi; };
    struct Blob { double x, y, s, a; };
    std::vector<Wave> waves;
    std::vector<Blob> blobs;
    double level = 0.4;
    // Disc (radius > 0): the texture lives inside a disc of this radius centred at (cx, cy), edge width
    // edge, over a sky level `sky`.
    double cx = 0, cy = 0, radius = 0, edge = 0.6, sky = 0.02;

    // Blob index: cells of kBin px listing the blobs within kBinReach px of the cell (a blob of width s
    // under blur sigma <= 3 contributes under e^-8 of its amplitude beyond 4 sqrt(s^2 + 9) <= 27 px).
    static constexpr double kBin = 16, kBinReach = 30;
    std::vector<std::vector<int>> bins;
    double binX0 = 0, binY0 = 0;
    int bnx = 0, bny = 0;
    void Bin(double x0, double y0, double x1, double y1) {
        binX0 = x0;
        binY0 = y0;
        bnx = static_cast<int>(std::ceil((x1 - x0) / kBin));
        bny = static_cast<int>(std::ceil((y1 - y0) / kBin));
        bins.assign(static_cast<size_t>(bnx) * bny, {});
        for (size_t k = 0; k < blobs.size(); ++k)
            for (int j = 0; j < bny; ++j)
                for (int i = 0; i < bnx; ++i) {
                    const double cx0 = x0 + i * kBin, cy0 = y0 + j * kBin;
                    const double ddx = std::max({cx0 - blobs[k].x, 0.0, blobs[k].x - (cx0 + kBin)});
                    const double ddy = std::max({cy0 - blobs[k].y, 0.0, blobs[k].y - (cy0 + kBin)});
                    if (ddx * ddx + ddy * ddy <= kBinReach * kBinReach) bins[static_cast<size_t>(j) * bnx + i].push_back(static_cast<int>(k));
                }
    }
    double Eval(double x, double y, double sigma) const {
        double v = level;
        for (const Wave& w : waves) {
            const double f2 = w.u * w.u + w.v * w.v;
            v += w.a * std::exp(-2 * M_PI * M_PI * sigma * sigma * f2) * std::sin(2 * M_PI * (w.u * x + w.v * y) + w.phi);
        }
        auto add = [&](const Blob& b) {
            const double s2 = b.s * b.s + sigma * sigma, dx = x - b.x, dy = y - b.y;
            v += b.a * (b.s * b.s / s2) * std::exp(-(dx * dx + dy * dy) / (2 * s2));
        };
        if (bins.empty()) {
            for (const Blob& b : blobs) add(b);
        } else {
            const int i = std::clamp(static_cast<int>(std::floor((x - binX0) / kBin)), 0, bnx - 1);
            const int j = std::clamp(static_cast<int>(std::floor((y - binY0) / kBin)), 0, bny - 1);
            for (int k : bins[static_cast<size_t>(j) * bnx + i]) add(blobs[static_cast<size_t>(k)]);
        }
        if (radius <= 0) return v;
        const double r = std::hypot(x - cx, y - cy);
        const double st = std::sqrt(edge * edge + sigma * sigma);
        const double inside = 0.5 * std::erfc((r - radius) / (std::sqrt(2.0) * st));
        return sky + inside * (v - sky);
    }
};

// A frame-filling lunar-like texture: a red spectrum of waves (finest at ~4 px) plus crater-like
// blobs (dark bowls with bright rims, sizes 1.5-6 px).
inline Scene MakeSurface(unsigned seed, int w, int h) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> uni(0, 1);
    Scene s;
    s.level = 0.45;
    for (int i = 0; i < 80; ++i) {
        const double f = 0.01 * std::pow(0.24 / 0.01, uni(rng)), th = 2 * M_PI * uni(rng);
        s.waves.push_back({f * std::cos(th), f * std::sin(th), 0.012 * std::sqrt(0.02 / f), 2 * M_PI * uni(rng)});
    }
    const int nb = w * h / 120;
    for (int i = 0; i < nb; ++i) {
        const double x = -10 + (w + 20) * uni(rng), y = -10 + (h + 20) * uni(rng), sz = 1.5 + 4.5 * uni(rng);
        s.blobs.push_back({x, y, sz, -0.10});
        s.blobs.push_back({x + 0.6 * sz, y - 0.4 * sz, 0.5 * sz, 0.12});
    }
    s.Bin(-40, -40, w + 40, h + 40);
    return s;
}

// A small planet: belts (waves along y), a few spots, inside a disc of the given radius.
inline Scene MakeDisc(unsigned seed, double cx, double cy, double radius) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> uni(0, 1);
    Scene s;
    s.level = 0.55;
    s.cx = cx;
    s.cy = cy;
    s.radius = radius;
    for (int i = 0; i < 6; ++i)
        s.waves.push_back({0.01 * (uni(rng) - 0.5), (0.05 + 0.15 * uni(rng)), 0.05, 2 * M_PI * uni(rng)});
    for (int i = 0; i < 6; ++i) {
        const double a = 2 * M_PI * uni(rng), r = 0.7 * radius * std::sqrt(uni(rng));
        s.blobs.push_back({cx + r * std::cos(a), cy + r * std::sin(a), 1.2 + 1.5 * uni(rng), uni(rng) < 0.5 ? 0.12 : -0.1});
    }
    return s;
}

// A smooth random field over the frame: values on a coarse grid of `cell` px, blended with sin^2
// weights (C1, no corners).
struct Field {
    int nx = 1, ny = 1;
    double cell = 32, ox = 0, oy = 0;
    std::vector<double> v;
    double At(double x, double y) const {
        auto axis = [&](double t, int n, double o, int& i0, double& w1) {
            const double g = (t - o) / cell;
            i0 = static_cast<int>(std::floor(g));
            const double f = g - i0;
            w1 = std::sin(0.5 * M_PI * f);
            w1 *= w1;
            if (i0 < 0) { i0 = 0; w1 = 0; }
            if (i0 >= n - 1) { i0 = n - 1; w1 = 0; }
        };
        int ix, iy;
        double wx, wy;
        axis(x, nx, ox, ix, wx);
        axis(y, ny, oy, iy, wy);
        const int ix1 = std::min(ix + 1, nx - 1), iy1 = std::min(iy + 1, ny - 1);
        auto g = [&](int i, int j) { return v[static_cast<size_t>(j) * nx + i]; };
        return (1 - wy) * ((1 - wx) * g(ix, iy) + wx * g(ix1, iy)) + wy * ((1 - wx) * g(ix, iy1) + wx * g(ix1, iy1));
    }
};

inline Field RandomField(std::mt19937& rng, int w, int h, double cell, const std::function<double()>& draw) {
    Field f;
    f.cell = cell;
    f.nx = static_cast<int>(std::ceil(w / cell)) + 2;
    f.ny = static_cast<int>(std::ceil(h / cell)) + 2;
    f.ox = -cell;
    f.oy = -cell;
    for (int i = 0; i < f.nx * f.ny; ++i) f.v.push_back(draw());
    (void)rng;
    return f;
}

// One synthetic frame: global shift g, local displacement (dxF, dyF), blur sigma field, gain, noise.
// frame(p) = gain * scene(p - g - local(p), sigma(p)) + noise: the scene's point x shows at x + g + d,
// so frame(x + g + d) ~ scene(x) (Align.h's convention).
struct FrameSpec {
    double gx = 0, gy = 0, gain = 1, noise = 0.01;
    Field dx, dy, sigma;
    bool hasLocal = false;
    // Optional: a displaced part (a tear): rows >= tearRow are moved by an extra (tearDx, tearDy).
    int tearRow = -1;
    double tearDx = 0, tearDy = 0;
    // Optional: a bright streak along the line through (sx0, sy0) - (sx1, sy1), width 1.5 px.
    bool streak = false;
    double sx0 = 0, sy0 = 0, sx1 = 0, sy1 = 0, streakLevel = 0.5;
};

inline std::vector<float> RenderFrame(const Scene& scene, int w, int h, const FrameSpec& f, unsigned noiseSeed) {
    std::vector<float> out(static_cast<size_t>(w) * h);
    std::mt19937 rng(noiseSeed);
    std::normal_distribution<double> n(0, 1);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            double ddx = f.gx, ddy = f.gy, sg = 0;
            if (f.hasLocal) {
                ddx += f.dx.At(x, y);
                ddy += f.dy.At(x, y);
                sg = std::max(0.0, f.sigma.At(x, y));
            }
            if (f.tearRow >= 0 && y >= f.tearRow) {
                ddx += f.tearDx;
                ddy += f.tearDy;
            }
            double v = f.gain * scene.Eval(x - ddx, y - ddy, sg);
            if (f.streak) {
                const double lx = f.sx1 - f.sx0, ly = f.sy1 - f.sy0, len = std::hypot(lx, ly);
                const double dist = std::fabs((x - f.sx0) * ly - (y - f.sy0) * lx) / len;
                v += f.streakLevel * std::exp(-dist * dist / (2 * 0.75 * 0.75));
            }
            v += f.noise * n(rng);
            out[static_cast<size_t>(y) * w + x] = static_cast<float>(std::clamp(v, 0.0, 1.0));
        }
    return out;
}

// In-memory capture: 16-bit mono (or a Bayer CFA when `bayer`), frames held as samples.
class MemorySource : public ser::FrameSource {
public:
    MemorySource(std::shared_ptr<const std::vector<std::vector<uint16_t>>> frames, ser::FileInfo info)
        : frames_(std::move(frames)), info_(std::move(info)) {}
    const ser::FileInfo& Info() const override { return info_; }
    ser::Frame ReadFrame(int64_t i) const override {
        if (i < 0 || i >= static_cast<int64_t>(frames_->size())) throw ser::SerError("MemorySource: index out of range");
        ser::Frame f;
        f.width = info_.header.width;
        f.height = info_.header.height;
        f.channels = 1;
        f.samples = (*frames_)[static_cast<size_t>(i)];
        return f;
    }
    std::optional<int64_t> FrameTimestamp(int64_t) const override { return std::nullopt; }

private:
    std::shared_ptr<const std::vector<std::vector<uint16_t>>> frames_;
    ser::FileInfo info_;
};

struct Capture {
    ser::FileInfo info;
    std::shared_ptr<std::vector<std::vector<uint16_t>>> frames = std::make_shared<std::vector<std::vector<uint16_t>>>();
    stack::SourceFactory Factory() const {
        auto fr = std::shared_ptr<const std::vector<std::vector<uint16_t>>>(frames);
        const ser::FileInfo inf = info;
        return [fr, inf] { return std::unique_ptr<ser::FrameSource>(new MemorySource(fr, inf)); };
    }
    void Add(const std::vector<float>& v) {
        std::vector<uint16_t> s(v.size());
        for (size_t i = 0; i < v.size(); ++i)
            s[i] = static_cast<uint16_t>(std::lround(std::clamp(static_cast<double>(v[i]), 0.0, 1.0) * 65535.0));
        frames->push_back(std::move(s));
        info.frameCount = static_cast<int64_t>(frames->size());
        info.header.frameCount = static_cast<int32_t>(frames->size());
    }
};

inline Capture NewCapture(int w, int h, ser::ColorId color = ser::ColorId::Mono) {
    Capture c;
    c.info.header.width = w;
    c.info.header.height = h;
    c.info.header.depth = 16;
    c.info.colorId = color;
    c.info.header.colorIdRaw = static_cast<int32_t>(color);
    c.info.planes = 1;
    c.info.bytesPerSample = 2;
    c.info.frameBytes = static_cast<int64_t>(w) * h * 2;
    c.info.normDepth = 16;
    return c;
}

}  // namespace planetary::testing
