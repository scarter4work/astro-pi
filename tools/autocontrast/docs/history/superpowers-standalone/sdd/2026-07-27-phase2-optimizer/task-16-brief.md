### Task 16: Fingerprint extraction cost — and the spec's budget reasoning

**Files:**
- Modify: `src/autocontrast/fingerprint/` (whichever modules profiling implicates)
- Modify: `src/autocontrast/optimize/loop.py` (redundant parent re-measure)
- Modify: `docs/superpowers/specs/2026-07-27-phase2-optimizer-design.md` §3.3
- Modify: `src/autocontrast/optimize/beam.py` (the budget rationale comment)
- Test: `tests/test_fingerprint_cost.py` (new), plus equivalence proof

---

## Why this task exists

A reviewer found that spec §3.3 (lines 181-195) **rejects** the unbounded guardrail
retry because ~127 min exceeds a stated 15-minute budget, then **adopts**
`max_attempts = 3 × top_k`, whose own table row four lines later says ~23 min worst
case — also over budget, by ~50%. The same reasoning is reproduced at
`beam.py:33-38`. The argument does not reach its own conclusion.

**User ruling: attack the cost driver, keep `max_attempts = 3 × top_k`.**

The rationale for not simply tightening the cap: narrowing the retry would
reintroduce the exact failure §3.3 was written to prevent. On the measured flattened
fixture, `local_contrast@2"` — the action that actually closes the gap — ranks
**fifth**. A tighter cap stops the branch before it gets there, and the loop declines
on an image it could plainly have fixed.

Measured cost at the real 1600px proxy: **2.54s per candidate** — executor 0.18s,
guardrails 0.83s, **fingerprint extraction 1.53s**. Fingerprinting dominates.

---

## THE HARD CONSTRAINT — read this before writing any code

**This is an optimization. It must be behavior-preserving.**

Every threshold in this project is calibrated against fingerprint values: the
magnitude buckets (0.075/0.15, tertiles of 227 measured gaps), `epsilon_improve`,
`epsilon`, the guardrail limits, and the §10 exit criterion itself. **If a
fingerprint shifts even slightly, all of that silently becomes wrong.**

So:
- Fingerprint equivalence before/after must be **PROVEN ON REAL DATA**, not asserted
  and not argued. Write a test that extracts fingerprints from real cached renders
  under `data/discovery_cache/` with the old and new code paths and asserts equality.
  Exact equality is the target. If some change can only achieve near-equality, STOP
  and report it rather than choosing a tolerance yourself — picking a tolerance here
  is a calibration decision, and spec §3.7 makes it the user's.
- Do **not** change the metric, the wavelet scale convention (plane `i` has angular
  center `2**i * pixel_scale_arcsec`), the band-limiting, the palette gate, or the
  distance function. This task makes existing computation cheaper. It does not make
  it different.
- Do **not** adjust any tunable.

---

## Step 1: MEASURE FIRST. Do not optimize on a guess.

Profile `fingerprint.extract` on a real 1600px render from `data/discovery_cache/`.
Report where the 1.53s actually goes, with numbers, before changing anything.

Two candidate wins were identified by the controller. **Both are hypotheses to
confirm or refute — neither is a directive:**

**(a) Redundant parent re-measure.** `loop.advance()` calls `_measure(parent, session)`
for every live branch every iteration (`loop.py` ~line 176). But that parent was a
*candidate* in the previous iteration and was already fingerprinted then, at
`loop.py` ~line 249, to score it. That is one full extraction per branch per
iteration, recomputed from scratch. A `Branch` could carry its fingerprint forward.
Note the wrinkle: `Branch` is serialized by `session.py`, so caching a fingerprint on
it widens the session record — measure whether the saving is worth that before doing
it, and check the round-trip still holds.

**(b) Two starlet transforms of the same image.** The guardrails (0.83s) compute an
MRS-noise starlet transform, and `extract` (1.53s) computes a starlet transform of
the *same* image. If they are the same decomposition, one could serve both. Verify
they genuinely are the same before sharing — different `n_scales`, different
normalization, or a different boundary mode would make this a correctness bug rather
than an optimization.

If profiling shows the real cost is somewhere else entirely, follow the measurement,
not this list. Say so in your report.

---

## Step 2: The spec fix

Once the post-optimization numbers are measured, correct spec §3.3 (lines 181-195)
and the mirrored rationale at `beam.py:33-38` so the argument reaches its own
conclusion: state the real per-candidate cost, the real worst-case iteration cost
under `max_attempts = 3 × top_k`, and a budget those numbers actually satisfy. If
after optimization the worst case still exceeds a defensible budget, **say so plainly
in the spec and in your report** — do not pick a budget that happens to fit, and do
not narrow the cap to make it fit. An honest "this is what it costs" is the required
outcome; a tidy one that misstates the numbers is not.

---

## Verification

- The equivalence test above, on real cached renders.
- A before/after timing measurement, reported with real numbers.
- `.venv/bin/python -m pytest -q` — full suite, report the count, confirm nothing
  deselected. Every existing test must pass **unchanged**: if optimizing requires
  editing an existing assertion, that is evidence the behavior moved, and it is a
  finding to report, not a test to adjust.

Test output must be pristine; warnings are findings.

## Global constraints

- Branch `phase2-optimizer`. Do not commit to master.
- No AI anywhere in this plan.
- Guardrail violations discard candidates; never a score term (§7).
- No test may be deselected to make the suite green.
- No tunable may be adjusted (spec §3.7).
- Every degraded path logs and surfaces (§12). No silent fallbacks.
- Wavelet scale convention unchanged: plane `i` has angular center
  `2**i * pixel_scale_arcsec`.

## Report

Append to `.superpowers/sdd/2026-07-27-phase2-optimizer/task-16-report.md`.
Reply with only: status, commit SHA(s), one-line test summary, the before/after
timing, and concerns.
