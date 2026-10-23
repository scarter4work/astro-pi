// Test-only calibration probe (Ruling P26; test/calibration/, built by test/CMakeLists.txt): never shipped, not a
// ctest case (synthetic frames, test/unit/StackSynth.h). Built as calib-regsig.
//
//   calib-regsig (no arguments)
//
// test_regions' "a region's qualitySigma tracks ..." at lower contrasts: median r, candidates, scatter over
// qualitySigma's rms and median.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include "Measure.h"
#include "StackSynth.h"
using namespace planetary;
int main(int argc, char** argv) {
    const int n = 64, runs = 2000;
    const stack::NoiseModel model = testing::WhiteNoise(0.01);
    std::mt19937 tr(5);
    std::uniform_real_distribution<double> u(0, 1);
    struct Wave {
        double fx, fy, ph, a;
    };
    std::vector<Wave> waves;
    for (int k = 0; k < 12; ++k) waves.push_back({0.02 + 0.15 * u(tr), 0.02 + 0.15 * u(tr), 2 * M_PI * u(tr), 0.5 + u(tr)});
    for (int ai = 1; ai < argc; ++ai) {
        const double A = std::atof(argv[ai]);
        std::vector<float> clean(static_cast<size_t>(n) * n);
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                double v = 0.3;
                for (const Wave& wv : waves) v += A * wv.a * std::sin(2 * M_PI * (wv.fx * x + wv.fy * y) + wv.ph);
                clean[static_cast<size_t>(y) * n + x] = static_cast<float>(v);
            }
        std::mt19937 rng(77);
        std::normal_distribution<double> g(0, 0.01);
        std::vector<double> q, sg, ratio;
        for (int k = 0; k < runs; ++k) {
            std::vector<float> im = clean;
            double mean = 0;
            for (float& v : im) {
                v = static_cast<float>(v + g(rng));
                mean += v;
            }
            const stack::QualityEstimate e = stack::RegionQuality(im.data(), n, 0, 0, n, n, mean / (n * n), model);
            if (!(e.quality > 0)) continue;
            q.push_back(e.quality);
            sg.push_back(e.sigma);
            ratio.push_back(e.quality / e.sigma);
        }
        double m = 0, m2 = 0, s2 = 0;
        for (size_t i = 0; i < q.size(); ++i) {
            m += q[i];
            m2 += q[i] * q[i];
            s2 += sg[i] * sg[i];
        }
        const double N = static_cast<double>(q.size());
        m /= N;
        const double emp = std::sqrt((m2 - N * m * m) / (N - 1)), rms = std::sqrt(s2 / N);
        std::sort(sg.begin(), sg.end());
        std::sort(ratio.begin(), ratio.end());
        std::printf("A %.4f: %zu of %d with detail, median r %.2f, scatter / rms %.3f, / median %.3f\n", A, q.size(), runs,
                    ratio[ratio.size() / 2], emp / rms, emp / sg[sg.size() / 2]);
    }
}
