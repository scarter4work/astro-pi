// Test-only calibration probe (Ruling P26; test/calibration/, built by test/CMakeLists.txt): never shipped, not a
// ctest case; it reads a real capture (the NAS is only read). Built as calib-injtiles.
//
//   calib-injtiles <video> <first frame> <triples> <dn>
//
// I5 diagnosis: one run of `n` triples from frame `first`, with and without injected noise of sd `dn` DN:
// one-level tiles per level band, their median ratio, pass 1 and the final model by band.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
#include "FrameSource.h"
#include "Measure.h"
#include "Noise.h"
using namespace planetary;
int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: calib-injtiles <video> <first frame> <triples> <dn>\n");
        return 1;
    }
    const std::string path = argv[1];
    const int first = std::atoi(argv[2]), n = std::atoi(argv[3]);
    const double dn = std::atof(argv[4]);
    const auto src = ser::OpenVideo(path, ser::BayerChoice::Auto);
    const ser::FileInfo info = src->Info();
    const int sites = stack::LumaSites(info);
    const double mx = std::ldexp(1.0, info.normDepth) - 1;
    std::vector<stack::NoiseRun> runs = stack::NoiseRuns(info);
    if (n > 0) runs = {{first, n}};
    for (int inj = 0; inj < 2; ++inj) {
        auto planes = [&](const stack::NoiseRun& run) {
            std::vector<stack::NoisePlane> pl;
            for (int64_t i = 0; i < run.triples + 3; ++i) {
                ser::Frame f = src->ReadFrame(run.first + i);
                if (inj) {
                    std::mt19937_64 rng(0x9e3779b97f4a7c15ull ^ static_cast<uint64_t>(run.first + i));
                    std::normal_distribution<double> g(0, dn);
                    for (auto& v : f.samples) v = static_cast<uint16_t>(std::clamp(std::lround(v + g(rng)), 0L, static_cast<long>(mx)));
                }
                pl.emplace_back(stack::MeasurementLuma(f, info));
            }
            return pl;
        };
        stack::NoiseSamples s1, s2;
        for (const auto& run : runs) {
            const auto pl = planes(run);
            for (int64_t i = 0; i < run.triples; ++i) stack::AddTriple(pl[i], pl[i + 1], pl[i + 2], pl[i + 3], s1);
        }
        const stack::NoiseModel m1 = stack::FitNoise(s1, sites);
        for (const auto& run : runs) {
            const auto pl = planes(run);
            for (int64_t i = 0; i < run.triples; ++i) stack::AddTriple(pl[i], pl[i + 1], pl[i + 2], pl[i + 3], s2, &m1);
        }
        const stack::NoiseModel m2 = stack::FitNoise(s1, s2, sites);
        std::printf("%s: %zu tiles\n", inj ? "injected" : "plain", s2.tiles.size());
        const double edges[] = {0, 0.015, 0.025, 0.04, 0.06, 0.09, 0.13, 0.18, 0.25, 0.35, 0.5, 0.7, 1.0};
        for (int b = 0; b + 1 < 13; ++b) {
            std::vector<double> r;
            for (const auto& t : s2.tiles)
                if (t.level >= edges[b] && t.level < edges[b + 1]) r.push_back(t.ratio);
            std::sort(r.begin(), r.end());
            const double x = 0.5 * (edges[b] + edges[b + 1]);
            std::printf("  %.3f-%.3f: tiles %5zu median ratio %.3f | pass1 %.4g final %.4g\n", edges[b], edges[b + 1], r.size(),
                        r.empty() ? 0.0 : r[r.size() / 2], m1.VarianceAtMean(x), m2.VarianceAtMean(x));
        }
    }
}
