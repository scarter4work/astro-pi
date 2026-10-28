// The mineral-moon engine (spec Part B, steps 1-8 and the 2026-10-27 amendment): each step against its
// measured requirement on synthetic lunar scenes whose truth is known (MoonSynth.h).
#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
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
