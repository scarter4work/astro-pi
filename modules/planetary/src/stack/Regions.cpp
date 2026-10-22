#include "Regions.h"

#include "Parallel.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <string>

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

// A multiple of 8 nearest to x, within the region size limits.
int RegionSize(double x) {
    return std::clamp(8 * static_cast<int>(std::lround(x / 8)), kMinRegionSize, kMaxRegionSize);
}

// One pyramid level down: [1 3 3 1] / 8 in each axis around (2i + 0.5, 2j + 0.5), then every other
// pixel, so level-l pixel i sits at level-0 position (i + 0.5) 2^l - 0.5, as with a 2 x 2 box, but
// with far less aliasing (response at the new Nyquist cos^3(pi / 4) = 0.35 against the box's 0.71:
// a fine lunar texture would otherwise fold into false coarse structure the search locks onto).
// Edges clamped; odd trailing row / column dropped; at least 1 x 1. `out` is resized.
void Halve(const float* p, int w, int h, ser::FloatImage& out, std::vector<float>& tmp) {
    const int hw = std::max(1, w / 2), hh = std::max(1, h / 2);
    out.width = hw;
    out.height = hh;
    out.channels = 1;
    out.data.resize(static_cast<size_t>(hw) * hh);
    tmp.resize(static_cast<size_t>(hw) * h);
    for (int y = 0; y < h; ++y) {  // horizontal
        const float* r = p + static_cast<size_t>(y) * w;
        float* t = tmp.data() + static_cast<size_t>(y) * hw;
        for (int x = 0; x < hw; ++x) {
            const int c = 2 * x;
            if (c >= 1 && c + 2 < w) {
                t[x] = 0.125f * (r[c - 1] + 3 * (r[c] + r[c + 1]) + r[c + 2]);
            } else {
                auto at = [&](int i) { return r[std::clamp(i, 0, w - 1)]; };
                t[x] = 0.125f * (at(c - 1) + 3 * (at(c) + at(c + 1)) + at(c + 2));
            }
        }
    }
    for (int y = 0; y < hh; ++y) {  // vertical
        const int c = 2 * y;
        const float* a = tmp.data() + static_cast<size_t>(std::clamp(c - 1, 0, h - 1)) * hw;
        const float* b = tmp.data() + static_cast<size_t>(std::clamp(c, 0, h - 1)) * hw;
        const float* d = tmp.data() + static_cast<size_t>(std::clamp(c + 1, 0, h - 1)) * hw;
        const float* e = tmp.data() + static_cast<size_t>(std::clamp(c + 2, 0, h - 1)) * hw;
        float* o = out.data.data() + static_cast<size_t>(y) * hw;
        for (int x = 0; x < hw; ++x) o[x] = 0.125f * (a[x] + 3 * (b[x] + d[x]) + e[x]);
    }
}

// Catmull-Rom weights for fractional offset t in [0, 1).
void CubicWeights(double t, double w[4]) {
    const double t2 = t * t, t3 = t2 * t;
    w[0] = 0.5 * (-t3 + 2 * t2 - t);
    w[1] = 0.5 * (3 * t3 - 5 * t2 + 2);
    w[2] = 0.5 * (-3 * t3 + 4 * t2 + t);
    w[3] = 0.5 * (t3 - t2);
}

// out[j * w + i] = image (iw x ih) sampled at (x0 + i + tx, y0 + j + ty), separable Catmull-Rom,
// coordinates clamped to the image. `tmp` is scratch.
void SampleWindow(const float* img, int iw, int ih, int x0, int y0, int w, int h, double tx, double ty,
                  std::vector<float>& out, std::vector<float>& tmp) {
    const double fx0 = std::floor(tx), fy0 = std::floor(ty);
    const int ix = static_cast<int>(fx0), iy = static_cast<int>(fy0);
    double wxd[4], wyd[4];
    CubicWeights(tx - fx0, wxd);
    CubicWeights(ty - fy0, wyd);
    const float wx[4] = {static_cast<float>(wxd[0]), static_cast<float>(wxd[1]), static_cast<float>(wxd[2]),
                         static_cast<float>(wxd[3])};
    const float wy[4] = {static_cast<float>(wyd[0]), static_cast<float>(wyd[1]), static_cast<float>(wyd[2]),
                         static_cast<float>(wyd[3])};
    out.resize(static_cast<size_t>(w) * h);
    tmp.resize(static_cast<size_t>(w) * (h + 3));
    const int c0 = x0 + ix - 1;  // first source column
    const bool inside = c0 >= 0 && c0 + w + 3 <= iw;
    for (int j = 0; j < h + 3; ++j) {
        const int r = std::clamp(y0 + iy - 1 + j, 0, ih - 1);
        const float* row = img + static_cast<size_t>(r) * iw;
        float* t = tmp.data() + static_cast<size_t>(j) * w;
        if (inside) {
            const float* s = row + c0;
            for (int i = 0; i < w; ++i) t[i] = wx[0] * s[i] + wx[1] * s[i + 1] + wx[2] * s[i + 2] + wx[3] * s[i + 3];
        } else {
            for (int i = 0; i < w; ++i) {
                float a = 0;
                for (int k = 0; k < 4; ++k) a += wx[k] * row[std::clamp(c0 + i + k, 0, iw - 1)];
                t[i] = a;
            }
        }
    }
    for (int j = 0; j < h; ++j) {
        const float* t0 = tmp.data() + static_cast<size_t>(j) * w;
        float* o = out.data() + static_cast<size_t>(j) * w;
        for (int i = 0; i < w; ++i) o[i] = wy[0] * t0[i] + wy[1] * t0[i + w] + wy[2] * t0[i + 2 * w] + wy[3] * t0[i + 3 * w];
    }
}


// The same samples at only the window points (i, j) with i = i0, i0 + st, ... < i1 and likewise j
// (direct 4 x 4 Catmull-Rom: with a stride of 2, a quarter of the points at twice the taps each).
// Points must have their support inside the image (the caller's valid part). Other entries of `out`
// are left as they are.
void SampleWindowStrided(const float* img, int iw, int x0, int y0, int w, int h, double tx, double ty, int i0, int i1,
                         int j0, int j1, int st, std::vector<float>& out) {
    const double fx0 = std::floor(tx), fy0 = std::floor(ty);
    const int ix = static_cast<int>(fx0), iy = static_cast<int>(fy0);
    double wxd[4], wyd[4];
    CubicWeights(tx - fx0, wxd);
    CubicWeights(ty - fy0, wyd);
    const float wx[4] = {static_cast<float>(wxd[0]), static_cast<float>(wxd[1]), static_cast<float>(wxd[2]),
                         static_cast<float>(wxd[3])};
    const float wy[4] = {static_cast<float>(wyd[0]), static_cast<float>(wyd[1]), static_cast<float>(wyd[2]),
                         static_cast<float>(wyd[3])};
    out.resize(static_cast<size_t>(w) * h);
    for (int j = j0; j < j1; j += st) {
        const float* r0 = img + static_cast<size_t>(y0 + j + iy - 1) * iw + x0 + ix - 1;
        float* o = out.data() + static_cast<size_t>(j) * w;
        for (int i = i0; i < i1; i += st) {
            float acc = 0;
            for (int k = 0; k < 4; ++k) {
                const float* s = r0 + static_cast<size_t>(k) * iw + i;
                acc += wy[k] * (wx[0] * s[0] + wx[1] * s[1] + wx[2] * s[2] + wx[3] * s[3]);
            }
            o[i] = acc;
        }
    }
}
// Lucas-Kanade iterations per level (index = level) and the convergence step (luma px).
constexpr int kIterations[kLocalLevels] = {2, 3, 3};
constexpr double kConverged = 0.01;
// Ridge on the gradient matrix, as a share of its mean eigenvalue.
constexpr double kRidge = 0.02;
// Smallest window side on a pyramid level for that level to be used; the coarsest usable level is
// where the exhaustive search runs.
constexpr int kMinLevelWindow = 12;
// A search position must correlate at least this well to be taken (a window with nothing of the
// reference's scene within reach fails here).
constexpr double kMinSearchNcc = 0.2;
// Full-resolution windows at least this wide are refined on every other pixel (see Measure).
constexpr int kStrideWindow = 96;

// Normalised cross-correlation of a window (template tm, w x h) with the image's w x h block at
// integer (fx0, fy0), over the part of the block inside the image (both means taken over that part);
// -1 when under half of the block is inside or either side is flat there.
double NccAt(const float* img, int iw, int ih, int fx0, int fy0, int w, int h, const float* tm) {
    const int i0 = std::max(0, -fx0), i1 = std::min(w, iw - fx0);
    const int j0 = std::max(0, -fy0), j1 = std::min(h, ih - fy0);
    if (i1 <= i0 || j1 <= j0 || 2 * static_cast<int64_t>(i1 - i0) * (j1 - j0) < static_cast<int64_t>(w) * h) return -1;
    double s = 0, s2 = 0, st = 0, t = 0, t2 = 0;
    for (int j = j0; j < j1; ++j) {
        const float* tr = tm + static_cast<size_t>(j) * w;
        const float* row = img + static_cast<size_t>(fy0 + j) * iw + fx0;
        for (int i = i0; i < i1; ++i) {
            const double v = row[i], u = tr[i];
            s += v;
            s2 += v * v;
            st += v * u;
            t += u;
            t2 += u * u;
        }
    }
    const double n = static_cast<double>(i1 - i0) * (j1 - j0);
    const double vs = s2 - s * s / n, vt = t2 - t * t / n;
    return vs > 0 && vt > 0 ? (st - s * t / n) / std::sqrt(vs * vt) : -1;
}

// Noise of a row-major w x h image from the responses of M = [1 -2 1] x [1 -2 1] over its interior:
// white noise s gives responses of sd 6 s, and M is 0 on any linear-or-quadratic surface and on
// anything varying along one axis only. Trimmed second moment: the responses up to their 80th
// percentile (texture lives in the top 20%); for a normal response of sd 6 s, E[M^2 | |M| <= its 80th
// percentile] = 0.4377 (6 s)^2. (A median would read 0 on quantised low-noise data: most responses of
// a smooth 8-bit disc are exactly 0.) 0 for an image under 3 x 3.
double MaskNoise(const float* p, size_t rowLen, int w, int h) {
    // At most ~2^17 responses: every step-th row and column (still an unbiased sample of the noise).
    const int step = std::max(1, static_cast<int>(std::sqrt(static_cast<double>(w) * h / 131072.0)));
    if (w < 3 || h < 3) return 0;
    thread_local std::vector<float> r;
    r.clear();
    r.reserve(static_cast<size_t>((w - 2) / step + 1) * ((h - 2) / step + 1));
    for (int y = 1; y + 1 < h; y += step) {
        const float* a = p + static_cast<size_t>(y - 1) * rowLen;
        const float* b = a + rowLen;
        const float* c = b + rowLen;
        for (int x = 1; x + 1 < w; x += step) {
            const float ra = a[x - 1] - 2 * a[x] + a[x + 1];
            const float rb = b[x - 1] - 2 * b[x] + b[x + 1];
            const float rc = c[x - 1] - 2 * c[x] + c[x + 1];
            r.push_back(std::fabs(ra - 2 * rb + rc));
        }
    }
    const auto cut = r.begin() + static_cast<std::ptrdiff_t>((r.size() * 4) / 5);
    std::nth_element(r.begin(), cut, r.end());
    const float top = *cut;
    double sum = 0;
    size_t n = 0;
    for (float v : r)
        if (v <= top) {
            sum += static_cast<double>(v) * v;
            ++n;
        }
    return n > 0 ? std::sqrt(sum / static_cast<double>(n) / 0.4377) / 6 : 0.0;
}

// Pixel noise blind to texture over the rectangle [x0, x1) x [y0, y1): the smaller of MaskNoise on the
// pixels and twice MaskNoise on their 2 x 2 bins (white noise s gives s / 2 in a bin, so both read s).
// Each one misreads a different texture as noise: a raw Bayer mosaic read as mono carries its colour
// pattern at the Nyquist frequency in every frame -- the pixels' estimate sees it, a bin (one cell of
// the pattern) cancels it; texture at 4-8 px, coarse for the pixels, is fine for the bins. Taken over
// the target (the regions' extent), not the frame: an 8-bit sky clipped to black reads 0.
double TextureBlindNoise(const ser::FloatImage& im, int x0, int y0, int x1, int y1) {
    const int w = im.width;
    x0 = std::max(x0, 0);
    y0 = std::max(y0, 0);
    x1 = std::min(x1, im.width);
    y1 = std::min(y1, im.height);
    if (x1 - x0 < 3 || y1 - y0 < 3) return 0;
    const float* p = im.data.data();
    thread_local std::vector<float> bins;
    const int cw = x1 - x0, ch = y1 - y0;
    const float* crop = p + static_cast<size_t>(y0) * w + x0;
    const double pixels = MaskNoise(crop, static_cast<size_t>(w), cw, ch);
    const int bw = cw / 2, bh = ch / 2;
    bins.resize(static_cast<size_t>(bw) * bh);
    for (int j = 0; j < bh; ++j) {
        const float* a = crop + static_cast<size_t>(2 * j) * w;
        const float* b = a + w;
        float* o = bins.data() + static_cast<size_t>(j) * bw;
        for (int i = 0; i < bw; ++i) o[i] = 0.25f * (a[2 * i] + a[2 * i + 1] + b[2 * i] + b[2 * i + 1]);
    }
    if (bw < 3 || bh < 3) return pixels;
    return std::min(pixels, 2 * MaskNoise(bins.data(), static_cast<size_t>(bw), bw, bh));
}

}  // namespace

AxisWeight AxisWeightAt(const RegionLayout& layout, bool vertical, double u) {
    AxisWeight a;
    const int n = vertical ? layout.ny : layout.nx;
    const int o = vertical ? layout.originY : layout.originX;
    if (n <= 0 || layout.step <= 0) return a;
    const double t = u + 0.5 - o;  // from node 0's window start
    const int k = static_cast<int>(std::floor(t / layout.step));
    if (layout.flatEdges) {
        if (k <= 0) {
            a.i0 = 0;
            a.w0 = 1;
            return a;
        }
        if (k >= n) {
            a.i0 = n - 1;
            a.w0 = 1;
            return a;
        }
    } else if (k < 0 || k > n) {
        return a;
    }
    // Node k rises (sin^2 over its first half), node k - 1 falls (cos^2 over its second half).
    const double s = std::sin(M_PI * (t - static_cast<double>(k) * layout.step) / layout.size);
    const double rise = s * s;
    a.i0 = k - 1;
    a.w0 = k - 1 >= 0 ? 1 - rise : 0;
    a.w1 = k <= n - 1 ? rise : 0;
    return a;
}

RegionLayout PlaceRegions(const ser::FloatImage& reference, const FrameStats& refStats, bool fullFrame) {
    CheckLuma(reference, "PlaceRegions");
    const int w = reference.width, h = reference.height;
    RegionLayout lay;
    lay.width = w;
    lay.height = h;
    int bx0 = 0, by0 = 0, bx1 = w, by1 = h;
    if (fullFrame) {
        lay.size = RegionSize(static_cast<double>(std::min(w, h)) / kRegionsAcrossFrame);
    } else {
        if (!refStats.targetFound || refStats.bx1 <= refStats.bx0 || refStats.by1 <= refStats.by0)
            throw std::invalid_argument("PlaceRegions: a disc reference without a target");
        bx0 = refStats.bx0;
        by0 = refStats.by0;
        bx1 = refStats.bx1;
        by1 = refStats.by1;
        lay.size = RegionSize(static_cast<double>(std::min(bx1 - bx0, by1 - by0)) / kRegionsAcrossDisc);
    }
    lay.step = lay.size / 2;
    const int step = lay.step, size = lay.size;
    if (fullFrame) {
        lay.flatEdges = true;
        lay.originX = lay.originY = 0;
        lay.nx = w <= size ? 1 : (w - size + step - 1) / step + 1;
        lay.ny = h <= size ? 1 : (h - size + step - 1) / step + 1;
    } else {
        // The covered box: the target's box plus half a step; the grid starts a step before it, so the
        // box lies where the weights of neighbouring nodes sum to 1.
        const int margin = step / 2;
        const int cx0 = bx0 - margin, cy0 = by0 - margin, cx1 = bx1 + margin, cy1 = by1 + margin;
        lay.originX = cx0 - step;
        lay.originY = cy0 - step;
        lay.nx = (cx1 - cx0 + step - 1) / step + 1;
        lay.ny = (cy1 - cy0 + step - 1) / step + 1;
    }
    lay.node.assign(static_cast<size_t>(lay.nx) * lay.ny, -1);

    // Structure: the predicted error of one frame's local shift (kMaxShiftNoise), from the window's
    // gradient energy less the reference's own noise share (2 s^2 per forward difference).
    const double sRef = TextureBlindNoise(reference, bx0, by0, bx1, by1);
    const double sf = std::max(sRef * std::sqrt(static_cast<double>(kReferenceFrames)), kMinFrameNoise);
    const float* p = reference.data.data();
    for (int j = 0; j < lay.ny; ++j)
        for (int i = 0; i < lay.nx; ++i) {
            const int ox = lay.originX + i * step, oy = lay.originY + j * step;
            const int x0 = std::max(0, ox), y0 = std::max(0, oy);
            const int x1 = std::min(w, ox + size), y1 = std::min(h, oy + size);
            if (x1 - x0 < 2 || y1 - y0 < 2) continue;
            if (4 * static_cast<int64_t>(x1 - x0) * (y1 - y0) < static_cast<int64_t>(size) * size) continue;
            if (!fullFrame && (x1 <= bx0 || x0 >= bx1 || y1 <= by0 || y0 >= by1)) continue;
            double e = 0;
            int64_t pairs = 0;
            for (int y = y0; y < y1; ++y) {
                const float* row = p + static_cast<size_t>(y) * w;
                for (int x = x0; x < x1; ++x) {
                    if (x + 1 < x1) {
                        const double d = static_cast<double>(row[x + 1]) - row[x];
                        e += d * d;
                        ++pairs;
                    }
                    if (y + 1 < y1) {
                        const double d = static_cast<double>(row[x + w]) - row[x];
                        e += d * d;
                        ++pairs;
                    }
                }
            }
            const double signal = e - 2 * sRef * sRef * static_cast<double>(pairs);
            if (!(signal > 0)) continue;
            const double shiftNoise = 2 * sf / std::sqrt(signal);
            if (!(shiftNoise <= kMaxShiftNoise)) continue;
            lay.node[static_cast<size_t>(j) * lay.nx + i] = static_cast<int>(lay.regions.size());
            lay.regions.push_back({x0, y0, x1, y1, shiftNoise, i, j});
        }
    return lay;
}

RegionLayout PlaceRegions(const ser::FloatImage& reference, const FrameStats& refStats) {
    CheckLuma(reference, "PlaceRegions");
    const bool full = !refStats.targetFound || FillsFrame(refStats, reference.width, reference.height);
    return PlaceRegions(reference, refStats, full);
}

RegionMeasurer::RegionMeasurer(const ser::FloatImage& reference, const RegionLayout& layout) : layout_(layout) {
    CheckLuma(reference, "RegionMeasurer");
    if (reference.width != layout.width || reference.height != layout.height)
        throw std::invalid_argument("RegionMeasurer: the reference is not the layout's size");
    std::vector<ser::FloatImage> pyr(kLocalLevels);
    std::vector<float> tmp;
    pyr[0] = reference;
    for (int l = 1; l < kLocalLevels; ++l) Halve(pyr[l - 1].data.data(), pyr[l - 1].width, pyr[l - 1].height, pyr[l], tmp);
    windows_.resize(layout.regions.size());
    ex0_ = layout.width;
    ey0_ = layout.height;
    for (const Region& reg : layout.regions) {
        ex0_ = std::min(ex0_, reg.x0);
        ey0_ = std::min(ey0_, reg.y0);
        ex1_ = std::max(ex1_, reg.x1);
        ey1_ = std::max(ey1_, reg.y1);
    }
    for (size_t r = 0; r < layout.regions.size(); ++r) {
        const Region& reg = layout.regions[r];
        for (int l = 0; l < kLocalLevels; ++l) {
            const ser::FloatImage& im = pyr[l];
            Window win;
            win.x0 = std::min(reg.x0 >> l, im.width - 1);
            win.y0 = std::min(reg.y0 >> l, im.height - 1);
            win.w = std::min(im.width, std::max(win.x0 + 1, reg.x1 >> l)) - win.x0;
            win.h = std::min(im.height, std::max(win.y0 + 1, reg.y1 >> l)) - win.y0;
            if (l > 0 && (win.w < kMinLevelWindow || win.h < kMinLevelWindow)) break;  // too small: finer levels only
            const int aw = win.w + 2;
            win.t.resize(static_cast<size_t>(aw) * (win.h + 2));
            for (int j = 0; j < win.h + 2; ++j)
                for (int i = 0; i < aw; ++i) {
                    const int x = std::clamp(win.x0 - 1 + i, 0, im.width - 1), y = std::clamp(win.y0 - 1 + j, 0, im.height - 1);
                    win.t[static_cast<size_t>(j) * aw + i] = im.data[static_cast<size_t>(y) * im.width + x];
                }
            double sum = 0;
            for (int j = 1; j <= win.h; ++j)
                for (int i = 1; i <= win.w; ++i) sum += win.t[static_cast<size_t>(j) * aw + i];
            win.mean = sum / (static_cast<double>(win.w) * win.h);
            win.tm.resize(static_cast<size_t>(win.w) * win.h);
            for (int j = 1; j <= win.h; ++j)
                for (int i = 1; i <= win.w; ++i) {
                    const size_t k = static_cast<size_t>(j) * aw + i;
                    const double v = win.t[k] - win.mean;
                    const double gx = 0.5 * (static_cast<double>(win.t[k + 1]) - win.t[k - 1]);
                    const double gy = 0.5 * (static_cast<double>(win.t[k + aw]) - win.t[k - aw]);
                    win.tm[static_cast<size_t>(j - 1) * win.w + i - 1] = static_cast<float>(v);
                    win.energy += v * v;
                    win.hxx += gx * gx;
                    win.hxy += gx * gy;
                    win.hyy += gy * gy;
                }
            windows_[r].push_back(std::move(win));
        }
    }
}

FrameRegions RegionMeasurer::Measure(const ser::FloatImage& frameLuma, int64_t frameIndex, const Shift& global) const {
    CheckLuma(frameLuma, "RegionMeasurer::Measure");
    if (frameLuma.width != layout_.width || frameLuma.height != layout_.height)
        throw std::invalid_argument("RegionMeasurer::Measure: the frame is not the reference's size");
    FrameRegions out;
    out.frameIndex = frameIndex;
    out.global = global;
    out.regions.resize(layout_.regions.size());
    if (layout_.regions.empty()) return out;

    // Texture-blind (MeasureFrame's smoothest-tile estimate reads a lunar surface's texture as noise and
    // would zero the quality of every smooth region): s from TextureBlindNoise, the uncertainty of s^2
    // that of a median over ~n / 4 independent responses (neighbouring 3 x 3 masks overlap):
    // Var(s^2) = 4 s^2 Var(s) = 4 s^2 x 1.36 s^2 / (n / 4).
    NoiseEstimate noise;
    noise.sigma = TextureBlindNoise(frameLuma, ex0_, ey0_, ex1_, ey1_);
    noise.varSigma2 = 21.8 * std::pow(noise.sigma, 4) / std::max(1.0, static_cast<double>(ex1_ - ex0_) * (ey1_ - ey0_));
    thread_local std::vector<ser::FloatImage> pyr;
    thread_local std::vector<float> sample, tmp;
    pyr.resize(kLocalLevels);
    for (int l = 1; l < kLocalLevels; ++l) {
        const ser::FloatImage& prev = l == 1 ? frameLuma : pyr[static_cast<size_t>(l - 1)];
        Halve(prev.data.data(), prev.width, prev.height, pyr[static_cast<size_t>(l)], tmp);
    }
    auto level = [&](int l) -> const ser::FloatImage& { return l == 0 ? frameLuma : pyr[static_cast<size_t>(l)]; };

    double gainNum = 0, gainDen = 0, gainNumAll = 0, gainDenAll = 0;
    for (size_t r = 0; r < layout_.regions.size(); ++r) {
        const std::vector<Window>& wins = windows_[r];
        RegionMeasure& m = out.regions[r];
        const int top = static_cast<int>(wins.size()) - 1;

        // Exhaustive integer search on the coarsest usable level, around the global shift.
        double dx = 0, dy = 0;
        bool ok = true;
        {
            const Window& ws = wins[static_cast<size_t>(top)];
            const ser::FloatImage& im = level(top);
            const double sc = static_cast<double>(1 << top);
            const int bx = static_cast<int>(std::lround(global.dx / sc)), by = static_cast<int>(std::lround(global.dy / sc));
            const int reach = static_cast<int>(std::ceil((kLocalSearch + 0.5 * sc) / sc));
            double best = -2;
            int sx = 0, sy = 0;
            for (int j = -reach; j <= reach; ++j)
                for (int i = -reach; i <= reach; ++i) {
                    const double v = NccAt(im.data.data(), im.width, im.height, ws.x0 + bx + i, ws.y0 + by + j, ws.w,
                                           ws.h, ws.tm.data());
                    if (v > best) {
                        best = v;
                        sx = i;
                        sy = j;
                    }
                }
            if (!(best >= kMinSearchNcc)) ok = false;
            dx = (bx + sx) * sc - global.dx;
            dy = (by + sy) * sc - global.dy;
        }

        // Lucas-Kanade refinement from that level down to full resolution, over the part of the window
        // whose interpolation support lies inside the frame (a window at the frame edge, moved by the
        // drift, would otherwise read clamped edge pixels and be pulled toward them).
        double ncc = 0, sMean = 0, sSum = 0, tSum = 0;
        bool sampled = false;
        for (int l = top; l >= 0 && ok; --l) {
            const Window& win = wins[static_cast<size_t>(l)];
            const ser::FloatImage& im = level(l);
            const double scale = static_cast<double>(1 << l);
            const int aw = win.w + 2;
            for (int it = 0; it < kIterations[l]; ++it) {
                // Level-l pixel i shows level-0 position (i + 0.5) 2^l - 0.5; shifted by g + d that is
                // level-l position i + (g + d) / 2^l.
                const double tx = (global.dx + dx) / scale, ty = (global.dy + dy) / scale;
                // Valid: 1 <= x0 + i + tx < iw - 2 (the 4-tap support inside), likewise in y.
                const int i0 = std::clamp(static_cast<int>(std::ceil(1 - win.x0 - tx)), 0, win.w);
                const int i1 = std::clamp(static_cast<int>(std::ceil(im.width - 2 - win.x0 - tx)), i0, win.w);
                const int j0 = std::clamp(static_cast<int>(std::ceil(1 - win.y0 - ty)), 0, win.h);
                const int j1 = std::clamp(static_cast<int>(std::ceil(im.height - 2 - win.y0 - ty)), j0, win.h);
                if (2 * static_cast<int64_t>(i1 - i0) * (j1 - j0) < static_cast<int64_t>(win.w) * win.h) {
                    ok = false;  // the window has mostly left the frame
                    break;
                }
                // Full-resolution windows of 96 px and more are evaluated on every other pixel in each
                // axis: still >= 48 x 48 points (a shift error well under 0.05 px), a quarter of the cost.
                const int stride = l == 0 && win.w >= kStrideWindow && win.h >= kStrideWindow ? 2 : 1;
                const bool full = stride == 1 && i0 == 0 && j0 == 0 && i1 == win.w && j1 == win.h;
                if (stride == 1)
                    SampleWindow(im.data.data(), im.width, im.height, win.x0, win.y0, win.w, win.h, tx, ty, sample, tmp);
                else
                    SampleWindowStrided(im.data.data(), im.width, win.x0, win.y0, win.w, win.h, tx, ty, i0, i1, j0, j1, stride,
                                        sample);
                // Sums over the valid part: frame mean, template mean (0 over the full window), and,
                // when the part is not the full window, its own energy and gradient matrix.
                const double n = static_cast<double>((i1 - i0 + stride - 1) / stride) * ((j1 - j0 + stride - 1) / stride);
                double ss = 0, ts = 0;
                for (int j = j0; j < j1; j += stride) {
                    const float* sv = sample.data() + static_cast<size_t>(j) * win.w;
                    const float* tv = win.tm.data() + static_cast<size_t>(j) * win.w;
                    for (int i = i0; i < i1; i += stride) {
                        ss += sv[i];
                        ts += tv[i];
                    }
                }
                const double sm = ss / n, tm = full ? 0.0 : ts / n;
                double st = 0, s2 = 0, energy = win.energy, hxx = win.hxx, hxy = win.hxy, hyy = win.hyy;
                if (!full) energy = hxx = hxy = hyy = 0;
                for (int j = j0; j < j1; j += stride) {
                    const float* sv = sample.data() + static_cast<size_t>(j) * win.w;
                    const float* tv = win.tm.data() + static_cast<size_t>(j) * win.w;
                    const float* t = win.t.data() + static_cast<size_t>(j + 1) * aw + 1;
                    for (int i = i0; i < i1; i += stride) {
                        const double a = sv[i] - sm, b = tv[i] - tm;
                        st += a * b;
                        s2 += a * a;
                        if (!full) {
                            const double gx = 0.5 * (static_cast<double>(t[i + 1]) - t[i - 1]);
                            const double gy = 0.5 * (static_cast<double>(t[i + aw]) - t[i - aw]);
                            energy += b * b;
                            hxx += gx * gx;
                            hxy += gx * gy;
                            hyy += gy * gy;
                        }
                    }
                }
                const double a = energy > 0 ? st / energy : 0;
                if (l == 0) {
                    ncc = s2 > 0 && energy > 0 ? st / std::sqrt(s2 * energy) : 0;
                    sMean = sm;
                    sSum = ss;
                    tSum = (tm + win.mean) * n;
                    sampled = true;
                }
                if (!(a > 0.05)) {  // nothing of the reference's window there
                    ok = false;
                    break;
                }
                double bx = 0, by = 0;
                for (int j = j0; j < j1; j += stride) {
                    const float* sv = sample.data() + static_cast<size_t>(j) * win.w;
                    const float* tv = win.tm.data() + static_cast<size_t>(j) * win.w;
                    const float* t = win.t.data() + static_cast<size_t>(j + 1) * aw + 1;
                    for (int i = i0; i < i1; i += stride) {
                        const double e = (sv[i] - sm) - a * (tv[i] - tm);
                        bx += 0.5 * (static_cast<double>(t[i + 1]) - t[i - 1]) * e;
                        by += 0.5 * (static_cast<double>(t[i + aw]) - t[i - aw]) * e;
                    }
                }
                const double ridge = kRidge * 0.5 * (hxx + hyy);
                const double rxx = hxx + ridge, ryy = hyy + ridge;
                const double det = rxx * ryy - hxy * hxy;
                if (!(det > 0)) {
                    ok = false;
                    break;
                }
                const double ux = (ryy * bx - hxy * by) / det / a, uy = (rxx * by - hxy * bx) / det / a;
                dx -= ux * scale;
                dy -= uy * scale;
                if (!(std::fabs(dx) <= kMaxLocalShift && std::fabs(dy) <= kMaxLocalShift)) {
                    ok = false;
                    break;
                }
                if (std::hypot(ux, uy) * scale < kConverged) break;
            }
        }
        if (sampled) {
            gainNumAll += sSum;
            gainDenAll += tSum;
        }
        // Quality on the frame's own pixels at the window moved by the rounded total shift.
        const Region& reg = layout_.regions[r];
        const int ox = static_cast<int>(std::lround(global.dx + dx)), oy = static_cast<int>(std::lround(global.dy + dy));
        const int rx0 = std::max(0, reg.x0 + ox), ry0 = std::max(0, reg.y0 + oy);
        const int rx1 = std::min(frameLuma.width, reg.x1 + ox), ry1 = std::min(frameLuma.height, reg.y1 + oy);
        const bool inside = rx1 > rx0 && ry1 > ry0 &&
                            2 * static_cast<int64_t>(rx1 - rx0) * (ry1 - ry0) >=
                                static_cast<int64_t>(reg.x1 - reg.x0) * (reg.y1 - reg.y0);
        m.aligned = ok && inside;
        m.dx = static_cast<float>(ok ? dx : 0);
        m.dy = static_cast<float>(ok ? dy : 0);
        m.confidence = static_cast<float>(ok ? ncc : 0);
        if (m.aligned) {
            const QualityEstimate q =
                RegionQuality(frameLuma.data.data(), frameLuma.width, rx0, ry0, rx1, ry1, sMean, noise);
            m.quality = static_cast<float>(q.quality);
            m.qualitySigma = static_cast<float>(q.sigma);
            gainNum += sSum;
            gainDen += tSum;
        }
    }
    if (gainDen > 0 && gainNum > 0) out.gain = gainNum / gainDen;
    else if (gainDenAll > 0 && gainNumAll > 0) out.gain = gainNumAll / gainDenAll;
    return out;
}

FrameRegions MeasureRegions(const ser::FloatImage& reference, const RegionLayout& layout,
                            const ser::FloatImage& frameLuma, const Shift& global) {
    return RegionMeasurer(reference, layout).Measure(frameLuma, 0, global);
}

int64_t RegionPlan::Find(int64_t frameIndex) const {
    const auto it = std::lower_bound(frames.begin(), frames.end(), frameIndex);
    return it != frames.end() && *it == frameIndex ? static_cast<int64_t>(it - frames.begin()) : -1;
}

RegionPlan PlanRegions(const RegionLayout& layout, const std::vector<FrameRegions>& measured, int threads) {
    const size_t nr = layout.regions.size();
    std::vector<size_t> order(measured.size());
    std::iota(order.begin(), order.end(), size_t{0});
    for (const FrameRegions& m : measured)
        if (m.regions.size() != nr)
            throw std::invalid_argument("PlanRegions: frame " + std::to_string(m.frameIndex) + " has " +
                                        std::to_string(m.regions.size()) + " region measurements for " +
                                        std::to_string(nr) + " regions");
    std::sort(order.begin(), order.end(),
              [&](size_t a, size_t b) { return measured[a].frameIndex < measured[b].frameIndex; });
    for (size_t i = 1; i < order.size(); ++i)
        if (measured[order[i]].frameIndex == measured[order[i - 1]].frameIndex)
            throw std::invalid_argument("PlanRegions: frame " + std::to_string(measured[order[i]].frameIndex) +
                                        " measured twice");
    RegionPlan plan;
    plan.layout = layout;
    const size_t nf = order.size();
    plan.frames.reserve(nf);
    plan.dx.assign(nf * nr, 0.f);
    plan.dy.assign(nf * nr, 0.f);
    plan.flags.assign(nf * nr, 0);
    for (size_t f = 0; f < nf; ++f) {
        const FrameRegions& m = measured[order[f]];
        plan.frames.push_back(m.frameIndex);
        plan.global.push_back(m.global);
        plan.gain.push_back(std::isfinite(m.gain) && m.gain > 0 ? static_cast<float>(m.gain) : 1.f);
        for (size_t r = 0; r < nr; ++r) {
            plan.dx[f * nr + r] = m.regions[r].dx;
            plan.dy[f * nr + r] = m.regions[r].dy;
        }
    }
    plan.detail.resize(nr);
    // Regions are independent: each band of regions writes only its own detail and flag entries.
    ParallelBands(static_cast<int>(nr), threads, [&](int r0, int r1) {
        for (size_t r = static_cast<size_t>(r0); r < static_cast<size_t>(r1); ++r) {
            RegionDetail& d = plan.detail[r];
            std::vector<double> conf;
            for (size_t f = 0; f < nf; ++f) {
                const RegionMeasure& q = measured[order[f]].regions[r];
                if (q.aligned && std::isfinite(q.confidence)) conf.push_back(q.confidence);
            }
            if (conf.empty()) continue;
            std::sort(conf.begin(), conf.end());
            d.confidenceCut = kAlignRelative * conf[(conf.size() - 1) / 2];
            std::vector<double> q, sg;
            std::vector<size_t> pos;
            for (size_t f = 0; f < nf; ++f) {
                const RegionMeasure& m = measured[order[f]].regions[r];
                if (!(m.aligned && std::isfinite(m.confidence) && m.confidence >= d.confidenceCut)) continue;
                plan.flags[f * nr + r] |= RegionPlan::kShiftUsable;
                if (!(std::isfinite(m.quality) && m.quality > 0)) continue;
                q.push_back(m.quality);
                sg.push_back(m.qualitySigma);
                pos.push_back(f);
            }
            const auto n = static_cast<int64_t>(q.size());
            d.candidates = n;
            if (n == 0) continue;
            const int64_t need = std::max<int64_t>(1, std::min<int64_t>(kMinStackFrames, (n + 1) / 2));
            const int64_t floorCount =
                std::max<int64_t>(need, static_cast<int64_t>(std::ceil(std::sqrt(static_cast<double>(n)))));
            // No upper fence (Ruling P8): every candidate passed this region's alignment cut (and the
            // frame the global one, Select's suspects included once admitted); a trail or a hot pixel that
            // raises one frame's quality here is removed sample by sample in the Accumulator, not by
            // dropping the frame, and a garbage frame does not align.
            const DetailRanking rank = RankByDetail(q, sg, need, floorCount, false);
            std::vector<double> keptSigma;
            for (size_t k = 0; k < rank.keep; ++k) {
                const size_t i = rank.ranked[k];
                plan.flags[pos[i] * nr + r] |= RegionPlan::kKept;
                keptSigma.push_back(std::isfinite(sg[i]) ? sg[i] : 0.0);
            }
            d.kept = static_cast<int64_t>(rank.keep);
            d.bestQuality = rank.qRef;
            d.detailScale = rank.qRef > 0 ? 1 / rank.qRef : 0;
            if (!keptSigma.empty()) {
                std::nth_element(keptSigma.begin(), keptSigma.begin() + static_cast<std::ptrdiff_t>(keptSigma.size() / 2),
                                 keptSigma.end());
                d.bestQualitySigma = keptSigma[keptSigma.size() / 2];
            }
        }
    });
    for (size_t r = 0; r < nr; ++r)
        if (plan.detail[r].kept > 0 &&
            (plan.bestRegion < 0 || plan.detail[r].bestQuality > plan.detail[static_cast<size_t>(plan.bestRegion)].bestQuality))
            plan.bestRegion = static_cast<int>(r);
    plan.stacked.assign(nf, 0);
    for (size_t f = 0; f < nf; ++f)
        for (size_t r = 0; r < nr && !plan.stacked[f]; ++r) plan.stacked[f] = (plan.flags[f * nr + r] & RegionPlan::kKept) != 0;
    plan.stackedCount = std::count(plan.stacked.begin(), plan.stacked.end(), uint8_t{1});
    if (plan.stackedCount == 0) {
        std::fill(plan.stacked.begin(), plan.stacked.end(), uint8_t{1});
        plan.stackedCount = static_cast<int64_t>(nf);
    }
    return plan;
}

}  // namespace planetary::stack
