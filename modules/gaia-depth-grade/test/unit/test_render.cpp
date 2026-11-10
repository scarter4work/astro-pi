// Task 9: the per-star depth render (render.py) against the Task 1 goldens. Image goldens are the
// float64 Python result stored as float32, so the reachable tolerance is float32's half ulp
// (<= 6e-8 on [0, 1]), not the plan's 1e-9; the port is reported as "float32-identical" when
// rounding its float64 result to float32 reproduces the golden exactly.
#include <catch_amalgamated.hpp>

#include "Golden.h"
#include "core/RenderStars.h"
#include "core/Transform.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using gdg::Image;
using gdg::test::golden;
using gdg::test::golden_image;

namespace {

const gdg::Gains kDialog{0.65, 0.85, 0.12, 0.4, 1.0};

std::vector<gdg::StarRow> table(const std::string& prefix) {
    const auto x = golden(prefix + "_x").data, y = golden(prefix + "_y").data, f = golden(prefix + "_flux").data;
    std::vector<gdg::StarRow> t;
    for (size_t i = 0; i < x.size(); ++i) t.push_back(gdg::StarRow{x[i], y[i], f[i]});
    return t;
}

// The (4, N) modulation golden as a Modulation.
gdg::Modulation modulation(const std::string& name) {
    const auto g = golden(name);
    const size_t n = g.shape.at(1);
    gdg::Modulation m;
    for (size_t i = 0; i < n; ++i) {
        m.brightness.push_back(g.data[i]);
        m.size.push_back(g.data[n + i]);
        m.contrast.push_back(g.data[2 * n + i]);
        m.saturation.push_back(g.data[3 * n + i]);
    }
    return m;
}

void check_f32(const Image& got, const std::string& name) {
    const Image want = golden_image(name);
    REQUIRE(got.same_shape(want));
    double d = 0.0;
    size_t f32_unequal = 0;
    for (size_t i = 0; i < got.data.size(); ++i) {
        d = std::max(d, std::fabs(got.data[i] - want.data[i]));
        if (static_cast<double>(static_cast<float>(got.data[i])) != want.data[i]) ++f32_unequal;
    }
    WARN(name << ": max |delta| " << d << " vs the float32 golden, " << f32_unequal << " of " << got.data.size()
              << " samples not float32-identical");
    CHECK(d <= 6e-8);
    CHECK(f32_unequal == 0);
}

}  // namespace

TEST_CASE("render_matches_golden_colour", "[render][golden]") {
    // The product path: render_from_prep = effective strength -> dialog gains -> the image's own
    // halo -> render. The golden was asserted equal to render_from_prep by the generator.
    const Image stars = golden_image("stars_rgb");
    const auto t = table("match_table");
    const gdg::HaloProfile halo = gdg::build_halo_profile(stars, t, 2.0, gdg::GradeConfig{}, gdg::Log{});
    REQUIRE(halo.is_empirical);
    const gdg::Modulation m = gdg::compute_modulation(golden("effective").data, kDialog);
    check_f32(gdg::render_stars(stars, t, m, 2.0, &halo, 1.0), "rendered_stars");
}

TEST_CASE("render_matches_golden_mono", "[render][golden]") {
    // Review Focus 2: a one-channel layer renders with no tint and no saturation step.
    const Image stars = golden_image("mono_stars");
    REQUIRE(stars.channels == 1);
    const auto t = table("mono_match_table");
    const gdg::HaloProfile halo = gdg::build_halo_profile(stars, t, 2.0, gdg::GradeConfig{}, gdg::Log{});
    check_f32(gdg::render_stars(stars, t, modulation("mono_modulation"), 2.0, &halo, 1.0), "mono_rendered_stars");
}

TEST_CASE("render_far_star_never_negative_ring", "[render]") {
    // A far star (size < 1) gets no halo: with brightness 1, no contrast and no saturation change
    // the render is the identity; with dimming it only ever dims, never digs below zero.
    const Image stars = golden_image("stars_rgb");
    const auto t = table("match_table");
    gdg::Modulation m;
    for (size_t i = 0; i < t.size(); ++i) {
        m.brightness.push_back(1.0);
        m.size.push_back(0.4);
        m.contrast.push_back(0.0);
        m.saturation.push_back(1.0);
    }
    CHECK(gdg::render_stars(stars, t, m, 2.0).data == stars.data);
    for (double& b : m.brightness) b = 0.6;
    const Image dim = gdg::render_stars(stars, t, m, 2.0);
    size_t brighter = 0, negative = 0;
    for (size_t i = 0; i < dim.data.size(); ++i) {
        if (dim.data[i] > stars.data[i]) ++brighter;
        if (dim.data[i] < 0.0) ++negative;
    }
    CHECK(brighter == 0);
    CHECK(negative == 0);
}

TEST_CASE("render_edge_star_window_clipped", "[render][golden]") {
    // 64x48 frame with stars on and over every edge and corner (windows clipped with int()
    // truncation), the default Moffat halo (halo = nullptr), an explicit NaN distance row.
    check_f32(gdg::render_stars(golden_image("odd_stars"), table("odd_table"), modulation("odd_modulation"), 2.0),
              "odd_rendered_stars");
    // A star entirely outside the frame is skipped; the result is just the [0, 1] clip.
    Image s(10, 8, 3, 0.5);
    s.data[0] = 1.5;
    gdg::Modulation m{{1.5}, {1.8}, {0.3}, {1.4}};
    const Image o = gdg::render_stars(s, {gdg::StarRow{-100.0, 3.0, 1.0}}, m, 2.0);
    CHECK(o.data[0] == 1.0);
    CHECK(o.data[1] == 0.5);
    CHECK_THROWS_AS(gdg::render_stars(s, {gdg::StarRow{NAN, 3.0, 1.0}}, m, 2.0), std::invalid_argument);
    CHECK_THROWS_AS(gdg::render_stars(s, {}, m, 2.0), std::invalid_argument);
}

TEST_CASE("render_overlap_order_matches_python", "[render][golden]") {
    // Two enlarged stars 3.9 px apart: each window covers the other, so the result depends on
    // the order. Table order and its reverse both reproduce Python, and they differ by ~0.2.
    const Image stars = golden_image("overlap_stars");
    auto t = table("overlap_table");
    gdg::Modulation m = gdg::compute_modulation({0.85, 0.55}, kDialog);
    const Image fwd = gdg::render_stars(stars, t, m, 2.0);
    // overlap_rendered is stored float64: here the plan tolerance (1e-9) applies, and the port is
    // reported against it at full precision.
    const Image want = golden_image("overlap_rendered");
    REQUIRE(fwd.same_shape(want));
    const double d64 = gdg::test::max_abs_diff(fwd.data, want.data);
    WARN("overlap_rendered (float64): max |delta| " << d64 << ", " << gdg::test::count_unequal(fwd.data, want.data)
                                                    << " of " << want.data.size() << " not bit-identical");
    CHECK(d64 <= 1e-12);
    std::reverse(t.begin(), t.end());
    const gdg::Modulation mr = gdg::compute_modulation({0.55, 0.85}, kDialog);
    const Image rev = gdg::render_stars(stars, t, mr, 2.0);
    check_f32(rev, "overlap_rendered_reversed");
    CHECK(gdg::test::max_abs_diff(fwd.data, rev.data) > 0.1);
}

TEST_CASE("render_halo_gain_scales_only_the_halo", "[render]") {
    // halo_gain multiplies the star-shine halo and nothing else: one enlarged star with
    // brightness 1, no contrast and no saturation change adds light linear in the gain.
    Image s = golden_image("mono_stars");
    for (double& v : s.data) v *= 0.25;  // keep clear of the [0, 1] clip
    const std::vector<gdg::StarRow> t{table("mono_match_table").at(0)};
    const gdg::Modulation m{{1.0}, {1.5}, {0.0}, {1.0}};
    CHECK(gdg::render_stars(s, t, m, 2.0, nullptr, 0.0).data == s.data);
    const Image g1 = gdg::render_stars(s, t, m, 2.0, nullptr, 1.0);
    const Image g2 = gdg::render_stars(s, t, m, 2.0, nullptr, 2.0);
    double added = 0.0, nonlinear = 0.0;
    for (size_t i = 0; i < s.data.size(); ++i) {
        added = std::max(added, g1.data[i] - s.data[i]);
        nonlinear = std::max(nonlinear, std::fabs((g2.data[i] - s.data[i]) - 2.0 * (g1.data[i] - s.data[i])));
    }
    CHECK(added > 1e-3);
    CHECK(nonlinear < 1e-15);
}
