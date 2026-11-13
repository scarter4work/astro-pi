// AutoContrast: beam pruning, the loop (advance / ingest / close / convergence / outcome) and the test-only
// ApproxExecutor vs the Python reference (optimize/{beam,loop}.py, executors/numpy_exec.py).
// Goldens: synthetic/loop (five loop runs) and synthetic/executor (NumpyExecutor outputs).
#include "loop_case.h"

#include "core/optimize/Beam.h"
#include "core/optimize/Executor.h"
#include "core/optimize/Recipe.h"

#include <cstdio>
#include <new>
#include <stdexcept>

using ac::test::json_compare;
using ac::test::loop_golden;
using ac::test::loop_run;
using ac::test::plain;

namespace {

ac::Branch branch(const std::vector<ac::Action>& actions, double d, bool alive = true, std::string id = "")
{
   ac::Branch b;
   b.recipe.actions = actions;
   b.distance = d;
   b.alive = alive;
   b.image_id = std::move(id);
   return b;
}

ac::Action act(const std::string& kind, const std::string& level = "gentle")
{
   ac::Action a;
   a.kind = kind;
   a.level = level;
   return a;
}

// A small session on the 24x32 loop fixture: proxy = the flattened image, reference = the original.
struct SmallFixture {
   ac::ImageStore store;
   ac::Session session;
   explicit SmallFixture(ac::BeamConfig cfg = {})
   {
      const ac::Image proxy = ac::test::load_image(loop_golden("loop/small_flat07_f8.npy"));
      const ac::Image ref = ac::test::load_image(loop_golden("loop/small_u8.npy"));
      const ac::FingerprintData ref_fp = ac::extract(ref, 1.0, 6, 2.0, "RGB");
      store.put(std::make_shared<const ac::Image>(proxy), "/t/proxy.png");
      session = ac::begin("/t/source.png", "/t/proxy.png", "ref", ref_fp, "/t/work", 1.0, 2.0, "RGB", 6, "sid",
                          store, cfg);
   }
};

class IdentityExecutor final : public ac::Executor {
public:
   ac::Image apply(const ac::Image& parent, const ac::Action&, double) override { return parent; }
};

class ZeroExecutor final : public ac::Executor {
public:
   ac::Image apply(const ac::Image& parent, const ac::Action&, double) override
   {
      ac::Image z = parent;
      for (double& v : z.data)
         v = 0.0;
      return z;
   }
};

// Throws on the first two calls (two exception types), then behaves as ApproxExecutor.
class FlakyExecutor final : public ac::Executor {
public:
   int calls = 0;
   ac::ApproxExecutor inner;
   ac::Image apply(const ac::Image& parent, const ac::Action& a, double px) override
   {
      ++calls;
      if (calls == 1)
         throw std::invalid_argument("boom");
      if (calls == 2)
         throw std::runtime_error("disk on fire");
      return inner.apply(parent, a, px);
   }
};

// Every quantity the brief names for a loop golden: per-iteration branches / best / history / new log entries /
// convergence, the ingest trace, the menus, the stored candidates, the whole log and the outcome.
void check_loop_case(const std::string& name)
{
   const ac::test::LoopRun& run = loop_run(name);
   const nlohmann::json& g = run.golden;
   double max_dd = 0.0;
   std::vector<std::string> diffs;

   json_compare(g.at("iterations"), plain(run.iterations), 1e-9, name + ".iterations", diffs, max_dd);
   json_compare(g.at("attempts"), plain(run.attempts), 1e-9, name + ".attempts", diffs, max_dd);
   json_compare(g.at("menus"), plain(run.menus), 0.0, name + ".menus", diffs, max_dd);
   json_compare(g.at("guardrail_log"), plain(run.session.guardrail_log), 0.0, name + ".guardrail_log", diffs,
                max_dd);
   json_compare(g.at("distance_history"), nlohmann::json(run.session.distance_history), 1e-9,
                name + ".distance_history", diffs, max_dd);
   json_compare(g.at("outcome"), plain(ac::outcome(run.session)), 1e-9, name + ".outcome", diffs, max_dd);
   json_compare(g.at("saved_paths"), nlohmann::json(run.saved), 0.0, name + ".saved_paths", diffs, max_dd);
   const double dbase = std::fabs(g.at("baseline_distance").get<double>() - run.session.baseline_distance);
   max_dd = std::max(max_dd, dbase);

   for (const std::string& d : diffs)
      UNSCOPED_INFO(d);
   CHECK(diffs.empty());
   CHECK(dbase <= 1e-9);
   CHECK(run.attempts.size() == g.at("n_attempts").get<size_t>());
   CHECK(run.store_consistent);
   std::printf("[parity] %s: iterations %d, attempts %zu, log entries %zu, reason \"%s\", improved %s, "
               "max |dD| %.3g, peak store %zu\n",
               name.c_str(), run.session.iteration, run.attempts.size(), run.session.guardrail_log.size(),
               run.session.convergence_reason.c_str(), ac::outcome(run.session)["improved"].get<bool>() ? "yes" : "no",
               max_dd, run.max_store);
}

} // namespace

TEST_CASE("prune_distinct_by_recipe", "[beam]")
{
   const ac::Action a = act("chroma"), b = act("core_hdr"), c = act("black_point");
   std::vector<ac::Branch> in = {branch({a}, 0.30, true, "x0"), branch({b}, 0.10, true, "x1"),
                                 branch({a}, 0.20, true, "x2"), branch({c}, 0.10, true, "x3"),
                                 branch({b, a}, 0.25, true, "x4")};
   const auto out = ac::prune(in, 3);
   REQUIRE(out.size() == 3);
   // Stable sort: the 0.10 tie keeps input order (x1 before x3); the second "chroma" (0.30) is deduplicated
   // behind its better twin x2.
   CHECK(out[0].image_id == "x1");
   CHECK(out[1].image_id == "x3");
   CHECK(out[2].image_id == "x2");
   const auto all = ac::prune(in, 10);
   REQUIRE(all.size() == 4);   // 5 branches, 4 distinct recipes
   CHECK(all[3].image_id == "x4");
   CHECK(ac::prune({}, 3).empty());
}

TEST_CASE("prune_ignores_dead", "[beam]")
{
   std::vector<ac::Branch> in = {branch({act("chroma")}, 0.05, false, "dead"), branch({act("chroma")}, 0.2, true, "a"),
                                 branch({act("core_hdr")}, 0.1, true, "b")};
   const auto out = ac::prune(in, 3);
   REQUIRE(out.size() == 2);
   CHECK(out[0].image_id == "b");
   CHECK(out[1].image_id == "a");   // the dead twin with the better score neither holds a slot nor blocks its key

   in.push_back(branch({act("tonal_reshape")}, std::nan(""), true, "nan"));
   CHECK_THROWS_AS(ac::prune(in, 3), std::invalid_argument);
}

TEST_CASE("prune_noise_level_ties_keep_input_order", "[beam]")
{
   // Golden (d), iteration 2: Python's two commuting permutations tie exactly (0.17260786760310548) and its stable
   // sort keeps cand-2-0 first; the C++ values differ by 8e-17. The noise must not reorder them.
   const ac::Action le = act("local_equalize"), ch = act("chroma");
   std::vector<ac::Branch> in = {branch({le, ch}, 0.17260786760310556, true, "cand-2-0"),
                                 branch({ch, le}, 0.17260786760310548, true, "cand-2-11")};
   auto out = ac::prune(in, 3);
   REQUIRE(out.size() == 2);
   CHECK(out[0].image_id == "cand-2-0");
   CHECK(out[1].image_id == "cand-2-11");

   // A real difference (1e-9, three orders above the tolerance) still orders by distance.
   in = {branch({le}, 0.2, true, "a"), branch({ch}, 0.2 - 1e-9, true, "b")};
   out = ac::prune(in, 3);
   CHECK(out[0].image_id == "b");

   // A run chains through neighbours within the tolerance; a run is ordered by input position; runs stay ordered.
   in = {branch({act("core_hdr")}, 0.3, true, "late"), branch({le}, 0.1 + 2e-13, true, "x"),
         branch({ch}, 0.1 + 1e-13, true, "y"), branch({act("black_point")}, 0.1, true, "z")};
   out = ac::prune(in, 4);
   REQUIRE(out.size() == 4);
   CHECK(out[0].image_id == "x");
   CHECK(out[1].image_id == "y");
   CHECK(out[2].image_id == "z");
   CHECK(out[3].image_id == "late");
}

TEST_CASE("beam_config_defaults_match_python", "[beam]")
{
   const ac::BeamConfig c;
   CHECK(c.width == 3);
   CHECK(c.top_k == 3);
   CHECK(c.iteration_cap == 20);
   CHECK(c.convergence_window == 3);
   CHECK(c.epsilon == 1e-3);
   CHECK(c.epsilon_improve == 1e-3);
   CHECK(c.resolved_max_attempts() == 9);
   ac::BeamConfig k7;
   k7.top_k = 7;
   CHECK(k7.resolved_max_attempts() == 21);
   k7.max_attempts = 4;
   CHECK(k7.resolved_max_attempts() == 4);
}

TEST_CASE("convergence_iteration_cap", "[loop]")
{
   ac::Session s;
   s.distance_history = {0.5, 0.4};
   s.iteration = 19;
   ac::check_convergence(s);
   CHECK_FALSE(s.converged);
   s.iteration = 20;
   ac::check_convergence(s);
   CHECK(s.converged);
   CHECK(s.convergence_reason == "iteration cap (20) reached");
}

TEST_CASE("convergence_window_epsilon", "[loop]")
{
   ac::Session s;
   s.iteration = 3;
   s.distance_history = {0.3995, 0.3992, 0.3991};   // 3 entries: a window of 3 needs 4 (len(window) > 3)
   ac::check_convergence(s);
   CHECK_FALSE(s.converged);

   s.iteration = 4;
   s.distance_history = {0.5, 0.4, 0.3995, 0.3992, 0.3991};   // |0.4 - 0.3991| = 9e-4 < 1e-3
   ac::check_convergence(s);
   CHECK(s.converged);
   CHECK(s.convergence_reason == "deltaD below 0.001 across the last 3 iterations");

   ac::Session t;
   t.iteration = 4;
   t.config.epsilon = 5e-3;
   t.config.convergence_window = 2;
   t.distance_history = {0.5, 0.40, 0.39, 0.38};             // |0.40 - 0.38| = 0.02: still moving
   ac::check_convergence(t);
   CHECK_FALSE(t.converged);
   t.distance_history.push_back(0.3790);                      // |0.39 - 0.379| = 0.011
   t.distance_history.push_back(0.3770);                      // |0.38 - 0.377| = 0.003 < 0.005
   ac::check_convergence(t);
   CHECK(t.converged);
   CHECK(t.convergence_reason == "deltaD below 0.005 across the last 2 iterations");
}

TEST_CASE("no_op_candidate_logged_not_kept", "[loop]")
{
   SmallFixture f;
   IdentityExecutor ex;
   int callbacks = 0;
   ac::advance(f.session, ex, f.store, {}, [&](const ac::Session&) { ++callbacks; });
   CHECK(callbacks == 1);
   const ac::Json& log = f.session.guardrail_log;
   REQUIRE(log.size() == 10);   // 9 no-op discards, then the attempt cap note
   for (size_t i = 0; i < 9; ++i) {
      CHECK(log[i]["failed"] == ac::Json::array({"no_op"}));
      CHECK(log[i]["reason"] == "candidate was pixel-identical to its parent; it explored nothing and must not "
                                "hold a beam slot");
      CHECK(log[i]["iteration"] == 1);
   }
   CHECK(log[9]["branch"] == "(root)");
   CHECK(log[9]["noted"] == ac::Json::array({"attempt_cap"}));
   CHECK(f.session.converged);
   CHECK(f.session.convergence_reason == "every branch was discarded by guardrails or executor failure");
   CHECK(f.session.branches.empty());
   CHECK(f.session.distance_history.size() == 1);   // nothing closed into the history
   CHECK(f.store.size() == 1);                      // only the proxy: no candidate was stored
   const ac::Json o = ac::outcome(f.session);
   CHECK(o["improved"] == false);
   CHECK(o["result_path"] == "/t/proxy.png");
   CHECK(o["recipe"] == ac::Json{{"actions", ac::Json::array()}});
   // advance on a converged session is a no-op
   ac::advance(f.session, ex, f.store, {}, [&](const ac::Session&) { ++callbacks; });
   CHECK(callbacks == 1);
}

TEST_CASE("executor_exception_logged_run_continues", "[loop]")
{
   CHECK(std::string(ac::python_exception_name(std::invalid_argument("x"))) == "ValueError");
   CHECK(std::string(ac::python_exception_name(std::domain_error("x"))) == "ValueError");
   CHECK(std::string(ac::python_exception_name(std::length_error("x"))) == "ValueError");
   CHECK(std::string(ac::python_exception_name(std::out_of_range("x"))) == "IndexError");
   CHECK(std::string(ac::python_exception_name(std::bad_alloc())) == "MemoryError");
   CHECK(std::string(ac::python_exception_name(std::overflow_error("x"))) == "OverflowError");
   CHECK(std::string(ac::python_exception_name(std::runtime_error("x"))) == "RuntimeError");
   CHECK(std::string(ac::python_exception_name(std::logic_error("x"))) == "RuntimeError");

   SmallFixture f;
   FlakyExecutor ex;
   ac::advance(f.session, ex, f.store);
   const ac::Json& log = f.session.guardrail_log;
   REQUIRE(log.size() >= 2);
   CHECK(log[0]["failed"] == ac::Json::array({"executor"}));
   CHECK(log[0]["reason"] == "ValueError: boom");
   CHECK(log[1]["failed"] == ac::Json::array({"executor"}));
   CHECK(log[1]["reason"] == "RuntimeError: disk on fire");
   CHECK(ex.calls > 2);                          // the branch went on to the next actions
   CHECK(f.session.iteration == 1);
   CHECK_FALSE(f.session.branches.empty());      // and kept live candidates
   CHECK(f.session.branches[0].image_id.rfind("/t/work/cand-1-", 0) == 0);
}

TEST_CASE("attempt_cap_noted", "[loop]")
{
   ac::BeamConfig cfg;
   cfg.max_attempts = 4;
   SmallFixture f(cfg);
   ZeroExecutor ex;   // every candidate crushes the shadows: shadow_clipping discards it
   ac::advance(f.session, ex, f.store);
   const ac::Json& log = f.session.guardrail_log;
   REQUIRE(log.size() >= 5);
   size_t n_failed = 0;
   for (size_t i = 0; i + 1 < log.size(); ++i)
      if (log[i].contains("failed")) {
         ++n_failed;
         CHECK(log[i]["action"].is_string());   // per-candidate discards are keyed by action
      }
   CHECK(n_failed == 4);                        // the cap bound after exactly max_attempts discards
   CHECK(f.session.branches.empty());
   CHECK(log.back() == ac::Json{{"iteration", 1},
                            {"branch", "(root)"},
                            {"noted", {"attempt_cap"}},
                            {"reason", "branch stopped after 4 attempts (cap 4) holding only 0 of 3 live candidates; "
                                       "guardrails or the executor rejected nearly everything tried on this image"}});
   CHECK(ac::dump_python(log.back()) == "{\"iteration\": 1, \"branch\": \"(root)\", \"noted\": [\"attempt_cap\"], "
                                    "\"reason\": \"branch stopped after 4 attempts (cap 4) holding only 0 of 3 live "
                                    "candidates; guardrails or the executor rejected nearly everything tried on this "
                                    "image\"}");
}

TEST_CASE("approx_executor_matches_numpy", "[executor]")
{
   const nlohmann::json cases = ac::test::load_json(loop_golden("executor/cases.json"));
   const ac::test::NpyArray outs = ac::test::load_npy(loop_golden("executor/outputs_f8.npy"));
   const std::map<std::string, ac::Image> inputs = {
      {"u8", ac::test::load_image(loop_golden("executor/in_u8.npy"))},
      {"flat", ac::test::load_image(loop_golden("executor/in_flat_f8.npy"))}};
   REQUIRE(outs.shape.size() == 4);
   const size_t plane = outs.shape[1] * outs.shape[2] * outs.shape[3];

   ac::ApproxExecutor ex1(1), exN(0);
   double max_abs = 0.0;
   size_t n_exact = 0, n_out = 0, n_err = 0;
   for (const auto& c : cases.at("cases")) {
      const ac::Action a = ac::action_from_json(ac::Json::parse(c.at("action").dump()));
      const ac::Image& in = inputs.at(c.at("input").get<std::string>());
      const double px = c.at("pixel_scale_arcsec").get<double>();
      INFO(c.at("input").get<std::string>() << " " << a.key() << " px " << px);
      if (!c.at("error").is_null()) {
         ++n_err;
         try {
            (void)ex1.apply(in, a, px);
            FAIL("expected a refusal");
         } catch (const std::exception& e) {
            CHECK(ac::describe_executor_failure(e) == c.at("error").get<std::string>());
         }
         continue;
      }
      ++n_out;
      const ac::Image got = ex1.apply(in, a, px);
      const ac::Image got_n = exN.apply(in, a, px);
      CHECK(got.data == got_n.data);   // thread-invariant
      REQUIRE(got.data.size() == plane);
      const size_t off = c.at("index").get<size_t>() * plane;
      double case_max = 0.0;
      for (size_t i = 0; i < plane; ++i)
         case_max = std::max(case_max, std::fabs(got.data[i] - outs.data[off + i]));
      CHECK(case_max <= 1e-12);
      max_abs = std::max(max_abs, case_max);
      if (case_max == 0.0)
         ++n_exact;
      else
         std::printf("[parity]   not bit-exact: %s %s px %g max |d| %.3g\n", c.at("input").get<std::string>().c_str(),
                     a.key().c_str(), px, case_max);
   }
   CHECK(n_out == outs.shape[0]);
   std::printf("[parity] approx executor: %zu outputs (%zu bit-exact), %zu refusals, max |d| %.3g\n", n_out, n_exact,
               n_err, max_abs);
}

TEST_CASE("approx_executor_rejects_unsupportable_layer", "[executor]")
{
   CHECK(ac::max_supportable_layer(24, 32) == 4);
   CHECK(ac::max_supportable_layer(32, 48) == 5);
   CHECK(ac::max_supportable_layer(1600, 1067) == 10);
   CHECK(ac::max_supportable_layer(1, 1) == 0);
   const ac::Image im = ac::test::load_image(loop_golden("loop/small_u8.npy"));   // 24 x 32
   ac::Action a = act("local_contrast");
   a.scale_arcsec = 32.0;
   a.params = {{"layer", 5}};
   ac::ApproxExecutor ex;
   try {
      (void)ex.apply(im, a, 1.0);
      FAIL("layer 5 on 24x32 must be refused");
   } catch (const std::invalid_argument& e) {
      // The exact text case (d)'s Python log carries.
      CHECK(ac::describe_executor_failure(e) ==
            "ValueError: local_contrast layer 5 exceeds what a 24x32 image supports (max layer 4); rejecting rather "
            "than hanging on an a trous kernel far larger than the image");
   }
   a.params = {{"layer", 4}};
   CHECK_NOTHROW(ex.apply(im, a, 1.0));
   CHECK_THROWS_WITH(ex.apply(im, act("bogus"), 1.0), "unknown action kind 'bogus'");
}

TEST_CASE("loop_declines_at_floor_matches_python", "[loop][parity]")
{
   check_loop_case("a_decline");
   const ac::Json o = ac::outcome(loop_run("a_decline").session);
   CHECK(o["improved"] == false);
   CHECK(o["result_path"] == "/ac/a_decline/proxy.png");
}

TEST_CASE("loop_improves_flattened_matches_python", "[loop][parity]")
{
   check_loop_case("b_flattened");
   const ac::Json o = ac::outcome(loop_run("b_flattened").session);
   CHECK(o["improved"] == true);
   CHECK(loop_run("b_flattened").session.iteration == 9);
}

TEST_CASE("loop_guardrail_hostile_exhausts_matches_python", "[loop][parity]")
{
   check_loop_case("c_hostile");
   const auto& s = loop_run("c_hostile").session;
   CHECK(s.branches.empty());
   CHECK(s.convergence_reason == "every branch was discarded by guardrails or executor failure");
}

TEST_CASE("loop_executor_failures_logged_matches_python", "[loop][parity]")
{
   check_loop_case("d_small_executor_failure");
   size_t n_exec = 0;
   for (const auto& e : loop_run("d_small_executor_failure").session.guardrail_log)
      if (e.contains("failed") && e["failed"] == ac::Json::array({"executor"}))
         ++n_exec;
   CHECK(n_exec > 0);
}

TEST_CASE("loop_config_iteration_cap_matches_python", "[loop][parity]")
{
   check_loop_case("e_config");
   CHECK(loop_run("e_config").session.convergence_reason == "iteration cap (4) reached");
}
