// Ruling P26: the noise every detail measure subtracts and propagates is the capture's own, level by
// level -- a target's shot noise, a black-clipped sky, the dark limb of a lunar surface -- and qualitySigma
// is the scatter of quality it claims to be. Through the engine's own entry points (MeasureAll, RunStack).
#include <catch_amalgamated.hpp>

#include <algorithm>
#include <memory>
#include <optional>
#include <cmath>
#include <random>
#include <string>
#include <vector>

#include "Engine.h"
#include "Measure.h"
#include "Noise.h"
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
        CHECK_FALSE(stack::FitNoiseModel(std::vector<ser::FloatImage>{}).Measured());  // unmeasured, not a measured 0
        CHECK(stack::FitNoiseModel(NoisyFrames(clean, w, h, 0, 0, 6, 8)).Measured());
        stack::NoiseEstimate sky;
        sky.sigma = 0.01;
        sky.varSigma2 = 1e-10;
        const stack::NoiseModel one = stack::NoiseModel::FromSkyNoise(sky);
        CHECK(one.ownFrame);
        CHECK(one.Lookup(0.7) == Catch::Approx(1e-4));
        CHECK(one.LineVariance(0.7) == Catch::Approx(1e-10));
    }
}

TEST_CASE("a capture too short for the model measures each frame on its own sky (Ruling P26 fix round 1, I1)", "[noise]") {
    // 1-3 frames give no frame triple: the capture's model is unmeasured, and each frame takes its own sky
    // noise (FromSkyNoise of its PixelNoise), as one image does. With the zero model in its place quality
    // read 1.164x the noise-free one and qualitySigma 0 (941ab4e7, review round 1).
    const int w = 160, h = 160;
    testing::Scene s = testing::MakeDisc(3, 80.3, 79.6, 40);
    s.sky = 0.05;
    std::vector<float> clean(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) clean[static_cast<size_t>(y) * w + x] = static_cast<float>(s.Eval(x, y, 2.0));
    const testing::Capture still = StillCapture(clean, w, h, 0, 0, 3, 1);
    const double truth = stack::MeasureAll(still.Factory(), still.info, 4, {})[0].quality;
    for (const int frames : {1, 2, 3}) {
        const testing::Capture cap = StillCapture(clean, w, h, 0.01, 0, frames, 21);
        CHECK_FALSE(stack::FitNoiseModel(cap.Factory(), cap.info, 2, {}).Measured());
        for (const stack::FrameStats& st : stack::MeasureAll(cap.Factory(), cap.info, 2, {})) {
            INFO(frames << " frames, frame " << st.index << ": quality " << st.quality << " against " << truth << " noise-free, sigma "
                        << st.qualitySigma);
            // The one-image reading's own accuracy: 0.97 of the noise-free here (review round 1: 0.971-0.973).
            CHECK(st.quality / truth == Catch::Approx(1).margin(0.04));
            CHECK(st.qualitySigma > 0);
            CHECK(st.quality2Sigma > 0);
        }
    }
}

TEST_CASE("the noise runs: none without a response, capped, each triple once (Ruling P26 fix round 1)", "[noise]") {
    auto info = [](int w, int h, int64_t frames) {
        ser::FileInfo i;
        i.header.width = w;
        i.header.height = h;
        i.header.depth = 16;
        i.normDepth = 16;
        i.colorId = ser::ColorId::Mono;
        i.frameCount = frames;
        return i;
    };
    // Frames under 5 x 5 hold no response: no run, the model unmeasured -- not the whole capture read for
    // nothing (941ab4e7 planned min(frames - 3, 2^25) triples there).
    CHECK(stack::NoiseRuns(info(4, 4, 100000)).empty());
    CHECK(stack::NoiseRuns(info(800, 3, 100)).empty());
    // A small frame of a long capture: at most kNoiseMaxTriples triples per pass (941ab4e7: 49 997 of a
    // 24 x 24 capture of 50 000 frames, each pass).
    int64_t triples = 0;
    for (const stack::NoiseRun& r : stack::NoiseRuns(info(24, 24, 50000))) triples += r.triples;
    CHECK(triples == stack::kNoiseMaxTriples);
    // Every triple read, each once: runs back to back (941ab4e7 read triple 24 twice and 72 not at all of
    // 100 frames).
    for (const int64_t frames : {5, 37, 100, 1000}) {
        const std::vector<stack::NoiseRun> plan = stack::NoiseRuns(info(64, 48, frames));
        std::vector<int> seen(static_cast<size_t>(frames - 3), 0);
        for (const stack::NoiseRun& r : plan)
            for (int64_t t = r.first; t < r.first + r.triples; ++t) ++seen[static_cast<size_t>(t)];
        INFO(frames << " frames");
        CHECK(std::all_of(seen.begin(), seen.end(), [](int c) { return c == 1; }));
    }
    // Spread over a capture when not all are read: no run overlaps another, none past the end.
    const std::vector<stack::NoiseRun> spread = stack::NoiseRuns(info(1920, 1080, 20000));
    for (size_t i = 0; i < spread.size(); ++i) {
        CHECK(spread[i].first + spread[i].triples + 3 <= 20000);
        if (i > 0) CHECK(spread[i].first >= spread[i - 1].first + spread[i - 1].triples);
    }
}

TEST_CASE("the noise phase: cancel and a reader's error stop it, every worker joined (Ruling P26 fix round 1)", "[noise]") {
    const int w = 96, h = 80;
    const std::vector<float> clean(static_cast<size_t>(w) * h, 0.3f);
    const testing::Capture cap = StillCapture(clean, w, h, 0.01, 0, 200, 31);
    // Cancelled once the first run is in: StackCancelled, whatever the thread count.
    for (const int threads : {1, 4}) {
        stack::Progress p;
        int64_t seen = 0;
        p.report = [&](const std::string&, int64_t done, int64_t) { seen = std::max(seen, done); };
        p.cancelled = [&] { return seen >= 1; };
        CHECK_THROWS_AS(stack::FitNoiseModel(cap.Factory(), cap.info, threads, p), stack::StackCancelled);
    }
    // A reader that fails on one frame: its error, rethrown after every worker is joined.
    class Failing : public ser::FrameSource {
    public:
        explicit Failing(std::unique_ptr<ser::FrameSource> s) : s_(std::move(s)) {}
        const ser::FileInfo& Info() const override { return s_->Info(); }
        ser::Frame ReadFrame(int64_t i) const override {
            if (i == 77) throw ser::SerError("frame 77 is unreadable");
            return s_->ReadFrame(i);
        }
        std::optional<int64_t> FrameTimestamp(int64_t i) const override { return s_->FrameTimestamp(i); }

    private:
        std::unique_ptr<ser::FrameSource> s_;
    };
    const stack::SourceFactory base = cap.Factory();
    const stack::SourceFactory failing = [&]() -> std::unique_ptr<ser::FrameSource> { return std::make_unique<Failing>(base()); };
    for (const int threads : {1, 4})
        CHECK_THROWS_WITH(stack::FitNoiseModel(failing, cap.info, threads, {}), Catch::Matchers::ContainsSubstring("frame 77"));
}

TEST_CASE("the capture's noise model rises by exactly an added noise, across levels no clean tile takes (Ruling P26 fix round 1, I5)",
          "[noise][model]") {
    // The samples of a lunar capture like the 2023-02-02 Moon's, built directly: pass 1 reads noise
    // N(t) = 1e-6 + 1e-4 t at every level, plus as much seeing-moved texture from 0.03 up; pass 2's one-level
    // tiles take the sky (0.019-0.021, ratio 1) and the surface (0.13-0.6, ratio 0.5: no texture in them),
    // nothing between -- the levels a limb's pixels take. Adding noise v0 everywhere must raise the model by
    // v0 at every level, measured or interpolated: texture does not grow with the noise.
    auto noise = [](double t) { return 1e-6 + 1e-4 * t; };
    auto texture = [&](double t) { return t >= 0.03 ? noise(t) : 0.0; };
    auto samples = [&](double v0, stack::NoiseSamples& s1, stack::NoiseSamples& s2) {
        for (int k = 0; k < stack::kFineBins; ++k) {
            const double t = (k + 0.5) / stack::kFineBins;
            if (!((t > 0.018 && t < 0.022) || (t > 0.03 && t < 0.6))) continue;
            const double v = noise(t) + texture(t) + v0, n = 4000;
            s1.n[static_cast<size_t>(k)] = n;
            s1.level[static_cast<size_t>(k)] = n * t;
            s1.r2[static_cast<size_t>(k)] = n * v;
            s1.r4[static_cast<size_t>(k)] = n * 3 * v * v;
        }
        s1.overlap = 1;
        s1.frames = 100;
        std::mt19937 rng(3);
        std::normal_distribution<double> n01(0, 1);
        std::uniform_real_distribution<double> uni(0, 1);
        auto tile = [&](double t) {
            const double clean = (noise(t) + v0) / (noise(t) + texture(t) + v0);
            const double tau = std::sqrt(2.0 / 256);
            s2.tiles.push_back({static_cast<float>(t), static_cast<float>(clean * (1 + tau * n01(rng))), 3.0f, 256.0f});
        };
        for (int i = 0; i < 1000; ++i) tile(0.019 + 0.002 * uni(rng));  // not a multiple of the 64-tile bin
        for (int i = 0; i < 3000; ++i) tile(0.13 + 0.47 * uni(rng));
        s2.tileOverlap = 1;
        s2.frames = 100;
    };
    stack::NoiseSamples a1, a2, b1, b2;
    const double v0 = 2e-5;
    samples(0, a1, a2);
    samples(v0, b1, b2);
    const stack::NoiseModel a = stack::FitNoise(a1, a2, 1), b = stack::FitNoise(b1, b2, 1);
    for (const double t : {0.04, 0.06, 0.08, 0.1, 0.12, 0.2, 0.4}) {
        INFO("level " << t << ": " << a.VarianceAtMean(t) << " -> " << b.VarianceAtMean(t) << ", rise / v0 "
                      << (b.VarianceAtMean(t) - a.VarianceAtMean(t)) / v0);
        CHECK((b.VarianceAtMean(t) - a.VarianceAtMean(t)) / v0 == Catch::Approx(1).margin(0.05));
    }
    // And the measured levels read the noise alone.
    CHECK(a.VarianceAtMean(0.02) == Catch::Approx(noise(0.02)).epsilon(0.05));
    CHECK(a.VarianceAtMean(0.4) == Catch::Approx(noise(0.4)).epsilon(0.05));
}
