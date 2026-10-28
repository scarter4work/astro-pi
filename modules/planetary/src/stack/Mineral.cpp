#include "Mineral.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>

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
    {
        std::vector<uint8_t> outside(n);
        for (size_t i = 0; i < n; ++i) outside[i] = !m.disk[i];
        std::vector<size_t> hs;
        std::vector<int> hl = Components(outside, w, h, hs);
        std::vector<uint8_t> touches(hs.size(), 0);
        for (int x = 0; x < w; ++x) {
            touches[static_cast<size_t>(hl[static_cast<size_t>(x)])] = 1;
            touches[static_cast<size_t>(hl[static_cast<size_t>(h - 1) * w + x])] = 1;
        }
        for (int y = 0; y < h; ++y) {
            touches[static_cast<size_t>(hl[static_cast<size_t>(y) * w])] = 1;
            touches[static_cast<size_t>(hl[static_cast<size_t>(y) * w + w - 1])] = 1;
        }
        for (size_t i = 0; i < n; ++i)
            if (hl[i] && !touches[static_cast<size_t>(hl[i])]) m.disk[i] = 1;
    }
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

}  // namespace planetary::stack
