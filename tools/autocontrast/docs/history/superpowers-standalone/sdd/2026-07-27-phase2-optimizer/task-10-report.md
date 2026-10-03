# Task 10 report — the loop, and the fail-safe that falls out of the valley

**Branch:** `phase2-optimizer`
**Commit:** `f1c8e41940287c620035b27e97ad1d702c5eef86`
**Files created:** `src/autocontrast/optimize/loop.py`, `tests/test_optimize_loop.py`
**Files modified:** none. `git status` before commit showed only the two new files.
**Note:** this report lives under `.superpowers/`, which is gitignored, so the commit contains only the two source files.

---

## 1. What I implemented

`src/autocontrast/optimize/loop.py` exposes:

| symbol | role |
| --- | --- |
| `begin(...) -> Session` | opens a session; measures the input as BOTH the baseline distance and the seed for best-so-far |
| `resume(session_file, *, proxy_path) -> Session` | reopens a persisted session, refusing a proxy mismatch — carried requirement (C) |
| `advance(session, executor, *, load, save, limits) -> Session` | one iteration: expand, guardrail, score, prune |
| `run_to_convergence(...) -> Session` | iterate until `session.converged` |
| `outcome(session) -> dict` | the reportable result, including the decline path |

The fail-safe is structural, per the brief. `begin` seeds `Session.best` with a root `Branch` whose
`image_path` **is** `proxy_path` and whose distance is the baseline. `advance` replaces `best` only on
`candidate.distance < best.distance - epsilon_improve`. `outcome` recomputes `improved` against the
baseline and, when false, returns `proxy_path`, `Recipe.empty()`, and `baseline_distance` — so a decline
hands back the literal input file, not a recombination of whatever the search produced. Measured
(§4 below): on the reference-vs-itself fixture every single candidate scores worse than baseline, so the
decline is the valley doing its job, not a guard bolted on top.

Global constraints honoured:

- Guardrail violations **discard**, never score. There is no penalty term anywhere in `advance`.
- Convergence is the OR of the three §6.3 conditions, each with its own reported reason. "Every branch
  was discarded by guardrails or executor failure" is reported as exactly that, and leaves `best`
  untouched so the run terminates in a decline rather than in an unvetted result.
- An executor exception logs the candidate and continues; it never propagates.
- No AI, no new dependencies.

---

## 2. Deviations from the brief's code, and why

### 2.1 `np.ptp(layered)`, not `layered.ptp()` (test fixture)

`ndarray.ptp` was **removed in NumPy 2.0**; this venv is NumPy 2.5.1. The brief's fixture raises
`AttributeError: 'numpy.ndarray' object has no attribute 'ptp'` verbatim. Verified:

```
$ .venv/bin/python -c "import numpy as np; print(np.__version__); np.arange(5.).ptp()"
2.5.1
ERR 'numpy.ndarray' object has no attribute 'ptp'
```

Changed to the free function, which is behaviourally identical.

### 2.2 A branch contributes `top_k` **live** candidates, not `top_k` attempts — FLAG FOR REVIEW

**This is a design decision beyond the brief's code and the one thing in this task I would most want a
second opinion on.** It is confined to `loop.py`.

The brief's `advance` calls `propose_actions(..., top_k=config.top_k)` and attempts exactly those.
Against the brief's own acceptance test, `test_a_flattened_image_is_measurably_improved`, that
**fails**:

```
$ .venv/bin/python -m pytest tests/test_optimize_loop.py -k flattened
E       assert False is True     # result["improved"]
```

The brief anticipates this and instructs: *"do not lower `epsilon_improve` to force it — re-read spec
§3.7. Diagnose whether the proposer is offering actions that address the actual gap first."* So I
diagnosed rather than tuned. Measured, on `flatten(_reference_image(), 0.6)`, baseline D = 0.154184:

```
core_hdr@-/strong                      D=0.2017 delta=+0.0475 ok
background_neutralize@-/-              D=0.1666 delta=+0.0125 DISCARD:hue_invention
black_point@-/strong                   D=0.1681 delta=+0.0139 DISCARD:noise_floor
tonal_reshape@-/moderate               D=0.1316 delta=-0.0226 DISCARD:noise_floor
local_contrast@2.000/moderate          D=0.1301 delta=-0.0241 ok        <-- rank 5
local_equalize@2.000/moderate          D=0.1219 delta=-0.0323 DISCARD:noise_floor
chroma@-/gentle                        D=0.1516 delta=-0.0026 ok
star_split@-/-                         D=0.1542 delta=+0.0000 ok        <-- the no-op, (B)
```

The proposer **is** offering an action that closes the actual gap — `local_contrast@2"/moderate`,
Δ = −0.0241, guardrails clean — but at **rank 5**. Component gaps are
`{spectrum 0.216, background 0.169, tonal 0.131, chroma 0.048}` while the per-band deficit at 2" is
0.121, so the three aggregate remedies out-rank the band remedy. That ranking is Task 7's calibrated
behaviour and I did not touch it. The failure is that with `top_k = 3`, two of the top three are
guardrail trips and the third climbs, and the loop never reaches rank 5.

The fix, in `loop.py` only: walk the ranked menu until the branch has produced `top_k` **surviving**
candidates. `top_k` becomes "how many live candidates this branch contributes", not "how many actions it
may attempt". Reasoning:

1. **A discard that also costs search breadth is a score term wearing a different hat.** §7 says a
   guardrail violation discards the candidate and is never a score term. Under attempt-counting, a
   guardrail trip silently removes one of the branch's chances to find anything — the guardrail stops
   being a filter and starts shaping the search. That is the same failure the "never a score term" rule
   exists to prevent, one level up.
2. **The beam width becomes a promise the loop can keep.** With attempt-counting the beam can only ever
   narrow and never refill.
3. **It changes no constant.** No threshold, no epsilon, no calibrated value moved. When nothing is
   discarded the behaviour is byte-for-byte the brief's.

Obtaining the full ranking: `propose_actions` caps at `top_k`, so `_ranked_menu` passes
`top_k=len(available_actions(...))` — the menu is a strict upper bound on the group count, since groups
cannot outnumber their members. This duplicates no ranking logic (only the menu construction) and tracks
`actions.py` automatically if a kind is added.

Cost: bounded by `width × menu_size` executions per iteration instead of `width × top_k`. Measured on
these fixtures: 18 groups per branch, and the whole 16-test file runs in ~31 s.

With this change the flattened image improves from D = 0.154184 to **D = 0.024127** (84 % of the gap
closed) with a 19-action recipe.

---

## 3. The three carried requirements

### (A) Passing verdicts that carry a reason are surfaced

`advance` now partitions verdicts twice:

```python
noted  = [v for v in verdicts if v.ok and v.reason]
failed = [v for v in verdicts if not v.ok]
```

`noted` entries are logged under a **`noted`** key, `failed` entries under **`failed`** — distinguishable
by field, not by parsing a string. A `noted` entry is recorded regardless of the candidate's eventual
fate, because "star integrity was never evaluated" is equally true of a candidate some other guardrail
went on to reject. Today the only guardrail that passes with a reason is `check_star_integrity` on a
starless baseline (`"star integrity not assessed: baseline had no detectable stars"`), but the rule is
written against the property, not that check.

### (B) Candidate ordering and the `star_split` no-op — approach and justification

**Approach: a candidate whose pixels are `np.array_equal` to its parent's is discarded before guardrails
are even run, and logged with `failed: ["no_op"]`.**

Justification, and why this and not the alternatives:

- **The problem is real and I measured it.** `star_split` is a deliberate no-op in `NumpyExecutor`. Its
  candidate is pixel-identical to its parent, so its distance is identical (measured: Δ = +0.0000,
  bit-identical), its recipe key differs so `prune`'s deduplication cannot see it, and `prune` sorts
  stably — so on an already-good image, where the parent's score is the *best score available*, the
  no-op sorts **first** and takes the top beam slot having explored nothing. Confirmed empirically:
  against the brief's unmodified code the surviving branch kinds are
  `{'black_point', 'core_hdr', 'star_split'}`.
- **Detection is on the property, never the string.** The check is "did the image change", not
  `action.kind == "star_split"`. `star_split` is a no-op today only because the numpy executor has no
  starless layer to route; the PixInsight executor's `star_split` genuinely changes the image and must
  survive. Matching the name would hard-code one executor's limitation into the loop, and would miss the
  next no-op — e.g. `local_contrast` at a band whose starlet plane has decayed to nothing, which shows up
  in the same measurement above at Δ = +0.0000 for `local_contrast@64"`.
- **Discard, not tie-break.** I considered instead giving `prune` a tie-break that prefers shorter
  recipes. Rejected: it treats the symptom (ordering) rather than the fact (a candidate that learned
  nothing), and it would still let the no-op occupy a slot whenever it was not exactly tied. A candidate
  identical to its parent carries zero information by definition; the correct disposition is to not have
  it.
- **It costs no search breadth.** Because a discard no longer consumes one of the branch's `top_k` live
  slots (§2.2), removing the no-op refunds the slot to a real action rather than shrinking the beam.
- **Placed before `evaluate_guardrails`.** Guardrailing an image identical to the baseline it is compared
  against is wasted work with a foregone result.

Acknowledged consequence, stated rather than hidden: because the no-op never lands in a recipe,
`applied_kinds` never records it, so `star_split` is re-proposed (and re-discarded) on every iteration
under the numpy executor. It ranks last among groups with zero gap, so the cost is one wasted executor
call per branch per iteration. I judged this preferable to the alternative of recording an action in the
recipe that did nothing — the recipe is the §12 audit artifact and must be replayable; an entry that
changes no pixels would be a lie in the audit log.

### (C) Exact `proxy_path` comparison on resume

`resume(session_file, *, proxy_path)` loads the session and raises `ValueError` naming **both** paths on
mismatch. The comparison is exact string equality. Two reasons, both in the docstring: path identity is
the only evidence available that the resumed image is the one the session's `layer` values were computed
for (the session records no dimensions), so normalizing trades evidence for a guess; and `proxy_path`
need not be a filesystem path at all — the executor seam may address images through an in-memory or
PixInsight-side store, leaving nothing to resolve. `test_resume_compares_exactly_not_by_resolution`
pins this by asserting `/mem/./proxy.png` is rejected.

---

## 4. Fixture verification (measured, not assumed)

```
reference vs itself (already good)     baseline D = -5.36e-20   stars=0
flatten(ref, 0.6)                      baseline D =  0.154184   stars=0
starry fixture                         baseline D =  0.053410   stars=13
```

- The "already good" fixture genuinely sits **at** the valley floor — D is floating-point zero
  (−5.4e−20), not merely small. The test asserts this (`pytest.approx(0.0, abs=1e-12)`) so the decline
  test cannot silently degrade into "a test of a bad reference".
- **Discovered while testing:** the brief's `_reference_image` is itself **starless** (0 detected stars).
  That is convenient for (A) — it is a natural starless-baseline fixture — but it also means the brief's
  five tests never exercise `check_star_integrity` in its assessing mode at all. I added `_starry_image`
  (the same field with 40 injected point sources, 13 detected) so the (A) test has a real negative
  direction, and so at least one test in this file runs the star guardrail for real.
- Both fixtures assert their own star counts inside the tests rather than relying on the docstrings.

End-to-end behaviour of the two fixtures:

```
reference vs itself: improved=False  D -0.000000 -> -0.000000  iters=3   reason='deltaD below 0.001 across the last 3 iterations'  recipe=[]
flatten(ref, 0.6):   improved=True   D  0.154184 ->  0.024127  iters=20  reason='iteration cap (20) reached'
```

Note the flattened run terminates on the **iteration cap**, not on ΔD — it was still improving at
iteration 20. Reported as such; "exhausted" is not dressed up as convergence.

---

## 5. Exact pytest commands and real output

### 5.1 Step 2 — expected failure, before `loop.py` existed

```
$ .venv/bin/python -m pytest tests/test_optimize_loop.py -v
collecting ... collected 0 items / 1 error
tests/test_optimize_loop.py:19: in <module>
    from autocontrast.optimize.loop import advance, begin, outcome, resume, run_to_convergence
E   ModuleNotFoundError: No module named 'autocontrast.optimize.loop'
=========================== short test summary info ============================
ERROR tests/test_optimize_loop.py
=============================== 1 error in 0.34s ===============================
```

### 5.2 The brief's UNMODIFIED implementation, against the final test file

I wrote the brief's `loop.py` verbatim into place to check it. It has no `resume` at all, which blocks
collection at import and would make (A)/(B) untestable, so I appended the **no-check** resume the brief
implies (`return Session.load(session_file)` — Task 9 "deliberately does NOT verify", and the brief adds
nothing). That makes (C) fail on *behaviour* rather than on ImportError, which is the stronger
demonstration.

```
$ .venv/bin/python -m pytest tests/test_optimize_loop.py -k "assessed_nothing or no_op or resum"
E       AssertionError: star_integrity passed while reporting it could not assess anything, and that reason never reached the log
E       assert []
E       AssertionError: assert 'star_split' not in {'black_point', 'core_hdr', 'star_split'}
E       AssertionError: a chroma action that changed nothing was allowed to hold a slot
E       Failed: DID NOT RAISE ValueError
E       Failed: DID NOT RAISE ValueError
=========================== short test summary info ============================
FAILED tests/test_optimize_loop.py::test_a_passing_guardrail_that_assessed_nothing_is_logged
FAILED tests/test_optimize_loop.py::test_a_no_op_candidate_does_not_take_a_beam_slot
FAILED tests/test_optimize_loop.py::test_the_no_op_rule_is_a_property_not_a_name
FAILED tests/test_optimize_loop.py::test_resuming_against_a_different_proxy_is_refused
FAILED tests/test_optimize_loop.py::test_resume_compares_exactly_not_by_resolution
================== 5 failed, 1 passed, 10 deselected in 1.54s ===================
```

**Confirmed: every one of (A), (B), (C) has at least one test that fails against the brief's unmodified
code, and each fails for its own stated reason** — not for a shared import error.

Note `assert 'star_split' not in {'black_point', 'core_hdr', 'star_split'}`: under the brief's code the
no-op did not merely survive, it took a beam slot, exactly as predicted.

The full run of the brief's code against the final test file additionally failed three of the brief's
**own** tests — `test_a_flattened_image_is_measurably_improved`,
`test_iteration_cap_terminates_the_loop` (it reported `every branch was discarded` instead of the cap),
and `test_an_executor_failure_kills_the_candidate_not_the_run` (the flaky `local_contrast` was never
reached at rank ≥ 5, so no executor failure was ever recorded). All three are consequences of §2.2.

`test_nothing_is_noted_when_the_guardrail_really_did_its_job` passes against the brief's code — correctly:
it is the *negative* direction of (A), and code that never notes anything trivially satisfies it. It
exists so that (A) cannot be satisfied by an implementation that notes every candidate.

### 5.3 Step 4 — the final implementation

```
$ .venv/bin/python -m pytest tests/test_optimize_loop.py -v
collected 16 items

tests/test_optimize_loop.py::test_a_flattened_image_is_measurably_improved PASSED [  6%]
tests/test_optimize_loop.py::test_the_reference_itself_is_declined PASSED [ 12%]
tests/test_optimize_loop.py::test_declining_returns_the_input_untouched PASSED [ 18%]
tests/test_optimize_loop.py::test_a_climbing_candidate_never_becomes_best PASSED [ 25%]
tests/test_optimize_loop.py::test_iteration_cap_terminates_the_loop PASSED [ 31%]
tests/test_optimize_loop.py::test_convergence_reason_is_always_reported PASSED [ 37%]
tests/test_optimize_loop.py::test_exhausting_every_branch_is_not_dressed_up_as_success PASSED [ 43%]
tests/test_optimize_loop.py::test_a_guardrail_violation_discards_rather_than_scores PASSED [ 50%]
tests/test_optimize_loop.py::test_an_executor_failure_kills_the_candidate_not_the_run PASSED [ 56%]
tests/test_optimize_loop.py::test_a_passing_guardrail_that_assessed_nothing_is_logged PASSED [ 62%]
tests/test_optimize_loop.py::test_nothing_is_noted_when_the_guardrail_really_did_its_job PASSED [ 68%]
tests/test_optimize_loop.py::test_a_no_op_candidate_does_not_take_a_beam_slot PASSED [ 75%]
tests/test_optimize_loop.py::test_the_no_op_rule_is_a_property_not_a_name PASSED [ 81%]
tests/test_optimize_loop.py::test_resuming_against_a_different_proxy_is_refused PASSED [ 87%]
tests/test_optimize_loop.py::test_resuming_against_the_same_proxy_succeeds PASSED [ 93%]
tests/test_optimize_loop.py::test_resume_compares_exactly_not_by_resolution PASSED [100%]

============================= 16 passed in 31.64s ==============================
```

### 5.4 Whole suite — nothing else regressed

```
$ .venv/bin/python -m pytest -q
382 passed in 74.75s (0:01:14)
```

No tests were deselected, skipped, or marked xfail. `git status --short` before commit listed only the
two new files; `git diff --stat` against HEAD was empty.

---

## 6. Test hygiene notes

Both directions are asserted wherever a behaviour has two sides:

| behaviour | positive | negative |
| --- | --- | --- |
| improve / decline | flattened image improves, returns a *different* path with *different* pixels | reference returns `proxy_path`, empty recipe, empty PI steps |
| decline is untouched | — | dtype, shape **and** `tobytes()` compared, not just `array_equal` |
| guardrail discards | strict limits empty the beam | same candidates under normal limits do survive, so the emptiness is the guardrail's doing and not an empty proposal list |
| (A) noted verdicts | starless baseline produces `noted` entries naming `star_integrity` | starry baseline produces none, while candidates demonstrably reached the guardrails |
| (B) no-op rule | a no-op `chroma` (via `_NoOpFor`) is discarded — the rule is not the string | a `star_split` that really changes pixels (via `_EffectiveStarSplit`) is **not** discarded |
| (C) resume | mismatched path raises, naming both | matching path resumes and round-trips `baseline_distance` |

"What would have to break for this to fail?" applied, and three assertions strengthened as a result:

- `test_iteration_cap_terminates_the_loop` asserts the exact string `"iteration cap (2) reached"`, not
  just `converged`. Under the brief's code it converged for a *different* reason and the loose assertion
  would have passed.
- `test_convergence_reason_is_always_reported` checks the reason is one of the three §6.3 forms, not
  merely truthy — a reason of `"?"` would satisfy truthiness.
- `test_a_climbing_candidate_never_becomes_best` asserts the *mechanism* (every candidate off the
  already-good image scores strictly worse than baseline), so the decline cannot be an accident of
  convergence timing.
- `test_exhausting_every_branch...` excludes `no_op`/`executor` entries from the "every discard names
  `noise_floor`" assertion rather than folding them in, so a run that discarded everything for the wrong
  reason still fails it.

Two of my own tests initially failed on wrong assumptions (that `_reference_image` was starry; that every
discard in an exhausted run would be a guardrail). I measured and corrected the tests rather than
loosening them.

---

## 7. Self-review

**What I am confident in.** The fail-safe is genuinely structural — `test_a_climbing_candidate_never_becomes_best`
shows every available move off the already-good image climbs, so the decline is the valley's doing.
Declining is byte-identical. (A), (B), (C) each have a test that fails against the brief's code for its
own reason.

**What I would want reviewed.**

1. **§2.2, the live-candidate change.** It is the one place I went beyond the brief's code on my own
   judgement, and it is load-bearing: without it the brief's own acceptance test fails. I believe the
   §7 argument is sound, but it changes the meaning of a config field (`top_k`) that Tasks 7–9 wrote
   against, and it raises worst-case per-iteration cost from `width × top_k` to `width × menu_size`.
   If the team prefers, the alternative is to leave `top_k` as attempts and raise `BeamConfig.top_k`'s
   default — but that is tuning a constant to rescue a test, which §3.7 forbids, so I did not.
2. **The flattened run hits the iteration cap rather than converging.** D was still falling at
   iteration 20. Not wrong — it is reported honestly — but it means the default `iteration_cap` may be
   under-sized for a badly flattened input, and Task 12's real-data run should be watched for this.
3. **The 19-action recipe is dominated by repeated `local_contrast`.** Fifteen of nineteen actions are
   `local_contrast`, because it is not in `ONCE_ONLY` and its band deficit keeps regenerating. The
   distance says this is right and the noise guardrail never trips, but whether stacking fifteen
   MultiscaleLinearTransform steps is a *sane PixInsight recipe* is a question only the live tests
   (§7.4) can answer. Flagging it now rather than letting Task 12 discover it.
4. **`star_split` is re-proposed and re-discarded every iteration** under the numpy executor (see (B)).
   Cheap, and I argued above why recording it in the recipe would be worse, but it is a known
   inefficiency rather than an oversight.

---

# Fix round 1 — the bounded retry

**Commit:** `9c5e19c` on `phase2-optimizer`
**Authority:** spec commit `eb2ac3e`, §3.3 subsection *"A guardrail trip must not also cost search
breadth — but the retry is bounded"*.
**Files:** `src/autocontrast/optimize/beam.py` (the one existing file authorised for modification, and
only to add the field), `src/autocontrast/optimize/loop.py`, `tests/test_optimize_loop.py`.

## What changed

`BeamConfig` gains a named `max_attempts` field:

```python
max_attempts: int | None = None

def __post_init__(self) -> None:
    if self.max_attempts is None:
        object.__setattr__(self, "max_attempts", 3 * self.top_k)
```

`None` means "derive `3 × top_k`" rather than a hard-coded `9`, so the relationship survives a change to
`top_k` instead of silently decoupling from it. It stays explicitly overridable, and it round-trips
through `Session.save`/`load` unchanged (`asdict` stores the resolved int; `BeamConfig(**d)` accepts it).
The comment block on the field carries the measurement and the cost table so the tunable can be
re-argued rather than merely re-read.

`advance` now walks the ranked menu under two conditions instead of one:

```python
kept = 0
attempts = 0
for action in actions:
    if kept >= session.config.top_k or attempts >= session.config.max_attempts:
        break
    attempts += 1
    ...
```

An attempt is counted **before** the executor runs — an attempt is an action the branch spent, whatever
became of it, and every path through the body costs at least an executor call. Executor failures, no-op
discards and guardrail trips all count; only that makes the bound a real bound on work.

## The surfaced event

When the cap binds before the branch fills its slots, an entry is appended to `guardrail_log`. Real
output, verbatim:

```json
{
  "iteration": 1,
  "branch": "(root)",
  "noted": ["attempt_cap"],
  "reason": "branch stopped after 9 attempts (cap 9) holding only 1 of 3 live candidates; guardrails or the executor rejected nearly everything tried on this image"
}
```

Three deliberate choices in that shape:

- **`noted`, not `failed`.** Nothing was discarded here — the branch stopped looking. `failed` means a
  candidate was thrown away, and conflating the two would let a reader count discards wrong.
- **`branch`, not `action`.** It is a property of the branch's whole expansion, not of any one
  candidate. It is also what makes the two `noted` event kinds mechanically distinguishable.
- **The condition is `kept < top_k AND attempts >= max_attempts`.** A branch that ran out of *menu*
  has not been truncated, and a branch that hit the cap having already filled its slots lost nothing.
  Neither logs.

Because `noted` is now shared by two event kinds, the two (A) tests were tightened to filter on
`"star_integrity" in e.get("noted", [])` rather than on the presence of the field — a test that accepted
any `noted` entry could have passed on the wrong event.

## Cost, measured on these fixtures

| | value |
| --- | --- |
| flattened e2e, D | 0.154184 → **0.024127** (unchanged from the unbounded version) |
| recipe length | 19 actions (unchanged) |
| candidate attempts, whole 20-iteration run | 413, i.e. **~20.7/iteration** against the 27 worst case |
| times the cap bound in that run | 3 |

**The bound cost nothing in result quality on this fixture.** It binds only where trips cluster, exactly
as §3.3 predicts.

## Tests — both directions

Five added, 21 total in the file:

| test | asserts |
| --- | --- |
| `test_max_attempts_defaults_to_three_times_top_k` | `BeamConfig().max_attempts == 9`, `BeamConfig(top_k=5).max_attempts == 15`, and an explicit override survives |
| `test_max_attempts_survives_a_session_round_trip` | save → `resume` preserves `max_attempts` and `top_k` |
| `test_the_attempt_cap_binds_and_says_so` | an executor where only `chroma` works: branch contributes `0 < n < top_k` **without error**, exactly one `attempt_cap` event, `branch == "(root)"`, no `failed` key, the reason names the real attempt count, and no more actions were tried than the cap allows |
| `test_the_attempt_cap_does_not_bind_on_an_ordinary_branch` | a branch whose actions survive gets its full `top_k` and logs no event |
| `test_a_tight_cap_binds_where_a_loose_one_does_not` | same image, same executor, same limits, **only the bound differs**: `max_attempts=2` truncates where the default does not |

The third test is what the ruling asked for; the fourth and fifth are why it means something. The fifth
in particular isolates the cap as the *cause* — without it, "the default cap did not bind" would be
compatible with an implementation whose cap never binds at all.

Verified the cap genuinely bound rather than the menu running out: attempts logged = **9**, cap = **9**,
live branches = **1**, `top_k` = 3.

## Real command output

```
$ .venv/bin/python -m pytest tests/test_optimize_loop.py -v
collected 21 items

tests/test_optimize_loop.py::test_a_flattened_image_is_measurably_improved PASSED [  4%]
tests/test_optimize_loop.py::test_the_reference_itself_is_declined PASSED [  9%]
tests/test_optimize_loop.py::test_declining_returns_the_input_untouched PASSED [ 14%]
tests/test_optimize_loop.py::test_a_climbing_candidate_never_becomes_best PASSED [ 19%]
tests/test_optimize_loop.py::test_iteration_cap_terminates_the_loop PASSED [ 23%]
tests/test_optimize_loop.py::test_convergence_reason_is_always_reported PASSED [ 28%]
tests/test_optimize_loop.py::test_exhausting_every_branch_is_not_dressed_up_as_success PASSED [ 33%]
tests/test_optimize_loop.py::test_a_guardrail_violation_discards_rather_than_scores PASSED [ 38%]
tests/test_optimize_loop.py::test_an_executor_failure_kills_the_candidate_not_the_run PASSED [ 42%]
tests/test_optimize_loop.py::test_a_passing_guardrail_that_assessed_nothing_is_logged PASSED [ 47%]
tests/test_optimize_loop.py::test_nothing_is_noted_when_the_guardrail_really_did_its_job PASSED [ 52%]
tests/test_optimize_loop.py::test_max_attempts_defaults_to_three_times_top_k PASSED [ 57%]
tests/test_optimize_loop.py::test_max_attempts_survives_a_session_round_trip PASSED [ 61%]
tests/test_optimize_loop.py::test_the_attempt_cap_binds_and_says_so PASSED [ 66%]
tests/test_optimize_loop.py::test_the_attempt_cap_does_not_bind_on_an_ordinary_branch PASSED [ 71%]
tests/test_optimize_loop.py::test_a_tight_cap_binds_where_a_loose_one_does_not PASSED [ 76%]
tests/test_optimize_loop.py::test_a_no_op_candidate_does_not_take_a_beam_slot PASSED [ 80%]
tests/test_optimize_loop.py::test_the_no_op_rule_is_a_property_not_a_name PASSED [ 85%]
tests/test_optimize_loop.py::test_resuming_against_a_different_proxy_is_refused PASSED [ 90%]
tests/test_optimize_loop.py::test_resuming_against_the_same_proxy_succeeds PASSED [ 95%]
tests/test_optimize_loop.py::test_resume_compares_exactly_not_by_resolution PASSED [100%]

============================= 21 passed in 31.15s ==============================

$ .venv/bin/python -m pytest -q
387 passed in 66.16s (0:01:06)

$ git log --oneline -1
9c5e19c optimize: bound the guardrail retry at max_attempts (3x top_k)

$ git status --short
(empty)
```

All 16 round-1 tests still pass. Nothing deselected, skipped or xfailed. 382 → 387 suite total is the
five new tests.

## Retained from round 1, per the ruling

The `np.array_equal` no-op discard (and its absence from the recipe), the passing-verdict `noted`
logging, the exact `proxy_path` comparison on resume, the byte-identical decline path, the `np.ptp` fix,
and the starry fixture — all unchanged.

## Open calibration questions, recorded per the ruling

1. **`iteration_cap` may be under-sized.** The flattened run still terminates on the cap at 20 with D
   falling, not on ΔD. A §3.7 tunable; Task 12/14 is where it gets calibrated against real convergence.
2. **`max_attempts = 3 × top_k` is itself a §3.7 tunable.** On these fixtures it binds 3 times in 20
   iterations and costs nothing in quality, but that is synthetic 160px data. The real 1600px proxy is
   where the trip rate should be re-measured — if the cap binds on most branches, the bound is doing
   more than protecting the budget.
3. **15 of 19 recipe actions are `local_contrast`.** Carried to Task 13 by the lead; only the live tests
   can say whether stacked MultiscaleLinearTransform steps make a sane PixInsight recipe.

---

## Fix round 2 (reviewer findings, commit df577e7)

Scope: exactly the two findings raised by the review at
`.superpowers/sdd/2026-07-27-phase2-optimizer/task-10-review.md` (range `094133f..9c5e19c`). Nothing else
was touched — no `session.save()` added to the loop, no tunable changed, no spec edits.

### Finding 1 (Important) — `_ranked_menu` duplicated `propose.py`'s menu construction

The danger as stated in the review: `_ranked_menu` rebuilt `palette_chroma_compatible(...)` +
`available_actions(...)` verbatim, purely to compute `len(menu)` as a `top_k` bound. Nothing enforced
that copy staying in sync with `propose.py`'s own construction — a future change to either one (a new
keyword argument, a different `psf_fwhm_arcsec` expression) would make `len(menu)` under-count, and
`propose_actions` would silently truncate the ranking it returns. That is not hypothetical: the §3.3
bounded-retry change exists specifically because the gap-closing action (`local_contrast@2"`) sits at
rank 5 on the measured fixture, past where a naive `top_k=3` would ever look.

**Fix:** `propose_actions` (`src/autocontrast/optimize/propose.py:109-186`) now takes
`top_k: int | None`, where `None` is an explicit, documented contract meaning "the whole ranking, no
truncation" — the single loop `for gk in ordered_groups: if top_k is not None and len(proposed) >= top_k:
break` is the only change to its body. `_ranked_menu` (`src/autocontrast/optimize/loop.py:110-125`) no
longer imports or calls `available_actions`/`palette_chroma_compatible` at all; it is now a pure
one-line delegation: `return propose_actions(reference_fp, parent_fp, applied_kinds=applied_kinds,
n_scales=n_scales, top_k=None)`. `propose.py` is now the only place in the codebase that constructs the
menu — confirmed by `grep -rn "propose_actions(" src/`, which shows exactly one call site
(`loop.py:123`).

**Test-driven, per the round's instructions.** Before touching `loop.py`, I added
`test_ranked_menu_has_no_menu_construction_of_its_own` (`tests/test_optimize_loop.py`, after the resume
tests) and ran it against the still-duplicated code to confirm it fails for the right reason:

```
$ .venv/bin/python -m pytest tests/test_optimize_loop.py -k "ranked_menu" -v
...
tests/test_optimize_loop.py::test_ranked_menu_has_no_menu_construction_of_its_own FAILED [ 50%]
tests/test_optimize_loop.py::test_ranked_menu_matches_an_explicit_top_k_none_call PASSED [100%]
...
>       assert len(via_wrapper) > 0
E       assert 0 > 0
E        +  where 0 = len([])
```

The test patches `loop.py`'s *own* binding of `available_actions` (via `monkeypatch.setattr(loop_mod,
"available_actions", lambda *a, **k: [], raising=False)`) — the exact shape a reintroduced duplicate
would take — while leaving `propose.py`'s separate binding untouched. Against the old code this starves
`_ranked_menu`'s private menu reconstruction (`menu = available_actions(...)` → `[]` →
`top_k=len(menu)=0`), so it returns nothing, while a direct call to `propose_actions(..., top_k=None)`
(unaffected, since it uses `propose.py`'s own unpatched import) still returns the full ranking. The
mismatch is the failure the finding describes, reproduced mechanically rather than asserted by
inspection. `raising=False` is there so the same test is meaningful post-fix, where `loop.py` no longer
has an `available_actions` name to patch at all.

After the fix, both this test and its sibling
(`test_ranked_menu_matches_an_explicit_top_k_none_call`, asserting `_ranked_menu(...) ==
propose_actions(..., top_k=None)` outright) pass. A third test,
`test_top_k_none_returns_the_whole_ranking_untruncated` (`tests/test_optimize_propose.py`), pins the new
contract at its source: `top_k=None` equals a deliberately oversized `top_k=1000`, and a bounded call
(`top_k=3`) is a strict prefix of the unbounded one — so `top_k=None` cannot be a second, separately
computed ordering that merely happens to agree.

### Finding 2 (Minor) — the `# checkpoint (§6.1)` comment overstated durability

`loop.py:290` (old numbering) labeled the in-memory `session.best = best_candidate` assignment a
"checkpoint," which reads as if that line persists anything. It does not — `Session.save(...)` is what
provides durability (spec §2.1: "a crashed run leaves an inspectable session file"), and it is called by
the sidecar (Task 11) between loop calls, not by the loop. Per the round's explicit instruction, no
`session.save()` call was added here — that boundary is deliberate and belongs to Task 11. The comment
now reads:

```python
session.best = best_candidate  # in-memory only; durability is Session.save (§2.1),
# called by the sidecar (Task 11) between loop calls, not by the loop itself
```

### Verification

```
$ .venv/bin/python -m pytest tests/test_optimize_loop.py tests/test_optimize_propose.py -v
...
37 passed in 33.43s
```

23 tests in `test_optimize_loop.py` (21 pre-existing + 2 new), 14 in `test_optimize_propose.py` (13
pre-existing + 1 new). All pass; no warnings.

```
$ .venv/bin/python -m pytest -q
........................................................................ [ 18%]
........................................................................ [ 36%]
........................................................................ [ 55%]
........................................................................ [ 73%]
........................................................................ [ 92%]
..............................                                           [100%]
390 passed in 64.65s (0:01:04)
```

390 passed (387 → 390 is the three new tests). Nothing skipped, deselected, or xfailed — confirmed with
`pytest -q -rs`, which reports the same count and no `s`/`x` lines. No warnings in either run.

### Files changed

- `src/autocontrast/optimize/propose.py` — `top_k: int | None`, docstring contract for `None`
- `src/autocontrast/optimize/loop.py` — `_ranked_menu` reduced to a delegation; dropped now-unused
  `available_actions`/`palette_chroma_compatible` imports; corrected the `# checkpoint` comment
- `tests/test_optimize_loop.py` — 2 new tests (`test_ranked_menu_has_no_menu_construction_of_its_own`,
  `test_ranked_menu_matches_an_explicit_top_k_none_call`)
- `tests/test_optimize_propose.py` — 1 new test (`test_top_k_none_returns_the_whole_ranking_untruncated`)

### Explicitly not touched, per scope

No `session.save()` call added anywhere in `loop.py`. No tunable (`max_attempts`, `top_k`,
`epsilon_improve`, `epsilon`, `iteration_cap`) changed. No edits to `beam.py` or the spec document — the
budget-arithmetic question the review raised (Minor/Important #3 in the original review) is unresolved
and out of scope for this round.

Commit: `df577e7` on `phase2-optimizer`.
