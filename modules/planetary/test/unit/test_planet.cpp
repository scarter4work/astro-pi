// Planet processing (spec 2026-10-29-planetary-planet-processing-design.md, call 3 and Ruling P1; plan Task 2):
// the catalogue colour calibration, the ring's place in the disc mask, and small-disc dispersion alignment, each
// against its requirement on synthetic planets whose truth is known (PlanetSynth.h).
#include <catch_amalgamated.hpp>

#include <cmath>
#include <cstdint>
#include <random>
#include <string>
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
