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
