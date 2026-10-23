// Test-only CLI: why the in-focus large-disc reading doubled with Ruling P26 (Select.h step 7; P26 fix
// round 2, O1). Never shipped, not a ctest case: it needs the NAS captures, which it only reads.
//
//   limb-probe <video>
//
// On one disc capture: each frame's quality with the capture's noise model (P26) and with its own sky
// noise (the one-image form, as before P26); a crude stack of the sharpest half (frames shifted to the
// first's centroid, whole px) measured both ways; and the limb blur read directly off that stack: an erf
// edge fitted across the left and right limbs (rows within R_h / 4 of the centre), sigma in luma px and as
// a fraction of R_h. Exit 0, or 1 on error.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

#include "FrameSource.h"
#include "Measure.h"
#include "Noise.h"
#include "StackSynth.h"
using namespace planetary;
namespace {
// Least squares fit of s + A 0.5 erfc(sgn (x - x0) / (sqrt2 sigma)) to (x, v), grid over x0 and sigma, linear s and A.
void FitEdge(const std::vector<double>& x, const std::vector<double>& v, double sgn, double x0lo, double x0hi, double& bx0,
             double& bsg, double& bres) {
    bres = 1e300;
    for (double x0 = x0lo; x0 <= x0hi; x0 += 0.02)
        for (double sg = 0.2; sg <= 8; sg += 0.02) {
            double n = 0, sf = 0, sv = 0, sff = 0, sfv = 0;
            for (size_t i = 0; i < x.size(); ++i) {
                const double f = 0.5 * std::erfc(sgn * (x[i] - x0) / (std::sqrt(2.0) * sg));
                n += 1;
                sf += f;
                sv += v[i];
                sff += f * f;
                sfv += f * v[i];
            }
            const double det = n * sff - sf * sf;
            if (!(det > 0)) continue;
            const double A = (n * sfv - sf * sv) / det, s = (sv - A * sf) / n;
            double res = 0;
            for (size_t i = 0; i < x.size(); ++i) {
                const double f = 0.5 * std::erfc(sgn * (x[i] - x0) / (std::sqrt(2.0) * sg));
                res += (v[i] - s - A * f) * (v[i] - s - A * f);
            }
            if (res < bres) {
                bres = res;
                bx0 = x0;
                bsg = sg;
            }
        }
}
}  // namespace
int main(int argc, char** argv) try {
    if (argc != 2) {
        std::fprintf(stderr, "usage: limb-probe <video>\n");
        return 1;
    }
    const std::string path = argv[1];
    const stack::SourceFactory f = [&] { return ser::OpenVideo(path, ser::BayerChoice::Auto); };
    const ser::FileInfo info = f()->Info();
    const double scale = stack::LumaScale(info);
    const auto stats = stack::MeasureAll(f, info, 16, {});
    auto src = f();
    std::vector<double> qNew, qOld;
    std::vector<std::pair<double, int64_t>> order;
    for (const auto& s : stats)
        if (s.targetFound && s.quality > 0) {
            qNew.push_back(s.quality);
            order.push_back({-s.quality, s.index});
        }
    std::sort(order.begin(), order.end());
    const size_t keep = std::max<size_t>(1, order.size() / 2);
    ser::FloatImage acc;
    double cx0 = 0, cy0 = 0, rh = 0;
    std::vector<double> halfR;
    for (size_t k = 0; k < order.size(); ++k) {
        const int64_t i = order[k].second;
        const ser::FloatImage l = stack::MeasurementLuma(src->ReadFrame(i), info);
        qOld.push_back(stack::MeasureFrame(l, i, scale).quality);
        halfR.push_back(stats[static_cast<size_t>(i)].halfRadius);
        if (k >= keep) continue;
        const auto& st = stats[static_cast<size_t>(i)];
        if (k == 0) {
            acc = ser::FloatImage{l.width, l.height, 1, std::vector<float>(l.data.size(), 0.f)};
            cx0 = st.cx;
            cy0 = st.cy;
        }
        const int dx = static_cast<int>(std::lround(st.cx - cx0)), dy = static_cast<int>(std::lround(st.cy - cy0));
        for (int y = 0; y < l.height; ++y)
            for (int x = 0; x < l.width; ++x) {
                const int xs = std::clamp(x + dx, 0, l.width - 1), ys = std::clamp(y + dy, 0, l.height - 1);
                acc.data[static_cast<size_t>(y) * l.width + x] += l.data[static_cast<size_t>(ys) * l.width + xs] / static_cast<float>(keep);
            }
    }
    std::sort(halfR.begin(), halfR.end());
    rh = halfR[halfR.size() / 2];
    auto mean = [](const std::vector<double>& v) { return std::accumulate(v.begin(), v.end(), 0.0) / v.size(); };
    auto top = [](std::vector<double> v, double share) {
        std::sort(v.begin(), v.end(), std::greater<double>());
        return v[std::min(v.size() - 1, static_cast<size_t>(share * v.size()))];
    };
    const stack::FrameStats sStackNew = stack::MeasureFrame(acc, 0, scale, testing::WhiteNoise(1e-6));
    const stack::FrameStats sStackOld = stack::MeasureFrame(acc, 0, scale);
    std::printf("%s: %zu frames with detail, R_h %.1f luma px\n", path.c_str(), qNew.size(), rh);
    std::printf("  per frame q, model (P26): mean %.4f, top 1%% %.4f | own sky (pre-P26): mean %.4f, top 1%% %.4f\n", mean(qNew),
                top(qNew, 0.01), mean(qOld), top(qOld, 0.01));
    std::printf("  stack of the sharpest %zu: q %.4f (near noise-free), sky-noise form %.4f\n", keep, sStackNew.quality, sStackOld.quality);
    const double sp = std::sqrt(M_PI);
    auto rd = [&](double q) { return 1 / (sp * q * q * rh) / rh; };
    std::printf("  reading sigma(q) / R_h at: model top 1%% %.4f, own-sky top 1%% %.4f, the stack's q %.4f\n", rd(top(qNew, 0.01)),
                rd(top(qOld, 0.01)), rd(sStackNew.quality));
    // Direct limb blur: rows within R_h / 4 of the centre, left and right limbs.
    const int cy = static_cast<int>(std::lround(cy0)), w = acc.width;
    for (double sgn : {-1.0, 1.0}) {
        std::vector<double> x, v;
        const double edge = cx0 + sgn * rh;
        for (int y = cy - static_cast<int>(rh / 4); y <= cy + static_cast<int>(rh / 4); ++y)
            for (int xi = static_cast<int>(edge - 10); xi <= static_cast<int>(edge + 10); ++xi) {
                if (xi < 0 || xi >= w) continue;
                x.push_back(xi);
                v.push_back(acc.data[static_cast<size_t>(y) * w + xi]);
            }
        double bx0 = 0, bsg = 0, bres = 0;
        FitEdge(x, v, sgn, edge - 5, edge + 5, bx0, bsg, bres);
        std::printf("  %s limb: edge at %.2f, sigma %.2f luma px (%.3f R_h)\n", sgn < 0 ? "left" : "right", bx0, bsg, bsg / rh);
    }
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "limb-probe: %s\n", e.what());
    return 1;
}
