#include "catch_amalgamated.hpp"
#include "nukex/alignment/homography.hpp"
#include "nukex/alignment/star_matcher.hpp"
#include "nukex/alignment/star_detector.hpp"
#include "nukex/io/image.hpp"
#include <cmath>
#include <random>

using namespace nukex;

TEST_CASE("HomographyComputer: identity from identical catalogs", "[homography]") {
    // Triangle similarity matching requires non-collinear, non-grid-regular
    // star positions (otherwise many triangles are congruent and vertex
    // correspondences become ambiguous). Use deterministic random positions.
    StarCatalog cat;
    std::mt19937 rng(2024);
    std::uniform_real_distribution<float> px(20.0f, 1980.0f);
    std::uniform_real_distribution<float> py(20.0f, 1980.0f);
    for (int i = 0; i < 20; i++) {
        Star s;
        s.x = px(rng);
        s.y = py(rng);
        s.flux = 100.0f - static_cast<float>(i);
        cat.stars.push_back(s);
    }

    auto matches = StarMatcher::match(cat, cat, StarMatcher::Config{});
    REQUIRE(matches.size() >= 8);

    auto result = HomographyComputer::compute(cat, cat, matches);
    REQUIRE(result.match.success == true);
    REQUIRE(result.alignment_failed == false);
    REQUIRE(result.H.is_identity(0.01f) == true);
    REQUIRE(result.match.rms_error < 0.1f);
}

TEST_CASE("HomographyComputer: translation recovery", "[homography]") {
    // Source catalog shifted by (10, 5). Random positions (not grid) so
    // triangle descriptors are distinct.
    StarCatalog ref, src;
    const float dx = 10.0f, dy = 5.0f;
    std::mt19937 rng(4567);
    std::uniform_real_distribution<float> px(30.0f, 1970.0f);
    std::uniform_real_distribution<float> py(30.0f, 1970.0f);
    for (int i = 0; i < 20; i++) {
        Star r;
        r.x = px(rng);
        r.y = py(rng);
        r.flux = 100.0f - static_cast<float>(i);
        ref.stars.push_back(r);

        Star s = r;
        s.x += dx;
        s.y += dy;
        src.stars.push_back(s);
    }

    auto matches = StarMatcher::match(src, ref, StarMatcher::Config{});
    REQUIRE(matches.size() >= 8);

    auto result = HomographyComputer::compute(src, ref, matches);
    REQUIRE(result.match.success == true);

    // Verify: transforming (ref + (dx,dy)) via H should land near ref.
    auto [tx, ty] = result.H.transform(ref.stars[0].x + dx, ref.stars[0].y + dy);
    REQUIRE(tx == Catch::Approx(ref.stars[0].x).margin(0.5f));
    REQUIRE(ty == Catch::Approx(ref.stars[0].y).margin(0.5f));
}

TEST_CASE("HomographyComputer: too few matches fails gracefully", "[homography]") {
    StarCatalog src, ref;
    for (int i = 0; i < 3; i++) {
        Star s;
        s.x = static_cast<float>(i) * 100.0f;
        s.y = 100.0f;
        src.stars.push_back(s);
        ref.stars.push_back(s);
    }

    auto matches = StarMatcher::match(src, ref, 5.0f);
    auto result = HomographyComputer::compute(src, ref, matches);

    REQUIRE(result.alignment_failed == true);
    REQUIRE(result.weight_penalty == Catch::Approx(0.5f));
}

TEST_CASE("HomographyComputer: warp with identity preserves image", "[homography]") {
    Image img(20, 20, 1);
    for (int y = 0; y < 20; y++)
        for (int x = 0; x < 20; x++)
            img.at(x, y, 0) = static_cast<float>(x + y * 20) / 400.0f;

    auto H = HomographyMatrix::identity();
    Image warped = HomographyComputer::warp(img, H, 20, 20);

    // Interior pixels should be preserved exactly
    for (int y = 1; y < 19; y++) {
        for (int x = 1; x < 19; x++) {
            REQUIRE(warped.at(x, y, 0) == Catch::Approx(img.at(x, y, 0)).margin(0.01f));
        }
    }
}

TEST_CASE("HomographyComputer: a 180-degree homography is left alone",
          "[homography]") {
    // This used to assert the opposite: that a 180-degree rotation must be
    // "corrected" to identity. That was the bug. A flipped frame's homography
    // IS a 180-degree rotation -- that rotation is what maps it onto the
    // reference -- so turning it into identity lays the frame down upside
    // down. Detection stays; the repair is gone. See the [meridian] case in
    // test_frame_aligner.cpp for the behaviour that matters.
    HomographyMatrix H;
    H(0,0) = -1; H(0,1) = 0; H(0,2) = 99;
    H(1,0) = 0;  H(1,1) = -1; H(1,2) = 79;
    H(2,0) = 0;  H(2,1) = 0;  H(2,2) = 1;

    REQUIRE(H.is_meridian_flip() == true);
    REQUIRE(H.is_identity(1.0f) == false);
}

TEST_CASE("StarMatcher: identical non-collinear catalogs match themselves", "[star_matcher]") {
    // Triangle similarity matching requires non-collinear stars (a triangle
    // needs three non-colinear vertices). Use pseudo-random positions seeded
    // deterministically so the test is reproducible.
    StarCatalog cat;
    std::mt19937 rng(123);
    std::uniform_real_distribution<float> px(10.0f, 990.0f);
    std::uniform_real_distribution<float> py(10.0f, 990.0f);
    for (int i = 0; i < 30; i++) {
        Star s;
        s.x = px(rng);
        s.y = py(rng);
        s.flux = 100.0f - static_cast<float>(i); // descending flux
        cat.stars.push_back(s);
    }

    auto matches = StarMatcher::match(cat, cat, StarMatcher::Config{});
    // Triangle matching over K=30 stars with 4060 triangles produces many
    // redundant vertex votes — we should recover most or all correspondences.
    REQUIRE(matches.size() >= 20);

    // Each returned correspondence must be a self-match.
    for (const auto& [si, ri] : matches) {
        REQUIRE(si == ri);
    }
}

TEST_CASE("StarMatcher: recovers correspondences under rotation + translation",
          "[star_matcher]") {
    // Generate 30 random reference stars, then transform them by a known
    // rotation + translation to produce a "source" catalog. Triangle matching
    // should recover most correspondences (descriptor is rotation-invariant).
    StarCatalog ref, src;
    std::mt19937 rng(456);
    std::uniform_real_distribution<float> px(50.0f, 4950.0f);
    std::uniform_real_distribution<float> py(50.0f, 4950.0f);

    // 15° rotation about image center (2500, 2500), translation (137, -89).
    const float theta = 15.0f * 3.14159265f / 180.0f;
    const float cs = std::cos(theta);
    const float sn = std::sin(theta);
    const float cx = 2500.0f, cy = 2500.0f;
    const float tx = 137.0f, ty = -89.0f;

    for (int i = 0; i < 30; i++) {
        Star r;
        r.x = px(rng);
        r.y = py(rng);
        r.flux = 100.0f - static_cast<float>(i);
        ref.stars.push_back(r);

        // Apply rotation about center, then translation.
        float dx = r.x - cx, dy = r.y - cy;
        Star s = r;
        s.x = cs * dx - sn * dy + cx + tx;
        s.y = sn * dx + cs * dy + cy + ty;
        // Small sub-pixel centroid noise (typical in practice).
        s.x += std::uniform_real_distribution<float>(-0.5f, 0.5f)(rng);
        s.y += std::uniform_real_distribution<float>(-0.5f, 0.5f)(rng);
        src.stars.push_back(s);
    }

    StarMatcher::Config cfg; // defaults
    auto matches = StarMatcher::match(src, ref, cfg);
    REQUIRE(matches.size() >= 20);

    // Correspondences must be self-index (star i in ref maps to star i in src)
    // because we constructed them that way.
    int correct = 0;
    for (const auto& [si, ri] : matches)
        if (si == ri) correct++;
    // Allow a couple of near-coincidence misses due to noise-induced descriptor
    // jitter, but the vast majority must be correct.
    REQUIRE(correct >= static_cast<int>(matches.size() * 0.9f));
}

// --- channel-aware warp --------------------------------------------------

#include "nukex/alignment/channel_registration.hpp"

TEST_CASE("warp with channel transforms brings a displaced channel into "
          "register", "[homography]") {
    // Red drawn 1.5 px right of green. A channel transform of exactly that
    // must pull it back on top.
    nukex::Image src(200, 200, 3);
    src.fill(0.0f);

    auto blob = [&](int ch, double cx, double cy) {
        for (int dy = -6; dy <= 6; dy++)
            for (int dx = -6; dx <= 6; dx++) {
                int px = int(std::lround(cx)) + dx;
                int py = int(std::lround(cy)) + dy;
                if (px < 0 || px >= 200 || py < 0 || py >= 200) continue;
                double ex = px - cx, ey = py - cy;
                src.at(px, py, ch) += float(0.5 * std::exp(-(ex*ex + ey*ey) / 5.12));
            }
    };
    blob(1, 100.0, 100.0);   // green
    blob(0, 101.5, 100.0);   // red, displaced

    nukex::ChannelTransforms ct;
    ct.cx = 99.5; ct.cy = 99.5;
    ct.reference_channel = 1;
    ct.per_channel.resize(3);
    ct.per_channel[0].s  = 1.0;
    ct.per_channel[0].tx = 1.5;         // where red images a green position
    ct.per_channel[0].fit = nukex::ChannelTransform::Fit::TranslationOnly;

    nukex::Image out = nukex::HomographyComputer::warp(
        src, nukex::HomographyMatrix::identity(), 200, 200, ct);

    // Centre of mass of each channel in a box around the green position.
    auto com_x = [&](const nukex::Image& im, int ch) {
        double w = 0, wx = 0;
        for (int y = 90; y < 110; y++)
            for (int x = 90; x < 112; x++) {
                double v = im.at(x, y, ch);
                if (v <= 0) continue;
                w += v; wx += v * x;
            }
        return wx / w;
    };

    // Before: red sits 1.5 px away. After: within a twentieth of a pixel.
    REQUIRE(std::abs(com_x(src, 0) - com_x(src, 1)) > 1.4);
    REQUIRE(std::abs(com_x(out, 0) - com_x(out, 1)) < 0.05);

    // Green must be untouched. It is the reference; resampling it would blur
    // it for nothing, and the acceptance criterion checks its FWHM.
    for (int y = 0; y < 200; y++)
        for (int x = 0; x < 200; x++)
            REQUIRE(out.at(x, y, 1) == Catch::Approx(src.at(x, y, 1)));
}

TEST_CASE("warp with empty channel transforms matches the old warp exactly",
          "[homography]") {
    nukex::Image src(64, 64, 3);
    for (int c = 0; c < 3; c++)
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 64; x++)
                src.at(x, y, c) = float((x * 7 + y * 13 + c * 29) % 251) / 251.0f;

    nukex::HomographyMatrix H = nukex::HomographyMatrix::identity();
    H(0, 2) = 2.5f;
    H(1, 2) = -1.25f;

    nukex::Image a = nukex::HomographyComputer::warp(src, H, 64, 64);
    nukex::Image b = nukex::HomographyComputer::warp(src, H, 64, 64,
                                                    nukex::ChannelTransforms{});

    REQUIRE(a.data_size() == b.data_size());
    for (size_t i = 0; i < a.data_size(); i++)
        REQUIRE(a.data()[i] == b.data()[i]);
}

TEST_CASE("warp with identity preserves the last row and column exactly",
          "[homography]") {
    // With an identity homography sx == x and sy == y, so the last column
    // (x == sw-1) and last row (y == sh-1) used to fail the old
    // `sx >= sw - 1` / `sy >= sh - 1` bounds check and come out zero. That
    // was cosmetic while the reference frame was always frame.clone() --
    // once Task 5 started warping the reference frame itself, those zeros
    // fed the stacker's accumulator as if they were real samples.
    nukex::Image src(10, 8, 2);
    for (int c = 0; c < 2; c++)
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 10; x++)
                src.at(x, y, c) = float((x * 11 + y * 17 + c * 31) % 97) / 97.0f;

    nukex::Image out = nukex::HomographyComputer::warp(
        src, nukex::HomographyMatrix::identity(), 10, 8,
        nukex::ChannelTransforms{});

    for (int c = 0; c < 2; c++) {
        for (int x = 0; x < 10; x++)
            REQUIRE(out.at(x, 7, c) == src.at(x, 7, c));
        for (int y = 0; y < 8; y++)
            REQUIRE(out.at(9, y, c) == src.at(9, y, c));
    }
}
