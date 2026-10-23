// Test-only calibration probe (Ruling P26; test/calibration/, built by test/CMakeLists.txt): never shipped, not a
// ctest case (synthetic frames, test/unit/StackSynth.h). Built as calib-nm2.
//
//   calib-nm2 <read noise> <gain> <sky>
//
// The noise model's knots on the unit test's still scenes (DiscScene): read + shot, or clipped sky.
#include <cmath>
#include <cstdio>
#include <random>
#include "Noise.h"
#include "StackSynth.h"
using namespace planetary;
int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: calib-nm2 <read noise> <gain> <sky>\n");
        return 1;
    }
    const double read = std::atof(argv[1]), gain = std::atof(argv[2]), sky = std::atof(argv[3]);
    const int w = 192, h = 160;
    testing::Scene s = testing::MakeDisc(3, 0.5 * w + 0.3, 0.5 * h - 0.4, 0.3 * std::min(w, h));
    s.sky = sky;
    std::vector<float> clean(w * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) clean[y * w + x] = static_cast<float>(s.Eval(x, y, 0.8));
    std::mt19937 rng(4);
    std::normal_distribution<double> n01(0, 1);
    std::vector<ser::FloatImage> frames;
    for (int k = 0; k < 40; ++k) {
        ser::FloatImage im{w, h, 1, clean};
        for (float& v : im.data)
            v = static_cast<float>(std::clamp(v + std::sqrt(read * read + (gain > 0 ? std::max(0.0f, v) / gain : 0.0)) * n01(rng), 0.0, 1.0));
        frames.push_back(im);
    }
    const stack::NoiseModel m = stack::FitNoiseModel(frames);
    std::printf("a %.4g b %.4g\n", m.a, m.b);
    for (size_t i = 0; i < m.knots.size(); i += std::max<size_t>(1, m.knots.size() / 40))
        std::printf("  %.4f v %.4g line %.4g truth %.4g %s\n", m.knots[i].level, m.knots[i].variance, m.Line(m.knots[i].level),
                    read * read + (gain > 0 ? m.knots[i].level / gain : 0), m.knots[i].clipped ? "CLIPPED" : "");
}
