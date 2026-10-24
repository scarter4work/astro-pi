// Test-only calibration probe (Ruling P26; test/calibration/, built by test/CMakeLists.txt): never shipped, not a
// ctest case (synthetic frames, test/unit/StackSynth.h). Built as calib-shot.
//
//   calib-shot <read noise> <gain> <blur> [frames (300)]
//
// A static textured disc (MakeDisc R 40, blurred) over sky 0.05 with read + shot noise (var = r^2 + t / g),
// measured by MeasureAll: mean quality against the noise-free capture's, and the scatter of quality against
// qualitySigma. Builds against the baseline too (MeasureAll's four-argument form).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include "Measure.h"
#include "StackSynth.h"
using namespace planetary;

std::vector<stack::FrameStats> Run(double read, double gain, double blur, int frames, unsigned seed) {
    const int w = 160, h = 160;
    testing::Scene s = testing::MakeDisc(3, 80.3, 79.6, 40);
    s.sky = 0.05;
    testing::Capture cap = testing::NewCapture(w, h);
    std::vector<float> clean(w * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) clean[y * w + x] = static_cast<float>(s.Eval(x, y, blur));
    std::mt19937 rng(seed);
    std::normal_distribution<double> n01(0, 1);
    for (int k = 0; k < frames; ++k) {
        std::vector<float> f = clean;
        if (read > 0 || gain > 0)
            for (float& v : f) v = static_cast<float>(v + std::sqrt(read * read + (gain > 0 ? v / gain : 0.0)) * n01(rng));
        cap.Add(f);
    }
    return stack::MeasureAll(cap.Factory(), cap.info, 8, {});
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: calib-shot <read noise> <gain> <blur> [frames (300)]\n");
        return 1;
    }
    const double read = std::atof(argv[1]), gain = std::atof(argv[2]), blur = std::atof(argv[3]);
    const int frames = argc > 4 ? std::atoi(argv[4]) : 300;
    const double q0 = Run(0, 0, blur, 3, 1)[0].quality;
    const auto st = Run(read, gain, blur, frames, 11);
    std::vector<double> q, sg, r;
    for (const auto& s : st)
        if (s.targetFound && s.quality > 0) {
            q.push_back(s.quality);
            sg.push_back(s.qualitySigma);
            r.push_back(s.quality / s.qualitySigma);
        }
    double m = 0, m2 = 0, ms2 = 0;
    for (size_t i = 0; i < q.size(); ++i) {
        m += q[i];
        m2 += q[i] * q[i];
        ms2 += sg[i] * sg[i];
    }
    const double N = q.size();
    m /= N;
    const double emp = std::sqrt(std::max(0.0, (m2 - N * m * m) / (N - 1))), rms = std::sqrt(ms2 / N);
    std::sort(r.begin(), r.end());
    std::printf("read %.4f gain %6.0f blur %.1f: q noise-free %.5f, noisy mean %.5f (%.4f), %zu with q > 0; sd(q) / rms sigma %.3f, median r %.1f\n",
                read, gain, blur, q0, m, m / q0, q.size(), emp / rms, r[r.size() / 2]);
}
