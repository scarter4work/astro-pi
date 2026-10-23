// Test-only calibration probe (Ruling P26; test/calibration/, built by test/CMakeLists.txt): never shipped, not a
// ctest case (synthetic frames, test/unit/StackSynth.h). Built as calib-p27.
//
//   calib-p27 [patch amplitude A (0.03)]
//
// The P27 featureless-patch scenario: per region, the median q / qualitySigma over its candidates, with
// the patch's blob at amplitude A (0 = pure noise).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include "Regions.h"
#include "StackSynth.h"
using namespace planetary;
int main(int argc, char** argv) {
    const double A = argc > 1 ? std::atof(argv[1]) : 0.03;
    const int w = 256, h = 192, px0 = 64, py0 = 64, px1 = 128, py1 = 128;
    const testing::Scene s = testing::MakeSurface(21, w, h);
    auto patch = [&](ser::FloatImage& im, double noise, unsigned seed) {
        std::mt19937 rng(seed);
        std::normal_distribution<double> n(0, noise);
        for (int y = py0; y < py1; ++y)
            for (int x = px0; x < px1; ++x) {
                const double dx = x - 96.3, dy = y - 95.7;
                im.data[static_cast<size_t>(y) * w + x] =
                    static_cast<float>(0.4 + A * std::exp(-(dx * dx + dy * dy) / 72.0) + (noise > 0 ? n(rng) : 0.0));
            }
    };
    ser::FloatImage ref{w, h, 1, std::vector<float>(w * h)};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) ref.data[y * w + x] = static_cast<float>(s.Eval(x, y, 0.6));
    patch(ref, 0, 0);
    stack::RegionLayout lay = stack::PlaceRegions(ref, stack::MeasureFrame(ref, 0, 1), true, testing::WhiteNoise(0.01));
    lay.noise = testing::WhiteNoise(0.01);
    const stack::RegionMeasurer m(ref, lay);
    std::vector<stack::FrameRegions> measured;
    for (int k = 0; k < 80; ++k) {
        testing::FrameSpec f;
        f.noise = 0.01;
        ser::FloatImage frame{w, h, 1, testing::RenderFrame(s, w, h, f, 300 + k)};
        patch(frame, 0.01, 700u + k);
        measured.push_back(m.Measure(frame, k, stack::Shift{}));
    }
    for (size_t r = 0; r < lay.regions.size(); ++r) {
        const auto& reg = lay.regions[r];

        std::vector<double> ratio, q;
        int cand = 0;
        for (const auto& fr : measured)
            if (fr.regions[r].aligned && fr.regions[r].quality > 0) {
                ++cand;
                ratio.push_back(fr.regions[r].quality / fr.regions[r].qualitySigma);
                q.push_back(fr.regions[r].quality);
            }
        std::sort(ratio.begin(), ratio.end());
        double mq = 0, mq2 = 0;
        for (double v : q) { mq += v; mq2 += v * v; }
        mq /= q.size();
        double ms = 0;
        for (const auto& fr : measured)
            if (fr.regions[r].aligned && fr.regions[r].quality > 0) ms += fr.regions[r].qualitySigma;
        std::printf("A %.3f region %d,%d: candidates %d of 400, r50 %.2f, q mean %.4f sd %.4f, mean sigma %.4f\n", A, reg.x0, reg.y0, cand,
                    ratio.empty() ? 0.0 : ratio[ratio.size() / 2], mq, std::sqrt(std::max(0.0, mq2 / q.size() - mq * mq)), ms / cand);
    }
}
