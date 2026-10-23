// Test-only calibration probe (Ruling P26; test/calibration/, built by test/CMakeLists.txt): never shipped, not a
// ctest case (synthetic frames, test/unit/StackSynth.h). Built as calib-qcal2.
//
//   calib-qcal2 [runs (2000)]
//
// qualitySigma calibration (Ruling P26): RegionQuality on a textured 64 x 64 window (sinusoids, contrast A)
// and MeasureFrame on a faint disc (contrast A) over many white-noise realisations, with the true noise as
// a capture model (not ownFrame). Empirical sd of q (q > 0) against the mean, rms and median of qualitySigma.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "Measure.h"
#include "StackSynth.h"

using namespace planetary;

struct Stat {
    double r50, emp, mean, rms, med;
    size_t n;
};
Stat Summ(std::vector<double> q, std::vector<double> sg) {
    Stat s{};
    std::vector<double> r;
    double m = 0, m2 = 0, ms = 0, ms2 = 0;
    for (size_t i = 0; i < q.size(); ++i) {
        m += q[i];
        m2 += q[i] * q[i];
        ms += sg[i];
        ms2 += sg[i] * sg[i];
        r.push_back(q[i] / sg[i]);
    }
    const double N = static_cast<double>(q.size());
    m /= N;
    s.emp = std::sqrt(std::max(0.0, (m2 - N * m * m) / (N - 1)));
    s.mean = ms / N;
    s.rms = std::sqrt(ms2 / N);
    std::nth_element(sg.begin(), sg.begin() + sg.size() / 2, sg.end());
    std::nth_element(r.begin(), r.begin() + r.size() / 2, r.end());
    s.med = sg[sg.size() / 2];
    s.r50 = r[r.size() / 2];
    s.n = q.size();
    return s;
}

int main(int argc, char** argv) {
    const int runs = argc > 1 ? std::atoi(argv[1]) : 2000;
    const double s = 0.01;
    const stack::NoiseModel model = testing::WhiteNoise(s);
    std::printf("kind    A        n     r50     emp/mean emp/rms emp/med\n");
    for (double A : std::vector<double>{0, 0.0005, 0.0008, 0.001, 0.0012, 0.0015, 0.002, 0.003, 0.004, 0.006, 0.01, 0.02, 0.05}) {
        const int n = 64;
        std::vector<float> clean(n * n);
        std::mt19937 tr(5);
        std::uniform_real_distribution<double> u(0, 1);
        struct W { double fx, fy, ph, a; };
        std::vector<W> ws;
        for (int k = 0; k < 12; ++k) ws.push_back({0.02 + 0.15 * u(tr), 0.02 + 0.15 * u(tr), 6.28 * u(tr), 0.5 + u(tr)});
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                double v = 0.3;
                for (auto& w : ws) v += A * w.a * std::sin(6.2832 * (w.fx * x + w.fy * y) + w.ph);
                clean[y * n + x] = static_cast<float>(v);
            }
        std::vector<double> q, sg;
        std::mt19937 rng(77);
        std::normal_distribution<double> g(0, s);
        for (int k = 0; k < runs; ++k) {
            std::vector<float> im = clean;
            double mean = 0;
            for (float& v : im) {
                v = static_cast<float>(v + g(rng));
                mean += v;
            }
            const auto e = stack::RegionQuality(im.data(), n, 0, 0, n, n, mean / (n * n), model);
            if (e.quality > 0) {
                q.push_back(e.quality);
                sg.push_back(e.sigma);
            }
        }
        const Stat st = Summ(q, sg);
        std::printf("region  %.4f  %5zu  %6.2f  %.3f    %.3f   %.3f\n", A, st.n, st.r50, st.emp / st.mean, st.emp / st.rms, st.emp / st.med);
    }
    // A faint blob (Gaussian, sigma 8 px) in sky, MeasureFrame with the capture model.
    for (double A : {0.04, 0.06, 0.08, 0.1, 0.15, 0.3, 0.6}) {
        const int w = 160, h = 160;
        std::vector<float> clean(w * h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const double dx = x - 80.3, dy = y - 79.7;
                clean[y * w + x] = static_cast<float>(0.1 + A * std::exp(-(dx * dx + dy * dy) / 512.0));
            }
        std::vector<double> q, sg;
        std::mt19937 rng(91);
        std::normal_distribution<double> g(0, s);
        for (int k = 0; k < runs / 2; ++k) {
            ser::FloatImage im{w, h, 1, clean};
            for (float& v : im.data) v = static_cast<float>(v + g(rng));
            const stack::FrameStats f = stack::MeasureFrame(im, k, 1, model);
            if (f.targetFound && f.quality > 0) {
                q.push_back(f.quality);
                sg.push_back(f.qualitySigma);
            }
        }
        if (q.size() < 20) continue;
        const Stat st = Summ(q, sg);
        std::printf("frame   %.4f  %5zu  %6.2f  %.3f    %.3f   %.3f\n", A, st.n, st.r50, st.emp / st.mean, st.emp / st.rms, st.emp / st.med);
    }
}
