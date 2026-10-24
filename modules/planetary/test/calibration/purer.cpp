// Test-only calibration probe (Ruling P26; test/calibration/, built by test/CMakeLists.txt): never shipped, not a
// ctest case (synthetic frames, test/unit/StackSynth.h). Built as calib-purer.
//
//   calib-purer [repeats (200)]
//
// P27's noise-only gate under Ruling P26's qualitySigma: the median q / qualitySigma over a featureless
// window's candidate frames (q > 0), pure white noise 0.01 on a flat 0.3 (window mean as background, the
// true noise as the capture's model), for window sides 32 / 64 / 128 and N frames; repeated `reps` times.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>
#include "Measure.h"
#include "StackSynth.h"
using namespace planetary;
int main(int argc, char** argv) {
    const int reps = argc > 1 ? std::atoi(argv[1]) : 200;
    const stack::NoiseModel model = testing::WhiteNoise(0.01);
    for (int n : {32, 128})
        for (int frames : {10, 20, 30, 64, 150, 400}) {
            std::vector<double> r50s;
            std::mt19937 rng(1000 + n + frames);
            std::normal_distribution<double> g(0, 0.01);
            std::vector<float> im(static_cast<size_t>(n) * n);
            double candShare = 0;
            for (int rep = 0; rep < reps; ++rep) {
                std::vector<double> r;
                for (int k = 0; k < frames; ++k) {
                    double mean = 0;
                    for (float& v : im) {
                        v = static_cast<float>(0.3 + g(rng));
                        mean += v;
                    }
                    const auto e = stack::RegionQuality(im.data(), n, 0, 0, n, n, mean / im.size(), model);
                    if (e.quality > 0) r.push_back(e.quality / e.sigma);
                }
                candShare += static_cast<double>(r.size()) / frames;
                if (r.empty()) continue;
                std::nth_element(r.begin(), r.begin() + r.size() / 2, r.end());
                r50s.push_back(r[r.size() / 2]);
            }
            std::sort(r50s.begin(), r50s.end());
            std::printf("window %3d, %3d frames: candidates %.2f; median r50 %.2f, p99 %.2f, max %.2f\n", n, frames, candShare / reps,
                        r50s[r50s.size() / 2], r50s[static_cast<size_t>(0.99 * (r50s.size() - 1))], r50s.back());
        }
}
