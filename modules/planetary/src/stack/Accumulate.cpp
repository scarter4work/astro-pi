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
constexpr int kBandRows = 64;

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

// Whole image (interleaved channels): rows, then columns -- the columns all at once, row by row, so
// the vertical pass streams through memory.
void Prefilter(ser::FloatImage& im) {
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
    step_ = ser::IsBayer(info.colorId) ? 2 : 1;
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
                uint16_t n[8];
                int m = 0, hi = 0;
                for (int j = -1; j <= 1; ++j)
                    for (int i = -1; i <= 1; ++i) {
                        if (!i && !j) continue;
                        n[m] = s[(static_cast<size_t>(y + j * d) * W + (x + i * d)) * C + c];
                        hi = std::max<int>(hi, n[m]);
                        ++m;
                    }
                const double excess = v - hi;
                if (!(excess > minExcess)) continue;
                std::nth_element(n, n + 4, n + 8);
                const double med = 0.5 * (n[4] + *std::max_element(n, n + 4));
                if (excess > kHotRatio * (v - med) && isolated_[k] < 65535) ++isolated_[k];
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
    sum_.assign(static_cast<size_t>(width_) * height_ * channels_, 0.0);
    weight_.assign(static_cast<size_t>(width_) * height_, 0.0);
    sumRej_.assign(sum_.size(), 0.f);
    weightRej_.assign(weight_.size(), 0.f);
    blendRegion_.resize(plan_.RegionCount());
    for (size_t r = 0; r < plan_.RegionCount(); ++r) blendRegion_[r] = plan_.detail[r].kept > 0 ? 1 : 0;
    const int bands = (lumaH_ + kBandRows - 1) / kBandRows;
    for (int b = 0; b < bands; ++b) bandLocks_.push_back(std::make_unique<std::mutex>());
    addedFlags_.assign(plan_.frames.size(), 0);
}

Accumulator::~Accumulator() = default;

bool Accumulator::Add(int64_t frameIndex, ser::FloatImage frame) {
    if (frame.width != width_ || frame.height != height_ || frame.channels != channels_ ||
        frame.data.size() != static_cast<size_t>(width_) * height_ * channels_)
        throw std::invalid_argument("Accumulator::Add: expected a " + std::to_string(width_) + "x" +
                                    std::to_string(height_) + " frame with " + std::to_string(channels_) +
                                    " channels");
    const int64_t pos = plan_.Find(frameIndex);
    if (pos < 0) return false;
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
    const double fallback = 1.0 / static_cast<double>(plan_.frames.size());
    const double gain = plan_.gain[static_cast<size_t>(pos)];
    const double invGain = 1 / gain;
    const double s = scale_;
    const double gx = plan_.global[static_cast<size_t>(pos)].dx * s, gy = plan_.global[static_cast<size_t>(pos)].dy * s;
    const int C = channels_, W = width_, H = height_;

    // alpha (this frame's blend weight) and the warp offset at full-resolution point (x, y), exact.
    auto weightAndWarp = [&](double x, double y, double& alpha, double& ox, double& oy) {
        const AxisWeight ax = AxisWeightAt(lay, false, (x + 0.5) / s - 0.5);
        const AxisWeight ay = AxisWeightAt(lay, true, (y + 0.5) / s - 0.5);
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
    for (int j = 0; j < gh; ++j)
        for (int i = 0; i < gw; ++i) {
            double a, ox, oy;
            weightAndWarp(static_cast<double>(i) * G, static_cast<double>(j) * G, a, ox, oy);
            const size_t k = static_cast<size_t>(j) * gw + i;
            gA[k] = static_cast<float>(a);
            gX[k] = static_cast<float>(ox);
            gY[k] = static_cast<float>(oy);
        }
    std::vector<float> rA(static_cast<size_t>(gw)), rX(rA.size()), rY(rA.size());
    const float* fr = frame.data.data();
    const float ig = static_cast<float>(invGain);

    // Band scratch: per full-resolution pixel of the band's luma rows plus kRejectReach above and
    // below: samples (C), alpha, outside flag; per luma cell its deviation in residual scales.
    const int bands = static_cast<int>(bandLocks_.size());
    std::vector<float> smp, dev;
    std::vector<double> alp;
    std::vector<uint8_t> out, grown, cellAny, strong, rowMax, nearStrong, reachStrong;
    int64_t rejected = 0;
    // Each frame starts at its own band, so concurrent Adds spread over the locks.
    for (int step = 0; step < bands; ++step) {
        const int band = static_cast<int>((pos + step) % bands);
        const int l0 = band * kBandRows, l1 = std::min(lumaH_, l0 + kBandRows);
        const int m0 = std::max(0, l0 - kRejectReach), m1 = std::min(lumaH_, l1 + kRejectReach);  // rows judged
        // Full-resolution rows of luma rows [a, b): [a s, b s), the last luma row taking the rest.
        auto fy0 = [&](int lr) { return lr * scale_; };
        auto fy1 = [&](int lr) { return lr + 1 >= lumaH_ ? H : (lr + 1) * scale_; };
        const int y0 = fy0(m0), y1 = fy1(m1 - 1);
        const size_t npx = static_cast<size_t>(y1 - y0) * W;
        smp.assign(npx * C, 0.f);
        alp.assign(npx, 0.0);
        out.assign(npx, 0);
        dev.assign(static_cast<size_t>(m1 - m0) * lumaW_, 0.f);
        cellAny.assign(dev.size(), 0);
        for (int y = y0; y < y1; ++y) {
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
                const size_t k = static_cast<size_t>(y - y0) * W + x;
                alp[k] = a;
                if (!(a > 0)) continue;
                const double sx = x + rX[gi] + tx * (rX[gi + 1] - rX[gi]);
                const double sy = y + rY[gi] + tx * (rY[gi + 1] - rY[gi]);
                out[k] = sx < 0 || sy < 0 || sx > W - 1 || sy > H - 1;
                const int ix = static_cast<int>(std::floor(sx)), iy = static_cast<int>(std::floor(sy));
                float wx[4], wy[4];
                BSplineWeights(static_cast<float>(sx - ix), wx);
                BSplineWeights(static_cast<float>(sy - iy), wy);
                float* o = smp.data() + k * C;
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
                        float acc[3] = {0, 0, 0};
                        for (int j = 0; j < 4; ++j) {
                            const float* p = p0 + j * rowLen;
                            for (int c = 0; c < 3; ++c)
                                acc[c] += wy[j] * (wx[0] * p[c] + wx[1] * p[3 + c] + wx[2] * p[6 + c] + wx[3] * p[9 + c]);
                        }
                        for (int c = 0; c < 3; ++c) o[c] = acc[c] * ig;
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
        }
        // Rejection per luma cell: the cell's mean luma against the reference's.
        for (int lr = m0; lr < m1; ++lr) {
            const int ya = fy0(lr), yb = fy1(lr);
            for (int lc = 0; lc < lumaW_; ++lc) {
                const int xa = lc * scale_, xb = lc + 1 >= lumaW_ ? W : (lc + 1) * scale_;
                double luma = 0;
                int n = 0;
                bool any = false;
                for (int y = ya; y < yb; ++y)
                    for (int x = xa; x < xb; ++x) {
                        const size_t k = static_cast<size_t>(y - y0) * W + x;
                        if (!(alp[k] > 0) || out[k]) continue;
                        any = true;
                        double v = 0;
                        for (int c = 0; c < C; ++c) v += lumaWeights_[c] * smp[k * C + c];
                        luma += v;
                        ++n;
                    }
                if (!any) continue;
                cellAny[static_cast<size_t>(lr - m0) * lumaW_ + lc] = 1;
                const size_t li = static_cast<size_t>(lr) * lumaW_ + lc;
                dev[static_cast<size_t>(lr - m0) * lumaW_ + lc] =
                    static_cast<float>(std::fabs(luma / n - refLuma_[li]) / sigma_[li]);
            }
        }
        // Rejected: within kRejectGrow cells of a cell beyond kRejectSigmas, or beyond kGrowSigmas and within
        // kRejectReach cells of one (a trail's wings, the demosaic's spread of a defect). Square
        // neighbourhoods by separable max filters over the judged rows.
        const int mr = m1 - m0;
        strong.assign(dev.size(), 0);
        for (size_t k = 0; k < dev.size(); ++k) strong[k] = dev[k] > kRejectSigmas;
        auto dilate = [&](int radius, std::vector<uint8_t>& dst) {
            rowMax.assign(dev.size(), 0);
            for (int j = 0; j < mr; ++j) {
                const uint8_t* s = strong.data() + static_cast<size_t>(j) * lumaW_;
                uint8_t* d = rowMax.data() + static_cast<size_t>(j) * lumaW_;
                for (int i = 0; i < lumaW_; ++i)
                    if (s[i])
                        for (int k = std::max(0, i - radius); k <= std::min(lumaW_ - 1, i + radius); ++k) d[k] = 1;
            }
            dst.assign(dev.size(), 0);
            for (int j = 0; j < mr; ++j) {
                const uint8_t* s = rowMax.data() + static_cast<size_t>(j) * lumaW_;
                for (int k = std::max(0, j - radius); k <= std::min(mr - 1, j + radius); ++k) {
                    uint8_t* d = dst.data() + static_cast<size_t>(k) * lumaW_;
                    for (int i = 0; i < lumaW_; ++i) d[i] |= s[i];
                }
            }
        };
        dilate(kRejectGrow, nearStrong);
        dilate(kRejectReach, reachStrong);
        grown.assign(static_cast<size_t>(l1 - l0) * lumaW_, 0);
        for (int lr = l0; lr < l1; ++lr)
            for (int lc = 0; lc < lumaW_; ++lc) {
                const size_t k = static_cast<size_t>(lr - m0) * lumaW_ + lc;
                grown[static_cast<size_t>(lr - l0) * lumaW_ + lc] =
                    nearStrong[k] || (dev[k] > kGrowSigmas && reachStrong[k]);
            }
        std::lock_guard<std::mutex> lock(*bandLocks_[static_cast<size_t>(band)]);
        for (int lr = l0; lr < l1; ++lr)
            for (int lc = 0; lc < lumaW_; ++lc)
                rejected += grown[static_cast<size_t>(lr - l0) * lumaW_ + lc] && cellAny[static_cast<size_t>(lr - m0) * lumaW_ + lc];
        for (int y = fy0(l0); y < fy1(l1 - 1); ++y) {
            const uint8_t* g = grown.data() + static_cast<size_t>(std::min(y / scale_, lumaH_ - 1) - l0) * lumaW_;
            const size_t kr = static_cast<size_t>(y - y0) * W, orow = static_cast<size_t>(y) * W;
            for (int x = 0; x < W; ++x) {
                const size_t k = kr + x;
                if (!(alp[k] > 0)) continue;
                const double wgt = alp[k] * (out[k] ? kOutsideWeight : 1.0);
                const size_t o = orow + x;
                if (g[std::min(x / scale_, lumaW_ - 1)]) {  // rejected: kept aside, see Finish
                    weightRej_[o] += static_cast<float>(wgt);
                    for (int c = 0; c < C; ++c) sumRej_[o * C + c] += static_cast<float>(wgt * smp[k * C + c]);
                    continue;
                }
                weight_[o] += wgt;
                for (int c = 0; c < C; ++c) sum_[o * C + c] += wgt * smp[k * C + c];
            }
        }
    }
    rejected_ += rejected;
    ++added_;
    return true;
}

ser::FloatImage Accumulator::Finish() const {
    if (added_.load() == 0) throw std::logic_error("Accumulator::Finish: no frame added");
    ser::FloatImage out{width_, height_, channels_, std::vector<float>(sum_.size(), 0.f)};
    // Bands are locked one at a time, so a Finish during Adds sees each band consistently.
    for (size_t b = 0; b < bandLocks_.size(); ++b) {
        std::lock_guard<std::mutex> lock(*bandLocks_[b]);
        const int l0 = static_cast<int>(b) * kBandRows, l1 = std::min(lumaH_, l0 + kBandRows);
        const int y0 = l0 * scale_, y1 = l1 >= lumaH_ ? height_ : l1 * scale_;
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < width_; ++x) {
                const size_t o = static_cast<size_t>(y) * width_ + x;
                // Never a hole: where rejection took most of a pixel's weight (under kMinKeptWeight of the
                // full 1), the rejected samples are blended back in, fully when nothing was kept.
                const double lam = std::max(0.0, 1 - weight_[o] / kMinKeptWeight);
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
    b += sum_.capacity() * sizeof(double) + weight_.capacity() * sizeof(double);
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
