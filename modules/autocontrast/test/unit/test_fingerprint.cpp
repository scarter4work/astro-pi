// AutoContrast: CIELAB, metrics, fingerprint extraction and fingerprint JSON vs the Python goldens
// (synthetic/color, synthetic/metrics, synthetic/extract).
#include <catch_amalgamated.hpp>

#include "Npy.h"
#include "core/Json.h"
#include "core/fingerprint/Color.h"
#include "core/fingerprint/Extract.h"
#include "core/fingerprint/Metrics.h"
#include "core/fingerprint/Starlet.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::string golden(const std::string& rel) { return std::string(AC_TEST_DIR) + "/golden/synthetic/" + rel; }

ac::test::NpyArray npy(const std::string& rel) { return ac::test::load_npy(golden(rel)); }

std::string read_text(const std::string& path)
{
   std::ifstream f(path, std::ios::binary);
   if (!f)
      throw std::runtime_error("cannot open " + path);
   std::ostringstream ss;
   ss << f.rdbuf();
   return ss.str();
}

// 8-bit renders are stored as uint8; the float image is u8 / 255.0 (MANIFEST image_convention).
ac::Image image_of(const ac::test::NpyArray& a)
{
   REQUIRE(a.shape.size() == 3);
   REQUIRE(a.shape[2] == 3);
   ac::Image im(static_cast<int>(a.shape[1]), static_cast<int>(a.shape[0]), 3);
   const bool u8 = a.dtype == "|u1";
   for (size_t i = 0; i < a.data.size(); ++i)
      im.data[i] = u8 ? a.data[i] / 255.0 : a.data[i];
   return im;
}

ac::Plane plane_of(const ac::test::NpyArray& a)
{
   REQUIRE(a.shape.size() == 2);
   ac::Plane p(static_cast<int>(a.shape[1]), static_cast<int>(a.shape[0]));
   p.data = a.data;
   return p;
}

// a (H, W, 3) Lab golden -> three planes
void lab_planes(const ac::test::NpyArray& lab, ac::Plane& L, ac::Plane& a, ac::Plane& b)
{
   REQUIRE(lab.shape.size() == 3);
   const int h = static_cast<int>(lab.shape[0]), w = static_cast<int>(lab.shape[1]);
   L = ac::Plane(w, h);
   a = ac::Plane(w, h);
   b = ac::Plane(w, h);
   for (size_t i = 0; i < L.data.size(); ++i) {
      L.data[i] = lab.data[i * 3];
      a.data[i] = lab.data[i * 3 + 1];
      b.data[i] = lab.data[i * 3 + 2];
   }
}

ac::Plane chroma_of(const ac::Plane& a, const ac::Plane& b)
{
   ac::Plane c(a.width, a.height);
   for (size_t i = 0; i < c.data.size(); ++i)
      c.data[i] = std::hypot(a.data[i], b.data[i]);
   return c;
}

// golden JSON number: NaN / Infinity / -Infinity arrive as strings
double num(const nlohmann::json& j)
{
   if (j.is_string()) {
      const std::string s = j.get<std::string>();
      if (s == "Infinity") return std::numeric_limits<double>::infinity();
      if (s == "-Infinity") return -std::numeric_limits<double>::infinity();
      if (s == "NaN") return std::numeric_limits<double>::quiet_NaN();
      throw std::runtime_error("not a number: " + s);
   }
   return j.get<double>();
}

std::vector<double> nums(const nlohmann::json& j)
{
   std::vector<double> v;
   for (const auto& x : j)
      v.push_back(num(x));
   return v;
}

// golden fingerprint (enc'd JSON, strings for non-finite) -> a plain Json with real doubles
ac::Json decode_fp(const nlohmann::json& j)
{
   std::function<ac::Json(const nlohmann::json&)> dec = [&](const nlohmann::json& x) -> ac::Json {
      if (x.is_object()) {
         ac::Json o = ac::Json::object();
         for (auto it = x.begin(); it != x.end(); ++it)
            o[it.key()] = it.key() == "palette_class" ? ac::Json(it.value().get<std::string>()) : dec(it.value());
         return o;
      }
      if (x.is_array()) {
         ac::Json a = ac::Json::array();
         for (const auto& e : x)
            a.push_back(dec(e));
         return a;
      }
      if (x.is_string()) {
         const std::string s = x.get<std::string>();
         if (s == "Infinity" || s == "-Infinity" || s == "NaN")
            return ac::Json(num(x));
         return ac::Json(s);
      }
      return ac::Json(x.get<double>());
   };
   return dec(j);
}

// the golden objects are written with sort_keys; rebuild them in to_dict order
ac::Json in_to_dict_order(const ac::Json& g)
{
   ac::Json j = ac::Json::object();
   ac::Json es = ac::Json::object();
   es["centers_arcsec"] = g.at("energy_spectrum").at("centers_arcsec");
   es["energy"] = g.at("energy_spectrum").at("energy");
   j["energy_spectrum"] = es;
   for (const char* k : {"tonal_quantiles", "saturation_quantiles", "chroma_hist"})
      j[k] = g.at(k);
   ac::Json bg = ac::Json::object();
   for (const char* k : {"floor_to_peak_ratio", "structure_to_gradient_ratio", "channel_balance"})
      bg[k] = g.at("background").at(k);
   j["background"] = bg;
   for (const char* k : {"pixel_scale_arcsec", "psf_fwhm_arcsec", "palette_class"})
      j[k] = g.at(k);
   return j;
}

double g_worst_lab = 0.0, g_worst_extract = 0.0;

struct Worst {
   double abs = 0.0, rel = 0.0;
   void add(double got, double want)
   {
      if (std::isnan(got) && std::isnan(want)) return;
      if (got == want) return;
      const double d = std::fabs(got - want);
      abs = std::max(abs, d);
      rel = std::max(rel, d / std::max(std::fabs(want), 1e-300));
   }
};

void require_close(double got, double want, double rel_tol, const char* what, Worst& w)
{
   INFO(what << ": got " << got << " want " << want);
   if (std::isnan(want)) {
      REQUIRE(std::isnan(got));
      return;
   }
   if (std::isinf(want)) {
      REQUIRE(got == want);
      return;
   }
   w.add(got, want);
   REQUIRE(std::fabs(got - want) <= rel_tol * std::max(std::fabs(want), 1.0));
}

void require_vec(const std::vector<double>& got, const std::vector<double>& want, double tol, const char* what, Worst& w)
{
   INFO(what);
   REQUIRE(got.size() == want.size());
   for (size_t i = 0; i < got.size(); ++i)
      require_close(got[i], want[i], tol, what, w);
}

struct ExtractCase {
   std::string name;
   nlohmann::json g;
};

std::vector<ExtractCase> extract_cases()
{
   std::vector<ExtractCase> out;
   for (const char* n : {"nebula", "nebula_fine", "flat06", "gray", "cast", "tiny", "noise"})
      out.push_back({n, ac::test::load_json(golden(std::string("extract/fingerprint_") + n + ".json"))});
   return out;
}

ac::FingerprintData run_case(const ExtractCase& c, unsigned threads = 0)
{
   const auto& p = c.g["params"];
   return ac::extract(image_of(npy(c.g["input"].get<std::string>())), num(p["pixel_scale_arcsec"]),
                      p["n_scales"].get<int>(), num(p["psf_fwhm_arcsec"]), p["palette_class"].get<std::string>(),
                      threads);
}

} // namespace

TEST_CASE("lab_matches_python", "[color]")
{
   Worst w;
   for (const char* name : {"ramp", "rand", "primaries"}) {
      const auto lab = npy(std::string("color/") + name + "_lab.npy");
      ac::Image rgb;
      if (std::string(name) == "ramp")
         rgb = image_of(npy("color/ramp_u8.npy"));
      else
         rgb = image_of(npy(std::string("color/") + name + "_rgb.npy"));
      ac::Plane L, a, b;
      ac::rgb_to_lab(rgb, L, a, b);
      ac::Plane gL, ga, gb;
      lab_planes(lab, gL, ga, gb);
      INFO(name);
      REQUIRE(L.data.size() == gL.data.size());
      for (size_t i = 0; i < L.data.size(); ++i) {
         REQUIRE(std::fabs(L.data[i] - gL.data[i]) <= 1e-12);
         REQUIRE(std::fabs(a.data[i] - ga.data[i]) <= 1e-12);
         REQUIRE(std::fabs(b.data[i] - gb.data[i]) <= 1e-12);
         w.add(L.data[i], gL.data[i]);
         w.add(a.data[i], ga.data[i]);
         w.add(b.data[i], gb.data[i]);
      }
   }
   {   // the 60x80 nebula crop used by the metrics goldens
      ac::Plane L, a, b, gL, ga, gb;
      ac::rgb_to_lab(image_of(npy("metrics/nebula_crop_u8.npy")), L, a, b);
      lab_planes(npy("metrics/nebula_crop_lab.npy"), gL, ga, gb);
      for (size_t i = 0; i < L.data.size(); ++i) {
         REQUIRE(std::fabs(L.data[i] - gL.data[i]) <= 1e-12);
         REQUIRE(std::fabs(a.data[i] - ga.data[i]) <= 1e-12);
         REQUIRE(std::fabs(b.data[i] - gb.data[i]) <= 1e-12);
         w.add(L.data[i], gL.data[i]);
         w.add(a.data[i], ga.data[i]);
         w.add(b.data[i], gb.data[i]);
      }
   }
   g_worst_lab = w.abs;
   std::printf("[lab] max |delta| vs Python = %.3e (max rel %.3e)\n", w.abs, w.rel);
   ac::Image four(2, 2, 4);
   ac::Plane L, a, b;
   REQUIRE_THROWS_AS(ac::rgb_to_lab(four, L, a, b), std::invalid_argument);
}

TEST_CASE("lab: thread count never changes a bit", "[color]")
{
   const auto rgb = image_of(npy("images/nebula_u8.npy"));
   ac::Plane L1, a1, b1, L7, a7, b7;
   ac::rgb_to_lab(rgb, L1, a1, b1, 1);
   ac::rgb_to_lab(rgb, L7, a7, b7, 7);
   REQUIRE(L1.data == L7.data);
   REQUIRE(a1.data == a7.data);
   REQUIRE(b1.data == b7.data);
}

TEST_CASE("quantile levels", "[metrics]")
{
   const std::array<double, 10> want = {1, 5, 10, 25, 50, 75, 90, 95, 99, 99.9};
   REQUIRE(ac::kQuantileLevels == want);
}

TEST_CASE("tonal quantiles bit-exact, saturation quantiles within tolerance, given Lab inputs", "[metrics]")
{
   ac::Plane L, a, b;
   lab_planes(npy("metrics/nebula_crop_lab.npy"), L, a, b);
   const auto tq = ac::tonal_quantiles(L);
   const auto wt = npy("metrics/tonal_quantiles.npy").data;
   const auto sq = ac::saturation_quantiles(chroma_of(a, b));
   const auto ws = npy("metrics/saturation_quantiles.npy").data;
   REQUIRE(wt.size() == 10);
   for (size_t i = 0; i < 10; ++i) {
      INFO("level " << i);
      REQUIRE(tq[i] == wt[i]);
      // tonal: selection on L, libm-free -> bit-exact. saturation: chroma = hypot(a, b), a libm transcendental that
      // differs by 1 ulp between glibc 2.34 and 2.43 (spec 6.4) -> the spec component tolerance.
      REQUIRE(std::fabs(sq[i] - ws[i]) <= 1e-9 * std::max(std::fabs(ws[i]), 1.0));
   }
}

TEST_CASE("chroma histogram vs golden", "[metrics]")
{
   ac::Plane L, a, b;
   lab_planes(npy("metrics/nebula_crop_lab.npy"), L, a, b);
   Worst w;
   auto check = [&](const std::vector<double>& got, const char* rel) {
      const auto want = npy(rel).data;
      require_vec(got, want, 1e-12, rel, w);
      // bins are chosen from hypot(a, b) (libm, 1 ulp apart across glibc versions, spec 6.4): the 1e-12 bound
      // above is the contract, not bit equality
   };
   check(ac::chroma_histogram(a, b), "metrics/chroma_hist.npy");
   check(ac::chroma_histogram(a, b, 8, 60.0), "metrics/chroma_hist_bins8_extent60.npy");
   ac::Plane z(8, 8);
   const auto neutral = ac::chroma_histogram(z, z);
   REQUIRE(neutral.size() == 256);
   REQUIRE(neutral == npy("metrics/chroma_hist_neutral.npy").data);   // all zero, not NaN
}

TEST_CASE("background channel balance vs golden", "[metrics]")
{
   const auto rgb = image_of(npy("metrics/nebula_crop_u8.npy"));
   REQUIRE(ac::background_channel_balance(rgb) == npy("metrics/balance.npy").data);
   REQUIRE(ac::background_channel_balance(rgb, 10.0) == npy("metrics/balance_p10.npy").data);
   const ac::Image black = image_of(npy("metrics/black_rgb.npy"));
   const auto bb = ac::background_channel_balance(black);
   REQUIRE(bb == npy("metrics/balance_black.npy").data);   // overall == 0 -> ones
   REQUIRE(bb == std::vector<double>{1.0, 1.0, 1.0});
}

TEST_CASE("scalar metrics vs golden", "[metrics]")
{
   const auto s = ac::test::load_json(golden("metrics/scalars.json"));
   Worst w;
   ac::Plane L, a, b;
   lab_planes(npy("metrics/nebula_crop_lab.npy"), L, a, b);
   require_close(ac::floor_to_peak_ratio(L), num(s["floor_to_peak_ratio"]), 1e-12, "floor_to_peak", w);
   REQUIRE(ac::floor_to_peak_ratio(L) == num(s["floor_to_peak_ratio"]));
   REQUIRE(ac::floor_to_peak_ratio(ac::Plane(6, 6)) == num(s["floor_to_peak_zero_peak"]));
   ac::Plane c37(6, 6);
   std::fill(c37.data.begin(), c37.data.end(), 37.5);
   REQUIRE(ac::floor_to_peak_ratio(c37) == num(s["floor_to_peak_constant"]));

   for (int n : {3, 5, 7}) {
      const auto st = ac::starlet_transform(L, n);
      const double got = ac::structure_to_gradient_ratio_from_transform(st);
      const std::string key = "structure_to_gradient_from_transform_n" + std::to_string(n);
      const std::string key2 = "structure_to_gradient_ratio_n" + std::to_string(n);
      require_close(got, num(s[key]), 1e-12, key.c_str(), w);
      REQUIRE(got == num(s[key]));
      REQUIRE(got == num(s[key2]));
   }
   {   // constant / zero images: zero gradient energy -> 0.0 (structure 0), as Python
      ac::Plane c = plane_of(npy("metrics/constant_L.npy"));
      REQUIRE(ac::structure_to_gradient_ratio_from_transform(ac::starlet_transform(c, 3)) ==
              num(s["structure_gradient_constant"]));
      ac::Plane z = plane_of(npy("metrics/zeros_L.npy"));
      REQUIRE(ac::structure_to_gradient_ratio_from_transform(ac::starlet_transform(z, 3)) ==
              num(s["structure_gradient_zero"]));
      ac::Plane t = plane_of(npy("metrics/tiny_L.npy"));
      REQUIRE(ac::structure_to_gradient_ratio_from_transform(ac::starlet_transform(t, 3)) ==
              num(s["structure_gradient_tiny_n3"]));
   }
   {   // hand-built: structure > 0 with a constant residual -> +inf; both zero -> 0.0
      ac::Starlet st;
      st.planes.push_back(ac::Plane(2, 2));
      st.planes[0].data = {1.0, -1.0, 2.0, -2.0};
      st.residual = ac::Plane(2, 2);
      st.residual.data = {5.0, 5.0, 5.0, 5.0};
      REQUIRE(ac::structure_to_gradient_ratio_from_transform(st) == std::numeric_limits<double>::infinity());
      st.planes[0].data = {0.0, 0.0, 0.0, 0.0};
      REQUIRE(ac::structure_to_gradient_ratio_from_transform(st) == 0.0);
   }
   std::printf("[metrics] max |delta| vs Python = %.3e\n", w.abs);
}

TEST_CASE("extract_synthetic_matches_golden", "[extract]")
{
   Worst w;
   for (const auto& c : extract_cases()) {
      INFO(c.name);
      const auto got = run_case(c);
      const auto& g = c.g["fingerprint"];
      require_vec(got.energy.centers_arcsec, nums(g["energy_spectrum"]["centers_arcsec"]), 1e-9, "centers", w);
      require_vec(got.energy.energy, nums(g["energy_spectrum"]["energy"]), 1e-9, "energy", w);
      require_vec(std::vector<double>(got.tonal_quantiles.begin(), got.tonal_quantiles.end()),
                  nums(g["tonal_quantiles"]), 1e-9, "tonal", w);
      require_vec(std::vector<double>(got.saturation_quantiles.begin(), got.saturation_quantiles.end()),
                  nums(g["saturation_quantiles"]), 1e-9, "saturation", w);
      std::vector<double> hist;
      for (const auto& row : g["chroma_hist"])
         for (const auto& x : row)
            hist.push_back(num(x));
      require_vec(got.chroma_hist, hist, 1e-9, "chroma_hist", w);
      require_close(got.background.floor_to_peak_ratio, num(g["background"]["floor_to_peak_ratio"]), 1e-9, "f2p", w);
      require_close(got.background.structure_to_gradient_ratio, num(g["background"]["structure_to_gradient_ratio"]),
                    1e-9, "s2g", w);
      require_vec(got.background.channel_balance, nums(g["background"]["channel_balance"]), 1e-9, "balance", w);
      REQUIRE(got.pixel_scale_arcsec == num(g["pixel_scale_arcsec"]));
      REQUIRE(got.psf_fwhm_arcsec == num(g["psf_fwhm_arcsec"]));
      REQUIRE(got.palette_class == g["palette_class"].get<std::string>());
   }
   g_worst_extract = w.rel;
   std::printf("[extract] max |delta| vs Python = %.3e abs, %.3e rel (all 7 cases)\n", w.abs, w.rel);
}

TEST_CASE("extract is bit-identical at any thread count", "[extract]")
{
   for (const auto& c : extract_cases()) {
      INFO(c.name);
      const std::string ref = ac::dump_python(run_case(c, 1).to_json());
      for (unsigned t : {2u, 3u, 8u, 0u})
         REQUIRE(ac::dump_python(run_case(c, t).to_json()) == ref);
   }
}

TEST_CASE("extract rejects non-RGB and empty input", "[extract]")
{
   REQUIRE_THROWS_AS(ac::extract(ac::Image(8, 8, 1), 1.0, 3, 2.0, "RGB"), std::invalid_argument);
   REQUIRE_THROWS_AS(ac::extract(ac::Image(0, 0, 3), 1.0, 3, 2.0, "RGB"), std::invalid_argument);
}

TEST_CASE("json_round_trip_python_bytes", "[extract][json]")
{
   for (const auto& c : extract_cases()) {
      INFO(c.name);
      const std::string want = read_text(golden("extract/pydumps_" + c.name + ".txt"));
      const auto fp = run_case(c);
      // the C++ extraction reproduces Python's text bytes ...
      const std::string got = ac::dump_python(fp.to_json());
      // ... exactly when every double in it is bit-equal; where extraction differs by an ulp, the golden
      // record itself must still survive from_json -> to_json -> dump_python byte for byte.
      const ac::Json gold = in_to_dict_order(decode_fp(c.g["fingerprint"]));
      REQUIRE(ac::dump_python(ac::FingerprintData::from_json(gold).to_json()) == want);
      REQUIRE(ac::dump_python(gold) == want);
      if (got != want)
         std::printf("[json] %s: extraction text differs from Python (ulp-level), round trip of the golden is exact\n",
                     c.name.c_str());
   }
}

TEST_CASE("json_round_trip_python_bytes: structure_to_gradient_ratio inf is Infinity", "[extract][json]")
{
   const auto& cases = extract_cases();
   auto fp = ac::FingerprintData::from_json(in_to_dict_order(decode_fp(cases[0].g["fingerprint"])));
   fp.background.structure_to_gradient_ratio = std::numeric_limits<double>::infinity();
   REQUIRE(ac::dump_python(fp.to_json()) == read_text(golden("extract/pydumps_nebula_inf.txt")));
   REQUIRE(ac::dump_python(fp.to_json(), 2) == read_text(golden("extract/pydumps_nebula_inf_indent2.txt")));
   REQUIRE(ac::dump_python(fp.to_json()).find("\"structure_to_gradient_ratio\": Infinity") != std::string::npos);
}

TEST_CASE("fingerprint JSON: key order and from_json validation", "[json]")
{
   const auto c = extract_cases()[0];
   const auto fp = run_case(c);
   const ac::Json j = fp.to_json();
   std::vector<std::string> keys;
   for (auto it = j.begin(); it != j.end(); ++it)
      keys.push_back(it.key());
   REQUIRE(keys == std::vector<std::string>{"energy_spectrum", "tonal_quantiles", "saturation_quantiles",
                                            "chroma_hist", "background", "pixel_scale_arcsec", "psf_fwhm_arcsec",
                                            "palette_class"});
   REQUIRE(j["chroma_hist"].size() == 16);
   REQUIRE(j["chroma_hist"][0].size() == 16);
   const auto back = ac::FingerprintData::from_json(j);
   REQUIRE(ac::dump_python(back.to_json()) == ac::dump_python(j));

   ac::Json bad = j;
   bad.erase("palette_class");
   REQUIRE_THROWS_AS(ac::FingerprintData::from_json(bad), std::invalid_argument);
   bad = j;
   bad["tonal_quantiles"].push_back(1.0);
   REQUIRE_THROWS_AS(ac::FingerprintData::from_json(bad), std::invalid_argument);
   bad = j;
   bad["chroma_hist"][3].push_back(0.5);
   REQUIRE_THROWS_AS(ac::FingerprintData::from_json(bad), std::invalid_argument);
}
