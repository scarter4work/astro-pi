// Test-only calibration probe (Ruling P26; test/calibration/, built by test/CMakeLists.txt): never shipped, not a
// ctest case (synthetic frames, test/unit/StackSynth.h). Built as calib-seedsel.
//
//   calib-seedsel [R (24)] [blur (24)] [sky (0.06)] [convolve 0/1 (1)]
//
// R 24 / 24 px over sky 0.06 (noise 0.01), seeds 7..907: SelectFrames' verdict and the step-7 reading.
// Head build: also with the q-domain likelihood (no quality2Sigma) and with the one-image sky noise.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include "Select.h"
#include "MeasuredDisc.h"
#include "StepSevenReading.h"
int main(int argc, char** argv) {
    const double R = argc > 1 ? std::atof(argv[1]) : 24, blur = argc > 2 ? std::atof(argv[2]) : 24;
    const double sky = argc > 3 ? std::atof(argv[3]) : 0.06;
    const bool conv = argc > 4 ? std::atoi(argv[4]) != 0 : true;
    int refused = 0;
    for (unsigned seed = 7; seed < 1000; seed += 100) {
        testing::Capture cap = MeasuredDiscCapture(R, blur, conv, 0.01, seed, sky);
        auto stats = stack::MeasureAll(cap.Factory(), cap.info, 8, {});
        const stack::Selection sel = stack::SelectFrames(stats, cap.info);
        const Reading r = ReadingOf(stats, cap.info);
        const bool out = sel.diagnosis.kind == stack::DiagnosisKind::OutOfFocus;
        refused += out;
        std::printf("seed %3u: X %.3f q_ref %.4f %s (reading %.3f)", seed, r.X, r.qRef, out ? "refused" : "stacked", sel.focusReading);
        auto noq2 = stats;
        for (auto& s : noq2) s.quality2Sigma = 0;
        const Reading r2 = ReadingOf(noq2, cap.info);
        std::vector<stack::FrameStats> sky1;
        auto src = cap.Factory()();
        for (int64_t i = 0; i < cap.info.frameCount; ++i)
            sky1.push_back(stack::MeasureFrame(stack::MeasurementLuma(src->ReadFrame(i), cap.info), i, 1));
        const Reading r3 = ReadingOf(sky1, cap.info);
        auto sky1noq2 = sky1;
        for (auto& s : sky1noq2) s.quality2Sigma = 0;
        const Reading r4 = ReadingOf(sky1noq2, cap.info);
        std::printf(" | q-likelihood X %.3f | sky noise X %.3f, + q-likelihood %.3f", r2.X, r3.X, r4.X);
        std::printf("\n");
    }
    std::printf("refused %d of 10\n", refused);
}
