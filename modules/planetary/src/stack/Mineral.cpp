#include "Mineral.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
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

// printf-style formatting of numbers into a HISTORY fragment (ASCII: the formats are). Every argument is passed
// as a double, so every conversion is a floating one (%f, %g, %e).
template <class... A>
std::string Fmt(const char* format, A... args) {
    char buf[256];
    std::snprintf(buf, sizeof buf, format, static_cast<double>(args)...);
    return buf;
}

std::string Percent(size_t part, size_t whole) { return Fmt("%.2f%%", 100.0 * part / std::max<size_t>(1, whole)); }

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

// Otsu's threshold over a 1024-bin histogram of v between its smallest and largest finite values (NaN left out,
// +-Inf in the end bins). The full range, not a percentile clip: a disc under 0.1% of the frame lies above the
// 99.9th percentile, and with that clip it shared the top bin with the sky's noise tail, so Otsu split the sky
// (task ENG review I2). +inf when v has no finite value.
double Otsu(const std::vector<float>& v) {
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
    if (upper.empty()) {
        m.reason = "the frame is flat: no level stands above the rest";
        return m;
    }
    const double D = Median(upper);
    // D must clear the level of the darker class (its median) by kDiscSigmas sigma of the 3 x 3 mean's noise;
    // otherwise Otsu has only split the sky's noise (task ENG review I2) and there is no disc to find. Not the
    // frame's median: a disc covering most of the frame puts that on the disc. Noise from neighbour differences over
    // the whole frame (blind to gradients), / sqrt 2, then / 3 for the 3 x 3 mean.
    {
        constexpr double kDiscSigmas = 5;
        std::vector<float> lower, diff;
        lower.reserve(n - upper.size());
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const size_t i = static_cast<size_t>(y) * w + x;
                if (Ls.data[i] <= t0) lower.push_back(Ls.data[i]);
                if (x + 1 < w && std::isfinite(L.data[i]) && std::isfinite(L.data[i + 1])) diff.push_back(L.data[i + 1] - L.data[i]);
            }
        const double low = Median(lower), sigma = MadStd(diff) / std::sqrt(2.0) / 3;
        if (!(D > 0) || !(D - low > kDiscSigmas * sigma)) {
            m.reason = Fmt("the brightest class's level %.4g is not %.0f noise sigma (%.2g) above the darker class's %.4g",
                           D, kDiscSigmas, sigma, low);
            return m;
        }
    }
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
    if (m.diskCount == 0) m.reason = "no piece above the disc threshold is 16 px or more";
    return m;
}

// ---- 2-3. Colour calibration and background ----

namespace {

void RequireMasks(const ser::FloatImage& rgb, const LunarMasks& m, const char* who) {
    RequireRgb(rgb, who);
    if (m.width != rgb.width || m.height != rgb.height || m.disk.size() != Pixels(rgb) || m.sky.size() != Pixels(rgb))
        throw std::invalid_argument(std::string(who) + ": masks do not match the image");
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
    double ux = 0, uy = 0;  // 1-sigma uncertainty of dx, dy: the windows' own scatter (see MeasureShift)
    int windows = 0;        // informative windows the estimate rests on
    bool converged = false;
};

// The window grid: kWin x kWin px windows tiling the frame less a kBorder px border.
constexpr int kAlignBorder = 8, kAlignWin = 16;
// Under this many informative windows (a disc of a few windows: under ~60 px across) the scatter that measures
// the estimate's own uncertainty is not measurable, and nothing is applied.
constexpr int kAlignMinWindows = 16;
// A window is informative only when C and G share its structure this well (squared correlation).
constexpr double kAlignMinR2 = 0.8;
// Gaussian sigma (px) of the smoothing the measurement works on (AlignChannels).
constexpr double kAlignPreSmooth = 1.0;

// Where channel c's content sits against G: G(x) ~ a_k C(x + s) + b_k, ONE shift s for the frame and a gain and
// offset per kAlignWin-px window k (rgb is AlignChannels' smoothed copy).
// Why per window: lunar colour varies across the disc (maria vs highlands, and within each), and with one gain for
// the frame a colour gradient gives the two limbs different contrast in C than in G, which Lucas-Kanade reads as a
// shift -- on MoonSynth with tints +-10% and 6% tint structure the single-gain fit put R/B 0.25-0.50 px off
// (applied), a 24 x 24 frame 0.13 px. Within a window the colour is near constant and its own gain absorbs it.
// Each round: per window, the least-squares a_k, b_k on C resampled at s, and s's normal equations with a_k, b_k
// eliminated (Schur complement); then one Gauss-Newton step for s from the chosen windows, to a step under 1e-4 px.
// Informative windows (chosen once, at s = 0): C and G share the window's structure (squared correlation >=
// kAlignMinR2) and its position constraint (the smaller eigenvalue of the eliminated 2 x 2) is at least 5% of the
// 90th percentile's among those. Every per-window sum lands in that window's own slot and is added in window
// order, so the result does not depend on the thread count.
// Uncertainty, per axis, the largest of:
//  - the scatter of the windows' own estimates (each one's 4-parameter step from the converged s),
//    1.4826 x MAD / sqrt(K): a colour bias shows as scatter, a true dispersion moves every window alike;
//  - the formal 1-sigma from the summed information, the residual variance inflated by the smoothing's
//    correlation area;
//  - half the difference between the solutions on the two checkerboard halves of the windows, each solved from
//    s = 0: a fit that noise has taken into a wrong basin does not repeat on independent data (a grey synthetic
//    crescent sliver, no true shift, noise 0.005, read R +0.41, -0.34 px +-0.12 by the first two alone).
// A half that does not converge leaves the shift unmeasured (converged false).
ChannelShift MeasureShift(const ser::FloatImage& rgb, int c, int threads) {
    constexpr int kRounds = 60;
    const int w = rgb.width, h = rgb.height;
    ChannelShift out;
    const int nx = (w - 2 * kAlignBorder) / kAlignWin, ny = (h - 2 * kAlignBorder) / kAlignWin;
    if (nx <= 0 || ny <= 0) return out;
    const size_t K = static_cast<size_t>(nx) * ny;
    struct Win {
        double a = 1, b = 0;
        double Hs[4] = {}, gs[2] = {};   // s's normal equations, gain and offset eliminated
        double H4[16] = {}, g4[4] = {};  // the window's own 4-parameter normal equations
        double lmin = 0;                 // smaller eigenvalue of Hs
        double rss = 0;                  // sum of squared residuals
        double r2 = 0;                   // squared correlation of C and G over the window
    };
    std::vector<Win> win(K);
    // Fills every window's sums with C sampled at (sx, sy).
    auto measure = [&](double sx, double sy) {
        const std::vector<float> C = ShiftedPlane(rgb, c, sx, sy, threads);
        ParallelBands(ny, threads, [&](int j0, int j1) {
            for (int j = j0; j < j1; ++j)
                for (int k = 0; k < nx; ++k) {
                    Win& W = win[static_cast<size_t>(j) * nx + k];
                    const int x0 = kAlignBorder + k * kAlignWin, y0 = kAlignBorder + j * kAlignWin;
                    double sc = 0, scc = 0, sg = 0, sgg = 0, scg = 0, n = 0;
                    for (int y = y0; y < y0 + kAlignWin; ++y)
                        for (int x = x0; x < x0 + kAlignWin; ++x) {
                            const size_t i = static_cast<size_t>(y) * w + x;
                            const double cv = C[i], gv = rgb.data[i * 3 + 1];
                            sc += cv;
                            scc += cv * cv;
                            sg += gv;
                            sgg += gv * gv;
                            scg += cv * gv;
                            n += 1;
                        }
                    const double var = scc - sc * sc / n, varG = sgg - sg * sg / n, cov = scg - sc * sg / n;
                    W.a = var > 1e-12 * n ? cov / var : 1.0;
                    W.b = (sg - W.a * sc) / n;
                    W.r2 = var > 0 && varG > 0 ? cov * cov / (var * varG) : 0.0;
                    std::fill(std::begin(W.H4), std::end(W.H4), 0.0);
                    std::fill(std::begin(W.g4), std::end(W.g4), 0.0);
                    W.rss = 0;
                    for (int y = y0; y < y0 + kAlignWin; ++y)
                        for (int x = x0; x < x0 + kAlignWin; ++x) {
                            const size_t i = static_cast<size_t>(y) * w + x;
                            const double gx = 0.5 * (C[i + 1] - C[i - 1]), gy = 0.5 * (C[i + w] - C[i - w]);
                            const double r = rgb.data[i * 3 + 1] - (W.a * C[i] + W.b);
                            const double J[4] = {W.a * gx, W.a * gy, C[i], 1.0};
                            W.rss += r * r;
                            for (int p = 0; p < 4; ++p) {
                                W.g4[p] += J[p] * r;
                                for (int q = 0; q < 4; ++q) W.H4[p * 4 + q] += J[p] * J[q];
                            }
                        }
                    // s's equations with the gain and offset eliminated: what the window says about position
                    // beyond what a gain and an offset explain (a linear ramp, such as a terminator, says nothing
                    // along its slope; held fixed instead, a and b let the shift wander along it).
                    const double t00 = W.H4[10], t01 = W.H4[11], t11 = W.H4[15], tdet = t00 * t11 - t01 * t01;
                    const double i00 = tdet > 0 ? t11 / tdet : 0, i01 = tdet > 0 ? -t01 / tdet : 0,
                                 i11 = tdet > 0 ? t00 / tdet : 0;
                    for (int p = 0; p < 2; ++p) {
                        const double hp2 = W.H4[p * 4 + 2], hp3 = W.H4[p * 4 + 3];
                        const double m0 = hp2 * i00 + hp3 * i01, m1 = hp2 * i01 + hp3 * i11;  // H_s,theta H_theta^-1
                        W.gs[p] = W.g4[p] - (m0 * W.g4[2] + m1 * W.g4[3]);
                        for (int q = 0; q < 2; ++q)
                            W.Hs[p * 2 + q] = W.H4[p * 4 + q] - (m0 * W.H4[2 * 4 + q] + m1 * W.H4[3 * 4 + q]);
                    }
                    const double tr = W.Hs[0] + W.Hs[3], det = W.Hs[0] * W.Hs[3] - W.Hs[1] * W.Hs[2];
                    W.lmin = 0.5 * (tr - std::sqrt(std::max(0.0, tr * tr - 4 * det)));
                }
        });
    };
    // Informative windows, at s = 0.
    measure(0, 0);
    std::vector<uint8_t> informative(K, 0);
    {
        std::vector<double> l;
        for (size_t k = 0; k < K; ++k)
            if (win[k].r2 >= kAlignMinR2) l.push_back(win[k].lmin);
        double floor = 0;
        if (!l.empty()) {
            const size_t q = std::min(l.size() - 1, static_cast<size_t>(0.9 * l.size()));
            std::nth_element(l.begin(), l.begin() + static_cast<long>(q), l.end());
            floor = 0.05 * l[q];
        }
        for (size_t k = 0; k < K; ++k) {
            informative[k] = win[k].r2 >= kAlignMinR2 && floor > 0 && win[k].lmin >= floor;
            out.windows += informative[k];
        }
    }
    if (out.windows < kAlignMinWindows) return out;
    // Gauss-Newton on the windows in `use`, from s = 0. False when it does not converge within 8 px.
    auto solve = [&](const std::vector<uint8_t>& use, double& sx, double& sy) {
        sx = sy = 0;
        for (int round = 0; round < kRounds; ++round) {
            if (round > 0) measure(sx, sy);
            double H[4] = {}, g[2] = {};
            for (size_t k = 0; k < K; ++k)
                if (use[k]) {
                    for (int p = 0; p < 4; ++p) H[p] += win[k].Hs[p];
                    for (int p = 0; p < 2; ++p) g[p] += win[k].gs[p];
                }
            double d[2];
            if (!Solve(2, H, g, d)) return false;
            sx += d[0];
            sy += d[1];
            if (!std::isfinite(sx) || !std::isfinite(sy) || std::hypot(sx, sy) > 8) return false;
            if (std::hypot(d[0], d[1]) < 1e-4) return true;
        }
        return false;
    };
    std::vector<uint8_t> half[2] = {std::vector<uint8_t>(K, 0), std::vector<uint8_t>(K, 0)};
    for (int j = 0; j < ny; ++j)
        for (int k = 0; k < nx; ++k) {
            const size_t i = static_cast<size_t>(j) * nx + k;
            half[(j + k) % 2][i] = informative[i];
        }
    double ax = 0, ay = 0, bx = 0, by = 0;
    if (!solve(half[0], ax, ay) || !solve(half[1], bx, by)) return out;
    measure(0, 0);
    if (!solve(informative, out.dx, out.dy)) return out;
    out.converged = true;
    // The windows are now measured at the converged s: their own estimates, and the formal 1-sigma.
    measure(out.dx, out.dy);
    std::vector<double> ex, ey;
    double H[4] = {}, rss = 0, npx = 0;
    for (size_t k = 0; k < K; ++k) {
        if (!informative[k]) continue;
        for (int p = 0; p < 4; ++p) H[p] += win[k].Hs[p];
        rss += win[k].rss;
        npx += kAlignWin * kAlignWin - 4;
        double H4[16], g4[4], d[4];
        std::copy(std::begin(win[k].H4), std::end(win[k].H4), H4);
        std::copy(std::begin(win[k].g4), std::end(win[k].g4), g4);
        if (!Solve(4, H4, g4, d) || !std::isfinite(d[0]) || !std::isfinite(d[1])) continue;
        ex.push_back(out.dx + d[0]);
        ey.push_back(out.dy + d[1]);
    }
    auto spread = [](std::vector<double> v) {
        if (v.size() < 2) return std::numeric_limits<double>::infinity();
        auto mid = v.begin() + static_cast<long>(v.size() / 2);
        std::nth_element(v.begin(), mid, v.end());
        const double med = *mid;
        for (double& e : v) e = std::fabs(e - med);
        std::nth_element(v.begin(), mid, v.end());
        return 1.4826 * *mid / std::sqrt(static_cast<double>(v.size()));
    };
    // Residuals of the smoothed data are correlated over ~4 pi sigma^2 px, so the independent-pixel variance is
    // inflated by that area.
    const double corrArea = std::max(1.0, 4 * M_PI * kAlignPreSmooth * kAlignPreSmooth);
    const double det = H[0] * H[3] - H[1] * H[2], var = npx > 0 ? corrArea * rss / npx : 0;
    const double inf = std::numeric_limits<double>::infinity();
    const double fx = det > 0 ? std::sqrt(var * H[3] / det) : inf, fy = det > 0 ? std::sqrt(var * H[0] / det) : inf;
    out.ux = std::max({spread(ex), fx, 0.5 * std::fabs(ax - bx)});
    out.uy = std::max({spread(ey), fy, 0.5 * std::fabs(ay - by)});
    return out;
}

// Amendment step 4b: a measured R or B shift against G above this is corrected; at or below it, nothing is
// resampled (resampling costs a little sharpness for nothing). Nor is a shift that its own uncertainty does not
// clear by kAlignSigmas: it is not shown to be there.
constexpr double kAlignMin = 0.1, kAlignSigmas = 3.0;

}  // namespace

StepRecord AlignChannels(ser::FloatImage& rgb, int threads) {
    RequireRgb(rgb, "AlignChannels");
    // Measured on a copy smoothed by a 1 px Gaussian: Catmull-Rom resampling cuts white noise's variance to 0.63 at a
    // half-pixel shift (sum of its weights squared) against 1 at a whole one, so on raw data a weak-signal fit is
    // pulled toward half pixels (grey synthetic crescent sliver, no true shift, noise 0.002: +0.27-0.31 px; 0.065 at
    // noise 0.0005). Smoothed first, the noise has little left at the frequencies the interpolation cuts. The
    // correction is applied to the unsmoothed data.
    const ser::FloatImage smooth = ChromaLayer(rgb, kAlignPreSmooth, threads);
    const ChannelShift r = MeasureShift(smooth, 0, threads), b = MeasureShift(smooth, 2, threads);
    StepRecord rec;
    if (r.windows < kAlignMinWindows || b.windows < kAlignMinWindows) {
        rec.skipped = true;
        rec.history = "Channel alignment skipped: " + std::to_string(std::min(r.windows, b.windows)) +
                      " window(s) of 16 px with structure, under the " + std::to_string(kAlignMinWindows) +
                      " needed to measure a dispersion shift and its uncertainty (disk too small); nothing resampled";
        return rec;
    }
    if (!r.converged || !b.converged) {
        rec.skipped = true;
        rec.history = std::string("Channel alignment skipped: the ") + (!r.converged ? "R" : "B") +
                      " shift against G could not be measured (no convergence within 8 px); nothing resampled";
        return rec;
    }
    const double mr = std::hypot(r.dx, r.dy), mb = std::hypot(b.dx, b.dy);
    const double ur = std::hypot(r.ux, r.uy), ub = std::hypot(b.ux, b.uy);
    rec.history = Fmt("Channel alignment: R %+.3f, %+.3f px (+-%.3f), B %+.3f, %+.3f px (+-%.3f) against G, from ", r.dx,
                      r.dy, ur, b.dx, b.dy, ub) +
                  std::to_string(std::min(r.windows, b.windows)) + " windows";
    // Each channel on its own: corrected when its shift exceeds 0.1 px and 3 x its uncertainty.
    const bool fixR = mr > kAlignMin && mr > kAlignSigmas * ur, fixB = mb > kAlignMin && mb > kAlignSigmas * ub;
    if (!fixR && !fixB) {
        rec.skipped = true;
        rec.history += "; within 0.1 px or within 3 sigma of its uncertainty, not applied";
        return rec;
    }
    for (int c : {0, 2}) {
        const ChannelShift& s = c == 0 ? r : b;
        if (!(c == 0 ? fixR : fixB)) continue;
        const std::vector<float> P = ShiftedPlane(rgb, c, s.dx, s.dy, threads);
        for (size_t i = 0; i < P.size(); ++i) rgb.data[i * 3 + static_cast<size_t>(c)] = P[i];
    }
    rec.history += fixR && fixB ? "; R and B resampled onto G (Catmull-Rom)"
                                : (fixR ? "; R resampled onto G (Catmull-Rom), B not" : "; B resampled onto G (Catmull-Rom), R not");
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

namespace {

// The disc's core: disc pixels at or above half the contrast between the sky (its median; 0 without sky) and the
// disc's median, on the 3 x 3 mean, enclosed holes filled (craters, dark mare floors). Maria sit at 0.5-0.7 of the
// highlands on the real Moon and above half the disc median; the limb's and the terminator's half-contrast lines
// are its edge. dist: distance into the core from that edge (+inf when nothing in view is outside it: a disc
// filling the frame).
struct DiscCore {
    std::vector<uint8_t> core;
    std::vector<float> dist;
};

DiscCore MakeDiscCore(const ser::FloatImage& lum1, const LunarMasks& m, int threads) {
    const int w = lum1.width, h = lum1.height;
    const size_t n = Pixels(lum1);
    DiscCore dc;
    dc.core.assign(n, 0);
    std::vector<float> d, s;
    for (size_t i = 0; i < n; ++i) {
        if (m.disk[i]) d.push_back(lum1.data[i]);
        if (!m.sky.empty() && m.sky[i]) s.push_back(lum1.data[i]);
    }
    const double half = 0.5 * (Median(d) + (s.empty() ? 0.0 : Median(s)));
    const ser::FloatImage Ls = Box3(lum1, threads);
    for (size_t i = 0; i < n; ++i) dc.core[i] = m.disk[i] && Ls.data[i] >= half;
    FillHoles(dc.core, w, h);
    dc.dist = DistanceOutside(dc.core, w, h);
    return dc;
}

// Where layer j's boost is at full weight; the statistics of layer j are taken there (its coefficients clear of
// the edge), or over the whole disc when that leaves under 1000 px (a small disc).
std::vector<uint8_t> LayerRegion(const DiscCore& dc, const LunarMasks& m, int j) {
    std::vector<uint8_t> r(dc.core.size());
    size_t k = 0;
    for (size_t i = 0; i < r.size(); ++i) k += r[i] = dc.core[i] && EdgeWeight(dc.dist[i], j) >= 1.0;
    return k >= 1000 ? r : m.disk;
}

// White-noise std from layer 1 (which keeps 0.889 of it): the lower of the sky's and the disc's flattest tiles'.
// Terrain only ever inflates a layer-1 estimate -- taken over the whole disc of a scene with detail out to 1/4
// cycle/px it read 1.5-2.4x the true noise and held the gains near 1 (measured on MoonSynth, blur 1.0-1.5 px) --
// so the disc's value is the 10th percentile over 16 x 16 tiles clear of the edge (the smoothest terrain, maria
// floors), and the sky, which is noise only, caps it. Fair only where the sky's noise is the disc's: on linear
// data with additive noise. After a display transfer it is not (the transfer clips the sky at 0 and is steeper
// on the disc), which is why PrepareMineral measures it before the transfer. Limit: where shot noise makes a
// linear stack's disc noise exceed its sky's, the sky's lower value lets more disc noise through the threshold;
// the gains stay bounded by kLayerGain and the Wiener factor.
double WhiteNoise(const ser::FloatImage& layer1, const DiscCore& dc, const LunarMasks& m) {
    const int w = layer1.width, h = layer1.height;
    const std::vector<uint8_t> r = LayerRegion(dc, m, 0);
    constexpr int kTile = 16;
    std::vector<float> tileNoise, v;
    for (int ty = 0; ty + kTile <= h; ty += kTile)
        for (int tx = 0; tx + kTile <= w; tx += kTile) {
            v.clear();
            for (int y = ty; y < ty + kTile; ++y)
                for (int x = tx; x < tx + kTile; ++x) {
                    const size_t i = static_cast<size_t>(y) * w + x;
                    if (r[i]) v.push_back(layer1.data[i]);
                }
            if (v.size() == static_cast<size_t>(kTile * kTile)) tileNoise.push_back(static_cast<float>(MadStd(v)));
        }
    double white;
    if (tileNoise.size() >= 10) {
        const size_t k = tileNoise.size() / 10;
        std::nth_element(tileNoise.begin(), tileNoise.begin() + static_cast<long>(k), tileNoise.end());
        white = tileNoise[k];
    } else {
        white = LayerNoise(layer1, r);
    }
    if (m.skyCount >= 1000) white = std::min(white, LayerNoise(layer1, m.sky));
    return white / kStarletNoise[0];
}

void RequireLuminance(const ser::FloatImage& lum1, const LunarMasks& m, const char* who) {
    if (lum1.channels != 1 || lum1.data.size() != Pixels(lum1) || m.width != lum1.width || m.height != lum1.height ||
        m.disk.size() != Pixels(lum1))
        throw std::invalid_argument(std::string(who) + ": needs a 1-channel image and masks of its size");
}

}  // namespace

double LuminanceNoise(const ser::FloatImage& lum1, const LunarMasks& m, int threads) {
    RequireLuminance(lum1, m, "LuminanceNoise");
    if (m.diskCount == 0) return 0;
    return WhiteNoise(StarletLayers(lum1, 1, threads)[0], MakeDiscCore(lum1, m, threads), m);
}

StepRecord SharpenLuminanceImage(ser::FloatImage& lum1, const LunarMasks& m, int threads, const std::vector<float>* noise) {
    RequireLuminance(lum1, m, "SharpenLuminanceImage");
    if (noise && noise->size() != Pixels(lum1))
        throw std::invalid_argument("SharpenLuminanceImage: the noise map does not match the image");
    const int w = lum1.width, h = lum1.height;
    const size_t n = Pixels(lum1);
    StepRecord rec;
    if (m.diskCount == 0) {
        rec.skipped = true;
        rec.history = "Sharpen skipped: no lunar disk found to sharpen";
        return rec;
    }
    const DiscCore dc = MakeDiscCore(lum1, m, threads);
    const std::vector<ser::FloatImage> layers = StarletLayers(lum1, kSharpenLayers, threads);
    // The white-noise std at each pixel: the given map, or one value measured on lum1 itself.
    const double white = noise ? 0.0 : WhiteNoise(layers[0], dc, m);
    auto sigma = [&](size_t i) { return noise ? static_cast<double>((*noise)[i]) : white; };
    double gain[kSharpenLayers];
    for (int j = 0; j < kSharpenLayers; ++j) {
        // The layer's own noise limit: a Wiener factor, the share of the layer's energy over the disc that is not
        // noise. A layer that is noise only gets gain 1 (spec step 4: a noise-only gain fails).
        const std::vector<uint8_t> r = LayerRegion(dc, m, j);
        double e = 0, nv = 0;
        size_t k = 0;
        for (size_t i = 0; i < n; ++i)
            if (r[i]) {
                e += static_cast<double>(layers[static_cast<size_t>(j)].data[i]) * layers[static_cast<size_t>(j)].data[i];
                nv += sigma(i) * sigma(i);
                ++k;
            }
        e /= std::max<size_t>(1, k);
        nv *= kStarletNoise[j] * kStarletNoise[j] / std::max<size_t>(1, k);
        const double rho = e > 0 ? std::max(0.0, 1 - nv / e) : 0.0;
        gain[j] = 1 + (kLayerGain[j] - 1) * rho;
    }
    ParallelBands(h, threads, [&](int y0, int y1) {
        for (size_t i = static_cast<size_t>(y0) * w; i < static_cast<size_t>(y1) * w; ++i) {
            double v = lum1.data[i];
            if (dc.core[i])
                for (int j = 0; j < kSharpenLayers; ++j)
                    v += (gain[j] - 1) * EdgeWeight(dc.dist[i], j) *
                         Soft(layers[static_cast<size_t>(j)].data[i], kBoostThreshold * sigma(i) * kStarletNoise[j]);
            lum1.data[i] = static_cast<float>(Shoulder(v));
        }
    });
    if (noise) {
        std::vector<float> d;
        for (size_t i = 0; i < n; ++i)
            if (m.disk[i]) d.push_back((*noise)[i]);
        rec.history = Fmt("Sharpen (luminance): starlet layers 1-4, noise %.3g (disk median, measured before the display "
                          "transfer); gains %.2f %.2f %.2f %.2f (noise-limited)",
                          Median(d), gain[0], gain[1], gain[2], gain[3]);
    } else {
        rec.history = Fmt("Sharpen (luminance): starlet layers 1-4, noise %.3g; gains %.2f %.2f %.2f %.2f (noise-limited)",
                          white, gain[0], gain[1], gain[2], gain[3]);
    }
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



// ---- 5-7. Chroma layer and mineral colour ----

namespace {

// The amendment's chroma blur default (2.5 px) is for frames 1-2k px wide; wider frames scale it with their
// width, so the colour's resolution against the disc stays the same.
double ResolutionScale(int width) { return std::max(1.0, width / 2000.0); }

void RequireParams(const MineralParams& p) {
    if (!(p.strength >= 0) || !(p.chromaBlurSigma >= 0) || !(p.eps >= 0) || !(p.devClamp >= 0) || !(p.satRamp > 0) ||
        !std::isfinite(p.strength) || !std::isfinite(p.chromaBlurSigma))
        throw std::invalid_argument("MineralColour: strength, chroma blur, eps and clamp must be >= 0 and the ramp > 0");
}

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
    RequireParams(p);
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
            // 2. d: each channel's deviation from the channels' mean, relative to it, clamped (one hot or saturated
            //    sample cannot colour its neighbourhood beyond +-devClamp). eps keeps a black sky from dividing
            //    by 0 and, in the numerator's difference form, biases nothing: a neutral pixel gets d = 0
            //    (Ruling R26; C / (mean + eps) - 1 darkened every pixel by k eps / (mean + eps)).
            const double mean = (static_cast<double>(q[0]) + q[1] + q[2]) / 3;
            double d[3];
            for (int c = 0; c < 3; ++c) d[c] = std::clamp((q[c] - mean) / (mean + p.eps), -p.devClamp, p.devClamp);
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
// dMTF(x, m) / dx = m (1 - m) / ((2m - 1) x - m)^2.
double MtfSlope(double x, double m) {
    const double q = (2 * m - 1) * x - m;
    return m * (1 - m) / (q * q);
}

// Spec step 8: the disc's median at a normal display level, and headroom for sharpening and colour above the
// brightest unsaturated sample.
constexpr double kDisplayMedian = 0.35, kDisplayHeadroom = 0.95;

// The transfer's slope dy/dv at a displayed sample y: 0 where y is clipped (at or beyond 0 or 1), else the MTF's
// slope at v / W (recovered by the inverse MTF, balance 1 - m), / W.
double DisplaySlope(double y, const DisplayCurve& c) {
    if (!(y > 0 && y < 1)) return 0;
    return MtfSlope(Mtf(y, 1 - c.balance), c.balance) / c.white;
}

}  // namespace

StepRecord DisplayTransfer(ser::FloatImage& rgb, const LunarMasks& m, const std::vector<uint8_t>& stackClipped,
                           DisplayCurve* curve) {
    if (curve) *curve = DisplayCurve{};
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
    if (curve) *curve = {W, bal};
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

// Step 1 found no disc: every later step says so instead of running (spec anti-goal: a mineral image that hides
// a defect).
constexpr const char* kNoDisk = "no lunar disk distinguished from the sky";

}  // namespace

MineralCache PrepareMineral(const ser::FloatImage& stack, int threads, const Progress& progress) {
    if (stack.channels != 3 || stack.width <= 0 || stack.height <= 0 || stack.data.size() != Pixels(stack) * 3)
        throw std::invalid_argument("Mineral moon needs a 3-channel colour stack (this one has " +
                                    std::to_string(stack.channels) + " channel(s))");
    // NaN is no level at all (an Inf is a saturated sample and is handled as one): refused, with the count, rather
    // than spread by every blur into the output (task ENG review M1).
    const size_t nans = static_cast<size_t>(std::count_if(stack.data.begin(), stack.data.end(), [](float v) { return std::isnan(v); }));
    if (nans > 0)
        throw std::invalid_argument("Mineral moon needs a stack of defined samples (this one has " + std::to_string(nans) +
                                    " NaN sample(s))");
    MineralCache cache;
    MineralSteps steps(progress, kMineralSteps);
    ser::FloatImage rgb = stack;
    std::vector<uint8_t> clipped(stack.data.size());
    for (size_t k = 0; k < stack.data.size(); ++k) clipped[k] = stack.data[k] >= kClippedLuma;
    const size_t n = Pixels(stack);
    bool noDisk = false;
    // Runs `fn`, or with no disc records `name` skipped for that reason.
    auto step = [&](const std::string& name, const std::function<std::string()>& fn) {
        steps.Run(name, cache.stepSeconds,
                  [&] { cache.history.push_back(noDisk ? name + " skipped: " + kNoDisk + "; nothing done" : fn()); });
    };
    steps.Run("Lunar disk", cache.stepSeconds, [&] {
        cache.masks = SegmentLunar(rgb, threads);
        noDisk = cache.masks.diskCount == 0;
        cache.history.push_back(noDisk ? std::string("Lunar disk: none, ") + kNoDisk + " (" + cache.masks.reason +
                                             "); the image is the stack clipped to [0, 1]"
                                       : "Lunar disk: " + Percent(cache.masks.diskCount, n) + " of the frame, sky " +
                                             Percent(cache.masks.skyCount, n));
    });
    if (noDisk) {
        cache.notice = kNoDisk;
        for (float& v : rgb.data) v = std::clamp(v, 0.0f, 1.0f);
    }
    step("Colour calibration", [&] { return CalibrateColour(rgb, cache.masks).history; });
    step("Background", [&] { return NeutraliseBackground(rgb, cache.masks).history; });
    // The sharpening's noise is measured here, on the linear data, where the sky is a fair reference for the
    // disc's noise, and carried through the display transfer's slope at each pixel (task ENG review I1: measured
    // after the transfer, which clips the sky at 0 and is steeper on the disc, it read ~7x low and the disc's
    // noise was boosted x1.53-1.57).
    double linearNoise = 0;
    DisplayCurve curve;
    step("Display transfer", [&] {
        linearNoise = LuminanceNoise(MeanOfChannels(rgb, threads), cache.masks, threads);
        return DisplayTransfer(rgb, cache.masks, clipped, &curve).history;
    });
    step("Channel alignment", [&] { return AlignChannels(rgb, threads).history; });
    step("Sharpen", [&] {
        cache.lum = MeanOfChannels(rgb, threads);
        // L is the channels' mean, so with equal per-channel noise its display noise is the linear L's times the
        // rms of the three channels' slopes.
        std::vector<float> noise(n);
        ParallelBands(rgb.height, threads, [&](int y0, int y1) {
            for (size_t i = static_cast<size_t>(y0) * rgb.width; i < static_cast<size_t>(y1) * rgb.width; ++i) {
                double s2 = 0;
                for (int c = 0; c < 3; ++c) {
                    const double s = DisplaySlope(rgb.data[i * 3 + c], curve);
                    s2 += s * s;
                }
                noise[i] = static_cast<float>(linearNoise * std::sqrt(s2 / 3));
            }
        });
        return SharpenLuminanceImage(cache.lum, cache.masks, threads, &noise).history;
    });
    if (noDisk) cache.lum = MeanOfChannels(rgb, threads);
    steps.Tick();
    cache.display = std::move(rgb);
    return cache;
}

ser::FloatImage ApplyMineralColour(const MineralCache& cache, const MineralParams& p, int threads, StepRecord* record) {
    RequireParams(p);
    ser::FloatImage out = cache.display;
    StepRecord rec;
    if (cache.masks.diskCount == 0) {
        rec.skipped = true;
        rec.history = std::string("Mineral colour skipped: ") + kNoDisk + "; nothing done";
    } else {
        rec = MineralColour(out, cache.lum, p, threads);
    }
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
    out.notice = std::move(cache.notice);
    return out;
}

}  // namespace planetary::stack
