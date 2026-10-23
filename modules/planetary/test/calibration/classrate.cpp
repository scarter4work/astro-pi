// Test-only calibration probe (Ruling P26; test/calibration/, built by test/CMakeLists.txt): never shipped, not a
// ctest case (synthetic frames, test/unit/StackSynth.h). Built as calib-classrate.
//
//   calib-classrate <R> <limb blur> <frames> <blur spread g> <pixel noise> <seeds> [threads (32)]; env PLAIN: texture-free disc, QSPREAD: seed 0's quality spread
//
// Step 7's steady / seeing classifier through MeasureFrame (Ruling P26's qualitySigma): a grossly blurred
// disc (radius R, limb blur b0 (1 + g n) per frame), white pixel noise s, N frames, `seeds` captures.
// Counts the verdicts: "the optics" (steady), PoorSeeing, "to tell", not diagnosed; and median sigma / q.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include "Measure.h"
#include "Select.h"
#include "StackSynth.h"
using namespace planetary;
int main(int argc, char** argv) {
    if (argc < 7) {
        std::fprintf(stderr, "usage: calib-classrate <R> <limb blur> <frames> <blur spread g> <pixel noise> <seeds> [threads (32)]; env PLAIN: texture-free disc, QSPREAD: seed 0's quality spread\n");
        return 1;
    }
    const double R = std::atof(argv[1]), b0 = std::atof(argv[2]);
    const int N = std::atoi(argv[3]);
    const double g = std::atof(argv[4]), s = std::atof(argv[5]);
    const int seeds = std::atoi(argv[6]), threads = argc > 7 ? std::atoi(argv[7]) : 32;
    const int half = static_cast<int>(R + 3 * b0 * (1 + 4 * g) + 30), w = 2 * half, h = 2 * half;
    testing::Scene sc = testing::MakeDisc(3, half + 0.3, half - 0.2, R);
    if (std::getenv("PLAIN")) {  // the disc model alone: q^2 = 1 / (sqrt(pi) R sigma)
        sc.waves.clear();
        sc.blobs.clear();
    }
    // Clean frames at blur b0 (1 + k step), k over +-4.5 g.
    const double step = 0.0025;
    const int K = g > 0 ? static_cast<int>(std::ceil(4.5 * g / step)) : 0;
    std::vector<std::vector<float>> clean(2 * K + 1, std::vector<float>(static_cast<size_t>(w) * h));
    {
        std::vector<std::thread> pool;
        std::atomic<int> next{0};
        for (int t = 0; t < threads; ++t)
            pool.emplace_back([&] {
                for (int k; (k = next++) < 2 * K + 1;) {
                    const double b = b0 * (1 + (k - K) * step);
                    for (int y = 0; y < h; ++y)
                        for (int x = 0; x < w; ++x) clean[k][static_cast<size_t>(y) * w + x] = static_cast<float>(sc.Eval(x, y, b));
                }
            });
        for (auto& t : pool) t.join();
    }
    ser::FileInfo info;
    info.header.width = w;
    info.header.height = h;
    info.header.depth = 16;
    info.colorId = ser::ColorId::Mono;
    info.normDepth = 16;
    const stack::NoiseModel model = testing::WhiteNoise(s);
    std::atomic<int> next{0}, optics{0}, seeing{0}, tell{0}, none{0};
    std::mutex m;
    std::vector<double> ratios;
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t)
        pool.emplace_back([&] {
            for (int sd; (sd = next++) < seeds;) {
                std::mt19937 rng(1000u + static_cast<unsigned>(sd));
                std::normal_distribution<double> n01(0, 1);
                std::vector<stack::FrameStats> stats;
                ser::FloatImage im{w, h, 1, std::vector<float>(static_cast<size_t>(w) * h)};
                double rs = 0;
                for (int i = 0; i < N; ++i) {
                    const int k = std::clamp(K + static_cast<int>(std::lround(g * n01(rng) / step)), 0, 2 * K);
                    const auto& c = clean[k];
                    for (size_t p = 0; p < c.size(); ++p) im.data[p] = static_cast<float>(c[p] + s * n01(rng));
                    stats.push_back(stack::MeasureFrame(im, i, 1, model));
                    if (i == N / 2 && stats.back().quality > 0) rs = stats.back().qualitySigma / stats.back().quality;
                }
                if (std::getenv("QSPREAD") && sd == 0) {
                    double a = 0, a2 = 0;
                    for (const auto& st : stats) { a += st.quality; a2 += st.quality * st.quality; }
                    a /= N;
                    std::printf("q spread %.4f (sd / mean)\n", std::sqrt(a2 / N - a * a) / a);
                }
                const stack::Selection sel = stack::SelectFrames(stats, info);
                const std::string& msg = sel.diagnosis.message;
                if (sel.diagnosis.kind == stack::DiagnosisKind::None) ++none;
                else if (sel.diagnosis.kind == stack::DiagnosisKind::PoorSeeing) ++seeing;
                else if (msg.find("the optics, not the air") != std::string::npos) ++optics;
                else if (msg.find("to tell") != std::string::npos) ++tell;
                std::lock_guard<std::mutex> lock(m);
                ratios.push_back(rs);
            }
        });
    for (auto& t : pool) t.join();
    std::sort(ratios.begin(), ratios.end());
    std::printf("R %g blur %g N %d g %g noise %g: sigma/q %.3f | steady %d, seeing %d, can't tell %d, stacked %d of %d\n", R, b0, N,
                g, s, ratios[ratios.size() / 2], optics.load(), seeing.load(), tell.load(), none.load(), seeds);
}
