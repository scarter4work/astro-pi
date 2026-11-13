// AutoContrast: session.json in Python's schema (optimize/session.py Session.save, json.dumps indent=2) vs the
// golden session files Python wrote at the end of each loop golden (synthetic/loop/<case>_session.json).
#include "loop_case.h"

#include "core/optimize/Session.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

using ac::test::json_compare;
using ac::test::loop_golden;
using ac::test::loop_run;

namespace {

std::string read_text(const std::string& path)
{
   std::ifstream f(path, std::ios::binary);
   REQUIRE(f.good());
   std::ostringstream ss;
   ss << f.rdbuf();
   return ss.str();
}

// Byte-equal where every number came through identical arithmetic; otherwise the parsed documents must agree with
// the same keys in the same order at every level, numbers within 1e-9 and everything else exact.
void check_session(const std::string& name)
{
   const ac::Session& s = loop_run(name).session;
   // The trailing "\n" is the golden writer's (make_goldens.py writes session_text + "\n"), not part of the
   // contract: Session.save writes json.dumps(payload, indent=2) with no newline, and so must a session.json writer.
   const std::string text = ac::dump_python(s.to_json(), 2) + "\n";
   const std::string want = read_text(loop_golden("loop/" + name + "_session.json"));
   const bool byte_equal = text == want;
   if (const char* dir = std::getenv("AC_SESSION_DUMP_DIR")) {   // inspection aid: the C++ text, for a diff
      std::ofstream(std::string(dir) + "/" + name + "_session.json", std::ios::binary) << text;
   }

   std::vector<std::string> diffs;
   double max_abs = 0.0;
   json_compare(ac::Json::parse(want), ac::Json::parse(text), 1e-9, name, diffs, max_abs, /*ordered=*/true);
   for (const std::string& d : diffs)
      UNSCOPED_INFO(d);
   CHECK(diffs.empty());
   std::printf("[parity] %s session.json: %s (%zu vs %zu bytes), max |d| %.3g\n", name.c_str(),
               byte_equal ? "byte-equal" : "parsed-equal", text.size(), want.size(), max_abs);
}

} // namespace

TEST_CASE("session_json_schema_matches_python", "[session][parity]")
{
   for (const char* name : {"a_decline", "b_flattened", "c_hostile", "d_small_executor_failure", "e_config"})
      check_session(name);
}

TEST_CASE("session_json_batched_fields_are_python_defaults", "[session]")
{
   ac::Session s;
   const ac::Json j = s.to_json();
   std::vector<std::string> keys;
   for (auto it = j.begin(); it != j.end(); ++it)
      keys.push_back(it.key());
   const std::vector<std::string> python_order = {
      "session_id", "work_dir", "source_path", "proxy_path", "reference_id", "reference_fp", "pixel_scale_arcsec",
      "psf_fwhm_arcsec", "palette_class", "n_scales", "config", "iteration", "branches", "best",
      "baseline_distance", "distance_history", "converged", "convergence_reason", "guardrail_log", "pending",
      "cursors", "candidates", "max_dim", "candidate_suffix"};
   CHECK(keys == python_order);
   CHECK(j["pending"] == ac::Json::array());
   CHECK(j["cursors"] == ac::Json::array());
   CHECK(j["candidates"] == ac::Json::array());
   CHECK(j["max_dim"].is_null());
   CHECK(j["candidate_suffix"] == ".png");
   CHECK(ac::dump_python(j["config"]) == "{\"width\": 3, \"top_k\": 3, \"iteration_cap\": 20, "
                                         "\"convergence_window\": 3, \"epsilon\": 0.001, \"epsilon_improve\": 0.001, "
                                         "\"max_attempts\": 9}");
   // An unmeasured branch writes fingerprint null, as _branch_to_dict does for None.
   CHECK(ac::dump_python(ac::branch_to_json(ac::Branch{})) ==
         "{\"recipe\": {\"actions\": []}, \"image_path\": \"\", \"distance\": 0.0, \"alive\": true, "
         "\"fingerprint\": null}");
}
