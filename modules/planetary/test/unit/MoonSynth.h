#pragma once
// Synthetic lunar scenes for the mineral-moon tests (Part B, Tasks B2-B7; Ruling P1: created with B2).
// A lit disc with maria and highlands of known true tint, fine albedo texture, a phase terminator, and
// what a real stack adds on top of it -- a colour cast, a pedestal, a linear sky gradient, seeing blur,
// noise, saturated highlights, chroma noise along the left frame edge, and per-channel offsets
// (atmospheric dispersion). The scene is analytic (sums of sinusoids, an erf limb), so a channel offset is
// an exact re-evaluation, not a resampling, and `truth` is known at every pixel.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include "Debayer.h"
#include "Parallel.h"

namespace planetary::testing {

struct MoonSceneSpec {
    int width = 640, height = 480;
    double cx = 320, cy = 240, radius = 180;
    // Sun's phase angle in degrees: 0 full, 90 quarter, > 90 crescent; the sun is toward +x.
    double phaseDeg = 0;
    double mariaFraction = 0.35;   // share of the disc that is mare
    double mariaAlbedo = 0.65;     // mare brightness relative to highlands
    // True tints (R, G, B), relative to G (spec "True tints").
    std::array<double, 3> mariaTint{0.97, 1.00, 1.04}, highlandTint{1.04, 1.00, 0.95};
    double tintVariation = 0.008;  // smooth per-channel tint modulation inside each terrain (relative std)
    double textureAmplitude = 0.12;  // relative std of the fine albedo texture
    // Its highest frequency (cycles/px). The truth is the image the optics deliver, which is band-limited: lucky
    // imaging samples at about twice the optics' Nyquist, so 1/4 cycle/px. Detail at 1/4 - 1/2 cycle/px under
    // seeing blur of 1.5 px keeps under 6% of its amplitude, below the noise, and no sharpener restores it.
    double textureMaxFreq = 0.25;
    double level = 0.30;           // highland G at full illumination (linear)
    std::array<double, 3> cast{1, 1, 1};      // multiplies the scene light per channel
    std::array<double, 3> pedestal{0, 0, 0};  // added per channel (sky glow, offset)
    double gradientX = 0, gradientY = 0;      // added to every channel, rising by this across the frame
    double blurSigma = 0;          // Gaussian seeing blur (px)
    double noiseStd = 0;           // white noise per sample
    // Saturated highlights: the brightest `clipFraction` of the disc's pixels (observed, before clipping)
    // have the channels flagged here set to 1.0.
    double clipFraction = 0;
    std::array<bool, 3> clipChannels{true, true, true};
    // Chroma noise along the left frame edge: R and B multiplied by (1 + n), n ~ N(0, edgeChromaNoise),
    // independently, in columns 0 .. edgeBandWidth - 1.
    double edgeChromaNoise = 0;
    int edgeBandWidth = 6;
    // Per-channel offset of the observed light (px; +y = down): the scene is evaluated at (x - sx, y - sy).
    std::array<double, 3> shiftX{0, 0, 0}, shiftY{0, 0, 0};
    unsigned seed = 1;
};

struct MoonScene {
    ser::FloatImage truth;     // sharp, untinted by the cast, linear RGB (no pedestal, gradient, blur, noise)
    ser::FloatImage observed;  // what a stack would hold
    std::vector<uint8_t> diskTruth;      // lit disc, clear of the limb: r <= R - 2 - 2 blur, illumination >= 0.1
    std::vector<uint8_t> skyTruth;       // clear of the disc: r >= R + 3 + 3 blur
    std::vector<uint8_t> mariaTruth;     // diskTruth and >= 0.9 mare
    std::vector<uint8_t> highlandTruth;  // diskTruth and <= 0.1 mare
    std::vector<uint8_t> clipped;        // pixels whose flagged channels were set to 1.0
};

namespace moon_detail {
struct Wave { double u, v, a, phi; };
inline double Sum(const std::vector<Wave>& ws, double x, double y) {
    double s = 0;
    for (const Wave& w : ws) s += w.a * std::sin(2 * M_PI * (w.u * x + w.v * y) + w.phi);
    return s;
}
// n waves, frequencies log-uniform in [fLo, fHi] cycles/px, random directions; scaled to std `rms`.
inline std::vector<Wave> Waves(std::mt19937& rng, int n, double fLo, double fHi, double rms) {
    std::uniform_real_distribution<double> u01(0, 1);
    std::vector<Wave> ws;
    double power = 0;
    for (int i = 0; i < n; ++i) {
        const double f = fLo * std::pow(fHi / fLo, u01(rng)), th = 2 * M_PI * u01(rng);
        ws.push_back({f * std::cos(th), f * std::sin(th), 1.0, 2 * M_PI * u01(rng)});
        power += 0.5;
    }
    for (Wave& w : ws) w.a *= rms / std::sqrt(power);
    return ws;
}
inline double GradRms(const std::vector<Wave>& ws) {
    double g = 0;
    for (const Wave& w : ws) g += 0.5 * w.a * w.a * 4 * M_PI * M_PI * (w.u * w.u + w.v * w.v);
    return std::sqrt(g);
}
// Separable Gaussian blur of one interleaved channel, mirrored edges.
inline void Blur(ser::FloatImage& img, double sigma) {
    if (sigma <= 0) return;
    const int r = static_cast<int>(std::ceil(4 * sigma)), w = img.width, h = img.height, nc = img.channels;
    std::vector<double> k(2 * r + 1);
    double s = 0;
    for (int i = -r; i <= r; ++i) s += k[i + r] = std::exp(-0.5 * i * i / (sigma * sigma));
    for (double& v : k) v /= s;
    auto mir = [](int i, int n) {
        while (i < 0 || i >= n) i = i < 0 ? -i - 1 : 2 * n - 1 - i;
        return i;
    };
    std::vector<float> tmp(img.data.size());
    stack::ParallelBands(h, 8, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x)
                for (int c = 0; c < nc; ++c) {
                    double a = 0;
                    for (int i = -r; i <= r; ++i) a += k[i + r] * img.data[(static_cast<size_t>(y) * w + mir(x + i, w)) * nc + c];
                    tmp[(static_cast<size_t>(y) * w + x) * nc + c] = static_cast<float>(a);
                }
    });
    stack::ParallelBands(h, 8, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x)
                for (int c = 0; c < nc; ++c) {
                    double a = 0;
                    for (int i = -r; i <= r; ++i) a += k[i + r] * tmp[(static_cast<size_t>(mir(y + i, h)) * w + x) * nc + c];
                    img.data[(static_cast<size_t>(y) * w + x) * nc + c] = static_cast<float>(a);
                }
    });
}
}  // namespace moon_detail

inline MoonScene MakeMoonScene(const MoonSceneSpec& s) {
    using namespace moon_detail;
    std::mt19937 rng(s.seed);
    // Fine albedo texture: 24 waves from 1/64 to 1/3 cycle/px, equal power per octave.
    const std::vector<Wave> texture = Waves(rng, 24, 1.0 / 64, s.textureMaxFreq, s.textureAmplitude);
    // Maria: a low-frequency field thresholded at the quantile that gives mariaFraction of the disc.
    const std::vector<Wave> mare = Waves(rng, 6, 0.6 / s.radius, 2.5 / s.radius, 1.0);
    std::array<std::vector<Wave>, 3> tint;
    // Tint structure inside each terrain at the scales of mare units, ejecta and rays: periods R / 12 to R.
    // (Periods of R and up only, as first written, left a 20 px band all but colour-flat -- ~0.003 B/G std,
    // under the 0.0054-0.022 the 64.6F's reference bands carry; task ENG.)
    for (auto& t : tint) t = Waves(rng, 6, 1.0 / s.radius, 12.0 / s.radius, 1.0);
    const int w = s.width, h = s.height;
    const size_t n = static_cast<size_t>(w) * h;
    double theta = 0;
    {
        std::vector<double> vals;
        for (int y = 0; y < h; y += 2)
            for (int x = 0; x < w; x += 2)
                if (std::hypot(x - s.cx, y - s.cy) < s.radius) vals.push_back(Sum(mare, x, y));
        if (!vals.empty()) {
            const size_t k = std::min(vals.size() - 1, static_cast<size_t>((1 - s.mariaFraction) * vals.size()));
            std::nth_element(vals.begin(), vals.begin() + static_cast<long>(k), vals.end());
            theta = vals[k];
        }
    }
    const double tau = 1.5 * GradRms(mare);  // mare edge about 3 px wide
    const double sa = std::sin(s.phaseDeg * M_PI / 180), ca = std::cos(s.phaseDeg * M_PI / 180);
    // Mare weight at (x, y).
    auto mareW = [&](double x, double y) { return 0.5 * (1 + std::tanh((Sum(mare, x, y) - theta) / tau)); };
    // Linear light of channel c at (x, y) with the given illumination model (no cast).
    auto light = [&](double x, double y, int c, double* illumOut, double* mOut) {
        const double nx = (x - s.cx) / s.radius, ny = (y - s.cy) / s.radius, r = std::hypot(nx, ny);
        const double inside = 0.5 * std::erfc((r - 1) * s.radius / (std::sqrt(2.0) * 0.5));
        const double nz = std::sqrt(std::max(1e-4, 1 - std::min(1.0, r) * std::min(1.0, r)));
        const double nxc = r > 1 ? nx / r : nx;
        const double mu0 = std::max(0.0, nxc * sa + nz * ca);
        const double illum = mu0 > 0 ? 2 * mu0 / (mu0 + nz) : 0;  // Lommel-Seeliger, 1 at full phase
        const double m = mareW(x, y);
        const double albedo = std::max(0.0, (1 - m * (1 - s.mariaAlbedo)) * (1 + Sum(texture, x, y)));
        const double base = m * s.mariaTint[c] + (1 - m) * s.highlandTint[c];
        const double t = base * (1 + s.tintVariation * Sum(tint[c], x, y));
        if (illumOut) *illumOut = illum;
        if (mOut) *mOut = m;
        return inside * illum * s.level * albedo * t;
    };
    MoonScene sc;
    for (ser::FloatImage* im : {&sc.truth, &sc.observed}) {
        im->width = w;
        im->height = h;
        im->channels = 3;
        im->data.assign(n * 3, 0.0f);
    }
    sc.diskTruth.assign(n, 0);
    sc.skyTruth.assign(n, 0);
    sc.mariaTruth.assign(n, 0);
    sc.highlandTruth.assign(n, 0);
    sc.clipped.assign(n, 0);
    const bool shifted = std::any_of(s.shiftX.begin(), s.shiftX.end(), [](double v) { return v != 0; }) ||
                         std::any_of(s.shiftY.begin(), s.shiftY.end(), [](double v) { return v != 0; });
    stack::ParallelBands(h, 16, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x) {
                const size_t i = static_cast<size_t>(y) * w + x;
                const double r = std::hypot(x - s.cx, y - s.cy);
                double illum = 0, m = 0;
                for (int c = 0; c < 3; ++c) {
                    const double v = light(x, y, c, c == 0 ? &illum : nullptr, c == 0 ? &m : nullptr);
                    sc.truth.data[i * 3 + c] = static_cast<float>(v);
                    sc.observed.data[i * 3 + c] =
                        static_cast<float>(shifted ? light(x - s.shiftX[c], y - s.shiftY[c], c, nullptr, nullptr) : v);
                }
                const bool disk = r <= s.radius - 2 - 2 * s.blurSigma && illum >= 0.1;
                sc.diskTruth[i] = disk;
                sc.skyTruth[i] = r >= s.radius + 3 + 3 * s.blurSigma;
                sc.mariaTruth[i] = disk && m >= 0.9;
                sc.highlandTruth[i] = disk && m <= 0.1;
            }
    });
    Blur(sc.observed, s.blurSigma);
    std::normal_distribution<double> nd(0, 1);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = static_cast<size_t>(y) * w + x;
            const double g = s.gradientX * x / std::max(1, w - 1) + s.gradientY * y / std::max(1, h - 1);
            for (int c = 0; c < 3; ++c) {
                double v = sc.observed.data[i * 3 + c] * s.cast[c] + s.pedestal[c] + g;
                if (s.noiseStd > 0) v += s.noiseStd * nd(rng);
                sc.observed.data[i * 3 + c] = static_cast<float>(v);
            }
            if (s.edgeChromaNoise > 0 && x < s.edgeBandWidth) {
                sc.observed.data[i * 3 + 0] *= static_cast<float>(1 + s.edgeChromaNoise * nd(rng));
                sc.observed.data[i * 3 + 2] *= static_cast<float>(1 + s.edgeChromaNoise * nd(rng));
            }
        }
    if (s.clipFraction > 0) {
        std::vector<std::pair<float, size_t>> lit;
        for (size_t i = 0; i < n; ++i)
            if (sc.diskTruth[i])
                lit.push_back({sc.observed.data[i * 3] + sc.observed.data[i * 3 + 1] + sc.observed.data[i * 3 + 2], i});
        const size_t k = static_cast<size_t>(s.clipFraction * lit.size());
        std::partial_sort(lit.begin(), lit.begin() + static_cast<long>(k), lit.end(),
                          [](const auto& a, const auto& b) { return a.first > b.first; });
        for (size_t j = 0; j < k; ++j) {
            sc.clipped[lit[j].second] = 1;
            for (int c = 0; c < 3; ++c)
                if (s.clipChannels[c]) sc.observed.data[lit[j].second * 3 + c] = 1.0f;
        }
    }
    for (float& v : sc.observed.data) v = std::min(v, 1.0f);  // a stack's range
    return sc;
}

// Mean of the channels, one value per pixel.
inline std::vector<double> Lum(const ser::FloatImage& im) {
    std::vector<double> out(static_cast<size_t>(im.width) * im.height);
    for (size_t i = 0; i < out.size(); ++i) {
        double s = 0;
        for (int c = 0; c < im.channels; ++c) s += im.data[i * im.channels + c];
        out[i] = s / im.channels;
    }
    return out;
}

}  // namespace planetary::testing
