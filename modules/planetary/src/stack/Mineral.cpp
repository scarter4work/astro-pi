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
#include "MineralInternal.h"
#include "Parallel.h"

namespace planetary::stack {

using namespace mineral_internal;

namespace {

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
        std::vector<float> P = ShiftedPlane(rgb, c, s.dx, s.dy, threads);
        FillFrameEdge(P, rgb, s.dx, s.dy);
        for (size_t i = 0; i < P.size(); ++i) rgb.data[i * 3 + static_cast<size_t>(c)] = P[i];
    }
    rec.history += fixR && fixB ? "; R and B resampled onto G (Catmull-Rom)"
                                : (fixR ? "; R resampled onto G (Catmull-Rom), B not" : "; B resampled onto G (Catmull-Rom), R not");
    rec.history += "; at the frame's edge, where the shifted sample needs light from outside the frame, the channel's "
                   "ratio to G is taken from the nearest pixels inside";
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
// 15.1% (too strong for the lighter blur). They are kMoonSharpen.gain (defined below, outside this namespace).
// The Sharpen amount a (Task SHARP): each layer's boost (nominal gain - 1) is a x the gain's up to a = 1 (the
// gains above, B9's sharpening), plus (a - 1) x the profile's extra beyond it (kMoonSharpen.extra). Measured on
// real 64.6F against Scott's MineralMoon.png (one grid; the reference's luminance quantile-mapped onto ours, starlet
// rms over the disc interior): at a = 1 our layers 1 / 2 / 3 held 0.37 / 0.68 / 0.94 of the reference's -- the
// deficit is at the finest scales. Scaling the gains alone (extra 0) brought layer 3 to 1.20 by the time layer 1
// reached 0.54 (a 2; a boost leaks into its neighbour's band); extra {3, 1, 0, 0} reaches 0.86 / 1.35 / 1.51 at a 2.
// Its crater rims measure 0.77 / 1.07 / 1.38 / 1.68 of the reference's crispness at a 1 / 1.5 / 2 / 2.5 (Ruling
// R42: default 1.5). {1.5, 0.5}, {1, 1}, {2.5, 0.5} and {5, 1} lie on nearly the same curve at other amounts; adding
// layer 3 or 4 ({0.5, 1.5, 0.5..1, 0..0.5}) only overshot layer 3 sooner.
std::array<double, kSharpenLayers> LayerBoost(double a, const SharpenProfile& p) {
    std::array<double, kSharpenLayers> b{};
    for (int j = 0; j < kSharpenLayers; ++j) {
        const size_t u = static_cast<size_t>(j);
        b[u] = a * (p.gain[u] - 1) + std::max(0.0, a - 1) * p.extra[u];
    }
    return b;
}
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
// A SharpenProfile with a fade fraction f > 0 (planets, Ruling P1) uses min(reach, f R) in its place.
constexpr double kLayerReach[kSharpenLayers] = {2, 6, 14, 30};
// The weight at `dist` px inside the half-contrast line of a fade that starts at `reach` (a layer's reach, or the
// disc-scaled one) and is full at twice that.
double EdgeWeight(double dist, double reach) { return std::clamp((dist - reach) / reach, 0.0, 1.0); }
// Highlight shoulder: above kKnee the sharpened luminance is rolled off smoothly toward kCeiling, so sharpening
// never clips a sample the stack did not (kCeiling sits under kClippedLuma, the clip count's level).
constexpr double kKnee = 0.9, kCeiling = 0.998;

double Shoulder(double x) {
    if (x <= kKnee) return x;
    return kKnee + (kCeiling - kKnee) * std::tanh((x - kKnee) / (kCeiling - kKnee));
}
// Shadow toe (Task B9), the shoulder's counterpart at black: sharpening may darken a pixel linearly down to kToe of its
// unsharpened value v0; below that, v0 kToe exp((v / v0 - kToe) / kToe), continuous with its slope and above 0, so it
// never clips a sample the stack did not. Real 0201-140 (a terminator: shadowed floors beside sunlit rims) went to
// -0.156 on 1.59% of its disc, and the colour step's clip to [0, 1] black-clipped it. Unchanged pixels stay as they
// were (v = v0 is in the linear part); at v0 <= 0 nothing goes below v0.
constexpr double kToe = 0.25;

double Toe(double v, double v0) {
    if (v0 <= 0) return std::max(v, v0);
    const double t = v / v0;
    return t >= kToe ? v : v0 * kToe * std::exp((t - kToe) / kToe);
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

// Where a layer whose fade starts at `reach` has its boost at full weight; the statistics of the layer are taken
// there (its coefficients clear of the edge), or over the whole disc when that leaves under 1000 px (a small disc).
std::vector<uint8_t> LayerRegionAt(const DiscCore& dc, const LunarMasks& m, double reach) {
    std::vector<uint8_t> r(dc.core.size());
    size_t k = 0;
    for (size_t i = 0; i < r.size(); ++i) k += r[i] = dc.core[i] && EdgeWeight(dc.dist[i], reach) >= 1.0;
    return k >= 1000 ? r : m.disk;
}
// The same for layer j's own reach (the Moon's fade).
std::vector<uint8_t> LayerRegion(const DiscCore& dc, const LunarMasks& m, int j) {
    return LayerRegionAt(dc, m, kLayerReach[j]);
}

// White-noise std from layer 1 (which keeps 0.889 of it): the lower of the sky's and the disc's flattest tiles'.
// Terrain only ever inflates a layer-1 estimate -- taken over the whole disc of a scene with detail out to 1/4
// cycle/px it read 1.5-2.4x the true noise and held the gains near 1 (measured on MoonSynth, blur 1.0-1.5 px) --
// so the disc's value is the 10th percentile over 16 x 16 tiles clear of the edge (the smoothest terrain, maria
// floors), and the sky, which is noise only, caps it. Fair only where the sky's noise is the disc's: on linear
// data with additive noise. After a display transfer it is not (the transfer clips the sky at 0 and is steeper
// on the disc), which is why PrepareMineral measures it before the transfer. Limit: where shot noise makes a
// linear stack's disc noise exceed its sky's, the sky's lower value lets more disc noise through the threshold;
// the gains stay bounded by the profile's gain (kMoonSharpen.gain for the Moon) and the Wiener factor.
// The two estimates WhiteNoise takes the lower of: `tiles` from the disc's flattest tiles, `sky` from the sky (0 under
// 1000 px of sky). Both already / 0.889.
void WhiteNoiseParts(const ser::FloatImage& layer1, const DiscCore& dc, const LunarMasks& m, double& tiles, double& sky) {
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
    tiles = white / kStarletNoise[0];
    sky = m.skyCount >= 1000 ? LayerNoise(layer1, m.sky) / kStarletNoise[0] : 0.0;
}

double WhiteNoise(const ser::FloatImage& layer1, const DiscCore& dc, const LunarMasks& m) {
    double tiles = 0, sky = 0;
    WhiteNoiseParts(layer1, dc, m, tiles, sky);
    return sky > 0 ? std::min(tiles, sky) : tiles;
}

// The flattest tenth (at least 10) of the 16 x 16 tiles wholly inside `region`, ranked by layer 1's robust std over
// the tile -- over `norm`'s median there when given (the stack's predicted noise per pixel, so a dark tile, whose shot
// noise is lower, is not taken for a flat one) -- as a pixel mask; empty under 10 tiles. Terrain only raises a tile's
// std, so these are the tiles nearest to noise only (the smoothest mare floors). The noise estimates with no sky
// (final review B, I1) are taken over them.
std::vector<uint8_t> FlatTiles(const ser::FloatImage& layer1, const std::vector<uint8_t>& region, const std::vector<float>* norm) {
    const int w = layer1.width, h = layer1.height;
    constexpr int kTile = 16;
    std::vector<std::pair<float, size_t>> tiles;  // (flatness, top-left pixel)
    std::vector<float> v, nv;
    for (int ty = 0; ty + kTile <= h; ty += kTile)
        for (int tx = 0; tx + kTile <= w; tx += kTile) {
            v.clear();
            nv.clear();
            for (int y = ty; y < ty + kTile; ++y)
                for (int x = tx; x < tx + kTile; ++x) {
                    const size_t i = static_cast<size_t>(y) * w + x;
                    if (!region[i]) continue;
                    v.push_back(layer1.data[i]);
                    if (norm) nv.push_back((*norm)[i]);
                }
            if (v.size() != static_cast<size_t>(kTile * kTile)) continue;
            double s = MadStd(v);
            if (norm) {
                const double d = Median(nv);
                if (!(d > 0)) continue;
                s /= d;
            }
            tiles.push_back({static_cast<float>(s), static_cast<size_t>(ty) * w + tx});
        }
    if (tiles.size() < 10) return {};
    const size_t k = std::max<size_t>(10, tiles.size() / 10);
    std::nth_element(tiles.begin(), tiles.begin() + static_cast<long>(k - 1), tiles.end());  // the k flattest first
    std::vector<uint8_t> mask(Pixels(layer1), 0);
    for (size_t t = 0; t < k; ++t)
        for (int y = 0; y < kTile; ++y)
            std::fill_n(mask.begin() + static_cast<long>(tiles[t].second + static_cast<size_t>(y) * w), kTile, uint8_t{1});
    return mask;
}

// Robust std over the mask of layer / norm per pixel (norm null: of the layer itself).
double NormalisedNoise(const ser::FloatImage& layer, const std::vector<uint8_t>& mask, const std::vector<float>* norm) {
    std::vector<float> v;
    for (size_t i = 0; i < mask.size(); ++i)
        if (mask[i] && (!norm || (*norm)[i] > 0)) v.push_back(norm ? static_cast<float>(layer.data[i] / (*norm)[i]) : layer.data[i]);
    return MadStd(v);
}

// The layer shape used when neither the sky nor the disc can measure it: the top of the real-data range (Ruling R33:
// real Moons' sky layers 2-4 read 1.6-1.7x, 1.9-2.2x, 2.4-3.1x the white prediction), so it errs toward less boost.
constexpr double kAssumedShape[kSharpenLayers] = {1, 1.7, 2.2, 3.1};
constexpr const char* kShapeSky = "the sky's";
constexpr const char* kShapeTiles = "the disc's flattest tiles'";
constexpr const char* kShapeAssumed = "assumed, the real-data range's top";

void RequireLuminance(const ser::FloatImage& lum1, const LunarMasks& m, const char* who) {
    if (lum1.channels != 1 || lum1.data.size() != Pixels(lum1) || m.width != lum1.width || m.height != lum1.height ||
        m.disk.size() != Pixels(lum1))
        throw std::invalid_argument(std::string(who) + ": needs a 1-channel image and masks of its size");
}

}  // namespace

std::array<double, 4> LayerNoiseShape(const ser::FloatImage& lum1, const LunarMasks& m, int threads,
                                      const std::vector<float>* norm, std::string* source) {
    RequireLuminance(lum1, m, "LayerNoiseShape");
    if (norm && norm->size() != Pixels(lum1)) throw std::invalid_argument("LayerNoiseShape: the noise map does not match the image");
    std::array<double, 4> shape{1, 1, 1, 1};
    if (m.diskCount == 0) {
        if (source) *source = "white, no disc";
        return shape;
    }
    const std::vector<ser::FloatImage> layers = StarletLayers(lum1, kSharpenLayers, threads);
    const std::vector<float> depth = DistanceOutside(m.sky, lum1.width, lum1.height);  // into the sky from its edge
    DiscCore dc;  // made only when some layer has too little sky
    std::string from[kSharpenLayers] = {kShapeSky, kShapeSky, kShapeSky, kShapeSky};
    for (int j = 1; j < kSharpenLayers; ++j) {
        const size_t u = static_cast<size_t>(j);
        // Layer j and layer 1 over the same sky, clear of the disc by layer j's reach.
        std::vector<uint8_t> r(m.sky.size());
        size_t k = 0;
        for (size_t i = 0; i < r.size(); ++i) k += r[i] = m.sky[i] && depth[i] > kLayerReach[j];
        const double white = k >= 1000 ? LayerNoise(layers[0], r) / kStarletNoise[0] : 0.0;
        if (white > 0) {
            shape[u] = std::max(1.0, LayerNoise(layers[u], r) / (kStarletNoise[j] * white));
            continue;
        }
        // Too little sky (a disc filling the frame; final review B, I1): white ratios here let a stack's correlated
        // noise through layer 2 as signal (flat disc, 0.6 px correlation: noise gain 1.24 at the default, 1.89 at 4).
        // The disc's flattest tiles where layer j is at full weight measure it instead. Picking the tiles by layer 1
        // reads layer 1 low there and terrain only adds to layer j, so the factor errs high: less boost, never more.
        if (dc.core.empty()) dc = MakeDiscCore(lum1, m, threads);
        const std::vector<uint8_t> tiles = FlatTiles(layers[0], LayerRegion(dc, m, j), norm);
        const double w1 = tiles.empty() ? 0.0 : NormalisedNoise(layers[0], tiles, norm) / kStarletNoise[0];
        if (w1 > 0) {
            shape[u] = std::max(1.0, NormalisedNoise(layers[u], tiles, norm) / (kStarletNoise[j] * w1));
            from[j] = kShapeTiles;
        } else {
            shape[u] = kAssumedShape[j];
            from[j] = kShapeAssumed;
        }
    }
    if (source) {
        if (from[1] == from[2] && from[2] == from[3])
            *source = from[1];
        else
            *source = "layer 2 " + from[1] + ", 3 " + from[2] + ", 4 " + from[3];
    }
    return shape;
}

namespace {

// Ruling R35: the per-pixel white-noise std of the calibrated linear luminance `lin` from the stack's noise
// (PrepareMineral): sqrt(model(t) alpha2), t the luma of the stack as it came (before calibration), scaled once
// so that its median over the sky (clear of the disc by layer 1's reach) is the sky's measured white-noise std on
// `lin` (layer 1's robust std / 0.889). With under 1000 px of such sky (a disc filling the frame; final review B, I1)
// it is scaled on the disc's flattest tiles instead (FlatTiles, ranked by the map itself): there `lin`'s layer 1 over
// the map, robust std / 0.889. Terrain only raises that (less boost); picking tiles by it lowers it (the flattest
// tenth of pure noise reads ~0.85 of it), which the layer shape, taken over tiles picked the same way, reads high
// by as much, so a layer 2-4 threshold stays at its noise. `on` names where it was scaled. Empty, with `why` set,
// when it cannot be had.
std::vector<float> StackNoiseMap(const ser::FloatImage& stack, const ser::FloatImage& lin, const LunarMasks& m,
                                 const StackNoise& sn, int threads, double& scale, std::string& on, std::string& why) {
    const int w = stack.width, h = stack.height;
    if (!sn.Usable()) {
        why = "the stack's noise model is not usable (unmeasured, zero, or no frame-weight grid)";
        return {};
    }
    if (sn.width != w || sn.height != h) {
        why = "the stack's noise model is for a " + std::to_string(sn.width) + "x" + std::to_string(sn.height) + " stack";
        return {};
    }
    const std::vector<float> depth = DistanceOutside(m.sky, w, h);
    std::vector<uint8_t> r(m.sky.size());
    size_t k = 0;
    for (size_t i = 0; i < r.size(); ++i) k += r[i] = m.sky[i] && depth[i] > kLayerReach[0];
    std::vector<float> map(Pixels(stack));
    ParallelBands(h, threads, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x) {
                const size_t i = static_cast<size_t>(y) * w + x;
                double t = 0;
                for (int c = 0; c < 3; ++c) t += sn.lumaWeights[static_cast<size_t>(c)] * stack.data[i * 3 + c];
                map[i] = static_cast<float>(std::sqrt(std::max(0.0, sn.model.Lookup(t) * sn.Alpha2At(x, y))));
            }
    });
    const ser::FloatImage layer1 = StarletLayers(lin, 1, threads)[0];
    if (k >= 1000) {
        std::vector<float> skyPred;
        for (size_t i = 0; i < r.size(); ++i)
            if (r[i]) skyPred.push_back(map[i]);
        const double pred = Median(skyPred);
        const double measured = LayerNoise(layer1, r) / kStarletNoise[0];
        if (!(pred > 0) || !(measured > 0)) {
            why = "the sky shows no noise to scale the stack's noise model by";
            return {};
        }
        scale = measured / pred;
        on = kShapeSky;
    } else {
        const std::vector<uint8_t> tiles = FlatTiles(layer1, LayerRegion(MakeDiscCore(lin, m, threads), m, 0), &map);
        scale = tiles.empty() ? 0.0 : NormalisedNoise(layer1, tiles, &map) / kStarletNoise[0];
        if (!(scale > 0)) {
            why = "under 1000 px of sky and no 10 disc tiles with noise to scale the stack's noise model by";
            return {};
        }
        on = kShapeTiles;
    }
    for (float& v : map) v = static_cast<float>(v * scale);
    return map;
}

}  // namespace

const SharpenProfile kMoonSharpen = {{1.5, 2.5, 1.0, 1.0}, {3.0, 1.0, 0.0, 0.0}, 0.0};

StepRecord SharpenLuminanceImage(ser::FloatImage& lum1, const LunarMasks& m, int threads, const std::vector<float>* noise,
                                 const std::array<double, 4>* layerShape, double amount, const SharpenProfile& profile) {
    RequireLuminance(lum1, m, "SharpenLuminanceImage");
    if (noise && noise->size() != Pixels(lum1))
        throw std::invalid_argument("SharpenLuminanceImage: the noise map does not match the image");
    if (!(amount >= 0) || !std::isfinite(amount))
        throw std::invalid_argument("SharpenLuminanceImage: the amount must be finite and >= 0");
    bool profileOk = profile.fadeRadiusFraction >= 0 && std::isfinite(profile.fadeRadiusFraction);
    for (size_t u = 0; u < profile.gain.size(); ++u)
        profileOk = profileOk && profile.gain[u] >= 0 && std::isfinite(profile.gain[u]) && profile.extra[u] >= 0 &&
                    std::isfinite(profile.extra[u]);
    if (!profileOk)
        throw std::invalid_argument("SharpenLuminanceImage: the profile's gains, extras and fade fraction must be finite and >= 0");
    const int w = lum1.width, h = lum1.height;
    const size_t n = Pixels(lum1);
    StepRecord rec;
    if (m.diskCount == 0) {
        rec.skipped = true;
        rec.history = "Sharpen skipped: no lunar disk found to sharpen";
        return rec;
    }
    if (amount == 0) {
        rec.skipped = true;
        rec.history = "Sharpen (luminance): amount 0.00, nothing sharpened (L unchanged)";
        return rec;
    }
    const std::array<double, kSharpenLayers> boost = LayerBoost(amount, profile);
    // Where each layer's limb fade starts (px inside the half-contrast line): its reach, or with a fade fraction f > 0
    // (Ruling P1), min(reach, f R), R the disc's equivalent radius.
    const double f = profile.fadeRadiusFraction;
    const double discR = std::sqrt(static_cast<double>(m.diskCount) / M_PI);
    double reach[kSharpenLayers];
    for (int j = 0; j < kSharpenLayers; ++j) reach[j] = f > 0 ? std::min(kLayerReach[j], f * discR) : kLayerReach[j];
    const DiscCore dc = MakeDiscCore(lum1, m, threads);
    const std::vector<ser::FloatImage> layers = StarletLayers(lum1, kSharpenLayers, threads);
    // The white-noise std at each pixel: the given map, or one value measured on lum1 itself.
    const double white = noise ? 0.0 : WhiteNoise(layers[0], dc, m);
    auto sigma = [&](size_t i) { return noise ? static_cast<double>((*noise)[i]) : white; };
    // Each layer's noise per unit of white-noise std: the white ratios, times the measured shape (Ruling R33).
    double kn[kSharpenLayers];
    for (int j = 0; j < kSharpenLayers; ++j) kn[j] = kStarletNoise[j] * (layerShape ? (*layerShape)[static_cast<size_t>(j)] : 1.0);
    double gain[kSharpenLayers];
    for (int j = 0; j < kSharpenLayers; ++j) {
        // The layer's own noise limit: a Wiener factor, the share of the layer's energy over the disc that is not
        // noise. A layer that is noise only gets gain 1 (spec step 4: a noise-only gain fails).
        const std::vector<uint8_t> r = LayerRegionAt(dc, m, reach[j]);
        double e = 0, nv = 0;
        size_t k = 0;
        for (size_t i = 0; i < n; ++i)
            if (r[i]) {
                e += static_cast<double>(layers[static_cast<size_t>(j)].data[i]) * layers[static_cast<size_t>(j)].data[i];
                nv += sigma(i) * sigma(i);
                ++k;
            }
        e /= std::max<size_t>(1, k);
        nv *= kn[j] * kn[j] / std::max<size_t>(1, k);
        const double rho = e > 0 ? std::max(0.0, 1 - nv / e) : 0.0;
        gain[j] = 1 + boost[static_cast<size_t>(j)] * rho;
    }
    ParallelBands(h, threads, [&](int y0, int y1) {
        for (size_t i = static_cast<size_t>(y0) * w; i < static_cast<size_t>(y1) * w; ++i) {
            const double v0 = lum1.data[i];
            double v = v0;
            if (dc.core[i])
                for (int j = 0; j < kSharpenLayers; ++j)
                    v += (gain[j] - 1) * EdgeWeight(dc.dist[i], reach[j]) *
                         Soft(layers[static_cast<size_t>(j)].data[i], kBoostThreshold * sigma(i) * kn[j]);
            lum1.data[i] = static_cast<float>(Shoulder(Toe(v, v0)));
        }
    });
    if (noise) {
        std::vector<float> d;
        for (size_t i = 0; i < n; ++i)
            if (m.disk[i]) d.push_back((*noise)[i]);
        rec.history = Fmt("Sharpen (luminance): amount %.2f, starlet layers 1-4, noise %.3g (disk median, measured before the "
                          "display transfer); gains %.2f %.2f %.2f %.2f (noise-limited)",
                          amount, Median(d), gain[0], gain[1], gain[2], gain[3]);
    } else {
        rec.history = Fmt("Sharpen (luminance): amount %.2f, starlet layers 1-4, noise %.3g; gains %.2f %.2f %.2f %.2f "
                          "(noise-limited)",
                          amount, white, gain[0], gain[1], gain[2], gain[3]);
    }
    if (f > 0)
        rec.history += Fmt("; limb fade scaled to the disc (radius %.1f px): from %.2g %.2g %.2g %.2g px inside the edge",
                           discR, reach[0], reach[1], reach[2], reach[3]);
    return rec;
}

// ---- 5-7. Chroma layer and mineral colour ----

namespace {

// The amendment's chroma blur default (2.5 px) is for frames 1-2k px wide; wider frames scale it with their
// width, so the colour's resolution against the disc stays the same.
double ResolutionScale(int width) { return std::max(1.0, width / 2000.0); }

void RequireParams(const MineralParams& p) {
    if (!(p.strength >= 0) || !(p.chromaBlurSigma >= 0) || !(p.sharpen >= 0) || !(p.eps >= 0) || !(p.devClamp >= 0) ||
        !(p.satRamp > 0) || !std::isfinite(p.strength) || !std::isfinite(p.chromaBlurSigma) || !std::isfinite(p.sharpen))
        throw std::invalid_argument("MineralColour: strength, chroma blur, sharpen, eps and clamp must be >= 0 and the ramp > 0");
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

double HighlightThreshold(const ser::FloatImage& rgb, const LunarMasks& m, double chromaBlurSigma, int threads) {
    RequireRgb(rgb, "HighlightThreshold");
    if (m.disk.size() != Pixels(rgb)) throw std::invalid_argument("HighlightThreshold: the masks must be the image's size");
    const double scott = AutoSatThreshold(rgb);
    // The fade's own variable: max(C_R, C_G) of the chroma layer, as MineralColour forms it.
    const ser::FloatImage C = ChromaLayer(rgb, chromaBlurSigma * ResolutionScale(rgb.width), threads);
    std::vector<float> v;
    for (size_t i = 0; i < m.disk.size(); ++i)
        if (m.disk[i]) v.push_back(std::max(C.data[i * 3], C.data[i * 3 + 1]));
    if (v.empty()) return scott;
    const size_t k = static_cast<size_t>(0.99 * static_cast<double>(v.size() - 1));
    std::nth_element(v.begin(), v.begin() + static_cast<long>(k), v.end());
    return std::min(scott, static_cast<double>(v[k]));
}

ColourCounts mineral_internal::ColourTransfer(ser::FloatImage& rgb, const ser::FloatImage& lumSharp, const MineralParams& p,
                                              int threads) {
    RequireRgb(rgb, "MineralColour");
    if (lumSharp.channels != 1 || lumSharp.width != rgb.width || lumSharp.height != rgb.height ||
        lumSharp.data.size() != Pixels(rgb))
        throw std::invalid_argument("MineralColour: the luminance must be one channel of the image's size");
    RequireParams(p);
    const double sigma = p.scaleBlurWithWidth ? p.chromaBlurSigma * ResolutionScale(rgb.width) : p.chromaBlurSigma;
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
    return {sigma, thr, faded, held};
}

StepRecord MineralColour(ser::FloatImage& rgb, const ser::FloatImage& lumSharp, const MineralParams& p, int threads) {
    const ColourCounts c = ColourTransfer(rgb, lumSharp, p, threads);
    const size_t n = Pixels(rgb);
    StepRecord rec;
    rec.history = Fmt("Mineral colour: k %.2f, chroma blur %.2f px, eps %.4f, clamp +-%.3f, highlight fade from %.4f over %.3f",
                      p.strength, c.sigma, p.eps, p.devClamp, c.threshold, p.satRamp) +
                  "; " + Percent(c.faded, n) + " of pixels faded, " + Percent(c.held, n) + " held in gamut";
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
constexpr double kDisplayHeadroom = 0.95;  // the Moon's median target, 0.35, is DisplayTransfer's default

// The transfer's slope dy/dv at a displayed sample y: 0 where y is clipped (at or beyond 0 or 1), else the MTF's
// slope at v / W (recovered by the inverse MTF, balance 1 - m), / W.
double DisplaySlope(double y, const DisplayCurve& c) {
    if (!(y > 0 && y < 1)) return 0;
    return MtfSlope(Mtf(y, 1 - c.balance), c.balance) / c.white;
}

}  // namespace

StepRecord DisplayTransfer(ser::FloatImage& rgb, const LunarMasks& m, const std::vector<uint8_t>& stackClipped,
                           DisplayCurve* curve, double medianTarget) {
    if (curve) *curve = DisplayCurve{};
    RequireMasks(rgb, m, "DisplayTransfer");
    if (!stackClipped.empty() && stackClipped.size() != rgb.data.size())
        throw std::invalid_argument("DisplayTransfer: clipped flags do not match the image");
    if (!(medianTarget < 1) || !std::isfinite(medianTarget))
        throw std::invalid_argument("DisplayTransfer: the median target must be finite and under 1 (<= 0: linear)");
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
    if (medianTarget <= 0) {
        // Linear (spec call 4, a planet: captures are exposed to fill the histogram and planetary images are shown
        // linear): balance 0.5, y = clamp(v / W, 0, 1), W the white point that puts the brightest sample the stack did
        // not clip at kDisplayHeadroom -- the rule below at balance 0.5, where W = top / 0.95 exactly. Calibration may
        // have lifted that sample above 1.0 (R or B scaled up); it lands at 0.95 all the same.
        if (!(top > 0)) {
            rec.skipped = true;
            rec.history = Fmt("Display transfer skipped: no usable signal (brightest unsaturated sample %.3g); "
                              "samples clipped to [0, 1]",
                              top);
            for (float& v : rgb.data) v = std::clamp(v, 0.0f, 1.0f);
            return rec;
        }
        const double W = top / kDisplayHeadroom;
        for (float& v : rgb.data) v = static_cast<float>(std::clamp(v / W, 0.0, 1.0));
        if (curve) *curve = {W, 0.5};
        rec.history = Fmt("Display transfer: linear (no midtones stretch), white point %.5f; disk median %.4f -> %.4f, "
                          "brightest unsaturated sample %.4f -> %.3f",
                          W, med, med / W, top, top / W);
        return rec;
    }
    if (!(med > 0) || !(top > med)) {
        rec.skipped = true;
        rec.history = Fmt("Display transfer skipped: no usable signal (disk median %.3g, brightest unsaturated sample %.3g); "
                          "samples clipped to [0, 1]",
                          med, top);
        for (float& v : rgb.data) v = std::clamp(v, 0.0f, 1.0f);
        return rec;
    }
    // One curve for all three channels (a linked stretch, as an STF or HistogramTransformation applies it):
    // x = v / W clipped to [0, 1], then the MTF whose balance puts the disc's median at medianTarget. W is
    // the smallest white point >= the brightest unsaturated sample at which that sample lands at or under
    // kDisplayHeadroom: the transfer clips nothing the stack did not, and the colour step and the sharpening
    // keep their room. As W grows (the median held), the brightest sample's level falls monotonically toward
    // rho / (rho + (1 - t) / t), rho = its ratio to the median -- under 1 always, so W exists whenever that
    // limit is under the headroom (rho < ~35); otherwise W = 1e4 x the brightest and it sits at the limit.
    auto levelOfTop = [&](double W) { return Mtf(top / W, MtfBalance(med / W, medianTarget)); };
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
    const double W = std::exp(lo), bal = MtfBalance(med / W, medianTarget);
    for (float& v : rgb.data) v = static_cast<float>(Mtf(v / W, bal));
    if (curve) *curve = {W, bal};
    rec.history = Fmt("Display transfer: linked midtones %.5f, white point %.5f; disk median %.4f -> %.2f, "
                      "brightest unsaturated sample %.4f -> %.3f",
                      bal, W, med, medianTarget, top, Mtf(top / W, bal));
    return rec;
}

namespace {

// Step 1 found no disc: every later step says so instead of running (spec anti-goal: a mineral image that hides
// a defect).
constexpr const char* kNoDisk = "no lunar disk distinguished from the sky";

// The Moon's display: the disc's median at 0.35 (spec step 8).
constexpr double kMoonDisplayMedian = 0.35;

constexpr const char* kMineralProduct = "Mineral moon";

// The finish steps' skip reason with no disc in the cache: the prepare's own (cache.notice: the Moon's kNoDisk, or the
// planet's), kNoDisk for a cache without one.
std::string NoDiscReason(const MineralCache& cache) { return cache.notice.empty() ? kNoDisk : cache.notice; }

}  // namespace

MineralCache mineral_internal::PrepareDisc(const ser::FloatImage& stack, int threads, const Progress& progress,
                                           const StackNoise* stackNoise, const DiscPipeline& kind) {
    if (stack.channels != 3 || stack.width <= 0 || stack.height <= 0 || stack.data.size() != Pixels(stack) * 3)
        throw std::invalid_argument(std::string(kind.product) + " needs a 3-channel colour stack (this one has " +
                                    std::to_string(stack.channels) + " channel(s))");
    // A NaN or an infinite sample is no level at all: refused, with the count, rather than spread by every blur into
    // the output (task ENG review M1; final review B, m1: one -Inf in the sky went into the sky plane's sums and made
    // every output sample NaN). RunStack's stacks hold none (a weighted mean of normalised samples).
    const size_t bad = static_cast<size_t>(std::count_if(stack.data.begin(), stack.data.end(), [](float v) { return !std::isfinite(v); }));
    if (bad > 0)
        throw std::invalid_argument(std::string(kind.product) + " needs a stack of finite samples (this one has " +
                                    std::to_string(bad) + " NaN or infinite sample(s))");
    MineralCache cache;
    PipelineSteps steps(progress, kind.phase, kind.product, kPipelineSteps);
    ser::FloatImage rgb = stack;
    std::vector<uint8_t> clipped(stack.data.size());
    for (size_t k = 0; k < stack.data.size(); ++k) clipped[k] = stack.data[k] >= kClippedLuma;
    const size_t n = Pixels(stack);
    bool noDisk = false;
    // Runs `fn`, or with no disc records `name` skipped for that reason.
    auto step = [&](const std::string& name, const std::function<std::string()>& fn) {
        steps.Run(name, cache.stepSeconds,
                  [&] { cache.history.push_back(noDisk ? name + " skipped: " + kind.noDisc + "; nothing done" : fn()); });
    };
    steps.Run(kind.discStep, cache.stepSeconds, [&] {
        cache.masks = kind.segment ? kind.segment(rgb, threads) : SegmentLunar(rgb, threads);
        noDisk = cache.masks.diskCount == 0;
        const std::string size =
            kind.discSize && !noDisk
                ? Fmt("%.0f px (equivalent radius %.1f px), ", static_cast<double>(cache.masks.diskCount),
                      std::sqrt(cache.masks.diskCount / M_PI))
                : std::string();
        if (!noDisk && kind.caution) cache.notice = kind.caution(cache.masks);
        cache.history.push_back(noDisk ? std::string(kind.discStep) + ": none, " + kind.noDisc + " (" + cache.masks.reason +
                                             "); the image is the stack clipped to [0, 1]"
                                       : std::string(kind.discStep) + ": " + size + Percent(cache.masks.diskCount, n) +
                                             " of the frame, sky " + Percent(cache.masks.skyCount, n) +
                                             (cache.notice.empty() ? std::string() : "; " + cache.notice));
    });
    if (noDisk) {
        cache.notice = kind.noDisc;
        for (float& v : rgb.data) v = std::clamp(v, 0.0f, 1.0f);
    }
    step("Colour calibration", [&] { return kind.calibrate(rgb, cache.masks).history; });
    step("Background", [&] { return NeutraliseBackground(rgb, cache.masks).history; });
    // The sharpening's noise is measured here, on the linear data, where the sky is a fair reference for the
    // disc's noise, and carried through the display transfer's slope at each pixel (task ENG review I1: measured
    // after the transfer, which clips the sky at 0 and is steeper on the disc, it read ~7x low and the disc's
    // noise was boosted x1.53-1.57).
    // On a bright disc the sky's noise is a lower bound only (shot noise: real discs 7-10x their sky's), so with the
    // stack's own noise (Ruling R35) the level and the frame count give it per pixel, scaled to the sky's; and the
    // noise's spread over the starlet layers is measured on the sky (Ruling R33: a stack's noise is correlated).
    // With no sky (a disc filling the frame) both come from the disc's flattest tiles instead (final review B, I1).
    double linearNoise = 0, noiseScale = 0;
    std::vector<float> linearMap;
    std::string noiseSource, shapeSource;
    std::array<double, 4> shape{1, 1, 1, 1};
    DisplayCurve curve;
    step("Display transfer", [&] {
        const ser::FloatImage linL = MeanOfChannels(rgb, threads);
        std::string why = "no stack noise model given", scaledOn;
        if (stackNoise) linearMap = StackNoiseMap(stack, linL, cache.masks, *stackNoise, threads, noiseScale, scaledOn, why);
        shape = LayerNoiseShape(linL, cache.masks, threads, linearMap.empty() ? nullptr : &linearMap, &shapeSource);
        if (!linearMap.empty()) {
            noiseSource = "noise: the stack's noise model per pixel (level and frame count), scaled to " + scaledOn +
                          Fmt(" noise by %.3g", noiseScale);
        } else {
            // Without the stack's model, one level everywhere (Ruling R43): the higher of the sky's, a lower bound on
            // a bright disc's (shot noise: a flat shot-noise disc's noise gain read 2.15 at the default and 5.74 at 4
            // on it), and the disc's flattest tiles', which terrain only raises. Erring high sharpens less, never more.
            double tiles = 0, sky = 0;
            WhiteNoiseParts(StarletLayers(linL, 1, threads)[0], MakeDiscCore(linL, cache.masks, threads), cache.masks,
                            tiles, sky);
            linearNoise = std::max(tiles, sky);
            if (sky <= 0)
                noiseSource = std::string("noise: ") + kShapeTiles + " (" + why + "; under 1000 px of sky)";
            else
                noiseSource = Fmt("noise: %.3g, ", linearNoise) + (sky >= tiles ? kShapeSky : kShapeTiles) +
                              Fmt(", the higher of the sky's (%.3g) and the disc's flattest tiles' (%.3g) (", sky, tiles) +
                              why + ")";
        }
        return DisplayTransfer(rgb, cache.masks, clipped, &curve, kind.displayMedian).history;
    });
    step("Channel alignment", [&] { return kind.align(rgb, cache.masks).history; });
    cache.lumBase = MeanOfChannels(rgb, threads);
    if (!noDisk) {
        // The sharpening's noise at display scale, kept for the Sharpen slider: L is the channels' mean, so with equal
        // per-channel noise its display noise is the linear L's times the rms of the three channels' slopes.
        cache.noise.resize(n);
        ParallelBands(rgb.height, threads, [&](int y0, int y1) {
            for (size_t i = static_cast<size_t>(y0) * rgb.width; i < static_cast<size_t>(y1) * rgb.width; ++i) {
                double s2 = 0;
                for (int c = 0; c < 3; ++c) {
                    const double s = DisplaySlope(rgb.data[i * 3 + c], curve);
                    s2 += s * s;
                }
                cache.noise[i] = static_cast<float>((linearMap.empty() ? linearNoise : linearMap[i]) * std::sqrt(s2 / 3));
            }
        });
        cache.layerShape = shape;
        cache.sharpenNote = "; " + noiseSource +
                            Fmt("; layer noise over white %.2f %.2f %.2f %.2f", shape[0], shape[1], shape[2], shape[3]) +
                            " (" + shapeSource + ")";
        // The fade threshold depends on the display RGB only: measured once here, not on every slider move (the
        // mineral moon's only: the planet colour has no highlight fade).
        if (kind.highlightThreshold)
            cache.satThreshold = HighlightThreshold(rgb, cache.masks, MineralParams{}.chromaBlurSigma, threads);
    }
    steps.Tick();
    cache.display = std::move(rgb);
    return cache;
}

MineralCache PrepareMineral(const ser::FloatImage& stack, int threads, const Progress& progress, const StackNoise* stackNoise) {
    const DiscPipeline moon{kPhaseMineral,
                            kMineralProduct,
                            "Lunar disk",
                            kNoDisk,
                            false,
                            [](ser::FloatImage& rgb, const LunarMasks& m) { return CalibrateColour(rgb, m); },
                            [threads](ser::FloatImage& rgb, const LunarMasks&) { return AlignChannels(rgb, threads); },
                            kMoonDisplayMedian,
                            true};
    return PrepareDisc(stack, threads, progress, stackNoise, moon);
}

ser::FloatImage SharpenedLuminance(const MineralCache& cache, double amount, int threads, StepRecord* record,
                                   const SharpenProfile& profile) {
    ser::FloatImage L = cache.lumBase;
    StepRecord rec;
    if (cache.masks.diskCount == 0) {
        rec.skipped = true;
        rec.history = "Sharpen skipped: " + NoDiscReason(cache) + "; nothing done";
    } else {
        rec = SharpenLuminanceImage(L, cache.masks, threads, &cache.noise, &cache.layerShape, amount, profile);
        if (!rec.skipped) rec.history += cache.sharpenNote;
    }
    if (record) *record = std::move(rec);
    return L;
}

namespace {

// Steps 5-7 on a copy of cache.display with the sharp luminance L (ApplyMineralColour and FinishMineral alike).
ser::FloatImage ColourOn(const MineralCache& cache, const ser::FloatImage& L, const MineralParams& p, int threads,
                         StepRecord& rec) {
    ser::FloatImage out = cache.display;
    if (cache.masks.diskCount == 0) {
        rec.skipped = true;
        rec.history = "Mineral colour skipped: " + NoDiscReason(cache) + "; nothing done";
    } else {
        MineralParams q = p;
        if (q.satThreshold < 0 && cache.satThreshold >= 0) q.satThreshold = cache.satThreshold;
        rec = MineralColour(out, L, q, threads);
    }
    return out;
}

}  // namespace

ser::FloatImage ApplyMineralColour(const MineralCache& cache, const MineralParams& p, int threads, StepRecord* record,
                                   StepRecord* sharpenRecord) {
    RequireParams(p);
    StepRecord rec;
    ser::FloatImage out = ColourOn(cache, SharpenedLuminance(cache, p.sharpen, threads, sharpenRecord), p, threads, rec);
    if (record) *record = std::move(rec);
    return out;
}

MineralOutcome FinishMineral(const MineralCache& cache, const MineralParams& params, int threads, const Progress& progress) {
    RequireParams(params);
    MineralOutcome out;
    StepRecord sharpen, colour;
    auto t0 = std::chrono::steady_clock::now();
    const ser::FloatImage L = SharpenedLuminance(cache, params.sharpen, threads, &sharpen);
    const double sharpenSecs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    PipelineSteps(progress, kPhaseMineral, kMineralProduct, kPipelineSteps).Report(kPipelineSteps - 1);
    t0 = std::chrono::steady_clock::now();
    out.image = ColourOn(cache, L, params, threads, colour);
    const double colourSecs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    PipelineSteps(progress, kPhaseMineral, kMineralProduct, kPipelineSteps).Report(kPipelineSteps);
    out.history = cache.history;
    out.history.push_back(sharpen.history);
    out.history.push_back(colour.history);
    out.stepSeconds = cache.stepSeconds;
    out.stepSeconds.push_back({"Sharpen", sharpenSecs});
    out.stepSeconds.push_back({"Mineral colour", colourSecs});
    out.notice = cache.notice;
    out.clipped = cache.masks.diskCount == 0;
    return out;
}

MineralOutcome MakeMineralMoon(const ser::FloatImage& stack, int threads, const Progress& progress, const MineralParams& params,
                               const StackNoise* stackNoise) {
    RequireParams(params);
    return FinishMineral(PrepareMineral(stack, threads, progress, stackNoise), params, threads, progress);
}

}  // namespace planetary::stack
