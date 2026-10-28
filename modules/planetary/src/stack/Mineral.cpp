#include "Mineral.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <mutex>
#include <stdexcept>

#include "Measure.h"
#include "Parallel.h"

namespace planetary::stack {

namespace {

size_t Pixels(const ser::FloatImage& im) { return static_cast<size_t>(im.width) * static_cast<size_t>(im.height); }

void RequireRgb(const ser::FloatImage& rgb, const char* who) {
    if (rgb.channels != 3 || rgb.width <= 0 || rgb.height <= 0 || rgb.data.size() != Pixels(rgb) * 3)
        throw std::invalid_argument(std::string(who) + ": needs a 3-channel image whose data matches its size");
}

// Median of v (v is reordered). 0 for an empty vector.
double Median(std::vector<float>& v) {
    if (v.empty()) return 0;
    const auto mid = v.begin() + static_cast<long>(v.size() / 2);
    std::nth_element(v.begin(), mid, v.end());
    return *mid;
}

// Robust std: 1.4826 x the median absolute deviation from the median (v is reordered).
double MadStd(std::vector<float>& v) {
    if (v.size() < 2) return 0;
    const double med = Median(v);
    for (float& x : v) x = static_cast<float>(std::fabs(x - med));
    return 1.4826 * Median(v);
}

// Mean of the channels, one sample per pixel.
ser::FloatImage MeanOfChannels(const ser::FloatImage& rgb, int threads) {
    ser::FloatImage L;
    L.width = rgb.width;
    L.height = rgb.height;
    L.channels = 1;
    L.data.resize(Pixels(rgb));
    const int nc = rgb.channels;
    ParallelBands(rgb.height, threads, [&](int y0, int y1) {
        for (size_t i = static_cast<size_t>(y0) * rgb.width; i < static_cast<size_t>(y1) * rgb.width; ++i) {
            double s = 0;
            for (int c = 0; c < nc; ++c) s += rgb.data[i * nc + c];
            L.data[i] = static_cast<float>(s / nc);
        }
    });
    return L;
}

// 3 x 3 box mean of a 1-channel image, edges replicated.
ser::FloatImage Box3(const ser::FloatImage& L, int threads) {
    ser::FloatImage out = L;
    const int w = L.width, h = L.height;
    ParallelBands(h, threads, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x) {
                double s = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int yy = std::clamp(y + dy, 0, h - 1), xx = std::clamp(x + dx, 0, w - 1);
                        s += L.data[static_cast<size_t>(yy) * w + xx];
                    }
                out.data[static_cast<size_t>(y) * w + x] = static_cast<float>(s / 9);
            }
    });
    return out;
}

// Otsu's threshold over a 1024-bin histogram of v between its 0.1 and 99.9 percentiles.
double Otsu(const std::vector<float>& v) {
    std::vector<float> s(v);
    const size_t lo = s.size() / 1000, hi = s.size() - 1 - s.size() / 1000;
    std::nth_element(s.begin(), s.begin() + static_cast<long>(lo), s.end());
    const double a = s[lo];
    std::nth_element(s.begin(), s.begin() + static_cast<long>(hi), s.end());
    const double b = s[hi];
    if (!(b > a)) return a;
    constexpr int kBins = 1024;
    std::vector<double> hist(kBins, 0);
    for (float x : v) hist[static_cast<size_t>(std::clamp((x - a) / (b - a) * kBins, 0.0, kBins - 1.0))] += 1;
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

// 4-connected components of `mask` (1 = member). Returns per-pixel labels (0 = none, 1..n) and the sizes
// (sizes[0] unused).
std::vector<int> Components(const std::vector<uint8_t>& mask, int w, int h, std::vector<size_t>& sizes) {
    std::vector<int> label(mask.size(), 0);
    sizes.assign(1, 0);
    std::vector<size_t> queue;
    for (size_t start = 0; start < mask.size(); ++start) {
        if (!mask[start] || label[start]) continue;
        const int id = static_cast<int>(sizes.size());
        sizes.push_back(0);
        queue.clear();
        queue.push_back(start);
        label[start] = id;
        for (size_t q = 0; q < queue.size(); ++q) {
            const size_t i = queue[q];
            ++sizes[static_cast<size_t>(id)];
            const int x = static_cast<int>(i % static_cast<size_t>(w)), y = static_cast<int>(i / static_cast<size_t>(w));
            const size_t nb[4] = {x > 0 ? i - 1 : i, x + 1 < w ? i + 1 : i, y > 0 ? i - static_cast<size_t>(w) : i,
                                  y + 1 < h ? i + static_cast<size_t>(w) : i};
            for (size_t j : nb)
                if (mask[j] && !label[j]) {
                    label[j] = id;
                    queue.push_back(j);
                }
        }
    }
    return label;
}

// Sets every pixel of a non-member region that does not reach the frame border (it is enclosed by members).
void FillHoles(std::vector<uint8_t>& mask, int w, int h) {
    std::vector<uint8_t> outside(mask.size());
    for (size_t i = 0; i < mask.size(); ++i) outside[i] = !mask[i];
    std::vector<size_t> hs;
    const std::vector<int> hl = Components(outside, w, h, hs);
    std::vector<uint8_t> touches(hs.size(), 0);
    for (int x = 0; x < w; ++x) {
        touches[static_cast<size_t>(hl[static_cast<size_t>(x)])] = 1;
        touches[static_cast<size_t>(hl[static_cast<size_t>(h - 1) * w + x])] = 1;
    }
    for (int y = 0; y < h; ++y) {
        touches[static_cast<size_t>(hl[static_cast<size_t>(y) * w])] = 1;
        touches[static_cast<size_t>(hl[static_cast<size_t>(y) * w + w - 1])] = 1;
    }
    for (size_t i = 0; i < mask.size(); ++i)
        if (hl[i] && !touches[static_cast<size_t>(hl[i])]) mask[i] = 1;
}

// Chebyshev dilation of `mask` by r px (separable max), in place.
void Dilate(std::vector<uint8_t>& mask, int w, int h, int r) {
    if (r <= 0) return;
    std::vector<uint8_t> tmp(mask.size(), 0);
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = &mask[static_cast<size_t>(y) * w];
        int last = -1 - r;  // most recent member at or left of x + r
        // two sweeps: nearest member to the left and to the right within r
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

}  // namespace

// ---- 1. Lunar disk and sky ----

LunarMasks SegmentLunar(const ser::FloatImage& rgb, int threads) {
    RequireRgb(rgb, "SegmentLunar");
    const int w = rgb.width, h = rgb.height;
    const size_t n = Pixels(rgb);
    const ser::FloatImage L = MeanOfChannels(rgb, threads);
    const ser::FloatImage Ls = Box3(L, threads);
    LunarMasks m;
    m.width = w;
    m.height = h;
    m.disk.assign(n, 0);
    m.sky.assign(n, 0);

    // The disc's level D: the median of the brighter Otsu class (sky / disc, or -- with no sky -- the
    // brighter terrain; either way D is lit disc).
    const double t0 = Otsu(Ls.data);
    std::vector<float> upper;
    for (float v : Ls.data)
        if (v > t0) upper.push_back(v);
    if (upper.empty()) return m;  // a flat image: no disc
    const double D = Median(upper);
    if (!(D > 0)) return m;
    // Sky candidates: darker than a quarter of the disc's level. Maria (0.5-0.7 of the highlands on the real
    // Moon) and lit terrain are not; the sky and the unlit side are. Limit: a sky brighter than D / 4 (a
    // twilight Moon) is not found -- it reads as "no sky" and the background step skips with that reason.
    std::vector<float> dark, darkDiff;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = static_cast<size_t>(y) * w + x;
            if (Ls.data[i] < 0.25 * D) {
                dark.push_back(Ls.data[i]);
                if (x + 1 < w && Ls.data[i + 1] < 0.25 * D) darkDiff.push_back(L.data[i + 1] - L.data[i]);
            }
        }
    // Under 0.1% of the frame: no sky to anchor a level.
    const bool haveSky = dark.size() >= std::max<size_t>(64, n / 1000);
    double t;
    if (haveSky) {
        const double b = Median(dark);
        // Pixel noise from neighbour differences (blind to gradients), / sqrt 2, then / 3 for the 3 x 3 mean.
        const double sigmaS = MadStd(darkDiff) / std::sqrt(2.0) / 3;
        // 5 sigma clears the noise; at least 4% and at most 15% of the contrast, so a faint lit terminator
        // (10% of the lit median, spec step 1) is disc and a noisy sky estimate cannot swallow it.
        t = b + std::clamp(5 * sigmaS, 0.04 * (D - b), 0.15 * (D - b));
    } else {
        t = 0.04 * D;
    }
    std::vector<uint8_t> above(n);
    for (size_t i = 0; i < n; ++i) above[i] = Ls.data[i] > t;
    // Keep the disc's own pieces: components of at least 1% of the largest (a crescent is one piece; a star,
    // a hot pixel or a noise speck is not).
    std::vector<size_t> sizes;
    std::vector<int> label = Components(above, w, h, sizes);
    const size_t largest = sizes.size() > 1 ? *std::max_element(sizes.begin() + 1, sizes.end()) : 0;
    const size_t keep = std::max<size_t>(16, largest / 100);
    for (size_t i = 0; i < n; ++i) m.disk[i] = label[i] && sizes[static_cast<size_t>(label[i])] >= keep;
    // Fill holes: a dark region (crater shadow, deep mare) that does not reach the frame border is enclosed by
    // the disc and is disc.
    FillHoles(m.disk, w, h);
    // Sky: below the disc threshold and more than kLimbMargin px from the disc (the blurred limb and its
    // transition are neither). With no sky candidates there is no sky.
    constexpr int kLimbMargin = 4;
    if (haveSky) {
        std::vector<uint8_t> near = m.disk;
        Dilate(near, w, h, kLimbMargin);
        for (size_t i = 0; i < n; ++i) m.sky[i] = !near[i] && Ls.data[i] <= t;
    }
    for (size_t i = 0; i < n; ++i) {
        m.diskCount += m.disk[i];
        m.skyCount += m.sky[i];
    }
    return m;
}

// ---- 2-3. Colour calibration and background ----

namespace {

void RequireMasks(const ser::FloatImage& rgb, const LunarMasks& m, const char* who) {
    RequireRgb(rgb, who);
    if (m.width != rgb.width || m.height != rgb.height || m.disk.size() != Pixels(rgb) || m.sky.size() != Pixels(rgb))
        throw std::invalid_argument(std::string(who) + ": masks do not match the image");
}

// printf-style formatting of numbers into a HISTORY fragment (ASCII: the formats are).
template <class... A>
std::string Fmt(const char* format, A... args) {
    char buf[256];
    std::snprintf(buf, sizeof buf, format, static_cast<double>(args)...);
    return buf;
}

std::string Percent(size_t part, size_t whole) { return Fmt("%.2f%%", 100.0 * part / std::max<size_t>(1, whole)); }

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
Plane SkyPlane(const ser::FloatImage& rgb, const LunarMasks& m, int ch) {
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

// Spec step 2: under 1% of the frame, the disc is too small for a white reference and the sky too small for a
// background reference.
constexpr double kMinShare = 0.01;

}  // namespace

StepRecord CalibrateColour(ser::FloatImage& rgb, const LunarMasks& m) {
    RequireMasks(rgb, m, "CalibrateColour");
    const int w = rgb.width, h = rgb.height;
    const size_t n = Pixels(rgb);
    StepRecord rec;
    if (m.diskCount < kMinShare * n) {
        rec.skipped = true;
        rec.history = "Colour calibration skipped: the lit disk is " + Percent(m.diskCount, n) +
                      " of the frame, under the 1% a white reference needs";
        return rec;
    }
    // Background reference: the sky's plane per channel (the same model NeutraliseBackground removes), so a
    // gradient does not bias the white reference. Without enough sky, none: the white reference is taken on
    // the raw levels, and HISTORY says so.
    const bool haveSky = m.skyCount >= kMinShare * n;
    Plane bg[3];
    if (haveSky)
        for (int c = 0; c < 3; ++c) bg[c] = SkyPlane(rgb, m, c);
    // White reference: the whole lit disc, leaving out every pixel with a saturated sample (its true colour is
    // unknown: a highland clipped in R alone would read too blue once scaled).
    double sum[3] = {0, 0, 0};
    size_t used = 0, saturated = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = static_cast<size_t>(y) * w + x;
            if (!m.disk[i]) continue;
            const float* p = &rgb.data[i * 3];
            if (p[0] >= kClippedLuma || p[1] >= kClippedLuma || p[2] >= kClippedLuma) {
                ++saturated;
                continue;
            }
            for (int c = 0; c < 3; ++c) sum[c] += p[c] - bg[c].At(x, y, w, h);
            ++used;
        }
    if (used < kMinShare * n || !(sum[0] > 0 && sum[1] > 0 && sum[2] > 0)) {
        rec.skipped = true;
        rec.history = "Colour calibration skipped: " + Percent(used, n) +
                      " of the frame is unsaturated lit disk with a positive level, under the 1% a white reference needs";
        return rec;
    }
    // Scale R and B to G's level about the background, so the disc's mean is neutral and G keeps its brightness.
    const double gain[3] = {sum[1] / sum[0], 1.0, sum[1] / sum[2]};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float* p = &rgb.data[(static_cast<size_t>(y) * w + x) * 3];
            for (int c : {0, 2}) {
                const double b = bg[c].At(x, y, w, h);
                p[c] = static_cast<float>(b + (p[c] - b) * gain[c]);
            }
        }
    rec.history = "Colour calibration: white = lit disk (" + std::to_string(used) + " px, " + std::to_string(saturated) +
                  " with a saturated sample left out), background = " +
                  (haveSky ? std::string("sky plane") : "none (sky " + Percent(m.skyCount, n) + " of the frame, under 1%)") +
                  Fmt("; gains R %.4f G 1 B %.4f", gain[0], gain[2]);
    return rec;
}

StepRecord NeutraliseBackground(ser::FloatImage& rgb, const LunarMasks& m) {
    RequireMasks(rgb, m, "NeutraliseBackground");
    const int w = rgb.width, h = rgb.height;
    const size_t n = Pixels(rgb);
    StepRecord rec;
    if (m.skyCount < kMinShare * n) {
        rec.skipped = true;
        rec.history = "Background skipped: the sky is " + Percent(m.skyCount, n) +
                      " of the frame, under the 1% needed to measure it; nothing subtracted";
        return rec;
    }
    // Spec step 3: the pedestal per channel plus a linear gradient, measured on sky pixels only and removed
    // everywhere (it is light added on top of the disc as well). Samples may go below 0 here; the display
    // transfer sets the black point.
    Plane p[3];
    for (int c = 0; c < 3; ++c) p[c] = SkyPlane(rgb, m, c);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float* q = &rgb.data[(static_cast<size_t>(y) * w + x) * 3];
            for (int c = 0; c < 3; ++c) q[c] = static_cast<float>(q[c] - p[c].At(x, y, w, h));
        }
    rec.history = "Background: sky plane removed per channel (" + std::to_string(m.skyCount) + " sky px)" +
                  Fmt("; level R %.5f G %.5f B %.5f", p[0].a, p[1].a, p[2].a) +
                  Fmt(", rise across the frame R %.5f G %.5f B %.5f", std::hypot(p[0].b, p[0].c), std::hypot(p[1].b, p[1].c),
                      std::hypot(p[2].b, p[2].c));
    return rec;
}

}  // namespace planetary::stack

namespace planetary::stack {

// ---- 4b. Channel alignment ----

namespace {

// Solves the n x n system A x = b in place (Gaussian elimination, partial pivoting). False when singular.
bool Solve(int n, double* A, double* b, double* x) {
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
void CubicWeights(double f, double wt[4]) {
    const double f2 = f * f, f3 = f2 * f;
    wt[0] = -0.5 * f3 + f2 - 0.5 * f;
    wt[1] = 1.5 * f3 - 2.5 * f2 + 1;
    wt[2] = -1.5 * f3 + 2 * f2 + 0.5 * f;
    wt[3] = 0.5 * f3 - 0.5 * f2;
}

// Channel c of `src` sampled at (x + sx, y + sy) at every pixel, edges replicated: a 1-channel plane.
std::vector<float> ShiftedPlane(const ser::FloatImage& src, int c, double sx, double sy, int threads) {
    const int w = src.width, h = src.height, nc = src.channels;
    const int ix = static_cast<int>(std::floor(sx)), iy = static_cast<int>(std::floor(sy));
    double wx[4], wy[4];
    CubicWeights(sx - ix, wx);
    CubicWeights(sy - iy, wy);
    const size_t n = Pixels(src);
    std::vector<float> tmp(n), out(n);
    ParallelBands(h, threads, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x) {
                double s = 0;
                for (int k = 0; k < 4; ++k)
                    s += wx[k] * src.data[(static_cast<size_t>(y) * w + std::clamp(x + ix - 1 + k, 0, w - 1)) * nc + c];
                tmp[static_cast<size_t>(y) * w + x] = static_cast<float>(s);
            }
    });
    ParallelBands(h, threads, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x) {
                double s = 0;
                for (int k = 0; k < 4; ++k) s += wy[k] * tmp[static_cast<size_t>(std::clamp(y + iy - 1 + k, 0, h - 1)) * w + x];
                out[static_cast<size_t>(y) * w + x] = static_cast<float>(s);
            }
    });
    return out;
}

struct ChannelShift {
    double dx = 0, dy = 0;
    bool converged = false;
};

// Where channel c's content sits against G: G(x) ~ a C(x + s) + b over the frame less an 8 px border
// (the gain and offset absorb what calibration left; the limb and the terrain carry the position).
// Gauss-Newton (Lucas-Kanade) from s = 0, each round on C resampled at the current s, to a step under
// 1e-4 px. Capture range: a few px (the limb is a large, smooth edge); dispersion is well under that.
ChannelShift MeasureShift(const ser::FloatImage& rgb, int c, int threads) {
    constexpr int kBorder = 8, kRounds = 30;
    const int w = rgb.width, h = rgb.height;
    ChannelShift s;
    if (w <= 2 * kBorder + 2 || h <= 2 * kBorder + 2) return s;
    double a = 1, b = 0;
    std::mutex mtx;
    for (int round = 0; round < kRounds; ++round) {
        const std::vector<float> C = ShiftedPlane(rgb, c, s.dx, s.dy, threads);
        double H[16] = {}, g[4] = {};
        ParallelBands(h - 2 * kBorder, threads, [&](int r0, int r1) {
            double Hl[16] = {}, gl[4] = {};
            for (int y = r0 + kBorder; y < r1 + kBorder; ++y)
                for (int x = kBorder; x < w - kBorder; ++x) {
                    const size_t i = static_cast<size_t>(y) * w + x;
                    const double gx = 0.5 * (C[i + 1] - C[i - 1]), gy = 0.5 * (C[i + w] - C[i - w]);
                    const double J[4] = {a * gx, a * gy, C[i], 1.0};
                    const double r = rgb.data[i * 3 + 1] - (a * C[i] + b);
                    for (int p = 0; p < 4; ++p) {
                        gl[p] += J[p] * r;
                        for (int q = 0; q < 4; ++q) Hl[p * 4 + q] += J[p] * J[q];
                    }
                }
            std::lock_guard<std::mutex> lock(mtx);
            for (int k = 0; k < 16; ++k) H[k] += Hl[k];
            for (int k = 0; k < 4; ++k) g[k] += gl[k];
        });
        double d[4];
        if (!Solve(4, H, g, d)) return s;
        s.dx += d[0];
        s.dy += d[1];
        a += d[2];
        b += d[3];
        if (!std::isfinite(s.dx) || !std::isfinite(s.dy) || std::hypot(s.dx, s.dy) > 8) return {};
        if (std::hypot(d[0], d[1]) < 1e-4) {
            s.converged = true;
            return s;
        }
    }
    return s;
}

// Amendment step 4b: a measured R or B shift against G above this is corrected; at or below it, nothing is
// resampled (resampling costs a little sharpness for nothing).
constexpr double kAlignMin = 0.1;

}  // namespace

StepRecord AlignChannels(ser::FloatImage& rgb, int threads) {
    RequireRgb(rgb, "AlignChannels");
    const ChannelShift r = MeasureShift(rgb, 0, threads), b = MeasureShift(rgb, 2, threads);
    StepRecord rec;
    if (!r.converged || !b.converged) {
        rec.skipped = true;
        rec.history = std::string("Channel alignment skipped: the ") + (!r.converged ? "R" : "B") +
                      " shift against G could not be measured (no convergence within 8 px); nothing resampled";
        return rec;
    }
    const double worst = std::max(std::hypot(r.dx, r.dy), std::hypot(b.dx, b.dy));
    rec.history = Fmt("Channel alignment: R %+.3f, %+.3f px, B %+.3f, %+.3f px against G", r.dx, r.dy, b.dx, b.dy);
    if (worst <= kAlignMin) {
        rec.skipped = true;
        rec.history += "; within 0.1 px, not applied";
        return rec;
    }
    const std::vector<float> R = ShiftedPlane(rgb, 0, r.dx, r.dy, threads), B = ShiftedPlane(rgb, 2, b.dx, b.dy, threads);
    for (size_t i = 0; i < R.size(); ++i) {
        rgb.data[i * 3] = R[i];
        rgb.data[i * 3 + 2] = B[i];
    }
    rec.history += "; R and B resampled onto G (Catmull-Rom)";
    return rec;
}

// ---- 4. Luminance sharpening ----

namespace {

int Mirror(int i, int n) {
    if (n == 1) return 0;
    while (i < 0 || i >= n) i = i < 0 ? -i : 2 * (n - 1) - i;
    return i;
}

// One B3-spline a trous smoothing ([1 4 6 4 1] / 16 with holes of `step` px, separable), mirrored edges.
std::vector<float> AtrousSmooth(const std::vector<float>& in, int w, int h, int step, int threads) {
    constexpr double k[5] = {1.0 / 16, 4.0 / 16, 6.0 / 16, 4.0 / 16, 1.0 / 16};
    std::vector<float> tmp(in.size()), out(in.size());
    ParallelBands(h, threads, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x) {
                double s = 0;
                for (int t = 0; t < 5; ++t) s += k[t] * in[static_cast<size_t>(y) * w + Mirror(x + (t - 2) * step, w)];
                tmp[static_cast<size_t>(y) * w + x] = static_cast<float>(s);
            }
    });
    ParallelBands(h, threads, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x) {
                double s = 0;
                for (int t = 0; t < 5; ++t) s += k[t] * tmp[static_cast<size_t>(Mirror(y + (t - 2) * step, h)) * w + x];
                out[static_cast<size_t>(y) * w + x] = static_cast<float>(s);
            }
    });
    return out;
}

// Approximate Euclidean distance (two-pass chamfer, steps 1 and sqrt 2) from each pixel to the nearest pixel
// outside `mask`; +inf everywhere when every pixel is in it (a disc filling the frame has no limb in view).
std::vector<float> DistanceOutside(const std::vector<uint8_t>& mask, int w, int h) {
    const float inf = std::numeric_limits<float>::infinity(), d1 = 1.0f, d2 = 1.41421356f;
    std::vector<float> d(mask.size());
    for (size_t i = 0; i < mask.size(); ++i) d[i] = mask[i] ? inf : 0.0f;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float& v = d[static_cast<size_t>(y) * w + x];
            if (x > 0) v = std::min(v, d[static_cast<size_t>(y) * w + x - 1] + d1);
            if (y > 0) {
                v = std::min(v, d[static_cast<size_t>(y - 1) * w + x] + d1);
                if (x > 0) v = std::min(v, d[static_cast<size_t>(y - 1) * w + x - 1] + d2);
                if (x + 1 < w) v = std::min(v, d[static_cast<size_t>(y - 1) * w + x + 1] + d2);
            }
        }
    for (int y = h - 1; y >= 0; --y)
        for (int x = w - 1; x >= 0; --x) {
            float& v = d[static_cast<size_t>(y) * w + x];
            if (x + 1 < w) v = std::min(v, d[static_cast<size_t>(y) * w + x + 1] + d1);
            if (y + 1 < h) {
                v = std::min(v, d[static_cast<size_t>(y + 1) * w + x] + d1);
                if (x + 1 < w) v = std::min(v, d[static_cast<size_t>(y + 1) * w + x + 1] + d2);
                if (x > 0) v = std::min(v, d[static_cast<size_t>(y + 1) * w + x - 1] + d2);
            }
        }
    return d;
}

constexpr int kSharpenLayers = 4;
// Std of each starlet layer of unit white noise (B3 spline; Starck, Murtagh & Fadili 2010, table 6.1).
constexpr double kStarletNoise[kSharpenLayers] = {0.889, 0.200, 0.086, 0.041};
// Nominal gains of layers 1-4 before the noise limit. Seeing of 1-1.5 px takes detail at layer 2's scale (4-8 px
// periods) most; layers 3-4 (16 px and up) keep >= 0.84 of theirs, and their 14-30 px reach rings at the limb.
// Chosen by measurement on MoonSynth (truth detail to 1/4 cycle/px, noise 0.005, 2 sigma threshold), disc RMS
// error against the truth: {1.5, 2.5, 1, 1} 18.2% / 14.3% lower at blur 1.0 / 1.5 px; {1.5, 2.2, 1.6, 1.25}
// -13.7% / +3.4% (over-boosts the coarse layers); {1.5, 2.0, 1.2, 1.05} 16.8% / 10.7%; {1.6, 3.0, 1, 1} 3.3% /
// 15.1% (too strong for the lighter blur).
constexpr double kLayerGain[kSharpenLayers] = {1.5, 2.5, 1.0, 1.0};
// Only a coefficient's part beyond this many layer-noise sigmas is boosted (soft threshold), so noise that
// survives the per-layer limit is not amplified pixel by pixel.
constexpr double kBoostThreshold = 2.0;
// Half-width of layer j's reach (the a trous kernels of layers 1..j stacked: 2 (2^j - 1) px = 2, 6, 14, 30).
// A layer's coefficients feel the disc's edge (limb or terminator) that far in. Its boost is 0 outside the disc
// (no ringing into the sky: spec step 4, no halo) and, inside, fades in from one reach to two reaches from the
// edge's HALF-contrast line (no bright rim inside the limb). Measured from there because that line stays on the
// limb whatever the seeing, while the disc mask's own edge (4% of the contrast) moves out with the blur: ramps
// from the mask edge left a rim of 0.043 (one 8 px ramp for every layer) and 0.017 (per-layer ramps, half a reach
// to 1.5 reaches) over a flat disc's 0.30, blur 1.5 px (test "sharpen: no bright rim inside the limb"). From the
// half line, a blurred step's layer-2 coefficient at full weight is under 0.5% of the contrast up to blur 3 px.
constexpr double kLayerReach[kSharpenLayers] = {2, 6, 14, 30};
double EdgeWeight(double dist, int j) { return std::clamp((dist - kLayerReach[j]) / kLayerReach[j], 0.0, 1.0); }
// Highlight shoulder: above kKnee the sharpened luminance is rolled off smoothly toward kCeiling, so sharpening
// never clips a sample the stack did not (kCeiling sits under kClippedLuma, the clip count's level).
constexpr double kKnee = 0.9, kCeiling = 0.998;

double Shoulder(double x) {
    if (x <= kKnee) return x;
    return kKnee + (kCeiling - kKnee) * std::tanh((x - kKnee) / (kCeiling - kKnee));
}

double Soft(double v, double t) { return v > t ? v - t : (v < -t ? v + t : 0.0); }

}  // namespace

std::vector<ser::FloatImage> StarletLayers(const ser::FloatImage& lum1, int layers, int threads) {
    if (lum1.channels != 1 || lum1.width <= 0 || lum1.height <= 0 || lum1.data.size() != Pixels(lum1))
        throw std::invalid_argument("StarletLayers: needs a 1-channel image whose data matches its size");
    if (layers < 1) throw std::invalid_argument("StarletLayers: needs at least one layer");
    std::vector<ser::FloatImage> out;
    std::vector<float> c = lum1.data;
    for (int j = 0; j < layers; ++j) {
        std::vector<float> next = AtrousSmooth(c, lum1.width, lum1.height, 1 << j, threads);
        ser::FloatImage layer{lum1.width, lum1.height, 1, std::vector<float>(c.size())};
        for (size_t i = 0; i < c.size(); ++i) layer.data[i] = c[i] - next[i];
        out.push_back(std::move(layer));
        c = std::move(next);
    }
    out.push_back(ser::FloatImage{lum1.width, lum1.height, 1, std::move(c)});
    return out;
}

double LayerNoise(const ser::FloatImage& layer, const std::vector<uint8_t>& region) {
    if (!region.empty() && region.size() != Pixels(layer))
        throw std::invalid_argument("LayerNoise: region does not match the layer");
    std::vector<float> v;
    for (size_t i = 0; i < Pixels(layer); ++i)
        if (region.empty() || region[i]) v.push_back(layer.data[i * static_cast<size_t>(layer.channels)]);
    return MadStd(v);
}

StepRecord SharpenLuminanceImage(ser::FloatImage& lum1, const LunarMasks& m, int threads) {
    if (lum1.channels != 1 || lum1.data.size() != Pixels(lum1) || m.width != lum1.width || m.height != lum1.height ||
        m.disk.size() != Pixels(lum1))
        throw std::invalid_argument("SharpenLuminanceImage: needs a 1-channel image and masks of its size");
    const int w = lum1.width, h = lum1.height;
    const size_t n = Pixels(lum1);
    StepRecord rec;
    if (m.diskCount == 0) {
        rec.skipped = true;
        rec.history = "Sharpen skipped: no lunar disk found to sharpen";
        return rec;
    }
    // The disc's core: disc pixels at or above half the contrast between the sky (its median; 0 without sky)
    // and the disc's median, on the 3 x 3 mean, enclosed holes filled (craters, dark mare floors). Maria sit at
    // 0.5-0.7 of the highlands on the real Moon and above half the disc median; the limb's and the terminator's
    // half-contrast lines are its edge. Distance into the core from that edge (+inf when nothing in view is
    // outside it: a disc filling the frame).
    std::vector<uint8_t> core(n, 0);
    {
        std::vector<float> d, s;
        for (size_t i = 0; i < n; ++i) {
            if (m.disk[i]) d.push_back(lum1.data[i]);
            if (!m.sky.empty() && m.sky[i]) s.push_back(lum1.data[i]);
        }
        const double half = 0.5 * (Median(d) + (s.empty() ? 0.0 : Median(s)));
        const ser::FloatImage Ls = Box3(lum1, threads);
        for (size_t i = 0; i < n; ++i) core[i] = m.disk[i] && Ls.data[i] >= half;
        FillHoles(core, w, h);
    }
    const std::vector<float> dist = DistanceOutside(core, w, h);
    // Where a layer's boost is at full weight; the statistics of layer j are taken there (its coefficients clear
    // of the edge), or over the whole disc when that leaves under 1000 px (a small disc).
    auto region = [&](int j) {
        std::vector<uint8_t> r(n);
        size_t k = 0;
        for (size_t i = 0; i < n; ++i) k += r[i] = core[i] && EdgeWeight(dist[i], j) >= 1.0;
        return k >= 1000 ? r : m.disk;
    };
    const std::vector<ser::FloatImage> layers = StarletLayers(lum1, kSharpenLayers, threads);
    // Noise (white-noise std, from layer 1, which keeps 0.889 of it): the lower of the sky's and the disc's
    // flattest tiles'. Terrain only ever inflates a layer-1 estimate -- taken over the whole disc of a scene with
    // detail out to 1/4 cycle/px it read 1.5-2.4x the true noise and held the gains near 1 (measured on
    // MoonSynth, blur 1.0-1.5 px) -- so the disc's value is the 10th percentile over 16 x 16 tiles clear of the
    // edge (the smoothest terrain, maria floors), and the sky, which is noise only, caps it. Limit: where a
    // stack's disc noise exceeds its sky's (shot noise; in display units the transfer's slope works the other
    // way), the sky's lower value lets more disc noise through the threshold; the gains stay bounded by
    // kLayerGain and the Wiener factor below.
    double white;
    {
        const std::vector<uint8_t> r = region(0);
        constexpr int kTile = 16;
        std::vector<float> tileNoise;
        std::vector<float> v;
        for (int ty = 0; ty + kTile <= h; ty += kTile)
            for (int tx = 0; tx + kTile <= w; tx += kTile) {
                v.clear();
                for (int y = ty; y < ty + kTile; ++y)
                    for (int x = tx; x < tx + kTile; ++x) {
                        const size_t i = static_cast<size_t>(y) * w + x;
                        if (r[i]) v.push_back(layers[0].data[i]);
                    }
                if (v.size() == static_cast<size_t>(kTile * kTile)) tileNoise.push_back(static_cast<float>(MadStd(v)));
            }
        double disc;
        if (tileNoise.size() >= 10) {
            const size_t k = tileNoise.size() / 10;
            std::nth_element(tileNoise.begin(), tileNoise.begin() + static_cast<long>(k), tileNoise.end());
            disc = tileNoise[k];
        } else {
            disc = LayerNoise(layers[0], r);
        }
        white = disc;
        if (m.skyCount >= 1000) white = std::min(white, LayerNoise(layers[0], m.sky));
        white /= kStarletNoise[0];
    }
    double gain[kSharpenLayers];
    for (int j = 0; j < kSharpenLayers; ++j) {
        // The layer's own noise limit: a Wiener factor, the share of the layer's energy over the disc that is not
        // noise. A layer that is noise only gets gain 1 (spec step 4: a noise-only gain fails).
        const std::vector<uint8_t> r = region(j);
        const double sj = white * kStarletNoise[j];
        double e = 0;
        size_t k = 0;
        for (size_t i = 0; i < n; ++i)
            if (r[i]) {
                e += static_cast<double>(layers[static_cast<size_t>(j)].data[i]) * layers[static_cast<size_t>(j)].data[i];
                ++k;
            }
        e /= std::max<size_t>(1, k);
        const double rho = e > 0 ? std::max(0.0, 1 - sj * sj / e) : 0.0;
        gain[j] = 1 + (kLayerGain[j] - 1) * rho;
    }
    ParallelBands(h, threads, [&](int y0, int y1) {
        for (size_t i = static_cast<size_t>(y0) * w; i < static_cast<size_t>(y1) * w; ++i) {
            double v = lum1.data[i];
            if (core[i])
                for (int j = 0; j < kSharpenLayers; ++j)
                    v += (gain[j] - 1) * EdgeWeight(dist[i], j) *
                         Soft(layers[static_cast<size_t>(j)].data[i], kBoostThreshold * white * kStarletNoise[j]);
            lum1.data[i] = static_cast<float>(Shoulder(v));
        }
    });
    rec.history = Fmt("Sharpen (luminance): starlet layers 1-4, noise %.3g; gains %.2f %.2f %.2f %.2f (noise-limited)",
                      white, gain[0], gain[1], gain[2], gain[3]);
    return rec;
}

StepRecord SharpenLuminance(ser::FloatImage& rgb, const LunarMasks& m, int threads) {
    RequireMasks(rgb, m, "SharpenLuminance");
    const ser::FloatImage L0 = MeanOfChannels(rgb, threads);
    ser::FloatImage L = L0;
    StepRecord rec = SharpenLuminanceImage(L, m, threads);
    if (rec.skipped) return rec;
    // Every channel scaled by the luminance's own factor: R/G and B/G stay as they were (chroma untouched); a
    // pixel whose largest channel would pass kCeiling is scaled less, uniformly, so its hue holds too.
    ParallelBands(rgb.height, threads, [&](int y0, int y1) {
        for (size_t i = static_cast<size_t>(y0) * rgb.width; i < static_cast<size_t>(y1) * rgb.width; ++i) {
            if (L.data[i] == L0.data[i] || !(L0.data[i] > 1e-6f)) continue;
            float* p = &rgb.data[i * 3];
            double f = std::max(0.0f, L.data[i]) / static_cast<double>(L0.data[i]);
            const double top = std::max({p[0], p[1], p[2]});
            if (top * f > kCeiling) f = std::min(f, std::max(1.0, kCeiling / top));
            for (int c = 0; c < 3; ++c) p[c] = static_cast<float>(p[c] * f);
        }
    });
    return rec;
}

}  // namespace planetary::stack

namespace planetary::stack {

// ---- 5-7. Chroma layer and mineral colour ----

namespace {

// The amendment's chroma blur default (2.5 px) is for frames 1-2k px wide; wider frames scale it with their
// width, so the colour's resolution against the disc stays the same.
double ResolutionScale(int width) { return std::max(1.0, width / 2000.0); }

}  // namespace

ser::FloatImage ChromaLayer(const ser::FloatImage& rgb, double sigma, int threads) {
    if (rgb.channels < 1 || rgb.width <= 0 || rgb.height <= 0 || rgb.data.size() != Pixels(rgb) * rgb.channels)
        throw std::invalid_argument("ChromaLayer: image data does not match its size");
    if (!(sigma >= 0) || !std::isfinite(sigma)) throw std::invalid_argument("ChromaLayer: sigma must be finite and >= 0");
    ser::FloatImage out = rgb;
    if (sigma == 0) return out;
    const int w = rgb.width, h = rgb.height, nc = rgb.channels;
    const int r = static_cast<int>(std::ceil(4 * sigma));
    std::vector<double> k(static_cast<size_t>(2 * r + 1));
    double s = 0;
    for (int i = -r; i <= r; ++i) s += k[static_cast<size_t>(i + r)] = std::exp(-0.5 * i * i / (sigma * sigma));
    for (double& v : k) v /= s;
    std::vector<float> tmp(rgb.data.size());
    // Separable, mirrored edges: a pixel at the frame edge averages its own side's pixels twice rather than
    // inventing any beyond the frame.
    ParallelBands(h, threads, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x)
                for (int c = 0; c < nc; ++c) {
                    double a = 0;
                    for (int i = -r; i <= r; ++i)
                        a += k[static_cast<size_t>(i + r)] * rgb.data[(static_cast<size_t>(y) * w + Mirror(x + i, w)) * nc + c];
                    tmp[(static_cast<size_t>(y) * w + x) * nc + c] = static_cast<float>(a);
                }
    });
    ParallelBands(h, threads, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x)
                for (int c = 0; c < nc; ++c) {
                    double a = 0;
                    for (int i = -r; i <= r; ++i)
                        a += k[static_cast<size_t>(i + r)] * tmp[(static_cast<size_t>(Mirror(y + i, h)) * w + x) * nc + c];
                    out.data[(static_cast<size_t>(y) * w + x) * nc + c] = static_cast<float>(a);
                }
    });
    return out;
}

double AutoSatThreshold(const ser::FloatImage& rgb) {
    RequireRgb(rgb, "AutoSatThreshold");
    // Amendment: 0.92 x the lowest per-channel 99.99th percentile (linear interpolation between order
    // statistics, numpy's default), over every pixel.
    const size_t n = Pixels(rgb);
    double lowest = std::numeric_limits<double>::infinity();
    std::vector<float> v(n);
    for (int c = 0; c < 3; ++c) {
        for (size_t i = 0; i < n; ++i) v[i] = rgb.data[i * 3 + static_cast<size_t>(c)];
        const double pos = (n - 1) * 0.9999;
        const size_t lo = static_cast<size_t>(std::floor(pos));
        std::nth_element(v.begin(), v.begin() + static_cast<long>(lo), v.end());
        const double a = v[lo];
        double b = a;
        if (lo + 1 < n) b = *std::min_element(v.begin() + static_cast<long>(lo) + 1, v.end());
        lowest = std::min(lowest, a + (pos - lo) * (b - a));
    }
    return 0.92 * lowest;
}

StepRecord MineralColour(ser::FloatImage& rgb, const ser::FloatImage& lumSharp, const MineralParams& p, int threads) {
    RequireRgb(rgb, "MineralColour");
    if (lumSharp.channels != 1 || lumSharp.width != rgb.width || lumSharp.height != rgb.height ||
        lumSharp.data.size() != Pixels(rgb))
        throw std::invalid_argument("MineralColour: the luminance must be one channel of the image's size");
    if (!(p.strength >= 0) || !(p.chromaBlurSigma >= 0) || !(p.eps >= 0) || !(p.devClamp >= 0) || !(p.satRamp > 0) ||
        !std::isfinite(p.strength) || !std::isfinite(p.chromaBlurSigma))
        throw std::invalid_argument("MineralColour: strength, chroma blur, eps and clamp must be >= 0 and the ramp > 0");
    const double sigma = p.chromaBlurSigma * ResolutionScale(rgb.width);
    const double thr = p.satThreshold >= 0 ? p.satThreshold : AutoSatThreshold(rgb);
    // 1. C: each channel blurred (the chroma noise reduction; colour at lower resolution than L, as in LRGB).
    const ser::FloatImage C = ChromaLayer(rgb, sigma, threads);
    const double k = p.strength;
    std::mutex mtx;
    size_t faded = 0, held = 0;
    ParallelBands(rgb.height, threads, [&](int y0, int y1) {
        size_t f = 0, g = 0;
        for (size_t i = static_cast<size_t>(y0) * rgb.width; i < static_cast<size_t>(y1) * rgb.width; ++i) {
            const float* q = &C.data[i * 3];
            // 2. d: each channel's ratio to the channels' mean, less 1, clamped (one hot or saturated sample
            //    cannot colour its neighbourhood beyond +-devClamp; eps keeps a black sky from dividing by 0).
            const double mean = (static_cast<double>(q[0]) + q[1] + q[2]) / 3;
            double d[3];
            for (int c = 0; c < 3; ++c) d[c] = std::clamp(q[c] / (mean + p.eps) - 1, -p.devClamp, p.devClamp);
            // 3. Highlight fade (replaces step 7's clean-up): neutral at and above the threshold, R and G being
            //    the channels that saturate first on lunar highlands.
            double wgt = std::clamp((thr - std::max(q[0], q[1])) / p.satRamp, 0.0, 1.0);
            if (wgt < 1) ++f;
            // Gamut: where L (1 + k w d) would pass kCeiling in some channel, w is lowered for the whole pixel --
            // its hue holds, its saturation gives -- so the colour never clips a sample the stack did not (spec
            // anti-goal: no new clipping; amendment's clip is then only a guard). With k = 8, d at the 0.06 clamp,
            // this acts only where L > 0.67 with w > 0.
            const double L = lumSharp.data[i];
            bool limited = false;
            for (int c = 0; c < 3; ++c)
                if (k * wgt * d[c] > 0 && L * (1 + k * wgt * d[c]) > kCeiling) {
                    wgt = L < kCeiling ? std::min(wgt, (kCeiling / L - 1) / (k * d[c])) : 0.0;
                    limited = true;
                }
            g += limited;
            // 4. out = clip(L (1 + k w d)): every detail from the sharp L, the colour from the ratios.
            float* o = &rgb.data[i * 3];
            for (int c = 0; c < 3; ++c) o[c] = static_cast<float>(std::clamp(L * (1 + k * wgt * d[c]), 0.0, 1.0));
        }
        std::lock_guard<std::mutex> lock(mtx);
        faded += f;
        held += g;
    });
    const size_t n = Pixels(rgb);
    StepRecord rec;
    rec.history = Fmt("Mineral colour: k %.2f, chroma blur %.2f px, eps %.4f, clamp +-%.3f, highlight fade from %.4f over %.3f",
                      k, sigma, p.eps, p.devClamp, thr, p.satRamp) +
                  "; " + Percent(faded, n) + " of pixels faded, " + Percent(held, n) + " held in gamut";
    return rec;
}

}  // namespace planetary::stack

namespace planetary::stack {

// ---- 8. Display transfer and the pipeline ----

namespace {

// PixInsight's midtones transfer function: MTF(0) = 0, MTF(m) = 0.5, MTF(1) = 1, monotone for m in (0, 1).
double Mtf(double x, double m) {
    if (x <= 0) return 0;
    if (x >= 1) return 1;
    return (m - 1) * x / ((2 * m - 1) * x - m);
}
// The midtones balance m that sends x to t (0 < x < 1, 0 < t < 1).
double MtfBalance(double x, double t) { return x * (1 - t) / (x - 2 * x * t + t); }

// Spec step 8: the disc's median at a normal display level, and headroom for sharpening and colour above the
// brightest unsaturated sample.
constexpr double kDisplayMedian = 0.35, kDisplayHeadroom = 0.95;

}  // namespace

StepRecord DisplayTransfer(ser::FloatImage& rgb, const LunarMasks& m, const std::vector<uint8_t>& stackClipped) {
    RequireMasks(rgb, m, "DisplayTransfer");
    if (!stackClipped.empty() && stackClipped.size() != rgb.data.size())
        throw std::invalid_argument("DisplayTransfer: clipped flags do not match the image");
    const size_t n = Pixels(rgb);
    StepRecord rec;
    // The disc's median luminance (the whole frame's without a disc), and the brightest sample the stack did
    // not saturate.
    std::vector<float> lum;
    for (size_t i = 0; i < n; ++i)
        if (m.diskCount == 0 || m.disk[i]) lum.push_back((rgb.data[i * 3] + rgb.data[i * 3 + 1] + rgb.data[i * 3 + 2]) / 3);
    const double med = Median(lum);
    double top = 0;
    for (size_t k = 0; k < rgb.data.size(); ++k)
        if (stackClipped.empty() || !stackClipped[k]) top = std::max(top, static_cast<double>(rgb.data[k]));
    if (!(med > 0) || !(top > med)) {
        rec.skipped = true;
        rec.history = Fmt("Display transfer skipped: no usable signal (disk median %.3g, brightest unsaturated sample %.3g); "
                          "samples clipped to [0, 1]",
                          med, top);
        for (float& v : rgb.data) v = std::clamp(v, 0.0f, 1.0f);
        return rec;
    }
    // One curve for all three channels (a linked stretch, as an STF or HistogramTransformation applies it):
    // x = v / W clipped to [0, 1], then the MTF whose balance puts the disc's median at kDisplayMedian. W is
    // the smallest white point >= the brightest unsaturated sample at which that sample lands at or under
    // kDisplayHeadroom: the transfer clips nothing the stack did not, and the colour step and the sharpening
    // keep their room. As W grows (the median held), the brightest sample's level falls monotonically toward
    // rho / (rho + (1 - t) / t), rho = its ratio to the median -- under 1 always, so W exists whenever that
    // limit is under the headroom (rho < ~35); otherwise W = 1e4 x the brightest and it sits at the limit.
    auto levelOfTop = [&](double W) { return Mtf(top / W, MtfBalance(med / W, kDisplayMedian)); };
    double lo = std::log(top), hi = std::log(top * 1e4);
    if (levelOfTop(std::exp(hi)) > kDisplayHeadroom) {
        lo = hi;
    } else {
        for (int it = 0; it < 60; ++it) {
            const double mid = 0.5 * (lo + hi);
            (levelOfTop(std::exp(mid)) > kDisplayHeadroom ? lo : hi) = mid;
        }
        lo = hi;
    }
    const double W = std::exp(lo), bal = MtfBalance(med / W, kDisplayMedian);
    for (float& v : rgb.data) v = static_cast<float>(Mtf(v / W, bal));
    rec.history = Fmt("Display transfer: linked midtones %.5f, white point %.5f; disk median %.4f -> %.2f, "
                      "brightest unsaturated sample %.4f -> %.3f",
                      bal, W, med, kDisplayMedian, top, Mtf(top / W, bal));
    return rec;
}

namespace {

// Runs the cached steps 1-6, timing each and reporting (kPhaseMineral, step, total); a cancel seen at any
// report throws StackCancelled before any image is returned.
class MineralSteps {
public:
    MineralSteps(const Progress& progress, int total) : progress_(progress), total_(total) {}
    template <class Fn>
    void Run(const std::string& name, std::vector<std::pair<std::string, double>>& seconds, Fn fn) {
        Tick();
        const auto t0 = std::chrono::steady_clock::now();
        fn();
        seconds.push_back({name, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count()});
        ++done_;
    }
    void Tick() {
        if (progress_.report) progress_.report(kPhaseMineral, done_, total_);
        if (progress_.cancelled && progress_.cancelled()) throw StackCancelled("Mineral moon cancelled");
    }

private:
    const Progress& progress_;
    int total_, done_ = 0;
};

constexpr int kMineralSteps = 7;

}  // namespace

MineralCache PrepareMineral(const ser::FloatImage& stack, int threads, const Progress& progress) {
    if (stack.channels != 3 || stack.width <= 0 || stack.height <= 0 || stack.data.size() != Pixels(stack) * 3)
        throw std::invalid_argument("Mineral moon needs a 3-channel colour stack (this one has " +
                                    std::to_string(stack.channels) + " channel(s))");
    MineralCache cache;
    MineralSteps steps(progress, kMineralSteps);
    ser::FloatImage rgb = stack;
    std::vector<uint8_t> clipped(stack.data.size());
    for (size_t k = 0; k < stack.data.size(); ++k) clipped[k] = stack.data[k] >= kClippedLuma;
    const size_t n = Pixels(stack);
    steps.Run("Lunar disk", cache.stepSeconds, [&] {
        cache.masks = SegmentLunar(rgb, threads);
        cache.history.push_back("Lunar disk: " + Percent(cache.masks.diskCount, n) + " of the frame, sky " +
                                Percent(cache.masks.skyCount, n));
    });
    steps.Run("Colour calibration", cache.stepSeconds,
              [&] { cache.history.push_back(CalibrateColour(rgb, cache.masks).history); });
    steps.Run("Background", cache.stepSeconds, [&] { cache.history.push_back(NeutraliseBackground(rgb, cache.masks).history); });
    steps.Run("Display transfer", cache.stepSeconds,
              [&] { cache.history.push_back(DisplayTransfer(rgb, cache.masks, clipped).history); });
    steps.Run("Channel alignment", cache.stepSeconds, [&] { cache.history.push_back(AlignChannels(rgb, threads).history); });
    steps.Run("Sharpen", cache.stepSeconds, [&] {
        cache.lum = MeanOfChannels(rgb, threads);
        cache.history.push_back(SharpenLuminanceImage(cache.lum, cache.masks, threads).history);
    });
    steps.Tick();
    cache.display = std::move(rgb);
    return cache;
}

ser::FloatImage ApplyMineralColour(const MineralCache& cache, const MineralParams& p, int threads, StepRecord* record) {
    ser::FloatImage out = cache.display;
    StepRecord rec = MineralColour(out, cache.lum, p, threads);
    if (record) *record = std::move(rec);
    return out;
}

MineralOutcome MakeMineralMoon(const ser::FloatImage& stack, int threads, const Progress& progress, const MineralParams& params) {
    MineralCache cache = PrepareMineral(stack, threads, progress);
    MineralOutcome out;
    StepRecord rec;
    const auto t0 = std::chrono::steady_clock::now();
    ser::FloatImage image = ApplyMineralColour(cache, params, threads, &rec);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (progress.report) progress.report(kPhaseMineral, kMineralSteps, kMineralSteps);
    if (progress.cancelled && progress.cancelled()) throw StackCancelled("Mineral moon cancelled");
    out.image = std::move(image);
    out.history = std::move(cache.history);
    out.history.push_back(rec.history);
    out.stepSeconds = std::move(cache.stepSeconds);
    out.stepSeconds.push_back({"Mineral colour", secs});
    return out;
}

}  // namespace planetary::stack
