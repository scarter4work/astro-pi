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

TEST_CASE("HomographyComputer: meridian flip correction", "[homography]") {
    // 180-degree rotation
    HomographyMatrix H;
    H(0,0) = -1; H(0,1) = 0; H(0,2) = 99;
    H(1,0) = 0;  H(1,1) = -1; H(1,2) = 79;
    H(2,0) = 0;  H(2,1) = 0;  H(2,2) = 1;

    REQUIRE(H.is_meridian_flip() == true);

    auto corrected = HomographyComputer::correct_meridian_flip(H, 100, 80);
    // After correction, should be approximately identity
    REQUIRE(corrected.is_identity(1.0f) == true);
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
