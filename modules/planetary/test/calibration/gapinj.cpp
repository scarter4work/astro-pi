// Test-only calibration probe (Ruling P26; test/calibration/, built by test/CMakeLists.txt): never shipped, not a
// ctest case (synthetic frames, test/unit/StackSynth.h). Built as calib-gapinj.
//
//   calib-gapinj <pixel noise s> <added noise s0> <jitter px> [sky (0.05)]
//
// I5's injection check on a synthetic capture with a level gap: a textured disc (level ~0.55) over sky
// `sky`, moving by `jit` px per frame (seeing-moved texture at the limb and inside), white noise s; the
// fitted model with and without added white noise s0, rise / s0^2 per level.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include "Noise.h"
#include "StackSynth.h"
using namespace planetary;
int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: calib-gapinj <pixel noise s> <added noise s0> <jitter px> [sky (0.05)]\n");
        return 1;
    }
    const double s = std::atof(argv[1]), s0 = std::atof(argv[2]), jit = std::atof(argv[3]), sky = argc > 4 ? std::atof(argv[4]) : 0.05;
    const int w = 256, h = 192, frames = 80;
    std::vector<std::vector<float>> clean;
    std::mt19937 rng(5);
    std::normal_distribution<double> n01(0, 1);
    const testing::Scene sc = testing::MakeSurface(31, w + 20, h + 20);
    for (int k = 0; k < frames; ++k) {
        const double gx = jit * n01(rng), gy = jit * n01(rng);
        std::vector<float> v(static_cast<size_t>(w) * h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                // A lunar surface on the left, a sharp limb at x 170 (moving with the seeing), sky beyond.
                const double xs = x + gx, ys = y + gy, t = sc.Eval(xs, ys, 1.0);
                const double in = 0.5 * std::erfc((xs - 170) / 1.5);
                v[static_cast<size_t>(y) * w + x] = static_cast<float>(sky + in * (t - sky));
            }
        clean.push_back(std::move(v));
    }
    auto fit = [&](double extra) {
        std::mt19937 r2(9);
        std::normal_distribution<double> n(0, 1);
        std::vector<ser::FloatImage> fr;
        for (const auto& c : clean) {
            ser::FloatImage im{w, h, 1, c};
            for (float& p : im.data) p = static_cast<float>(p + std::sqrt(s * s + extra * extra) * n(r2));
            fr.push_back(std::move(im));
        }
        return stack::FitNoiseModel(fr);
    };
    const stack::NoiseModel a = fit(0), b = fit(s0);
    for (double x : {0.05, 0.08, 0.12, 0.18, 0.25, 0.32, 0.4, 0.48, 0.55, 0.62})
        std::printf("level %.2f: %.4g -> %.4g, rise / v0 %.3f\n", x, a.VarianceAtMean(x), b.VarianceAtMean(x),
                    (b.VarianceAtMean(x) - a.VarianceAtMean(x)) / (s0 * s0));
}
