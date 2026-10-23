#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <vector>

#include "Align.h"
#include "Measure.h"
#include "Regions.h"
#include "StackSynth.h"

using namespace planetary;
using ser::FloatImage;
using stack::AxisWeight;
using stack::FrameRegions;
using stack::RegionLayout;
using stack::RegionMeasurer;
using stack::Shift;
using testing::FrameSpec;
using testing::Scene;

namespace {

FloatImage Render(const Scene& s, int w, int h, const FrameSpec& f, unsigned seed) {
    return FloatImage{w, h, 1, testing::RenderFrame(s, w, h, f, seed)};
}

// The scene itself, noise-free, at blur `sigma`: what a reference looks like.
FloatImage Truth(const Scene& s, int w, int h, double sigma = 0) {
    FloatImage im{w, h, 1, std::vector<float>(static_cast<size_t>(w) * h)};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) im.data[static_cast<size_t>(y) * w + x] = static_cast<float>(s.Eval(x, y, sigma));
    return im;
}

testing::Field Constant(double v) {
    testing::Field f;
    f.nx = f.ny = 1;
    f.v = {v};
    return f;
}

// Sigma field: `sharp` in the cells of `cell` px where pick(i, j) is true, `blurred` elsewhere,
// with hard cell boundaries (sin^2 blended over one cell, like the generator's fields).
testing::Field Cells(int w, int h, double cell, double sharp, double blurred, const std::function<bool(int, int)>& pick) {
    testing::Field f;
    f.cell = cell;
    f.ox = f.oy = -cell / 2;
    f.nx = static_cast<int>(std::ceil(w / cell)) + 2;
    f.ny = static_cast<int>(std::ceil(h / cell)) + 2;
    for (int j = 0; j < f.ny; ++j)
        for (int i = 0; i < f.nx; ++i) f.v.push_back(pick(i, j) ? sharp : blurred);
    return f;
}

double RegionCentreX(const stack::Region& r) { return 0.5 * (r.x0 + r.x1); }
double RegionCentreY(const stack::Region& r) { return 0.5 * (r.y0 + r.y1); }

}  // namespace

TEST_CASE("Region weights are a partition of unity, flat to the edges for a full frame", "[regions]") {
    const int w = 250, h = 190;
    const Scene s = testing::MakeSurface(3, w, h);
    const FloatImage ref = Truth(s, w, h, 0.5);
    const RegionLayout lay = stack::PlaceRegions(ref, stack::MeasureFrame(ref, 0, 1), true, stack::NoiseModel{});
    REQUIRE(lay.flatEdges);
    REQUIRE(lay.size % 8 == 0);
    REQUIRE(lay.step * 2 == lay.size);
    for (bool vertical : {false, true}) {
        const int n = vertical ? h : w, nodes = vertical ? lay.ny : lay.nx;
        std::vector<double> prev(static_cast<size_t>(nodes), -1);
        for (double u = -0.5; u <= n - 0.5; u += 0.125) {
            const AxisWeight a = stack::AxisWeightAt(lay, vertical, u);
            CHECK(a.w0 + a.w1 == Catch::Approx(1).margin(1e-12));
            // Each node's weight is continuous in u.
            for (int j = 0; j < nodes; ++j) {
                const double wj = a.i0 == j ? a.w0 : a.i0 + 1 == j ? a.w1 : 0.0;
                if (prev[static_cast<size_t>(j)] >= 0) CHECK(std::fabs(wj - prev[static_cast<size_t>(j)]) < 0.05);
                prev[static_cast<size_t>(j)] = wj;
            }
        }
    }
    // Every node of the surface holds structure, and the windows cover the frame.
    CHECK(static_cast<int>(lay.regions.size()) == lay.nx * lay.ny);
    int maxX = 0, maxY = 0;
    for (const auto& r : lay.regions) {
        CHECK(r.shiftNoise > 0);
        CHECK(r.shiftNoise <= stack::kMaxShiftNoise);
        maxX = std::max(maxX, r.x1);
        maxY = std::max(maxY, r.y1);
        CHECK(r.x0 >= 0);
        CHECK(r.y0 >= 0);
    }
    CHECK(maxX == w);
    CHECK(maxY == h);
}

TEST_CASE("Regions on a 40 px disc cover the disc fully and taper to 0 in the sky", "[regions]") {
    const int w = 128, h = 112;
    const Scene s = testing::MakeDisc(5, 61.3, 54.7, 20);
    const FloatImage ref = Truth(s, w, h, 0.6);
    const stack::FrameStats st = stack::MeasureFrame(ref, 0, 1);
    REQUIRE(st.targetFound);
    const RegionLayout lay = stack::PlaceRegions(ref, st, false, stack::NoiseModel{});
    CHECK_FALSE(lay.flatEdges);
    CHECK(lay.size == stack::kMinRegionSize);
    REQUIRE(!lay.regions.empty());
    // Total weight of the placed regions at a point.
    auto cover = [&](double u, double v) {
        const AxisWeight ax = stack::AxisWeightAt(lay, false, u), ay = stack::AxisWeightAt(lay, true, v);
        double sum = 0;
        for (int b = 0; b < 2; ++b)
            for (int a = 0; a < 2; ++a) {
                const int i = ax.i0 + a, j = ay.i0 + b;
                if (i < 0 || j < 0 || i >= lay.nx || j >= lay.ny) continue;
                if (lay.node[static_cast<size_t>(j) * lay.nx + i] < 0) continue;
                sum += (a ? ax.w1 : ax.w0) * (b ? ay.w1 : ay.w0);
            }
        return sum;
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const double r = std::hypot(x - s.cx, y - s.cy);
            if (r <= s.radius + 2) CHECK(cover(x, y) == Catch::Approx(1).margin(1e-9));
        }
    CHECK(cover(2, 2) == 0);
    CHECK(cover(w - 2, h - 2) == 0);
    for (const auto& r : lay.regions) {  // every region touches the target's box
        CHECK(r.x1 > st.bx0);
        CHECK(r.x0 < st.bx1);
        CHECK(r.y1 > st.by0);
        CHECK(r.y0 < st.by1);
    }
}

TEST_CASE("No region on a featureless frame; a sky-only node gets none", "[regions]") {
    const int w = 160, h = 120;
    FloatImage flat{w, h, 1, std::vector<float>(static_cast<size_t>(w) * h, 0.4f)};
    std::mt19937 rng(1);
    std::normal_distribution<double> n(0, 0.002);
    for (float& v : flat.data) v += static_cast<float>(n(rng));
    const RegionLayout lay = stack::PlaceRegions(flat, stack::MeasureFrame(flat, 0, 1), true, testing::WhiteNoise(0.008));
    CHECK(lay.regions.empty());
    CHECK(lay.nx * lay.ny == static_cast<int>(lay.node.size()));
    for (int v : lay.node) CHECK(v == -1);
}

TEST_CASE("Region sizes follow the target", "[regions]") {
    // Frame-filling 1080p-mono-like proportions: shorter side / 8, capped at kMaxRegionSize.
    {
        const int w = 400, h = 300;
        const FloatImage ref = Truth(testing::MakeSurface(9, w, h), w, h, 0.5);
        const RegionLayout lay = stack::PlaceRegions(ref, stack::MeasureFrame(ref, 0, 1), true, stack::NoiseModel{});
        CHECK(lay.size == 40);  // 300 / 8 = 37.5 -> multiple of 8 -> 40
    }
    // A big disc: a quarter of its box.
    {
        const int w = 400, h = 360;
        const Scene s = testing::MakeDisc(2, 200, 180, 120);
        const FloatImage ref = Truth(s, w, h, 0.6);
        const stack::FrameStats st = stack::MeasureFrame(ref, 0, 1);
        const RegionLayout lay = stack::PlaceRegions(ref, st, false, stack::NoiseModel{});
        const int side = std::min(st.bx1 - st.bx0, st.by1 - st.by0);
        CHECK(lay.size == std::clamp(8 * static_cast<int>(std::lround(side / 4.0 / 8.0)), stack::kMinRegionSize,
                                     stack::kMaxRegionSize));
    }
}

TEST_CASE("PlaceRegions argument errors", "[regions]") {
    FloatImage bad{10, 10, 1, std::vector<float>(5)};
    CHECK_THROWS_AS(stack::PlaceRegions(bad, stack::FrameStats{}, true, stack::NoiseModel{}), std::invalid_argument);
    FloatImage rgb{10, 10, 3, std::vector<float>(300)};
    CHECK_THROWS_AS(stack::PlaceRegions(rgb, stack::FrameStats{}, true, stack::NoiseModel{}), std::invalid_argument);
    FloatImage sky{64, 64, 1, std::vector<float>(64 * 64, 0.1f)};
    CHECK_THROWS_AS(stack::PlaceRegions(sky, stack::FrameStats{}, false, stack::NoiseModel{}), std::invalid_argument);  // disc without target
}

TEST_CASE("Local shifts: sub-pixel, up to 8 px, and a torn frame's halves", "[regions]") {
    const int w = 256, h = 192;
    const Scene s = testing::MakeSurface(11, w, h);
    const FloatImage ref = Truth(s, w, h, 0.6);
    const RegionLayout lay = stack::PlaceRegions(ref, stack::MeasureFrame(ref, 0, 1), true, testing::WhiteNoise(0.01));
    const RegionMeasurer m(ref, lay);
    REQUIRE(lay.regions.size() > 20);

    SECTION("sub-pixel local shift under noise") {
        FrameSpec f;
        f.gx = 3.3;
        f.gy = -2.2;
        f.hasLocal = true;
        f.dx = Constant(0.37);
        f.dy = Constant(-0.61);
        f.sigma = Constant(0.8);
        f.noise = 0.01;
        const FloatImage fr = Render(s, w, h, f, 7);
        const FrameRegions r = m.Measure(fr, 42, Shift{3.3, -2.2, 1});
        CHECK(r.frameIndex == 42);
        REQUIRE(r.regions.size() == lay.regions.size());
        // One frame's shift error is noise-limited: sigma_d ~ s sqrt(2 / sum |grad T|^2), 0.03-0.05 px
        // for these 32-px windows at noise 0.01 (shiftNoise x s / kMinFrameNoise / sqrt 2).
        double sum2 = 0, worst = 0;
        int aligned = 0;
        for (size_t i = 0; i < r.regions.size(); ++i) {
            const auto& q = r.regions[i];
            if (!q.aligned) continue;
            ++aligned;
            const double ex = q.dx - 0.37, ey = q.dy + 0.61;
            sum2 += ex * ex + ey * ey;
            worst = std::max({worst, std::fabs(ex), std::fabs(ey)});
            CHECK(q.confidence > 0.8);
            CHECK(q.quality > 0);
            CHECK(q.qualitySigma > 0);
        }
        CHECK(aligned == static_cast<int>(r.regions.size()));
        const double rms = std::sqrt(sum2 / (2.0 * aligned));
        INFO("rms " << rms << " worst " << worst);
        CHECK(rms < 0.05);
        CHECK(worst < 0.15);
        CHECK(r.gain == Catch::Approx(1).margin(0.01));
    }
    SECTION("an 8 px error of the global shift is found locally") {
        FrameSpec f;
        f.gx = 1.0;
        f.gy = 2.0;
        f.noise = 0.01;
        const FloatImage fr = Render(s, w, h, f, 8);
        const FrameRegions r = m.Measure(fr, 0, Shift{1.0 - 8.0, 2.0 + 7.5, 1});
        int good = 0, interior = 0;
        for (size_t i = 0; i < r.regions.size(); ++i) {
            const auto& reg = lay.regions[i];
            // Windows whose shifted copy stays inside the frame.
            if (reg.x0 < 10 || reg.y0 < 10 || reg.x1 > w - 10 || reg.y1 > h - 10) continue;
            ++interior;
            const auto& q = r.regions[i];
            if (q.aligned && std::fabs(q.dx - 8.0) < 0.08 && std::fabs(q.dy + 7.5) < 0.08) ++good;
        }
        REQUIRE(interior > 10);
        CHECK(good == interior);
    }
    SECTION("a torn frame: each half reports its own shift (tears up to 6 px)") {
        for (double tear : {4.0, 6.0}) {
            FrameSpec f;
            f.noise = 0.01;
            f.tearRow = h / 2;
            f.tearDx = tear;
            f.tearDy = -0.8 * tear;
            const FloatImage fr = Render(s, w, h, f, 9);
            const FrameRegions r = m.Measure(fr, 0, Shift{0, 0, 1});
            int upper = 0, lower = 0, upperOk = 0, lowerOk = 0;
            for (size_t i = 0; i < r.regions.size(); ++i) {
                const auto& reg = lay.regions[i];
                const auto& q = r.regions[i];
                if (reg.y1 <= h / 2 - 4) {
                    ++upper;
                    upperOk += q.aligned && std::fabs(q.dx) < 0.05 && std::fabs(q.dy) < 0.05 && q.confidence > 0.8;
                } else if (reg.y0 >= h / 2 + 4 && reg.y1 <= h - 8 && reg.x1 <= w - 8) {
                    ++lower;
                    lowerOk += q.aligned && std::fabs(q.dx - tear) < 0.1 && std::fabs(q.dy + 0.8 * tear) < 0.1 &&
                               q.confidence > 0.8;
                }
            }
            INFO("tear " << tear);
            REQUIRE(upper > 5);
            REQUIRE(lower > 5);
            CHECK(upperOk == upper);
            CHECK(lowerOk == lower);
        }
    }
    SECTION("the one-call form matches") {
        FrameSpec f;
        f.gx = 0.4;
        const FloatImage fr = Render(s, w, h, f, 10);
        const FrameRegions a = m.Measure(fr, 0, Shift{0.4, 0, 1});
        const FrameRegions b = stack::MeasureRegions(ref, lay, fr, Shift{0.4, 0, 1});
        REQUIRE(a.regions.size() == b.regions.size());
        for (size_t i = 0; i < a.regions.size(); ++i) {
            CHECK(a.regions[i].dx == b.regions[i].dx);
            CHECK(a.regions[i].quality == b.regions[i].quality);
        }
    }
    SECTION("size mismatch") {
        FloatImage small{w / 2, h, 1, std::vector<float>(static_cast<size_t>(w / 2) * h)};
        CHECK_THROWS_AS(m.Measure(small, 0, Shift{}), std::invalid_argument);
    }
}

TEST_CASE("Region quality follows the local blur; gain follows the frame's brightness", "[regions]") {
    const int w = 256, h = 192;
    const Scene s = testing::MakeSurface(12, w, h);
    const FloatImage ref = Truth(s, w, h, 0.6);
    const RegionLayout lay = stack::PlaceRegions(ref, stack::MeasureFrame(ref, 0, 1), true, testing::WhiteNoise(0.005));
    const RegionMeasurer m(ref, lay);
    // Frame A: sharp (0.5 px) left of x ~ 128 and blurred (2.5 px) right of it (sin^2 transition over
    // one 64-px cell); frame B the other way round, dimmed to 0.8.
    auto frame = [&](bool leftSharp, double gain, unsigned seed) {
        FrameSpec f;
        f.hasLocal = true;
        f.dx = Constant(0);
        f.dy = Constant(0);
        f.gain = gain;
        f.noise = 0.005;
        f.sigma = Cells(w, h, 64, 0.5, 2.5, [&](int i, int) { return (i <= 2) == leftSharp; });
        return m.Measure(Render(s, w, h, f, seed), 0, Shift{});
    };
    const FrameRegions a = frame(true, 1.0, 3), b = frame(false, 0.8, 4);
    int left = 0, right = 0;
    for (size_t i = 0; i < a.regions.size(); ++i) {
        const double cx = RegionCentreX(lay.regions[i]);
        const double qa = a.regions[i].quality, qb = b.regions[i].quality;
        INFO("region at x " << cx << ": sharp-left frame " << qa << ", sharp-right frame " << qb);
        if (cx < 80) {
            ++left;
            CHECK(qa > 1.3 * qb);
        }
        if (cx > 190) {
            ++right;
            CHECK(qb > 1.3 * qa);
        }
    }
    CHECK(left > 10);
    CHECK(right > 10);
    CHECK(a.gain == Catch::Approx(1.0).margin(0.01));
    CHECK(b.gain == Catch::Approx(0.8).margin(0.01));
}

TEST_CASE("PlanRegions: each region keeps the frames sharp there", "[regions]") {
    const int w = 256, h = 192;
    const Scene s = testing::MakeSurface(21, w, h);
    const FloatImage ref = Truth(s, w, h, 0.6);
    const RegionLayout lay = stack::PlaceRegions(ref, stack::MeasureFrame(ref, 0, 1), true, testing::WhiteNoise(0.01));
    const RegionMeasurer m(ref, lay);
    std::mt19937 rng(4);
    std::uniform_real_distribution<double> uni(0, 1);
    const int frames = 60;
    std::vector<testing::Field> sigmas;
    std::vector<FrameRegions> measured;
    for (int k = 0; k < frames; ++k) {
        FrameSpec f;
        f.hasLocal = true;
        f.dx = Constant(0);
        f.dy = Constant(0);
        f.noise = 0.01;
        std::vector<bool> sharp(64);
        for (auto&& b : sharp) b = uni(rng) < 0.3;
        f.sigma = Cells(w, h, 64, 0.5, 2.5, [&](int i, int j) { return sharp[static_cast<size_t>((j * 8 + i) % 64)]; });
        sigmas.push_back(f.sigma);
        measured.push_back(m.Measure(Render(s, w, h, f, 100 + k), 1000 + k, Shift{}));
    }
    std::reverse(measured.begin(), measured.end());  // order-independent
    const stack::RegionPlan plan = stack::PlanRegions(lay, measured);
    REQUIRE(plan.frames.size() == static_cast<size_t>(frames));
    CHECK(plan.frames.front() == 1000);
    CHECK(plan.Uses(1010));
    CHECK_FALSE(plan.Uses(5));
    int64_t keptTotal = 0, keptSharp = 0;
    for (size_t r = 0; r < plan.RegionCount(); ++r) {
        const auto& d = plan.detail[r];
        CHECK(d.kept >= 1);
        CHECK(d.kept <= d.candidates);
        CHECK(d.bestQuality > 0);
        CHECK(d.detailScale == Catch::Approx(1 / d.bestQuality));
        const double cx = RegionCentreX(lay.regions[r]), cy = RegionCentreY(lay.regions[r]);
        for (size_t f = 0; f < plan.frames.size(); ++f) {
            if (!(plan.flags[f * plan.RegionCount() + r] & stack::RegionPlan::kKept)) continue;
            ++keptTotal;
            keptSharp += sigmas[static_cast<size_t>(plan.frames[f] - 1000)].At(cx, cy) < 1.0;
        }
    }
    // Kept frames are the sharp ones at the region (a region straddling a cell boundary sees both).
    CHECK(static_cast<double>(keptSharp) > 0.85 * static_cast<double>(keptTotal));
    // ... and not everything (about 30% are sharp at any place).
    CHECK(static_cast<double>(keptTotal) < 0.6 * static_cast<double>(frames * plan.RegionCount()));
    REQUIRE(plan.bestRegion >= 0);
    // Only frames some region keeps are stacked at all (no region keeps -> no weight anywhere).
    for (size_t f = 0; f < plan.frames.size(); ++f) {
        bool kept = false;
        for (size_t r = 0; r < plan.RegionCount(); ++r) kept = kept || (plan.flags[f * plan.RegionCount() + r] & stack::RegionPlan::kKept);
        CHECK(plan.Uses(plan.frames[f]) == kept);
    }

    std::vector<FrameRegions> dup = measured;
    dup.push_back(measured.front());
    CHECK_THROWS_AS(stack::PlanRegions(lay, dup), std::invalid_argument);
}

TEST_CASE("A featureless region stacks every frame; textured ones keep their lucky frames (Ruling P27)", "[regions]") {
    // The surface of "each region keeps the frames sharp there", with a 64-px patch replaced by a flat
    // level and a faint smooth blob (amplitude 0.03, sigma 6 px): structure enough for a region to be
    // placed and aligned there, but gradient energy far under the noise's, so its frames' quality is
    // the noise floor's -- the Moon 193142's dark limb regions. Ranked, such a region kept a handful of
    // noise-floor frames (sqrt(frames / kept) times the noise of the mean).
    const int w = 256, h = 192, px0 = 64, py0 = 64, px1 = 128, py1 = 128;
    const Scene s = testing::MakeSurface(21, w, h);
    auto patch = [&](FloatImage& im, double noise, unsigned seed) {
        std::mt19937 rng(seed);
        std::normal_distribution<double> n(0, noise);
        for (int y = py0; y < py1; ++y)
            for (int x = px0; x < px1; ++x) {
                const double dx = x - 96.3, dy = y - 95.7;
                im.data[static_cast<size_t>(y) * w + x] =
                    static_cast<float>(0.4 + 0.03 * std::exp(-(dx * dx + dy * dy) / 72.0) + (noise > 0 ? n(rng) : 0.0));
            }
    };
    FloatImage ref = Truth(s, w, h, 0.6);
    patch(ref, 0, 0);
    const RegionLayout lay = stack::PlaceRegions(ref, stack::MeasureFrame(ref, 0, 1), true, testing::WhiteNoise(0.01));
    const RegionMeasurer m(ref, lay);
    std::mt19937 rng(4);
    std::uniform_real_distribution<double> uni(0, 1);
    const int frames = 80;
    std::vector<FrameRegions> measured;
    for (int k = 0; k < frames; ++k) {
        FrameSpec f;
        f.hasLocal = true;
        f.dx = Constant(0);
        f.dy = Constant(0);
        f.noise = 0.01;
        std::vector<bool> sharp(64);
        for (auto&& b : sharp) b = uni(rng) < 0.3;
        f.sigma = Cells(w, h, 64, 0.5, 2.5, [&](int i, int j) { return sharp[static_cast<size_t>((j * 8 + i) % 64)]; });
        FloatImage frame = Render(s, w, h, f, 300 + k);
        patch(frame, 0.01, 700u + static_cast<unsigned>(k));
        measured.push_back(m.Measure(frame, k, Shift{}));
    }
    const stack::RegionPlan plan = stack::PlanRegions(lay, measured);
    int flat = 0, textured = 0;
    for (size_t r = 0; r < plan.RegionCount(); ++r) {
        const auto& reg = lay.regions[r];
        const auto& d = plan.detail[r];
        const bool inside = reg.x0 >= px0 && reg.x1 <= px1 && reg.y0 >= py0 && reg.y1 <= py1;
        const bool clear = reg.x1 <= px0 || reg.x0 >= px1 || reg.y1 <= py0 || reg.y0 >= py1;
        std::vector<double> ratio;
        for (const FrameRegions& fr : measured)
            if (fr.regions[r].aligned && fr.regions[r].quality > 0) ratio.push_back(fr.regions[r].quality / fr.regions[r].qualitySigma);
        std::sort(ratio.begin(), ratio.end());
        INFO("region at " << reg.x0 << "," << reg.y0 << ": candidates " << d.candidates << ", kept " << d.kept
                          << (d.noiseOnly ? ", noise only" : "") << ", median q / qualitySigma "
                          << (ratio.empty() ? 0.0 : ratio[ratio.size() / 2]));
        if (inside) {
            ++flat;
            CHECK(d.candidates > 0);  // the case that matters: noise-floor "detail" in some frames
            CHECK(d.noiseOnly);
            CHECK(d.kept == 0);       // the fallback: the plain mean of the stacked frames
        } else if (clear) {
            ++textured;
            CHECK_FALSE(d.noiseOnly);
            CHECK(d.kept >= 1);
            CHECK(d.kept < d.candidates);  // a lucky subset
        }
    }
    CHECK(flat >= 1);
    CHECK(textured >= 10);
}

TEST_CASE("A low-SNR region with real seeing variation keeps a lucky subset (P27 review round 3)", "[regions]") {
    // The surface at half contrast, noise 0.01, seeing changing from frame to frame (sigma uniform in
    // 0.5-2.5 px over the whole frame): per region a median q / qualitySigma of ~3.3-12, where
    // qualitySigma overstates q's scatter (Ruling P26). An override that stacked every aligned frame
    // when the spread looked like noise fired on ~40 of these regions and threw their sharper frames
    // away. The ranking must keep a subset there, of frames truly sharper than the average.
    const int w = 256, h = 192, frames = 150;
    const Scene s = testing::MakeSurface(21, w, h);
    FloatImage ref = Truth(s, w, h, 0.6);
    double level = 0;
    for (const float v : ref.data) level += v;
    level /= static_cast<double>(ref.data.size());
    for (float& v : ref.data) v = static_cast<float>(0.4 + 0.5 * (v - level));
    const RegionLayout lay = stack::PlaceRegions(ref, stack::MeasureFrame(ref, 0, 1), true, testing::WhiteNoise(0.01));
    const RegionMeasurer m(ref, lay);
    std::mt19937 rng(9);
    std::uniform_real_distribution<double> uni(0, 1);
    std::normal_distribution<double> noise(0, 0.01);
    std::vector<FrameRegions> measured;
    std::vector<double> blur;
    for (int k = 0; k < frames; ++k) {
        FrameSpec f;
        f.hasLocal = true;
        f.dx = Constant(0);
        f.dy = Constant(0);
        f.noise = 0;
        const double sg = 0.5 + 2.0 * uni(rng);
        f.sigma = Constant(sg);
        FloatImage frame = Render(s, w, h, f, 100u + static_cast<unsigned>(k));
        for (float& v : frame.data) v = static_cast<float>(0.4 + 0.5 * (v - level) + noise(rng));
        blur.push_back(sg);
        measured.push_back(m.Measure(frame, k, Shift{}));
    }
    const stack::RegionPlan plan = stack::PlanRegions(lay, measured);
    const size_t nr = plan.RegionCount();
    int judged = 0, sharper = 0;
    for (size_t r = 0; r < nr; ++r) {
        const auto& d = plan.detail[r];
        if (d.noiseOnly) continue;
        double keptBlur = 0, allBlur = 0;
        int64_t usable = 0;
        for (size_t f = 0; f < plan.frames.size(); ++f) {
            const uint8_t fl = plan.flags[f * nr + r];
            if (!(fl & stack::RegionPlan::kShiftUsable)) continue;
            ++usable;
            allBlur += blur[static_cast<size_t>(plan.frames[f])];
            if (fl & stack::RegionPlan::kKept) keptBlur += blur[static_cast<size_t>(plan.frames[f])];
        }
        REQUIRE(d.kept > 0);
        keptBlur /= static_cast<double>(d.kept);
        allBlur /= static_cast<double>(usable);
        INFO("region " << r << ": kept " << d.kept << " of " << usable << " aligned, true blur kept " << keptBlur << " / all "
                       << allBlur);
        ++judged;
        CHECK(d.kept < usable);              // a subset, never every aligned frame
        sharper += keptBlur < allBlur - 0.25;  // of frames truly sharper
    }
    INFO(sharper << " of " << judged << " regions kept frames >= 0.25 px sharper than the average");
    CHECK(judged >= 100);
    CHECK(sharper >= 0.6 * judged);
}

TEST_CASE("A region with too few candidates takes the plain mean (P27 review round 3)", "[regions]") {
    // Alignment collapsed in one region: 2 of 200 frames align there, both with a quality in the noise's
    // tail (6 qualitySigma: the most of ~200 noise readings). The ratio test alone passed them and the
    // region stacked them alone; with fewer than kMinStackFrames candidates it takes the plain mean.
    const int w = 192, h = 160;
    const Scene s = testing::MakeSurface(23, w, h);
    const FloatImage ref = Truth(s, w, h, 0.6);
    const RegionLayout lay = stack::PlaceRegions(ref, stack::MeasureFrame(ref, 0, 1), true, stack::NoiseModel{});
    REQUIRE(lay.regions.size() >= 2);
    std::mt19937 rng(8);
    std::normal_distribution<double> n(0, 1);
    std::vector<FrameRegions> measured;
    for (int k = 0; k < 200; ++k) {
        FrameRegions fr;
        fr.frameIndex = k;
        for (size_t r = 0; r < lay.regions.size(); ++r) {
            stack::RegionMeasure q;
            q.confidence = 0.95f;
            if (r == 0) {
                q.aligned = k < 2;
                q.quality = 0.06f;
                q.qualitySigma = 0.01f;
            } else {
                q.aligned = true;
                q.quality = static_cast<float>(0.3 * (1 + 0.05 * n(rng)));
                q.qualitySigma = 0.002f;
            }
            fr.regions.push_back(q);
        }
        measured.push_back(fr);
    }
    const stack::RegionPlan plan = stack::PlanRegions(lay, measured);
    CHECK(plan.detail[0].candidates == 2);
    CHECK(plan.detail[0].noiseOnly);
    CHECK(plan.detail[0].kept == 0);
    for (size_t r = 1; r < plan.RegionCount(); ++r) {
        CHECK_FALSE(plan.detail[r].noiseOnly);
        CHECK(plan.detail[r].kept > 0);
    }
}

TEST_CASE("An exceptionally sharp frame is kept by the regions, not fenced off (Ruling P8)", "[regions]") {
    // Select holds frames above its fence as suspects and admits those that align (Ruling P6); every
    // frame reaching the regions has passed alignment, and in a region a frame passes the region's own
    // confidence cut too. A lucky moment far sharper than the rest must count where it is sharp.
    const int w = 192, h = 160;
    const Scene s = testing::MakeSurface(23, w, h);
    const FloatImage ref = Truth(s, w, h, 0.6);
    const RegionLayout lay = stack::PlaceRegions(ref, stack::MeasureFrame(ref, 0, 1), true, stack::NoiseModel{});
    std::mt19937 rng(8);
    std::normal_distribution<double> n(0, 1);
    std::vector<FrameRegions> measured;
    for (int k = 0; k < 60; ++k) {
        FrameRegions m;
        m.frameIndex = k;
        for (size_t r = 0; r < lay.regions.size(); ++r) {
            stack::RegionMeasure q;
            q.aligned = true;
            q.confidence = 0.95f;
            q.quality = static_cast<float>(k == 7 ? 0.9 : 0.3 * (1 + 0.01 * n(rng)));
            q.qualitySigma = 0.002f;
            m.regions.push_back(q);
        }
        measured.push_back(m);
    }
    const stack::RegionPlan plan = stack::PlanRegions(lay, measured);
    const int64_t pos = plan.Find(7);
    REQUIRE(pos >= 0);
    for (size_t r = 0; r < plan.RegionCount(); ++r)
        CHECK((plan.flags[static_cast<size_t>(pos) * plan.RegionCount() + r] & stack::RegionPlan::kKept) != 0);
}

TEST_CASE("an unmeasured capture model: each frame's regions take its own noise (Ruling P26 fix round 2, N2)", "[regions][noise]") {
    // A capture of 1-3 frames has no model (NoiseModel::Unmeasured): RegionMeasurer reads each frame's own
    // texture-blind noise over the regions' extent, as before Ruling P26, instead of none. Against the true
    // noise as the model, the qualities agree to a few percent and qualitySigma is there.
    const int w = 256, h = 192;
    const Scene s = testing::MakeSurface(11, w, h);
    const FloatImage ref = Truth(s, w, h, 0.6);
    const RegionLayout known = stack::PlaceRegions(ref, stack::MeasureFrame(ref, 0, 1), true, testing::WhiteNoise(0.01));
    const RegionLayout unmeasured = stack::PlaceRegions(ref, stack::MeasureFrame(ref, 0, 1), true, stack::NoiseModel::Unmeasured());
    const RegionLayout none = stack::PlaceRegions(ref, stack::MeasureFrame(ref, 0, 1), true, stack::NoiseModel{});
    REQUIRE(known.regions.size() == unmeasured.regions.size());
    FrameSpec f;
    f.hasLocal = true;
    f.dx = Constant(0);
    f.dy = Constant(0);
    f.sigma = Constant(1.0);
    f.noise = 0.01;
    const FloatImage fr = Render(s, w, h, f, 9);
    const FrameRegions a = RegionMeasurer(ref, known).Measure(fr, 0, Shift{});
    const FrameRegions b = RegionMeasurer(ref, unmeasured).Measure(fr, 0, Shift{});
    const FrameRegions z = RegionMeasurer(ref, none).Measure(fr, 0, Shift{});
    int compared = 0;
    for (size_t r = 0; r < a.regions.size(); ++r) {
        if (!(a.regions[r].aligned && b.regions[r].aligned && a.regions[r].quality > 0)) continue;
        ++compared;
        INFO("region " << r << ": quality " << b.regions[r].quality << " against " << a.regions[r].quality << " with the true noise");
        CHECK(b.regions[r].quality == Catch::Approx(a.regions[r].quality).epsilon(0.05));
        CHECK(b.regions[r].qualitySigma > 0);
        CHECK(z.regions[r].qualitySigma == 0);  // a measured zero: no noise to propagate
        CHECK(z.regions[r].quality > a.regions[r].quality);  // and none subtracted
    }
    CHECK(compared > 20);
}

TEST_CASE("a region's qualitySigma tracks its quality's scatter from low to high SNR (Ruling P26)", "[regions][noise]") {
    // A 64 x 64 window of texture (12 sinusoids, contrast A) at white noise 0.01 over 2000 frames, the
    // capture's noise model the truth, the window mean as background (as RegionMeasurer has it). Median
    // q / qualitySigma 3.4 to ~1700 from A 0.0012 to 0.05. Before Ruling P26 (measured with the true noise
    // as the frame's level, 4000 frames): quality's scatter was 0.50 / 0.81 / 0.98 of qualitySigma's rms at
    // a median ratio of 4.1 / 6.9 / 15 (qualitySigma taken at each frame's noisy quality, whose low draws
    // it blew up) and 1.11 / 1.15 at 1861 / 8200 (the window's edge pixels missing from the noise's linear
    // term); with each frame's own noise reading, as the engine had it, 0.32-0.67 throughout. Under a
    // median ratio of ~3.1 a window is noise in both its gradient and its signal energy (pure noise reads
    // 2.9), where Ruling P27's plain mean takes over.
    const int n = 64, runs = 2000;
    const stack::NoiseModel model = testing::WhiteNoise(0.01);
    std::mt19937 tr(5);
    std::uniform_real_distribution<double> u(0, 1);
    struct Wave {
        double fx, fy, ph, a;
    };
    std::vector<Wave> waves;
    for (int k = 0; k < 12; ++k) waves.push_back({0.02 + 0.15 * u(tr), 0.02 + 0.15 * u(tr), 2 * M_PI * u(tr), 0.5 + u(tr)});
    for (const double A : {0.0012, 0.0015, 0.002, 0.004, 0.01, 0.05}) {
        std::vector<float> clean(static_cast<size_t>(n) * n);
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                double v = 0.3;
                for (const Wave& wv : waves) v += A * wv.a * std::sin(2 * M_PI * (wv.fx * x + wv.fy * y) + wv.ph);
                clean[static_cast<size_t>(y) * n + x] = static_cast<float>(v);
            }
        std::mt19937 rng(77);
        std::normal_distribution<double> g(0, 0.01);
        std::vector<double> q, sg, ratio;
        for (int k = 0; k < runs; ++k) {
            std::vector<float> im = clean;
            double mean = 0;
            for (float& v : im) {
                v = static_cast<float>(v + g(rng));
                mean += v;
            }
            const stack::QualityEstimate e = stack::RegionQuality(im.data(), n, 0, 0, n, n, mean / (n * n), model);
            if (!(e.quality > 0)) continue;
            q.push_back(e.quality);
            sg.push_back(e.sigma);
            ratio.push_back(e.quality / e.sigma);
        }
        double m = 0, m2 = 0, s2 = 0;
        for (size_t i = 0; i < q.size(); ++i) {
            m += q[i];
            m2 += q[i] * q[i];
            s2 += sg[i] * sg[i];
        }
        const double N = static_cast<double>(q.size());
        m /= N;
        const double empirical = std::sqrt((m2 - N * m * m) / (N - 1)), rms = std::sqrt(s2 / N);
        std::sort(sg.begin(), sg.end());
        std::sort(ratio.begin(), ratio.end());
        INFO("contrast " << A << ": " << q.size() << " frames with detail, median q / qualitySigma " << ratio[ratio.size() / 2]
                         << "; scatter " << empirical << " against qualitySigma rms " << rms << " / median " << sg[sg.size() / 2]);
        REQUIRE(q.size() >= runs * 8 / 10);
        CHECK(empirical / rms == Catch::Approx(1).margin(0.1));
        CHECK(empirical / sg[sg.size() / 2] == Catch::Approx(1).margin(0.1));
    }
    // Under that (Ruling P26 fix round 1, M8): the same window at A 0 / 0.0004 / 0.0006 / 0.0008 / 0.001 reads a
    // median ratio of 2.83 / 3.01 / 3.04 / 3.05 / 3.12 -- no window's median ratio falls below pure noise's
    // ~2.8, the frames with q > 0 being those the noise lifted -- and there quality's scatter is 0.33 / 0.34 /
    // 0.54 / 0.84 / 0.89 of qualitySigma's rms: qualitySigma overstates the scatter of a quality that is
    // the positive part of noise. Such a region's ratio is under kRegionDetailSigmas, so it takes the plain
    // mean (P27) and its qualitySigma ranks nothing. A frame's target at that SNR is not found at all (the
    // smooth blob of "qualitySigma tracks quality's scatter ..." at contrast 0.065: no frame with detail).
    for (const double A : {0.0, 0.0006}) {
        std::vector<float> clean(static_cast<size_t>(n) * n);
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                double v = 0.3;
                for (const Wave& wv : waves) v += A * wv.a * std::sin(2 * M_PI * (wv.fx * x + wv.fy * y) + wv.ph);
                clean[static_cast<size_t>(y) * n + x] = static_cast<float>(v);
            }
        std::mt19937 rng(77);
        std::normal_distribution<double> g(0, 0.01);
        std::vector<double> ratio;
        for (int k = 0; k < runs; ++k) {
            std::vector<float> im = clean;
            double mean = 0;
            for (float& v : im) {
                v = static_cast<float>(v + g(rng));
                mean += v;
            }
            const stack::QualityEstimate e = stack::RegionQuality(im.data(), n, 0, 0, n, n, mean / (n * n), model);
            if (e.quality > 0) ratio.push_back(e.quality / e.sigma);
        }
        std::sort(ratio.begin(), ratio.end());
        INFO("contrast " << A << ": median q / qualitySigma " << ratio[ratio.size() / 2] << " over " << ratio.size());
        CHECK(ratio[ratio.size() / 2] > 2.5);
        CHECK(ratio[ratio.size() / 2] < stack::kRegionDetailSigmas);
    }
}

TEST_CASE("RegionNoise is unbiased on pixels and on bins, chosen once per capture (Ruling P25)", "[regions][noise]") {
    // The region stage used to take, per frame, the smaller of the two readings: s^2 0.923 / 0.961 /
    // 0.981 / 0.989 / 0.995 of the truth over 32^2 / 64^2 / 128^2 / 256^2 / 512^2 (white noise), each
    // frame taking whichever fluctuated low. Each reading alone, white noise 0.01:
    for (const int n : {32, 48, 64, 128, 256, 512})
        for (const bool onBins : {false, true}) {
            const int runs = std::max(150, static_cast<int>(3e6 / (static_cast<double>(n) * n)));
            double sum = 0, sum2 = 0, reported = 0;
            for (int k = 0; k < runs; ++k) {
                std::mt19937 rng(4000u + 31u * static_cast<unsigned>(k) + static_cast<unsigned>(n) + (onBins ? 7u : 0u));
                std::normal_distribution<double> g(0, 0.01);
                FloatImage im{n, n, 1, std::vector<float>(static_cast<size_t>(n) * n)};
                for (float& v : im.data) v = static_cast<float>(0.3 + g(rng));
                const stack::NoiseEstimate e = stack::RegionNoise(im, 0, 0, n, n, onBins);
                const double v = e.sigma * e.sigma / 1e-4;
                sum += v;
                sum2 += v * v;
                reported += e.varSigma2 / 1e-8;
            }
            const double mean = sum / runs, var = (sum2 - runs * mean * mean) / (runs - 1);
            INFO(n << "^2 on " << (onBins ? "bins" : "pixels") << ": mean s^2 / truth " << mean << " over " << runs
                   << " runs; variance " << var << ", reported " << reported / runs);
            // Within 1%, but for 16 x 16 bins (196 responses, a mosaic's smallest extent): the trimmed
            // moment's small-sample excess, +2.2% measured.
            CHECK(mean == Catch::Approx(1).margin(onBins && n == 32 ? 0.025 : 0.01));
            CHECK(var >= 0.65 * reported / runs);  // the formula is one constant for 32^2-1024^2: 0.71-1.23 of it
            CHECK(var <= 1.5 * reported / runs);
        }
    // The choice, made on the reference: a mono disc (texture at a few px raises the bins' reading) is
    // read on the pixels; the same disc as a mono-labelled colour mosaic (sites R 1 / G 0.5 / B 0.35),
    // whose pattern the pixels read as noise, on the bins.
    const int w = 160, h = 160;
    const Scene s = testing::MakeDisc(3, 80.3, 79.8, 40);
    auto cfa = [&](FloatImage& im) {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                im.data[static_cast<size_t>(y) * w + x] *= y % 2 == 0 ? (x % 2 == 0 ? 1.0f : 0.5f) : (x % 2 == 0 ? 0.5f : 0.35f);
    };
    FrameSpec f;
    f.noise = 0.0025;  // a reference: the mean of 16 frames of noise 0.01
    const FloatImage mono{w, h, 1, testing::RenderFrame(s, w, h, f, 11)};
    FloatImage mosaic = mono;
    cfa(mosaic);
    CHECK_FALSE(stack::RegionNoiseOnBins(mono, 30, 30, 130, 130));
    CHECK(stack::RegionNoiseOnBins(mosaic, 30, 30, 130, 130));
    // ... and a frame of that mosaic: the pixels read its pattern as noise, the bins do not.
    f.noise = 0.01;
    FloatImage frame{w, h, 1, testing::RenderFrame(s, w, h, f, 12)};
    cfa(frame);
    const double onBins = stack::RegionNoise(frame, 30, 30, 130, 130, true).sigma;
    const double onPixels = stack::RegionNoise(frame, 30, 30, 130, 130, false).sigma;
    INFO("mosaic frame: bins " << onBins << ", pixels " << onPixels);
    CHECK(onPixels > 1.3 * onBins);
}

TEST_CASE("The blend mask is 1 over the target and its limb and 0 over the sky; a frame-filling surface without sky has none (E1)", "[regions]") {
    const int w = 160, h = 128;
    const double cx = 62.4, cy = 70.3, R = 16;
    const Scene s = testing::MakeDisc(11, cx, cy, R);
    std::mt19937 rng(3);
    std::normal_distribution<double> n(0, 1);
    FloatImage ref{w, h, 1, std::vector<float>(static_cast<size_t>(w) * h)};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)  // a reference: the disc blurred 1.5 px, a 16-frame mean's noise
            ref.data[static_cast<size_t>(y) * w + x] = static_cast<float>(0.02 + s.Eval(x, y, 1.5) + 0.0025 * n(rng));
    const stack::FrameStats st = stack::MeasureFrame(ref, 0, 1);
    REQUIRE(st.targetFound);
    const RegionLayout lay = stack::PlaceRegions(ref, st, false, testing::WhiteNoise(0.01));
    REQUIRE_FALSE(lay.regions.empty());
    REQUIRE_FALSE(lay.mask.Empty());
    // Over the disc and out past its limb (the blur's halo above 2 frame sd, then kMaskGrow): 1.
    for (double a = 0; a < 2 * M_PI; a += 0.1)
        for (double r : {0.0, 0.5 * R, R, R + 2}) CHECK(lay.mask.At(cx + r * std::cos(a), cy + r * std::sin(a)) == 1.0);
    // Sky beyond the taper: 0; in between, within [0, 1].
    int sky = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const double m = lay.mask.At(x, y), r = std::hypot(x - cx, y - cy);
            CHECK((m >= 0 && m <= 1));
            if (r > R + 3 + stack::kMaskGrow + stack::kMaskTaper + 2) {
                CHECK(m == 0.0);
                ++sky;
            }
        }
    CHECK(sky > 1000);
    // The mask is the layout's: copies share it.
    const RegionLayout copy = lay;
    CHECK(copy.mask.m == lay.mask.m);
    // The same reference laid out as frame-filling (fix round 1): masked the same way -- 1 over the disc, 0 over
    // the sky past the taper.
    const RegionLayout full = stack::PlaceRegions(ref, st, true, testing::WhiteNoise(0.01));
    REQUIRE(full.flatEdges);
    REQUIRE_FALSE(full.mask.Empty());
    CHECK(full.mask.At(cx, cy) == 1.0);
    CHECK(full.mask.At(cx + R + 2, cy) == 1.0);
    CHECK(full.mask.At(2, 2) == 0.0);
    CHECK(full.mask.At(w - 3, 2) == 0.0);
    // A frame-filling surface with no sky (its darkest end is surface, kMinSkyShare): no mask, every region kept.
    const Scene surf = testing::MakeSurface(31, w, h);
    FloatImage sref{w, h, 1, std::vector<float>(static_cast<size_t>(w) * h)};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            sref.data[static_cast<size_t>(y) * w + x] = static_cast<float>(surf.Eval(x, y, 1.0) + 0.0025 * n(rng));
    const stack::FrameStats sst = stack::MeasureFrame(sref, 0, 1);
    REQUIRE(sst.targetFound);
    const RegionLayout surface = stack::PlaceRegions(sref, sst, true, testing::WhiteNoise(0.01));
    REQUIRE_FALSE(surface.regions.empty());
    CHECK(surface.mask.Empty());
}
