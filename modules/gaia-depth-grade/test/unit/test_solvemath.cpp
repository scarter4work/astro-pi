// Plan Task 14: the plate solver's PCL-free arithmetic (core/SolveMath), against ImageSolver 6.6's
// formulas and hand-computed values.
#include <catch_amalgamated.hpp>

#include "core/SolveMath.h"

#include <cmath>
#include <limits>
#include <random>

using namespace gdg;
using Catch::Approx;

namespace {
constexpr double NaN = std::numeric_limits<double>::quiet_NaN();
}  // namespace

TEST_CASE("parse_seed_focal_pixel_to_scale", "[solvemath]") {
    // 2.9 um at 491.28728 mm (the HaO3 master's XPIXSZ / FOCALLEN): 0.001 * deg(2.9 / 491.28728)
    // = 1.217552260088188764 arcsec/px (bc, 20 digits).
    CHECK(resolution_from_focal(2.9, 491.28728) * 3600 == Approx(1.217552260088188764).epsilon(1e-14));
    CHECK(resolution_from_focal(2.9, 0.0) == 0.0);

    const Seed s = seed_from_metadata(350.1671987228608, 61.22006490843979, 491.28728, 2.9);
    REQUIRE(s.scale_deg);
    CHECK(*s.scale_deg == Approx(0.00033820896113560799).epsilon(1e-14));
    CHECK(*s.ra == 350.1671987228608);
    CHECK(seed_is_valid(s));

    // No pixel size: focal length kept, no scale (ExtractMetadata: useFocal && xpixsz > 0).
    const Seed nofocal = seed_from_metadata(10.0, 20.0, 500.0, std::nullopt);
    CHECK(nofocal.focal_mm == 500.0);
    CHECK(!nofocal.scale_deg);
    CHECK(!seed_is_valid(nofocal));
    // FOCALLEN <= 0 is ignored.
    CHECK(!seed_from_metadata(10.0, 20.0, 0.0, 3.76).focal_mm);
}

TEST_CASE("parse_seed_ra_dec_formats", "[solvemath]") {
    // hasValidSeed ranges.
    Seed s;
    s.ra = 0.0; s.dec = -90.0; s.scale_deg = 0.5;
    CHECK(seed_is_valid(s));
    s.ra = 360.0; s.dec = 90.0;
    CHECK(seed_is_valid(s));
    s.ra = 360.0001;
    CHECK(!seed_is_valid(s));
    s.ra = 10; s.dec = 90.5;
    CHECK(!seed_is_valid(s));
    s.dec = 10; s.scale_deg = 0.5000001;
    CHECK(!seed_is_valid(s));
    s.scale_deg = 0.0;
    CHECK(!seed_is_valid(s));
    s.scale_deg = NaN;
    CHECK(!seed_is_valid(s));
    s.scale_deg = 1e-3; s.ra = NaN;
    CHECK(!seed_is_valid(s));
    s.ra.reset();
    CHECK(!seed_is_valid(s));
}

TEST_CASE("observation_time_is_the_exposure_middle", "[solvemath]") {
    CHECK(!observation_jd(std::nullopt, 2461256.9, 120.0));
    CHECK(*observation_jd(2461256.5, std::nullopt, std::nullopt) == 2461256.5);
    CHECK(*observation_jd(2461256.5, 2461256.7, std::nullopt) == Approx(2461256.6).epsilon(1e-15));
    CHECK(*observation_jd(2461256.7, 2461256.5, std::nullopt) == Approx(2461256.6).epsilon(1e-15));  // swapped
    CHECK(*observation_jd(2461256.5, std::nullopt, 86400.0) == Approx(2461257.0).epsilon(1e-15));
}

TEST_CASE("auto_limit_magnitude_formula", "[solvemath]") {
    // 14.5 * fov^-0.179, clamped to [7,20], rounded to 0.01.
    CHECK(auto_limit_magnitude(1.0) == 14.5);
    CHECK(auto_limit_magnitude(3840 * 0.00033820915389159235) == Approx(13.84).margin(1e-12));
    CHECK(auto_limit_magnitude(1e-6) == 20.0);
    CHECK(auto_limit_magnitude(1e6) == 7.0);
}

TEST_CASE("optimal_limit_magnitude_bisection", "[solvemath]") {
    // A field with 10^(0.3 (m - 8)) * 100 stars brighter than m: target 3000 -> m ~ 12.92.
    auto count = [](double m) { return static_cast<long>(100.0 * std::pow(10.0, 0.3 * (m - 8.0))); };
    const double m = optimal_limit_magnitude(14.2, 21.0, 3000, count);
    CHECK(count(m) >= 3000);
    CHECK(count(m) <= 3150);
    // Never above the database limit; never below 7.
    CHECK(optimal_limit_magnitude(14.2, 21.0, 100000000, count) <= 21.0);
    CHECK(optimal_limit_magnitude(14.2, 21.0, 1, count) >= 7.0);
}

TEST_CASE("star_matching_tolerance_and_binning", "[solvemath]") {
    CHECK(star_matching_tolerance(1.2) == 2);
    CHECK(star_matching_tolerance(6.0) == 3);
    CHECK(star_matching_tolerance(12.0) == 7);
    CHECK(blind_binning_factor(5.9, 3840, 2160) == 1);
    CHECK(blind_binning_factor(6.0, 3840, 2160) == 2);
    CHECK(blind_binning_factor(40.0, 3840, 2160) == 5);  // trunc(2160/400)
    CHECK(blind_binning_factor(40.0, 8000, 8000) == 8);
    CHECK(blind_binning_factor(40.0, 700, 700) == 1);
}

TEST_CASE("median_bright_fwhm_uses_the_200_brightest", "[solvemath]") {
    std::vector<double> signal, fwhm;
    for (int i = 0; i < 300; ++i) {
        signal.push_back(i);
        fwhm.push_back(i < 100 ? 99.0 : 1.0 + (i - 100) * 0.01);  // the 100 faintest are bloated
    }
    // The 200 brightest have fwhm 1.00 .. 2.99; element 100 of the sorted list is 2.00.
    CHECK(median_bright_fwhm(signal, fwhm) == Approx(2.0).margin(1e-12));
    CHECK(median_bright_fwhm({}, {}) == 0.0);
}

TEST_CASE("match_score_rounding", "[solvemath]") {
    CHECK(match_score(2469, 0.10782340260704887) == Approx(2228.695).margin(1e-9));
    CHECK(match_score(0, 0.0) == 0.0);
}

TEST_CASE("rect_index_matches_brute_force", "[solvemath]") {
    std::mt19937 rng(11);
    std::uniform_real_distribution<double> u(0, 1000), s(0.2, 6);
    std::vector<Box> obj;
    for (int i = 0; i < 2000; ++i) {
        const double x = u(rng), y = u(rng), h = s(rng);
        obj.push_back({x - h, y - h, x + h, y + h});
    }
    obj.push_back({500, 500, 500, 500});  // a degenerate (point) rectangle
    const RectIndex idx(obj);
    auto brute = [&](const Box& q) {
        std::vector<int> r;
        for (int i = 0; i < static_cast<int>(obj.size()); ++i)
            if (obj[i].x1 >= q.x0 && obj[i].x0 <= q.x1 && obj[i].y1 >= q.y0 && obj[i].y0 <= q.y1) r.push_back(i);
        return r;
    };
    for (int k = 0; k < 500; ++k) {
        const double x = u(rng) * 1.2 - 100, y = u(rng) * 1.2 - 100, r = (k % 3 == 0) ? 160.0 : s(rng);
        const Box q{x - r, y - r, x + r, y + r};
        const auto want = brute(q);
        CHECK(idx.search(q) == want);
        CHECK(idx.count(q) == static_cast<int>(want.size()));
    }
    // Touching edges intersect (closed rectangles).
    CHECK(idx.count({500, 500, 500, 500}) >= 1);
    const RectIndex empty({});
    CHECK(empty.search({0, 0, 1, 1}).empty());
}

TEST_CASE("conflicting_sources_removed", "[solvemath]") {
    // Stars 0 and 1 overlap within 1 px; star 2 is isolated; star 3's large box reaches star 4.
    std::vector<Pt> p{{10, 10}, {10.8, 10}, {50, 50}, {100, 100}, {105, 100}};
    std::vector<Box> r;
    for (size_t i = 0; i < p.size(); ++i) {
        const double h = (i == 3) ? 4.5 : 0.5;
        r.push_back({p[i].x - h, p[i].y - h, p[i].x + h, p[i].y + h});
    }
    CHECK(remove_conflicting_sources(p, r) == std::vector<int>{2, 3});
}
