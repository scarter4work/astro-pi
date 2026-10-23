// Test-only calibration probe (Ruling P26; test/calibration/, built by test/CMakeLists.txt): never shipped, not a
// ctest case; it reads a real capture (the NAS is only read). Built as calib-passes.
//
//   calib-passes <video> <first frame> <triples>
//
// A real capture's two passes over frames [first, first + n): pass 1's curve, the tiles' ratio quantiles by
// level, and the final curve, at a list of levels; optionally only tiles inside a window.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "FrameSource.h"
#include "Measure.h"
#include "Noise.h"
using namespace planetary;
int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: calib-passes <video> <first frame> <triples>\n");
        return 1;
    }
    const std::string path = argv[1];
    const int first = std::atoi(argv[2]), n = std::atoi(argv[3]);
    const auto src = ser::OpenVideo(path, ser::BayerChoice::Auto);
    const ser::FileInfo info = src->Info();
    std::vector<stack::NoisePlane> planes;
    for (int i = 0; i < n + 3; ++i) planes.emplace_back(stack::MeasurementLuma(src->ReadFrame(first + i), info));
    stack::NoiseSamples s1, s2;
    for (int i = 0; i < n; ++i) stack::AddTriple(planes[i], planes[i + 1], planes[i + 2], planes[i + 3], s1);
    const int sites = stack::LumaSites(info);
    const stack::NoiseModel m1 = stack::FitNoise(s1, sites);
    for (int i = 0; i < n; ++i) stack::AddTriple(planes[i], planes[i + 1], planes[i + 2], planes[i + 3], s2, &m1);
    const stack::NoiseModel m2 = stack::FitNoise(s1, s2, sites);
    std::printf("tiles %zu\n", s2.tiles.size());
    for (double x : {0.0137, 0.0157, 0.02, 0.03, 0.047, 0.07, 0.1, 0.13, 0.2, 0.27, 0.4, 0.6, 0.8, 0.9, 0.94}) {
        std::vector<double> r, kap;
        for (const auto& t : s2.tiles)
            if (std::fabs(t.level - x) < 0.1 * x) {
                r.push_back(t.ratio);
                kap.push_back(t.kurtosis);
            }
        std::sort(r.begin(), r.end());
        std::sort(kap.begin(), kap.end());
        if (!kap.empty()) std::printf("  kurtosis median %.2f, zero-ratio tiles %ld\n", kap[kap.size() / 2], std::count(r.begin(), r.end(), 0.0));
        const auto q = [&](double p) { return r.empty() ? 0.0 : r[static_cast<size_t>(p * (r.size() - 1))]; };
        std::printf("level %.3f: pass1 %.4g final %.4g (%.3f) | tiles within 10%%: %zu, ratio q10 %.3f q25 %.3f q50 %.3f q75 %.3f\n", x,
                    m1.VarianceAtMean(x), m2.VarianceAtMean(x), m2.VarianceAtMean(x) / m1.VarianceAtMean(x), r.size(), q(0.1), q(0.25),
                    q(0.5), q(0.75));
    }
}
