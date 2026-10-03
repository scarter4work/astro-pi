#include "catch_amalgamated.hpp"
#include "nukex/core/coverage_mask.hpp"
#include "nukex/core/welford.hpp"

using namespace nukex;

TEST_CASE("CoverageMask: default-constructed is empty and means 'all covered'",
          "[coverage]") {
    CoverageMask m;
    REQUIRE(m.empty());
    REQUIRE(m.channels() == 0);
}

TEST_CASE("CoverageMask: bits are independent across channels and pixels",
          "[coverage]") {
    CoverageMask m(4, 3, 2);
    REQUIRE_FALSE(m.empty());
    // Everything starts uncovered.
    for (int c = 0; c < 2; ++c)
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 4; ++x)
                REQUIRE_FALSE(m.covered(c, y, x));

    m.set_covered(1, 2, 3, true);
    REQUIRE(m.covered(1, 2, 3));
    REQUIRE_FALSE(m.covered(0, 2, 3));   // other channel untouched
    REQUIRE_FALSE(m.covered(1, 1, 3));
    REQUIRE_FALSE(m.covered(1, 2, 2));

    REQUIRE(m.covered_count(1) == 1);
    REQUIRE(m.covered_count(0) == 0);

    m.set_covered(1, 2, 3, false);
    REQUIRE_FALSE(m.covered(1, 2, 3));
    REQUIRE(m.covered_count(1) == 0);
}

TEST_CASE("CoverageMask: a channel can be covered where another is not",
          "[coverage]") {
    // This is why the mask is per-channel: channel registration can push one
    // colour plane off the source while the others stay on it.
    CoverageMask m(2, 1, 3);
    m.set_covered(0, 0, 0, true);
    m.set_covered(1, 0, 0, true);
    // channel 2 shifted off the edge here
    REQUIRE(m.covered(0, 0, 0));
    REQUIRE(m.covered(1, 0, 0));
    REQUIRE_FALSE(m.covered(2, 0, 0));
}

TEST_CASE("an uncovered sample must not enter the statistics", "[coverage]") {
    // The defect in one assertion. Frame 0 covers the pixel with 0.8;
    // frame 1's warp places it outside the source, so it is absent -- not
    // black. Averaging the absence in gives 0.4, which is the rim.
    CoverageMask covered_mask(1, 1, 1);
    covered_mask.set_covered(0, 0, 0, true);

    CoverageMask absent_mask(1, 1, 1);   // stays false

    WelfordAccumulator w;
    if (covered_mask.covered(0, 0, 0)) w.update(0.8f);
    if (absent_mask.covered(0, 0, 0))  w.update(0.0f);

    REQUIRE(w.count() == 1);
    REQUIRE(w.mean == Catch::Approx(0.8f));
}

// ---------------------------------------------------------------------------
// warp populates the mask
// ---------------------------------------------------------------------------

#include "nukex/alignment/homography.hpp"
#include "nukex/alignment/types.hpp"

TEST_CASE("warp marks exactly the pixels it sampled", "[coverage][warp]") {
    // A 4x4 source warped by identity into a 6x6 output. Columns 4-5 and
    // rows 4-5 have no source behind them: warp leaves them 0, and the
    // mask is what says those zeros are absence rather than black.
    Image src(4, 4, 1);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            src.at(x, y, 0) = 0.75f;

    HomographyMatrix H = HomographyMatrix::identity();
    CoverageMask cov;
    Image out = HomographyComputer::warp(src, H, 6, 6, ChannelTransforms{}, cov);

    REQUIRE(cov.width() == 6);
    REQUIRE(cov.height() == 6);
    REQUIRE(cov.channels() == 1);

    for (int y = 0; y < 6; ++y) {
        for (int x = 0; x < 6; ++x) {
            const bool inside = (x <= 3 && y <= 3);
            INFO("x=" << x << " y=" << y);
            REQUIRE(cov.covered(0, y, x) == inside);
        }
    }

    // The uncovered pixels really are the zero-valued ones, which is the
    // whole reason the accumulator could not tell them apart.
    REQUIRE(out.at(5, 5, 0) == 0.0f);
    REQUIRE(cov.covered_count(0) == 16);
}

TEST_CASE("warp on a degenerate source covers nothing", "[coverage][warp]") {
    // A source narrower than 2 px cannot be bilinearly interpolated, so the
    // whole output is absent rather than black. This also pins the
    // width() < 2 guard, which had no dedicated test.
    Image src(1, 4, 1);
    src.at(0, 0, 0) = 1.0f;

    HomographyMatrix H = HomographyMatrix::identity();
    CoverageMask cov;
    Image out = HomographyComputer::warp(src, H, 3, 3, ChannelTransforms{}, cov);

    REQUIRE(cov.covered_count(0) == 0);
    REQUIRE(out.at(0, 0, 0) == 0.0f);
}

// ---------------------------------------------------------------------------
// FrameAligner populates the mask it hands the stacker
// ---------------------------------------------------------------------------

#include "nukex/alignment/frame_aligner.hpp"
#include <cmath>
#include <tuple>

static Image coverage_star_field(int w, int h,
                                 const std::vector<std::tuple<float,float,float>>& stars,
                                 float dx = 0.0f, float dy = 0.0f) {
    Image img(w, h, 1);
    img.fill(0.05f);
    const float sigma = 3.0f;
    for (const auto& [sx0, sy0, amp] : stars) {
        const float sx = sx0 + dx, sy = sy0 + dy;
        int r = static_cast<int>(sigma * 4);
        for (int ddy = -r; ddy <= r; ddy++) {
            for (int ddx = -r; ddx <= r; ddx++) {
                int px = static_cast<int>(sx + 0.5f) + ddx;
                int py = static_cast<int>(sy + 0.5f) + ddy;
                if (px < 0 || px >= w || py < 0 || py >= h) continue;
                float fx = static_cast<float>(px) - sx;
                float fy = static_cast<float>(py) - sy;
                img.at(px, py, 0) += amp * std::exp(-(fx*fx + fy*fy) / (2*sigma*sigma));
            }
        }
    }
    return img;
}

TEST_CASE("FrameAligner: a cloned reference reports empty coverage",
          "[coverage][aligner]") {
    // Irregular on purpose: triangle-similarity matching is degenerate on a
    // regular grid, because many triangles are congruent to one in another
    // cell and the matcher pairs stars across cells.
    std::vector<std::tuple<float,float,float>> stars = {
        {50, 50, 0.8f}, {150, 47, 0.7f}, {103, 100, 0.9f},
        {47, 152, 0.6f}, {150, 150, 0.75f}, {83, 78, 0.65f},
        {122, 118, 0.55f}, {31, 97, 0.5f}, {168, 104, 0.45f}
    };
    Image ref = coverage_star_field(200, 200, stars);
    FrameAligner aligner;
    auto r0 = aligner.align(ref, 0);
    // Mono, no channel correction to make: the frame is cloned, so it covers
    // itself completely and there is no mask to allocate.
    REQUIRE(r0.coverage.empty());
}

TEST_CASE("FrameAligner: a shifted frame reports its uncovered edge",
          "[coverage][aligner]") {
    std::vector<std::tuple<float,float,float>> stars = {
        {50, 50, 0.8f}, {150, 47, 0.7f}, {103, 100, 0.9f},
        {47, 152, 0.6f}, {150, 150, 0.75f}, {83, 78, 0.65f},
        {122, 118, 0.55f}, {31, 97, 0.5f}, {168, 104, 0.45f}
    };
    Image ref     = coverage_star_field(200, 200, stars);
    Image shifted = coverage_star_field(200, 200, stars, /*dx*/ 12.0f, /*dy*/ 0.0f);

    FrameAligner aligner;
    aligner.align(ref, 0);
    auto r1 = aligner.align(shifted, 1);

    REQUIRE_FALSE(r1.alignment.alignment_failed);
    REQUIRE_FALSE(r1.coverage.empty());

    const std::int64_t total   = 200LL * 200LL;
    const std::int64_t covered = r1.coverage.covered_count(0);

    // A 12 px translation cannot cover the whole output: roughly one
    // 12-column strip has no source behind it. Those pixels sit at 0 in the
    // warped image, and before CoverageMask the stacker averaged them in as
    // black -- which is the rim.
    REQUIRE(covered > 0);
    REQUIRE(covered < total);
    INFO("covered " << covered << " of " << total);
    REQUIRE(total - covered >= 200);   // at least one full column-worth
}
