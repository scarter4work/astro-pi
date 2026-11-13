#include "core/optimize/Loop.h"

#include "core/fingerprint/Distance.h"
#include "core/optimize/Propose.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <utility>

namespace ac {

namespace {

FingerprintData measure(const Image& rgb, const Session& s)
{
   return extract(rgb, s.pixel_scale_arcsec, s.n_scales, s.psf_fwhm_arcsec, s.palette_class);
}

// _Walk (loop.py:155-169): one branch's progress through its ranked menu.
struct Walk {
   int kept = 0, attempts = 0;
   bool may_issue(const BeamConfig& c) const { return kept < c.top_k && attempts < c.resolved_max_attempts(); }
};

// loop.py:172-178 — a dead candidate, not a dead run (§12).
void log_executor_failure(Session& s, int iteration, const std::string& action_key, const std::string& reason)
{
   Json e = Json::object();
   e["iteration"] = iteration;
   e["action"] = action_key;
   e["failed"] = Json::array({"executor"});
   e["reason"] = reason;
   s.guardrail_log.push_back(std::move(e));
}

// loop.py:181-204 — the attempt cap bound before the branch filled its slots. `noted`, not `failed`: nothing was
// discarded here, the branch just stopped looking. Keyed by branch, not action.
void log_attempt_cap(Session& s, int iteration, const Branch& branch, const Walk& walk)
{
   const BeamConfig& c = s.config;
   if (walk.kept >= c.top_k || walk.attempts < c.resolved_max_attempts())
      return;
   const std::string key = branch.recipe.key();
   Json e = Json::object();
   e["iteration"] = iteration;
   e["branch"] = key.empty() ? std::string("(root)") : key;
   e["noted"] = Json::array({"attempt_cap"});
   e["reason"] = "branch stopped after " + std::to_string(walk.attempts) + " attempts (cap " +
                 std::to_string(c.resolved_max_attempts()) + ") holding only " + std::to_string(walk.kept) + " of " +
                 std::to_string(c.top_k) +
                 " live candidates; guardrails or the executor rejected nearly everything tried on this image";
   s.guardrail_log.push_back(std::move(e));
}

// np.array_equal: same shape and every element ==, so a NaN anywhere makes them unequal.
bool pixel_identical(const Image& a, const Image& b)
{
   if (a.width != b.width || a.height != b.height || a.channels != b.channels)
      return false;
   for (size_t i = 0; i < a.data.size(); ++i)
      if (!(a.data[i] == b.data[i]))
         return false;
   return true;
}

std::string join_reasons(const std::vector<const GuardrailVerdict*>& vs)
{
   std::string out;
   for (size_t i = 0; i < vs.size(); ++i)
      out += (i ? "; " : "") + vs[i]->reason;
   return out;
}

Json names_of(const std::vector<const GuardrailVerdict*>& vs)
{
   Json a = Json::array();
   for (const GuardrailVerdict* v : vs)
      a.push_back(v->name);
   return a;
}

// ingest_candidate (loop.py:207-286): no-op check -> guardrails -> score. nullopt means the candidate is out.
// A kept candidate's pixels go into the store under candidate_id (Python's `save`), before it is fingerprinted.
std::optional<Branch> ingest_candidate(Session& s, int iteration, const Recipe& parent_recipe, const Image& parent,
                                       Image&& produced, const Image& source, const Action& action,
                                       const GuardrailLimits& limits, const std::string& candidate_id,
                                       ImageStore& store)
{
   const std::string akey = action.key();
   if (pixel_identical(produced, parent)) {
      // Identical pixels learned nothing but carry a distinct recipe key, so prune's dedup cannot see it; left in
      // it would tie its parent's (on a good image, the best) score and hold a beam slot. The test is the
      // property, never the action's name.
      Json e = Json::object();
      e["iteration"] = iteration;
      e["action"] = akey;
      e["failed"] = Json::array({"no_op"});
      e["reason"] = "candidate was pixel-identical to its parent; it explored nothing and must not hold a beam slot";
      s.guardrail_log.push_back(std::move(e));
      return std::nullopt;
   }

   const std::vector<GuardrailVerdict> verdicts = evaluate_guardrails(produced, parent, source, limits);

   // A guardrail may pass and still report it could not assess anything (star_integrity on a starless
   // baseline): `noted`, never `failed`, recorded whatever the candidate's fate.
   std::vector<const GuardrailVerdict*> noted, failed;
   for (const GuardrailVerdict& v : verdicts) {
      if (v.ok && !v.reason.empty())
         noted.push_back(&v);
      if (!v.ok)
         failed.push_back(&v);
   }
   if (!noted.empty()) {
      Json e = Json::object();
      e["iteration"] = iteration;
      e["action"] = akey;
      e["noted"] = names_of(noted);
      e["reason"] = join_reasons(noted);
      s.guardrail_log.push_back(std::move(e));
   }
   if (!failed.empty()) {
      // A violation DISCARDS the candidate; it is never a score term (§7).
      Json e = Json::object();
      e["iteration"] = iteration;
      e["action"] = akey;
      e["failed"] = names_of(failed);
      e["reason"] = join_reasons(failed);
      s.guardrail_log.push_back(std::move(e));
      return std::nullopt;
   }

   auto stored = std::make_shared<const Image>(std::move(produced));
   store.put(stored, candidate_id);
   // Kept on the Branch: if it survives into the beam, the next iteration's ranked menu needs this fingerprint.
   FingerprintData fp = measure(*stored, s);
   const double distance = fingerprint_distance(s.reference_fp, fp);
   Branch b;
   b.recipe = parent_recipe.extend(action);
   b.image_id = candidate_id;
   b.distance = distance;
   b.fingerprint = std::move(fp);
   return b;
}

// _close_iteration (loop.py:289-313): prune -> update best -> check convergence. session.iteration is already the
// number of the iteration being closed.
void close_iteration(Session& s, std::vector<Branch> candidates)
{
   std::vector<Branch> survivors = prune(std::move(candidates), s.config.width);
   if (survivors.empty()) {
      // §6.3 condition 3: guardrails exhausted the branch set; best is untouched, so this ends in a decline.
      s.branches.clear();
      s.converged = true;
      s.convergence_reason = "every branch was discarded by guardrails or executor failure";
      return;
   }
   s.branches = std::move(survivors);
   const Branch& best_candidate = s.branches.front();
   if (best_candidate.distance < s.best.distance - s.config.epsilon_improve)
      s.best = best_candidate;
   s.distance_history.push_back(s.best.distance);
   check_convergence(s);
}

} // namespace

Session begin(const std::string& source_path, const std::string& proxy_id, const std::string& reference_id,
              const FingerprintData& reference_fp, const std::string& work_dir, double pixel_scale_arcsec,
              double psf_fwhm_arcsec, const std::string& palette_class, int n_scales,
              const std::string& session_id, const ImageStore& store, BeamConfig config)
{
   Session s;
   s.session_id = session_id;
   s.work_dir = work_dir;
   s.source_path = source_path;
   s.proxy_id = proxy_id;
   s.reference_id = reference_id;
   s.reference_fp = reference_fp;
   s.pixel_scale_arcsec = pixel_scale_arcsec;
   s.psf_fwhm_arcsec = psf_fwhm_arcsec;
   s.palette_class = palette_class;
   s.n_scales = n_scales;
   s.config = config;

   const std::shared_ptr<const Image> proxy = store.get(proxy_id);
   FingerprintData baseline_fp = measure(*proxy, s);
   const double baseline = fingerprint_distance(reference_fp, baseline_fp);

   Branch root;
   root.image_id = proxy_id;
   root.distance = baseline;
   root.fingerprint = std::move(baseline_fp);
   s.branches = {root};
   s.best = root;
   s.baseline_distance = baseline;
   s.distance_history = {baseline};
   return s;
}

void advance(Session& s, Executor& executor, ImageStore& store, const GuardrailLimits& limits,
             const std::function<void(const Session&)>& on_iteration, const LoopTrace* trace)
{
   if (s.converged)
      return;

   const std::shared_ptr<const Image> source = store.get(s.proxy_id);
   std::vector<Branch> candidates;
   const int iteration = s.iteration + 1;

   for (const Branch& branch : s.branches) {
      const std::shared_ptr<const Image> parent = store.get(branch.image_id);
      std::optional<FingerprintData> measured;
      if (!branch.fingerprint)
         measured = measure(*parent, s);   // a cache miss, not a degraded path: the identical value, only slower
      const FingerprintData& parent_fp = branch.fingerprint ? *branch.fingerprint : *measured;
      // _ranked_menu: the whole ranking (top_k = None), in the proposer's own priority order.
      const std::vector<Action> menu =
         propose_actions(s.reference_fp, parent_fp, branch.recipe.applied_kinds(), s.n_scales, std::nullopt);
      if (trace && trace->on_menu)
         trace->on_menu(iteration, branch, menu);

      Walk walk;
      for (const Action& action : menu) {
         if (!walk.may_issue(s.config))
            break;
         // Counted before the executor runs: an attempt is an action this branch spent, whatever became of it.
         ++walk.attempts;

         Image produced;
         try {
            produced = executor.apply(*parent, action, s.pixel_scale_arcsec);
         } catch (const std::exception& e) {
            log_executor_failure(s, iteration, action.key(), describe_executor_failure(e));
            continue;
         }

         const std::string candidate_id =
            s.work_dir + "/cand-" + std::to_string(iteration) + "-" + std::to_string(candidates.size()) + ".png";
         const size_t log_before = s.guardrail_log.size();
         std::optional<Branch> candidate = ingest_candidate(s, iteration, branch.recipe, *parent, std::move(produced),
                                                            *source, action, limits, candidate_id, store);
         if (trace && trace->on_ingest) {
            LoopTrace::Attempt a;
            a.iteration = iteration;
            a.parent_recipe_key = branch.recipe.key();
            a.action_key = action.key();
            a.candidate_id = candidate_id;
            a.kept = candidate;
            for (size_t i = log_before; i < s.guardrail_log.size(); ++i)
               a.log_entries.push_back(s.guardrail_log[i]);
            trace->on_ingest(a);
         }
         if (!candidate)
            continue;
         candidates.push_back(std::move(*candidate));
         ++walk.kept;
      }
      log_attempt_cap(s, iteration, branch, walk);
   }

   s.iteration = iteration;
   close_iteration(s, std::move(candidates));
   store.release_unreferenced(live_image_ids(s));
   if (on_iteration)
      on_iteration(s);
}

void run_to_convergence(Session& s, Executor& executor, ImageStore& store, const GuardrailLimits& limits,
                        const std::function<void(const Session&)>& on_iteration, const LoopTrace* trace)
{
   while (!s.converged)
      advance(s, executor, store, limits, on_iteration, trace);
}

void check_convergence(Session& s)
{
   const BeamConfig& c = s.config;
   if (s.iteration >= c.iteration_cap) {
      s.converged = true;
      s.convergence_reason = "iteration cap (" + std::to_string(c.iteration_cap) + ") reached";
      return;
   }
   // window = distance_history[-(convergence_window + 1):], with Python's slice semantics for every window
   // (a non-positive window included: h[-0:] is the whole list, h[2:] starts at index 2).
   const auto& h = s.distance_history;
   const long long len = static_cast<long long>(h.size());
   const long long k = static_cast<long long>(c.convergence_window) + 1;
   const long long start = k > 0 ? std::max(len - k, 0LL) : std::min(-k, len);
   const long long wlen = len - start;
   if (wlen > c.convergence_window) {
      if (wlen == 0)
         throw std::out_of_range("list index out of range");   // window[0] on an empty slice, as Python
      const double first = h[static_cast<size_t>(start)], last = h.back();
      if (std::fabs(first - last) < c.epsilon) {
         s.converged = true;
         s.convergence_reason = "deltaD below " + dump_python(Json(c.epsilon)) + " across the last " +
                                std::to_string(c.convergence_window) + " iterations";
      }
   }
}

std::set<std::string> live_image_ids(const Session& s)
{
   std::set<std::string> live{s.proxy_id, s.best.image_id};
   for (const Branch& b : s.branches)
      live.insert(b.image_id);
   return live;
}

Json outcome(const Session& s)
{
   const bool improved = s.best.distance < s.baseline_distance - s.config.epsilon_improve;
   const Recipe empty;
   const Recipe& recipe = improved ? s.best.recipe : empty;
   Json j = Json::object();
   j["improved"] = improved;
   j["baseline_distance"] = s.baseline_distance;
   j["distance"] = improved ? s.best.distance : s.baseline_distance;
   j["result_path"] = improved ? s.best.image_id : s.proxy_id;
   j["recipe"] = recipe.to_json();
   j["pixinsight_steps"] = recipe.to_pixinsight_steps();
   j["iterations"] = s.iteration;
   j["convergence_reason"] = s.convergence_reason;
   j["reference_id"] = s.reference_id;
   j["guardrail_log"] = s.guardrail_log;
   return j;
}

} // namespace ac
