// Test-only calibration probe (Ruling P26; test/calibration/, built by test/CMakeLists.txt): never shipped, not a
// ctest case; it reads a real capture (the NAS is only read). Built as calib-skynoise.
//
//   calib-skynoise <video> [frames (all)]
//
// The noise model's responses on a sky-only capture (the 2026-01-30 Saturn AVI 203627: no target in any
// frame), exactly as FitNoiseModel forms them (Noise.h: [1 -2 1] in time x [1 -2 1]^2 in space over each
// consecutive triple, gain 216, the level from the frame after the triple), every triple of the first
// `frames` frames:
//   - their kurtosis, and the Gaussian-corrected trimmed second moment at several shares (keep the share
//     with the smallest |r|, divide by the truncated normal's 1 - 2 z phi(z) / P) over the plain mean square
//     -- why pass 1 does not trim (Noise.h: a trimmed moment is biased by the noise's own tails);
//   - pass 1's bins (AddTriple, merged from the dark end to kMinBinResponses as FitNoise does) and a line
//     fitted to them by plain weighted least squares, with no kSlopeSpread gate: its slope, the bins' level
//     span, and the line at the responses' mean level over their mean square -- why FitNoise fits a slope
//     only across more than kSlopeSpread level-noise sd (Noise.h, Noise.cpp); against the pass-1 model
//     FitNoise gives (gated), at the same level.
// Exit 0, or 1 on error.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

#include "FrameSource.h"
#include "Measure.h"
#include "Noise.h"

using namespace planetary;

namespace {
// Gaussian-corrected trimmed second moment: the `share` of responses with the smallest |r|, their mean
// square over the share a standard normal keeps of its variance when cut at the same quantile.
double Trimmed(std::vector<double> r, double share) {
    double all = 0;
    for (double v : r) all += v * v;
    if (share >= 1) return all / static_cast<double>(r.size());
    for (double& v : r) v = std::fabs(v);
    const size_t c = std::max<size_t>(1, static_cast<size_t>(share * static_cast<double>(r.size())));
    std::nth_element(r.begin(), r.begin() + static_cast<std::ptrdiff_t>(c - 1), r.end());
    const double top = r[c - 1];
    double s = 0;
    size_t n = 0;
    for (double v : r)
        if (v <= top) {
            s += v * v;
            ++n;
        }
    const double p = static_cast<double>(n) / static_cast<double>(r.size());
    double lo = 0, hi = 10;  // z with P(|Z| < z) = p
    for (int i = 0; i < 80; ++i) {
        const double m = 0.5 * (lo + hi);
        (std::erf(m / std::sqrt(2.0)) < p ? lo : hi) = m;
    }
    const double z = 0.5 * (lo + hi);
    const double kept = 1 - 2 * z * std::exp(-0.5 * z * z) / std::sqrt(2 * M_PI) / p;
    return s / static_cast<double>(n) / kept;
}
}  // namespace

int main(int argc, char** argv) try {
    if (argc < 2) {
        std::fprintf(stderr, "usage: calib-skynoise <video> [frames (all)]\n");
        return 1;
    }
    const std::string path = argv[1];
    const auto src = ser::OpenVideo(path, ser::BayerChoice::Auto);
    const ser::FileInfo info = src->Info();
    const int64_t frames = argc > 2 ? std::min<int64_t>(std::atoll(argv[2]), info.frameCount) : info.frameCount;
    if (frames < 4) throw std::invalid_argument("calib-skynoise: needs at least 4 frames");
    std::vector<stack::NoisePlane> planes;
    for (int64_t i = 0; i < frames; ++i) planes.emplace_back(stack::MeasurementLuma(src->ReadFrame(i), info));

    std::vector<double> r;  // standardised to the pixel variance: r / sqrt(216)
    double levelSum = 0;
    stack::NoiseSamples pass1;
    for (int64_t k = 0; k + 3 < frames; ++k) {
        const auto& p0 = planes[static_cast<size_t>(k)];
        const auto& p1 = planes[static_cast<size_t>(k + 1)];
        const auto& p2 = planes[static_cast<size_t>(k + 2)];
        const auto& lv = planes[static_cast<size_t>(k + 3)];
        for (size_t i = 0; i < p1.response.size(); ++i) {
            r.push_back((static_cast<double>(p0.response[i]) - 2.0 * p1.response[i] + p2.response[i]) / std::sqrt(216.0));
            levelSum += lv.level[i];
        }
        stack::AddTriple(p0, p1, p2, lv, pass1);
    }
    double m2 = 0, m4 = 0;
    for (double v : r) {
        m2 += v * v;
        m4 += v * v * v * v;
    }
    const double n = static_cast<double>(r.size()), variance = m2 / n, meanLevel = levelSum / n;
    std::printf("%s: %lld frames, %zu responses, mean level %.4f, variance %.4g, kurtosis %.2f\n", path.c_str(),
                static_cast<long long>(frames), r.size(), meanLevel, variance, (m4 / n) / (variance * variance));
    std::printf("  trimmed (Gaussian-corrected) / plain:");
    for (double share : {0.5, 0.8, 0.9, 0.95, 0.99}) std::printf("  %.2f: %.3f", share, Trimmed(r, share) / variance);
    std::printf("\n");

    // Pass 1's points as FitNoise forms them (Noise.cpp FineBinPoints: the fine bins merged from the dark end
    // to kMinBinResponses, the short last group into the one before; each bin's sampling variance from its
    // fourth moment times the responses' overlap), the bins near black left out as CurveFromPoints does.
    struct Point {
        double x, v, n, var;
    };
    std::vector<Point> pts;
    {
        double gn = 0, gl = 0, g2 = 0, g4 = 0;
        for (size_t k = 0; k < pass1.n.size(); ++k) {
            gn += pass1.n[k];
            gl += pass1.level[k];
            g2 += pass1.r2[k];
            g4 += pass1.r4[k];
            const bool last = k + 1 == pass1.n.size();
            if (gn >= stack::kMinBinResponses || (last && gn > 0)) {
                if (gn < stack::kMinBinResponses && !pts.empty()) {
                    const Point& p = pts.back();
                    gn += p.n;
                    gl += p.x * p.n;
                    g2 += p.v * p.n;
                    g4 += (p.var / pass1.overlap * p.n + p.v * p.v) * p.n;
                    pts.pop_back();
                }
                const double v = g2 / gn;
                pts.push_back({gl / gn, v, gn, pass1.overlap * std::max(0.0, g4 / gn - v * v) / gn});
                gn = gl = g2 = g4 = 0;
            }
        }
    }
    const int sites = stack::LumaSites(info);
    std::vector<Point> live;
    for (const Point& p : pts)
        if (p.v > 0 && !(p.x < stack::kClipSigmas * std::sqrt(sites * p.v))) live.push_back(p);
    if (live.size() < 2) throw std::runtime_error("calib-skynoise: fewer than 2 bins clear of black");
    // The line as CurveFromPoints fits it with a slope allowed (no kSlopeSpread gate): Theil-Sen start, then
    // weighted least squares iterated with Tukey's biweight on each bin's relative error; a, b >= 0.
    std::vector<double> slopes, icpt;
    for (size_t i = 0; i < live.size(); ++i)
        for (size_t j = i + 1; j < live.size(); ++j)
            if (live[j].x > live[i].x) slopes.push_back((live[j].v - live[i].v) / (live[j].x - live[i].x));
    auto median = [](std::vector<double> v) {
        std::sort(v.begin(), v.end());
        const size_t m = v.size() / 2;
        return v.size() % 2 ? v[m] : 0.5 * (v[m - 1] + v[m]);
    };
    double b = slopes.empty() ? 0.0 : std::max(0.0, median(slopes));
    for (const Point& p : live) icpt.push_back(p.v - b * p.x);
    double a = median(icpt);
    for (int round = 0; round < 30; ++round) {
        double sw = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (const Point& p : live) {
            const double f = std::max(a + b * p.x, 1e-30);
            const double rel2 = p.var / (p.v * p.v) + stack::kFitSlack * stack::kFitSlack;
            const double z = (p.v - f) / (f * std::sqrt(rel2)) / stack::kTukey;
            const double w = std::fabs(z) < 1 ? (1 - z * z) * (1 - z * z) / (f * f * rel2) : 0;
            sw += w;
            sx += w * p.x;
            sy += w * p.v;
            sxx += w * p.x * p.x;
            sxy += w * p.x * p.v;
        }
        if (!(sw > 0)) break;
        double na = sy / sw, nb = 0;
        const double det = sw * sxx - sx * sx;
        if (det > 1e-9 * sw * sxx) {
            nb = (sw * sxy - sx * sy) / det;
            na = (sy - nb * sx) / sw;
        }
        if (nb < 0) {
            nb = 0;
            na = sy / sw;
        } else if (na < 0 && sxx > 0) {
            na = 0;
            nb = sxy / sxx;
        }
        const bool done = std::fabs(na - a) <= 1e-9 * std::fabs(a) && std::fabs(nb - b) <= 1e-9 * std::fabs(b) + 1e-15;
        a = na;
        b = nb;
        if (done) break;
    }
    double ln = 0, lx = 0, lv = 0;
    for (const Point& p : live) {
        ln += p.n;
        lx += p.n * p.x;
        lv += p.n * p.v;
    }
    const double liveLevel = lx / ln, liveVariance = lv / ln;
    const stack::NoiseModel gated = stack::FitNoise(pass1, sites);
    std::printf("  pass 1: %zu bins, %zu clear of black over levels %.4f-%.4f (span %.4f), their variance %.4g at mean level %.4f\n",
                pts.size(), live.size(), live.front().x, live.back().x, live.back().x - live.front().x, liveVariance, liveLevel);
    std::printf("  ungated line: a %.4g b %.3g, at their mean level %.3f of their variance | FitNoise (gated): a %.4g b %.3g, %.3f\n", a,
                b, (a + b * liveLevel) / liveVariance, gated.a, gated.b, gated.Line(liveLevel) / liveVariance);
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "calib-skynoise: %s\n", e.what());
    return 1;
}
