// AutoContrast: the six §7 guardrails (optimize/guardrails.py) vs the Python goldens (synthetic/guardrails).
// Images are 8-bit renders stored uint8: float image = u8 / 255.0 (MANIFEST image_convention).
#include <catch_amalgamated.hpp>

#include "Npy.h"
#include "core/optimize/Guardrails.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <string>

namespace {

std::string golden(const std::string& rel) { return std::string(AC_TEST_DIR) + "/golden/synthetic/guardrails/" + rel; }

const ac::Image& img(const std::string& name)
{
   static std::map<std::string, ac::Image> cache;
   auto it = cache.find(name);
   if (it == cache.end()) {
      const auto a = ac::test::load_npy(golden("img_" + name + "_u8.npy"));
      REQUIRE(a.shape.size() == 3);
      ac::Image im(static_cast<int>(a.shape[1]), static_cast<int>(a.shape[0]), static_cast<int>(a.shape[2]));
      REQUIRE(im.data.size() == a.data.size());
      for (size_t i = 0; i < a.data.size(); ++i)
         im.data[i] = a.data[i] / 255.0;
      it = cache.emplace(name, std::move(im)).first;
   }
   return it->second;
}

double num(const nlohmann::json& j)
{
   if (j.is_string()) {
      const std::string s = j.get<std::string>();
      if (s == "Infinity") return std::numeric_limits<double>::infinity();
      if (s == "-Infinity") return -std::numeric_limits<double>::infinity();
      return std::numeric_limits<double>::quiet_NaN();
   }
   return j.get<double>();
}

const nlohmann::json& cases()
{
   static const nlohmann::json j = ac::test::load_json(golden("cases.json"));
   return j;
}

ac::GuardrailLimits limits_from(const nlohmann::json& kw)
{
   ac::GuardrailLimits L;
   for (auto it = kw.begin(); it != kw.end(); ++it) {
      const double v = it.value().get<double>();
      const std::string& k = it.key();
      if (k == "noise_growth") L.noise_growth = v;
      else if (k == "star_count_drop") L.star_count_drop = v;
      else if (k == "star_fwhm_growth") L.star_fwhm_growth = v;
      else if (k == "star_ecc_growth") L.star_ecc_growth = v;
      else if (k == "hue_tolerance_deg") L.hue_tolerance_deg = v;
      else if (k == "channel_ratio_drift") L.channel_ratio_drift = v;
      else FAIL("unknown limit " << k);
   }
   return L;
}

const nlohmann::json* find_case(const std::string& name, const std::string& limits)
{
   for (const auto& c : cases().at("cases"))
      if (c.at("name") == name && c.at("limits") == limits)
         return &c;
   return nullptr;
}

// Runs one golden case; every verdict's name/ok/reason exact, value within 1e-12, limit exact.
void check_case(const std::string& name, const std::string& limits, double* max_dv = nullptr)
{
   const auto* c = find_case(name, limits);
   REQUIRE(c != nullptr);
   const auto L = limits_from(c->at("limits_kw"));
   const auto got = ac::evaluate_guardrails(img(c->at("candidate")), img(c->at("baseline")), img(c->at("source")), L);
   const auto& want = c->at("verdicts");
   REQUIRE(got.size() == want.size());
   for (size_t i = 0; i < got.size(); ++i) {
      INFO(name << "/" << limits << " verdict " << want[i].at("name").get<std::string>());
      CHECK(got[i].name == want[i].at("name").get<std::string>());
      CHECK(got[i].ok == want[i].at("ok").get<bool>());
      CHECK(got[i].reason == want[i].at("reason").get<std::string>());
      const double wv = num(want[i].at("value")), wl = num(want[i].at("limit"));
      if (std::isinf(wv))
         CHECK(got[i].value == wv);
      else {
         CHECK(std::fabs(got[i].value - wv) <= 1e-12);
         if (max_dv) *max_dv = std::max(*max_dv, std::fabs(got[i].value - wv));
      }
      CHECK(got[i].limit == wl);
   }
   // the tripped list agrees with the stage's own record
   std::vector<std::string> tripped;
   for (const auto& v : got)
      if (!v.ok) tripped.push_back(v.name);
   std::vector<std::string> wt;
   for (const auto& t : c->at("tripped")) wt.push_back(t.get<std::string>());
   CHECK(tripped == wt);
}

} // namespace

TEST_CASE("limit defaults match the Python dataclass", "[guardrails]")
{
   const auto& d = cases().at("limit_defaults");
   const ac::GuardrailLimits L;
   CHECK(L.noise_growth == d.at("noise_growth").get<double>());
   CHECK(L.shadow_clip_fraction == d.at("shadow_clip_fraction").get<double>());
   CHECK(L.highlight_clip_fraction == d.at("highlight_clip_fraction").get<double>());
   CHECK(L.star_count_drop == d.at("star_count_drop").get<double>());
   CHECK(L.star_fwhm_growth == d.at("star_fwhm_growth").get<double>());
   CHECK(L.star_ecc_growth == d.at("star_ecc_growth").get<double>());
   CHECK(L.hue_invention_mass == d.at("hue_invention_mass").get<double>());
   CHECK(L.hue_tolerance_deg == d.at("hue_tolerance_deg").get<double>());
   CHECK(L.channel_ratio_drift == d.at("channel_ratio_drift").get<double>());
   const auto& k = cases().at("consts");
   CHECK(ac::kMadToSigma == k.at("MAD_TO_SIGMA").get<double>());
   CHECK(ac::kNumHueBins == k.at("NUM_HUE_BINS").get<int>());
   CHECK(ac::kHueChromaFloor == k.at("HUE_CHROMA_FLOOR").get<double>());
}

TEST_CASE("component quantities per image", "[guardrails]")
{
   static const auto comp = ac::test::load_json(golden("components.json"));
   for (auto it = comp.begin(); it != comp.end(); ++it) {
      const std::string name = it.key();
      if (name == "base_ksigma") continue;
      DYNAMIC_SECTION(name)
      {
         const auto& im = img(name);
         const auto gray = ac::gray_of(im);
         CHECK(std::fabs(ac::mrs_noise_sigma(gray) - it.value().at("mrs_noise_sigma").get<double>()) <= 1e-12);
         const auto st = ac::detect_stars(gray);
         CHECK(st.count == it.value().at("stars").at("count").get<int>());
         CHECK(std::fabs(st.median_fwhm - it.value().at("stars").at("median_fwhm").get<double>()) <= 1e-12);
         CHECK(std::fabs(st.median_ecc - it.value().at("stars").at("median_ecc").get<double>()) <= 1e-12);
         const auto c0 = ac::clipped_fraction(im, 0.0), c1 = ac::clipped_fraction(im, 1.0);
         for (int c = 0; c < 3; ++c) {
            CHECK(c0[c] == it.value().at("clipped_at_0")[c].get<double>());
            CHECK(c1[c] == it.value().at("clipped_at_1")[c].get<double>());
         }
         const auto cr = ac::channel_ratios(im);
         for (int c = 0; c < 3; ++c)
            CHECK(std::fabs(cr[c] - it.value().at("channel_ratios")[c].get<double>()) <= 1e-12);
         // hue mass: Lab is ulp-level (not bit) identical to numpy, so compare to a relative tolerance and
         // require the SAME occupied bins (the support decision is what the guardrail uses).
         const auto hm = ac::hue_mass_by_bin(im);
         const auto want = ac::test::load_npy(golden("huemass_" + name + ".npy"));
         REQUIRE(hm.size() == want.size());
         for (size_t b = 0; b < hm.size(); ++b) {
            CHECK((hm[b] > 0.0) == (want.data[b] > 0.0));
            CHECK(std::fabs(hm[b] - want.data[b]) <= 1e-9 * std::max(1.0, want.data[b]));
         }
      }
   }
   for (const double k : {3.0, 5.0, 8.0}) {
      const auto s = ac::detect_stars(ac::gray_of(img("base")), k);
      const auto& w = comp.at("base_ksigma").at(k == 3.0 ? "3.0" : k == 5.0 ? "5.0" : "8.0");
      CHECK(s.count == w.at("count").get<int>());
      CHECK(std::fabs(s.median_fwhm - w.at("median_fwhm").get<double>()) <= 1e-12);
      CHECK(std::fabs(s.median_ecc - w.at("median_ecc").get<double>()) <= 1e-12);
   }
}

TEST_CASE("circular hue dilation", "[guardrails]")
{
   const auto in = ac::test::load_npy(golden("dilate_in.npy"));
   std::vector<uint8_t> occ(in.data.size());
   for (size_t i = 0; i < occ.size(); ++i) occ[i] = in.data[i] != 0.0;
   for (const int r : {0, 1, 20, 40}) {
      const auto want = ac::test::load_npy(golden("dilate_r" + std::to_string(r) + ".npy"));
      const auto got = ac::dilate_circular(occ, r);
      REQUIRE(got.size() == want.size());
      for (size_t i = 0; i < got.size(); ++i)
         CHECK((got[i] != 0) == (want.data[i] != 0.0));
   }
}

TEST_CASE("pass case", "[guardrails]") { check_case("ok", "default"); }
TEST_CASE("noise_floor_trips", "[guardrails]") { check_case("noise", "default"); }
TEST_CASE("noise_floor_nonpositive_baseline_fails_closed", "[guardrails]")
{
   check_case("baseline_sigma_zero", "default");
   const auto got = ac::evaluate_guardrails(img("base"), img("const"), img("base"));
   CHECK_FALSE(got[0].ok);
   CHECK(std::isinf(got[0].value));
   CHECK(got[0].reason == "baseline noise sigma was non-positive (0.0); noise growth could not be assessed");
}
TEST_CASE("shadow_clip_trips", "[guardrails]") { check_case("shadow", "default"); }
TEST_CASE("highlight_clip_trips", "[guardrails]") { check_case("highlight", "default"); }
TEST_CASE("star_count_drop_trips", "[guardrails]") { check_case("star_loss", "default"); }
TEST_CASE("star_fwhm_trips", "[guardrails]")
{
   // star_bloat trips on count first in the default limits; the tight variant and loosened count limit reach FWHM.
   check_case("star_bloat", "default");
   check_case("hue", "tight");   // reaches the FWHM branch ("star FWHM ballooned 4.8% ... limit 0.0%")
   ac::GuardrailLimits L;
   L.star_count_drop = 1.0;
   const auto v = ac::check_star_integrity(img("starbloat"), img("base"), L);
   CHECK_FALSE(v.ok);
   CHECK(v.reason.rfind("star FWHM ballooned", 0) == 0);
}
TEST_CASE("star_ecc_trips", "[guardrails]") { check_case("star_ecc", "default"); }
TEST_CASE("starless_baseline_noted_not_failed", "[guardrails]")
{
   check_case("starless_baseline", "default");
   check_case("starless_baseline", "tight");
   const auto v = ac::check_star_integrity(img("starless"), img("starless"), {});
   CHECK(v.ok);
   CHECK(v.reason == "star integrity not assessed: baseline had no detectable stars");
}
TEST_CASE("hue_invention_angle_not_cell", "[guardrails]")
{
   check_case("hue", "default");      // a 116-degree-class shift trips
   check_case("hue", "tight");
   check_case("ok", "default");       // boost within tolerance passes
   check_case("mild_noise", "default");
}
TEST_CASE("hue_wraps_at_360", "[guardrails]")
{
   // A source hue just below 360 supports a candidate hue just above 0: the dilation wraps.
   std::vector<uint8_t> occ(360, 0);
   occ[359] = 1;
   const auto d = ac::dilate_circular(occ, 20);
   CHECK(d[0]);
   CHECK(d[19]);
   CHECK_FALSE(d[20]);
   CHECK(d[339]);
   CHECK_FALSE(d[338]);
}
TEST_CASE("neutral_source_any_chroma_trips", "[guardrails]")
{
   check_case("hue_neutral_source", "default");
   check_case("hue_neutral_source", "tight");
}
TEST_CASE("channel_ratio_drift_trips", "[guardrails]")
{
   check_case("ratio", "default");
   check_case("ratio", "tight");
}
TEST_CASE("every golden case, both limit sets", "[guardrails]")
{
   double max_dv = 0.0;
   for (const auto& c : cases().at("cases"))
      check_case(c.at("name"), c.at("limits"), &max_dv);
   CHECK(max_dv <= 1e-12);
}
TEST_CASE("guardrails_thread_invariant", "[guardrails]")
{
   for (const auto& c : cases().at("cases")) {
      const auto L = limits_from(c.at("limits_kw"));
      // named strings: img() returns into its static cache, but GCC's -Wdangling-reference cannot see that
      // through a temporary std::string argument
      const std::string cand_name = c.at("candidate"), base_name = c.at("baseline"), src_name = c.at("source");
      const auto& cand = img(cand_name);
      const auto& base = img(base_name);
      const auto& src = img(src_name);
      const auto a = ac::evaluate_guardrails(cand, base, src, L, 1);
      const auto b = ac::evaluate_guardrails(cand, base, src, L, 7);
      REQUIRE(a.size() == b.size());
      for (size_t i = 0; i < a.size(); ++i) {
         CHECK(a[i].ok == b[i].ok);
         CHECK(a[i].reason == b[i].reason);
         CHECK(std::memcmp(&a[i].value, &b[i].value, sizeof(double)) == 0);
         CHECK(std::memcmp(&a[i].limit, &b[i].limit, sizeof(double)) == 0);
      }
   }
}
