// Test-only calibration probe (Ruling P26; test/calibration/, built by test/CMakeLists.txt): never shipped, not a
// ctest case (synthetic frames, test/unit/StackSynth.h). Built as calib-q2steady.
//
//   calib-q2steady <frames> <quality^2 noise r> <seeds> [blur spread g (0)]
//
// A steady grossly blurred disc measured as MeasureFrame reports it: quality^2 = u^2 + N(0, rho^2), rho =
// r u^2, qualitySigma by Measure.h's soft-floored form, quality2Sigma = rho; N frames, `seeds` captures:
// how often step 7 calls it seeing.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include "Measure.h"
#include "Select.h"
using namespace planetary;
int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: calib-q2steady <frames> <quality^2 noise r> <seeds> [blur spread g (0)]\n");
        return 1;
    }
    const int N = std::atoi(argv[1]);
    const double r = std::atof(argv[2]), g = argc > 4 ? std::atof(argv[4]) : 0;
    const int seeds = std::atoi(argv[3]);
    ser::FileInfo info;
    info.header.width = 640;
    info.header.height = 480;
    info.header.depth = 8;
    info.colorId = ser::ColorId::Mono;
    info.normDepth = 8;
    int optics = 0, seeing = 0, tell = 0, none = 0;
    const int d = stack::DetectionBlock(640, 480);
    for (int sd = 1; sd <= seeds; ++sd) {
        std::mt19937 rng(static_cast<uint32_t>(sd));
        std::normal_distribution<double> n01(0, 1);
        std::vector<stack::FrameStats> v;
        for (int64_t i = 0; i < N; ++i) {
            const double u2 = 1 / (std::sqrt(M_PI) * 60 * 20.0 * (1 + g * n01(rng))), rho = r / (std::sqrt(M_PI) * 60 * 20.0);
            const double q2 = u2 + rho * n01(rng);
            stack::FrameStats s;
            s.index = i;
            s.targetFound = true;
            s.cx = 320;
            s.cy = 240;
            s.bx0 = 320 - 60 - d;
            s.by0 = 240 - 60 - d;
            s.bx1 = 320 + 60 + d;
            s.by1 = 240 + 60 + d;
            s.halfRadius = 60;
            s.brightness = 0.5;
            if (q2 > 0) {
                const double f = stack::kSigmaFloor * rho;
                s.quality = std::sqrt(q2);
                s.qualitySigma = rho / (2 * std::pow(q2 * q2 * q2 * q2 + f * f * f * f, 0.125));
                s.quality2Sigma = rho;
            }
            v.push_back(s);
        }
        const stack::Selection sel = stack::SelectFrames(v, info);
        const std::string& m = sel.diagnosis.message;
        if (sel.diagnosis.kind == stack::DiagnosisKind::None) ++none;
        else if (sel.diagnosis.kind == stack::DiagnosisKind::PoorSeeing) ++seeing;
        else if (m.find("the optics, not the air") != std::string::npos) ++optics;
        else ++tell;
    }
    std::printf("N %d rho/u^2 %g g %g: steady %d, seeing %d, can't tell %d, stacked %d of %d\n", N, r, g, optics, seeing, tell, none, seeds);
}
