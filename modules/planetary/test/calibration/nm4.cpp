// Test-only calibration probe (Ruling P26; test/calibration/, built by test/CMakeLists.txt): never shipped, not a
// ctest case (synthetic frames, test/unit/StackSynth.h). Built as calib-nm4.
//
//   calib-nm4 <read noise> <gain> <blur>
//
// Two-pass internals on the shot-noise test's disc: pass 1's line, pass 2's tiles.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include "Noise.h"
#include "StackSynth.h"
using namespace planetary;
int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: calib-nm4 <read noise> <gain> <blur>\n");
        return 1;
    }
    const double read = std::atof(argv[1]), gain = std::atof(argv[2]), blur = std::atof(argv[3]);
    const int w = 160, h = 160;
    testing::Scene s = testing::MakeDisc(3, 80.3, 79.6, 40);
    s.sky = 0.05;
    std::vector<float> clean(w * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) clean[y * w + x] = static_cast<float>(s.Eval(x, y, blur));
    std::mt19937 rng(11);
    std::normal_distribution<double> n01(0, 1);
    std::vector<stack::NoisePlane> planes;
    for (int k = 0; k < 60; ++k) {
        ser::FloatImage im{w, h, 1, clean};
        for (float& v : im.data) v = static_cast<float>(v + std::sqrt(read * read + std::max(0.0f, v) / gain) * n01(rng));
        planes.emplace_back(im);
    }
    stack::NoiseSamples s1, s2;
    for (int k = 0; k + 3 < 60; ++k) stack::AddTriple(planes[k], planes[k + 1], planes[k + 2], planes[k + 3], s1);
    const stack::NoiseModel first = stack::FitNoise(s1, 1);
    for (int k = 0; k + 3 < 60; ++k) stack::AddTriple(planes[k], planes[k + 1], planes[k + 2], planes[k + 3], s2, &first);
    std::printf("pass 1: a %.4g b %.4g; tiles %zu overlap %.3f\n", first.a, first.b, s2.tiles.size(), s2.tileOverlap);
    std::vector<double> r, k, n;
    double mr = 0;
    for (auto& t : s2.tiles) {
        r.push_back(t.ratio);
        k.push_back(t.kurtosis);
        n.push_back(t.n);
        mr += t.ratio;
    }
    mr /= r.size();
    double sd = 0;
    for (double x : r) sd += (x - mr) * (x - mr);
    sd = std::sqrt(sd / r.size());
    std::sort(r.begin(), r.end());
    std::sort(k.begin(), k.end());
    std::sort(n.begin(), n.end());
    std::printf("ratio mean %.4f q25 %.4f median %.4f sd %.4f; kurtosis median %.3f; n median %.0f\n", mr, r[r.size() / 4],
                r[r.size() / 2], sd, k[k.size() / 2], n[n.size() / 2]);
    const stack::NoiseModel fin = stack::FitNoise(s1, s2, 1);
    std::printf("final: a %.4g b %.4g\n", fin.a, fin.b);
    for (double x : {0.04, 0.045, 0.048, 0.05, 0.052, 0.055, 0.06, 0.08, 0.15, 0.3, 0.5, 0.55})
        std::printf("  level %.3f truth %.4g pass1 %.4g (%.3f) final %.4g (%.3f)\n", x, read * read + x / gain, first.VarianceAtMean(x),
                    first.VarianceAtMean(x) / (read * read + x / gain), fin.VarianceAtMean(x), fin.VarianceAtMean(x) / (read * read + x / gain));
    for (const auto& k : first.knots)
        if (k.level > 0.03 && k.level < 0.07)
            std::printf("    pass1 knot %.4f v %.4g %s\n", k.level, k.variance, k.clipped ? "CLIPPED" : "");
    // Sky-only tiles' ratios.
    std::vector<double> sky;
    for (auto& t : s2.tiles)
        if (t.level > 0.045 && t.level < 0.055) sky.push_back(t.ratio);
    std::sort(sky.begin(), sky.end());
    double ms = 0;
    for (double x : sky) ms += x;
    if (!sky.empty()) std::printf("sky tiles %zu: mean ratio %.4f q25 %.4f\n", sky.size(), ms / sky.size(), sky[sky.size() / 4]);
}
