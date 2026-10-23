// Test-only calibration probe (Ruling P26; test/calibration/, built by test/CMakeLists.txt): never shipped, not a
// ctest case; it reads a real capture (the NAS is only read). Built as calib-snr.
//
//   calib-snr <video> [first N frames, 0 = all]...
//
// Per-frame SNR of a capture's frame quality (P26): median q / qualitySigma, median quality2Sigma / q^2,
// and sd(q) / mean(q), over the first N frames (all if N = 0).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "FrameSource.h"
#include "Measure.h"
using namespace planetary;
int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: calib-snr <video> [first N frames, 0 = all]...\n");
        return 1;
    }
    const std::string path = argv[1];
    const stack::SourceFactory f = [&] { return ser::OpenVideo(path, ser::BayerChoice::Auto); };
    const ser::FileInfo info = f()->Info();
    const auto stats = stack::MeasureAll(f, info, 16, {});
    for (int ai = 2; ai < argc; ++ai) {
        const size_t N = std::atoi(argv[ai]) > 0 ? std::min<size_t>(std::atoi(argv[ai]), stats.size()) : stats.size();
        std::vector<double> r, rel2, q;
        for (size_t i = 0; i < N; ++i)
            if (stats[i].targetFound && stats[i].quality > 0) {
                r.push_back(stats[i].quality / stats[i].qualitySigma);
                rel2.push_back(stats[i].quality2Sigma / (stats[i].quality * stats[i].quality));
                q.push_back(stats[i].quality);
            }
        std::sort(r.begin(), r.end());
        std::sort(rel2.begin(), rel2.end());
        double m = 0, m2 = 0;
        for (double v : q) {
            m += v;
            m2 += v * v;
        }
        m /= q.size();
        std::printf("%zu frames: median q / qualitySigma %.1f, median rho / q^2 %.4f, sd(q) / mean %.4f\n", q.size(), r[r.size() / 2],
                    rel2[rel2.size() / 2], std::sqrt(std::max(0.0, m2 / q.size() - m * m)) / m);
    }
}
