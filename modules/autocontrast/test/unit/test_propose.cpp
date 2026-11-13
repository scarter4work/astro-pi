// AutoContrast: the gap-ranked proposer (optimize/propose.py) vs the Python goldens (synthetic/propose).
// The propose stage's fingerprints are synthetic/fingerprints/ (NOT distance/fingerprints/).
#include <catch_amalgamated.hpp>

#include "Npy.h"
#include "core/Json.h"
#include "core/fingerprint/Extract.h"
#include "core/optimize/Actions.h"
#include "core/optimize/Propose.h"
#include "core/optimize/Recipe.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <string>

namespace {

std::string golden(const std::string& rel) { return std::string(AC_TEST_DIR) + "/golden/synthetic/" + rel; }

const ac::FingerprintData& fp(const std::string& name)
{
   static std::map<std::string, ac::FingerprintData> cache;
   auto it = cache.find(name);
   if (it == cache.end()) {
      const auto j = ac::test::load_json(golden("fingerprints/" + name + ".json"));
      it = cache.emplace(name, ac::FingerprintData::from_json(ac::Json::parse(j.dump()))).first;
   }
   return it->second;
}

ac::Json cases()
{
   std::ifstream f(golden("propose/cases.json"));
   return ac::Json::parse(f);
}

std::set<std::string> kinds(const ac::Json& a)
{
   std::set<std::string> s;
   for (const auto& k : a)
      s.insert(k.get<std::string>());
   return s;
}

void check_list(const std::vector<ac::Action>& got, const ac::Json& want)
{
   REQUIRE(got.size() == want.size());
   for (size_t i = 0; i < got.size(); ++i) {
      ac::Json w = ac::Json::object();
      w["kind"] = want[i].at("kind");
      w["level"] = want[i].at("level");
      w["scale_arcsec"] = want[i].at("scale_arcsec");
      w["params"] = want[i].at("params");
      CHECK(ac::dump_python(ac::action_to_json(got[i])) == ac::dump_python(w));
      CHECK(got[i].key() == want[i].at("key").get<std::string>());
   }
}

} // namespace

TEST_CASE("level_for_gap thresholds", "[propose]")
{
   std::ifstream f(golden("actions/key_strength_layer.json"));
   const auto g = ac::Json::parse(f);
   for (const auto& r : g.at("level_for_gap"))
      CHECK(ac::level_for_gap(r.at("gap")) == r.at("level").get<std::string>());
}

TEST_CASE("component_gaps / band_deficits match Python", "[propose]")
{
   const auto g = cases();
   REQUIRE(g.at("cases").size() == 12);
   for (const auto& c : g.at("cases")) {
      INFO(c.at("ref").get<std::string>() + " vs " + c.at("target").get<std::string>());
      const ac::FingerprintData ref = fp(c.at("ref").get<std::string>());
      const ac::FingerprintData tgt = fp(c.at("target").get<std::string>());
      const auto gaps = ac::component_gaps(ref, tgt);
      for (const auto& [k, v] : c.at("component_gaps").items())
         CHECK(gaps.at(k) == Catch::Approx(v.get<double>()).margin(1e-9));
      const auto d = ac::band_deficits(ref, tgt);
      REQUIRE(d.size() == c.at("band_deficits").size());
      for (size_t i = 0; i < d.size(); ++i)
         CHECK(d[i] == Catch::Approx(c.at("band_deficits")[i].get<double>()).margin(1e-9));
   }
}

TEST_CASE("propose_matches_python", "[propose]")
{
   const auto g = cases();
   for (const auto& c : g.at("cases")) {
      INFO(c.at("ref").get<std::string>() + " vs " + c.at("target").get<std::string>());
      const ac::FingerprintData ref = fp(c.at("ref").get<std::string>());
      const ac::FingerprintData tgt = fp(c.at("target").get<std::string>());
      const auto applied = kinds(c.at("applied_kinds"));
      const int n = c.at("n_scales");
      check_list(ac::propose_actions(ref, tgt, applied, n, 3), c.at("top3"));
      check_list(ac::propose_actions(ref, tgt, applied, n, std::nullopt), c.at("all"));
      check_list(ac::propose_actions(ref, tgt, applied, n, 1), c.at("top1"));
      check_list(ac::propose_actions(ref, tgt, applied, n, 7), c.at("top7"));
   }
}

TEST_CASE("propose_withholds_chroma_on_mismatch", "[propose]")
{
   // ref_hoo vs flat06 (RGB-family target): palettes are not chroma-compatible.
   const auto all = ac::propose_actions(fp("ref_hoo"), fp("flat06"), {}, 6, std::nullopt);
   REQUIRE_FALSE(all.empty());
   CHECK(std::none_of(all.begin(), all.end(), [](const ac::Action& a) { return a.kind == "chroma"; }));
   const auto ok = ac::propose_actions(fp("ref"), fp("flat06"), {}, 6, std::nullopt);
   CHECK(std::any_of(ok.begin(), ok.end(), [](const ac::Action& a) { return a.kind == "chroma"; }));
}

TEST_CASE("group_sort_numeric_not_string", "[propose]")
{
   // 16.000 sorts after 2.000 as a number (as a string "16.000" < "2.000"): ref vs flat06, all groups.
   const auto all = ac::propose_actions(fp("ref"), fp("flat06"), {}, 6, std::nullopt);
   int i2 = -1, i16 = -1;
   for (size_t i = 0; i < all.size(); ++i) {
      if (all[i].kind != "local_contrast" || !all[i].scale_arcsec)
         continue;
      if (*all[i].scale_arcsec == 2.0) i2 = static_cast<int>(i);
      if (*all[i].scale_arcsec == 16.0) i16 = static_cast<int>(i);
   }
   REQUIRE(i2 >= 0);
   REQUIRE(i16 >= 0);
   CHECK(i2 < i16);
}
