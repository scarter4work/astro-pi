// AutoContrast core (PCL-free): the optimisation loop (design §6) — optimize/loop.py (begin, advance,
// ingest_candidate, _close_iteration, _check_convergence, run_to_convergence, outcome), line for line.
//
// The fail-safe is structural, not a special case. Best-so-far is seeded with the INPUT and its own baseline
// distance; a candidate replaces it only on a strict improvement beyond epsilon_improve. The fingerprint distance
// is a valley (D = 0 at the reference, rising in both directions, the over-processed one included), so an
// already-good image starts near the floor, every move climbs, and the loop declines on its own. On a decline the
// outcome hands back the proxy itself, untouched.
#pragma once

#include "core/Json.h"
#include "core/optimize/Executor.h"
#include "core/optimize/Guardrails.h"
#include "core/optimize/Session.h"

#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ac {

// Optional observation points for tests and the tool (the loop never depends on them).
struct LoopTrace {
   // A branch's whole ranked menu, as the loop is about to walk it.
   std::function<void(int iteration, const Branch& parent, const std::vector<Action>& menu)> on_menu;
   // One ingest_candidate call (executor failures never reach it): the guardrail-log entries it appended, and
   // the scored branch when the candidate was kept.
   struct Attempt {
      int iteration = 0;
      std::string parent_recipe_key, action_key, candidate_id;
      std::optional<Branch> kept;
      Json log_entries = Json::array();
   };
   std::function<void(const Attempt&)> on_ingest;
};

// Open a session, measuring the proxy (store.get(proxy_id), which must hold it) as the baseline AND as
// best-so-far. The root branch's image is the proxy.
Session begin(const std::string& source_path, const std::string& proxy_id, const std::string& reference_id,
              const FingerprintData& reference_fp, const std::string& work_dir, double pixel_scale_arcsec,
              double psf_fwhm_arcsec, const std::string& palette_class, int n_scales,
              const std::string& session_id, const ImageStore& store, BeamConfig config = {});

// One iteration: every live branch walks its ranked menu until it holds config.top_k live candidates or has
// attempted config.resolved_max_attempts() actions (§3.3); each candidate goes no-op check -> guardrails ->
// score; then prune -> update best -> check convergence. Candidate ids are Python's paths,
// "<work_dir>/cand-<iteration>-<number of candidates kept so far>.png", and only kept candidates are stored.
// An executor exception is logged (`failed: ["executor"]`, reason describe_executor_failure) and the branch goes
// on. After closing the iteration, every store image no live branch, best or the proxy references is released,
// then on_iteration(session) runs (the caller writes session.json there). No-op on a converged session.
// If advance() throws (an exception escaping the executor, a NaN distance refused by prune, a failure inside
// extract / the guardrails), the session and store are left mid-iteration — this iteration's log entries
// appended and its kept candidates stored, `iteration` not advanced — and must not be advanced again: a retry
// would duplicate log entries and collide on the reused candidate ids. Python's advance behaves the same way.
void advance(Session& session, Executor& executor, ImageStore& store, const GuardrailLimits& limits = {},
             const std::function<void(const Session&)>& on_iteration = {}, const LoopTrace* trace = nullptr);

// advance until converged.
void run_to_convergence(Session& session, Executor& executor, ImageStore& store, const GuardrailLimits& limits = {},
                        const std::function<void(const Session&)>& on_iteration = {},
                        const LoopTrace* trace = nullptr);

// §6.3, an OR of the conditions (condition 2, the VLM, is absent): the iteration cap, or |ΔD| below epsilon
// across the last convergence_window iterations. Sets converged / convergence_reason (Python's text).
void check_convergence(Session& session);

// The image ids the session still needs: the proxy, best, and every branch.
std::set<std::string> live_image_ids(const Session& session);

// The reportable result (loop.py:outcome), Python's keys and order. improved == false means the loop DECLINED:
// result_path is the proxy id, recipe empty. result_path / proxy ids are image ids.
Json outcome(const Session& session);

} // namespace ac
