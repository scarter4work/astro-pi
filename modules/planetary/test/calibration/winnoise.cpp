// Test-only calibration probe (Ruling P26; test/calibration/, built by test/CMakeLists.txt): never shipped, not a
// ctest case; it reads a real capture (the NAS is only read). Built as calib-winnoise.
//
//   calib-winnoise <video> <x0> <y0> <x1> <y1> [triples (300)]; env PASS1: pass 1 alone over the same runs
//
// Ground truth for one region's noise: the spatio-temporal responses ([1 -2 1] in time x [1 -2 1]^2 in
// space) restricted to a luma window, over consecutive triples -- the window's own noise variance --
// against the capture model's mean over the window's pixels (PixelVariance) and at its mean level.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "FrameSource.h"
#include "Measure.h"
#include "Noise.h"
using namespace planetary;
int main(int argc, char** argv) {
    if (argc < 6) {
        std::fprintf(stderr, "usage: calib-winnoise <video> <x0> <y0> <x1> <y1> [triples (300)]; env PASS1: pass 1 alone over the same runs\n");
        return 1;
    }
    const std::string path = argv[1];
    const int x0 = std::atoi(argv[2]), y0 = std::atoi(argv[3]), x1 = std::atoi(argv[4]), y1 = std::atoi(argv[5]);
    const int triples = argc > 6 ? std::atoi(argv[6]) : 300;
    const stack::SourceFactory f = [&] { return ser::OpenVideo(path, ser::BayerChoice::Auto); };
    const ser::FileInfo info = f()->Info();
    stack::NoiseModel model;
    if (std::getenv("PASS1")) {  // pass 1 alone over the same runs
        stack::NoiseSamples s1;
        auto r = f();
        for (const stack::NoiseRun& run : stack::NoiseRuns(info)) {
            std::vector<stack::NoisePlane> pl;
            for (int64_t i = 0; i < run.triples + 3; ++i) pl.emplace_back(stack::MeasurementLuma(r->ReadFrame(run.first + i), info));
            for (int64_t i = 0; i < run.triples; ++i) stack::AddTriple(pl[i], pl[i + 1], pl[i + 2], pl[i + 3], s1);
        }
        model = stack::FitNoise(s1, stack::LumaSites(info));
    } else {
        model = stack::FitNoiseModel(f, info, 16, {});
    }
    auto src = f();
    std::vector<ser::FloatImage> fr;
    double s2 = 0, n = 0, lev = 0, modelMean = 0, nm = 0;
    for (int k = 0; k < triples + 2; ++k) {
        fr.push_back(stack::MeasurementLuma(src->ReadFrame(k), info));
        if (fr.size() < 3) continue;
        const int w = fr[0].width;
        auto S = [&](const ser::FloatImage& im, int x, int y) {
            const float* p = im.data.data();
            double r = 0;
            const int c[3] = {1, -2, 1};
            for (int j = -1; j <= 1; ++j)
                for (int i = -1; i <= 1; ++i) r += c[i + 1] * c[j + 1] * p[(y + j) * w + x + i];
            return r;
        };
        for (int y = y0 + 1; y < y1 - 1; ++y)
            for (int x = x0 + 1; x < x1 - 1; ++x) {
                const double r = S(fr[0], x, y) - 2 * S(fr[1], x, y) + S(fr[2], x, y);
                s2 += r * r / 216;
                n += 1;
                lev += fr[1].data[y * w + x];
            }
        std::vector<float> v(static_cast<size_t>(x1 - x0) * (y1 - y0));
        stack::PixelVariance(fr[1].data.data(), w, x0, y0, x1, y1, model, v.data(), x1 - x0);
        for (float q : v) {
            modelMean += q;
            nm += 1;
        }
        fr.erase(fr.begin());
    }
    std::printf("window %d,%d-%d,%d: mean level %.4f; local noise %.4g; model mean over its pixels %.4g (%.3f), at the mean level %.4g\n", x0, y0,
                x1, y1, lev / n, s2 / n, modelMean / nm, modelMean / nm / (s2 / n), model.VarianceAtMean(lev / n));
}
