// Test-only calibration probe (Ruling P26; test/calibration/, built by test/CMakeLists.txt): never shipped, not a
// ctest case (synthetic frames, test/unit/StackSynth.h). Built as calib-moon.
//
//   calib-moon [dark (0.1)] [gain (400)] [limb x (236)] [frames (80)] [out.ser]
//
// A Moon-like capture with a dark limb: MakeSurface brightened on the left and darkened to `dark` of its
// level toward the right (a terminator / limb band), black sky past `limb`; read + shot noise; seeing.
// RunStack; per region: kept, candidates, noise only. Builds against the baseline too.
#include <cstdio>
#include <random>
#include "Engine.h"
#include "SerWriter.h"
#include "StackSynth.h"
using namespace planetary;
int main(int argc, char** argv) {
    const double dark = argc > 1 ? std::atof(argv[1]) : 0.1, gain = argc > 2 ? std::atof(argv[2]) : 400;
    const int limb = argc > 3 ? std::atoi(argv[3]) : 236, frames = argc > 4 ? std::atoi(argv[4]) : 80;
    const int w = 256, h = 192;
    const testing::Scene s = testing::MakeSurface(31, w, h);
    auto profile = [&](double x) {
        if (x >= limb) return 0.0;
        if (x < 120) return 1.0;
        const double t = std::min(1.0, (x - 120) / 80.0);
        return 1.0 + (dark - 1.0) * t;
    };
    testing::Capture cap = testing::NewCapture(w, h);
    std::mt19937 rng(5), noise(9);
    std::normal_distribution<double> n01(0, 1);
    for (int k = 0; k < frames; ++k) {
        testing::FrameSpec f = testing::Seeing(rng, w, h, 1.0, 64, 1.0);
        f.noise = 0;
        std::vector<float> v = testing::RenderFrame(s, w, h, f, 100u + k);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const double t = v[y * w + x] * profile(x - f.gx);
                v[y * w + x] = static_cast<float>(t + std::sqrt(0.003 * 0.003 + t / gain) * n01(noise));
            }
        cap.Add(v);
    }
    if (argc > 5) {  // also as a 16-bit SER, for stack-region-probe
        testing::SerSpec spec;
        spec.width = w;
        spec.height = h;
        spec.depth = 16;
        spec.frameCount = frames;
        std::vector<std::vector<uint8_t>> bytes;
        for (const auto& fr : *cap.frames) {
            std::vector<uint8_t> b;
            for (uint16_t v : fr) {
                b.push_back(static_cast<uint8_t>(v & 0xff));
                b.push_back(static_cast<uint8_t>(v >> 8));
            }
            bytes.push_back(std::move(b));
        }
        testing::WriteSer(argv[5], spec, bytes);
    }
    const stack::StackOutcome o = stack::RunStack(cap.Factory(), 8, {});
    std::printf("dark %.2f gain %.0f limb %d: %s\n  %s\n", dark, gain, limb, o.summary.selection.c_str(), o.diagnosis.message.c_str());
    int zero = 0, noiseOnly = 0;
    for (size_t r = 0; r < o.frames.regionDetail.size(); ++r) {
        const auto& d = o.frames.regionDetail[r];
        zero += d.kept == 0;
        noiseOnly += d.noiseOnly;
    }
    std::printf("  regions %d: kept 0 in %d (noise only %d)\n", o.frames.regions, zero, noiseOnly);
    for (const auto& line : o.summary.warnings) std::printf("  warning: %s\n", line.c_str());
    std::printf("  %s\n", o.summary.alignment.c_str());
}
