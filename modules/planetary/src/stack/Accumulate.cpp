#include "Accumulate.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

#include "Measure.h"
#include "SerFile.h"

namespace planetary::stack {

namespace {

void CheckLuma(const ser::FloatImage& im, const char* what) {
    if (im.width <= 0 || im.height <= 0) throw std::invalid_argument(std::string(what) + ": empty image");
    if (im.channels != 1)
        throw std::invalid_argument(std::string(what) + ": expected a 1-channel luma image, got " +
                                    std::to_string(im.channels) + " channels");
    if (im.data.size() != static_cast<size_t>(im.width) * im.height)
        throw std::invalid_argument(std::string(what) + ": " + std::to_string(im.data.size()) + " samples for a " +
                                    std::to_string(im.width) + "x" + std::to_string(im.height) + " image");
}

// Luma rows per locked band of the accumulator.
// Luma rows per lock of the sums.
constexpr int kLockRows = 16;

// Catmull-Rom weights for fractional offset t in [0, 1).
void CubicWeights(double t, double w[4]) {
    const double t2 = t * t, t3 = t2 * t;
    w[0] = 0.5 * (-t3 + 2 * t2 - t);
    w[1] = 0.5 * (3 * t3 - 5 * t2 + 2);
    w[2] = 0.5 * (-3 * t3 + 4 * t2 + t);
    w[3] = 0.5 * (t3 - t2);
}

// Cubic B-spline weights for fractional offset t in [0, 1).
void BSplineWeights(float t, float w[4]) {
    const float u = 1 - t, t2 = t * t, t3 = t2 * t;
    w[0] = u * u * u / 6;
    w[1] = (3 * t3 - 6 * t2 + 4) / 6;
    w[2] = (-3 * t3 + 3 * t2 + 3 * t + 1) / 6;
    w[3] = t3 / 6;
}

inline int FloorInt(float v) {
    const int i = static_cast<int>(v);
    return i > v ? i - 1 : i;
}

// Mirror (whole-sample symmetric) index into [0, n), as the B-spline prefilter assumes.
inline int Mirror(int i, int n) {
    if (n == 1) return 0;
    const int period = 2 * (n - 1);
    i %= period;
    if (i < 0) i += period;
    return i < n ? i : period - i;
}

// Cubic B-spline interpolation coefficients along one line of n samples at stride `stride`, in place
// (Unser / Thevenaz: gain 6, one pole z = sqrt(3) - 2, mirror boundaries; the causal start is the
// series truncated where |z|^k < 1e-7, or the exact mirrored sum on a short line).
constexpr double kPole = -0.26794919243112270;  // sqrt(3) - 2
void PrefilterLine(float* c, int n, ptrdiff_t stride) {
    if (n < 2) return;
    const double z = kPole;
    auto at = [&](int i) -> float& { return c[static_cast<ptrdiff_t>(i) * stride]; };
    for (int i = 0; i < n; ++i) at(i) *= 6.0f;
    const int horizon = 13;  // |z|^13 < 4e-8
    double sum;
    if (horizon < n) {
        double zn = z;
        sum = at(0);
        for (int i = 1; i < horizon; ++i) {
            sum += zn * at(i);
            zn *= z;
        }
    } else {
        double zn = z;
        const double iz = 1 / z;
        double z2n = std::pow(z, n - 1);
        sum = at(0) + z2n * at(n - 1);
        z2n *= z2n * iz;
        for (int i = 1; i <= n - 2; ++i) {
            sum += (zn + z2n) * at(i);
            zn *= z;
            z2n *= iz;
        }
        sum /= 1 - zn * zn;
    }
    at(0) = static_cast<float>(sum);
    for (int i = 1; i < n; ++i) at(i) = static_cast<float>(at(i) + z * at(i - 1));
    at(n - 1) = static_cast<float>((z / (z * z - 1)) * (z * at(n - 2) + at(n - 1)));
    for (int i = n - 2; i >= 0; --i) at(i) = static_cast<float>(z * (at(i + 1) - at(i)));
}

// Up to 8 lines of n samples at once (the same recursion as PrefilterLine with the truncated start,
// n > 13): independent lines interleaved, so the serial recursion of one does not stall the others.
void PrefilterLines(float* const* line, int lines, int n, ptrdiff_t stride) {
    const float z = static_cast<float>(kPole), z0 = static_cast<float>(kPole / (kPole * kPole - 1));
    float prev[8];
    for (int k = 0; k < lines; ++k) {
        double sum = 0, zn = 1;
        for (int i = 0; i < 13; ++i) {
            sum += zn * 6.0 * line[k][i * stride];
            zn *= kPole;
        }
        prev[k] = static_cast<float>(sum);
        line[k][0] = prev[k];
    }
    for (int i = 1; i < n; ++i)
        for (int k = 0; k < lines; ++k) {
            float& v = line[k][i * stride];
            v = 6.0f * v + z * prev[k];
            prev[k] = v;
        }
    for (int k = 0; k < lines; ++k) {
        float& last = line[k][(n - 1) * stride];
        last = z0 * (z * line[k][(n - 2) * stride] + last);
        prev[k] = last;
    }
    for (int i = n - 2; i >= 0; --i)
        for (int k = 0; k < lines; ++k) {
            float& v = line[k][i * stride];
            v = z * (prev[k] - v);
            prev[k] = v;
        }
}

// Whole image (interleaved channels), separately: rows, then columns -- the columns all at once, row by row, so
// the vertical pass streams through memory.
void PrefilterSeparate(ser::FloatImage& im) {
    const int w = im.width, h = im.height, c = im.channels;
    const ptrdiff_t rowLen = static_cast<ptrdiff_t>(w) * c;
    if (w > 13) {
        float* line[8];
        int lines = 0;
        const int total = h * c;
        for (int l = 0; l < total; ++l) {
            line[lines++] = im.data.data() + (l / c) * rowLen + l % c;
            if (lines == 8 || l == total - 1) {
                PrefilterLines(line, lines, w, c);
                lines = 0;
            }
        }
    } else {
        for (int y = 0; y < h; ++y)
            for (int ch = 0; ch < c; ++ch) PrefilterLine(im.data.data() + y * rowLen + ch, w, c);
    }
    if (h < 2) return;
    const double z = kPole;
    float* d = im.data.data();
    auto row = [&](int y) { return d + y * rowLen; };
    for (ptrdiff_t k = 0; k < static_cast<ptrdiff_t>(h) * rowLen; ++k) d[k] *= 6.0f;
    const int horizon = 13;
    std::vector<double> sum(static_cast<size_t>(rowLen));
    if (horizon < h) {
        double zn = 1;
        for (int y = 0; y < horizon; ++y) {
            const float* r = row(y);
            for (ptrdiff_t k = 0; k < rowLen; ++k) sum[static_cast<size_t>(k)] += zn * r[k];
            zn *= z;
        }
    } else {
        double zn = z;
        const double iz = 1 / z;
        double z2n = std::pow(z, h - 1);
        for (ptrdiff_t k = 0; k < rowLen; ++k) sum[static_cast<size_t>(k)] = row(0)[k] + z2n * row(h - 1)[k];
        z2n *= z2n * iz;
        for (int y = 1; y <= h - 2; ++y) {
            const float* r = row(y);
            for (ptrdiff_t k = 0; k < rowLen; ++k) sum[static_cast<size_t>(k)] += (zn + z2n) * r[k];
            zn *= z;
            z2n *= iz;
        }
        for (double& s : sum) s /= 1 - zn * zn;
    }
    for (ptrdiff_t k = 0; k < rowLen; ++k) row(0)[k] = static_cast<float>(sum[static_cast<size_t>(k)]);
    const float zf = static_cast<float>(z);
    for (int y = 1; y < h; ++y) {
        float* r = row(y);
        const float* p = row(y - 1);
        for (ptrdiff_t k = 0; k < rowLen; ++k) r[k] += zf * p[k];
    }
    {
        float* r = row(h - 1);
        const float* p = row(h - 2);
        const float a = static_cast<float>(z / (z * z - 1));
        for (ptrdiff_t k = 0; k < rowLen; ++k) r[k] = a * (zf * p[k] + r[k]);
    }
    for (int y = h - 2; y >= 0; --y) {
        float* r = row(y);
        const float* n = row(y + 1);
        for (ptrdiff_t k = 0; k < rowLen; ++k) r[k] = zf * (n[k] - r[k]);
    }
}

// The same filter in two streaming passes over memory instead of five (the frame is 25 MB for 1080p
// RGB, and memory bandwidth is what limits many concurrent Adds): pass 1, top to bottom, filters each
// row horizontally while it is in cache and applies the vertical causal recursion to it; pass 2,
// bottom to top, the vertical anticausal one. Images with a side of 13 or less take the separate form.
void Prefilter(ser::FloatImage& im) {
    const int w = im.width, h = im.height, c = im.channels;
    if (w <= 13 || h <= 13) {
        PrefilterSeparate(im);
        return;
    }
    const ptrdiff_t rowLen = static_cast<ptrdiff_t>(w) * c;
    float* d = im.data.data();
    auto row = [&](int y) { return d + y * rowLen; };
    auto horizontal = [&](int y) {
        float* line[3];
        for (int ch = 0; ch < c; ++ch) line[ch] = row(y) + ch;
        PrefilterLines(line, c, w, c);
    };
    const float z = static_cast<float>(kPole);
    // Causal start: c+(0) = sum_{i < 13} z^i 6 r(i), over the first 13 rows (filtered horizontally first).
    for (int y = 0; y < 13; ++y) horizontal(y);
    std::vector<double> start(static_cast<size_t>(rowLen), 0.0);
    double zn = 1;
    for (int y = 0; y < 13; ++y) {
        const float* r = row(y);
        for (ptrdiff_t k = 0; k < rowLen; ++k) start[static_cast<size_t>(k)] += zn * 6.0 * r[k];
        zn *= kPole;
    }
    {
        float* r = row(0);
        for (ptrdiff_t k = 0; k < rowLen; ++k) r[k] = static_cast<float>(start[static_cast<size_t>(k)]);
    }
    for (int y = 1; y < h; ++y) {
        if (y >= 13) horizontal(y);
        float* r = row(y);
        const float* p = row(y - 1);
        for (ptrdiff_t k = 0; k < rowLen; ++k) r[k] = 6.0f * r[k] + z * p[k];
    }
    {
        float* r = row(h - 1);
        const float* p = row(h - 2);
        const float a = static_cast<float>(kPole / (kPole * kPole - 1));
        for (ptrdiff_t k = 0; k < rowLen; ++k) r[k] = a * (z * p[k] + r[k]);
    }
    for (int y = h - 2; y >= 0; --y) {
        float* r = row(y);
        const float* nx = row(y + 1);
        for (ptrdiff_t k = 0; k < rowLen; ++k) r[k] = z * (nx[k] - r[k]);
    }
}

// Catmull-Rom sample of a 1-channel image, neighbours clamped.
double SampleCubic(const ser::FloatImage& im, double x, double y) {
    const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
    double wx[4], wy[4];
    CubicWeights(x - x0, wx);
    CubicWeights(y - y0, wy);
    double acc = 0;
    for (int j = 0; j < 4; ++j) {
        const float* r = im.data.data() + static_cast<size_t>(std::clamp(y0 - 1 + j, 0, im.height - 1)) * im.width;
        double a = 0;
        for (int i = 0; i < 4; ++i) a += wx[i] * r[std::clamp(x0 - 1 + i, 0, im.width - 1)];
        acc += wy[j] * a;
    }
    return acc;
}

// Per-region warp arrays of one frame (shift usable where `aligned`, for ResidualScale).
struct FrameNodes {
    std::vector<float> dx, dy;
    std::vector<uint8_t> usable;
};

}  // namespace

void LocalWarpAt(const RegionLayout& layout, const AxisWeight& ax, const AxisWeight& ay, const float* dx,
                 const float* dy, const uint8_t* usable, double& outDx, double& outDy) {
    double nx = 0, ny = 0, den = 0;
    for (int b = 0; b < 2; ++b) {
        const int j = ay.i0 + b;
        const double wy = b ? ay.w1 : ay.w0;
        if (j < 0 || j >= layout.ny || wy == 0) continue;
        for (int a = 0; a < 2; ++a) {
            const int i = ax.i0 + a;
            const double wx = a ? ax.w1 : ax.w0;
            if (i < 0 || i >= layout.nx || wx == 0) continue;
            const int r = layout.node[static_cast<size_t>(j) * layout.nx + i];
            if (r < 0 || !usable[r]) continue;
            const double w = wx * wy;
            nx += w * dx[r];
            ny += w * dy[r];
            den += w;
        }
    }
    const double d = std::max(den, kWarpFade);
    outDx = nx / d;
    outDy = ny / d;
}

// ---- ResidualScale ---------------------------------------------------------------------------------

ResidualScale::ResidualScale(const RegionLayout& layout, const ser::FloatImage& reference)
    : layout_(layout), reference_(reference) {
    CheckLuma(reference, "ResidualScale");
    if (reference.width != layout.width || reference.height != layout.height)
        throw std::invalid_argument("ResidualScale: the reference is not the layout's size");
    sum2_.assign(reference.data.size(), 0.0);
    max2_.assign(reference.data.size(), 0.f);
    count_.assign(reference.data.size(), 0.f);
}

void ResidualScale::Observe(const ser::FloatImage& frameLuma, const FrameRegions& m) {
    CheckLuma(frameLuma, "ResidualScale::Observe");
    const int w = reference_.width, h = reference_.height;
    if (frameLuma.width != w || frameLuma.height != h)
        throw std::invalid_argument("ResidualScale::Observe: the frame is not the reference's size");
    const size_t nr = layout_.regions.size();
    if (m.regions.size() != nr)
        throw std::invalid_argument("ResidualScale::Observe: " + std::to_string(m.regions.size()) +
                                    " region measurements for " + std::to_string(nr) + " regions");
    FrameNodes nodes;
    nodes.dx.resize(nr);
    nodes.dy.resize(nr);
    nodes.usable.resize(nr);
    for (size_t r = 0; r < nr; ++r) {
        nodes.dx[r] = m.regions[r].dx;
        nodes.dy[r] = m.regions[r].dy;
        nodes.usable[r] = m.regions[r].aligned ? 1 : 0;
    }
    const double gain = std::isfinite(m.gain) && m.gain > 0 ? m.gain : 1.0;
    const float ig = static_cast<float>(1 / gain);
    // The blended local shift on a kWarpGrid grid, bilinear in between (as Accumulator::Add).
    const int G = kWarpGrid, gw = (w + G - 1) / G + 1, gh = (h + G - 1) / G + 1;
    std::vector<float> gX(static_cast<size_t>(gw) * gh, 0.f), gY(gX.size(), 0.f);
    if (nr > 0)
        for (int j = 0; j < gh; ++j) {
            const AxisWeight ay = AxisWeightAt(layout_, true, static_cast<double>(j) * G);
            for (int i = 0; i < gw; ++i) {
                double ldx, ldy;
                LocalWarpAt(layout_, AxisWeightAt(layout_, false, static_cast<double>(i) * G), ay, nodes.dx.data(),
                            nodes.dy.data(), nodes.usable.data(), ldx, ldy);
                gX[static_cast<size_t>(j) * gw + i] = static_cast<float>(ldx);
                gY[static_cast<size_t>(j) * gw + i] = static_cast<float>(ldy);
            }
        }
    std::vector<float> rX(static_cast<size_t>(gw)), rY(rX.size());
    const float* fp = frameLuma.data.data();
    for (int y = 0; y < h; ++y) {
        const int gj = y / G;
        const float ty = static_cast<float>(y - gj * G) / G;
        for (int i = 0; i < gw; ++i) {
            const size_t k0 = static_cast<size_t>(gj) * gw + i, k1 = k0 + gw;
            rX[i] = gX[k0] + ty * (gX[k1] - gX[k0]);
            rY[i] = gY[k0] + ty * (gY[k1] - gY[k0]);
        }
        for (int x = 0; x < w; ++x) {
            const int gi = x / G;
            const float tx = static_cast<float>(x - gi * G) / G;
            const double sx = x + m.global.dx + rX[gi] + tx * (rX[gi + 1] - rX[gi]);
            const double sy = y + m.global.dy + rY[gi] + tx * (rY[gi + 1] - rY[gi]);
            if (sx < 0 || sy < 0 || sx > w - 1 || sy > h - 1) continue;
            const int ix = static_cast<int>(sx), iy = static_cast<int>(sy);
            float v;
            if (ix >= 1 && iy >= 1 && ix + 2 < w && iy + 2 < h) {
                double wxd[4], wyd[4];
                CubicWeights(sx - ix, wxd);
                CubicWeights(sy - iy, wyd);
                const float* p0 = fp + static_cast<size_t>(iy - 1) * w + ix - 1;
                float acc = 0;
                for (int j = 0; j < 4; ++j) {
                    const float* p = p0 + static_cast<size_t>(j) * w;
                    acc += static_cast<float>(wyd[j]) *
                           (static_cast<float>(wxd[0]) * p[0] + static_cast<float>(wxd[1]) * p[1] +
                            static_cast<float>(wxd[2]) * p[2] + static_cast<float>(wxd[3]) * p[3]);
                }
                v = acc;
            } else {
                v = static_cast<float>(SampleCubic(frameLuma, sx, sy));
            }
            const size_t k = static_cast<size_t>(y) * w + x;
            const double r = v * ig - reference_.data[k];
            sum2_[k] += r * r;
            max2_[k] = std::max(max2_[k], static_cast<float>(r * r));
            count_[k] += 1;
        }
    }
    ++frames_;
}

void ResidualScale::Merge(const ResidualScale& other) {
    if (other.sum2_.size() != sum2_.size() || other.reference_.width != reference_.width ||
        other.layout_.regions.size() != layout_.regions.size())
        throw std::invalid_argument("ResidualScale::Merge: a different reference or layout");
    for (size_t k = 0; k < sum2_.size(); ++k) {
        sum2_[k] += other.sum2_[k];
        max2_[k] = std::max(max2_[k], other.max2_[k]);
        count_[k] += other.count_[k];
    }
    frames_ += other.frames_;
}

ser::FloatImage ResidualScale::Sigma(int normDepth) const {
    if (frames_ == 0) throw std::logic_error("ResidualScale::Sigma: nothing observed");
    if (normDepth < 1 || normDepth > 16) throw std::invalid_argument("ResidualScale::Sigma: depth outside 1..16");
    const int w = reference_.width, h = reference_.height;
    const double floor = 1.0 / (std::pow(2.0, normDepth) - 1);
    // Per pixel, robust to one outlier: the RMS without the pixel's largest squared residual (a trail
    // or a bird crossing in one observed frame does not widen its own tolerance).
    std::vector<float> raw(sum2_.size());
    std::vector<float> seen;
    for (size_t k = 0; k < raw.size(); ++k) {
        const double n = count_[k];
        raw[k] = n >= 2 ? static_cast<float>(std::sqrt(std::max(0.0, sum2_[k] - max2_[k]) / (n - 1)))
                 : n > 0 ? static_cast<float>(std::sqrt(sum2_[k] / n))
                         : -1.f;
        if (raw[k] >= 0) seen.push_back(raw[k]);
    }
    float fill = static_cast<float>(floor);
    if (!seen.empty()) {
        std::nth_element(seen.begin(), seen.begin() + static_cast<std::ptrdiff_t>(seen.size() / 2), seen.end());
        fill = seen[seen.size() / 2];
    }
    // The largest over 3 x 3 pixels: a sharp feature that lands a pixel off in some frame meets the
    // tolerance of its own pixel. Never a median or mean over space: thin structure (a rim, a spot, a
    // gap) carries the large residual on its own pixels only, and smoothing it away rejected the
    // sharpest frames exactly there (review finding: +293% RMS on sharp point features).
    ser::FloatImage out{w, h, 1, std::vector<float>(raw.size())};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float m = -1;
            for (int j = std::max(0, y - 1); j <= std::min(h - 1, y + 1); ++j)
                for (int i = std::max(0, x - 1); i <= std::min(w - 1, x + 1); ++i)
                    m = std::max(m, raw[static_cast<size_t>(j) * w + i]);
            out.data[static_cast<size_t>(y) * w + x] = std::max(m >= 0 ? m : fill, static_cast<float>(floor));
        }
    return out;
}

// ---- HotPixelMap -----------------------------------------------------------------------------------

HotPixelMap::HotPixelMap(const ser::FileInfo& info) {
    width_ = info.header.width;
    height_ = info.header.height;
    channels_ = info.colorId == ser::ColorId::RGB || info.colorId == ser::ColorId::BGR ? 3 : 1;
    step_ = ser::IsBayer(info.colorId) || UndeclaredMosaic(info) ? 2 : 1;
    if (width_ <= 0 || height_ <= 0) throw std::invalid_argument("HotPixelMap: empty frame size");
    if (info.normDepth < 1 || info.normDepth > 16) throw std::invalid_argument("HotPixelMap: depth outside 1..16");
    fullScale_ = std::pow(2.0, info.normDepth) - 1;
    isolated_.assign(static_cast<size_t>(width_) * height_ * channels_, 0);
}

void HotPixelMap::Observe(const ser::Frame& frame) {
    if (frame.width != width_ || frame.height != height_ || frame.channels != channels_ ||
        frame.samples.size() != isolated_.size())
        throw std::invalid_argument("HotPixelMap::Observe: the frame does not match the capture");
    const int d = step_, C = channels_, W = width_;
    const double minExcess = kHotMin * fullScale_;
    const uint16_t* s = frame.samples.data();
    for (int y = d; y < height_ - d; ++y)
        for (int x = d; x < W - d; ++x)
            for (int c = 0; c < C; ++c) {
                const size_t k = (static_cast<size_t>(y) * W + x) * C + c;
                const double v = s[k];
                if (v < minExcess) continue;
                // The ring, row by row: 1 and 6 are the column neighbours, 3 and 4 the row neighbours, 0 and 7
                // the diagonal ones, 2 and 5 the anti-diagonal ones.
                uint16_t ring[8], n[8];
                int m = 0;
                for (int j = -1; j <= 1; ++j)
                    for (int i = -1; i <= 1; ++i) {
                        if (!i && !j) continue;
                        ring[m++] = s[(static_cast<size_t>(y + j * d) * W + (x + i * d)) * C + c];
                    }
                std::copy(ring, ring + 8, n);
                // A lone site: above the whole ring (sorted ascending), by the shape margin over its median.
                std::sort(n, n + 8);
                const double excess = v - n[7];
                const double med = 0.5 * (n[3] + n[4]);
                bool alone = excess > minExcess && excess > kHotRatio * (v - med);
                // A member of a hot pair or segment along a line through it (column, row or diagonal): its
                // fellow members are the ring's two neighbours on that line (1 px away in mono, 2 in a
                // mosaic). It counts when it stands above the 6 ring neighbours off the line, by the shape
                // margin over the lowest of the ring and the two same-colour samples 2 ring steps out
                // across the line: a segment is one pixel wide, so the background is flat across it; a still
                // optical feature that would pass the first part -- a point between two sites, two sites
                // flanking a peak, a thin line along the line -- falls off across it, and its far samples
                // are well below the ring (unit test: still moons on every site colour and between sites,
                // lines along a row, a column, a diagonal and between rows, PSF sigma 1.6; round 1's mono
                // rule -- above all but the two largest neighbours -- flagged the points and lines).
                // Only where the far samples are inside the frame.
                if (!alone && v - n[5] > minExcess)  // off any line, the largest is at least the third largest
                    for (int line = 0; line < 4 && !alone; ++line) {
                        static constexpr int kPair[4][2] = {{1, 6}, {3, 4}, {0, 7}, {2, 5}};
                        static constexpr int kAcross[4][2] = {{1, 0}, {0, 1}, {1, -1}, {1, 1}};
                        const int fx = 2 * d * kAcross[line][0], fy = 2 * d * kAcross[line][1];
                        if (x - std::abs(fx) < 0 || x + std::abs(fx) >= W || y - std::abs(fy) < 0 || y + std::abs(fy) >= height_)
                            continue;
                        double off = 0, low = 65535;
                        for (int q = 0; q < 8; ++q) {
                            low = std::min<double>(low, ring[q]);
                            if (q != kPair[line][0] && q != kPair[line][1]) off = std::max<double>(off, ring[q]);
                        }
                        low = std::min<double>(low, s[(static_cast<size_t>(y - fy) * W + (x - fx)) * C + c]);
                        low = std::min<double>(low, s[(static_cast<size_t>(y + fy) * W + (x + fx)) * C + c]);
                        const double e = v - off;
                        alone = e > minExcess && e > kHotRatio * (v - low);
                    }
                if (alone && isolated_[k] < 65535) ++isolated_[k];
            }
    ++frames_;
}

void HotPixelMap::Merge(const HotPixelMap& other) {
    if (other.isolated_.size() != isolated_.size()) throw std::invalid_argument("HotPixelMap::Merge: another capture");
    for (size_t k = 0; k < isolated_.size(); ++k)
        isolated_[k] = static_cast<uint16_t>(std::min<int>(65535, isolated_[k] + other.isolated_[k]));
    frames_ += other.frames_;
}

void HotPixelMap::Finish() {
    hot_.clear();
    if (frames_ < kMinHotFrames) return;
    const double need = kHotShare * static_cast<double>(frames_);
    for (size_t k = 0; k < isolated_.size(); ++k)
        if (isolated_[k] >= need) hot_.push_back(k);
}

void HotPixelMap::Repair(ser::Frame& frame) const {
    if (hot_.empty()) return;
    if (frame.width != width_ || frame.height != height_ || frame.channels != channels_ ||
        frame.samples.size() != isolated_.size())
        throw std::invalid_argument("HotPixelMap::Repair: the frame does not match the capture");
    const int d = step_, C = channels_, W = width_;
    // Neighbours are read from the frame as given: two hot sites next to each other (2 px apart in a
    // mosaic) are each repaired from the other's raw value at most once, which the median absorbs.
    std::vector<uint16_t> fixed;
    fixed.reserve(hot_.size());
    for (size_t k : hot_) {
        const int c = static_cast<int>(k % C);
        const size_t px = k / C;
        const int x = static_cast<int>(px % W), y = static_cast<int>(px / W);
        uint16_t n[8];
        int m = 0;
        for (int j = -1; j <= 1; ++j)
            for (int i = -1; i <= 1; ++i) {
                if (!i && !j) continue;
                n[m++] = frame.samples[(static_cast<size_t>(y + j * d) * W + (x + i * d)) * C + c];
            }
        std::nth_element(n, n + 4, n + 8);
        fixed.push_back(static_cast<uint16_t>((n[4] + *std::max_element(n, n + 4) + 1) / 2));
    }
    for (size_t i = 0; i < hot_.size(); ++i) frame.samples[hot_[i]] = fixed[i];
}

// ---- Accumulator -----------------------------------------------------------------------------------

Accumulator::Accumulator(const RegionPlan& plan, const ser::FloatImage& reference, const ser::FloatImage& residualSigma,
                         const ser::FileInfo& info)
    : plan_(plan) {
    CheckLuma(reference, "Accumulator");
    CheckLuma(residualSigma, "Accumulator");
    if (plan.frames.empty()) throw std::invalid_argument("Accumulator: the plan has no frames");
    width_ = info.header.width;
    height_ = info.header.height;
    const bool bayer = ser::IsBayer(info.colorId);
    const bool rgb = info.colorId == ser::ColorId::RGB || info.colorId == ser::ColorId::BGR;
    channels_ = bayer || rgb ? 3 : 1;
    scale_ = static_cast<int>(LumaScale(info));
    lumaW_ = width_ / scale_;
    lumaH_ = height_ / scale_;
    if (lumaW_ < 1 || lumaH_ < 1) throw std::invalid_argument("Accumulator: empty frame");
    if (reference.width != lumaW_ || reference.height != lumaH_ || residualSigma.width != lumaW_ ||
        residualSigma.height != lumaH_ || plan.layout.width != lumaW_ || plan.layout.height != lumaH_)
        throw std::invalid_argument("Accumulator: the reference, residual scale and plan must be the capture's luma size (" +
                                    std::to_string(lumaW_) + "x" + std::to_string(lumaH_) + ")");
    if (bayer) {
        lumaWeights_[0] = 0.25;
        lumaWeights_[1] = 0.5;
        lumaWeights_[2] = 0.25;
    } else if (rgb) {
        lumaWeights_[0] = lumaWeights_[1] = lumaWeights_[2] = 1.0 / 3;
    } else {
        lumaWeights_[0] = 1;
        lumaWeights_[1] = lumaWeights_[2] = 0;
    }
    refLuma_ = reference.data;
    sigma_ = residualSigma.data;
    sum_.assign(static_cast<size_t>(width_) * height_ * channels_, 0.f);
    weight_.assign(static_cast<size_t>(width_) * height_, 0.f);
    sumRej_.assign(sum_.size(), 0.f);
    weightRej_.assign(weight_.size(), 0.f);
    blendRegion_.resize(plan_.RegionCount());
    for (size_t r = 0; r < plan_.RegionCount(); ++r) blendRegion_[r] = plan_.detail[r].kept > 0 ? 1 : 0;
    const int locks = (lumaH_ + kLockRows - 1) / kLockRows;
    for (int b = 0; b < locks; ++b) bandLocks_.push_back(std::make_unique<std::mutex>());
    addedFlags_.assign(plan_.frames.size(), 0);
    cellOfX_.resize(static_cast<size_t>(width_));
    for (int x = 0; x < width_; ++x) cellOfX_[static_cast<size_t>(x)] = std::min(x / scale_, lumaW_ - 1);
}

Accumulator::~Accumulator() = default;

bool Accumulator::Add(int64_t frameIndex, ser::FloatImage frame) {
    if (frame.width != width_ || frame.height != height_ || frame.channels != channels_ ||
        frame.data.size() != static_cast<size_t>(width_) * height_ * channels_)
        throw std::invalid_argument("Accumulator::Add: expected a " + std::to_string(width_) + "x" +
                                    std::to_string(height_) + " frame with " + std::to_string(channels_) +
                                    " channels");
    const int64_t pos = plan_.Find(frameIndex);
    if (pos < 0 || !plan_.stacked[static_cast<size_t>(pos)]) return false;
    {
        std::lock_guard<std::mutex> lock(addedLock_);
        if (addedFlags_[static_cast<size_t>(pos)])
            throw std::logic_error("Accumulator::Add: frame " + std::to_string(frameIndex) + " added twice");
        addedFlags_[static_cast<size_t>(pos)] = 1;
    }
    Prefilter(frame);

    // This frame's per-region data: local shift, usable, and its weight in each region's stack.
    const size_t nr = plan_.RegionCount();
    const RegionLayout& lay = plan_.layout;
    std::vector<float> ldx(nr), ldy(nr);
    std::vector<uint8_t> usable(nr);
    std::vector<double> mine(nr, 0.0);
    for (size_t r = 0; r < nr; ++r) {
        const size_t k = static_cast<size_t>(pos) * nr + r;
        ldx[r] = plan_.dx[k];
        ldy[r] = plan_.dy[k];
        usable[r] = plan_.flags[k] & RegionPlan::kShiftUsable ? 1 : 0;
        if (plan_.flags[k] & RegionPlan::kKept) mine[r] = 1.0 / static_cast<double>(plan_.detail[r].kept);
    }
    const double fallback = 1.0 / static_cast<double>(plan_.stackedCount);
    const double gain = plan_.gain[static_cast<size_t>(pos)];
    const double invGain = 1 / gain;
    const double s = scale_;
    const double gx = plan_.global[static_cast<size_t>(pos)].dx * s, gy = plan_.global[static_cast<size_t>(pos)].dy * s;
    const int C = channels_, W = width_, H = height_;

    // alpha (this frame's blend weight) and the warp offset at full-resolution point (x, y), exact.
    auto weightAndWarp = [&](const AxisWeight& ax, const AxisWeight& ay, double& alpha, double& ox, double& oy) {
        double cover = 0, a = 0;
        for (int b = 0; b < 2; ++b) {
            const int j = ay.i0 + b;
            const double wy = b ? ay.w1 : ay.w0;
            if (j < 0 || j >= lay.ny || wy == 0) continue;
            for (int c = 0; c < 2; ++c) {
                const int i = ax.i0 + c;
                const double wx = c ? ax.w1 : ax.w0;
                if (i < 0 || i >= lay.nx || wx == 0) continue;
                const int r = lay.node[static_cast<size_t>(j) * lay.nx + i];
                if (r < 0 || !blendRegion_[static_cast<size_t>(r)]) continue;
                const double w = wx * wy;
                cover += w;
                a += w * mine[static_cast<size_t>(r)];
            }
        }
        alpha = a + std::max(0.0, 1 - cover) * fallback;
        double ldxp = 0, ldyp = 0;
        if (nr > 0) LocalWarpAt(lay, ax, ay, ldx.data(), ldy.data(), usable.data(), ldxp, ldyp);
        ox = gx + s * ldxp;
        oy = gy + s * ldyp;
    };
    // Both vary smoothly (sin^2 blends over regions of >= 32 luma px), so they are evaluated exactly on
    // a grid of kWarpGrid px and interpolated bilinearly in between: linear, so the frames' weights
    // still sum to 1 everywhere; the warp's interpolation error is under 0.02 px.
    const int G = kWarpGrid;
    const int gw = (W + G - 1) / G + 1, gh = (H + G - 1) / G + 1;
    std::vector<float> gA(static_cast<size_t>(gw) * gh), gX(gA.size()), gY(gA.size());
    std::vector<AxisWeight> colW(static_cast<size_t>(gw)), rowW(static_cast<size_t>(gh));
    for (int i = 0; i < gw; ++i) colW[static_cast<size_t>(i)] = AxisWeightAt(lay, false, (static_cast<double>(i) * G + 0.5) / s - 0.5);
    for (int j = 0; j < gh; ++j) rowW[static_cast<size_t>(j)] = AxisWeightAt(lay, true, (static_cast<double>(j) * G + 0.5) / s - 0.5);
    for (int j = 0; j < gh; ++j)
        for (int i = 0; i < gw; ++i) {
            double a, ox, oy;
            weightAndWarp(colW[static_cast<size_t>(i)], rowW[static_cast<size_t>(j)], a, ox, oy);
            const size_t k = static_cast<size_t>(j) * gw + i;
            gA[k] = static_cast<float>(a);
            gX[k] = static_cast<float>(ox);
            gY[k] = static_cast<float>(oy);
        }
    std::vector<float> rA(static_cast<size_t>(gw)), rX(rA.size()), rY(rA.size());
    const float* fr = frame.data.data();
    const float ig = static_cast<float>(invGain);

    // Luma rows are processed in order through a ring of 2 kRejectReach + 1 rows (samples, weights,
    // deviations, dilated outlier masks): a row is added once the rows kRejectReach below it are
    // judged. The working set is a few rows -- it stays in cache, which is what lets many concurrent
    // Adds scale. Each frame starts at its own row (two segments, each with its own judged margin), so
    // concurrent Adds spread over the locks.
    auto fy0 = [&](int lr) { return lr * scale_; };
    auto fy1 = [&](int lr) { return lr + 1 >= lumaH_ ? H : (lr + 1) * scale_; };
    const int R = kRejectReach, NR = 2 * R + 1;
    const int maxRows = scale_ + (H - lumaH_ * scale_);  // full-resolution rows of the last luma row
    const size_t slotPx = static_cast<size_t>(maxRows) * W, lw = static_cast<size_t>(lumaW_);
    std::vector<float> smp(NR * slotPx * C), alp(NR * slotPx), dev(NR * lw);
    std::vector<uint8_t> out(NR * slotPx), cellAny(NR * lw), near(NR * lw), reach(NR * lw), strong(lw);
    int64_t rejected = 0;

    auto sampleRow = [&](int y, float* srow, float* arow, uint8_t* orow) {
        const int gj = y / G;
        const float ty = static_cast<float>(y - gj * G) / G;
        for (int i = 0; i < gw; ++i) {
            const size_t k0 = static_cast<size_t>(gj) * gw + i, k1 = k0 + gw;
            rA[i] = gA[k0] + ty * (gA[k1] - gA[k0]);
            rX[i] = gX[k0] + ty * (gX[k1] - gX[k0]);
            rY[i] = gY[k0] + ty * (gY[k1] - gY[k0]);
        }
        for (int x = 0; x < W; ++x) {
            const int gi = x / G;
            const float tx = static_cast<float>(x - gi * G) / G;
            const float a = rA[gi] + tx * (rA[gi + 1] - rA[gi]);
            arow[x] = a;
            if (!(a > 0)) continue;
            const float sx = static_cast<float>(x) + rX[gi] + tx * (rX[gi + 1] - rX[gi]);
            const float sy = static_cast<float>(y) + rY[gi] + tx * (rY[gi + 1] - rY[gi]);
            orow[x] = sx < 0 || sy < 0 || sx > W - 1 || sy > H - 1;
            // floor without a libm call (the baseline x86-64 target has no rounding instruction).
            const int ix = FloorInt(sx), iy = FloorInt(sy);
            // Exact weights: a table at 1/64 px moved the stack by up to 6e-4 (measured), these by none.
            float wxa[4], wya[4];
            BSplineWeights(sx - ix, wxa);
            BSplineWeights(sy - iy, wya);
            const float* wx = wxa;
            const float* wy = wya;
            float* o = srow + static_cast<size_t>(x) * C;
            if (ix >= 1 && iy >= 1 && ix + 2 < W && iy + 2 < H) {
                const float* p0 = fr + (static_cast<size_t>(iy - 1) * W + (ix - 1)) * C;
                const size_t rowLen = static_cast<size_t>(W) * C;
                if (C == 1) {
                    float acc = 0;
                    for (int j = 0; j < 4; ++j) {
                        const float* p = p0 + j * rowLen;
                        acc += wy[j] * (wx[0] * p[0] + wx[1] * p[1] + wx[2] * p[2] + wx[3] * p[3]);
                    }
                    o[0] = acc * ig;
                } else {
                    // 12 lanes (4 taps x 3 channels) a row at a time: the compiler keeps them in
                    // three 4-wide vectors; the taps are summed per channel at the end.
                    float w12[12], t[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
                    for (int q = 0; q < 12; ++q) w12[q] = wx[q / 3];
                    for (int j = 0; j < 4; ++j) {
                        const float* p = p0 + j * rowLen;
                        for (int q = 0; q < 12; ++q) t[q] += wy[j] * (w12[q] * p[q]);
                    }
                    for (int c = 0; c < 3; ++c) o[c] = (t[c] + t[3 + c] + t[6 + c] + t[9 + c]) * ig;
                }
            } else {
                float acc[3] = {0, 0, 0};
                for (int j = 0; j < 4; ++j) {
                    const float* row = fr + static_cast<size_t>(Mirror(iy - 1 + j, H)) * W * C;
                    for (int i = 0; i < 4; ++i) {
                        const float* p = row + static_cast<size_t>(Mirror(ix - 1 + i, W)) * C;
                        for (int c = 0; c < C; ++c) acc[c] += wx[i] * wy[j] * p[c];
                    }
                }
                for (int c = 0; c < C; ++c) o[c] = acc[c] * ig;
            }
        }
    };
    // Samples luma row lr into its slot and judges its cells: deviation in residual scales, and the
    // row's outlier mask dilated along the row by kRejectGrow and by kRejectReach.
    auto judgeRow = [&](int lr) {
        const size_t slot = static_cast<size_t>(lr % NR);
        float* sbase = smp.data() + slot * slotPx * C;
        float* abase = alp.data() + slot * slotPx;
        uint8_t* obase = out.data() + slot * slotPx;
        const int ya = fy0(lr), yb = fy1(lr);
        for (int y = ya; y < yb; ++y) {
            const size_t r = static_cast<size_t>(y - ya) * W;
            sampleRow(y, sbase + r * C, abase + r, obase + r);
        }
        float* d = dev.data() + slot * lw;
        uint8_t* any = cellAny.data() + slot * lw;
        for (int lc = 0; lc < lumaW_; ++lc) {
            const int xa = lc * scale_, xb = lc + 1 >= lumaW_ ? W : (lc + 1) * scale_;
            float luma = 0;
            int n = 0;
            for (int y = ya; y < yb; ++y)
                for (int x = xa; x < xb; ++x) {
                    const size_t k = static_cast<size_t>(y - ya) * W + x;
                    if (!(abase[k] > 0) || obase[k]) continue;
                    float v = 0;
                    for (int c = 0; c < C; ++c) v += static_cast<float>(lumaWeights_[c]) * sbase[k * C + c];
                    luma += v;
                    ++n;
                }
            any[lc] = n > 0;
            const size_t li = static_cast<size_t>(lr) * lw + lc;
            d[lc] = n > 0 ? std::fabs(luma / n - refLuma_[li]) / sigma_[li] : 0.f;
            strong[lc] = d[lc] > kRejectSigmas;
        }
        uint8_t* nr2 = near.data() + slot * lw;
        uint8_t* rr4 = reach.data() + slot * lw;
        std::fill(nr2, nr2 + lw, 0);
        std::fill(rr4, rr4 + lw, 0);
        for (int lc = 0; lc < lumaW_; ++lc)
            if (strong[lc]) {
                for (int k = std::max(0, lc - kRejectGrow); k <= std::min(lumaW_ - 1, lc + kRejectGrow); ++k) nr2[k] = 1;
                for (int k = std::max(0, lc - R); k <= std::min(lumaW_ - 1, lc + R); ++k) rr4[k] = 1;
            }
    };
    // Rejected: within kRejectGrow cells of a cell beyond kRejectSigmas, or beyond kGrowSigmas and
    // within kRejectReach of one (a trail's wings, the interpolation's ringing next to a sharp trail,
    // the demosaic's spread of a defect); then the row is added (rejected samples aside, see Finish).
    std::vector<uint8_t> grown(lw);
    auto finishRow = [&](int f, int j0, int j1) {  // judged rows [j0, j1)
        const size_t slot = static_cast<size_t>(f % NR);
        const float* d = dev.data() + slot * lw;
        const uint8_t* any = cellAny.data() + slot * lw;
        for (int lc = 0; lc < lumaW_; ++lc) {
            bool g = false;
            for (int j = std::max(j0, f - kRejectGrow); j <= std::min(j1 - 1, f + kRejectGrow) && !g; ++j)
                g = near[static_cast<size_t>(j % NR) * lw + lc] != 0;
            if (!g && d[lc] > kGrowSigmas)
                for (int j = std::max(j0, f - R); j <= std::min(j1 - 1, f + R) && !g; ++j)
                    g = reach[static_cast<size_t>(j % NR) * lw + lc] != 0;
            grown[static_cast<size_t>(lc)] = g;
            rejected += g && any[lc];
        }
        const float* sbase = smp.data() + slot * slotPx * C;
        const float* abase = alp.data() + slot * slotPx;
        const uint8_t* obase = out.data() + slot * slotPx;
        const int ya = fy0(f), yb = fy1(f);
        std::lock_guard<std::mutex> lock(*bandLocks_[static_cast<size_t>(f / kLockRows)]);
        for (int y = ya; y < yb; ++y) {
            const size_t kr = static_cast<size_t>(y - ya) * W, orow = static_cast<size_t>(y) * W;
            for (int x = 0; x < W; ++x) {
                const size_t k = kr + x;
                if (!(abase[k] > 0)) continue;
                const float wgt = abase[k] * (obase[k] ? static_cast<float>(kOutsideWeight) : 1.0f);
                const size_t o = orow + x;
                if (grown[static_cast<size_t>(cellOfX_[static_cast<size_t>(x)])]) {
                    weightRej_[o] += wgt;
                    for (int c = 0; c < C; ++c) sumRej_[o * C + c] += wgt * sbase[k * C + c];
                    continue;
                }
                weight_[o] += wgt;
                for (int c = 0; c < C; ++c) sum_[o * C + c] += wgt * sbase[k * C + c];
            }
        }
    };
    auto segment = [&](int a, int b) {  // finish luma rows [a, b)
        if (a >= b) return;
        const int j0 = std::max(0, a - R), j1 = std::min(lumaH_, b + R);
        for (int lr = j0; lr < j1; ++lr) {
            judgeRow(lr);
            const int f = lr - R;
            if (f >= a && f < b) finishRow(f, j0, j1);
        }
        for (int f = std::max(a, j1 - R); f < b; ++f) finishRow(f, j0, j1);
    };
    const int start = static_cast<int>((pos * 97) % lumaH_);
    segment(start, lumaH_);
    segment(0, start);
    rejected_ += rejected;
    ++added_;
    return true;
}

ser::FloatImage Accumulator::Finish() const {
    if (added_.load() == 0) throw std::logic_error("Accumulator::Finish: no frame added");
    ser::FloatImage out{width_, height_, channels_, std::vector<float>(sum_.size(), 0.f)};
    // Chunks are locked one at a time, so a Finish during Adds sees each chunk consistently.
    for (size_t b = 0; b < bandLocks_.size(); ++b) {
        std::lock_guard<std::mutex> lock(*bandLocks_[b]);
        const int l0 = static_cast<int>(b) * kLockRows, l1 = std::min(lumaH_, l0 + kLockRows);
        const int y0 = l0 * scale_, y1 = l1 >= lumaH_ ? height_ : l1 * scale_;
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < width_; ++x) {
                const size_t o = static_cast<size_t>(y) * width_ + x;
                // Never a hole: where rejection took most of a pixel's weight (it kept under kMinKeptWeight
                // of the weight its samples brought), the rejected samples are blended back in, fully when
                // nothing was kept. The share, not the absolute weight: a border band that only a few
                // frames reach in-bounds (drift) holds a small weight however few samples were rejected
                // there, and an absolute threshold blended a rejected trail back into it.
                const double all = static_cast<double>(weight_[o]) + weightRej_[o];
                if (!(all > 0)) continue;
                const double lam = std::max(0.0, 1 - weight_[o] / all / kMinKeptWeight);
                const double wt = weight_[o] + lam * weightRej_[o];
                if (!(wt > 0)) continue;
                for (int c = 0; c < channels_; ++c)
                    out.data[o * channels_ + c] =
                        static_cast<float>((sum_[o * channels_ + c] + lam * sumRej_[o * channels_ + c]) / wt);
            }
    }
    return out;
}

size_t Accumulator::BytesHeld() const {
    size_t b = sizeof(*this);
    b += (sum_.capacity() + weight_.capacity()) * sizeof(float);
    b += (sumRej_.capacity() + weightRej_.capacity()) * sizeof(float);
    b += refLuma_.capacity() * sizeof(float) + sigma_.capacity() * sizeof(float);
    b += blendRegion_.capacity() * sizeof(int) + addedFlags_.capacity();
    b += bandLocks_.capacity() * (sizeof(std::unique_ptr<std::mutex>) + sizeof(std::mutex));
    b += plan_.frames.capacity() * sizeof(int64_t) + plan_.global.capacity() * sizeof(Shift) +
         plan_.gain.capacity() * sizeof(float);
    b += (plan_.dx.capacity() + plan_.dy.capacity()) * sizeof(float) + plan_.flags.capacity();
    b += plan_.detail.capacity() * sizeof(RegionDetail);
    b += plan_.layout.regions.capacity() * sizeof(Region) + plan_.layout.node.capacity() * sizeof(int);
    return b;
}

}  // namespace planetary::stack
