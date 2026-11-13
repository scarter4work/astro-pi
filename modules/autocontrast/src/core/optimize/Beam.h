// AutoContrast core (PCL-free): beam search state (design §6.1) — optimize/beam.py.
//
// A beam, not a hill-climb: improvement is non-monotonic, and a greedy loop walks straight into the over-cooked
// attractor because every individual step "increased contrast".
#pragma once

#include "core/fingerprint/Extract.h"
#include "core/optimize/Recipe.h"

#include <optional>
#include <string>
#include <vector>

namespace ac {

// tools/autocontrast/src/autocontrast/optimize/beam.py:18-76 (BeamConfig defaults :19-24; max_attempts None -> 3 * top_k :72-76)
struct BeamConfig {
   int width = 3;
   int top_k = 3;
   int iteration_cap = 20;
   int convergence_window = 3;
   double epsilon = 1e-3;
   double epsilon_improve = 1e-3;
   // How many actions a branch may ATTEMPT while reaching for top_k live candidates (§3.3): top_k counts
   // candidates that survived, so a guardrail trip must not also cost the branch a slot; but the retry is
   // bounded so the run has a predictable cost. 0 means "derive 3 * top_k" (Python's None), so the
   // relationship survives a change to top_k.
   int max_attempts = 0;

   int resolved_max_attempts() const { return max_attempts == 0 ? 3 * top_k : max_attempts; }
};

struct Branch {
   Recipe recipe;
   std::string image_id;   // Python's image_path
   double distance = 0.0;
   bool alive = true;
   // The fingerprint of image_id's pixels, carried forward from scoring instead of re-measured (beam.py:80-107).
   // nullopt only means "not measured yet"; the loop then measures, with an identical result.
   std::optional<FingerprintData> fingerprint;
};

// Distances closer than this are one tie in prune (C++ only; beam.py has no counterpart).
//
// Why: two candidates whose actions commute (chroma then local_equalize vs local_equalize then chroma) differ only
// at the last bits, so their distances tie to ~1e-16. Python's sort then orders them by noise, and that noise is
// numpy's own: on an AVX512 host np.exp / np.power / np.log1p dispatch to SIMD (SVML) kernels that differ from
// libm by an ulp on ~5% of inputs (measured 2026-11-12), so even Python's order changes with the host CPU. In
// golden (d) Python's two permutations tie EXACTLY (stable order: the earlier candidate first) while the C++
// values differ by 8e-17 and would reverse them. No port can reproduce last-bit noise short of reimplementing
// numpy's SIMD transcendental kernels, so the noise is not allowed to decide: a run of distances closer than this
// keeps its input order (branch order, then menu order), exactly what Python's stable sort does for a tie.
// 1e-12 is ~4 orders above the measured C++/Python distance difference on the loop goldens (<= 1.7e-16) and
// ~9 orders below every decision threshold (epsilon, epsilon_improve = 1e-3); on all five loop goldens every
// candidate pair closer than 1e-6 was either an exact tie or already in input order, so the rule changes no
// Python decision there.
constexpr double kPruneTieTolerance = 1e-12;

// Keep the `width` best live, distinct branches (beam.py:110-126). A stable sort by distance — Python's sorted()
// is stable, so ties keep their input order — then noise-level ties are also kept in input order: the sorted list
// is cut into runs, each starting at the smallest distance not yet placed (its anchor) and holding every following
// branch whose distance is at most anchor + kPruneTieTolerance; each run is ordered by input position.
// Guarantee: two branches change relative order versus a plain stable sort by distance only if they are in the
// same run, so their distances differ by at most kPruneTieTolerance (both lie in [anchor, anchor + tol]). Branches
// further apart than the tolerance always keep distance order. Then the first branch of each recipe key. Like Python, `width` is tested only after a branch is kept (width <= 0 therefore keeps every
// distinct branch). Throws std::invalid_argument on a NaN distance among the live branches: Python's sort order is
// then an artefact of timsort's comparisons, and a NaN distance is an upstream bug that must not pick the beam
// silently.
std::vector<Branch> prune(std::vector<Branch> candidates, int width);

} // namespace ac
