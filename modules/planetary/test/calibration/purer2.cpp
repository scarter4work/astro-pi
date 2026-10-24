// Test-only calibration probe (Ruling P26; test/calibration/, built by test/CMakeLists.txt): never shipped, not a
// ctest case (synthetic frames, test/unit/StackSynth.h). Built as calib-purer2.
//
//   calib-purer2 [regions per cell (2000)]
//
// P27's gate under Ruling P26's qualitySigma: how often pure noise passes kRegionDetailSigmas -- the median
// q / qualitySigma over exactly C candidates (frames with q > 0) of a featureless window, white noise 0.01
// on a flat 0.3, the true noise as the model; `reps` regions per cell.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
#include "Measure.h"
#include "Regions.h"
#include "StackSynth.h"
using namespace planetary;
int main(int argc, char** argv) {
    const int reps = argc > 1 ? std::atoi(argv[1]) : 2000;
    const stack::NoiseModel model = testing::WhiteNoise(0.01);
    for (int n : {32, 64})
        for (int C : {10, 30, 100, 300}) {
            std::mt19937 rng(7000 + n + C);
            std::normal_distribution<double> g(0, 0.01);
            std::vector<float> im(static_cast<size_t>(n) * n);
            int pass = 0;
            for (int rep = 0; rep < reps; ++rep) {
                std::vector<double> r;
                while (static_cast<int>(r.size()) < C) {
                    double mean = 0;
                    for (float& v : im) {
                        v = static_cast<float>(0.3 + g(rng));
                        mean += v;
                    }
                    const auto e = stack::RegionQuality(im.data(), n, 0, 0, n, n, mean / im.size(), model);
                    if (e.quality > 0) r.push_back(e.quality / e.sigma);
                }
                std::sort(r.begin(), r.end());
                pass += r[r.size() / 2] >= stack::kRegionDetailSigmas;
            }
            std::printf("window %d, %d candidates: noise passes the gate in %.2f%% of %d\n", n, C, 100.0 * pass / reps, reps);
        }
}
