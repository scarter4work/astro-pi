// Test-only calibration probe (Ruling P26; test/calibration/, built by test/CMakeLists.txt): never shipped, not a
// ctest case (synthetic frames, test/unit/StackSynth.h). Built as calib-modelerror.
//
//   calib-modelerror [R (24)] [blur (24)] [sky (0.06)] [seed (7)] [error (0.005)]
//
// How a capture model's error moves the frames' quality on a faint blob (Noise.h: the error of a capture's
// model is shared by all its frames): MeasuredDiscCapture (convolved, pixel noise 0.01) measured by MeasureAll
// with its own fitted model, and with that model's variance scaled by 1 + error (a, b and every knot); the
// mean quality of the frames each way. Also the fitted line's own relative sd at the blob's sky and disc
// levels, sqrt(LineVariance) / Line -- the size of the model's error when every pixel of every triple is read.
// Exit 0.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "MeasuredDisc.h"
#include "Measure.h"
#include "Noise.h"

using namespace planetary;

int main(int argc, char** argv) {
    const double R = argc > 1 ? std::atof(argv[1]) : 24, blur = argc > 2 ? std::atof(argv[2]) : 24;
    const double sky = argc > 3 ? std::atof(argv[3]) : 0.06;
    const unsigned seed = argc > 4 ? static_cast<unsigned>(std::atoi(argv[4])) : 7;
    const double error = argc > 5 ? std::atof(argv[5]) : 0.005;
    const testing::Capture cap = MeasuredDiscCapture(R, blur, true, 0.01, seed, sky);
    const stack::NoiseModel model = stack::FitNoiseModel(cap.Factory(), cap.info, 8, {});
    stack::NoiseModel off = model;
    off.a *= 1 + error;
    off.b *= 1 + error;
    for (auto& k : off.knots) k.variance *= 1 + error;
    off.Tabulate();
    auto meanQuality = [&](const stack::NoiseModel& m) {
        const auto stats = stack::MeasureAll(cap.Factory(), cap.info, 8, {}, m);
        double q = 0;
        for (const auto& s : stats) q += s.quality;
        return q / static_cast<double>(stats.size());
    };
    const double q0 = meanQuality(model), q1 = meanQuality(off);
    std::printf("R %g blur %g sky %g seed %u: mean quality %.4f with the fitted model, %.4f with its variance x %.3f (%+.1f%%)\n", R,
                blur, sky, seed, q0, q1, 1 + error, 100 * (q1 / q0 - 1));
    for (double t : {sky, sky + 0.1, 0.5})
        if (model.Line(t) > 0)
            std::printf("  the line's own relative sd at level %.3f: %.4f%%\n", t, 100 * std::sqrt(model.LineVariance(t)) / model.Line(t));
    return 0;
}
