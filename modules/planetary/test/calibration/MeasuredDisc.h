// Test-only (test/calibration/): MeasuredDiscCapture, test_select's measured-disc capture (60 frames) as a
// Capture, with an optional shot-noise gain.
#pragma once
#include <cmath>
#include <random>
#include "StackSynth.h"
using namespace planetary;

inline testing::Capture MeasuredDiscCapture(double radius, double blur, bool convolve, double noise, unsigned seed,
                                            double sky = -1, double shotGain = 0) {
    const int half = static_cast<int>(radius + 3 * blur + 30), w = 2 * half, h = 2 * half;
    testing::Scene s = testing::MakeDisc(seed, half + 0.3, half - 0.2, radius);
    if (sky >= 0) s.sky = sky;
    testing::Capture cap = testing::NewCapture(w, h);
    std::mt19937 rng(seed + 1);
    std::normal_distribution<double> n01(0, 1);
    std::vector<testing::FrameSpec> specs;
    for (int k = 0; k < 60; ++k) {
        testing::FrameSpec f;
        f.gx = 1.5 * n01(rng);
        f.gy = 1.5 * n01(rng);
        f.hasLocal = true;
        f.dx = testing::RandomField(rng, w, h, 28, [] { return 0.0; });
        f.dy = f.dx;
        const double b = (convolve ? 0.6 : blur) * std::exp(0.04 * n01(rng));
        f.sigma = testing::RandomField(rng, w, h, 28, [b] { return b; });
        f.noise = convolve ? 0.0 : noise;
        specs.push_back(f);
    }
    for (int k = 0; k < 60; ++k) {
        std::vector<float> v = testing::RenderFrame(s, w, h, specs[static_cast<size_t>(k)], 500u + k);
        if (convolve) {
            const double sg = blur;
            const int r = static_cast<int>(std::ceil(3 * sg));
            std::vector<double> kern;
            double sum = 0;
            for (int i = -r; i <= r; ++i) sum += kern.emplace_back(std::exp(-0.5 * i * i / (sg * sg)));
            for (double& kv : kern) kv /= sum;
            auto pass = [&](std::vector<float>& img, bool alongX) {
                std::vector<float> out(img.size());
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x) {
                        double acc = 0;
                        for (int i = -r; i <= r; ++i) {
                            const int xx = alongX ? std::clamp(x + i, 0, w - 1) : x;
                            const int yy = alongX ? y : std::clamp(y + i, 0, h - 1);
                            acc += kern[static_cast<size_t>(i + r)] * img[static_cast<size_t>(yy) * w + xx];
                        }
                        out[static_cast<size_t>(y) * w + x] = static_cast<float>(acc);
                    }
                img.swap(out);
            };
            pass(v, true);
            pass(v, false);
            for (float& p : v) p = static_cast<float>(p + std::sqrt(noise * noise + (shotGain > 0 ? std::max(0.0f, p) / shotGain : 0.0)) * n01(rng));
        }
        cap.Add(v);
    }
    return cap;
}
