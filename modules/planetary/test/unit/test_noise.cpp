// Ruling P26: the noise every detail measure subtracts and propagates is the capture's own, level by
// level -- a target's shot noise, a black-clipped sky, the dark limb of a lunar surface -- and qualitySigma
// is the scatter of quality it claims to be. Through the engine's own entry points (MeasureAll, RunStack).
#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cmath>
#include <random>
#include <string>
#include <vector>

#include "Engine.h"
#include "Measure.h"
#include "StackSynth.h"

using namespace planetary;

namespace {

// `frames` frames of one still image, each with its own noise of variance read^2 + level / gain (gain 0:
// read noise only), as a 16-bit capture.
testing::Capture StillCapture(const std::vector<float>& clean, int w, int h, double read, double gain, int frames, unsigned seed) {
    testing::Capture cap = testing::NewCapture(w, h);
    std::mt19937 rng(seed);
    std::normal_distribution<double> n01(0, 1);
    for (int k = 0; k < frames; ++k) {
        std::vector<float> f = clean;
        for (float& v : f) v = static_cast<float>(v + std::sqrt(read * read + (gain > 0 ? std::max(0.0f, v) / gain : 0.0)) * n01(rng));
        cap.Add(f);
    }
    return cap;
}

struct Scatter {
    double meanQ = 0, empirical = 0, rmsSigma = 0, medianSigma = 0, medianRatio = 0;
    size_t n = 0;
};

// Quality's spread over the frames that show detail (quality > 0) against what qualitySigma says.
Scatter Measure(const std::vector<stack::FrameStats>& stats) {
    std::vector<double> q, sg, r;
    for (const auto& s : stats)
        if (s.targetFound && s.quality > 0) {
            q.push_back(s.quality);
            sg.push_back(s.qualitySigma);
            r.push_back(s.qualitySigma > 0 ? s.quality / s.qualitySigma : 1e9);
        }
    Scatter out;
    out.n = q.size();
    if (q.size() < 2) return out;
    double m = 0, m2 = 0, s2 = 0;
    for (size_t i = 0; i < q.size(); ++i) {
        m += q[i];
        m2 += q[i] * q[i];
        s2 += sg[i] * sg[i];
    }
    const double n = static_cast<double>(q.size());
    out.meanQ = m / n;
    out.empirical = std::sqrt(std::max(0.0, (m2 - n * out.meanQ * out.meanQ) / (n - 1)));
    out.rmsSigma = std::sqrt(s2 / n);
    std::sort(sg.begin(), sg.end());
    std::sort(r.begin(), r.end());
    out.medianSigma = sg[sg.size() / 2];
    out.medianRatio = r[r.size() / 2];
    return out;
}

}  // namespace

TEST_CASE("quality and qualitySigma are unbiased with a target's own shot noise (Ruling P26)", "[noise]") {
    // A textured disc (MakeDisc, R 40, over a sky of 0.05), still, blurred 2 / 4 px, read noise 0.005 plus
    // shot noise of variance level / gain: at gains 300 / 1000 / 3000 the disc (~0.55) carries 9 / 5 / 3x
    // the sky's noise. With the sky's level taken as the target's, the quality kept the target's shot noise
    // as "detail" and qualitySigma missed it: mean quality 1.98 / 2.82 / 1.37 / 1.77 / 1.13 / 1.31 times
    // the noise-free one, quality's scatter 2.10 / 2.03 / 1.84 / 1.78 / 1.49 / 1.42 times qualitySigma
    // (measured 2026-10-22 before Ruling P26, gain and blur in that order).
    const int w = 160, h = 160;
    testing::Scene s = testing::MakeDisc(3, 80.3, 79.6, 40);
    s.sky = 0.05;
    for (const double blur : {2.0, 4.0}) {
        std::vector<float> clean(static_cast<size_t>(w) * h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) clean[static_cast<size_t>(y) * w + x] = static_cast<float>(s.Eval(x, y, blur));
        const testing::Capture still = StillCapture(clean, w, h, 0, 0, 3, 1);
        const double truth = stack::MeasureAll(still.Factory(), still.info, 4, {})[0].quality;
        for (const double gain : {300.0, 1000.0, 3000.0}) {
            const testing::Capture cap = StillCapture(clean, w, h, 0.005, gain, 300, 11);
            const Scatter sc = Measure(stack::MeasureAll(cap.Factory(), cap.info, 8, {}));
            INFO("blur " << blur << ", gain " << gain << ": quality " << sc.meanQ << " against " << truth << " noise-free ("
                         << sc.meanQ / truth << "); scatter " << sc.empirical << " against qualitySigma rms " << sc.rmsSigma << " ("
                         << sc.empirical / sc.rmsSigma << "), median q / qualitySigma " << sc.medianRatio);
            REQUIRE(sc.n == 300);
            CHECK(sc.meanQ / truth == Catch::Approx(1).margin(0.03));
            CHECK(sc.empirical / sc.rmsSigma == Catch::Approx(1).margin(0.1));
        }
    }
}

TEST_CASE("qualitySigma tracks quality's scatter from low to high SNR (Ruling P26)", "[noise]") {
    // A smooth blob (Gaussian, sigma 16 px, contrast A over 0.1) at white noise 0.01, 600 frames: median
    // q / qualitySigma ~2.9 / 4.1 / 11.2 / 41 at A 0.1 / 0.15 / 0.3 / 0.6. Before Ruling P26 qualitySigma was
    // taken at each frame's own, noisy quality, whose low draws it blew up (1 / quality), and the sky's
    // noise estimate's own error (a common part over the frame) was added as if it scattered them.
    const int w = 160, h = 160;
    for (const double A : {0.1, 0.15, 0.3, 0.6}) {
        std::vector<float> clean(static_cast<size_t>(w) * h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const double dx = x - 80.3, dy = y - 79.7;
                clean[static_cast<size_t>(y) * w + x] = static_cast<float>(0.1 + A * std::exp(-(dx * dx + dy * dy) / 512.0));
            }
        const testing::Capture cap = StillCapture(clean, w, h, 0.01, 0, 600, 91);
        const Scatter sc = Measure(stack::MeasureAll(cap.Factory(), cap.info, 8, {}));
        INFO("contrast " << A << ": " << sc.n << " frames with detail, median q / qualitySigma " << sc.medianRatio << "; scatter "
                         << sc.empirical << " against qualitySigma rms " << sc.rmsSigma << " / median " << sc.medianSigma);
        REQUIRE(sc.n >= 400);
        CHECK(sc.empirical / sc.rmsSigma == Catch::Approx(1).margin(0.1));
        CHECK(sc.empirical / sc.medianSigma == Catch::Approx(1).margin(0.1));
    }
}

TEST_CASE("a dark lunar limb keeps its frames: no region keeps none (Ruling P26)", "[noise][engine]") {
    // A frame-filling surface (MakeSurface) at full brightness on the left, darkened to a tenth of it over
    // x 120-200 (a terminator and dark limb), black sky past x 236; read noise 0.003 plus shot noise of
    // variance level / 2000, so the bright surface's noise sd is 0.015 and the dark limb's 0.0056; seeing
    // per frame. Every region holds real detail at its own noise (median q / qualitySigma 4.7 and up,
    // stack-region-probe). With one noise level per frame, read over all the regions (mostly bright
    // surface), the dark regions' quality was zeroed: 29 of 142 regions kept no frame (6 of them "no detail
    // above the measurement noise") -- the Moon 193142's "sharpest 0-2102". The capture's model here reads
    // 8.0e-6 + 5.04e-4 t against the true 9e-6 + 5e-4 t.
    const int w = 256, h = 192, frames = 80;
    const testing::Scene s = testing::MakeSurface(31, w, h);
    auto profile = [](double x) { return x >= 236 ? 0.0 : x < 120 ? 1.0 : 1.0 - 0.9 * std::min(1.0, (x - 120) / 80.0); };
    testing::Capture cap = testing::NewCapture(w, h);
    std::mt19937 rng(5), noise(9);
    std::normal_distribution<double> n01(0, 1);
    for (int k = 0; k < frames; ++k) {
        testing::FrameSpec f = testing::Seeing(rng, w, h, 1.0, 64, 1.0);
        f.noise = 0;
        std::vector<float> v = testing::RenderFrame(s, w, h, f, 100u + static_cast<unsigned>(k));
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const double t = v[static_cast<size_t>(y) * w + x] * profile(x - f.gx);
                v[static_cast<size_t>(y) * w + x] = static_cast<float>(t + std::sqrt(0.003 * 0.003 + t / 2000) * n01(noise));
            }
        cap.Add(v);
    }
    const stack::StackOutcome o = stack::RunStack(cap.Factory(), 8, {});
    INFO(o.summary.selection << " | " << o.summary.alignment);
    REQUIRE(o.diagnosis.kind == stack::DiagnosisKind::None);
    REQUIRE(o.frames.regions > 50);
    int none = 0;
    for (const stack::RegionDetail& d : o.frames.regionDetail) none += d.kept == 0;
    CHECK(none == 0);
    CHECK(o.summary.alignment.find("no detail above the measurement noise") == std::string::npos);
}

namespace {
// Frames of a still scene: `clean` with noise of variance read^2 + level / gain (gain 0: read only), each
// clamped to [0, 1] as a capture is, optionally quantised to 8 bits.
std::vector<ser::FloatImage> NoisyFrames(const std::vector<float>& clean, int w, int h, double read, double gain, int frames,
                                         unsigned seed, bool eightBit = false) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> n01(0, 1);
    std::vector<ser::FloatImage> out;
    for (int k = 0; k < frames; ++k) {
        ser::FloatImage im{w, h, 1, clean};
        for (float& v : im.data) {
            double x = v + std::sqrt(read * read + (gain > 0 ? std::max(0.0f, v) / gain : 0.0)) * n01(rng);
            x = std::clamp(x, 0.0, 1.0);
            if (eightBit) x = std::round(x * 255) / 255;
            v = static_cast<float>(x);
        }
        out.push_back(std::move(im));
    }
    return out;
}

std::vector<float> DiscScene(int w, int h, double sky, double blur) {
    testing::Scene s = testing::MakeDisc(3, 0.5 * w + 0.3, 0.5 * h - 0.4, 0.3 * std::min(w, h));
    s.sky = sky;
    std::vector<float> v(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) v[static_cast<size_t>(y) * w + x] = static_cast<float>(s.Eval(x, y, blur));
    return v;
}
}  // namespace

TEST_CASE("the capture's noise model: the line, the clipped sky, texture and a static pattern (Ruling P26)", "[noise][model]") {
    const int w = 192, h = 160;
    SECTION("white noise over a textured disc: the line is flat at the truth") {
        const stack::NoiseModel m = stack::FitNoiseModel(NoisyFrames(DiscScene(w, h, 0.05, 0.8), w, h, 0.01, 0, 40, 3));
        INFO("a " << m.a << ", b " << m.b);
        CHECK(m.a == Catch::Approx(1e-4).epsilon(0.015));
        CHECK(m.b * 0.6 < 0.02 * m.a);  // at the disc's level the slope adds under 2%
        CHECK(m.VarianceAtMean(0.3) == Catch::Approx(1e-4).epsilon(0.03));
        CHECK_FALSE(m.ownFrame);
    }
    SECTION("read plus shot noise: the photon-transfer line") {
        for (const double gain : {500.0, 3000.0}) {
            const stack::NoiseModel m = stack::FitNoiseModel(NoisyFrames(DiscScene(w, h, 0.05, 0.8), w, h, 0.005, gain, 40, 4));
            INFO("gain " << gain << ": a " << m.a << " (truth 2.5e-5), b " << m.b << " (truth " << 1 / gain << ")");
            for (const double t : {0.05, 0.3, 0.55})
                CHECK(m.VarianceAtMean(t) == Catch::Approx(2.5e-5 + t / gain).epsilon(0.05));
        }
    }
    SECTION("a sky 2 noise sd above black: its own, clipped variance, not the line's") {
        // The 16-bit capture clips 2.3% of a sky of 0.02 at noise 0.01: its variance is 0.960 of the
        // target's (clamp(N(0.02, 0.01), 0, 1): 0.9601e-4) and its mean rises to 0.0201.
        const stack::NoiseModel m = stack::FitNoiseModel(NoisyFrames(DiscScene(w, h, 0.02, 0.8), w, h, 0.01, 0, 40, 5));
        INFO("a " << m.a << ", b " << m.b << ", at the sky " << m.VarianceAtMean(0.0201));
        CHECK(m.a == Catch::Approx(1e-4).epsilon(0.02));
        CHECK(m.AnyClipped());
        CHECK(m.VarianceAtMean(0.0201) == Catch::Approx(0.9601e-4).epsilon(0.015));
    }
    SECTION("a static pattern (a colour mosaic read as mono) cancels in time") {
        // Sites R 1 / G 0.5 / B 0.35: a pattern at Nyquist in every frame, which a spatial reading takes for
        // noise (Ruling P25 read such frames on 2 x 2 bins).
        std::vector<float> clean = DiscScene(w, h, 0.05, 0.8);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                clean[static_cast<size_t>(y) * w + x] *= y % 2 == 0 ? (x % 2 == 0 ? 1.0f : 0.5f) : (x % 2 == 0 ? 0.5f : 0.35f);
        const stack::NoiseModel m = stack::FitNoiseModel(NoisyFrames(clean, w, h, 0.01, 0, 40, 6));
        INFO("a " << m.a << ", b " << m.b);
        CHECK(m.VarianceAtMean(0.15) == Catch::Approx(1e-4).epsilon(0.03));
    }
    SECTION("8-bit data: quantisation included") {
        for (const double dn : {1.0, 3.0}) {
            const std::vector<float> flat(static_cast<size_t>(w) * h, 40.0f / 255);
            const std::vector<ser::FloatImage> frames = NoisyFrames(flat, w, h, dn / 255, 0, 40, 7, true);
            double m1 = 0, m2 = 0, n = 0;
            for (const auto& f : frames)
                for (const float v : f.data) {
                    m1 += v;
                    m2 += static_cast<double>(v) * v;
                    n += 1;
                }
            const double truth = m2 / n - (m1 / n) * (m1 / n);
            const stack::NoiseModel m = stack::FitNoiseModel(frames);
            INFO(dn << " DN: model " << m.VarianceAtMean(40.0 / 255) << " against the frames' own variance " << truth);
            CHECK(m.VarianceAtMean(40.0 / 255) == Catch::Approx(truth).epsilon(0.02));
        }
    }
    SECTION("no noise, no frames, one image") {
        const std::vector<float> clean = DiscScene(w, h, 0.05, 0.8);
        CHECK(stack::FitNoiseModel(NoisyFrames(clean, w, h, 0, 0, 6, 8)).VarianceAtMean(0.3) < 1e-9);
        CHECK(stack::FitNoiseModel(std::vector<ser::FloatImage>{}).Zero());
        stack::NoiseEstimate sky;
        sky.sigma = 0.01;
        sky.varSigma2 = 1e-10;
        const stack::NoiseModel one = stack::NoiseModel::FromSkyNoise(sky);
        CHECK(one.ownFrame);
        CHECK(one.Lookup(0.7) == Catch::Approx(1e-4));
        CHECK(one.LineVariance(0.7) == Catch::Approx(1e-10));
    }
}
