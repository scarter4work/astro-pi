// Planet processing (spec 2026-10-29-planetary-planet-processing-design.md, call 3 and Ruling P1; plan Task 2):
// the catalogue colour calibration, the ring's place in the disc mask, small-disc dispersion alignment (Task 2), the
// sharpen profile (Task 3), and the pipeline -- PreparePlanet / FinishPlanet, the linear display and the saturation
// (Task 4) -- each against its requirement on synthetic planets whose truth is known (PlanetSynth.h).
#include <catch_amalgamated.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "Measure.h"
#include "Planet.h"
#include "PlanetSynth.h"

using namespace planetary;
using planetary::testing::MakePlanetScene;
using planetary::testing::PlanetScene;
using planetary::testing::PlanetSceneSpec;

namespace {

bool Contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

// A frame for a disc of radius r: the acceptance ROIs hold the disc with room around it (320 x 240 for Saturn).
PlanetSceneSpec Framed(double r) {
    PlanetSceneSpec s;
    s.radius = r;
    s.width = std::max(64, static_cast<int>(std::lround(3.2 * r)));
    s.height = std::max(48, static_cast<int>(std::lround(2.4 * r)));
    s.cx = 0.5 * s.width + 0.3;
    s.cy = 0.5 * s.height - 0.2;
    return s;
}

// The disc's mean B/G and R/G in im less the known background (pedestal + gradient), over the mask's pixels with no
// sample at or above kClippedLuma in `input` (the stack, before any step): the quantity spec call 3 calibrates.
struct Ratios { double bg, rg; size_t used; };
Ratios DiscRatios(const ser::FloatImage& im, const stack::LunarMasks& m, const PlanetSceneSpec& s,
                  const ser::FloatImage& input) {
    double sum[3] = {0, 0, 0};
    size_t used = 0;
    for (int y = 0; y < im.height; ++y)
        for (int x = 0; x < im.width; ++x) {
            const size_t i = static_cast<size_t>(y) * im.width + x;
            if (!m.disk[i]) continue;
            const float* q = &input.data[i * 3];
            if (q[0] >= stack::kClippedLuma || q[1] >= stack::kClippedLuma || q[2] >= stack::kClippedLuma) continue;
            const float* p = &im.data[i * 3];
            for (int c = 0; c < 3; ++c)
                sum[c] += p[c] - (s.pedestal[c] + s.gradientX[c] * x / std::max(1, im.width - 1) +
                                  s.gradientY[c] * y / std::max(1, im.height - 1));
            ++used;
        }
    return {sum[2] / sum[1], sum[0] / sum[1], used};
}

// A stack's look: a camera cast, a pedestal and gradient per channel, seeing, noise.
PlanetSceneSpec Realistic(double r, unsigned seed) {
    PlanetSceneSpec s = Framed(r);
    s.seed = seed;
    s.tint = {1.15, 1.0, 0.85};
    s.cast = {1.25, 1.0, 0.70};
    s.pedestal = {0.030, 0.025, 0.020};
    s.gradientX = {0.010, 0.008, 0.006};
    s.gradientY = {-0.004, -0.003, -0.002};
    s.blurSigma = 1.2;
    s.noiseStd = 0.004;
    return s;
}

}  // namespace

// ---- Colour calibration (spec call 3) ----

TEST_CASE("planet reflectance: spec call 3's table, exactly") {
    using ser::Target;
    struct Row { Target t; double bg, rg; };
    const Row rows[] = {{Target::Mercury, 0.739, 1.113}, {Target::Venus, 0.955, 0.955}, {Target::Mars, 0.518, 1.471},
                        {Target::Jupiter, 0.823, 0.954}, {Target::Saturn, 0.679, 1.295}, {Target::Uranus, 1.150, 0.541},
                        {Target::Neptune, 1.271, 0.511}};
    for (const Row& r : rows) {
        const auto c = stack::PlanetReflectance(r.t);
        REQUIRE(c.has_value());
        CHECK(c->bOverG == r.bg);
        CHECK(c->rOverG == r.rg);
    }
    CHECK_FALSE(stack::PlanetReflectance(Target::Moon).has_value());
    CHECK_FALSE(stack::PlanetReflectance(Target::Other).has_value());
}

TEST_CASE("planet calibration hits the catalogue colour") {
    for (double r : {6.0, 10.0, 20.0, 100.0})
        for (ser::Target t : {ser::Target::Mars, ser::Target::Jupiter, ser::Target::Saturn}) {
            const PlanetSceneSpec s = Realistic(r, 11 + static_cast<unsigned>(r));
            const PlanetScene sc = MakePlanetScene(s);
            ser::FloatImage im = sc.observed;
            const stack::LunarMasks m = stack::SegmentLunar(im, 4);
            const Ratios before = DiscRatios(im, m, s, sc.observed);
            const stack::StepRecord rec = stack::CalibratePlanetColour(im, m, t);
            const Ratios after = DiscRatios(im, m, s, sc.observed);
            const stack::BodyColour want = *stack::PlanetReflectance(t);
            INFO("r " << r << " " << ser::TargetName(t) << ": disc " << m.diskCount << " px; B/G " << before.bg << " -> "
                      << after.bg << " (want " << want.bOverG << "), R/G " << before.rg << " -> " << after.rg << " (want "
                      << want.rOverG << "); " << rec.history);
            REQUIRE_FALSE(rec.skipped);
            CHECK(std::fabs(after.bg / want.bOverG - 1) <= 0.02);
            CHECK(std::fabs(after.rg / want.rOverG - 1) <= 0.02);
            // G is the reference: untouched.
            for (size_t i = 0; i < im.data.size(); i += 3) REQUIRE(im.data[i + 1] == sc.observed.data[i + 1]);
            // HISTORY names the source, the target, the approximation, and the before/after ratios.
            CHECK(Contains(rec.history, "Mallama, Krobusek & Pavlov 2017"));
            CHECK(Contains(rec.history, "Table 7"));
            CHECK(Contains(rec.history, ser::TargetName(t)));
            CHECK(Contains(rec.history, "Johnson B"));
            CHECK(Contains(rec.history, "Cousins Rc"));
            CHECK(Contains(rec.history, "before"));
            CHECK(Contains(rec.history, "after"));
        }
}

TEST_CASE("planet calibration floor: 30 unsaturated disc px") {
    // A hand-made disc of n pixels (a 10-wide block) in a 48 x 48 frame of sky, so the count is exact.
    auto run = [](int n, int clippedCount) {
        const int w = 48, h = 48;
        ser::FloatImage im{w, h, 3, std::vector<float>(static_cast<size_t>(w) * h * 3)};
        std::mt19937 rng(5);
        std::normal_distribution<double> nd(0, 0.002);
        stack::LunarMasks m;
        m.width = w;
        m.height = h;
        m.disk.assign(static_cast<size_t>(w) * h, 0);
        m.sky.assign(m.disk.size(), 0);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const size_t i = static_cast<size_t>(y) * w + x;
                for (int c = 0; c < 3; ++c) im.data[i * 3 + c] = static_cast<float>(0.02 + nd(rng));
                if (x >= 30 || y >= 30) m.sky[i] = 1;
            }
        for (int k = 0; k < n; ++k) {
            const size_t i = static_cast<size_t>(10 + k / 10) * w + 10 + k % 10;
            m.disk[i] = 1;
            const float v[3] = {0.40f, 0.30f, 0.15f};
            for (int c = 0; c < 3; ++c) im.data[i * 3 + c] = k < clippedCount ? 1.0f : v[c];
        }
        for (size_t i = 0; i < m.disk.size(); ++i) {
            m.diskCount += m.disk[i];
            m.skyCount += m.sky[i];
        }
        const ser::FloatImage before = im;
        const stack::StepRecord rec = stack::CalibratePlanetColour(im, m, ser::Target::Jupiter);
        return std::make_pair(rec, im.data == before.data);
    };
    {
        const auto [rec, unchanged] = run(20, 0);
        INFO(rec.history);
        CHECK(rec.skipped);
        CHECK(unchanged);
        CHECK(Contains(rec.history, "skipped"));
        CHECK(Contains(rec.history, "20"));
        CHECK(Contains(rec.history, "30"));
    }
    {
        const auto [rec, unchanged] = run(40, 0);
        INFO(rec.history);
        CHECK_FALSE(rec.skipped);
        CHECK_FALSE(unchanged);
    }
    {  // 60 disc pixels, 40 of them clipped: 20 unsaturated
        const auto [rec, unchanged] = run(60, 40);
        INFO(rec.history);
        CHECK(rec.skipped);
        CHECK(unchanged);
    }
}

TEST_CASE("planet calibration leaves clipped pixels out") {
    // R alone clipped on the brightest 15% of the disc: counted, R's mean would read high and the gain come out low.
    PlanetSceneSpec s = Realistic(20, 3);
    s.clipFraction = 0.15;
    s.clipChannels = {true, false, false};
    const PlanetScene sc = MakePlanetScene(s);
    ser::FloatImage im = sc.observed;
    const stack::LunarMasks m = stack::SegmentLunar(im, 4);
    size_t clippedInDisc = 0;
    for (size_t i = 0; i < m.disk.size(); ++i) clippedInDisc += m.disk[i] && sc.clipped[i];
    REQUIRE(clippedInDisc > 50);
    // Counted, the clipped pixels would move the disc's R/G by over 2%: the check below can tell the difference.
    const Ratios counted = DiscRatios(sc.observed, m, s, sc.clean), unclipped = DiscRatios(sc.observed, m, s, sc.observed);
    INFO("R/G with the clipped pixels " << counted.rg << ", without " << unclipped.rg);
    CHECK(std::fabs(counted.rg / unclipped.rg - 1) > 0.02);
    const stack::StepRecord rec = stack::CalibratePlanetColour(im, m, ser::Target::Saturn);
    const Ratios after = DiscRatios(im, m, s, sc.observed);
    const stack::BodyColour want = *stack::PlanetReflectance(ser::Target::Saturn);
    INFO(rec.history << "; B/G " << after.bg << " R/G " << after.rg << ", " << clippedInDisc << " clipped in the disc");
    REQUIRE_FALSE(rec.skipped);
    CHECK(std::fabs(after.bg / want.bOverG - 1) <= 0.02);
    CHECK(std::fabs(after.rg / want.rOverG - 1) <= 0.02);
    CHECK(Contains(rec.history, std::to_string(clippedInDisc) + " with a saturated sample left out"));
}

TEST_CASE("planet calibration skips without a catalogue colour or a disc") {
    const PlanetSceneSpec s = Realistic(10, 2);
    const PlanetScene sc = MakePlanetScene(s);
    ser::FloatImage im = sc.observed;
    const stack::LunarMasks m = stack::SegmentLunar(im, 2);
    for (ser::Target t : {ser::Target::Moon, ser::Target::Other}) {
        const stack::StepRecord rec = stack::CalibratePlanetColour(im, m, t);
        INFO(rec.history);
        CHECK(rec.skipped);
        CHECK(Contains(rec.history, "skipped"));
        CHECK(im.data == sc.observed.data);
    }
    stack::LunarMasks none = m;
    std::fill(none.disk.begin(), none.disk.end(), 0);
    none.diskCount = 0;
    none.reason = "a test's reason";
    const stack::StepRecord rec = stack::CalibratePlanetColour(im, none, ser::Target::Mars);
    INFO(rec.history);
    CHECK(rec.skipped);
    CHECK(Contains(rec.history, "a test's reason"));
    CHECK(im.data == sc.observed.data);
}

// ---- A small planet in a large frame of sky (Task 5: 2026-10-23 Neptune) ----

namespace {

// 2026-10-23 Neptune: a 1911 x 1071 stack of sky at 0.058 (noise 0.006, the rows rising 0.008 down the
// frame) holding a disc of about 3 px radius peaking at 0.85. SegmentLunar took the whole frame for the disc (100.00%,
// no sky): the gradient lifts the Otsu split of the sky's own histogram 5 sigma clear.
PlanetSceneSpec SmallInLargeSky(unsigned seed) {
    PlanetSceneSpec s;
    s.width = 1911;
    s.height = 1071;
    s.radius = 3;
    s.cx = 905.3;
    s.cy = 482.6;
    s.level = 0.9;
    s.tint = {0.55, 1.0, 1.25};
    s.limbDarkening = {0.3, 0.3, 0.3};
    s.bandAmplitude = 0;
    s.bandTint = 0;
    s.pedestal = {0.058, 0.058, 0.058};
    s.gradientY = {0.008, 0.008, 0.008};
    s.gradientX = {-0.003, -0.003, -0.003};
    s.blurSigma = 1.2;
    s.seed = seed;
    return s;
}

// The scene with a stack's noise: correlated (white noise blurred by 1.2 px, as the demosaic and the warp leave it),
// std 0.006 per sample. Neighbour differences read correlated noise low, which is how the Otsu split of the sky's
// own histogram passed SegmentLunar's 5-sigma test on the real frame; and the sky's pedestal (0.058) is above a
// quarter of that split's level, so no pixel was a sky candidate and the whole frame became disc.
ser::FloatImage WithStackNoise(ser::FloatImage im, unsigned seed) {
    ser::FloatImage nz{im.width, im.height, 3, std::vector<float>(im.data.size())};
    std::mt19937 rng(seed * 7919u + 13u);
    std::normal_distribution<float> g(0.0f, 1.0f);
    for (float& v : nz.data) v = g(rng);
    planetary::testing::moon_detail::Blur(nz, 1.2);
    double ss = 0;
    for (float v : nz.data) ss += static_cast<double>(v) * v;
    const float k = static_cast<float>(0.006 / std::sqrt(ss / static_cast<double>(nz.data.size())));
    for (size_t i = 0; i < im.data.size(); ++i) im.data[i] = std::clamp(im.data[i] + k * nz.data[i], 0.0f, 1.0f);
    return im;
}

}  // namespace

TEST_CASE("planet disc: a small planet in a large frame of sky is found, not the frame") {
    for (unsigned seed : {1u, 2u, 3u}) {
        const ser::FloatImage im = WithStackNoise(MakePlanetScene(SmallInLargeSky(seed)).observed, seed);
        const stack::LunarMasks m = stack::SegmentPlanet(im, 4);
        const size_t n = static_cast<size_t>(im.width) * im.height;
        const size_t centre = static_cast<size_t>(483) * im.width + 905;
        INFO("seed " << seed << ": disc " << m.diskCount << " px, sky " << m.skyCount << " px of " << n << "; " << m.reason);
        REQUIRE(m.diskCount > 0);
        CHECK(m.diskCount < 200);
        CHECK(m.disk[centre]);
        CHECK(m.skyCount > n / 2);
    }
    // The pipeline takes it: the disc's line names its px, not the frame.
    const ser::FloatImage big = WithStackNoise(MakePlanetScene(SmallInLargeSky(4)).observed, 4);
    const stack::MineralCache cache = stack::PreparePlanet(big, ser::Target::Neptune, 4, {});
    INFO(cache.history.front());
    CHECK(cache.masks.diskCount < 200);
    CHECK_FALSE(Contains(cache.history.front(), "100.00% of the frame"));
}

TEST_CASE("planet disc: a disc filling the frame stays the frame's") {
    // The fallback's limit: a planet larger than the frame keeps SegmentLunar's masks (nothing compact to find).
    PlanetSceneSpec s = Framed(20);
    s.width = 48;
    s.height = 40;
    s.cx = 24;
    s.cy = 20;
    s.radius = 40;
    s.noiseStd = 0.004;
    const ser::FloatImage im = MakePlanetScene(s).observed;
    const stack::LunarMasks a = stack::SegmentPlanet(im, 4), b = stack::SegmentLunar(im, 4);
    CHECK(a.disk == b.disk);
    CHECK(a.sky == b.sky);
}

// ---- The ring in the disc mask (spec P6, plan risk 2) ----

TEST_CASE("ring stays in the disc") {
    // Saturn as captured: a globe of 16 px across (rings 36 px), and the 127 Mak's 2x; ring openings 4 and 25 deg.
    for (double r : {8.0, 16.0})
        for (double tilt : {4.0, 25.0}) {
            PlanetSceneSpec s = Realistic(r, 7);
            s.width = std::max(96, static_cast<int>(6 * r));
            s.height = std::max(72, static_cast<int>(4.5 * r));
            s.cx = 0.5 * s.width;
            s.cy = 0.5 * s.height;
            s.ring = true;
            s.ringTiltDeg = tilt;
            s.positionAngleDeg = 3;
            s.noiseStd = 0.003;
            const PlanetScene sc = MakePlanetScene(s);
            const stack::LunarMasks m = stack::SegmentLunar(sc.observed, 4);
            // Ring pixels above the 4% contrast line: the ring's own light over 4% of the globe's level above the sky.
            size_t ring = 0, kept = 0;
            for (size_t i = 0; i < m.disk.size(); ++i)
                if (sc.ringLight.data[i] > 0.04 * sc.globeLevel) {
                    ++ring;
                    kept += m.disk[i];
                }
            INFO("r " << r << " tilt " << tilt << ": " << kept << " of " << ring << " ring px in the disc mask ("
                      << m.diskCount << " disc px)");
            REQUIRE(ring > 20);
            CHECK(static_cast<double>(kept) / ring >= 0.95);
        }
}

// ---- Small-disc dispersion (Ruling P1) ----

namespace {

// The shift left after alignment: the true one less what was applied.
double Residual(const stack::DispersionShift& d, double tx, double ty) {
    return d.applied ? std::hypot(tx - d.dx, ty - d.dy) : std::hypot(tx, ty);
}
double Applied(const stack::DispersionShift& d) { return d.applied ? std::hypot(d.dx, d.dy) : 0.0; }
bool AllFinite(const ser::FloatImage& im) {
    for (float v : im.data)
        if (!std::isfinite(v)) return false;
    return true;
}

}  // namespace

TEST_CASE("dispersion residual") {
    // Ruling P1: 12, 20, 40 and 200 px discs, R and B 0.5 px off G (in opposite senses) along a random axis.
    std::mt19937 rng(2026);
    std::uniform_real_distribution<double> angle(0, 2 * M_PI);
    for (double r : {6.0, 10.0, 20.0, 100.0})
        for (int k = 0; k < 2; ++k) {
            PlanetSceneSpec s = Realistic(r, 100 + static_cast<unsigned>(r) + k);
            const double th = angle(rng), tx = 0.5 * std::cos(th), ty = 0.5 * std::sin(th);
            s.shiftX = {tx, 0, -tx};
            s.shiftY = {ty, 0, -ty};
            const PlanetScene sc = MakePlanetScene(s);
            ser::FloatImage im = sc.observed;
            const stack::LunarMasks m = stack::SegmentLunar(im, 4);
            stack::PlanetDispersion d;
            const stack::StepRecord rec = stack::AlignPlanetChannels(im, m, 4, &d);
            const stack::PlanetDispersion again = stack::MeasurePlanetDispersion(im, m, 4);
            INFO("r " << r << ": true R " << tx << ", " << ty << "; " << rec.history << "; re-measured R " << again.r.dx
                      << ", " << again.r.dy << " B " << again.b.dx << ", " << again.b.dy);
            REQUIRE_FALSE(rec.skipped);
            CHECK(Residual(d.r, tx, ty) <= 0.1);
            CHECK(Residual(d.b, -tx, -ty) <= 0.1);
            // The resample went the right way: the output reads as aligned.
            REQUIRE(again.r.measured);
            REQUIRE(again.b.measured);
            CHECK(std::hypot(again.r.dx, again.r.dy) <= 0.1);
            CHECK(std::hypot(again.b.dx, again.b.dy) <= 0.1);
            CHECK(Contains(rec.history, "resampled"));
            CHECK(AllFinite(im));
        }
}

TEST_CASE("dispersion: a channel's own limb profile does not hold back a real shift") {
    // Task 5, 2023-11-02 Jupiter (r 27.7 px): R -0.85 px and B +0.64 px against G along y (atmospheric dispersion: R
    // and B on opposite sides), B's limb wider than G's (blue seeing is worse; the fit's normal offset e read 0.64 px).
    // B was declined at +-0.47 px: its uncertainty takes in the shift's dependence on e, which stays (the zero-shift twin
    // below needs it); along R's axis, R corrected on its own, B is tested without it. Both must be corrected.
    for (unsigned seed : {1u, 2u, 3u}) {
        PlanetSceneSpec s = Realistic(28, 300 + seed);
        s.bandContrast = {0.7, 1.0, 1.4};
        s.shiftY = {-0.85, 0, 0.64};
        // The limb brighter on one side than the other (Jupiter's limb is not alike all round): 0.6 to 1.4 of its light
        // across the disc along y, on the noise-free scene, then the noise.
        PlanetSceneSpec wide = s;
        wide.blurSigma = 2.0;  // B's seeing is worse: its limb is wider than G's
        const PlanetScene sc = MakePlanetScene(s), sb = MakePlanetScene(wide);
        ser::FloatImage im = sc.clean;
        for (size_t i = 0; i < im.data.size() / 3; ++i) im.data[i * 3 + 2] = sb.clean.data[i * 3 + 2];
        std::mt19937 rng(seed);
        std::normal_distribution<float> noise(0.0f, static_cast<float>(s.noiseStd));
        for (int y = 0; y < im.height; ++y)
            for (int x = 0; x < im.width; ++x)
                for (int c = 0; c < 3; ++c) {
                    const double f = std::clamp(1 + 0.4 * (y - s.cy) / s.radius, 0.6, 1.4);
                    float& v = im.data[(static_cast<size_t>(y) * im.width + x) * 3 + c];
                    v = static_cast<float>(s.pedestal[c] + f * (v - s.pedestal[c])) + noise(rng);
                }
        const stack::LunarMasks m = stack::SegmentLunar(im, 4);
        stack::PlanetDispersion d;
        const stack::StepRecord rec = stack::AlignPlanetChannels(im, m, 4, &d);
        INFO("seed " << seed << ": " << rec.history);
        CHECK(Residual(d.r, 0, -0.85) <= 0.1);
        CHECK(Residual(d.b, 0, 0.64) <= 0.1);
    }
}

TEST_CASE("dispersion: a channel's own limb profile makes no false shift") {
    // Task 5 review, Important 1: the zero-shift twin of the test above. B's limb wider than G's (blurred 2.0 px against
    // 1.2) and the light tilted across the disc (0.4 or 0.8 of it per radius, clamped 0.6-1.4) bias B's fitted shift
    // through its trade-off with e (0.29 px at r 10, tilt 0.4, seed 3); the shift's dependence on e must keep it from
    // being applied. 4 radii x 2 tilts x 6 seeds, no true shift: nothing moved by more than 0.1 px.
    for (double r : {10.0, 14.0, 20.0, 28.0})
        for (double tilt : {0.4, 0.8})
            for (unsigned seed = 1; seed <= 6; ++seed) {
                PlanetSceneSpec s = Realistic(r, 300 + seed);
                s.bandContrast = {0.7, 1.0, 1.4};
                PlanetSceneSpec wide = s;
                wide.blurSigma = 2.0;
                const PlanetScene sc = MakePlanetScene(s), sb = MakePlanetScene(wide);
                ser::FloatImage im = sc.clean;
                for (size_t i = 0; i < im.data.size() / 3; ++i) im.data[i * 3 + 2] = sb.clean.data[i * 3 + 2];
                std::mt19937 rng(seed);
                std::normal_distribution<float> noise(0.0f, static_cast<float>(s.noiseStd));
                for (int y = 0; y < im.height; ++y)
                    for (int x = 0; x < im.width; ++x)
                        for (int c = 0; c < 3; ++c) {
                            const double f = std::clamp(1 + tilt * (y - s.cy) / s.radius, 0.6, 1.4);
                            float& v = im.data[(static_cast<size_t>(y) * im.width + x) * 3 + c];
                            v = static_cast<float>(s.pedestal[c] + f * (v - s.pedestal[c])) + noise(rng);
                        }
                const stack::LunarMasks m = stack::SegmentLunar(im, 4);
                stack::PlanetDispersion d;
                const stack::StepRecord rec = stack::AlignPlanetChannels(im, m, 4, &d);
                INFO("r " << r << " tilt " << tilt << " seed " << seed << ": " << rec.history);
                CHECK(Applied(d.r) <= 0.1);
                CHECK(Applied(d.b) <= 0.1);
            }
}

TEST_CASE("dispersion: the weaker channel follows the stronger's axis") {
    // Task 5, 2026-10-23 Saturn: B -0.31, +0.32 px (+-0.08) was corrected, R +0.39, -0.40 px (+-0.22, the rings' colour
    // in the jackknife) was not, and left a 0.5 px red fringe. Atmospheric dispersion puts R and B on opposite sides of
    // G along one line: once one channel is corrected, the other is tested along that line alone (its component there,
    // with that component's uncertainty). Here R is faint and noisy (Neptune-like: R/G 0.15), so on its own it is
    // within 3 sigma; along B's axis it is not.
    // Seeds whose R is within 3 sigma on its own (deterministic scenes; 6 of 16 seeds at this noise stay declined even
    // along the axis, and 5 pass on their own).
    for (unsigned seed : {2u, 4u, 6u, 15u, 16u}) {
        PlanetSceneSpec s = Realistic(14, 400 + seed);
        s.tint = {0.15, 1.0, 1.2};
        s.cast = {1, 1, 1};
        s.noiseStd = 0.016;
        const double ux = 0.69, uy = -0.73;  // the axis, R's sense
        s.shiftX = {0.55 * ux, 0, -0.45 * ux};
        s.shiftY = {0.55 * uy, 0, -0.45 * uy};
        ser::FloatImage im = MakePlanetScene(s).observed;
        const stack::LunarMasks m = stack::SegmentLunar(im, 4);
        stack::PlanetDispersion d;
        const stack::StepRecord rec = stack::AlignPlanetChannels(im, m, 4, &d);
        INFO("seed " << seed << ": " << rec.history);
        CHECK(d.b.applied);
        CHECK(d.r.applied);
        CHECK(d.r.alongAxis);
        CHECK(Contains(rec.history, "R along B's dispersion axis"));
        // Its own uncertainty along the axis is 0.13-0.17 px here: corrected to within that, not to 0.1 px (0.55 px left
        // uncorrected otherwise).
        CHECK(Residual(d.r, 0.55 * ux, 0.55 * uy) <= 0.2);
        CHECK(Residual(d.b, -0.45 * ux, -0.45 * uy) <= 0.1);
    }
    // R unshifted: the axis invents no shift for it.
    for (unsigned seed : {1u, 2u, 3u}) {
        PlanetSceneSpec s = Realistic(14, 410 + seed);
        s.tint = {0.15, 1.0, 1.2};
        s.cast = {1, 1, 1};
        s.noiseStd = 0.016;
        s.shiftX = {0, 0, -0.45 * 0.69};
        s.shiftY = {0, 0, 0.45 * 0.73};
        ser::FloatImage im = MakePlanetScene(s).observed;
        const stack::LunarMasks m = stack::SegmentLunar(im, 4);
        stack::PlanetDispersion d;
        const stack::StepRecord rec = stack::AlignPlanetChannels(im, m, 4, &d);
        INFO("seed " << seed << " (R unshifted): " << rec.history);
        CHECK(Applied(d.r) <= 0.1);
    }
}

TEST_CASE("dispersion: no false shift") {
    for (double r : {6.0, 10.0, 20.0, 100.0})
        for (unsigned seed = 1; seed <= 6; ++seed) {
            const PlanetSceneSpec s = Realistic(r, 200 + seed);
            const PlanetScene sc = MakePlanetScene(s);
            ser::FloatImage im = sc.observed;
            const stack::LunarMasks m = stack::SegmentLunar(im, 4);
            stack::PlanetDispersion d;
            const stack::StepRecord rec = stack::AlignPlanetChannels(im, m, 4, &d);
            INFO("r " << r << " seed " << seed << ": " << rec.history);
            CHECK(Applied(d.r) <= 0.1);
            CHECK(Applied(d.b) <= 0.1);
            if (!d.r.applied && !d.b.applied) CHECK(im.data == sc.observed.data);
        }
}

TEST_CASE("dispersion: disc cut by the frame edge") {
    // The disc's centre 3 px from the left edge: the step runs or skips, but misregisters no channel by over 0.1 px
    // and adds no NaN. With no true shift nothing over 0.1 px is applied; with a true 0.5 px one, a correction that
    // is applied leaves at most 0.1 px, and a skip says why in HISTORY.
    for (double r : {6.0, 10.0, 20.0})
        for (unsigned seed = 1; seed <= 3; ++seed)
            for (double shift : {0.0, 0.5}) {
                PlanetSceneSpec s = Realistic(r, 300 + 7 * seed + static_cast<unsigned>(r));
                s.cx = 3;
                const double tx = shift * std::cos(0.7 * seed), ty = shift * std::sin(0.7 * seed);
                s.shiftX = {tx, 0, -tx};
                s.shiftY = {ty, 0, -ty};
                const PlanetScene sc = MakePlanetScene(s);
                ser::FloatImage im = sc.observed;
                const stack::LunarMasks m = stack::SegmentLunar(im, 4);
                stack::PlanetDispersion d;
                const stack::StepRecord rec = stack::AlignPlanetChannels(im, m, 4, &d);
                INFO("r " << r << " seed " << seed << " shift " << shift << ": " << rec.history);
                CHECK(d.frameCut);
                CHECK(Applied(d.r) <= (shift == 0 ? 0.1 : 1e9));
                CHECK(Applied(d.b) <= (shift == 0 ? 0.1 : 1e9));
                if (d.r.applied) CHECK(Residual(d.r, tx, ty) <= 0.1);
                if (d.b.applied) CHECK(Residual(d.b, -tx, -ty) <= 0.1);
                if (!d.r.applied && !d.b.applied) {
                    CHECK(rec.skipped);
                    CHECK(Contains(rec.history, "not applied"));
                    CHECK(im.data == sc.observed.data);
                }
                CHECK(AllFinite(im));
            }
}

TEST_CASE("dispersion: a ringed disc") {
    // Saturn: rings a different colour from the globe, which a single gain would read as a shift.
    for (double tilt : {4.0, 25.0})
        for (double shift : {0.0, 0.5}) {
            PlanetSceneSpec s = Realistic(8, 400 + static_cast<unsigned>(tilt));
            s.width = 96;
            s.height = 72;
            s.cx = 48.3;
            s.cy = 35.8;
            s.ring = true;
            s.ringTiltDeg = tilt;
            s.ringTint = {0.92, 1.0, 1.08};
            const double tx = shift * std::cos(1.9), ty = shift * std::sin(1.9);
            s.shiftX = {tx, 0, -tx};
            s.shiftY = {ty, 0, -ty};
            const PlanetScene sc = MakePlanetScene(s);
            ser::FloatImage im = sc.observed;
            const stack::LunarMasks m = stack::SegmentLunar(im, 4);
            stack::PlanetDispersion d;
            const stack::StepRecord rec = stack::AlignPlanetChannels(im, m, 4, &d);
            INFO("tilt " << tilt << " shift " << shift << ": " << rec.history);
            CHECK(Residual(d.r, tx, ty) <= 0.1);
            CHECK(Residual(d.b, -tx, -ty) <= 0.1);
        }
}

TEST_CASE("dispersion: skips with its reason, and does not depend on the thread count") {
    // A frame of sky: flat, SegmentLunar finds no disc; with a gradient, it reads the frame as a disc filling it
    // (its documented limit), which has no limb.
    for (bool gradient : {false, true}) {
        PlanetSceneSpec s = Realistic(10, 9);
        s.level = 0;
        if (!gradient) s.gradientX = s.gradientY = {0, 0, 0};
        const PlanetScene sc = MakePlanetScene(s);
        ser::FloatImage im = sc.observed;
        const stack::LunarMasks m = stack::SegmentLunar(im, 2);
        const stack::StepRecord rec = stack::AlignPlanetChannels(im, m, 2);
        INFO(rec.history);
        CHECK(rec.skipped);
        CHECK(Contains(rec.history, gradient ? "no limb inside the frame" : "no planet disc"));
        CHECK(im.data == sc.observed.data);
    }
    {  // 20 disc pixels: under the floor
        PlanetSceneSpec s = Realistic(10, 9);
        ser::FloatImage im = MakePlanetScene(s).observed;
        stack::LunarMasks m = stack::SegmentLunar(im, 2);
        size_t kept = 0;
        for (auto& v : m.disk)
            if (v && ++kept > 20) v = 0;
        m.diskCount = 20;
        const ser::FloatImage before = im;
        const stack::StepRecord rec = stack::AlignPlanetChannels(im, m, 2);
        INFO(rec.history);
        CHECK(rec.skipped);
        CHECK(Contains(rec.history, "30"));
        CHECK(im.data == before.data);
    }
    {
        PlanetSceneSpec s = Realistic(20, 12);
        s.shiftX = {0.4, 0, -0.3};
        s.shiftY = {-0.2, 0, 0.35};
        const ser::FloatImage in = MakePlanetScene(s).observed;
        const stack::LunarMasks m = stack::SegmentLunar(in, 4);
        ser::FloatImage a = in, b = in;
        const stack::StepRecord ra = stack::AlignPlanetChannels(a, m, 1), rb = stack::AlignPlanetChannels(b, m, 8);
        CHECK(ra.history == rb.history);
        CHECK(a.data == b.data);
    }
}

// ---- Sharpening for small discs (Ruling P1: the limb fade scaled to the disc) ----

namespace {

// The luminance (the channels' mean, as the mineral path takes it) as a 1-channel image.
ser::FloatImage LumImage(const ser::FloatImage& rgb) {
    ser::FloatImage L{rgb.width, rgb.height, 1, {}};
    for (double v : planetary::testing::Lum(rgb)) L.data.push_back(static_cast<float>(v));
    return L;
}

double RmsOver(const std::vector<float>& a, const std::vector<double>& b, const std::vector<uint8_t>& mask) {
    double s = 0;
    size_t n = 0;
    for (size_t i = 0; i < mask.size(); ++i)
        if (mask[i]) {
            s += (a[i] - b[i]) * (a[i] - b[i]);
            ++n;
        }
    return std::sqrt(s / n);
}

double StdOver(const std::vector<float>& a, const std::vector<uint8_t>& mask) {
    double s = 0, s2 = 0;
    size_t n = 0;
    for (size_t i = 0; i < mask.size(); ++i)
        if (mask[i]) {
            s += a[i];
            s2 += static_cast<double>(a[i]) * a[i];
            ++n;
        }
    return std::sqrt(std::max(0.0, s2 / n - (s / n) * (s / n)));
}

// A planet as the sharpening tests take it: seeing blur, white noise, no cast or background (so the truth and the
// observed light are on one scale), in at least a 160 x 120 frame (the acceptance ROIs are 320 x 240), so the sky
// has the 1000 px the noise estimate needs (Framed's 64 x 48 left a 40 px disc 856 px of sky, and the noise read 5x
// high off the disc). Fine detail (texture to 4 px periods, spots) at every radius, so a large disc has something
// the blur took to give back.
PlanetSceneSpec Blurred(double r, double blur, unsigned seed) {
    PlanetSceneSpec s = Framed(r);
    s.width = std::max(s.width, 160);
    s.height = std::max(s.height, 120);
    s.cx = 0.5 * s.width + 0.3;
    s.cy = 0.5 * s.height - 0.2;
    s.seed = seed;
    s.blurSigma = blur;
    s.noiseStd = 0.005;
    s.textureAmplitude = 0.05;  // detail at the seeing's scale at every radius (ruling Q2)
    s.spotDensity = 4;
    return s;
}

}  // namespace

TEST_CASE("small-disc sharpening reduces error against the truth") {
    // Ruling P1 (mineral Ruling 2's ground-truth test): on blurred discs of known truth, the Sharpen at the default
    // amount with the planet profile brings the disc closer to the sharp truth.
    for (double r : {10.0, 20.0, 100.0})
        for (double blur : {1.0, 1.5}) {
            const PlanetSceneSpec s = Blurred(r, blur, 31 + static_cast<unsigned>(r));
            const PlanetScene sc = MakePlanetScene(s);
            const stack::LunarMasks m = stack::SegmentLunar(sc.observed, 4);
            const std::vector<double> truth = planetary::testing::Lum(sc.truth);
            ser::FloatImage L = LumImage(sc.observed);
            const double e0 = RmsOver(L.data, truth, sc.diskTruth);
            const stack::StepRecord rec = stack::SharpenLuminanceImage(L, m, 4, nullptr, nullptr, stack::kPlanetSharpenDefault,
                                                                       stack::kPlanetSharpen);
            const double e1 = RmsOver(L.data, truth, sc.diskTruth);
            INFO("r " << r << ", blur " << blur << ": disc RMS error vs truth " << e0 << " -> " << e1 << " ("
                      << 100 * (1 - e1 / e0) << "% lower); " << rec.history);
            WARN("r " << r << ", blur " << blur << ": disc RMS error vs truth " << e0 << " -> " << e1 << " ("
                      << 100 * (1 - e1 / e0) << "% lower)");
            REQUIRE_FALSE(rec.skipped);
            CHECK(e1 < e0);
        }
}

TEST_CASE("no halo around a ringed disc") {
    // Spec acceptance 4 (mineral B4): the profile across the limb and the ring tips overshoots the sky by no more than
    // 2% of the disc's mean, and sharpening puts no bright rim inside the edge (above both the blurred input and the
    // truth). Profiles are 5 px wide strips (noise down by sqrt 5): up and down from the globe's centre (the limb; the
    // far half of the ring is behind the globe there, the near half ends inside it) and out along the ring's major
    // axis (its tips; the window takes in the Cassini gap, 2-5 px in), from 8 px inside the edge to 10 px into the
    // sky.
    for (double r : {8.0, 16.0}) {
        PlanetSceneSpec s = Blurred(r, 1.2, 41 + static_cast<unsigned>(r));
        s.noiseStd = 0.002;
        s.ring = true;
        s.ringTiltDeg = 25;
        s.width = std::max(96, static_cast<int>(std::lround(6 * r)));
        s.height = std::max(64, static_cast<int>(std::lround(3.2 * r)));
        s.cx = 0.5 * s.width + 0.3;
        s.cy = 0.5 * s.height - 0.2;
        const PlanetScene sc = MakePlanetScene(s);
        const stack::LunarMasks m = stack::SegmentLunar(sc.observed, 4);
        const ser::FloatImage L0 = LumImage(sc.observed);
        ser::FloatImage L1 = L0;
        const stack::StepRecord rec = stack::SharpenLuminanceImage(L1, m, 4, nullptr, nullptr, stack::kPlanetSharpenDefault,
                                                                   stack::kPlanetSharpen);
        REQUIRE_FALSE(rec.skipped);
        const std::vector<double> truth = planetary::testing::Lum(sc.truth);
        double discMean = 0;
        size_t nd = 0;
        for (size_t i = 0; i < sc.diskTruth.size(); ++i)
            if (sc.diskTruth[i]) {
                discMean += L1.data[i];
                ++nd;
            }
        discMean /= static_cast<double>(nd);
        // Strip mean at distance t along (dx, dy) from the edge point (ex, ey); t < 0 inside.
        auto strip = [&](const std::vector<double>& v, double ex, double ey, double dx, double dy, double t) {
            double sum = 0;
            for (int k = -2; k <= 2; ++k) {
                const int x = static_cast<int>(std::lround(ex + t * dx - k * dy)), y = static_cast<int>(std::lround(ey + t * dy + k * dx));
                sum += v[static_cast<size_t>(y) * s.width + x];
            }
            return sum / 5;
        };
        const std::vector<double> before(L0.data.begin(), L0.data.end()), after(L1.data.begin(), L1.data.end());
        struct Edge { const char* name; double ex, ey, dx, dy; };
        const Edge edges[] = {{"top limb", s.cx, s.cy - r, 0, -1},
                              {"bottom limb", s.cx, s.cy + r, 0, 1},
                              {"left ring tip", s.cx - s.ringOuter * r, s.cy, -1, 0},
                              {"right ring tip", s.cx + s.ringOuter * r, s.cy, 1, 0}};
        for (const Edge& e : edges) {
            double sky = 0;
            for (int t = 7; t <= 10; ++t) sky += strip(after, e.ex, e.ey, e.dx, e.dy, t) / 4;
            double over = 0, under = 0, rim = 0;
            for (double t = -8; t <= 10; t += 1) {
                const double p0 = strip(before, e.ex, e.ey, e.dx, e.dy, t), p1 = strip(after, e.ex, e.ey, e.dx, e.dy, t),
                             pt = strip(truth, e.ex, e.ey, e.dx, e.dy, t);
                if (t > 0) {
                    over = std::max(over, p1 - p0);
                    under = std::max(under, sky - p1);
                } else {
                    rim = std::max(rim, p1 - std::max(p0, pt));
                }
            }
            INFO("globe r " << r << ", " << e.name << ": disc mean " << discMean << "; sky-side brightening " << over
                            << ", undershoot " << under << ", rim over the input and truth " << rim << "; " << rec.history);
            WARN("globe r " << r << ", " << e.name << ": sky-side brightening " << over / discMean * 100 << "%, undershoot "
                            << under / discMean * 100 << "%, rim " << rim / discMean * 100 << "% of the disc mean");
            CHECK(over <= 0.02 * discMean);
            CHECK(under <= 0.02 * discMean);
            CHECK(rim <= 0.02 * discMean);
        }
    }
}

TEST_CASE("planet sharpen series (measurement, not a check)", "[.series]") {
    // Task 5, brief Step 3 (the Sharpen default): not run by default (hidden tag); run it with
    //   build/test/test_planet "[.series]"
    // Per amount 0, 1, 1.5, 2, 3, 4 with kPlanetSharpen, on Blurred() discs (fine detail, noise 0.005): the disc's RMS
    // error against the sharp truth (lower than the input's by), and the noise gain on the disc -- the same scene with
    // two noise draws, std of the sharpened difference over the input's -- averaged over 4 seeds.
    const double amounts[] = {0, 1, 1.5, 2, 3, 4};
    std::printf("planet sharpen series (kPlanetSharpen): r, blur: per amount, error lower by %% / disc noise gain\n");
    for (double r : {10.0, 20.0, 40.0, 100.0})
        for (double blur : {1.0, 1.5}) {
            double lower[6] = {}, gain[6] = {};
            const int seeds = 4;
            for (int k = 0; k < seeds; ++k) {
                PlanetSceneSpec s = Blurred(r, blur, 600 + static_cast<unsigned>(10 * r) + static_cast<unsigned>(k));
                s.noiseSeed = 1;
                const PlanetScene a = MakePlanetScene(s);
                s.noiseSeed = 2;
                const PlanetScene b = MakePlanetScene(s);
                const stack::LunarMasks m = stack::SegmentLunar(a.observed, 4);
                const std::vector<double> truth = planetary::testing::Lum(a.truth);
                const ser::FloatImage La = LumImage(a.observed), Lb = LumImage(b.observed);
                const double e0 = RmsOver(La.data, truth, a.diskTruth);
                std::vector<float> d0(La.data.size());
                for (size_t i = 0; i < d0.size(); ++i) d0[i] = La.data[i] - Lb.data[i];
                const double n0 = StdOver(d0, a.diskTruth);
                for (int j = 0; j < 6; ++j) {
                    ser::FloatImage sa = La, sb = Lb;
                    stack::SharpenLuminanceImage(sa, m, 4, nullptr, nullptr, amounts[j], stack::kPlanetSharpen);
                    stack::SharpenLuminanceImage(sb, m, 4, nullptr, nullptr, amounts[j], stack::kPlanetSharpen);
                    std::vector<float> d(sa.data.size());
                    for (size_t i = 0; i < d.size(); ++i) d[i] = sa.data[i] - sb.data[i];
                    lower[j] += 100 * (1 - RmsOver(sa.data, truth, a.diskTruth) / e0) / seeds;
                    gain[j] += StdOver(d, a.diskTruth) / n0 / seeds;
                }
            }
            std::printf("  r %5.1f blur %.1f:", r, blur);
            for (int j = 0; j < 6; ++j) std::printf("  A%-3g %+6.1f%% x%.2f", amounts[j], lower[j], gain[j]);
            std::printf("\n");
        }
    SUCCEED();
}

TEST_CASE("sky noise") {
    // Ruling P2: sharpening at the default raises the sky's noise std by no more than 1.5x.
    for (double r : {10.0, 20.0, 100.0}) {
        const PlanetScene sc = MakePlanetScene(Blurred(r, 1.5, 51 + static_cast<unsigned>(r)));
        const stack::LunarMasks m = stack::SegmentLunar(sc.observed, 4);
        const ser::FloatImage L0 = LumImage(sc.observed);
        ser::FloatImage L1 = L0;
        stack::SharpenLuminanceImage(L1, m, 4, nullptr, nullptr, stack::kPlanetSharpenDefault, stack::kPlanetSharpen);
        const double n0 = StdOver(L0.data, sc.skyTruth), n1 = StdOver(L1.data, sc.skyTruth);
        INFO("r " << r << ": sky noise std " << n0 << " -> " << n1);
        CHECK(n1 <= 1.5 * n0);
    }
}

TEST_CASE("sharpen profile: refused when not finite and >= 0, and HISTORY names a scaled fade") {
    const PlanetScene sc = MakePlanetScene(Blurred(10, 1.0, 61));
    const stack::LunarMasks m = stack::SegmentLunar(sc.observed, 4);
    const double bad[] = {-0.1, std::nan(""), HUGE_VAL};
    for (double v : bad) {
        stack::SharpenProfile f = stack::kPlanetSharpen, g = stack::kPlanetSharpen, x = stack::kPlanetSharpen;
        f.fadeRadiusFraction = v;
        g.gain[1] = v;
        x.extra[0] = v;
        for (const stack::SharpenProfile* p : {&f, &g, &x}) {
            ser::FloatImage L = LumImage(sc.observed);
            CHECK_THROWS_AS(stack::SharpenLuminanceImage(L, m, 4, nullptr, nullptr, 0.0, *p), std::invalid_argument);
        }
    }
    ser::FloatImage L = LumImage(sc.observed);
    const stack::StepRecord rec = stack::SharpenLuminanceImage(L, m, 4, nullptr, nullptr, stack::kSharpenDefault,
                                                               stack::kPlanetSharpen);
    INFO(rec.history);
    CHECK(Contains(rec.history, "limb fade scaled to the disc"));
    ser::FloatImage M = LumImage(sc.observed);
    CHECK_FALSE(Contains(stack::SharpenLuminanceImage(M, m, 4).history, "limb fade"));  // the Moon's line as it was
}

// ---- The planet pipeline (spec calls 2, 4, 6; acceptance 6-9; plan Task 4) ----

namespace {

bool Ascii(const std::string& s) {
    return std::all_of(s.begin(), s.end(), [](char ch) { return static_cast<unsigned char>(ch) < 0x80; });
}

void CheckValid(const stack::MineralOutcome& out, const ser::FloatImage& in) {
    REQUIRE(out.image.width == in.width);
    REQUIRE(out.image.height == in.height);
    REQUIRE(out.image.channels == 3);
    REQUIRE(out.image.data.size() == in.data.size());
    size_t bad = 0;
    for (float v : out.image.data) bad += !(std::isfinite(v) && v >= 0.0f && v <= 1.0f);
    CHECK(bad == 0);  // every sample finite and in [0, 1]
}

// The planet pipeline's seven steps, in order (spec call 2: the mineral moon's order, its own disc and colour step).
const std::vector<std::string> kPlanetSteps = {"Planet disc", "Colour calibration", "Background", "Display transfer",
                                               "Channel alignment", "Sharpen", "Saturation"};

// Every line in order, ASCII, and every step that declined says why (spec anti-goal: no defect hidden).
void CheckHistory(const stack::MineralOutcome& out) {
    REQUIRE(out.history.size() == kPlanetSteps.size());
    REQUIRE(out.stepSeconds.size() == kPlanetSteps.size());
    for (size_t k = 0; k < kPlanetSteps.size(); ++k) {
        const std::string& h = out.history[k];
        INFO(h);
        CHECK(h.rfind(kPlanetSteps[k], 0) == 0);
        CHECK(out.stepSeconds[k].first == kPlanetSteps[k]);
        CHECK(Ascii(h));
        const size_t at = h.find(" skipped: ");
        if (at != std::string::npos) CHECK(h.size() >= at + std::string(" skipped: ").size() + 10);  // a reason
    }
}

// The disc's mean B/G and R/G of an image over a mask.
std::pair<double, double> MeanRatios(const ser::FloatImage& im, const std::vector<uint8_t>& mask) {
    double s[3] = {0, 0, 0};
    for (size_t i = 0; i < mask.size(); ++i)
        if (mask[i])
            for (int c = 0; c < 3; ++c) s[c] += im.data[i * 3 + c];
    return {s[2] / s[1], s[0] / s[1]};
}

}  // namespace

TEST_CASE("planet pipeline: history, refusals and the parameters") {
    const PlanetSceneSpec s = Realistic(20, 71);
    const PlanetScene sc = MakePlanetScene(s);
    const stack::MineralCache cache = stack::PreparePlanet(sc.observed, ser::Target::Saturn, 4, {});
    const stack::MineralOutcome out = stack::FinishPlanet(cache, {}, 4);
    CheckValid(out, sc.observed);
    CheckHistory(out);
    CHECK(out.notice.empty());
    CHECK(Contains(out.history[1], "Saturn"));
    CHECK(Contains(out.history[3], "linear"));
    CHECK(Contains(out.history[5], "limb fade scaled to the disc"));  // kPlanetSharpen, not the Moon's profile
    // Defaults (spec call 7: saturation 1 is the natural colour; Task 5 measures the other two).
    const stack::PlanetParams p;
    CHECK(p.saturation == 1.0);
    CHECK(p.chromaBlurSigma == stack::kPlanetChromaBlurDefault);
    CHECK(p.sharpen == stack::kPlanetSharpenDefault);
    // Not a planet: refused (the colour reference needs a known body).
    for (ser::Target t : {ser::Target::Moon, ser::Target::Other})
        CHECK_THROWS_AS(stack::PreparePlanet(sc.observed, t, 4, {}), std::invalid_argument);
    // PrepareMineral's own refusals: mono, NaN, Inf.
    ser::FloatImage mono{64, 48, 1, std::vector<float>(64 * 48, 0.3f)};
    CHECK_THROWS_AS(stack::PreparePlanet(mono, ser::Target::Mars, 2, {}), std::invalid_argument);
    for (float v : {std::nanf(""), HUGE_VALF}) {
        ser::FloatImage bad = sc.observed;
        bad.data[100] = v;
        CHECK_THROWS_AS(stack::PreparePlanet(bad, ser::Target::Mars, 2, {}), std::invalid_argument);
    }
    // Parameters out of range.
    for (double v : {-0.1, std::nan(""), HUGE_VAL}) {
        stack::PlanetParams a, b, c;
        a.saturation = v;
        b.chromaBlurSigma = v;
        c.sharpen = v;
        for (const stack::PlanetParams* q : {&a, &b, &c}) CHECK_THROWS_AS(stack::FinishPlanet(cache, *q, 4), std::invalid_argument);
    }
}

TEST_CASE("planet pipeline: thread count and re-runs do not change the output") {
    const PlanetScene sc = MakePlanetScene(Realistic(20, 73));
    const stack::MineralOutcome a = stack::FinishPlanet(stack::PreparePlanet(sc.observed, ser::Target::Jupiter, 1, {}), {}, 1);
    const stack::MineralCache cache = stack::PreparePlanet(sc.observed, ser::Target::Jupiter, 7, {});
    const stack::MineralOutcome b = stack::FinishPlanet(cache, {}, 7), c = stack::FinishPlanet(cache, {}, 7);
    CHECK(a.image.data == b.image.data);
    CHECK(b.image.data == c.image.data);
    CHECK(a.history == b.history);
}

TEST_CASE("saturation 1 is the natural colour") {
    // Spec call 6 / acceptance 7: at saturation 1 the processed disc's mean B/G and R/G are the calibrated ones.
    for (double r : {10.0, 20.0, 60.0})
        for (ser::Target t : {ser::Target::Mars, ser::Target::Jupiter, ser::Target::Saturn}) {
            const PlanetScene sc = MakePlanetScene(Realistic(r, 81 + static_cast<unsigned>(r)));
            const stack::MineralCache cache = stack::PreparePlanet(sc.observed, t, 4, {});
            const stack::MineralOutcome out = stack::FinishPlanet(cache, {}, 4);
            CheckValid(out, sc.observed);
            const auto cal = MeanRatios(cache.display, cache.masks.disk), got = MeanRatios(out.image, cache.masks.disk);
            const stack::BodyColour want = *stack::PlanetReflectance(t);
            INFO("r " << r << " " << ser::TargetName(t) << ": calibrated B/G " << cal.first << " R/G " << cal.second
                      << " (catalogue " << want.bOverG << ", " << want.rOverG << "); processed B/G " << got.first << " R/G "
                      << got.second << "; " << out.history[1] << "; " << out.history[6]);
            REQUIRE_FALSE(cache.masks.diskCount == 0);
            CHECK(std::fabs(got.first / cal.first - 1) <= 0.02);
            CHECK(std::fabs(got.second / cal.second - 1) <= 0.02);
        }
}

TEST_CASE("saturation 1 is the natural colour on a dim, peaked disc") {
    // Task 5, 2026-10-23 Neptune (r 5.3 px): the linear display puts the brightest sample at 0.95, so a small blurred
    // disc's median sits low (0.035) and the colour step's eps (0.002, the sky's divide guard) pulled the disc 2.4%
    // toward grey (B/G 1.259 against 1.272); Mars (median 0.057) by 1.9%. Acceptance 7's bar is 2%.
    // A 2.5 px globe whose light is 40% in the seeing's core (1.2 px) and 60% in a wide halo (4 px), as a short-
    // exposure stack's profile is; the disc mask reaches into the halo, so the disc's median is a few % of its peak.
    for (unsigned seed : {1u, 2u, 3u}) {
        PlanetSceneSpec s = Realistic(2.5, 95 + seed);
        s.width = 96;
        s.height = 72;
        s.cx = 48.3;
        s.cy = 35.8;
        s.level = 0.9;
        s.noiseStd = 0;
        PlanetSceneSpec halo = s;
        halo.blurSigma = 4.0;
        const PlanetScene core = MakePlanetScene(s), wide = MakePlanetScene(halo);
        ser::FloatImage im = core.clean;
        std::mt19937 rng(seed);
        std::normal_distribution<float> noise(0.0f, 0.002f);
        for (size_t i = 0; i < im.data.size(); ++i) im.data[i] = 0.4f * im.data[i] + 0.6f * wide.clean.data[i] + noise(rng);
        const stack::MineralCache cache = stack::PreparePlanet(im, ser::Target::Neptune, 4, {});
        const stack::MineralOutcome out = stack::FinishPlanet(cache, {}, 4);
        CheckValid(out, im);
        const auto cal = MeanRatios(cache.display, cache.masks.disk), got = MeanRatios(out.image, cache.masks.disk);
        INFO("seed " << seed << ": calibrated B/G " << cal.first << " R/G " << cal.second << "; processed B/G " << got.first
                     << " R/G " << got.second << "; " << out.history[0] << "; " << out.history[3] << "; " << out.history[6]);
        REQUIRE_FALSE(cache.masks.diskCount == 0);
        CHECK(std::fabs(got.first / cal.first - 1) <= 0.02);
        CHECK(std::fabs(got.second / cal.second - 1) <= 0.02);
    }
}

TEST_CASE("saturation 0 is grey") {
    const PlanetScene sc = MakePlanetScene(Realistic(20, 91));
    const stack::MineralCache cache = stack::PreparePlanet(sc.observed, ser::Target::Mars, 4, {});
    stack::PlanetParams p;
    p.saturation = 0;
    const stack::MineralOutcome out = stack::FinishPlanet(cache, p, 4);
    CheckValid(out, sc.observed);
    double worst = 0;
    for (size_t i = 0; i < out.image.data.size(); i += 3)
        worst = std::max({worst, static_cast<double>(std::fabs(out.image.data[i] - out.image.data[i + 1])),
                          static_cast<double>(std::fabs(out.image.data[i + 2] - out.image.data[i + 1]))});
    INFO(out.history[6]);
    CHECK(worst <= 1e-6);
}

TEST_CASE("no new clipping") {
    // Spec anti-goal and acceptance 6, at the Saturation slider's top on a bright disc with saturated highlights: no
    // sample above 1.0, and none at or above 0.999 in a pixel the stack did not clip.
    for (ser::Target t : {ser::Target::Mars, ser::Target::Saturn, ser::Target::Neptune}) {
        PlanetSceneSpec s = Realistic(30, 101);
        s.level = 0.85;
        s.clipFraction = 0.02;
        const PlanetScene sc = MakePlanetScene(s);
        const stack::MineralCache cache = stack::PreparePlanet(sc.observed, t, 4, {});
        stack::PlanetParams p;
        p.saturation = stack::kPlanetSaturationMax;
        const stack::MineralOutcome out = stack::FinishPlanet(cache, p, 4);
        CheckValid(out, sc.observed);
        size_t stackClipped = 0, newClipped = 0;
        for (size_t i = 0; i < sc.clipped.size(); ++i) {
            bool clippedIn = false, clippedOut = false;
            for (int c = 0; c < 3; ++c) {
                clippedIn = clippedIn || sc.observed.data[i * 3 + c] >= stack::kClippedLuma;
                clippedOut = clippedOut || out.image.data[i * 3 + c] >= 0.999f;
            }
            stackClipped += clippedIn;
            newClipped += clippedOut && !clippedIn;
        }
        INFO(ser::TargetName(t) << ": pixels clipped in the stack " << stackClipped << ", newly clipped " << newClipped << "; "
                                << out.history[6]);
        REQUIRE(stackClipped > 0);
        CHECK(newClipped == 0);
    }
}

TEST_CASE("no chroma off the disc after a real alignment shift") {
    // Task 4 review (Important): AlignPlanetChannels resamples R and B with Catmull-Rom, which overshoots below 0 beside
    // the limb; with the deviation clamp off, a sky pixel whose blurred chroma mean sits near -eps got a d that drove
    // one channel to the gamut guard's 0.998 -- a saturated one-channel speck on a pixel with L ~ 0.001. With the
    // display in [0, 1], d is in (-1, 2), so out_c <= (1 + 2k) L everywhere: off the disc (L at the sky's level) no
    // channel can stand out. Judged at Saturation 3 on discs whose R and B the stack moved by 0.5-0.8 px (resampled).
    for (double r : {10.0, 20.0, 40.0})
        for (ser::Target t : {ser::Target::Mars, ser::Target::Saturn, ser::Target::Neptune}) {
            PlanetSceneSpec s = Realistic(r, 141 + static_cast<unsigned>(r));
            s.shiftX = {0.6, 0, -0.5};
            s.shiftY = {-0.5, 0, 0.6};
            const PlanetScene sc = MakePlanetScene(s);
            const stack::MineralCache cache = stack::PreparePlanet(sc.observed, t, 4, {});
            REQUIRE(Contains(cache.history[4], "resampled onto G"));  // the alignment ran
            stack::PlanetParams p;
            p.saturation = stack::kPlanetSaturationMax;
            const stack::MineralOutcome out = stack::FinishPlanet(cache, p, 4);
            CheckValid(out, sc.observed);
            const ser::FloatImage L = stack::SharpenedLuminance(cache, p.sharpen, 4, nullptr, stack::kPlanetSharpen);
            const double k = p.saturation;
            size_t over = 0, specks = 0;
            double worst = 0, worstL = 0;
            for (size_t i = 0; i < cache.masks.disk.size(); ++i) {
                if (cache.masks.disk[i]) continue;
                const float* o = &out.image.data[i * 3];
                const double hi = std::max({o[0], o[1], o[2]}), lo = std::min({o[0], o[1], o[2]});
                const double l = std::max(0.0f, L.data[i]);
                over += hi > (1 + 2 * k) * l + 1e-6;
                specks += l < 0.02 && hi - lo > 0.1;  // a dark pixel with a bright channel
                if (hi - lo > worst) {
                    worst = hi - lo;
                    worstL = l;
                }
            }
            INFO("r " << r << " " << ser::TargetName(t) << ": off-disc pixels over (1 + 2k) L " << over << ", dark specks "
                      << specks << "; worst chroma " << worst << " at L " << worstL << "; " << cache.history[4]);
            CHECK(over == 0);
            CHECK(specks == 0);
        }
}

TEST_CASE("linear display") {
    // Spec call 4: no midtones stretch for a planet; the white point puts the brightest sample the stack did not clip
    // at 0.95. G is untouched by the calibration and the alignment, so with no background (noise-free, no pedestal or
    // gradient: the sky plane is 0) display G over stack G is the transfer's own slope.
    PlanetSceneSpec s = Framed(20);
    s.width = 160;
    s.height = 120;
    s.cx = 80.3;
    s.cy = 59.8;
    s.blurSigma = 1.2;
    s.tint = {1.2, 1.0, 0.8};
    const PlanetScene sc = MakePlanetScene(s);
    const stack::MineralCache cache = stack::PreparePlanet(sc.observed, ser::Target::Jupiter, 4, {});
    double lo = 1e9, hi = 0, top = 0;
    for (size_t i = 0; i < cache.masks.disk.size(); ++i) {
        for (int c = 0; c < 3; ++c) top = std::max(top, static_cast<double>(cache.display.data[i * 3 + c]));
        if (!cache.masks.disk[i] || sc.observed.data[i * 3 + 1] < 0.02f) continue;
        const double q = cache.display.data[i * 3 + 1] / static_cast<double>(sc.observed.data[i * 3 + 1]);
        lo = std::min(lo, q);
        hi = std::max(hi, q);
    }
    INFO(cache.history[2] << "; " << cache.history[3] << "; display/stack G " << lo << " - " << hi << ", brightest " << top);
    CHECK(Contains(cache.history[3], "linear"));
    CHECK(hi / lo - 1 <= 1e-5);
    CHECK(top <= 0.95 + 1e-6);
    CHECK(top >= 0.94);
}

TEST_CASE("planet chroma blur does not scale with the frame's width") {
    // Final review: the Moon's chroma blur scales by width / 2000 above 2000 px (a frame-filling disc: the colour's
    // resolution against the disc); a planet's disc is the same size in a full 3840 px frame as in a 1920 px one, so
    // its blur is the slider's px. The same disc (noise-free, no background: the 1920 frame is the 3840 frame's left
    // half exactly) gives the same blur in HISTORY and the same output over the disc.
    auto run = [](int width) {
        PlanetSceneSpec s = Framed(20);
        s.width = width;
        s.height = 120;
        s.cx = 80.3;
        s.cy = 59.8;
        s.blurSigma = 1.2;
        s.tint = {1.2, 1.0, 0.8};
        const PlanetScene sc = MakePlanetScene(s);
        const stack::MineralCache cache = stack::PreparePlanet(sc.observed, ser::Target::Jupiter, 4, {});
        REQUIRE(cache.masks.diskCount > 0);
        return std::make_pair(cache, stack::FinishPlanet(cache, {}, 4));
    };
    const auto [c1, o1] = run(1920);
    const auto [c2, o2] = run(3840);
    INFO(o1.history[6] << "\n" << o2.history[6]);
    CHECK(Contains(o1.history[6], "chroma blur 1.00 px"));
    CHECK(Contains(o2.history[6], "chroma blur 1.00 px"));
    double worst = 0;
    size_t disc = 0;
    for (int y = 0; y < 120; ++y)
        for (int x = 0; x < 160; ++x) {
            const size_t i1 = static_cast<size_t>(y) * 1920 + x, i2 = static_cast<size_t>(y) * 3840 + x;
            if (!c1.masks.disk[i1]) continue;
            ++disc;
            for (int c = 0; c < 3; ++c)
                worst = std::max(worst, std::fabs(static_cast<double>(o1.image.data[i1 * 3 + c]) - o2.image.data[i2 * 3 + c]));
        }
    INFO("disc " << disc << " px, largest difference " << worst);
    REQUIRE(disc > 1000);
    CHECK(worst <= 1e-5);
}

TEST_CASE("tiny or absent disc completes") {
    // Ruling P1: no disc (a faint Neptune, a frame of sky) or a disc of a few px runs every step, each that cannot
    // run saying why; the image is valid.
    // r 2 under the seeing (blur 1.2: SegmentLunar's disc is the blurred blob, about 120 px), and r 1.5 unblurred and
    // bright (a disc whose unsaturated part is under the colour reference's 30 px).
    PlanetSceneSpec tiny = Realistic(2, 111);
    PlanetSceneSpec sharp = Realistic(2, 113);
    sharp.radius = 1.5;
    sharp.level = 3;  // its core saturated in the stack: under 30 unsaturated disc px
    sharp.blurSigma = 0;
    PlanetSceneSpec none = Realistic(2, 112);
    none.level = 0;
    // A sky pedestal and noise, no gradient: SegmentLunar's documented limit takes a frame of sky whose gradient lifts
    // one half 5 noise sigma over the other for a frame-filling disc (Mineral.h), and Realistic's does (0.010 over
    // noise 0.004) -- that case is "frame-filling disc completes"'s.
    none.gradientX = none.gradientY = {0, 0, 0};
    for (const PlanetSceneSpec* s : {&tiny, &sharp, &none}) {
        const PlanetScene sc = MakePlanetScene(*s);
        stack::MineralOutcome out;
        stack::MineralCache cache;
        REQUIRE_NOTHROW(cache = stack::PreparePlanet(sc.observed, ser::Target::Neptune, 4, {}));
        REQUIRE_NOTHROW(out = stack::FinishPlanet(cache, {}, 4));
        CheckValid(out, sc.observed);
        CheckHistory(out);
        std::string all;
        for (const std::string& h : out.history) all += h + "\n";
        INFO("level " << s->level << ", blur " << s->blurSigma << ", notice '" << out.notice << "':\n" << all);
        if (s == &none) {
            CHECK_FALSE(out.notice.empty());
            CHECK(Contains(out.history[0], "none"));
            for (size_t k = 1; k < out.history.size(); ++k) CHECK(Contains(out.history[k], "skipped: " + out.notice));
        }
        if (s == &sharp) REQUIRE(cache.masks.diskCount > 0);
        if (s != &tiny) CHECK(Contains(out.history[1], "skipped"));  // none, or under 30 unsaturated disc px
    }
}

TEST_CASE("frame-filling disc completes") {
    // A disc larger than the frame, no sky (a MoonSynth frame run as Jupiter): every step runs or says why.
    planetary::testing::MoonSceneSpec s;
    s.width = 320;
    s.height = 240;
    s.cx = 160;
    s.cy = 120;
    s.radius = 260;
    s.cast = {1.1, 1.0, 0.85};
    s.blurSigma = 1.2;
    s.noiseStd = 0.003;
    const planetary::testing::MoonScene sc = planetary::testing::MakeMoonScene(s);
    stack::MineralOutcome out;
    REQUIRE_NOTHROW(out = stack::FinishPlanet(stack::PreparePlanet(sc.observed, ser::Target::Jupiter, 4, {}), {}, 4));
    CheckValid(out, sc.observed);
    CheckHistory(out);
    std::string all;
    for (const std::string& h : out.history) all += h + "\n";
    INFO(all);
    CHECK(out.notice.empty());
}

TEST_CASE("cancel throws StackCancelled") {
    const PlanetScene sc = MakePlanetScene(Realistic(20, 121));
    // At the first report, before PreparePlanet's last report (5 of 7, before the Sharpen), and at the end (7 of 7).
    for (int64_t at : {int64_t(0), int64_t(5), int64_t(7)}) {
        int64_t last = -1;
        stack::Progress pr;
        pr.report = [&](const std::string& phase, int64_t done, int64_t total) {
            if (phase == stack::kPhasePlanet) {
                last = done;
                CHECK(total == 7);
            }
        };
        pr.cancelled = [&] { return last == at; };
        INFO("cancel at " << at << ", last report " << last);
        stack::MineralOutcome o;
        if (at < 6) {
            CHECK_THROWS_AS(stack::PreparePlanet(sc.observed, ser::Target::Saturn, 4, pr), stack::StackCancelled);
        } else {
            const stack::MineralCache cache = stack::PreparePlanet(sc.observed, ser::Target::Saturn, 4, pr);
            CHECK_THROWS_AS(o = stack::FinishPlanet(cache, {}, 4, pr), stack::StackCancelled);
            CHECK(o.image.data.empty());
        }
        CHECK(last == at);
    }
}

TEST_CASE("finish is cheap") {
    // Spec acceptance 8: a slider move (FinishPlanet on the cache) takes <= 0.2 s at 1920 x 1080 on 32 threads.
#ifndef NDEBUG
    SKIP("timed in a Release build only");
#endif
    PlanetSceneSpec s = Realistic(200, 131);
    s.width = 1920;
    s.height = 1080;
    s.cx = 960.3;
    s.cy = 539.8;
    const PlanetScene sc = MakePlanetScene(s);
    const int threads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    const stack::MineralCache cache = stack::PreparePlanet(sc.observed, ser::Target::Jupiter, threads, {});
    double best = 1e9;
    for (double sat : {stack::kPlanetSaturationMax, 0.0, stack::kPlanetSaturationDefault}) {
        stack::PlanetParams p;
        p.saturation = sat;
        p.sharpen = stack::kPlanetSharpenMax;
        p.chromaBlurSigma = stack::kPlanetChromaBlurMax;
        const auto t0 = std::chrono::steady_clock::now();
        stack::FinishPlanet(cache, p, threads);
        best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    }
    WARN(threads << " threads: a slider move (FinishPlanet) " << best << " s at 1920 x 1080");
    if (threads >= 32) CHECK(best <= 0.2);
    else WARN("fewer than 32 threads here: not judged");
}
