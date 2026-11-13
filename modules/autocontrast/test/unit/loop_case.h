// AutoContrast tests: run one Python loop golden (synthetic/loop/<case>.json, written by make_goldens.py
// _run_loop_case) through the C++ loop with ApproxExecutor, recording the same per-iteration snapshots, ingest
// trace and menus Python recorded. Shared by test_loop.cpp and test_session.cpp.
#pragma once

#include <catch_amalgamated.hpp>

#include "Npy.h"
#include "core/Json.h"
#include "core/fingerprint/Extract.h"
#include "core/optimize/ApproxExecutor.h"
#include "core/optimize/Loop.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ac::test {

inline std::string loop_golden(const std::string& rel)
{
   return std::string(AC_TEST_DIR) + "/golden/synthetic/" + rel;
}

// An .npy golden image; uint8 arrays are 8-bit renders: float = u8 / 255.0 (MANIFEST image_convention).
inline ac::Image load_image(const std::string& path)
{
   const NpyArray a = load_npy(path);
   REQUIRE(a.shape.size() == 3);
   ac::Image im(static_cast<int>(a.shape[1]), static_cast<int>(a.shape[0]), static_cast<int>(a.shape[2]));
   REQUIRE(im.data.size() == a.data.size());
   const bool u8 = a.dtype == "|u1";
   for (size_t i = 0; i < a.data.size(); ++i)
      im.data[i] = u8 ? a.data[i] / 255.0 : a.data[i];
   return im;
}

// Golden JSON encodes non-finite floats as the strings "Infinity", "-Infinity", "NaN" (MANIFEST nonfinite_encoding).
inline bool golden_nonfinite(const nlohmann::json& g, double& out)
{
   if (!g.is_string())
      return false;
   const std::string s = g.get<std::string>();
   if (s == "Infinity") out = std::numeric_limits<double>::infinity();
   else if (s == "-Infinity") out = -std::numeric_limits<double>::infinity();
   else if (s == "NaN") out = std::numeric_limits<double>::quiet_NaN();
   else return false;
   return true;
}

// Structural comparison: objects by key (and, with `ordered`, in the same key order), arrays elementwise, numbers
// within tol * max(1, |golden|) — absolute for distances (< 1), relative for large fingerprint energies — integers
// exactly, everything else exact. Appends a readable line per mismatch to `diffs`
// (capped) and tracks the largest numeric difference.
template <class JG, class JA>
void json_compare(const JG& g, const JA& a, double tol, const std::string& path, std::vector<std::string>& diffs,
                  double& max_abs, bool ordered = false)
{
   auto fail = [&](const std::string& why) {
      if (diffs.size() < 25)
         diffs.push_back(path + ": " + why);
   };
   double gv = 0.0;
   if (a.is_number() && (g.is_number() || golden_nonfinite(g, gv))) {
      if (g.is_number())
         gv = g.template get<double>();
      const double av = a.template get<double>();
      if (g.is_number_integer() != a.is_number_integer()) {
         fail("integer/float kind differs: golden " + g.dump() + " actual " + a.dump());
         return;
      }
      if (std::isnan(gv) || std::isnan(av)) {
         if (!(std::isnan(gv) && std::isnan(av)))
            fail("NaN mismatch: golden " + g.dump() + " actual " + a.dump());
         return;
      }
      if (std::isinf(gv) || std::isinf(av)) {
         if (gv != av)
            fail("inf mismatch");
         return;
      }
      const double d = std::fabs(gv - av);
      if (d > max_abs)
         max_abs = d;
      if (d > tol * std::max(1.0, std::fabs(gv)))
         fail("number " + g.dump() + " vs " + a.dump());
      return;
   }
   if (g.type() != a.type() && !(g.is_number() && a.is_number())) {
      fail("type differs: golden " + g.dump() + " actual " + a.dump());
      return;
   }
   if (g.is_object()) {
      if (g.size() != a.size())
         fail("object size " + std::to_string(g.size()) + " vs " + std::to_string(a.size()));
      if (ordered) {
         auto gi = g.begin();
         auto ai = a.begin();
         for (; gi != g.end() && ai != a.end(); ++gi, ++ai) {
            if (gi.key() != ai.key()) {
               fail("key order: golden '" + gi.key() + "' actual '" + ai.key() + "'");
               return;
            }
            json_compare(gi.value(), ai.value(), tol, path + "." + gi.key(), diffs, max_abs, ordered);
         }
      } else {
         for (auto it = g.begin(); it != g.end(); ++it) {
            if (!a.contains(it.key())) {
               fail("missing key '" + it.key() + "'");
               continue;
            }
            json_compare(it.value(), a.at(it.key()), tol, path + "." + it.key(), diffs, max_abs, ordered);
         }
         for (auto it = a.begin(); it != a.end(); ++it)
            if (!g.contains(it.key()))
               fail("extra key '" + it.key() + "'");
      }
      return;
   }
   if (g.is_array()) {
      if (g.size() != a.size()) {
         fail("array size " + std::to_string(g.size()) + " vs " + std::to_string(a.size()));
         return;
      }
      for (size_t i = 0; i < g.size(); ++i)
         json_compare(g[i], a[i], tol, path + "[" + std::to_string(i) + "]", diffs, max_abs, ordered);
      return;
   }
   if (g != a)
      fail("value " + g.dump() + " vs " + a.dump());
}

// The C++ JSON as nlohmann::json (map-ordered), for comparison with goldens written with sort_keys.
inline nlohmann::json plain(const ac::Json& j) { return nlohmann::json::parse(j.dump()); }

inline ac::Json action_record(const ac::Action& a)
{
   ac::Json d = ac::action_to_json(a);
   d["key"] = a.key();
   return d;
}

struct LoopRun {
   nlohmann::json golden;
   ac::Session session;
   ac::Json iterations = ac::Json::array(), attempts = ac::Json::array(), menus = ac::Json::array();
   std::vector<std::string> saved;   // kept candidate ids, in the order the loop stored them
   bool store_consistent = true;     // after every iteration the store held exactly the live ids
   size_t max_store = 0;
};

inline ac::BeamConfig config_from(const nlohmann::json& c)
{
   ac::BeamConfig cfg;
   cfg.width = c.at("width").get<int>();
   cfg.top_k = c.at("top_k").get<int>();
   cfg.iteration_cap = c.at("iteration_cap").get<int>();
   cfg.convergence_window = c.at("convergence_window").get<int>();
   cfg.epsilon = c.at("epsilon").get<double>();
   cfg.epsilon_improve = c.at("epsilon_improve").get<double>();
   cfg.max_attempts = c.at("max_attempts").get<int>();
   return cfg;
}

inline ac::GuardrailLimits limits_from(const nlohmann::json& l)
{
   ac::GuardrailLimits L;
   L.noise_growth = l.at("noise_growth").get<double>();
   L.shadow_clip_fraction = l.at("shadow_clip_fraction").get<double>();
   L.highlight_clip_fraction = l.at("highlight_clip_fraction").get<double>();
   L.star_count_drop = l.at("star_count_drop").get<double>();
   L.star_fwhm_growth = l.at("star_fwhm_growth").get<double>();
   L.star_ecc_growth = l.at("star_ecc_growth").get<double>();
   L.hue_invention_mass = l.at("hue_invention_mass").get<double>();
   L.hue_tolerance_deg = l.at("hue_tolerance_deg").get<double>();
   L.channel_ratio_drift = l.at("channel_ratio_drift").get<double>();
   REQUIRE(l.size() == 9);
   return L;
}

// Runs (and caches, per process) the named golden case.
inline const LoopRun& loop_run(const std::string& name)
{
   static std::map<std::string, std::unique_ptr<LoopRun>> cache;
   auto it = cache.find(name);
   if (it != cache.end())
      return *it->second;

   auto run = std::make_unique<LoopRun>();
   run->golden = load_json(loop_golden("loop/" + name + ".json"));
   const nlohmann::json& g = run->golden;
   const nlohmann::json& kw = g.at("params");
   const double px = kw.at("pixel_scale_arcsec").get<double>();
   const double psf = kw.at("psf_fwhm_arcsec").get<double>();
   const int n_scales = kw.at("n_scales").get<int>();
   const std::string palette = kw.at("palette_class").get<std::string>();
   const std::string proxy_id = g.at("paths").at("proxy").get<std::string>();
   const std::string source_id = g.at("paths").at("source").get<std::string>();
   const std::string work_dir = g.at("paths").at("work_dir").get<std::string>();

   const ac::Image proxy = load_image(loop_golden(g.at("inputs").at("proxy").get<std::string>()));
   const ac::Image ref = load_image(loop_golden(g.at("inputs").at("reference").get<std::string>()));
   const ac::FingerprintData ref_fp = ac::extract(ref, px, n_scales, psf, palette);

   ac::ImageStore store;
   store.put(std::make_shared<const ac::Image>(proxy), proxy_id);

   const ac::BeamConfig cfg = config_from(g.at("config"));
   const ac::GuardrailLimits limits = limits_from(g.at("limits"));
   run->session = ac::begin(source_id, proxy_id, "golden-" + name, ref_fp, work_dir, px, psf, palette, n_scales,
                            "golden-" + name, store, cfg);

   LoopRun* r = run.get();
   ac::LoopTrace trace;
   trace.on_menu = [r](int iteration, const ac::Branch& parent, const std::vector<ac::Action>& menu) {
      ac::Json m = ac::Json::object();
      m["iteration"] = iteration;
      ac::Json kinds = ac::Json::array();
      for (const std::string& k : parent.recipe.applied_kinds())   // std::set: sorted, as the golden's enc()
         kinds.push_back(k);
      m["applied_kinds"] = kinds;
      ac::Json keys = ac::Json::array();
      for (const ac::Action& a : menu)
         keys.push_back(a.key());
      m["keys"] = keys;
      r->menus.push_back(m);
   };
   trace.on_ingest = [r](const ac::LoopTrace::Attempt& a) {
      ac::Json j = ac::Json::object();
      j["iteration"] = a.iteration;
      j["parent"] = a.parent_recipe_key;
      j["action"] = a.action_key;
      j["candidate_path"] = a.candidate_id;
      j["kept"] = a.kept.has_value();
      j["distance"] = a.kept ? ac::Json(a.kept->distance) : ac::Json(nullptr);
      j["recipe"] = a.kept ? ac::Json(a.kept->recipe.key()) : ac::Json(nullptr);
      j["log_entries"] = a.log_entries;
      r->attempts.push_back(j);
      if (a.kept)
         r->saved.push_back(a.candidate_id);
   };
   size_t log_before = 0;
   const ac::ImageStore* store_ptr = &store;
   auto on_iteration = [r, &log_before, store_ptr](const ac::Session& s) {
      ac::Json it = ac::Json::object();
      it["iteration"] = s.iteration;
      ac::Json branches = ac::Json::array();
      for (const ac::Branch& b : s.branches) {
         ac::Json bj = ac::Json::object();
         bj["recipe"] = b.recipe.key();
         ac::Json acts = ac::Json::array();
         for (const ac::Action& a : b.recipe.actions)
            acts.push_back(action_record(a));
         bj["actions"] = acts;
         bj["image_path"] = b.image_id;
         bj["distance"] = b.distance;
         bj["alive"] = b.alive;
         branches.push_back(bj);
      }
      it["branches"] = branches;
      it["best"] = {{"recipe", s.best.recipe.key()}, {"distance", s.best.distance}, {"image_path", s.best.image_id}};
      it["distance_history"] = s.distance_history;
      ac::Json fresh = ac::Json::array();
      for (size_t i = log_before; i < s.guardrail_log.size(); ++i)
         fresh.push_back(s.guardrail_log[i]);
      log_before = s.guardrail_log.size();
      it["guardrail_log_new"] = fresh;
      it["converged"] = s.converged;
      it["convergence_reason"] = s.convergence_reason;
      r->iterations.push_back(it);

      const std::set<std::string> live = ac::live_image_ids(s);
      if (store_ptr->size() != live.size())
         r->store_consistent = false;
      for (const std::string& id : live)
         if (!store_ptr->contains(id))
            r->store_consistent = false;
      if (store_ptr->size() > r->max_store)
         r->max_store = store_ptr->size();
   };

   ac::ApproxExecutor executor;
   ac::run_to_convergence(run->session, executor, store, limits, on_iteration, &trace);
   return *cache.emplace(name, std::move(run)).first->second;
}

} // namespace ac::test
