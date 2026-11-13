// AutoContrast: Task 5's deferred extract_real_renders_match_golden -- the 10 Phase 0 renders (5 references,
// 5 amateur) loaded natively by load_raster(max_dim=1600) and fingerprinted by extract() vs the fingerprints the
// Python reference computed from its own load_raster output (golden/real/<key>.json "fingerprint"). The inputs
// are bit-identical (test_raster_real); the fingerprints are compared at 1e-9 relative per component with
// Task 5's definition (test_fingerprint require_close: |d| <= 1e-9 * max(|want|, 1)), not bytes: Lab is ulp-level,
// and a* / b* of near-neutral pixels come from a difference of two nearly equal f(t) values, so a component as
// small as the lowest saturation quantile (~4e-6) carries that cancellation (pure relative ~6e-9, |d| ~3e-14).
// Both the Task 5 measure and the pure relative one are reported. Needs the render corpus; exits 77 without it.
#include <catch_amalgamated.hpp>

#include "DataDir.h"
#include "Npy.h"
#include "core/fingerprint/Extract.h"
#include "core/io/Raster.h"

#include <cmath>
#include <limits>
#include <string>

namespace {

std::string real(const std::string& rel) { return std::string(AC_TEST_DIR) + "/golden/real/" + rel; }

double num(const nlohmann::json& x)
{
   if (x.is_string()) {
      const std::string s = x.get<std::string>();
      if (s == "NaN") return std::numeric_limits<double>::quiet_NaN();
      if (s == "Infinity") return std::numeric_limits<double>::infinity();
      if (s == "-Infinity") return -std::numeric_limits<double>::infinity();
      throw std::runtime_error("not a number: " + s);
   }
   return x.get<double>();
}

double g_worst_rel = 0.0, g_worst_task5 = 0.0, g_worst_abs = 0.0;

// Walk the golden (sorted keys) and ours (to_dict order) by key; every number within rel_tol of max(|want|, 1).
void compare(const nlohmann::json& want, const ac::Json& got, const std::string& at, double rel_tol)
{
   INFO(at);
   if (want.is_object()) {
      REQUIRE(got.is_object());
      REQUIRE(got.size() == want.size());
      for (auto it = want.begin(); it != want.end(); ++it) {
         REQUIRE(got.contains(it.key()));
         compare(it.value(), got.at(it.key()), at + "." + it.key(), rel_tol);
      }
      return;
   }
   if (want.is_array()) {
      REQUIRE(got.is_array());
      REQUIRE(got.size() == want.size());
      for (size_t i = 0; i < want.size(); ++i)
         compare(want[i], got[i], at + "[" + std::to_string(i) + "]", rel_tol);
      return;
   }
   if (want.is_string() && want.get<std::string>() != "NaN" && want.get<std::string>() != "Infinity" &&
       want.get<std::string>() != "-Infinity") {
      CHECK(got.get<std::string>() == want.get<std::string>());
      return;
   }
   const double w = num(want);
   REQUIRE(got.is_number());
   const double g = got.get<double>();
   if (std::isnan(w)) {
      CHECK(std::isnan(g));
      return;
   }
   if (std::isinf(w) || w == 0.0) {
      CHECK(g == w);
      return;
   }
   const double d = std::fabs(g - w);
   const double rel = d / std::fabs(w), task5 = d / std::max(std::fabs(w), 1.0);
   g_worst_rel = std::max(g_worst_rel, rel);
   g_worst_task5 = std::max(g_worst_task5, task5);
   g_worst_abs = std::max(g_worst_abs, d);
   INFO("got " << g << " want " << w << " |d| " << d << " rel " << rel);
   CHECK(task5 <= rel_tol);
}

} // namespace

TEST_CASE("extract_real_renders_match_golden", "[fingerprint][real]")
{
   const auto index = ac::test::load_json(real("index.json"));
   const auto& params = index.at("extract_params");
   const int max_dim = index.at("max_dim");
   int n = 0;
   for (const auto& e : index.at("entries")) {
      const std::string group = e.at("group");
      if (group != "reference" && group != "amateur")
         continue;
      const std::string key = e.at("key");
      INFO(key);
      const auto g = ac::test::load_json(real(key + ".json"));
      REQUIRE(g.at("fingerprint").at("params") == params);
      const ac::Image img = ac::load_raster(ac::test::group_dir(group) + "/" + e.at("file").get<std::string>(), max_dim);
      const auto fp = ac::extract(img, params.at("pixel_scale_arcsec"), params.at("n_scales"),
                                  params.at("psf_fwhm_arcsec"), params.at("palette_class"));
      compare(g.at("fingerprint").at("value"), fp.to_json(), key, 1e-9);
      ++n;
   }
   REQUIRE(n == 10);
   WARN("10 Phase 0 fingerprints vs Python: worst |d|/max(|want|,1) " << g_worst_task5 << ", worst |d|/|want| "
        << g_worst_rel << ", worst |d| " << g_worst_abs);
}
