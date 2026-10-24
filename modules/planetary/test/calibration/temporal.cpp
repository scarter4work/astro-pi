// Test-only calibration probe (Ruling P26; test/calibration/, built by test/CMakeLists.txt): never shipped, not a
// ctest case; it reads a real capture (the NAS is only read). Built as calib-temporal.
//
//   calib-temporal <video>
//
// Per level window: noise variance read (A) spatially, [1 -2 1]^2 (gain 36), (B) temporally, [1 -2 1] over
// three consecutive frames (gain 6), (C) both, [1 -2 1]_t x [1 -2 1]_x x [1 -2 1]_y (gain 216); plain mean
// squares (no trimming). Level: the middle frame's 16-pixel ring around the 3 x 3 support.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include "FrameSource.h"
#include "Measure.h"
using namespace planetary;
int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: calib-temporal <video>\n");
        return 1;
    }
    const std::string path = argv[1];
    const int nbins = 20;
    auto src = ser::OpenVideo(path, ser::BayerChoice::Auto);
    const ser::FileInfo info = src->Info();
    std::vector<double> sA(nbins), sB(nbins), sC(nbins), cnt(nbins), lev(nbins);
    for (int k = 0; k < 32; ++k) {
        const int64_t f = 1 + k * (info.frameCount - 3) / 32;
        ser::FloatImage F[3];
        for (int j = 0; j < 3; ++j) F[j] = stack::MeasurementLuma(src->ReadFrame(f - 1 + j), info);
        const int w = F[1].width, h = F[1].height;
        auto at = [&](int j, int x, int y) { return double(F[j].data[size_t(y) * w + x]); };
        const int c[3] = {1, -2, 1};
        for (int y = 2; y + 2 < h; ++y)
            for (int x = 2; x + 2 < w; ++x) {
                double ring = 0;
                for (int d = -2; d <= 2; ++d) ring += at(1, x + d, y - 2) + at(1, x + d, y + 2);
                for (int d = -1; d <= 1; ++d) ring += at(1, x - 2, y + d) + at(1, x + 2, y + d);
                ring /= 16;
                const int b = std::clamp(int(ring * nbins), 0, nbins - 1);
                double A = 0, C = 0;
                for (int j = -1; j <= 1; ++j)
                    for (int i = -1; i <= 1; ++i) {
                        const double ws = c[i + 1] * c[j + 1];
                        A += ws * at(1, x + i, y + j);
                        double t = 0;
                        for (int q = 0; q < 3; ++q) t += c[q] * at(q, x + i, y + j);
                        C += ws * t;
                    }
                const double B = at(0, x, y) - 2 * at(1, x, y) + at(2, x, y);
                sA[b] += A * A / 36;
                sB[b] += B * B / 6;
                sC[b] += C * C / 216;
                cnt[b] += 1;
                lev[b] += ring;
            }
    }
    std::printf("%s\n", path.c_str());
    for (int b = 0; b < nbins; ++b)
        if (cnt[b] > 2000)
            std::printf("  level %.3f n %8.0f  spatial %.4g  temporal %.4g  both %.4g   spatial/both %.3f temporal/both %.3f\n", lev[b] / cnt[b],
                        cnt[b], sA[b] / cnt[b], sB[b] / cnt[b], sC[b] / cnt[b], sA[b] / sC[b], sB[b] / sC[b]);
}
