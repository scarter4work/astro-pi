// Test-only calibration probe (Ruling P26; test/calibration/, built by test/CMakeLists.txt): never shipped, not a
// ctest case (synthetic frames, test/unit/StackSynth.h). Built as calib-mdq.
//
//   calib-mdq <R> <blur> <convolve 0/1> <noise> <sky> [seed (7)]; env MDQ_DUMP: each frame's q, qualitySigma, quality2Sigma to stderr
//
// A MeasuredDisc capture: its noise model, per-frame quality (mean, frames with q 0), Select's verdict and
// its step-7 reading (Selection::focusReading), with X = sigma(q_ref) / R_h and round 1's Xg
// (StepSevenReading.h), against the same capture noise-free.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include "Select.h"
#include "MeasuredDisc.h"
#include "StepSevenReading.h"
int main(int argc, char** argv) {
    if (argc < 6) {
        std::fprintf(stderr, "usage: calib-mdq <R> <blur> <convolve 0/1> <noise> <sky> [seed (7)]; env MDQ_DUMP: each frame's q, qualitySigma, quality2Sigma to stderr\n");
        return 1;
    }
    const double R = std::atof(argv[1]), blur = std::atof(argv[2]), noise = std::atof(argv[4]), sky = std::atof(argv[5]);
    const bool conv = std::atoi(argv[3]) != 0;
    const unsigned seed = argc > 6 ? std::atoi(argv[6]) : 7;
    for (int clean = 0; clean < 2; ++clean) {
        testing::Capture cap = MeasuredDiscCapture(R, blur, conv, clean ? 0.0 : noise, seed, sky);
        const stack::NoiseModel m = stack::FitNoiseModel(cap.Factory(), cap.info, 8, {});
        const auto stats = stack::MeasureAll(cap.Factory(), cap.info, 8, {}, m);
        const stack::Selection sel = stack::SelectFrames(stats, cap.info);
        const Reading r = ReadingOf(stats, cap.info);
        double mq = 0, mq2 = 0, ms2 = 0;
        int zero = 0, pos = 0;
        std::vector<double> rr;
        for (const auto& s : stats) {
            mq += s.quality;
            zero += s.quality == 0;
            if (s.quality > 0) {
                ++pos;
                mq2 += s.quality * s.quality;
                ms2 += s.qualitySigma * s.qualitySigma;
                rr.push_back(s.quality / s.qualitySigma);
            }
        }
        if (std::getenv("MDQ_DUMP") && !clean)
            for (const auto& s : stats)
                if (s.quality > 0) std::fprintf(stderr, "%.6f %.6f %.6f\n", s.quality, s.qualitySigma, s.quality2Sigma);
        if (pos > 5) {
            std::vector<double> q, sg, sc;
            double rms = 0;
            for (const auto& s : stats)
                if (s.quality > 0) {
                    q.push_back(s.quality);
                    sg.push_back(s.qualitySigma);
                    rms += s.qualitySigma * s.qualitySigma;
                }
            rms = std::sqrt(rms / q.size());
            sc.assign(q.size(), rms);
            std::vector<double> s13 = sg;
            for (double& v : s13) v *= 1.13;
            std::printf("   q_ref: own sigma %.4f, one rms sigma %.4f, own x1.13 %.4f\n", stack::RankByDetail(q, sg, 5, 8).qRef,
                        stack::RankByDetail(q, sc, 5, 8).qRef, stack::RankByDetail(q, s13, 5, 8).qRef);
        }
        if (pos > 1) {
            const double m1 = (mq) / pos;
            std::sort(rr.begin(), rr.end());
            std::printf("   q>0 frames %d: sd(q) %.4f, rms qualitySigma %.4f (ratio %.2f), median r %.2f\n", pos,
                        std::sqrt(std::max(0.0, mq2 / pos - m1 * m1)), std::sqrt(ms2 / pos),
                        std::sqrt(std::max(0.0, mq2 / pos - m1 * m1)) / std::sqrt(ms2 / pos), rr[rr.size() / 2]);
        }
        int clipped = 0;
        for (const auto& k : m.knots) clipped += k.clipped;
        // reading: Select's own (Selection::focusReading, the shipped step 7); X at q_ref; Xg round 1's (StepSevenReading.h).
        std::printf("%s: model a %.4g b %.3g knots %zu clipped %d | mean q %.4f, q=0 in %d | q_ref %.4f reading %.3f X %.3f "
                    "Xg (round 1) %.3f (muG %.4f sdG %.4f tauLo %.4f) | %s\n",
                    clean ? "noise-free" : "noisy     ", m.a, m.b, m.knots.size(), clipped, mq / stats.size(), zero, r.qRef,
                    sel.focusReading, r.X, r.Xg, r.muG, r.sdG, r.tauLo,
                    sel.diagnosis.kind == stack::DiagnosisKind::None ? "stacked" : "DIAGNOSED");
    }
}
