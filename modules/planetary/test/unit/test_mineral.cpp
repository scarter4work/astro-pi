// The mineral-moon engine (spec Part B, steps 1-8 and the 2026-10-27 amendment): each step against its
// measured requirement on synthetic lunar scenes whose truth is known (MoonSynth.h).
#include <catch_amalgamated.hpp>

#include <algorithm>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "Mineral.h"
#include "MoonSynth.h"

using namespace planetary;
using planetary::testing::MakeMoonScene;
using planetary::testing::MoonScene;
using planetary::testing::MoonSceneSpec;

namespace {

size_t Count(const std::vector<uint8_t>& a) {
    size_t n = 0;
    for (uint8_t v : a) n += v != 0;
    return n;
}
// Share of a's members that are also b's.
double Within(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    size_t n = 0, both = 0;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i]) {
            ++n;
            both += b[i] != 0;
        }
    return n ? static_cast<double>(both) / n : 0.0;
}

void CheckMasksConsistent(const stack::LunarMasks& m, const ser::FloatImage& im) {
    REQUIRE(m.width == im.width);
    REQUIRE(m.height == im.height);
    REQUIRE(m.disk.size() == static_cast<size_t>(im.width) * im.height);
    REQUIRE(m.sky.size() == m.disk.size());
    size_t both = 0;
    for (size_t i = 0; i < m.disk.size(); ++i) both += m.disk[i] && m.sky[i];
    CHECK(both == 0);
    CHECK(m.diskCount == Count(m.disk));
    CHECK(m.skyCount == Count(m.sky));
}

void CheckSegmentation(const MoonScene& sc, const stack::LunarMasks& m) {
    CheckMasksConsistent(m, sc.observed);
    const double diskIn = Within(sc.diskTruth, m.disk), skyInDisk = Within(sc.skyTruth, m.disk),
                 skyIn = Within(sc.skyTruth, m.sky);
    INFO("diskTruth in disk " << diskIn << ", skyTruth in disk " << skyInDisk << ", skyTruth in sky " << skyIn);
    CHECK(diskIn >= 0.98);
    CHECK(skyInDisk <= 0.005);
    CHECK(skyIn >= 0.95);
}

}  // namespace

// ---- Step 1: lunar disk and sky (Task B2) ----

TEST_CASE("segment: full disk") {
    MoonSceneSpec s;
    s.blurSigma = 1.5;
    s.noiseStd = 0.005;
    s.pedestal = {0.01, 0.01, 0.01};
    const MoonScene sc = MakeMoonScene(s);
    CheckSegmentation(sc, stack::SegmentLunar(sc.observed, 4));
}

TEST_CASE("segment: disk cut by the frame top and bottom") {
    MoonSceneSpec s;  // like the 2026-10-23 64.6F: 1080p, the disc cut off top and bottom
    s.width = 1920;
    s.height = 1080;
    s.cx = 960;
    s.cy = 540;
    s.radius = 700;
    s.blurSigma = 1.5;
    s.noiseStd = 0.005;
    s.pedestal = {0.01, 0.01, 0.01};
    const MoonScene sc = MakeMoonScene(s);
    CheckSegmentation(sc, stack::SegmentLunar(sc.observed, 8));
}

TEST_CASE("segment: a featureless disc covering most of the frame") {
    // The frame's median is on the disc here: the check that the disc is not just the sky's noise must compare
    // the disc with the sky's class, not with the frame's median (which read "no lunar disk").
    MoonSceneSpec s;
    s.width = 1920;
    s.height = 1080;
    s.cx = 960;
    s.cy = 540;
    s.radius = 700;
    s.textureAmplitude = 0;
    s.mariaFraction = 0;
    s.tintVariation = 0;
    s.blurSigma = 1.2;
    s.noiseStd = 0.002;
    s.pedestal = {0.01, 0.01, 0.01};
    const MoonScene sc = MakeMoonScene(s);
    const stack::LunarMasks m = stack::SegmentLunar(sc.observed, 8);
    INFO(m.reason);
    CheckSegmentation(sc, m);
}

TEST_CASE("segment: crescent with terminator") {
    MoonSceneSpec s;
    s.phaseDeg = 120;
    s.blurSigma = 1.0;
    s.noiseStd = 0.005;
    const MoonScene sc = MakeMoonScene(s);
    const stack::LunarMasks m = stack::SegmentLunar(sc.observed, 4);
    CheckMasksConsistent(m, sc.observed);
    // Lit pixels clear of the limb, by their true luminance.
    const std::vector<double> L = testing::Lum(sc.truth);
    std::vector<double> lit;
    const double rIn = s.radius - 2 - 2 * s.blurSigma;
    for (int y = 0; y < s.height; ++y)
        for (int x = 0; x < s.width; ++x) {
            const size_t i = static_cast<size_t>(y) * s.width + x;
            if (std::hypot(x - s.cx, y - s.cy) <= rIn && L[i] > 0) lit.push_back(L[i]);
        }
    REQUIRE(lit.size() > 1000);
    std::vector<double> sorted = lit;
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<long>(sorted.size() / 2), sorted.end());
    const double median = sorted[sorted.size() / 2];
    size_t n = 0, in = 0;
    for (int y = 0; y < s.height; ++y)
        for (int x = 0; x < s.width; ++x) {
            const size_t i = static_cast<size_t>(y) * s.width + x;
            if (std::hypot(x - s.cx, y - s.cy) <= rIn && L[i] >= 0.1 * median) {
                ++n;
                in += m.disk[i] != 0;
            }
        }
    INFO("lit >= 10% of the lit median: " << n << " px, in disk " << in);
    CHECK(static_cast<double>(in) / n >= 0.95);
    CHECK(Within(sc.skyTruth, m.disk) <= 0.005);
}

TEST_CASE("segment: no sky") {
    MoonSceneSpec s;
    s.radius = 600;  // the disc fills the 640 x 480 frame
    s.blurSigma = 1.0;
    s.noiseStd = 0.005;
    const MoonScene sc = MakeMoonScene(s);
    stack::LunarMasks m;
    REQUIRE_NOTHROW(m = stack::SegmentLunar(sc.observed, 4));
    CheckMasksConsistent(m, sc.observed);
    CHECK(m.skyCount < 0.01 * sc.observed.width * sc.observed.height);
    CHECK(Within(sc.diskTruth, m.disk) >= 0.98);
}

// ---- Steps 2-3: colour calibration and background (Task B3) ----

namespace {

// Mean of channel c over mask pixels, leaving out `exclude` pixels (may be empty).
double MaskMean(const ser::FloatImage& im, const std::vector<uint8_t>& mask, int c,
                const std::vector<uint8_t>& exclude = {}) {
    double s = 0;
    size_t n = 0;
    for (size_t i = 0; i < mask.size(); ++i)
        if (mask[i] && (exclude.empty() || !exclude[i])) {
            s += im.data[i * im.channels + c];
            ++n;
        }
    return n ? s / n : 0;
}

// Largest |mean_c / mean_G - 1| over R and B on the disc.
double DiskImbalance(const ser::FloatImage& im, const std::vector<uint8_t>& disk, const std::vector<uint8_t>& exclude = {}) {
    const double g = MaskMean(im, disk, 1, exclude);
    return std::max(std::fabs(MaskMean(im, disk, 0, exclude) / g - 1), std::fabs(MaskMean(im, disk, 2, exclude) / g - 1));
}

// Least-squares plane a + b x + c y of channel ch over mask; returns the rise across the frame,
// hypot(b (w - 1), c (h - 1)).
double PlaneRise(const ser::FloatImage& im, const std::vector<uint8_t>& mask, int ch) {
    double S[3][3] = {}, r[3] = {};
    for (int y = 0; y < im.height; ++y)
        for (int x = 0; x < im.width; ++x) {
            const size_t i = static_cast<size_t>(y) * im.width + x;
            if (!mask[i]) continue;
            const double f[3] = {1, static_cast<double>(x), static_cast<double>(y)}, v = im.data[i * im.channels + ch];
            for (int a = 0; a < 3; ++a) {
                r[a] += f[a] * v;
                for (int b = 0; b < 3; ++b) S[a][b] += f[a] * f[b];
            }
        }
    // Cramer's rule
    auto det = [](double M[3][3]) {
        return M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) +
               M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
    };
    const double d = det(S);
    double coef[3];
    for (int k = 0; k < 3; ++k) {
        double M[3][3];
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b) M[a][b] = b == k ? r[a] : S[a][b];
        coef[k] = det(M) / d;
    }
    return std::hypot(coef[1] * (im.width - 1), coef[2] * (im.height - 1));
}

bool Ascii(const std::string& s) {
    return std::all_of(s.begin(), s.end(), [](char ch) { return static_cast<unsigned char>(ch) < 0x80; });
}

}  // namespace

TEST_CASE("calibrate: cast removed") {
    MoonSceneSpec s;
    s.cast = {1.2, 1.0, 0.8};
    s.pedestal = {0.02, 0.02, 0.02};
    s.blurSigma = 1.0;
    s.noiseStd = 0.003;
    MoonScene sc = MakeMoonScene(s);
    const stack::LunarMasks m = stack::SegmentLunar(sc.observed, 4);
    REQUIRE(DiskImbalance(sc.observed, sc.diskTruth) > 0.1);
    const stack::StepRecord cal = stack::CalibrateColour(sc.observed, m);
    const stack::StepRecord bg = stack::NeutraliseBackground(sc.observed, m);
    CHECK_FALSE(cal.skipped);
    CHECK_FALSE(bg.skipped);
    INFO(cal.history << "\n" << bg.history);
    INFO("disk imbalance " << DiskImbalance(sc.observed, sc.diskTruth));
    CHECK(DiskImbalance(sc.observed, sc.diskTruth) <= 0.01);
}

TEST_CASE("background: sky neutral and flat") {
    MoonSceneSpec s;
    s.cast = {1.1, 1.0, 0.9};
    s.pedestal = {0.02, 0.015, 0.025};
    s.gradientX = 0.01;
    s.blurSigma = 1.0;
    s.noiseStd = 0.003;
    MoonScene sc = MakeMoonScene(s);
    REQUIRE(PlaneRise(sc.observed, sc.skyTruth, 1) > 0.009);
    const stack::LunarMasks m = stack::SegmentLunar(sc.observed, 4);
    stack::CalibrateColour(sc.observed, m);
    const stack::StepRecord bg = stack::NeutraliseBackground(sc.observed, m);
    INFO(bg.history);
    CHECK_FALSE(bg.skipped);
    for (int c = 0; c < 3; ++c) {
        const double skyMean = MaskMean(sc.observed, sc.skyTruth, c), diskMean = MaskMean(sc.observed, sc.diskTruth, c);
        const double rise = PlaneRise(sc.observed, sc.skyTruth, c);
        INFO("channel " << c << ": sky mean " << skyMean << ", disk mean " << diskMean << ", residual rise " << rise);
        CHECK(std::fabs(skyMean) <= 0.005 * diskMean);
        CHECK(rise <= 0.1 * s.gradientX);
    }
    INFO("disk imbalance " << DiskImbalance(sc.observed, sc.diskTruth));
    CHECK(DiskImbalance(sc.observed, sc.diskTruth) <= 0.01);
}

TEST_CASE("calibrate: clipped pixels excluded") {
    MoonSceneSpec s;
    s.cast = {1.2, 1.0, 0.8};
    s.pedestal = {0.02, 0.02, 0.02};
    s.blurSigma = 1.0;
    s.noiseStd = 0.003;
    s.clipFraction = 0.03;
    s.clipChannels = {true, false, false};  // 3% of the disc saturated in R only
    MoonScene sc = MakeMoonScene(s);
    const stack::LunarMasks m = stack::SegmentLunar(sc.observed, 4);
    const stack::StepRecord cal = stack::CalibrateColour(sc.observed, m);
    stack::NeutraliseBackground(sc.observed, m);
    INFO(cal.history);
    CHECK_FALSE(cal.skipped);
    INFO("disk imbalance " << DiskImbalance(sc.observed, sc.diskTruth, sc.clipped));
    CHECK(DiskImbalance(sc.observed, sc.diskTruth, sc.clipped) <= 0.01);
}

TEST_CASE("calibrate: tiny disk skips the white reference") {
    MoonSceneSpec s;
    s.radius = 22;  // ~0.5% of the 640 x 480 frame
    s.cast = {1.2, 1.0, 0.8};
    s.noiseStd = 0.003;
    MoonScene sc = MakeMoonScene(s);
    const stack::LunarMasks m = stack::SegmentLunar(sc.observed, 4);
    INFO("disk " << m.diskCount << " px");
    REQUIRE(m.diskCount > 0);
    REQUIRE(m.diskCount < 0.01 * s.width * s.height);
    const std::vector<float> before = sc.observed.data;
    const stack::StepRecord cal = stack::CalibrateColour(sc.observed, m);
    INFO(cal.history);
    CHECK(cal.skipped);
    CHECK(cal.history.find("1%") != std::string::npos);
    CHECK(sc.observed.data == before);
}

TEST_CASE("background: no sky") {
    MoonSceneSpec s;
    s.radius = 600;
    s.pedestal = {0.02, 0.02, 0.02};
    s.noiseStd = 0.003;
    MoonScene sc = MakeMoonScene(s);
    const stack::LunarMasks m = stack::SegmentLunar(sc.observed, 4);
    const std::vector<float> before = sc.observed.data;
    const stack::StepRecord bg = stack::NeutraliseBackground(sc.observed, m);
    INFO(bg.history);
    CHECK(bg.skipped);
    CHECK(bg.history.find("sky") != std::string::npos);
    CHECK(sc.observed.data == before);
    // Calibration still runs on the disc, and says it has no background reference.
    const stack::StepRecord cal = stack::CalibrateColour(sc.observed, m);
    INFO(cal.history);
    CHECK_FALSE(cal.skipped);
    CHECK(cal.history.find("sky") != std::string::npos);
}

TEST_CASE("history ASCII") {
    MoonSceneSpec s;
    s.cast = {1.2, 1.0, 0.8};
    s.pedestal = {0.02, 0.02, 0.02};
    s.gradientY = 0.005;
    s.noiseStd = 0.003;
    MoonScene sc = MakeMoonScene(s);
    const stack::LunarMasks m = stack::SegmentLunar(sc.observed, 4);
    const stack::StepRecord cal = stack::CalibrateColour(sc.observed, m);
    const stack::StepRecord bg = stack::NeutraliseBackground(sc.observed, m);
    for (const std::string& h : {cal.history, bg.history}) {
        INFO(h);
        CHECK_FALSE(h.empty());
        CHECK(Ascii(h));
    }
}

// ---- Step 4b: channel alignment (amendment) ----

namespace {

// Shift s of channel c of `im` against channel c of `ref` (im(x) ~ ref(x - s)), first order, over the
// interior 10 px in from every edge: s = -(sum g g^T)^-1 sum g (im - ref), g = ref's central gradient.
// Independent of the engine's estimator; good to a few % for |s| <~ 0.3 px (it reads low at 0.5 px).
std::pair<double, double> ResidualShift(const ser::FloatImage& im, const ser::FloatImage& ref, int c) {
    double sxx = 0, sxy = 0, syy = 0, bx = 0, by = 0;
    const int w = ref.width;
    auto at = [&](const ser::FloatImage& f, int x, int y) { return static_cast<double>(f.data[(static_cast<size_t>(y) * w + x) * 3 + c]); };
    for (int y = 10; y < ref.height - 10; ++y)
        for (int x = 10; x < w - 10; ++x) {
            const double gx = 0.5 * (at(ref, x + 1, y) - at(ref, x - 1, y)), gy = 0.5 * (at(ref, x, y + 1) - at(ref, x, y - 1));
            const double d = at(im, x, y) - at(ref, x, y);
            sxx += gx * gx;
            sxy += gx * gy;
            syy += gy * gy;
            bx += gx * d;
            by += gy * d;
        }
    const double det = sxx * syy - sxy * sxy;
    return {-(syy * bx - sxy * by) / det, -(sxx * by - sxy * bx) / det};
}

}  // namespace

TEST_CASE("align: a 0.5 px vertical R/B offset is corrected") {
    MoonSceneSpec s;
    s.blurSigma = 1.0;
    s.noiseStd = 0.001;
    const MoonScene ref = MakeMoonScene(s);  // same seed, same noise: the offset is the only difference
    s.shiftY = {0.5, 0, -0.5};
    MoonScene sc = MakeMoonScene(s);
    const auto r0 = ResidualShift(sc.observed, ref.observed, 0), b0 = ResidualShift(sc.observed, ref.observed, 2);
    INFO("before: R (" << r0.first << ", " << r0.second << "), B (" << b0.first << ", " << b0.second << ")");
    REQUIRE(r0.second > 0.3);
    REQUIRE(b0.second < -0.3);
    const stack::StepRecord rec = stack::AlignChannels(sc.observed, 4);
    const auto r1 = ResidualShift(sc.observed, ref.observed, 0), b1 = ResidualShift(sc.observed, ref.observed, 2);
    INFO(rec.history);
    INFO("after: R (" << r1.first << ", " << r1.second << "), B (" << b1.first << ", " << b1.second << ")");
    CHECK_FALSE(rec.skipped);
    CHECK(std::hypot(r1.first, r1.second) <= 0.05);
    CHECK(std::hypot(b1.first, b1.second) <= 0.05);
    CHECK(Ascii(rec.history));
}

TEST_CASE("align: a 0.02 px offset is a no-op with a HISTORY line") {
    MoonSceneSpec s;
    s.blurSigma = 1.0;
    s.noiseStd = 0.001;
    s.shiftY = {0.02, 0, -0.02};
    MoonScene sc = MakeMoonScene(s);
    const std::vector<float> before = sc.observed.data;
    const stack::StepRecord rec = stack::AlignChannels(sc.observed, 4);
    INFO(rec.history);
    CHECK(rec.skipped);
    CHECK(rec.history.find("0.1 px") != std::string::npos);
    CHECK(Ascii(rec.history));
    CHECK(sc.observed.data == before);
}

TEST_CASE("align: a tiny disc is not resampled") {
    // A 24 x 24 frame with an 8 px disc and no true offset: too few windows of structure to tell a dispersion
    // shift from the channels' own colour (task ENG review M3: a false B shift of 0.23 px was applied).
    for (unsigned seed = 1; seed <= 5; ++seed) {
        MoonSceneSpec s;
        s.width = s.height = 24;
        s.cx = s.cy = 12;
        s.radius = 8;
        s.blurSigma = 1.0;
        s.noiseStd = 0.002;
        s.seed = seed;
        MoonScene sc = MakeMoonScene(s);
        const std::vector<float> before = sc.observed.data;
        const stack::StepRecord rec = stack::AlignChannels(sc.observed, 4);
        INFO("seed " << seed << ": " << rec.history);
        CHECK(rec.skipped);
        CHECK(sc.observed.data == before);
    }
}

TEST_CASE("align: no false shift on a quarter Moon without sky") {
    // The disc fills the frame, half of it unlit; no true offset (review M3 measured 0.081 px here).
    for (unsigned seed = 1; seed <= 3; ++seed) {
        MoonSceneSpec s;
        s.radius = 600;
        s.phaseDeg = 90;
        s.blurSigma = 1.0;
        s.noiseStd = 0.003;
        s.seed = seed;
        MoonScene sc = MakeMoonScene(s);
        const std::vector<float> before = sc.observed.data;
        const stack::StepRecord rec = stack::AlignChannels(sc.observed, 4);
        INFO("seed " << seed << ": " << rec.history);
        CHECK(rec.skipped);
        CHECK(sc.observed.data == before);
    }
}

TEST_CASE("align: the result does not depend on the thread count") {
    MoonSceneSpec s;
    s.blurSigma = 1.0;
    s.noiseStd = 0.002;
    s.shiftY = {0.4, 0, -0.3};
    const MoonScene sc = MakeMoonScene(s);
    ser::FloatImage ref = sc.observed;
    const stack::StepRecord r1 = stack::AlignChannels(ref, 1);
    REQUIRE_FALSE(r1.skipped);
    for (int threads : {2, 3, 7, 16, 32}) {
        ser::FloatImage im = sc.observed;
        const stack::StepRecord rec = stack::AlignChannels(im, threads);
        INFO(threads << " threads: " << rec.history);
        CHECK(rec.history == r1.history);
        CHECK(im.data == ref.data);
    }
}

// ---- Step 4: luminance wavelet sharpening (Task B4) ----

namespace {

double RmsOver(const std::vector<double>& a, const std::vector<double>& b, const std::vector<uint8_t>& mask) {
    double s = 0;
    size_t n = 0;
    for (size_t i = 0; i < a.size(); ++i)
        if (mask[i]) {
            s += (a[i] - b[i]) * (a[i] - b[i]);
            ++n;
        }
    return std::sqrt(s / n);
}

double StdOver(const std::vector<double>& a, const std::vector<uint8_t>& mask) {
    double s = 0, s2 = 0;
    size_t n = 0;
    for (size_t i = 0; i < a.size(); ++i)
        if (mask[i]) {
            s += a[i];
            s2 += a[i] * a[i];
            ++n;
        }
    return std::sqrt(std::max(0.0, s2 / n - (s / n) * (s / n)));
}

// Azimuthal mean of v in 0.5 px rings, r from r0 to r1.
std::vector<double> RadialProfile(const std::vector<double>& v, const MoonSceneSpec& s, double r0, double r1) {
    const int bins = static_cast<int>((r1 - r0) / 0.5);
    std::vector<double> sum(bins, 0), cnt(bins, 0);
    for (int y = 0; y < s.height; ++y)
        for (int x = 0; x < s.width; ++x) {
            const int b = static_cast<int>((std::hypot(x - s.cx, y - s.cy) - r0) / 0.5);
            if (b < 0 || b >= bins) continue;
            sum[b] += v[static_cast<size_t>(y) * s.width + x];
            cnt[b] += 1;
        }
    for (int b = 0; b < bins; ++b) sum[b] /= std::max(1.0, cnt[b]);
    return sum;
}

}  // namespace

TEST_CASE("starlet: layers sum to the input") {
    MoonSceneSpec s;
    s.blurSigma = 1.0;
    s.noiseStd = 0.005;
    const MoonScene sc = MakeMoonScene(s);
    ser::FloatImage L;
    L.width = s.width;
    L.height = s.height;
    L.channels = 1;
    for (double v : testing::Lum(sc.observed)) L.data.push_back(static_cast<float>(v));
    const std::vector<ser::FloatImage> layers = stack::StarletLayers(L, 4, 4);
    REQUIRE(layers.size() == 5);
    double worst = 0;
    for (size_t i = 0; i < L.data.size(); ++i) {
        double sum = 0;
        for (const ser::FloatImage& l : layers) sum += l.data[i];
        worst = std::max(worst, std::fabs(sum - L.data[i]));
    }
    INFO("max |sum - input| " << worst);
    CHECK(worst <= 1e-5);
    // LayerNoise on the sky (white noise of std 0.005 / sqrt 3 in the luminance): the B3-spline starlet's
    // first layer keeps 0.889 of white noise's std (Starck, Murtagh & Fadili 2010, table 6.1).
    const double expect = 0.889 * 0.005 / std::sqrt(3.0), got = stack::LayerNoise(layers[0], sc.skyTruth);
    INFO("layer 1 noise on the sky " << got << ", expected " << expect);
    CHECK(std::fabs(got / expect - 1) <= 0.05);
}

TEST_CASE("sharpen: closer to the truth") {
    MoonSceneSpec s;
    s.blurSigma = 1.5;
    s.noiseStd = 0.005;
    MoonScene sc = MakeMoonScene(s);
    const stack::LunarMasks m = stack::SegmentLunar(sc.observed, 4);
    const std::vector<double> truth = testing::Lum(sc.truth), before = testing::Lum(sc.observed);
    const stack::StepRecord rec = stack::SharpenLuminance(sc.observed, m, 4);
    const std::vector<double> after = testing::Lum(sc.observed);
    const double e0 = RmsOver(before, truth, sc.diskTruth), e1 = RmsOver(after, truth, sc.diskTruth);
    INFO(rec.history);
    INFO("disk RMS error vs truth: before " << e0 << ", after " << e1 << " (" << 100 * (1 - e1 / e0) << "% lower)");
    CHECK_FALSE(rec.skipped);
    CHECK(e1 <= 0.9 * e0);
    CHECK(Ascii(rec.history));
}

TEST_CASE("sharpen: noise-only gain fails") {
    // The textured scene: the sky is noise only.
    {
        MoonSceneSpec s;
        s.blurSigma = 1.5;
        s.noiseStd = 0.005;
        MoonScene sc = MakeMoonScene(s);
        const stack::LunarMasks m = stack::SegmentLunar(sc.observed, 4);
        const double n0 = StdOver(testing::Lum(sc.observed), sc.skyTruth);
        stack::SharpenLuminance(sc.observed, m, 4);
        const double n1 = StdOver(testing::Lum(sc.observed), sc.skyTruth);
        INFO("sky noise std " << n0 << " -> " << n1);
        CHECK(n1 <= 1.5 * n0);
    }
    // A flat disc (no texture, no maria, no tint structure) plus noise: every layer's content is noise.
    {
        MoonSceneSpec s;
        s.textureAmplitude = 0;
        s.mariaFraction = 0;
        s.tintVariation = 0;
        s.blurSigma = 1.5;
        s.noiseStd = 0.005;
        MoonScene sc = MakeMoonScene(s);
        const stack::LunarMasks m = stack::SegmentLunar(sc.observed, 4);
        const double n0 = StdOver(testing::Lum(sc.observed), sc.diskTruth);
        stack::SharpenLuminance(sc.observed, m, 4);
        const double n1 = StdOver(testing::Lum(sc.observed), sc.diskTruth);
        INFO("flat disc noise std " << n0 << " -> " << n1);
        CHECK(n1 <= 1.5 * n0);
    }
}

TEST_CASE("sharpen: no limb halo") {
    MoonSceneSpec s;
    s.blurSigma = 1.5;
    s.noiseStd = 0.002;
    MoonScene sc = MakeMoonScene(s);
    const stack::LunarMasks m = stack::SegmentLunar(sc.observed, 4);
    const std::vector<double> before = testing::Lum(sc.observed);
    stack::SharpenLuminance(sc.observed, m, 4);
    const std::vector<double> after = testing::Lum(sc.observed);
    double diskMean = 0;
    size_t n = 0;
    for (size_t i = 0; i < after.size(); ++i)
        if (sc.diskTruth[i]) {
            diskMean += after[i];
            ++n;
        }
    diskMean /= n;
    const std::vector<double> p0 = RadialProfile(before, s, s.radius, s.radius + 20),
                              p1 = RadialProfile(after, s, s.radius, s.radius + 20);
    double sky = 0;
    for (size_t b = 20; b < p1.size(); ++b) sky += p1[b];  // r >= R + 10
    sky /= static_cast<double>(p1.size() - 20);
    double under = 0, over = 0;
    for (size_t b = 0; b < p1.size(); ++b) {
        under = std::max(under, sky - p1[b]);
        over = std::max(over, p1[b] - p0[b]);
    }
    INFO("disk mean " << diskMean << "; sky-side undershoot " << under << ", brightening " << over);
    CHECK(under <= 0.02 * diskMean);
    CHECK(over <= 0.02 * diskMean);
}

TEST_CASE("sharpen: no bright rim inside the limb") {
    // A flat disc: its true profile inside the limb is flat, so any rise there is ringing from the limb edge.
    MoonSceneSpec s;
    s.textureAmplitude = 0;
    s.mariaFraction = 0;
    s.tintVariation = 0;
    s.blurSigma = 1.5;
    s.noiseStd = 0.002;
    MoonScene sc = MakeMoonScene(s);
    const stack::LunarMasks m = stack::SegmentLunar(sc.observed, 4);
    const std::vector<double> before = testing::Lum(sc.observed);
    stack::SharpenLuminance(sc.observed, m, 4);
    const std::vector<double> after = testing::Lum(sc.observed);
    const std::vector<double> p0 = RadialProfile(before, s, s.radius - 60, s.radius),
                              p1 = RadialProfile(after, s, s.radius - 60, s.radius);
    double level = 0;
    for (size_t b = 0; b < 40; ++b) level += p0[b];  // R - 60 .. R - 40
    level /= 40;
    // Above the flat level anywhere inside the limb (a bright rim); below it clear of the blurred edge, which
    // is r <= R - 4 sigma (a dark ring).
    double rim = 0, ring = 0;
    for (size_t b = 0; b < p1.size(); ++b) {
        rim = std::max(rim, p1[b] - level);
        if (s.radius - 60 + 0.5 * b <= s.radius - 4 * s.blurSigma) ring = std::max(ring, level - p1[b]);
    }
    INFO("disc level " << level << "; bright rim " << rim << ", dark ring " << ring);
    CHECK(rim <= 0.02 * level);
    CHECK(ring <= 0.02 * level);
}

TEST_CASE("sharpen: chroma untouched") {
    MoonSceneSpec s;
    s.blurSigma = 1.5;
    s.noiseStd = 0.005;
    MoonScene sc = MakeMoonScene(s);
    const stack::LunarMasks m = stack::SegmentLunar(sc.observed, 4);
    const ser::FloatImage before = sc.observed;
    stack::SharpenLuminance(sc.observed, m, 4);
    double worst = 0;
    size_t judged = 0;
    for (size_t i = 0; i < m.disk.size(); ++i) {
        const float* a = &before.data[i * 3];
        const float* b = &sc.observed.data[i * 3];
        if (a[1] < 0.05 || b[1] < 0.05) continue;
        ++judged;
        worst = std::max({worst, static_cast<double>(std::fabs(b[0] / b[1] - a[0] / a[1])),
                          static_cast<double>(std::fabs(b[2] / b[1] - a[2] / a[1]))});
    }
    INFO("judged " << judged << " px; worst ratio change " << worst);
    REQUIRE(judged > 10000);
    CHECK(worst <= 1e-4);
    CHECK(sc.observed.data != before.data);  // it did sharpen
}

// ---- Step 5: chroma layer (Task B5, amended: Gaussian chroma blur) ----

namespace {

// RMS over the mask of the R/G and B/G differences between a and b.
double ChromaError(const ser::FloatImage& a, const ser::FloatImage& b, const std::vector<uint8_t>& mask) {
    double s = 0;
    size_t n = 0;
    for (size_t i = 0; i < mask.size(); ++i)
        if (mask[i]) {
            const float* p = &a.data[i * 3];
            const float* q = &b.data[i * 3];
            const double dr = p[0] / p[1] - q[0] / q[1], db = p[2] / p[1] - q[2] / q[1];
            s += 0.5 * (dr * dr + db * db);
            ++n;
        }
    return std::sqrt(s / n);
}

// Mean of B/R over the mask.
double MeanBR(const ser::FloatImage& im, const std::vector<uint8_t>& mask) {
    double s = 0;
    size_t n = 0;
    for (size_t i = 0; i < mask.size(); ++i)
        if (mask[i]) {
            s += im.data[i * 3 + 2] / im.data[i * 3];
            ++n;
        }
    return s / n;
}

// edge_chroma.py's measure (Rulings R8, R12): over rows [r0, r1), the std of B/G and R/G on disc pixels
// (G >= 0.05) in columns [e0, e1) over the same in [f0, f1); returns the larger of the two ratios.
double EdgeChromaRatio(const ser::FloatImage& im, int r0, int r1, int e0, int e1, int f0, int f1) {
    auto stdOf = [&](int c, int x0, int x1) {
        double s = 0, s2 = 0;
        size_t n = 0;
        for (int y = r0; y < r1; ++y)
            for (int x = x0; x < x1; ++x) {
                const float* p = &im.data[(static_cast<size_t>(y) * im.width + x) * 3];
                if (p[1] < 0.05f) continue;
                const double v = p[c] / p[1];
                s += v;
                s2 += v * v;
                ++n;
            }
        return std::sqrt(std::max(0.0, s2 / n - (s / n) * (s / n)));
    };
    return std::max(stdOf(2, e0, e1) / stdOf(2, f0, f1), stdOf(0, e0, e1) / stdOf(0, f0, f1));
}

// The B5 edge scene: a 1080p frame, the disc over the left edge from row 180 to 900 (and so over rows
// 320-760, edge_chroma.py's default band), chroma noise 0.03 in columns 0-5. Highland terrain with its own
// 1% tint structure: with maria and highlands both in the reference band (x 20-40) the true colour contrast
// (7-9%) set its std and the unprocessed edge already read 1.40 -- a test that cannot fail.
MoonSceneSpec EdgeScene() {
    MoonSceneSpec s;
    s.width = 1920;
    s.height = 1080;
    s.cx = 600;
    s.cy = 540;
    s.radius = 700;
    s.mariaFraction = 0;
    s.tintVariation = 0.01;
    s.blurSigma = 1.0;
    s.noiseStd = 0.002;
    s.edgeChromaNoise = 0.03;
    return s;
}

ser::FloatImage LumImage(const ser::FloatImage& rgb) {
    ser::FloatImage L{rgb.width, rgb.height, 1, {}};
    for (double v : testing::Lum(rgb)) L.data.push_back(static_cast<float>(v));
    return L;
}

}  // namespace

TEST_CASE("chroma: error halved") {
    MoonSceneSpec s;
    s.noiseStd = 0.01;  // truth plus per-channel noise
    const MoonScene sc = MakeMoonScene(s);
    const ser::FloatImage C = stack::ChromaLayer(sc.observed, 2.5, 4);
    const double e0 = ChromaError(sc.observed, sc.truth, sc.diskTruth), e1 = ChromaError(C, sc.truth, sc.diskTruth);
    INFO("chroma error vs truth: " << e0 << " -> " << e1);
    CHECK(e1 <= 0.5 * e0);
}

TEST_CASE("chroma: luminance kept") {
    // Chroma comes from the blurred layer, luminance from L: at the module's settings (k 8, eps, clamp, fade),
    // the output's luminance is still L's (Ruling R26: a neutral pixel gets d = 0, so the colour step neither
    // darkens nor brightens).
    MoonSceneSpec s;
    s.noiseStd = 0.01;
    MoonScene sc = MakeMoonScene(s);
    const std::vector<double> truth = testing::Lum(sc.truth), before = testing::Lum(sc.observed);
    const ser::FloatImage L = LumImage(sc.observed);
    stack::MineralColour(sc.observed, L, stack::MineralParams{}, 4);
    const double e0 = RmsOver(before, truth, sc.diskTruth), e1 = RmsOver(testing::Lum(sc.observed), truth, sc.diskTruth);
    INFO("luminance RMS error vs truth: " << e0 << " -> " << e1);
    CHECK(std::fabs(e1 / e0 - 1) <= 0.02);
}

TEST_CASE("chroma: edge band") {
    const MoonScene sc = MakeMoonScene(EdgeScene());
    const double r0 = EdgeChromaRatio(sc.observed, 320, 760, 0, 6, 20, 40);
    REQUIRE(r0 > 1.5);  // the noise is there to remove
    const ser::FloatImage C = stack::ChromaLayer(sc.observed, 2.5, 8);
    const double r1 = EdgeChromaRatio(C, 320, 760, 0, 6, 20, 40);
    INFO("edge / ref chroma std: " << r0 << " -> " << r1);
    CHECK(r1 <= 1.5);
}

TEST_CASE("chroma: maria/highland tint survives") {
    MoonSceneSpec s;
    s.noiseStd = 0.005;
    s.blurSigma = 1.0;
    const MoonScene sc = MakeMoonScene(s);
    const ser::FloatImage C = stack::ChromaLayer(sc.observed, 2.5, 4);
    const double d0 = MeanBR(sc.observed, sc.mariaTruth) - MeanBR(sc.observed, sc.highlandTruth);
    const double d1 = MeanBR(C, sc.mariaTruth) - MeanBR(C, sc.highlandTruth);
    INFO("maria B/R - highland B/R: " << d0 << " -> " << d1);
    REQUIRE(d0 > 0.05);
    CHECK(d1 >= 0.9 * d0);
}

// ---- Steps 6-7: mineral colour (amendment: out = clip(L (1 + k w d))) ----

namespace {

// Type-7 (linear interpolation) percentile of channel c.
double Percentile(const ser::FloatImage& im, int c, double q) {
    std::vector<float> v;
    for (size_t i = 0; i < im.data.size() / 3; ++i) v.push_back(im.data[i * 3 + c]);
    std::sort(v.begin(), v.end());
    const double h = (v.size() - 1) * q;
    const size_t lo = static_cast<size_t>(std::floor(h));
    return v[lo] + (h - lo) * (v[std::min(lo + 1, v.size() - 1)] - v[lo]);
}

// A display-level scene: the disc's median luminance near 0.35 (linear, like a stretched image).
MoonScene DisplayScene(double level = 0.45) {
    MoonSceneSpec s;
    s.level = level;
    s.blurSigma = 1.0;
    s.noiseStd = 0.003;
    return MakeMoonScene(s);
}

// out / L - 1 per sample: k w d.
double Chroma(const ser::FloatImage& out, const ser::FloatImage& L, size_t i, int c) {
    return out.data[i * 3 + c] / L.data[i] - 1;
}

}  // namespace

TEST_CASE("colour: hue preserved") {
    MoonScene sc = DisplayScene();
    const ser::FloatImage L = LumImage(sc.observed);
    ser::FloatImage out = sc.observed;
    const stack::StepRecord rec = stack::MineralColour(out, L, stack::MineralParams{}, 4);
    INFO(rec.history);
    CHECK(Ascii(rec.history));
    for (const std::vector<uint8_t>* mask : {&sc.mariaTruth, &sc.highlandTruth}) {
        double din[3] = {0, 0, 0}, dout[3] = {0, 0, 0};
        size_t n = 0;
        for (size_t i = 0; i < mask->size(); ++i)
            if ((*mask)[i]) {
                const float* a = &sc.observed.data[i * 3];
                const float* b = &out.data[i * 3];
                const double ma = (a[0] + a[1] + a[2]) / 3, mb = (b[0] + b[1] + b[2]) / 3;
                for (int c = 0; c < 3; ++c) {
                    din[c] += a[c] / ma - 1;
                    dout[c] += b[c] / mb - 1;
                }
                ++n;
            }
        INFO((mask == &sc.mariaTruth ? "maria" : "highlands") << ": d in R " << din[0] / n << " B " << din[2] / n
                                                             << ", out R " << dout[0] / n << " B " << dout[2] / n);
        for (int c : {0, 2}) {
            CHECK((din[c] > 0) == (dout[c] > 0));
            CHECK(std::fabs(dout[c]) > std::fabs(din[c]));  // and amplified (less on bright highlands: the fade)
        }
    }
}

TEST_CASE("colour: strength scales") {
    MoonScene sc = DisplayScene();
    const ser::FloatImage L = LumImage(sc.observed);
    stack::MineralParams p4, p8;
    p4.strength = 4;
    ser::FloatImage o4 = sc.observed, o8 = sc.observed;
    stack::MineralColour(o4, L, p4, 4);
    stack::MineralColour(o8, L, p8, 4);
    const double thr = stack::AutoSatThreshold(sc.observed);
    const ser::FloatImage C = stack::ChromaLayer(sc.observed, p8.chromaBlurSigma, 4);
    double worst = 0;
    size_t judged = 0;
    for (size_t i = 0; i < L.data.size(); ++i) {
        const float* q = &C.data[i * 3];
        const double m = (q[0] + q[1] + q[2]) / 3;
        double dmax = 0, dabs = 0;
        bool clamped = false;
        for (int c = 0; c < 3; ++c) {
            const double d = (q[c] - m) / (m + p8.eps);
            clamped = clamped || std::fabs(d) >= 0.95 * p8.devClamp;
            dmax = std::max(dmax, d);
            dabs = std::max(dabs, std::fabs(d));
        }
        const bool faded = std::max(q[0], q[1]) > thr - p8.satRamp;
        const bool gamut = L.data[i] * (1 + 8 * dmax) > 0.99;
        if (clamped || faded || gamut || dabs < 0.005 || L.data[i] < 0.05) continue;
        ++judged;
        for (int c = 0; c < 3; ++c) {
            const double a = Chroma(o4, L, i, c), b = Chroma(o8, L, i, c);
            if (std::fabs(a) > 1e-3) worst = std::max(worst, std::fabs(b / a / 2 - 1));
        }
    }
    INFO("judged " << judged << " px; worst |chroma(8) / (2 chroma(4)) - 1| " << worst);
    REQUIRE(judged > 10000);
    CHECK(worst <= 0.05);
}

TEST_CASE("colour: fade at highlights") {
    MoonSceneSpec s;
    s.level = 0.45;
    s.blurSigma = 1.0;
    s.noiseStd = 0.003;
    MoonScene sc = MakeMoonScene(s);
    // A saturated highland: a 40 x 40 patch on the disc at 1.0 in every channel (scattered saturated pixels
    // never reach the threshold through the 2.5 px chroma blur).
    for (int y = 220; y < 260; ++y)
        for (int x = 300; x < 340; ++x)
            for (int c = 0; c < 3; ++c) sc.observed.data[(static_cast<size_t>(y) * s.width + x) * 3 + c] = 1.0f;
    const ser::FloatImage L = LumImage(sc.observed);
    ser::FloatImage out = sc.observed;
    stack::MineralParams p;
    stack::MineralColour(out, L, p, 4);
    const double thr = stack::AutoSatThreshold(sc.observed);
    const ser::FloatImage C = stack::ChromaLayer(sc.observed, p.chromaBlurSigma, 4);
    size_t judged = 0;
    double worst = 0;
    for (size_t i = 0; i < L.data.size(); ++i) {
        if (std::max(C.data[i * 3], C.data[i * 3 + 1]) < thr) continue;
        ++judged;
        const float* o = &out.data[i * 3];
        worst = std::max({worst, static_cast<double>(std::fabs(o[0] - o[1])), static_cast<double>(std::fabs(o[2] - o[1]))});
    }
    INFO("threshold " << thr << "; " << judged << " px at or above it; worst channel spread " << worst);
    REQUIRE(judged > 100);
    CHECK(worst <= 1e-6);
}

TEST_CASE("colour: the deviation clamp holds") {
    // A single 1.0 blue pixel on a grey field, chroma blur at its smallest (0.5 px) so the pixel's B deviation
    // far exceeds the clamp; no fade.
    ser::FloatImage rgb{64, 64, 3, std::vector<float>(64 * 64 * 3, 0.1f)};
    const size_t centre = 32 * 64 + 32;
    rgb.data[centre * 3 + 2] = 1.0f;
    const ser::FloatImage L = LumImage(rgb);
    stack::MineralParams p;
    p.chromaBlurSigma = 0.5;
    p.satThreshold = 2.0;
    ser::FloatImage out = rgb;
    stack::MineralColour(out, L, p, 1);
    double worst = 0;
    for (size_t i = 0; i < L.data.size(); ++i)
        for (int c = 0; c < 3; ++c) worst = std::max(worst, std::fabs(Chroma(out, L, i, c)) / p.strength);
    const double atPixel = Chroma(out, L, centre, 2) / p.strength;
    INFO("largest |d| " << worst << ", the blue pixel's d_B " << atPixel);
    CHECK(worst <= 0.06 + 1e-6);
    CHECK(atPixel >= 0.06 - 1e-6);  // the clamp engaged
}

TEST_CASE("colour: range") {
    MoonSceneSpec s;
    s.level = 0.7;
    s.blurSigma = 1.0;
    s.noiseStd = 0.01;
    s.clipFraction = 0.02;
    s.clipChannels = {false, false, true};
    MoonScene sc = MakeMoonScene(s);
    const ser::FloatImage L = LumImage(sc.observed);
    stack::MineralParams p;
    p.strength = 15;
    p.chromaBlurSigma = 0.5;
    stack::MineralColour(sc.observed, L, p, 4);
    float lo = 1, hi = 0;
    for (float v : sc.observed.data) {
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    INFO("output range " << lo << " .. " << hi);
    CHECK(lo >= 0.0f);
    CHECK(hi <= 1.0f);
}

TEST_CASE("colour: no new clipping at full strength") {
    // Bright terrain at k = 15: L (1 + k d) would pass 1 on the brightest positive-d pixels the fade leaves.
    MoonScene sc = DisplayScene(0.6);
    const ser::FloatImage L = LumImage(sc.observed);
    size_t before = 0, after = 0;
    for (float v : sc.observed.data) before += v >= 0.999f;
    stack::MineralParams p;
    p.strength = 15;
    stack::MineralColour(sc.observed, L, p, 4);
    for (float v : sc.observed.data) after += v >= 0.999f;
    INFO("samples >= 0.999: " << before << " -> " << after);
    CHECK(after <= before);
}

TEST_CASE("colour: auto saturation threshold") {
    MoonScene sc = DisplayScene();
    const double expect = 0.92 * std::min({Percentile(sc.observed, 0, 0.9999), Percentile(sc.observed, 1, 0.9999),
                                           Percentile(sc.observed, 2, 0.9999)});
    const double got = stack::AutoSatThreshold(sc.observed);
    INFO("auto threshold " << got << ", expected " << expect);
    CHECK(std::fabs(got - expect) <= 1e-6);
}

// ---- Step 8 and the pipeline (Task B7, amended order) ----

namespace {

// A stack-like input: linear, cast, pedestal, gradient, seeing, noise.
MoonSceneSpec StackScene() {
    MoonSceneSpec s;
    s.level = 0.25;
    s.cast = {1.15, 1.0, 0.85};
    s.pedestal = {0.012, 0.010, 0.014};
    s.gradientX = 0.006;
    s.blurSigma = 1.2;
    s.noiseStd = 0.003;
    return s;
}

double DiskMedianLum(const ser::FloatImage& im, const std::vector<uint8_t>& disk) {
    std::vector<double> v;
    const std::vector<double> L = testing::Lum(im);
    for (size_t i = 0; i < disk.size(); ++i)
        if (disk[i]) v.push_back(L[i]);
    std::nth_element(v.begin(), v.begin() + static_cast<long>(v.size() / 2), v.end());
    return v[v.size() / 2];
}

size_t ClippedSamples(const ser::FloatImage& im) {
    size_t n = 0;
    for (float v : im.data) n += v >= 0.999f;
    return n;
}

void CheckValidOutput(const stack::MineralOutcome& out, const ser::FloatImage& in) {
    REQUIRE(out.image.width == in.width);
    REQUIRE(out.image.height == in.height);
    REQUIRE(out.image.channels == 3);
    REQUIRE(out.image.data.size() == in.data.size());
    size_t bad = 0;
    for (float v : out.image.data) bad += !(std::isfinite(v) && v >= 0.0f && v <= 1.0f);
    CHECK(bad == 0);  // every sample finite and in [0, 1]
}

}  // namespace

TEST_CASE("pipeline: display ready") {
    const MoonScene sc = MakeMoonScene(StackScene());
    const stack::MineralOutcome out = stack::MakeMineralMoon(sc.observed, 4, {});
    CheckValidOutput(out, sc.observed);
    const double med = DiskMedianLum(out.image, sc.diskTruth);
    INFO("input disk median " << DiskMedianLum(sc.observed, sc.diskTruth) << ", output " << med);
    CHECK(med >= 0.25);
    CHECK(med <= 0.45);
}

TEST_CASE("pipeline: no new clipping") {
    MoonSceneSpec s = StackScene();
    s.level = 0.7;          // bright: the highlands near full scale
    s.clipFraction = 0.005;  // and some saturated in the stack
    const MoonScene sc = MakeMoonScene(s);
    const size_t before = ClippedSamples(sc.observed);
    REQUIRE(before > 0);
    for (double k : {8.0, 15.0}) {
        stack::MineralParams p;
        p.strength = k;
        const stack::MineralOutcome out = stack::MakeMineralMoon(sc.observed, 4, {}, p);
        CheckValidOutput(out, sc.observed);
        const size_t after = ClippedSamples(out.image);
        INFO("k " << k << ": samples >= 0.999 " << before << " -> " << after);
        CHECK(after <= before);
    }
}

TEST_CASE("pipeline: edge chroma survives the pipeline") {
    MoonSceneSpec s = EdgeScene();
    s.level = 0.25;
    s.cast = {1.15, 1.0, 0.85};
    s.pedestal = {0.01, 0.01, 0.01};
    const MoonScene sc = MakeMoonScene(s);
    const stack::MineralOutcome out = stack::MakeMineralMoon(sc.observed, 8, {});
    CheckValidOutput(out, sc.observed);
    const double r = EdgeChromaRatio(out.image, 320, 760, 0, 6, 20, 40);
    INFO("edge / ref chroma std on the mineral output: " << r);
    CHECK(r <= 1.5);
}

TEST_CASE("pipeline: small disc completes") {
    MoonSceneSpec s = StackScene();
    s.radius = 60;
    const MoonScene sc = MakeMoonScene(s);
    stack::MineralOutcome out;
    REQUIRE_NOTHROW(out = stack::MakeMineralMoon(sc.observed, 4, {}));
    CheckValidOutput(out, sc.observed);
    CHECK(out.history.size() == 7);
}

namespace {

// The sharpening's noise gain in the shipped order (after the display transfer), on the disc's interior
// (r <= R - 40, lit): PrepareMineral on two noise realisations of the same scene; the std of the difference of
// their sharp L over the std of the difference of their display L before sharpening. (Against the noise-free
// scene instead, the difference would also hold the texture times the gain the Wiener factor rightly withholds
// from the noisy one: on the textured disc at noise 0.002 / 0.005 that read x2.00 / x1.75, this x1.39 / x1.22.)
double PipelineNoiseGain(const MoonSceneSpec& spec) {
    MoonSceneSpec sa = spec, sb = spec;
    sa.noiseSeed = 101;
    sb.noiseSeed = 202;
    const MoonScene a = MakeMoonScene(sa), b = MakeMoonScene(sb);
    const stack::MineralCache ca = stack::PrepareMineral(a.observed, 4, {}), cb = stack::PrepareMineral(b.observed, 4, {});
    const std::vector<double> la = testing::Lum(ca.display), lb = testing::Lum(cb.display);
    std::vector<double> before(la.size()), after(la.size());
    std::vector<uint8_t> interior(la.size(), 0);
    for (int y = 0; y < spec.height; ++y)
        for (int x = 0; x < spec.width; ++x) {
            const size_t i = static_cast<size_t>(y) * spec.width + x;
            before[i] = la[i] - lb[i];
            after[i] = static_cast<double>(ca.lum.data[i]) - cb.lum.data[i];
            interior[i] = a.diskTruth[i] && std::hypot(x - spec.cx, y - spec.cy) <= spec.radius - 40;
        }
    return StdOver(after, interior) / StdOver(before, interior);
}

}  // namespace

TEST_CASE("pipeline: sharpening is noise-limited in the shipped order") {
    // Task ENG review I1: measured after the display transfer, the noise read ~7x low and noise was boosted
    // x1.53-1.57 on the disc. Spec step 4: a noise-only gain fails (<= 1.5x); the flat disc is noise only.
    for (bool flat : {false, true})
        for (double noise : {0.002, 0.005}) {
            MoonSceneSpec s = StackScene();
            s.noiseStd = noise;
            if (flat) {
                s.textureAmplitude = 0;
                s.mariaFraction = 0;
                s.tintVariation = 0;
            }
            const double g = PipelineNoiseGain(s);
            INFO((flat ? "flat disc" : "textured disc") << ", noise " << noise << ": disc-interior noise gain " << g);
            CHECK(g <= 1.5);
        }
}

TEST_CASE("pipeline: a small disc in a big frame is found, not the whole frame") {
    // A 1080p frame with a 20 px disc (0.06% of it; task ENG review I2: it read "disk 100%", the sky was grey).
    MoonSceneSpec s = StackScene();
    s.width = 1920;
    s.height = 1080;
    s.cx = 960;
    s.cy = 540;
    s.radius = 20;
    const MoonScene sc = MakeMoonScene(s);
    const size_t n = static_cast<size_t>(s.width) * s.height;
    const stack::LunarMasks m = stack::SegmentLunar(sc.observed, 8);
    INFO("disk " << m.diskCount << " px, sky " << m.skyCount << " px; diskTruth in disk " << Within(sc.diskTruth, m.disk)
                 << ", skyTruth in disk " << Within(sc.skyTruth, m.disk));
    CHECK(m.diskCount < n / 100);
    CHECK(Within(sc.diskTruth, m.disk) >= 0.98);
    CHECK(Within(sc.skyTruth, m.disk) <= 0.005);
    CHECK(Within(sc.skyTruth, m.sky) >= 0.95);
    const stack::MineralOutcome out = stack::MakeMineralMoon(sc.observed, 8, {});
    CheckValidOutput(out, sc.observed);
    REQUIRE(out.history.size() == 7);
    INFO(out.history[0] << "\n" << out.history[1] << "\n" << out.history[2]);
    CHECK(out.history[0].find("100.00%") == std::string::npos);
    CHECK(out.history[1].find("skipped") != std::string::npos);  // the disc is under the 1% a white reference needs
    CHECK(out.history[2].find("skipped") == std::string::npos);  // the sky is measured and removed
    CHECK(out.notice.empty());
    std::vector<double> sky;
    const std::vector<double> L = testing::Lum(out.image);
    for (size_t i = 0; i < n; ++i)
        if (sc.skyTruth[i]) sky.push_back(L[i]);
    std::nth_element(sky.begin(), sky.begin() + static_cast<long>(sky.size() / 2), sky.end());
    INFO("output sky median " << sky[sky.size() / 2]);
    CHECK(sky[sky.size() / 2] <= 0.02);
}

TEST_CASE("pipeline: a frame of sky has no lunar disk") {
    // The stack scene with no Moon in it: sky pedestal and noise only. (A gradient over 5 noise sigma of the 3 x 3
    // mean is a stated limit of SegmentLunar: it reads as a disc filling the frame, and HISTORY says "100%".)
    MoonSceneSpec s = StackScene();
    s.level = 0;
    s.gradientX = 0;
    const MoonScene sc = MakeMoonScene(s);
    const stack::LunarMasks m = stack::SegmentLunar(sc.observed, 4);
    INFO("disk " << m.diskCount << " px");
    CHECK(m.diskCount == 0);
    const stack::MineralOutcome out = stack::MakeMineralMoon(sc.observed, 4, {});
    CheckValidOutput(out, sc.observed);
    REQUIRE(out.history.size() == 7);
    for (const std::string& h : out.history) {
        INFO(h);
        CHECK(h.find("no lunar disk distinguished from the sky") != std::string::npos);
        CHECK(Ascii(h));
    }
    CHECK(out.notice.find("no lunar disk distinguished from the sky") != std::string::npos);
}

TEST_CASE("pipeline: NaN in the stack is refused, Inf is not") {
    const MoonScene sc = MakeMoonScene(StackScene());
    ser::FloatImage nan1 = sc.observed, nan3 = sc.observed, inf1 = sc.observed;
    nan1.data[(240 * 640 + 320) * 3 + 1] = std::numeric_limits<float>::quiet_NaN();
    for (size_t k : {size_t(7), size_t(100000), size_t(500001)}) nan3.data[k] = std::numeric_limits<float>::quiet_NaN();
    inf1.data[(240 * 640 + 320) * 3 + 1] = std::numeric_limits<float>::infinity();
    CHECK_THROWS_MATCHES(stack::PrepareMineral(nan1, 4, {}), std::invalid_argument,
                         Catch::Matchers::MessageMatches(Catch::Matchers::ContainsSubstring("1 NaN sample")));
    CHECK_THROWS_MATCHES(stack::MakeMineralMoon(nan3, 4, {}), std::invalid_argument,
                         Catch::Matchers::MessageMatches(Catch::Matchers::ContainsSubstring("3 NaN sample")));
    stack::MineralOutcome out;
    REQUIRE_NOTHROW(out = stack::MakeMineralMoon(inf1, 4, {}));
    CheckValidOutput(out, inf1);
    CHECK(out.notice.empty());
}

TEST_CASE("pipeline: cancel throws StackCancelled") {
    const MoonScene sc = MakeMoonScene(StackScene());
    bool seen = false;
    stack::Progress pr;
    pr.report = [&](const std::string& phase, int64_t, int64_t) { seen = seen || phase == stack::kPhaseMineral; };
    pr.cancelled = [&] { return seen; };
    stack::MineralOutcome out;
    CHECK_THROWS_AS(out = stack::MakeMineralMoon(sc.observed, 4, pr), stack::StackCancelled);
    CHECK(seen);
    CHECK(out.image.data.empty());
    CHECK_THROWS_AS(stack::PrepareMineral(sc.observed, 4, pr), stack::StackCancelled);
}

TEST_CASE("pipeline: mono refused") {
    ser::FloatImage mono{64, 48, 1, std::vector<float>(64 * 48, 0.3f)};
    CHECK_THROWS_AS(stack::MakeMineralMoon(mono, 2, {}), std::invalid_argument);
    CHECK_THROWS_AS(stack::PrepareMineral(mono, 2, {}), std::invalid_argument);
}

TEST_CASE("pipeline: history order") {
    const MoonScene sc = MakeMoonScene(StackScene());
    const stack::MineralOutcome out = stack::MakeMineralMoon(sc.observed, 4, {});
    // Amendment order: Segment, Calibrate, Background, Display transfer, AlignChannels, SharpenLuminance,
    // MineralColour -- seven lines (B7's eight were for the pre-amendment steps).
    const std::vector<std::string> prefix = {"Lunar disk", "Colour calibration", "Background", "Display transfer",
                                             "Channel alignment", "Sharpen", "Mineral colour"};
    REQUIRE(out.history.size() == prefix.size());
    REQUIRE(out.stepSeconds.size() == prefix.size());
    for (size_t k = 0; k < prefix.size(); ++k) {
        INFO(out.history[k]);
        CHECK(out.history[k].rfind(prefix[k], 0) == 0);
        CHECK(Ascii(out.history[k]));
        CHECK(out.stepSeconds[k].second >= 0);
    }
}

TEST_CASE("pipeline: re-running the colour on the cache") {
    // Increment 2 / B8's sliders: PrepareMineral once, ApplyMineralColour per setting; the same result as
    // MakeMineralMoon with that setting, and the luminance detail does not move with k.
    const MoonScene sc = MakeMoonScene(StackScene());
    const stack::MineralCache cache = stack::PrepareMineral(sc.observed, 4, {});
    stack::MineralParams p5;
    p5.strength = 5;
    p5.chromaBlurSigma = 3.5;
    stack::StepRecord rec;
    const ser::FloatImage a = stack::ApplyMineralColour(cache, p5, 4, &rec);
    const stack::MineralOutcome full = stack::MakeMineralMoon(sc.observed, 4, {}, p5);
    CHECK(a.data == full.image.data);
    CHECK(rec.history == full.history.back());
    CHECK(cache.history.size() == 6);
    const ser::FloatImage b = stack::ApplyMineralColour(cache, stack::MineralParams{}, 4);
    CHECK(a.data != b.data);
}

TEST_CASE("pipeline: 1080p within the time budget") {
    // Spec: the mineral steps add <= 5 s to a 1920 x 1080 stack on 32 threads (CPU only).
    MoonSceneSpec s = StackScene();
    s.width = 1920;
    s.height = 1080;
    s.cx = 960;
    s.cy = 540;
    s.radius = 700;
    const MoonScene sc = MakeMoonScene(s);
    const int threads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    const auto t0 = std::chrono::steady_clock::now();
    const stack::MineralOutcome out = stack::MakeMineralMoon(sc.observed, threads, {});
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::string steps;
    for (const auto& st : out.stepSeconds) steps += st.first + " " + std::to_string(st.second) + " s; ";
    INFO(threads << " threads: " << secs << " s (" << steps << ")");
    CheckValidOutput(out, sc.observed);
    if (threads >= 32) CHECK(secs <= 5.0);
    else WARN("fewer than 32 threads here: " << secs << " s, not judged");
}
