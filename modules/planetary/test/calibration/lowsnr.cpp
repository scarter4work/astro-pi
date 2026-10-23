// Test-only calibration probe (Ruling P26; test/calibration/, built by test/CMakeLists.txt): never shipped, not a
// ctest case (synthetic frames, test/unit/StackSynth.h). Built as calib-lowsnr.
//
//   calib-lowsnr (no arguments)
//
// test_regions' "A low-SNR region with real seeing variation keeps a lucky subset": per region the median
// q / qualitySigma over its candidates, and q's scatter against the rms qualitySigma.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include "Measure.h"
#include "Regions.h"
#include "StackSynth.h"
using namespace planetary;
using namespace planetary::testing;
namespace {
ser::FloatImage Render(const Scene& s, int w, int h, const FrameSpec& f, unsigned seed) {
    return ser::FloatImage{w, h, 1, RenderFrame(s, w, h, f, seed)};
}
ser::FloatImage Truth(const Scene& s, int w, int h, double sigma) {
    ser::FloatImage im{w, h, 1, std::vector<float>(static_cast<size_t>(w) * h)};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) im.data[static_cast<size_t>(y) * w + x] = static_cast<float>(s.Eval(x, y, sigma));
    return im;
}
Field Constant(double v) {
    Field f;
    f.nx = f.ny = 1;
    f.v = {v};
    return f;
}
}  // namespace
int main() {
    const int w = 256, h = 192, frames = 150;
    const Scene s = MakeSurface(21, w, h);
    ser::FloatImage ref = Truth(s, w, h, 0.6);
    double level = 0;
    for (const float v : ref.data) level += v;
    level /= static_cast<double>(ref.data.size());
    for (float& v : ref.data) v = static_cast<float>(0.4 + 0.5 * (v - level));
    const stack::RegionLayout lay = stack::PlaceRegions(ref, stack::MeasureFrame(ref, 0, 1), true, WhiteNoise(0.01));
    const stack::RegionMeasurer m(ref, lay);
    std::mt19937 rng(9);
    std::uniform_real_distribution<double> uni(0, 1);
    std::normal_distribution<double> noise(0, 0.01);
    std::vector<stack::FrameRegions> measured;
    for (int k = 0; k < frames; ++k) {
        FrameSpec f;
        f.hasLocal = true;
        f.dx = Constant(0);
        f.dy = Constant(0);
        f.noise = 0;
        f.sigma = Constant(0.5 + 2.0 * uni(rng));
        ser::FloatImage frame = Render(s, w, h, f, 100u + static_cast<unsigned>(k));
        for (float& v : frame.data) v = static_cast<float>(0.4 + 0.5 * (v - level) + noise(rng));
        measured.push_back(m.Measure(frame, k, stack::Shift{}));
    }
    std::vector<double> r50s;
    for (size_t r = 0; r < lay.regions.size(); ++r) {
        std::vector<double> ratio;
        for (const auto& fr : measured)
            if (fr.regions[r].aligned && fr.regions[r].quality > 0) ratio.push_back(fr.regions[r].quality / fr.regions[r].qualitySigma);
        if (ratio.empty()) continue;
        std::sort(ratio.begin(), ratio.end());
        r50s.push_back(ratio[ratio.size() / 2]);
    }
    std::sort(r50s.begin(), r50s.end());
    std::printf("under 4: %ld\n", std::count_if(r50s.begin(), r50s.end(), [](double v) { return v < 4; }));
    std::printf("%zu regions: median q / qualitySigma min %.2f p10 %.2f median %.2f max %.2f\n", r50s.size(), r50s.front(),
                r50s[r50s.size() / 10], r50s[r50s.size() / 2], r50s.back());
}
