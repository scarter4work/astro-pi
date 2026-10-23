// Test-only calibration probe (Ruling P26; test/calibration/, built by test/CMakeLists.txt): never shipped, not a
// ctest case (synthetic frames, test/unit/StackSynth.h). Built as calib-synthrate.
//
//   calib-synthrate <frames> <blur spread g> <quality noise> [seeds (200)] [limb blur px (24)]
//
// Select.h step 7's documented protocol: synthetic disc FrameStats (R 60, limb blur b (1 + g n), b 24 by default: 20,
// the protocol before Ruling P15a, reads under kGrossBlurLimit 0.28 at 6% seeing's sharpest frames), quality
// noise `noise` (Gaussian, qualitySigma exact), N frames, 200 seeds: the verdict counts.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include "Select.h"
using namespace planetary;
int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: calib-synthrate <frames> <blur spread g> <quality noise> [seeds (200)] [limb blur px (24)]\n");
        return 1;
    }
    const int N = std::atoi(argv[1]);
    const double g = std::atof(argv[2]), noise = std::atof(argv[3]);
    const int seeds = argc > 4 ? std::atoi(argv[4]) : 200;
    const double blur = argc > 5 ? std::atof(argv[5]) : 24;
    ser::FileInfo info;
    info.header.width = 640;
    info.header.height = 480;
    info.header.depth = 8;
    info.colorId = ser::ColorId::Mono;
    info.normDepth = 8;
    int optics = 0, seeing = 0, tell = 0, none = 0;
    for (int sd = 1; sd <= seeds; ++sd) {
        std::mt19937 rng(static_cast<uint32_t>(sd));
        std::normal_distribution<double> n01(0, 1);
        std::vector<stack::FrameStats> v;
        const int d = stack::DetectionBlock(640, 480);
        for (int64_t i = 0; i < N; ++i) {
            const double qt = 1 / std::sqrt(std::sqrt(M_PI) * 60 * blur * (1 + g * n01(rng)));
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
            s.quality = qt * (1 + noise * n01(rng));
            s.qualitySigma = noise * qt;
            v.push_back(s);
        }
        const stack::Selection sel = stack::SelectFrames(v, info);
        const std::string& m = sel.diagnosis.message;
        if (sel.diagnosis.kind == stack::DiagnosisKind::None) ++none;
        else if (sel.diagnosis.kind == stack::DiagnosisKind::PoorSeeing) ++seeing;
        else if (m.find("the optics, not the air") != std::string::npos) ++optics;
        else ++tell;
    }
    std::printf("synthetic R 60 limb %g N %d g %g noise %g: steady %d, seeing %d, can't tell %d, stacked %d of %d\n", blur, N, g, noise,
                optics, seeing, tell, none, seeds);
}
