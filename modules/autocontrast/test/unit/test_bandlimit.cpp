// AutoContrast: band-limiting (fingerprint/bandlimit.py) vs the Python goldens (synthetic/bandlimit),
// and Python's format(x, '.3g') that its reason strings use.
#include <catch_amalgamated.hpp>

#include "Npy.h"
#include "core/PyFormat.h"
#include "core/fingerprint/BandLimit.h"
#include "core/fingerprint/Energy.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

nlohmann::json cases()
{
   return ac::test::load_json(std::string(AC_TEST_DIR) + "/golden/synthetic/bandlimit/cases.json")["cases"];
}

ac::EnergySpectrum spec_of(const nlohmann::json& j)
{
   return {j["centers"].get<std::vector<double>>(), j["energy"].get<std::vector<double>>()};
}

nlohmann::json by_name(const nlohmann::json& cs, const std::string& name)
{
   for (const auto& c : cs)
      if (c["name"] == name)
         return c;
   FAIL("no golden case " << name);
   throw std::logic_error("unreachable");
}

ac::BandLimitResult run(const nlohmann::json& c)
{
   return ac::band_limit(spec_of(c["ref"]), spec_of(c["target"]), c["ref_psf"].get<double>(),
                         c["target_psf"].get<double>(), c["min_bins"].get<int>());
}

double g_worst_shape = 0.0;

void check_case(const nlohmann::json& c)
{
   const auto& want = c["result"];
   const auto got = run(c);
   INFO(c["name"].get<std::string>());
   REQUIRE(got.lo_arcsec == want["lo"].get<double>());
   REQUIRE(got.hi_arcsec == want["hi"].get<double>());
   REQUIRE(got.usable == want["usable"].get<bool>());
   REQUIRE(got.reason == want["reason"].get<std::string>());
   const auto cb = ac::comparable_band(spec_of(c["ref"]), spec_of(c["target"]), c["ref_psf"].get<double>(),
                                       c["target_psf"].get<double>());
   REQUIRE(cb.first == c["comparable_band"][0].get<double>());
   REQUIRE(cb.second == c["comparable_band"][1].get<double>());
   if (!got.usable) {
      REQUIRE(want["grid"].is_null());
      REQUIRE(got.grid_arcsec.empty());
      REQUIRE(got.ref_shape.empty());
      REQUIRE(got.target_shape.empty());
      return;
   }
   REQUIRE(got.grid_arcsec == want["grid"].get<std::vector<double>>());   // a selection: exact
   const auto wr = want["ref_shape"].get<std::vector<double>>(), wt = want["target_shape"].get<std::vector<double>>();
   REQUIRE(got.ref_shape.size() == wr.size());
   REQUIRE(got.target_shape.size() == wt.size());
   for (size_t i = 0; i < wr.size(); ++i) {
      const double dr = std::fabs(got.ref_shape[i] - wr[i]), dt = std::fabs(got.target_shape[i] - wt[i]);
      g_worst_shape = std::max({g_worst_shape, dr, dt});
      INFO("i=" << i);
      REQUIRE(dr <= 1e-12);   // unit vectors: abs == rel scale
      REQUIRE(dt <= 1e-12);
   }
}

} // namespace

TEST_CASE("band_limit_usable_matches_golden")
{
   const auto cs = cases();
   int usable = 0;
   for (const auto& c : cs)
      if (c["result"]["usable"].get<bool>()) {
         check_case(c);
         ++usable;
      }
   REQUIRE(usable >= 6);   // incl. true log-arcsec interpolation (cross_scale) and clamping (extrapolated)
   std::printf("[max|d|] %-44s %.3g\n", "band_limit shapes (worst)", g_worst_shape);
}

TEST_CASE("band_limit_all_golden_cases")
{
   for (const auto& c : cases())
      check_case(c);
}

TEST_CASE("band_limit_no_overlap_reason")
{
   const auto cs = cases();
   for (const char* name : {"hi_le_lo", "hi_eq_lo", "no_overlap_awkward_digits"}) {
      const auto c = by_name(cs, name);
      const auto got = run(c);
      INFO(name);
      REQUIRE_FALSE(got.usable);
      REQUIRE(got.reason.rfind("No band overlap: lo=", 0) == 0);
      REQUIRE(got.reason == c["result"]["reason"].get<std::string>());
   }
   CHECK(run(by_name(cs, "hi_le_lo")).reason ==
         "No band overlap: lo=1e+03\" >= hi=16\". Instruments share no resolvable scale; structural guidance unusable.");
}

TEST_CASE("band_limit_too_few_bins_reason")
{
   const auto cs = cases();
   for (const char* name : {"usable", "lt_min_bins", "lt_min_bins_custom", "short_spectrum", "coarse_ref_fine_target",
                            "few_bins_awkward_digits"}) {
      const auto c = by_name(cs, name);
      const auto got = run(c);
      INFO(name);
      REQUIRE_FALSE(got.usable);
      REQUIRE(got.reason == c["result"]["reason"].get<std::string>());
   }
   CHECK(run(by_name(cs, "few_bins_awkward_digits")).reason ==
         "Only 2 comparable bin(s) in [0.0999, 0.395]\" (need 9); structural guidance unusable, fall back to tonal/chroma.");
}

TEST_CASE("band_limit_zero_energy_target_is_left_unnormalised")
{
   const auto got = run(by_name(cases(), "zero_energy_target"));
   REQUIRE(got.usable);
   for (double v : got.target_shape)
      CHECK(v == 0.0);   // _l2_normalize: norm 0 -> v unchanged
}

TEST_CASE("band_limit_needs_two_centres")
{
   // Python: spec.centers_arcsec[-2] raises IndexError on a one-bin spectrum
   const ac::EnergySpectrum one{{1.0}, {5.0}}, two{{1.0, 2.0}, {5.0, 4.0}};
   CHECK_THROWS_AS(ac::comparable_band(one, two, 1.0, 1.0), std::out_of_range);
   CHECK_THROWS_AS(ac::band_limit(two, one, 1.0, 1.0), std::out_of_range);
}

TEST_CASE("py_format_g3_matches_python")
{
   // format(x, '.3g') from CPython 3 (tools/autocontrast venv), recorded verbatim
   const double inf = std::numeric_limits<double>::infinity(), nan = std::numeric_limits<double>::quiet_NaN();
   const std::pair<double, const char*> table[] = {
      {0.09995, "0.0999"}, {1000.0, "1e+03"}, {2.4, "2.4"}, {16.0, "16"}, {123456.789, "1.23e+05"},
      {0.0, "0"}, {-0.0, "-0"}, {1e-5, "1e-05"}, {0.0001234, "0.000123"}, {999.5, "1e+03"}, {9.995, "9.99"},
      {0.00012345, "0.000123"}, {1e16, "1e+16"}, {5e-324, "4.94e-324"}, {1.0005, "1"}, {-2.5e-7, "-2.5e-07"},
      {inf, "inf"}, {-inf, "-inf"}, {nan, "nan"}, {-nan, "nan"}, {100.0, "100"}, {0.5, "0.5"}};
   for (const auto& [x, want] : table) {
      INFO("x=" << x);
      CHECK(ac::py_format_g(x, 3) == want);
   }
}
