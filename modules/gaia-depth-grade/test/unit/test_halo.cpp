// Task 8: the star-shine halo profile (psf.py) against the Task 1 goldens: the empirical
// profile of the colour and mono fields, the Moffat fallback with its warning, the sampler.
#include <catch_amalgamated.hpp>

#include "Golden.h"
#include "core/HaloProfile.h"

#include <cmath>
#include <limits>
#include <string>
#include <vector>

using gdg::test::count_unequal;
using gdg::test::golden;
using gdg::test::golden_image;
using gdg::test::max_abs_diff;

namespace {

std::vector<gdg::StarRow> table(const std::string& prefix) {
    const auto x = golden(prefix + "_x").data, y = golden(prefix + "_y").data, f = golden(prefix + "_flux").data;
    std::vector<gdg::StarRow> t;
    for (size_t i = 0; i < x.size(); ++i) t.push_back(gdg::StarRow{x[i], y[i], f[i]});
    return t;
}

struct Captured {
    std::vector<std::string> info, warning;
    gdg::Log log() {
        gdg::Log l;
        l.info = [this](const std::string& m) { info.push_back(m); };
        l.warning = [this](const std::string& m) { warning.push_back(m); };
        return l;
    }
};

}  // namespace

TEST_CASE("halo_empirical_matches_golden", "[halo][golden]") {
    for (const std::string p : {"", "mono_"}) {
        const gdg::Image stars = golden_image(p.empty() ? "stars_rgb" : "mono_stars");
        Captured cap;
        const gdg::HaloProfile h = gdg::build_halo_profile(stars, table(p + "match_table"), 2.0, gdg::GradeConfig{}, cap.log());
        CHECK(h.is_empirical);
        CHECK(cap.warning.empty());
        const auto rg = golden(p + "halo_r_grid").data, v = golden(p + "halo_values").data;
        REQUIRE(h.r_grid.size() == 17u);
        const double dg = max_abs_diff(h.r_grid, rg), dv = max_abs_diff(h.values, v);
        WARN(p << "halo: max |d r_grid| " << dg << ", max |d values| " << dv << ", " << count_unequal(h.values, v)
               << " of " << v.size() << " values not bit-identical");
        // Plan 1e-12. Only exact arithmetic and medians are involved, so it is bit-exact.
        CHECK(count_unequal(h.r_grid, rg) == 0);
        CHECK(count_unequal(h.values, v) == 0);
        CHECK(h.values[0] == 1.0);
        for (size_t i = 1; i < h.values.size(); ++i) CHECK(h.values[i] <= h.values[i - 1]);
    }
}

TEST_CASE("halo_reference_selection_matches_golden", "[halo][golden]") {
    // make_goldens.py phase_c_cases 6b: brightest-first with a NaN flux, the 0.9 saturation
    // ceiling, frame-cut and dark stamps, the 4-sigma / half-peak neighbour test, the stop after
    // 3 * halo_min_refs stamps, a cored centre (normalisation) and a ring (monotone clamp).
    gdg::GradeConfig cfg;
    cfg.halo_min_refs = 3;
    cfg.halo_radius_sigma = 4.0;
    const gdg::Image stars = golden_image("halo_select_stars");
    const auto t = table("halo_select_table");
    Captured cap;
    const gdg::HaloProfile h = gdg::build_halo_profile(stars, t, 1.0, cfg, cap.log());
    CHECK(h.is_empirical);
    CHECK(cap.warning.empty());
    CHECK(count_unequal(h.r_grid, golden("halo_select_r_grid").data) == 0);
    const auto want = golden("halo_select_values").data;
    WARN("halo_select: max |d values| " << max_abs_diff(h.values, want));
    CHECK(count_unequal(h.values, want) == 0);
    // Exactly halo_min_refs clean stamps (the first three clean rows) are enough.
    const std::vector<gdg::StarRow> three(t.begin() + 7, t.begin() + 10);
    Captured c3;
    const gdg::HaloProfile h3 = gdg::build_halo_profile(stars, three, 1.0, cfg, c3.log());
    CHECK(h3.is_empirical);
    CHECK(c3.warning.empty());
    CHECK(count_unequal(h3.values, golden("halo_select_three_values").data) == 0);
    // Those three plus the dark stamp (row 4) or the bright-neighbour stamp (row 5):
    // psf.py rejects both, so the profile is the three-stamp one (a fourth would move the median).
    for (size_t row : {size_t{4}, size_t{5}}) {
        std::vector<gdg::StarRow> four{t[row]};
        four.insert(four.end(), three.begin(), three.end());
        Captured c4;
        const gdg::HaloProfile h4 = gdg::build_halo_profile(stars, four, 1.0, cfg, c4.log());
        INFO("rejected row " << row);
        CHECK(count_unequal(h4.values, golden("halo_select_three_values").data) == 0);
    }
}

TEST_CASE("halo_falls_back_to_moffat_with_warning", "[halo][golden]") {
    // The 64x48 edge-case frame: one clean reference star (make_goldens.py phase_c_cases 7).
    Captured cap;
    const gdg::HaloProfile h =
        gdg::build_halo_profile(golden_image("odd_stars"), table("odd_table"), 2.0, gdg::GradeConfig{}, cap.log());
    CHECK_FALSE(h.is_empirical);
    REQUIRE(cap.warning.size() == 1);
    CHECK(cap.warning[0] ==
          "star-shine: only 1 clean reference star(s) (need 8) \xE2\x80\x94 using a Moffat halo this run, not the "
          "image's own PSF");  // stage.json halo_fallback.warning, verbatim
    CHECK(cap.info.empty());
    CHECK(max_abs_diff(h.values, golden("moffat_values").data) <= 1e-15);
    CHECK(count_unequal(h.r_grid, golden("moffat_r_grid").data) == 0);

    // No stars at all, and a raised minimum on the real field (its clean stamps < 100).
    Captured c2;
    CHECK_FALSE(gdg::build_halo_profile(golden_image("stars_rgb"), {}, 2.0, gdg::GradeConfig{}, c2.log()).is_empirical);
    REQUIRE(c2.warning.size() == 1);
    CHECK(c2.warning[0].rfind("star-shine: only 0 clean reference star(s) (need 8)", 0) == 0);
    gdg::GradeConfig many;
    many.halo_min_refs = 100;
    Captured c3;
    CHECK_FALSE(gdg::build_halo_profile(golden_image("stars_rgb"), table("match_table"), 2.0, many, c3.log()).is_empirical);
    REQUIRE(c3.warning.size() == 1);
    CHECK(c3.warning[0].find("(need 100)") != std::string::npos);
}

TEST_CASE("moffat_matches_golden", "[halo][golden]") {
    const gdg::HaloProfile m = gdg::moffat_profile(2.0, 2.5, 16.0);
    CHECK_FALSE(m.is_empirical);
    CHECK(count_unequal(m.r_grid, golden("moffat_r_grid").data) == 0);
    const double d = max_abs_diff(m.values, golden("moffat_values").data);
    WARN("moffat: max |d values| " << d << ", " << count_unequal(m.values, golden("moffat_values").data)
                                   << " not bit-identical (numpy's SIMD pow vs libm)");
    CHECK(d <= 1e-15);  // plan 1e-12
    CHECK(m.values[0] == 1.0);
    // Short radius: at least 2 grid points (np.linspace(0, 0.5, 2)).
    const gdg::HaloProfile s = gdg::moffat_profile(1.0, 3.0, 0.5);
    CHECK(s.r_grid == std::vector<double>{0.0, 0.5});
}

TEST_CASE("sample_zero_beyond_grid", "[halo]") {
    // HaloProfile.sample is np.interp(r, xp, fp, left=fp[0], right=0.0) plus 0 beyond the grid:
    // the interp_q_left_right golden is exactly that lookup on a non-uniform grid.
    gdg::HaloProfile h;
    h.r_grid = golden("interp_xp").data;
    h.values = golden("interp_fp").data;
    CHECK(count_unequal(h.sample(golden("interp_xq").data), golden("interp_q_left_right").data) == 0);
    const auto v = h.sample({-1.0, 0.0, 4.0, 4.0000001, 1e9, std::numeric_limits<double>::quiet_NaN()});
    CHECK(v[0] == 1.0);   // left = values[0]
    CHECK(v[1] == 1.0);
    CHECK(v[2] == 0.0);   // the last grid value
    CHECK(v[3] == 0.0);
    CHECK(v[4] == 0.0);
    CHECK(std::isnan(v[5]));
    gdg::HaloProfile m = gdg::moffat_profile(2.0, 2.5, 16.0);
    const auto w = m.sample({16.0, 16.5, 0.5});
    CHECK(w[0] == m.values.back());
    CHECK(w[1] == 0.0);
    CHECK(w[2] == Catch::Approx((m.values[0] + m.values[1]) / 2.0).epsilon(1e-15));
}
