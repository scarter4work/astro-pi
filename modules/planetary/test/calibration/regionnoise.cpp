// Test-only calibration probe (Ruling P26; test/calibration/, built by test/CMakeLists.txt): never shipped, not a
// ctest case (synthetic frames). Built as calib-regionnoise.
//
//   calib-regionnoise [runs scale (3e6)]
//
// RegionNoise (Regions.h) on white noise 0.01 over a flat 0.3, square windows of 32-512 px, the protocol of test
// "RegionNoise is unbiased on pixels and on bins ...": the mean s^2 over the truth on the pixels, on the 2 x 2
// bins, and of the smaller of the two per run (what the region stage once took per frame). Exit 0.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "Measure.h"
#include "Regions.h"

using namespace planetary;

int main(int argc, char** argv) {
    const double scale = argc > 1 ? std::atof(argv[1]) : 3e6;
    for (const int n : {32, 48, 64, 128, 256, 512}) {
        const int runs = std::max(150, static_cast<int>(scale / (static_cast<double>(n) * n)));
        double pixels = 0, bins = 0, smaller = 0;
        for (int k = 0; k < runs; ++k) {
            std::mt19937 rng(4000u + 31u * static_cast<unsigned>(k) + static_cast<unsigned>(n));
            std::normal_distribution<double> g(0, 0.01);
            ser::FloatImage im{n, n, 1, std::vector<float>(static_cast<size_t>(n) * n)};
            for (float& v : im.data) v = static_cast<float>(0.3 + g(rng));
            const stack::NoiseEstimate p = stack::RegionNoise(im, 0, 0, n, n, false), b = stack::RegionNoise(im, 0, 0, n, n, true);
            const double vp = p.sigma * p.sigma / 1e-4, vb = b.sigma * b.sigma / 1e-4;
            pixels += vp;
            bins += vb;
            smaller += std::min(vp, vb);
        }
        std::printf("%4d^2 (%d runs): pixels %.3f, bins %.3f, the smaller %.3f of the truth\n", n, runs, pixels / runs, bins / runs,
                    smaller / runs);
    }
    return 0;
}
