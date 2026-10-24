// Test-only calibration probe (Ruling P26; test/calibration/, built by test/CMakeLists.txt): never shipped, not a
// ctest case; it reads a real capture (the NAS is only read). Built as calib-nreal.
//
//   calib-nreal <video>; env NREAL_KNOTS=k: every k-th knot
//
// The capture noise model of a real video; NREAL_KNOTS=k prints every k-th knot.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "FrameSource.h"
#include "Measure.h"
#include "Noise.h"
using namespace planetary;
int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: calib-nreal <video>; env NREAL_KNOTS=k: every k-th knot\n");
        return 1;
    }
    const std::string path = argv[1];
    const stack::SourceFactory f = [&] { return ser::OpenVideo(path, ser::BayerChoice::Auto); };
    const ser::FileInfo info = f()->Info();
    const stack::NoiseModel m = stack::FitNoiseModel(f, info, 16, {});
    const ser::FloatImage l0 = stack::MeasurementLuma(f()->ReadFrame(0), info);
    const double s = stack::EstimatePixelNoise(l0).sigma;
    const int every = std::getenv("NREAL_KNOTS") ? std::atoi(std::getenv("NREAL_KNOTS")) : 0;
    int clipped = 0;
    for (size_t i = 0; i < m.knots.size(); ++i) {
        clipped += m.knots[i].clipped;
        if (every > 0 && (i % every == 0 || m.knots[i].clipped))
            std::printf("  knot %.4f v %.4g line %.4g %s\n", m.knots[i].level, m.knots[i].variance, m.Line(m.knots[i].level),
                        m.knots[i].clipped ? "CLIPPED" : "");
    }
    std::printf("%s: %dx%d | a %.4g b %.4g (sd %.2g %.2g) | knots %zu, clipped %d | sky tile s^2 %.4g | VarianceAtMean 0.02/0.1/0.3/0.6/0.9: %.4g %.4g %.4g %.4g %.4g\n",
                path.c_str(), l0.width, l0.height, m.a, m.b, std::sqrt(m.varA), std::sqrt(m.varB), m.knots.size(), clipped, s * s,
                m.VarianceAtMean(0.02), m.VarianceAtMean(0.1), m.VarianceAtMean(0.3), m.VarianceAtMean(0.6), m.VarianceAtMean(0.9));
    std::printf("at 0.80 / 0.85 / 0.90 / 0.94: %.4g %.4g %.4g %.4g\n", m.VarianceAtMean(0.8), m.VarianceAtMean(0.85), m.VarianceAtMean(0.9), m.VarianceAtMean(0.94));
}
