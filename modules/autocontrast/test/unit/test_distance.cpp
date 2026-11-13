// AutoContrast: the fingerprint distance function (fingerprint/distance.py) vs the Python goldens
// (synthetic/distance, including distance/fingerprints: the exact fingerprints that stage measured).
#include <catch_amalgamated.hpp>

#include "Npy.h"
#include "core/Json.h"
#include "core/fingerprint/Distance.h"
#include "core/fingerprint/Extract.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::string golden(const std::string& rel) { return std::string(AC_TEST_DIR) + "/golden/synthetic/" + rel; }

const ac::FingerprintData& fp(std::string name)   // by value: no temporary bound to a reference parameter
{
   static std::map<std::string, ac::FingerprintData> cache;   // test-only memo; tests run single-threaded
   auto it = cache.find(name);
   if (it == cache.end()) {
      const auto j = ac::test::load_json(golden("distance/fingerprints/" + name + ".json"));
      it = cache.emplace(name, ac::FingerprintData::from_json(ac::Json::parse(j.dump()))).first;
   }
   return it->second;
}

ac::DistanceWeights weights_from(const nlohmann::json& w)
{
   ac::DistanceWeights W;
   if (w.is_null())
      return W;
   W.spectrum = w.at("spectrum").get<double>();
   W.tonal = w.at("tonal").get<double>();
   W.chroma = w.at("chroma").get<double>();
   W.background = w.at("background").get<double>();
   return W;
}

ac::FingerprintData with_hist(const std::vector<double>& h)
{
   ac::FingerprintData f;
   f.chroma_hist = h;
   return f;
}

} // namespace

TEST_CASE("chroma_distance matches the exact reference and the Python golden (20 pairs)", "[distance]")
{
   // Python's _chroma_distance uses scipy.stats.wasserstein_distance_nd: HiGHS on the dual LP through milp at its
   // default 1e-7 tolerances. On pairs 2 and 3 that lands 1.97e-9 and 9.24e-9 (after /diagonal) below the optimum
   // that the dual certificate proves and HiGHS on the primal at 1e-10 (emd_exact_normalized.npy) confirms, so
   // there the Python golden is held to scipy's own tolerance (2e-8 normalised = 5e-6 raw) and the exact
   // reference to 1e-12. Every other pair: <= 1e-9 vs Python and <= 1e-12 vs the exact reference.
   const auto pairs = ac::test::load_npy(golden("distance/emd_pairs.npy"));
   const auto norm = ac::test::load_npy(golden("distance/emd_normalized.npy"));
   const auto exact = ac::test::load_npy(golden("distance/emd_exact_normalized.npy"));
   REQUIRE(norm.size() == 20);
   REQUIRE(exact.size() == 20);
   double worst_py = 0.0, worst_exact = 0.0;
   for (size_t p = 0; p < 20; ++p) {
      const std::vector<double> h0(pairs.data.begin() + p * 512, pairs.data.begin() + p * 512 + 256);
      const std::vector<double> h1(pairs.data.begin() + p * 512 + 256, pairs.data.begin() + p * 512 + 512);
      const double got = ac::chroma_distance(with_hist(h0), with_hist(h1));
      INFO("pair " << p << " C++ " << got << " Python " << norm.data[p] << " exact " << exact.data[p]);
      const double py_tol = (p == 2 || p == 3) ? 2e-8 : 1e-9;
      REQUIRE(std::abs(got - exact.data[p]) <= 1e-12);
      REQUIRE(std::abs(got - norm.data[p]) <= py_tol);
      worst_exact = std::max(worst_exact, std::abs(got - exact.data[p]));
      worst_py = std::max(worst_py, std::abs(got - norm.data[p]));
   }
   std::printf("[distance] chroma_distance: max |delta| vs exact reference %.3g, vs Python %.3g\n", worst_exact,
               worst_py);
}

TEST_CASE("chroma_distance_neutral_is_zero", "[distance]")
{
   std::vector<double> h(256, 0.0), zero(256, 0.0);
   h[5] = 1.0;
   REQUIRE(ac::chroma_distance(with_hist(zero), with_hist(h)) == 0.0);
   REQUIRE(ac::chroma_distance(with_hist(h), with_hist(zero)) == 0.0);
   REQUIRE(ac::chroma_distance(with_hist(zero), with_hist(zero)) == 0.0);
}

TEST_CASE("combine_palette_mismatch_ignores_chroma", "[distance]")
{
   const ac::DistanceWeights W;
   for (double other : {0.0, 0.3, 1.0}) {
      const ac::DistanceComponents c0{other, 0.2, 0.0, 0.7}, c1{other, 0.2, 1.0, 0.7};
      REQUIRE(ac::combine_distance(c0, W, false) == ac::combine_distance(c1, W, false));
      REQUIRE(ac::combine_distance(c0, W, true) != ac::combine_distance(c1, W, true));
   }
}

TEST_CASE("combine_equal_components_returns_value", "[distance]")
{
   for (double x : {0.0, 0.25, 0.5, 1.0}) {
      const ac::DistanceComponents c{x, x, x, x};
      REQUIRE(ac::combine_distance(c, {}, true) == Catch::Approx(x).margin(1e-15));
      REQUIRE(ac::combine_distance(c, {}, false) == Catch::Approx(x).margin(1e-15));
   }
}

TEST_CASE("combine_distance matches the golden cases; zero surviving weight refuses loudly", "[distance]")
{
   const auto cases = ac::test::load_json(golden("distance/combine_distance.json"))["cases"];
   REQUIRE(cases.size() == 6);
   for (const auto& c : cases) {
      const auto v = c["components"].get<std::vector<double>>();
      const ac::DistanceComponents comp{v[0], v[1], v[2], v[3]};
      const double got = ac::combine_distance(comp, weights_from(c["weights"]), c["palette_match"].get<bool>());
      REQUIRE(std::abs(got - c["d"].get<double>()) <= 1e-15);
   }
   // Python: total / used_weight with used_weight == 0 raises ZeroDivisionError; C++ refuses with the weights named
   const ac::DistanceWeights only_chroma{0.0, 0.0, 1.0, 0.0};
   REQUIRE_THROWS_AS(ac::combine_distance({0.1, 0.2, 0.3, 0.4}, only_chroma, false), std::invalid_argument);
   REQUIRE_THROWS_WITH(ac::combine_distance({0.1, 0.2, 0.3, 0.4}, only_chroma, false),
                       Catch::Matchers::ContainsSubstring("chroma=1"));
   REQUIRE(ac::combine_distance({0.1, 0.2, 0.3, 0.4}, only_chroma, true) == 0.3);
}

TEST_CASE("squash matches the golden", "[distance]")
{
   const auto g = ac::test::load_json(golden("distance/squash.json"));
   const std::vector<double> x = {0.0, 0.5, 1.0, 3.0, 1e6, std::numeric_limits<double>::infinity()};
   REQUIRE(g["y"].size() == x.size());
   for (size_t i = 0; i < x.size(); ++i)
      REQUIRE(ac::squash(x[i]) == g["y"][i].get<double>());
   REQUIRE(ac::squash(std::numeric_limits<double>::quiet_NaN()) == 1.0);   // np.isfinite(nan) is False
}

TEST_CASE("fingerprint_distance_matches_golden: 20 pairs x 3 weightings", "[distance]")
{
   const auto cases = ac::test::load_json(golden("distance/fingerprint_distance.json"))["cases"];
   REQUIRE(cases.size() == 20);
   double worst = 0.0;
   int errors = 0;
   auto track = [&](const char* what, double got, double want) {
      INFO(what << ": C++ " << got << " Python " << want);
      REQUIRE(std::abs(got - want) <= 1e-9);
      worst = std::max(worst, std::abs(got - want));
   };
   for (const auto& c : cases) {
      const ac::FingerprintData ref = fp(c["ref"].get<std::string>());
      const ac::FingerprintData tgt = fp(c["target"].get<std::string>());
      INFO(c["ref"].get<std::string>() << " vs " << c["target"].get<std::string>());

      const auto [sd, usable] = ac::spectrum_distance(ref, tgt);
      REQUIRE(usable == c["spectrum_usable"].get<bool>());
      const auto& comp = c["components"];
      track("spectrum", sd, comp["spectrum"].get<double>());
      track("tonal", ac::tonal_distance(ref, tgt), comp["tonal"].get<double>());
      track("chroma", ac::chroma_distance(ref, tgt), comp["chroma"].get<double>());
      track("background", ac::background_distance(ref, tgt), comp["background"].get<double>());
      track("D", ac::fingerprint_distance(ref, tgt), c["d"].get<double>());

      for (const auto& w : c["weighted"]) {
         const auto W = weights_from(w["weights"]);
         if (w["error"].is_null()) {
            track("D weighted", ac::fingerprint_distance(ref, tgt, W), w["d"].get<double>());
         } else {
            REQUIRE(w["error"].get<std::string>() == "ZeroDivisionError");
            REQUIRE_THROWS_AS(ac::fingerprint_distance(ref, tgt, W), std::invalid_argument);
            ++errors;
         }
      }
   }
   // every (spectrum-only weights, spectrum unusable) case: ref_bigpsf/flat06, ref/flat06_psf20 and the two
   // cross-resolution pairs ref_fine/flat06, ref/flat06_fine -- Python raises, C++ refuses
   REQUIRE(errors == 4);
   std::printf("[distance] fingerprint_distance and components vs Python: max |delta| %.3g\n", worst);
}

TEST_CASE("fingerprint_distance: identical fingerprints are at distance 0", "[distance]")
{
   REQUIRE(std::abs(ac::fingerprint_distance(fp("ref"), fp("ref"))) <= 1e-12);
}

TEST_CASE("distance_unusable_band_redistributes", "[distance]")
{
   // ref_bigpsf has a 500 arcsec PSF: no comparable band, so the spectrum term is dropped and its weight is
   // redistributed over tonal / chroma / background (combine_distance option (b)).
   const ac::FingerprintData ref = fp("ref_bigpsf");
   const ac::FingerprintData tgt = fp("flat06");
   const auto [sd, usable] = ac::spectrum_distance(ref, tgt);
   REQUIRE_FALSE(usable);
   REQUIRE(sd == 0.0);
   const ac::DistanceComponents comp{0.0, ac::tonal_distance(ref, tgt), ac::chroma_distance(ref, tgt),
                                     ac::background_distance(ref, tgt)};
   ac::DistanceWeights W;
   W.spectrum = 0.0;
   const double want = (0.25 * comp.tonal + 0.20 * comp.chroma + 0.15 * comp.background) / (0.25 + 0.20 + 0.15);
   REQUIRE(ac::fingerprint_distance(ref, tgt) == Catch::Approx(want).margin(1e-15));
   REQUIRE(ac::fingerprint_distance(ref, tgt) == ac::combine_distance(comp, W, true));
   // a large spectrum value would have moved D if the spectrum were still weighted
   const ac::DistanceComponents loud{1.0, comp.tonal, comp.chroma, comp.background};
   REQUIRE(ac::combine_distance(loud, W, true) == ac::combine_distance(comp, W, true));
}
