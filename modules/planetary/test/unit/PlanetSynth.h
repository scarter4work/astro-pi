#pragma once
// Synthetic planet scenes for the planet-processing tests (spec 2026-10-29-planetary-planet-processing-design.md,
// Ruling P1; plan Task 2, reused by Tasks 3 and 4). A globe with limb darkening (per channel) and latitude bands
// (brightness and a little colour), optional fine detail at a fixed pixel scale (texture and spots), an optional
// ring (inner and outer radius, opening angle, Cassini gap; the globe hides the ring's far half, the ring's near half
// crosses the globe), and what a stack adds on top -- a
// colour cast, a pedestal and a linear gradient per channel, seeing blur, white noise, clipped highlights -- with a
// known per-channel offset (atmospheric dispersion). Built like MoonSynth.h: the scene is analytic (sinusoids,
// erf edges 0.5 px wide), so a channel offset is an exact re-evaluation, not a resampling, and `truth` is known
// at every pixel. The centre may sit near or past the frame's edge (the disc cut by it).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include "Debayer.h"
#include "MoonSynth.h"  // moon_detail::Blur, Waves, Sum
#include "Parallel.h"

namespace planetary::testing {

struct PlanetSceneSpec {
    int width = 160, height = 120;
    double cx = 80, cy = 60, radius = 20;  // the globe (px); the centre may lie outside the frame
    double positionAngleDeg = 0;           // the planet's equator (bands, ring) turned by this from the +x axis
    double level = 0.5;                    // G at the globe's centre, before the bands (linear)
    std::array<double, 3> tint{1, 1, 1};   // the globe's true colour, relative to G (multiplies its light)
    // Limb darkening per channel, I = 1 - u (1 - mu) (real giants darken more in blue).
    std::array<double, 3> limbDarkening{0.45, 0.50, 0.55};
    double bandAmplitude = 0.10;   // relative std of the latitude bands' brightness
    // The bands' contrast per channel, times bandAmplitude (Jupiter's belts are darker in blue than in red: a colour
    // that follows the brightness, which one gain per channel does not absorb).
    std::array<double, 3> bandContrast{1, 1, 1};
    double bandTint = 0.03;        // relative std of each channel's own latitude colour modulation
    // Fine detail at a fixed pixel scale, at every radius (plan Task 3, controller ruling Q2: the bands above scale with
    // R, so a 200 px disc has nothing at the seeing's scale to recover). On the globe's light, every channel alike:
    // texture waves of 4-16 px periods (relative std textureAmplitude), and spots (Gaussian, sigma 1-2.5 px, each
    // +-spotContrast relative), spotDensity per 1000 px^2 of the globe (at least one when > 0). 0: none.
    double textureAmplitude = 0;
    double spotDensity = 0, spotContrast = 0.15;
    // Ring: radii in globe radii along the ring plane (Saturn: C ring 1.24, A ring 2.27, Cassini 1.95-2.03).
    bool ring = false;
    double ringInner = 1.24, ringOuter = 2.27;
    double cassiniInner = 1.95, cassiniOuter = 2.03;  // equal: no gap
    double ringTiltDeg = 25;       // opening angle B (0 = edge-on); the near half is below the centre (+y)
    double ringBrightness = 0.8;   // the B/A ring's light relative to `level`; the C ring (inside 1.53) has 0.35 of it
    std::array<double, 3> ringTint{1, 1, 1};
    std::array<double, 3> cast{1, 1, 1};       // multiplies the scene light per channel
    std::array<double, 3> pedestal{0, 0, 0};   // added per channel
    std::array<double, 3> gradientX{0, 0, 0}, gradientY{0, 0, 0};  // per channel, the rise across the frame
    double blurSigma = 0;          // Gaussian seeing blur (px)
    double noiseStd = 0;           // white noise per sample
    // Saturated highlights: the brightest `clipFraction` of diskTruth's pixels (observed) get the flagged channels
    // set to 1.0.
    double clipFraction = 0;
    std::array<bool, 3> clipChannels{true, true, true};
    // Per-channel offset of the observed light (px; +y = down): channel c is evaluated at (x - sx_c, y - sy_c).
    std::array<double, 3> shiftX{0, 0, 0}, shiftY{0, 0, 0};
    unsigned seed = 1;
    unsigned noiseSeed = 0;  // 0: noise from `seed`'s stream after the scene; else its own stream (same scene)
};

struct PlanetScene {
    ser::FloatImage truth;     // sharp, noiseless, linear RGB: tint, no cast, pedestal, gradient, shift or blur
    ser::FloatImage clean;     // observed before noise and clipping (cast, pedestal, gradient, shift, blur)
    ser::FloatImage observed;  // what a stack would hold (clean + noise, highlights clipped, capped at 1.0)
    // The ring's visible light alone (1 channel, the channels' mean after the cast, blurred, no shift): where the
    // ring is seen, in front of the globe or beside it. Empty without a ring.
    ser::FloatImage ringLight;
    double globeLevel = 0;               // the channels' mean of the globe's centre after the cast (contrast reference)
    std::vector<uint8_t> diskTruth;      // the globe clear of its limb: r <= R - 1 - 2 blur, inside the frame
    std::vector<uint8_t> skyTruth;       // clear of the globe and the ring by 3 + 3 blur px
    std::vector<uint8_t> clipped;        // pixels whose flagged channels were set to 1.0
};

namespace planet_detail {
// 0.5 erfc(-d / (sqrt 2 x 0.5)): an edge 0.5 px wide, 1 inside (d > 0).
inline double Step(double d) { return 0.5 * std::erfc(-d / (std::sqrt(2.0) * 0.5)); }
}  // namespace planet_detail

inline PlanetScene MakePlanetScene(const PlanetSceneSpec& s) {
    using namespace moon_detail;
    using planet_detail::Step;
    std::mt19937 rng(s.seed);
    const double R = s.radius;
    // Latitude bands: 6 waves along the axis, periods R / 3 to 1.4 R (at least 4 px).
    auto bandWaves = [&](double rms) {
        const double fHi = std::min(0.25, 3.0 / R), fLo = std::min(fHi, 0.7 / R);
        std::vector<Wave> ws = Waves(rng, 6, fLo, fHi, rms);
        for (Wave& w : ws) {  // along the axis only
            w.v = std::hypot(w.u, w.v);
            w.u = 0;
        }
        return ws;
    };
    const std::vector<Wave> bands = bandWaves(s.bandAmplitude);
    std::array<std::vector<Wave>, 3> bandColour;
    for (auto& b : bandColour) b = bandWaves(s.bandTint);
    // Fine detail (its own stream, so a scene without it is the one it always was): texture waves of 4-16 px periods
    // in any direction, and spots at r <= 0.85 R of the globe, all in the planet frame.
    std::vector<Wave> texture;
    struct Spot { double u, v, sigma, contrast; };
    std::vector<Spot> spots;
    if (s.textureAmplitude > 0 || s.spotDensity > 0) {
        std::mt19937 drng(s.seed * 2654435761u + 97u);
        if (s.textureAmplitude > 0) texture = Waves(drng, 16, 1.0 / 16, 1.0 / 4, s.textureAmplitude);
        if (s.spotDensity > 0) {
            std::uniform_real_distribution<double> u01(0, 1);
            const int count = std::max(1, static_cast<int>(std::lround(s.spotDensity * M_PI * R * R / 1000)));
            for (int k = 0; k < count; ++k) {
                const double rr = 0.85 * R * std::sqrt(u01(drng)), th = 2 * M_PI * u01(drng);
                const double sigma = 1.0 + 1.5 * u01(drng), sign = u01(drng) < 0.5 ? -1.0 : 1.0;
                spots.push_back({rr * std::cos(th), rr * std::sin(th), sigma, sign * s.spotContrast});
            }
        }
    }
    auto detail = [&](double u, double v) {
        double d = 1 + Sum(texture, u, v);
        for (const Spot& p : spots) {
            const double q = ((u - p.u) * (u - p.u) + (v - p.v) * (v - p.v)) / (p.sigma * p.sigma);
            if (q < 25) d += p.contrast * std::exp(-0.5 * q);
        }
        return d;
    };
    const double pa =s.positionAngleDeg * M_PI / 180, cp = std::cos(pa), sp = std::sin(pa);
    const double sinB = std::max(1e-3, std::sin(std::fabs(s.ringTiltDeg) * M_PI / 180));
    // Ring-plane radius (globe radii) of a point at planet-frame (u, v), and its distance in px from the radius e
    // (positive outside), linearised: (rr - e) R / |grad(rr R)|.
    auto ringDist = [&](double u, double v, double e) {
        const double W = v / sinB, rho = std::hypot(u, W);
        const double grad = rho > 1e-9 ? std::sqrt(u * u + W * W / (sinB * sinB)) / rho : 1.0;
        return (rho - e * R) / grad;
    };
    // Globe light, its inside share, and the ring's visible light, for channel c at image (x, y) (no cast).
    struct Light { double total, ring; };
    auto light = [&](double x, double y, int c) -> Light {
        const double dx = x - s.cx, dy = y - s.cy;
        const double u = dx * cp + dy * sp, v = -dx * sp + dy * cp;  // planet frame: u along the equator
        const double r = std::hypot(u, v) / R;
        const double inside = 0.5 * std::erfc((r - 1) * R / (std::sqrt(2.0) * 0.5));
        const double mu = std::sqrt(std::max(0.0, 1 - std::min(1.0, r) * std::min(1.0, r)));
        const double globe = inside * s.level * s.tint[c] * (1 - s.limbDarkening[c] * (1 - mu)) * (1 + s.bandContrast[c] * Sum(bands, 0, v)) *
                             (1 + Sum(bandColour[c], 0, v)) * (texture.empty() && spots.empty() ? 1.0 : detail(u, v));
        if (!s.ring) return {globe, 0.0};
        const double cover = Step(ringDist(u, v, s.ringInner)) * Step(-ringDist(u, v, s.ringOuter)) *
                             (1 - Step(ringDist(u, v, s.cassiniInner)) * Step(-ringDist(u, v, s.cassiniOuter)));
        const double cRing = 0.35 + 0.65 * Step(ringDist(u, v, 1.53));
        const double ringL = cover * cRing * s.ringBrightness * s.level * s.ringTint[c];
        // The near half (v > 0) is in front of the globe (opaque); the far half is hidden by it.
        if (v > 0) return {ringL + (1 - cover) * globe, ringL};
        return {globe + (1 - inside) * ringL, (1 - inside) * ringL};
    };
    const int w = s.width, h = s.height;
    const size_t n = static_cast<size_t>(w) * h;
    PlanetScene sc;
    for (ser::FloatImage* im : {&sc.truth, &sc.observed}) *im = ser::FloatImage{w, h, 3, std::vector<float>(n * 3, 0.0f)};
    if (s.ring) sc.ringLight = ser::FloatImage{w, h, 1, std::vector<float>(n, 0.0f)};
    sc.diskTruth.assign(n, 0);
    sc.skyTruth.assign(n, 0);
    sc.clipped.assign(n, 0);
    double castMean = 0;
    for (int c = 0; c < 3; ++c) castMean += s.level * s.tint[c] * s.cast[c] / 3;
    sc.globeLevel = castMean;
    stack::ParallelBands(h, 16, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x) {
                const size_t i = static_cast<size_t>(y) * w + x;
                double ringSum = 0;
                for (int c = 0; c < 3; ++c) {
                    const Light l = light(x, y, c);
                    sc.truth.data[i * 3 + c] = static_cast<float>(l.total);
                    sc.observed.data[i * 3 + c] = static_cast<float>(light(x - s.shiftX[c], y - s.shiftY[c], c).total);
                    ringSum += l.ring * s.cast[c];
                }
                if (s.ring) sc.ringLight.data[i] = static_cast<float>(ringSum / 3);
                const double dx = x - s.cx, dy = y - s.cy, r = std::hypot(dx, dy);
                const double margin = 3 + 3 * s.blurSigma;
                sc.diskTruth[i] = r <= R - 1 - 2 * s.blurSigma;
                bool sky = r >= R + margin;
                if (sky && s.ring) {
                    const double u = dx * cp + dy * sp, v = -dx * sp + dy * cp;
                    sky = ringDist(u, v, s.ringOuter) >= margin;
                }
                sc.skyTruth[i] = sky;
            }
    });
    Blur(sc.observed, s.blurSigma);
    if (s.ring) Blur(sc.ringLight, s.blurSigma);
    for (size_t i = 0; i < n; ++i) {
        const int x = static_cast<int>(i % static_cast<size_t>(w)), y = static_cast<int>(i / static_cast<size_t>(w));
        for (int c = 0; c < 3; ++c) {
            const double g = s.gradientX[c] * x / std::max(1, w - 1) + s.gradientY[c] * y / std::max(1, h - 1);
            sc.observed.data[i * 3 + c] = static_cast<float>(sc.observed.data[i * 3 + c] * s.cast[c] + s.pedestal[c] + g);
        }
    }
    sc.clean = sc.observed;
    std::normal_distribution<double> nd(0, 1);
    if (s.noiseSeed != 0) rng.seed(s.noiseSeed);
    if (s.noiseStd > 0)
        for (float& v : sc.observed.data) v = static_cast<float>(v + s.noiseStd * nd(rng));
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

}  // namespace planetary::testing
