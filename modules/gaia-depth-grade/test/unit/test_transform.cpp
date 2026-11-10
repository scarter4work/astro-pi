// Task 7: config defaults, depth strength / confidence / effective strength and modulation
// against the Task 1 goldens and numpy-computed literals (gaia_depth_grade.transform).
#include <catch_amalgamated.hpp>

#include "Golden.h"
#include "core/Config.h"
#include "core/Log.h"
#include "core/Modulate.h"
#include "core/Transform.h"

#include <cmath>
#include <limits>
#include <string>
#include <vector>

using gdg::test::count_unequal;
using gdg::test::golden;

namespace {

const double kNaN = std::numeric_limits<double>::quiet_NaN();
const double kInf = std::numeric_limits<double>::infinity();

const gdg::Gains kDialog{0.65, 0.85, 0.12, 0.4, 1.0};  // make_goldens.py DIALOG_GAINS

std::vector<double> col(const std::string& name) { return golden(name).data; }

std::vector<double> stack(const gdg::Modulation& m) {
    std::vector<double> v = m.brightness;
    v.insert(v.end(), m.size.begin(), m.size.end());
    v.insert(v.end(), m.contrast.begin(), m.contrast.end());
    v.insert(v.end(), m.saturation.begin(), m.saturation.end());
    return v;
}

}  // namespace

TEST_CASE("config_defaults_match_config_py", "[transform][config]") {
    const gdg::GradeConfig c;
    CHECK(c.gains.brightness == 0.5);
    CHECK(c.gains.size == 0.4);
    CHECK(c.gains.contrast == 0.3);
    CHECK(c.gains.saturation == 0.3);
    CHECK(c.gains.halo_gain == 1.0);
    CHECK(c.p_low == 5.0);
    CHECK(c.p_high == 95.0);
    CHECK(c.match_tolerance_px == 4.0);
    CHECK(c.min_match_rate == 0.3);
    CHECK(c.detect_fwhm == 3.0);
    CHECK(c.detect_threshold_sigma == 6.0);
    CHECK(c.base_sigma_px == 2.0);
    CHECK(c.neutral_strength == 0.0);
    REQUIRE(c.gaia_mag_limit.has_value());
    CHECK(*c.gaia_mag_limit == 18.0);
    CHECK(c.gaia_parallax_snr == 5.0);
    CHECK(c.halo_min_refs == 8);
    CHECK(c.halo_radius_sigma == 8.0);
    CHECK(c.halo_moffat_beta == 2.5);

    std::string got;
    gdg::Log log;
    gdg::log_warning(log, "dropped");  // no sink: no-op
    log.warning = [&](const std::string& m) { got = m; };
    gdg::log_warning(log, "w");
    gdg::log_info(log, "i");  // info sink unset
    CHECK(got == "w");
}

TEST_CASE("strength_matches_golden", "[transform][golden]") {
    // log10 is the one transcendental: numpy's SIMD log10 and libm's disagree in the last ulp on
    // some inputs (log10(90): numpy ...aff, libm ...afe), which moves a strength by <= ~1e-15.
    // Everything else is exact arithmetic, so the tolerance is that ulp, not the plan's "exact".
    constexpr double kTol = 2e-15;
    auto check = [&](const std::vector<double>& got, const std::string& name) {
        const auto want = col(name);
        const double d = gdg::test::max_abs_diff(got, want);
        WARN(name << ": max |delta| " << d << ", " << count_unequal(got, want) << " of " << want.size()
                  << " not bit-identical");
        CHECK(d <= kTol);
    };
    for (const std::string p : {"", "mono_"}) {
        const auto rm = col(p + "match_table_r_med_geo"), rl = col(p + "match_table_r_lo_geo"),
                   rh = col(p + "match_table_r_hi_geo");
        check(gdg::depth_strength(rm, 5.0, 95.0, 0.0), p + "strength");
        CHECK(count_unequal(gdg::confidence(rm, rl, rh), col(p + "confidence")) == 0);  // no log10: exact
        check(gdg::effective_strength(rm, rl, rh, 5.0, 95.0, 0.0), p + "effective");
    }
    // The edge-case table: a NaN distance and a low-confidence star.
    const auto rm = col("odd_table_r_med_geo"), rl = col("odd_table_r_lo_geo"), rh = col("odd_table_r_hi_geo");
    check(gdg::effective_strength(rm, rl, rh), "odd_effective");

    // numpy literals (transform.py on these arrays): r = 0, negative, +inf, NaN r_lo, and two
    // percentile pairs with a non-zero neutral.
    const std::vector<double> m = {100.0, kNaN, 0.0, -5.0, 500.0, 2000.0, kInf, 800.0};
    std::vector<double> l(m.size()), h(m.size());
    for (size_t i = 0; i < m.size(); ++i) {
        l[i] = m[i] * 0.9;
        h[i] = m[i] * 1.2;
    }
    l[2] = 1.0;
    h[2] = 3.0;
    l[7] = kNaN;
    const auto s = gdg::depth_strength(m, 5.0, 95.0, 0.25);
    CHECK(s[0] == 1.0);
    CHECK(s[1] == 0.25);
    CHECK(s[2] == 0.25);
    CHECK(s[3] == 0.25);
    CHECK(s[5] == -1.0);
    CHECK(s[6] == 0.25);
    const std::vector<double> want_e1 = {0x1.b333333333333p-1, 0x1.0000000000000p-2, 0.0, 0x1.b333333333333p-3,
                                         -0x1.3d1a07804355bp-5, -0x1.b333333333333p-1, 0x1.0000000000000p-2, 0.0};
    CHECK(count_unequal(gdg::effective_strength(m, l, h, 5.0, 95.0, 0.25), want_e1) == 0);
    const std::vector<double> want_e2 = {0x1.b333333333333p-1, -0x1.999999999999ap-4, 0.0, -0x1.5c28f5c28f5c3p-4,
                                         -0x1.c918f26050254p-3, -0x1.b333333333333p-1, -0x1.999999999999ap-4, 0.0};
    CHECK(count_unequal(gdg::effective_strength(m, l, h, 20.0, 60.0, -0.1), want_e2) == 0);
    CHECK_THROWS_AS(gdg::confidence({1.0}, {1.0, 2.0}, {1.0}), std::invalid_argument);
    CHECK_THROWS_AS(gdg::effective_strength({1.0}, {1.0}, {}), std::invalid_argument);
}

TEST_CASE("strength_degenerate_spread_is_neutral", "[transform]") {
    // hi <= lo: every row neutral (numpy: [0.25, 0.25, 0.25, 0.25]), never +-1 or NaN.
    const auto s = gdg::depth_strength({300.0, 300.0, 300.0, kNaN}, 5.0, 95.0, 0.25);
    CHECK(s == std::vector<double>{0.25, 0.25, 0.25, 0.25});
    CHECK(gdg::depth_strength({}, 5.0, 95.0, 0.1).empty());
    CHECK(gdg::depth_strength({kNaN, -1.0, 0.0}, 5.0, 95.0, 0.1) == std::vector<double>{0.1, 0.1, 0.1});
    // A single finite distance is degenerate too (both percentiles equal it).
    CHECK(gdg::depth_strength({kNaN, 450.0}, 5.0, 95.0, -0.3) == std::vector<double>{-0.3, -0.3});
}

TEST_CASE("confidence_nan_is_zero", "[transform]") {
    // NaN anywhere, r_med = 0 (inf or NaN fraction) and an inverted interval.
    const auto c = gdg::confidence({kNaN, 100.0, 100.0, 0.0, 0.0, 100.0, 100.0},
                                   {90.0, kNaN, 90.0, 1.0, 0.0, 120.0, 0.0},
                                   {110.0, 110.0, kNaN, 3.0, 0.0, 80.0, 400.0});
    CHECK(c == std::vector<double>{0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0});
    CHECK(gdg::confidence({100.0}, {90.0}, {110.0})[0] == 1.0 - 20.0 / 200.0);
}

TEST_CASE("modulation_matches_golden", "[transform][golden]") {
    CHECK(count_unequal(stack(gdg::compute_modulation(col("effective"), kDialog)), col("modulation")) == 0);
    CHECK(count_unequal(stack(gdg::compute_modulation(col("mono_effective"), gdg::Gains{})), col("mono_modulation")) == 0);
    CHECK(count_unequal(stack(gdg::compute_modulation(col("odd_effective"), kDialog)), col("odd_modulation")) == 0);
    CHECK(count_unequal(stack(gdg::compute_modulation({0.85, 0.55}, kDialog)), col("overlap_modulation")) == 0);
    const auto m = gdg::compute_modulation({1.0, -1.0, 0.0}, gdg::Gains{0.5, 0.4, 0.3, 0.2, 1.0});
    CHECK(m.brightness == std::vector<double>{1.5, 0.5, 1.0});
    CHECK(m.size == std::vector<double>{1.4, 0.6, 1.0});
    CHECK(m.contrast == std::vector<double>{0.3, -0.3, 0.0});
    CHECK(m.saturation == std::vector<double>{1.2, 0.8, 1.0});
}
