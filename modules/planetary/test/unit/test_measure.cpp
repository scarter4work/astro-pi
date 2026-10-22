#include <catch_amalgamated.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "AviWriter.h"
#include "Demosaic.h"
#include "Measure.h"
#include "StackTypes.h"
#include "TestUtil.h"

using namespace planetary;
using ser::FloatImage;
using stack::FrameStats;
using stack::MeasureAll;
using stack::MeasureFrame;

namespace {

FloatImage Flat(int w, int h, float v) {
    return FloatImage{w, h, 1, std::vector<float>(static_cast<size_t>(w) * h, v)};
}

float& At(FloatImage& im, int x, int y) { return im.data[static_cast<size_t>(y) * im.width + x]; }

// Paints a disc (anti-aliased by 8x8 supersampling of the coverage) whose inside value is
// inside(x, y); the result is coverage * inside + (1 - coverage) * what was there.
template <class F>
void PaintDisc(FloatImage& im, double cx, double cy, double r, F inside) {
    const int x0 = std::max(0, static_cast<int>(std::floor(cx - r - 1)));
    const int x1 = std::min(im.width - 1, static_cast<int>(std::ceil(cx + r + 1)));
    const int y0 = std::max(0, static_cast<int>(std::floor(cy - r - 1)));
    const int y1 = std::min(im.height - 1, static_cast<int>(std::ceil(cy + r + 1)));
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            int in = 0;
            for (int sy = 0; sy < 8; ++sy)
                for (int sx = 0; sx < 8; ++sx) {
                    const double px = x + (sx + 0.5) / 8 - 0.5, py = y + (sy + 0.5) / 8 - 0.5;
                    if ((px - cx) * (px - cx) + (py - cy) * (py - cy) < r * r) ++in;
                }
            const double cov = in / 64.0;
            At(im, x, y) = static_cast<float>(cov * inside(x, y) + (1 - cov) * At(im, x, y));
        }
}

// A planet-like disc: limb darkening, two belt systems and a few spots.
double Texture(double cx, double cy, double r, int x, int y) {
    const double dx = x - cx, dy = y - cy;
    const double rr = std::min(1.0, std::sqrt(dx * dx + dy * dy) / r);
    double v = 0.55 * (0.6 + 0.4 * std::sqrt(1 - rr * rr));
    v *= 1 + 0.25 * std::sin(2 * M_PI * dy / 9.0) + 0.12 * std::sin(2 * M_PI * (dx + 0.5 * dy) / 5.0);
    const double spots[3][2] = {{-0.4, 0.2}, {0.3, -0.35}, {0.1, 0.5}};
    for (const auto& s : spots) {
        const double sx = dx - s[0] * r, sy = dy - s[1] * r;
        v += 0.2 * std::exp(-(sx * sx + sy * sy) / 4.0);
    }
    return v;
}

FloatImage TexturedPlanet(int w, int h, double cx, double cy, double r, float sky) {
    FloatImage im = Flat(w, h, sky);
    PaintDisc(im, cx, cy, r, [&](int x, int y) { return Texture(cx, cy, r, x, y); });
    return im;
}

FloatImage GaussianBlur(const FloatImage& in, double sigma) {
    if (sigma <= 0) return in;
    const int rad = static_cast<int>(std::ceil(4 * sigma));
    std::vector<double> k(2 * rad + 1);
    double sum = 0;
    for (int i = -rad; i <= rad; ++i) sum += k[i + rad] = std::exp(-0.5 * i * i / (sigma * sigma));
    for (double& v : k) v /= sum;
    FloatImage tmp = in, out = in;
    auto clampi = [](int v, int n) { return std::min(std::max(v, 0), n - 1); };
    for (int y = 0; y < in.height; ++y)
        for (int x = 0; x < in.width; ++x) {
            double a = 0;
            for (int i = -rad; i <= rad; ++i) a += k[i + rad] * in.data[static_cast<size_t>(y) * in.width + clampi(x + i, in.width)];
            tmp.data[static_cast<size_t>(y) * in.width + x] = static_cast<float>(a);
        }
    for (int y = 0; y < in.height; ++y)
        for (int x = 0; x < in.width; ++x) {
            double a = 0;
            for (int i = -rad; i <= rad; ++i) a += k[i + rad] * tmp.data[static_cast<size_t>(clampi(y + i, in.height)) * in.width + x];
            out.data[static_cast<size_t>(y) * in.width + x] = static_cast<float>(a);
        }
    return out;
}

void Scale(FloatImage& im, float k) {
    for (float& v : im.data) v *= k;
}

void AddNoise(FloatImage& im, double sigma, unsigned seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> n(0, sigma);
    for (float& v : im.data) v = static_cast<float>(v + n(rng));
}

void Quantize8(FloatImage& im) {
    for (float& v : im.data) v = static_cast<float>(std::clamp(std::lround(v * 255.0), 0L, 255L)) / 255.0f;
}

bool SameStats(const FrameStats& a, const FrameStats& b) {
    return a.index == b.index && a.targetFound == b.targetFound && a.cx == b.cx && a.cy == b.cy &&
           a.bx0 == b.bx0 && a.by0 == b.by0 && a.bx1 == b.bx1 && a.by1 == b.by1 && a.quality == b.quality &&
           a.qualitySigma == b.qualitySigma && a.brightness == b.brightness && a.clippedFraction == b.clippedFraction;
}

}  // namespace

TEST_CASE("quality falls strictly with blur, whatever the brightness, with and without noise") {
    const int w = 192, h = 176;
    const double cx = 95.3, cy = 88.6, r = 42;
    const FloatImage base = TexturedPlanet(w, h, cx, cy, r, 0.05f);
    const double sigmas[4] = {0, 0.5, 1, 2};
    double clean[3][4] = {};
    const float scales[3] = {0.25f, 0.5f, 1.0f};
    for (int noisy = 0; noisy < 2; ++noisy)
        for (int si = 0; si < 3; ++si) {
            double q[4];
            for (int b = 0; b < 4; ++b) {
                FloatImage im = GaussianBlur(base, sigmas[b]);
                Scale(im, scales[si]);
                if (noisy) AddNoise(im, 0.004, 1000u + 10u * si + b);  // fixed read noise: dimmer = lower SNR
                const FrameStats s = MeasureFrame(im, b, 1);
                INFO("noisy " << noisy << " scale " << scales[si] << " blur " << sigmas[b] << " quality " << s.quality);
                REQUIRE(s.targetFound);
                q[b] = s.quality;
                if (!noisy) clean[si][b] = s.quality;
                else CHECK(std::fabs(q[b] - clean[si][b]) <= 0.10 * clean[si][b]);  // noise removed, not "detail"
            }
            INFO("noisy " << noisy << " scale " << scales[si] << ": " << q[0] << " > " << q[1] << " > " << q[2] << " > " << q[3]);
            CHECK(q[0] > q[1]);
            CHECK(q[1] > q[2]);
            CHECK(q[2] > q[3]);
        }
    // Brightness alone does not move it (noiseless: exact up to float rounding).
    for (int b = 0; b < 4; ++b) {
        INFO("blur " << sigmas[b]);
        CHECK(clean[0][b] == Catch::Approx(clean[2][b]).epsilon(1e-3));
        CHECK(clean[1][b] == Catch::Approx(clean[2][b]).epsilon(1e-3));
    }
}

TEST_CASE("qualitySigma matches the scatter of quality over noise realisations") {
    // A textured planet (gradient energy mostly in texture) and a plain small disc (all in the limb).
    FloatImage plain = Flat(128, 112, 0.05f);
    PaintDisc(plain, 61.3, 57.8, 20, [](int, int) { return 0.5; });
    const FloatImage images[2] = {TexturedPlanet(192, 176, 95.3, 88.6, 42, 0.05f), plain};
    for (const FloatImage& clean : images) {
        const FrameStats noiseless = MeasureFrame(clean, 0, 1);
        REQUIRE(noiseless.targetFound);
        CHECK(noiseless.qualitySigma == 0);  // no measurable noise
        for (const double noise : {0.004, 0.015}) {
            constexpr int kRuns = 200;
            double sum = 0, sum2 = 0, sigmaSum = 0;
            for (int k = 0; k < kRuns; ++k) {
                FloatImage im = clean;
                AddNoise(im, noise, 5000u + static_cast<unsigned>(k) + (noise > 0.01 ? 100000u : 0u));
                const FrameStats s = MeasureFrame(im, k, 1);
                REQUIRE(s.targetFound);
                REQUIRE(s.qualitySigma > 0);
                sum += s.quality;
                sum2 += s.quality * s.quality;
                sigmaSum += s.qualitySigma;
            }
            const double mean = sum / kRuns;
            const double empirical = std::sqrt(std::max(0.0, (sum2 - kRuns * mean * mean) / (kRuns - 1)));
            const double reported = sigmaSum / kRuns;
            INFO(clean.width << "x" << clean.height << " noise " << noise << ": quality mean " << mean
                             << ", empirical std " << empirical << ", mean qualitySigma " << reported);
            CHECK(empirical >= 0.7 * reported);
            CHECK(empirical <= 1.3 * reported);
        }
    }
}

TEST_CASE("a frame of background only has no target") {
    SECTION("1080p-sized pedestal, gentle gradient, 8-bit quantised, a hot pixel (the 2026-01-30 Saturn case)") {
        FloatImage im = Flat(1280, 720, 0.0f);
        for (int y = 0; y < im.height; ++y)
            for (int x = 0; x < im.width; ++x) At(im, x, y) = 0.022f + 0.004f * x / im.width + 0.002f * y / im.height;
        AddNoise(im, 1.5 / 255, 7);
        Quantize8(im);
        At(im, 400, 300) = 1.0f;
        const FrameStats s = MeasureFrame(im, 3, 1);
        CHECK(s.index == 3);
        CHECK_FALSE(s.targetFound);
        CHECK(s.quality == 0);
        CHECK(s.cx == 0);
        CHECK(s.cy == 0);
        CHECK((s.bx0 == 0 && s.by0 == 0 && s.bx1 == 0 && s.by1 == 0));
        double mean = 0;
        for (float v : im.data) mean += v;
        mean /= static_cast<double>(im.data.size());
        CHECK(s.brightness == Catch::Approx(mean).epsilon(1e-6));
    }
    SECTION("small frame (detection grid of single pixels), noise and a hot pixel") {
        FloatImage im = Flat(96, 64, 0.03f);
        AddNoise(im, 0.006, 11);
        At(im, 40, 30) = 1.0f;
        CHECK_FALSE(MeasureFrame(im, 0, 1).targetFound);
    }
    SECTION("perfectly flat frame") {
        CHECK_FALSE(MeasureFrame(Flat(64, 48, 0.2f), 0, 1).targetFound);
    }
    SECTION("fully saturated frame reports its clipping") {
        const FrameStats s = MeasureFrame(Flat(64, 48, 1.0f), 0, 1);
        CHECK_FALSE(s.targetFound);
        CHECK(s.clippedFraction == 1.0);
    }
    SECTION("all-black frame") {
        const FrameStats s = MeasureFrame(Flat(64, 48, 0.0f), 0, 1);
        CHECK_FALSE(s.targetFound);
        CHECK(s.quality == 0);
        CHECK(s.brightness == 0);
        CHECK(s.clippedFraction == 0);
    }
    SECTION("1x1 and 2x2 frames") {
        const FrameStats one = MeasureFrame(Flat(1, 1, 1.0f), 0, 1);
        CHECK_FALSE(one.targetFound);
        CHECK(one.clippedFraction == 1.0);
        FloatImage two = Flat(2, 2, 0.05f);
        At(two, 1, 1) = 0.9f;
        const FrameStats s2 = MeasureFrame(two, 0, 1);
        CHECK_FALSE(s2.targetFound);
        CHECK(s2.brightness == Catch::Approx((3 * 0.05 + 0.9) / 4).epsilon(1e-6));
    }
}

namespace {

// The 2026-01-30 Saturn AVI's background: a 5-7 DN pedestal with a gentle gradient, 1.5 DN noise, 8-bit.
FloatImage SaturnBackground(int w, int h, unsigned seed) {
    FloatImage im = Flat(w, h, 0.0f);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) At(im, x, y) = 0.022f + 0.004f * x / w + 0.002f * y / h;
    AddNoise(im, 1.5 / 255, seed);
    Quantize8(im);
    return im;
}

// The 2 x 2 cell mean LumaForMeasure makes of a Bayer frame (and MeasurementLuma of an undeclared
// mosaic): luma pixel (i, j) is sensor pixels 2i..2i+1 x 2j..2j+1.
FloatImage Bin2x2(const FloatImage& in) {
    FloatImage out = Flat(in.width / 2, in.height / 2, 0.0f);
    for (int y = 0; y < out.height; ++y)
        for (int x = 0; x < out.width; ++x) {
            const size_t a = static_cast<size_t>(2 * y) * in.width + 2 * x, b = a + in.width;
            At(out, x, y) = (in.data[a] + in.data[a + 1] + in.data[b] + in.data[b + 1]) / 4;
        }
    return out;
}

}  // namespace

TEST_CASE("small bright structures on a background-only frame are not a target") {
    using Paint = void (*)(FloatImage&, int, int);
    const std::pair<const char*, Paint> cases[] = {
        {"hot pair", [](FloatImage& im, int x, int y) { At(im, x, y) = At(im, x + 1, y) = 1.0f; }},
        {"hot column segment", [](FloatImage& im, int x, int y) { for (int k = 0; k < 16; ++k) At(im, x, y + k) = 1.0f; }},
        {"hot row segment", [](FloatImage& im, int x, int y) { for (int k = 0; k < 16; ++k) At(im, x + k, y) = 1.0f; }},
        {"3-px diagonal cosmic-ray streak", [](FloatImage& im, int x, int y) { for (int k = 0; k < 3; ++k) At(im, x + k, y + k) = 1.0f; }},
        {"staircase streak", [](FloatImage& im, int x, int y) {
             for (int k = 0; k < 6; ++k) At(im, x + k, y + k) = At(im, x + k + 1, y + k) = 1.0f;
         }},
        {"3x3 hot cluster", [](FloatImage& im, int x, int y) {
             for (int j = 0; j < 3; ++j)
                 for (int i = 0; i < 3; ++i) At(im, x + i, y + j) = 1.0f;
         }},
    };
    for (const auto& size : {std::pair<int, int>{1920, 1080}, std::pair<int, int>{96, 64}}) {
        const FloatImage background = SaturnBackground(size.first, size.second, 21);
        REQUIRE_FALSE(MeasureFrame(background, 0, 1).targetFound);
        for (const auto& c : cases) {
            FloatImage im = background;
            c.second(im, size.first / 3, size.second / 3);
            INFO(size.first << "x" << size.second << ": " << c.first);
            CHECK_FALSE(MeasureFrame(im, 0, 1).targetFound);
        }
    }
    // The same structures on a sensor-resolution frame measured as Bayer luma: binned 2 x 2 and
    // under the 4-luma-px floor of luma scale 2 (the opening is what rejects them there).
    const FloatImage sensor = SaturnBackground(1920, 1080, 22);
    REQUIRE_FALSE(MeasureFrame(Bin2x2(sensor), 0, 2).targetFound);
    for (const auto& c : cases)
        for (const int phase : {0, 1}) {  // aligned with a 2 x 2 cell, and straddling two
            FloatImage im = sensor;
            c.second(im, 640 + phase, 360 + phase);
            INFO("binned, phase " << phase << ": " << c.first);
            CHECK_FALSE(MeasureFrame(Bin2x2(im), 0, 2).targetFound);
        }
}

TEST_CASE("on half-resolution luma the size floor counts sensor pixels") {
    // Neptune as an undeclared-mosaic or Bayer capture measures it: an 8-px disc at sensor
    // resolution, seeing sigma 1 px, binned 2 x 2 -- 12 luma px above half its contrast (the
    // 2026-01-30 Neptune AVI: 9-22), under a 16-luma-px floor but 48 sensor pixels' worth.
    const double cx = 241.3, cy = 178.6;
    FloatImage sensor = Flat(480, 360, 0.0f);
    PaintDisc(sensor, cx, cy, 4.0, [](int, int) { return 0.6; });
    sensor = GaussianBlur(sensor, 1.0);
    for (float& v : sensor.data) v += 0.022f;
    AddNoise(sensor, 1.5 / 255, 43);
    Quantize8(sensor);
    const FloatImage luma = Bin2x2(sensor);
    const FrameStats s = MeasureFrame(luma, 0, 2);
    INFO("centroid " << s.cx << "," << s.cy);
    REQUIRE(s.targetFound);
    // FrameStats stay in luma pixels: sensor x = 2 * luma x + 0.5.
    CHECK(std::fabs(s.cx - (cx - 0.5) / 2) < 0.5);
    CHECK(std::fabs(s.cy - (cy - 0.5) / 2) < 0.5);
    // The same image taken as sensor resolution is a 6x6-px blob, under the 16-px floor.
    CHECK_FALSE(MeasureFrame(luma, 0, 1).targetFound);
}

TEST_CASE("a small real planet on that background is still found") {
    for (const double r : {20.0, 6.0}) {  // a 40-px disc and a 12-px one (Neptune-sized)
        FloatImage im = SaturnBackground(1920, 1080, 33);
        const double cx = 1011.4, cy = 517.7;
        PaintDisc(im, cx, cy, r, [](int, int) { return 0.6; });
        At(im, 300, 300) = At(im, 301, 300) = 1.0f;  // and a hot pair elsewhere
        const FrameStats s = MeasureFrame(im, 0, 1);
        INFO("r " << r << " centroid " << s.cx << "," << s.cy);
        REQUIRE(s.targetFound);
        CHECK(std::fabs(s.cx - cx) < 0.5);
        CHECK(std::fabs(s.cy - cy) < 0.5);
        CHECK(s.quality > 0);
    }
}

TEST_CASE("target centroid within half a pixel of the truth") {
    struct Case { int w, h; double cx, cy, r; };
    const Case cases[] = {{160, 120, 71.37, 58.81, 20}, {320, 240, 151.62, 97.19, 20}, {1280, 720, 640.27, 351.73, 60}};
    for (const Case& c : cases) {
        FloatImage im = Flat(c.w, c.h, 0.05f);
        PaintDisc(im, c.cx, c.cy, c.r, [](int, int) { return 0.6; });
        AddNoise(im, 0.01, 5);
        const FrameStats s = MeasureFrame(im, 0, 1);
        INFO(c.w << "x" << c.h << " centroid " << s.cx << "," << s.cy << " truth " << c.cx << "," << c.cy);
        REQUIRE(s.targetFound);
        CHECK(std::fabs(s.cx - c.cx) < 0.5);
        CHECK(std::fabs(s.cy - c.cy) < 0.5);
        // The box holds the whole disc and stays clear of the edges.
        CHECK(s.bx0 <= c.cx - c.r);
        CHECK(s.by0 <= c.cy - c.r);
        CHECK(s.bx1 >= c.cx + c.r);
        CHECK(s.by1 >= c.cy + c.r);
        CHECK(s.bx0 > 0);
        CHECK(s.by0 > 0);
        CHECK(s.bx1 < c.w);
        CHECK(s.by1 < c.h);
        CHECK(s.clippedFraction == 0);
        CHECK(s.brightness == Catch::Approx(0.6).margin(0.05));
    }
}

TEST_CASE("a target cut by the frame edge has its box touching that edge") {
    const int w = 200, h = 150;
    struct Case { double cx, cy; int edge; };  // 0 left, 1 top, 2 right, 3 bottom
    const Case cases[] = {{10.4, 70, 0}, {100, 6.3, 1}, {192.2, 80, 2}, {90, 147.5, 3}};
    for (const Case& c : cases) {
        for (int big = 0; big < 2; ++big) {  // 200x150 (1-pixel grid) and 4x that (4-pixel grid)
            const int k = big ? 4 : 1;
            FloatImage im = TexturedPlanet(w * k, h * k, c.cx * k, c.cy * k, 25.0 * k, 0.05f);
            AddNoise(im, 0.005, 3);
            const FrameStats s = MeasureFrame(im, 0, 1);
            INFO("edge " << c.edge << " scale " << k << " box " << s.bx0 << "," << s.by0 << " - " << s.bx1 << "," << s.by1);
            REQUIRE(s.targetFound);
            if (c.edge == 0) CHECK(s.bx0 == 0);
            if (c.edge == 1) CHECK(s.by0 == 0);
            if (c.edge == 2) CHECK(s.bx1 == w * k);
            if (c.edge == 3) CHECK(s.by1 == h * k);
        }
    }
}

TEST_CASE("clipped fraction is exact on a saturated disc") {
    for (int k = 1; k <= 2; ++k) {  // 160x120 (1-pixel grid) and 320x240 (2-pixel grid)
        const int w = 160 * k, h = 120 * k;
        const double cx = 80.5 * k, cy = 60.25 * k, r1 = 8.0 * k, r2 = 20.0 * k;
        FloatImage im = Flat(w, h, 0.05f);
        int n1 = 0, n2 = 0;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const double d2 = (x - cx) * (x - cx) + (y - cy) * (y - cy);
                if (d2 < r1 * r1) { At(im, x, y) = 1.0f; ++n1; ++n2; }
                else if (d2 < r2 * r2) { At(im, x, y) = 0.6f; ++n2; }
            }
        const FrameStats s = MeasureFrame(im, 0, 1);
        INFO("grid " << k << " n1 " << n1 << " n2 " << n2);
        REQUIRE(s.targetFound);
        CHECK(s.clippedFraction == static_cast<double>(n1) / n2);
        CHECK(s.brightness == Catch::Approx((n1 * 1.0 + (n2 - n1) * 0.6) / n2).epsilon(1e-6));
    }
}

TEST_CASE("MeasureFrame rejects malformed input") {
    CHECK_THROWS_AS(MeasureFrame(FloatImage{}, 0, 1), std::invalid_argument);
    CHECK_THROWS_AS(MeasureFrame(FloatImage{4, 4, 3, std::vector<float>(48)}, 0, 1), std::invalid_argument);
    CHECK_THROWS_AS(MeasureFrame(FloatImage{4, 4, 1, std::vector<float>(15)}, 0, 1), std::invalid_argument);
    CHECK_THROWS_AS(MeasureFrame(Flat(8, 8, 0.1f), 0, 0.5), std::invalid_argument);
    CHECK_THROWS_AS(MeasureFrame(Flat(8, 8, 0.1f), 0, 0), std::invalid_argument);
    CHECK_THROWS_AS(MeasureFrame(Flat(8, 8, 0.1f), 0, std::nan("")), std::invalid_argument);
}

namespace {

constexpr int kAviW = 128, kAviH = 96, kAviFrames = 200;

// Counts live readers and reads; optionally slow, optionally failing.
class CountingSource : public ser::FrameSource {
public:
    CountingSource(std::unique_ptr<ser::FrameSource> inner, std::atomic<int>& live, std::atomic<int>& reads, int delayMs,
                   int64_t failAt = -1)
        : inner_(std::move(inner)), live_(live), reads_(reads), delayMs_(delayMs), failAt_(failAt) { ++live_; }
    ~CountingSource() override { --live_; }
    const ser::FileInfo& Info() const override { return inner_->Info(); }
    ser::Frame ReadFrame(int64_t i) const override {
        if (delayMs_ > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delayMs_));
        ++reads_;
        if (i == failAt_) throw ser::SerError("frame " + std::to_string(i) + " is unreadable");
        return inner_->ReadFrame(i);
    }
    std::optional<int64_t> FrameTimestamp(int64_t i) const override { return inner_->FrameTimestamp(i); }

private:
    std::unique_ptr<ser::FrameSource> inner_;
    std::atomic<int>& live_;
    std::atomic<int>& reads_;
    int delayMs_;
    int64_t failAt_;
};

}  // namespace

TEST_CASE("MeasureAll matches per-frame measurement and is identical for 1 and 8 threads") {
    static testing::TempDir dir;
    const std::string path = dir.NewPath(".avi");
    {
        std::vector<std::vector<uint8_t>> frames;
        for (int f = 0; f < kAviFrames; ++f) {
            const double cx = 40 + 48.0 * f / kAviFrames, cy = 46 + 4 * std::sin(f * 0.1);
            FloatImage im = TexturedPlanet(kAviW, kAviH, cx, cy, 16, 0.04f);
            AddNoise(im, 0.008, 100u + f);
            Quantize8(im);
            std::vector<uint8_t> bytes(im.data.size());
            for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<uint8_t>(std::lround(im.data[i] * 255));
            frames.push_back(std::move(bytes));
        }
        testing::AviSpec spec;
        spec.width = kAviW;
        spec.height = kAviH;
        testing::WriteAvi(path, spec, frames);
    }
    // Auto: no sidecar next to the file, so the pattern is unknown -- an undeclared mosaic (Ruling P9),
    // measured binned like RGGB though it is read as mono.
    for (ser::BayerChoice choice : {ser::BayerChoice::None, ser::BayerChoice::RGGB, ser::BayerChoice::Auto}) {
        auto probe = ser::OpenVideo(path, choice);
        const ser::FileInfo info = probe->Info();
        REQUIRE(info.frameCount == kAviFrames);
        REQUIRE(info.colorId == (choice == ser::BayerChoice::RGGB ? ser::ColorId::BayerRGGB : ser::ColorId::Mono));
        REQUIRE(stack::UndeclaredMosaic(info) == (choice == ser::BayerChoice::Auto));
        REQUIRE(stack::LumaScale(info) == (choice == ser::BayerChoice::None ? 1.0 : 2.0));
        const stack::SourceFactory factory = [&] { return ser::OpenVideo(path, choice); };

        std::vector<std::string> phases;
        std::vector<int64_t> dones;
        bool otherThread = false;
        const auto caller = std::this_thread::get_id();
        stack::Progress progress;
        progress.report = [&](const std::string& phase, int64_t done, int64_t total) {
            if (std::this_thread::get_id() != caller) otherThread = true;
            phases.push_back(phase);
            dones.push_back(done);
            CHECK(total == kAviFrames);
        };
        progress.cancelled = [&] {
            if (std::this_thread::get_id() != caller) otherThread = true;
            return false;
        };

        auto run = [&](int threads) {
            phases.clear();
            dones.clear();
            std::vector<FrameStats> r = MeasureAll(factory, info, threads, progress);
            INFO("threads " << threads);
            REQUIRE_FALSE(phases.empty());
            for (const std::string& p : phases) CHECK(p == "Measuring frames");
            CHECK(std::is_sorted(dones.begin(), dones.end()));
            CHECK(dones.back() == kAviFrames);
            return r;
        };
        const std::vector<FrameStats> one = run(1);
        const std::vector<FrameStats> eight = run(8);
        INFO("bayer choice " << static_cast<int>(choice));
        REQUIRE(one.size() == static_cast<size_t>(kAviFrames));
        REQUIRE(eight.size() == static_cast<size_t>(kAviFrames));
        int found = 0;
        for (int64_t i = 0; i < kAviFrames; ++i) {
            const FrameStats direct = MeasureFrame(stack::MeasurementLuma(probe->ReadFrame(i), info), i, stack::LumaScale(info));
            INFO("frame " << i);
            CHECK(SameStats(one[i], direct));
            CHECK(SameStats(eight[i], direct));
            found += direct.targetFound ? 1 : 0;
        }
        CHECK(found == kAviFrames);
        CHECK_FALSE(otherThread);
    }
}

TEST_CASE("MeasureAll: cancel mid-run throws and leaves no thread or reader behind") {
    static testing::TempDir dir;
    const std::string path = dir.NewPath(".avi");
    {
        std::vector<std::vector<uint8_t>> frames(kAviFrames, std::vector<uint8_t>(kAviW * kAviH, 10));
        testing::AviSpec spec;
        spec.width = kAviW;
        spec.height = kAviH;
        testing::WriteAvi(path, spec, frames);
    }
    const ser::FileInfo info = ser::OpenVideo(path)->Info();
    std::atomic<int> live{0}, reads{0};
    const stack::SourceFactory factory = [&] {
        return std::make_unique<CountingSource>(ser::OpenVideo(path), live, reads, 5);
    };
    int64_t reported = 0;
    stack::Progress progress;
    progress.report = [&](const std::string&, int64_t done, int64_t) { reported = done; };
    progress.cancelled = [&] { return reported >= 20; };
    CHECK_THROWS_AS(MeasureAll(factory, info, 8, progress), stack::StackCancelled);
    CHECK(live == 0);  // every worker's reader is gone: the workers returned and were joined
    const int after = reads;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(reads == after);  // nothing reads any more
    CHECK(after < kAviFrames * 3 / 4);  // it stopped early
}

TEST_CASE("MeasureAll: a worker's failure is rethrown after all threads are joined") {
    static testing::TempDir dir;
    const std::string path = dir.NewPath(".avi");
    {
        std::vector<std::vector<uint8_t>> frames(40, std::vector<uint8_t>(kAviW * kAviH, 10));
        testing::AviSpec spec;
        spec.width = kAviW;
        spec.height = kAviH;
        testing::WriteAvi(path, spec, frames);
    }
    const ser::FileInfo info = ser::OpenVideo(path)->Info();
    std::atomic<int> live{0}, reads{0}, calls{0};
    SECTION("factory throws in one worker") {
        const stack::SourceFactory factory = [&]() -> std::unique_ptr<ser::FrameSource> {
            if (++calls == 3) throw ser::SerError("cannot open");
            return std::make_unique<CountingSource>(ser::OpenVideo(path), live, reads, 1);
        };
        CHECK_THROWS_WITH(MeasureAll(factory, info, 4, {}), "cannot open");
    }
    SECTION("factory returns no reader") {
        const stack::SourceFactory factory = [&]() -> std::unique_ptr<ser::FrameSource> { return nullptr; };
        CHECK_THROWS_AS(MeasureAll(factory, info, 4, {}), std::runtime_error);
    }
    SECTION("reader for a different file") {
        const stack::SourceFactory factory = [&] {
            return std::make_unique<CountingSource>(ser::OpenVideo(path), live, reads, 0);
        };
        ser::FileInfo other = info;
        SECTION("frame count") { other.frameCount = 41; }
        SECTION("colour") { other.colorId = ser::ColorId::BayerRGGB; }
        SECTION("normalisation depth") { other.normDepth = 12; }
        CHECK_THROWS_AS(MeasureAll(factory, other, 2, {}), ser::SerError);
    }
    SECTION("a read fails mid-run: the error comes out, no partial result") {
        const stack::SourceFactory factory = [&] {
            return std::make_unique<CountingSource>(ser::OpenVideo(path), live, reads, 1, 17);
        };
        CHECK_THROWS_WITH(MeasureAll(factory, info, 4, {}), "frame 17 is unreadable");
    }
    SECTION("a throwing progress callback: workers are joined, its exception comes out") {
        const stack::SourceFactory factory = [&] {
            return std::make_unique<CountingSource>(ser::OpenVideo(path), live, reads, 2);
        };
        int reports = 0;
        stack::Progress progress;
        progress.report = [&](const std::string&, int64_t, int64_t) {
            if (++reports == 2) throw std::runtime_error("interface gone");
        };
        CHECK_THROWS_WITH(MeasureAll(factory, info, 4, progress), "interface gone");
    }
    SECTION("a throwing cancel poll: the same") {
        const stack::SourceFactory factory = [&] {
            return std::make_unique<CountingSource>(ser::OpenVideo(path), live, reads, 2);
        };
        int polls = 0;
        stack::Progress progress;
        progress.cancelled = [&]() -> bool {
            if (++polls == 3) throw std::runtime_error("poll failed");
            return false;
        };
        CHECK_THROWS_WITH(MeasureAll(factory, info, 4, progress), "poll failed");
    }
    CHECK(live == 0);
    const int after = reads;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(reads == after);
}

TEST_CASE("MeasureAll on an empty capture returns nothing") {
    ser::FileInfo info;
    info.frameCount = 0;
    int calls = 0;
    const stack::SourceFactory factory = [&]() -> std::unique_ptr<ser::FrameSource> { ++calls; return nullptr; };
    CHECK(MeasureAll(factory, info, 8, {}).empty());
    CHECK(calls == 0);
}
