### Task 14 (v2): The §10 exit criterion, on real data

> **SUPERSEDES `task-14-brief.md`.** Its `test_declines_on_a_cached_professional_render`
> optimizes `sorted(glob("data/discovery_cache/*.jpg"))[0]` — which is `eso0104a.jpg` —
> against **`eso1103a`'s** fingerprint. Two different objects. Its own docstring claims
> "a professional render measured against its own fingerprint," which it is not. Read
> v1 for its structure; the corrections below govern.

**Files:** Create `tests/test_optimize_exit_criterion_live.py`

**This task decides whether Phase 2 is done.** All assertions must hold with ONE set of
thresholds.

---

## Verified prerequisites (controller checked on disk — do not re-derive)

- `/mnt/qnap/astro_data/prints/M42_hoo_2_18_23.jpg` EXISTS (30 MB, Feb 2023). This is
  the user's own finished HOO M42.
- `data/fingerprints.sqlite` holds **`eso1103a` only**. `esa_hubble:opo9545a1` is **NOT**
  in it — the Phase 1 run that ingested it used a fresh store. Your fixture will fall
  through to `eso1103a`. Do not ingest anything to "fix" this.
- `data/discovery_cache/` holds 35 real `.jpg` renders, named `<record_id>.jpg`.
  `provenance` has **no** `cache_file` key.
- Task 12 (`tests/test_optimize_offline_real.py`) already derives the cached path from
  `discover.py`'s own `{entry_id}{suffix}` convention. **Reuse that**, do not write a
  second derivation.

## Correction 1 — measure a render against ITS OWN fingerprint

Any "a professional render is declined" test must use the reference's own cached file,
`data/discovery_cache/{rid}.jpg` for the `rid` your fixture resolved. Optimizing an
unrelated render against a different target's fingerprint is not a fail-safe test: two
unrelated objects are genuinely far apart, so *finding improvements would be correct*,
and the assertion would pass or fail for reasons that say nothing about §3.2.

## Correction 2 — state the palette situation, do not paper over it

`eso1103a` is **broadband RGB**; the user's print is **HOO**. So for the
print-vs-reference tests the palette gate drops chroma (§2.3) and the comparison runs
on **structure and tone only**. That is correct behavior, but it must be visible in the
test — a reader must not mistake a green suite for chroma coverage.

Task 12 recorded the same gap: the chroma-drop path is not exercised anywhere, because
its tests compare `eso1103a` against its own fingerprint so the palette always matches.
Your print-vs-`eso1103a` tests DO cross palettes. Say explicitly, in the module
docstring, which of your tests gate chroma and which do not.

## Correction 3 — assert on the convergence reason, not just the outcome

Task 12 found the flattened-recovery run converged via
`"every branch was discarded by guardrails or executor failure"` — §6.3 condition 3,
the **exhaustion** path — while `distance_history` was still falling monotonically at
the final step (`0.08794 → 0.08643`). It stopped because everything was rejected, while
still improving.

Consequence you must handle: a bare `improved is True` assertion passes on a run that
died by exhaustion, and the measured best is then a **floor** on the loop's capability,
not a measure of it. So:
- Record `convergence_reason` and `iterations` for every run, in the failure message.
- Assert nothing that would silently accept exhaustion as a healthy convergence without
  reporting it. If a run terminates by exhaustion, the test may still pass — but the
  report must say so.

## Correction 4 — the binding tunable is `max_attempts`, NOT `iteration_cap`

Task 12's reviewer re-instrumented the recovery run standalone and identified the
mechanism precisely. **Do not repeat the misattribution in Task 12's report, which
blames `iteration_cap`:**

- The ranked menu for that fixture holds **18 distinct `(priority, band, kind)` groups**.
  `max_attempts = 3 × top_k = 9`, so **each branch attempts only half the menu** before
  giving up.
- `applied_kinds` was NOT the cause — it withholds only ONCE_ONLY kinds (`star_split`,
  `background_neutralize`), neither of which was used. The full 18-group menu was
  available, unshrunk, every iteration.
- At iteration 7 all three branches hit the cap holding 0 of 3 live candidates, with
  guardrail failures (noise floor, star integrity, shadow clipping, highlight clipping)
  escalating each iteration. Real guardrail pressure — but 9 of 18 groups per branch
  were never attempted.
- `iteration_cap = 20` was never binding (7 and 1 iterations).

So the measured best is a floor set by the **bounded retry**, not evidence of the best
achievable recipe.

Report, for each run: iterations used, the convergence reason, whether distance was
still falling when it stopped, and — when a run ends by exhaustion — how many menu
groups went unattempted.

**Do NOT adjust `max_attempts`, `iteration_cap`, or anything else.** §3.7 forbids
adjusting a tunable to rescue a test without re-running all of them, and this task IS
the run that would have to be re-run. Establish the criterion under the current tunables
first; whether the bound should change is a decision to take AFTER, on this evidence.

---

## The criterion (spec §10) — REFRAMED ON EVIDENCE (user ruling, 2026-07-28)

> "measurably improves a flat image, and — more importantly — fails safe on an
> already-well-processed image by declining to make it worse."

**The literal reading of "already-well-processed" as "the user's finished print" is a
FALSE PREMISE, and Task 13's live run proved it.** That premise assumes a finished print
sits at the valley floor. It does not: the user's print measures **D ≈ 0.23–0.25** from
`eso1103a` — a different instrument, different integration, different processing house.
A real headless PixInsight run improved it 4.3% (`D 0.2467 → 0.2361`) using two
conservative global operations, and Task 13's reviewer traced the fail-safe end to end
(`loop.py:89-97, 307, 703`) and confirmed it is **structurally intact**: baseline and
every candidate are measured through the same loader at the same `max_dim`, so
`improved` cannot be true unless a candidate is genuinely closer to the reference than
the input the search was given.

With D ≈ 0.24 of headroom, declining would have been the WRONG answer.

This is the same shape as the Phase 0 exit finding, which was correctly reframed on
evidence rather than tuned around. **Assert what the fail-safe actually guarantees:**

Required cases:
1. **Declines on an image AT the valley floor.** Take a cached professional render and
   measure it against **its OWN stored fingerprint**, where D ≈ 0 by construction.
   `improved is False`, and the recipe is empty. This is the important half, and it is
   the case that genuinely tests §3.2's structural fail-safe.
2. **Improves a deliberately flattened copy of that same render**
   (`eval.degrade.flatten(..., strength=0.6)`). `improved is True` and
   `distance < baseline_distance`. Same image, same reference, same thresholds — only
   input quality differs, which is what makes it a fair pair rather than two unrelated
   runs.
3. **The pair is decided by ONE threshold set.** The same config must decline the
   floor-case and improve the flat one. Passing the halves under different thresholds
   would defeat the criterion.
4. **Record, do not assert, the amateur-vs-professional gap.** Measure the user's
   finished HOO print against `eso1103a` and RECORD baseline D, best D, iterations,
   convergence reason, and the recipe. Do **not** assert `improved is False` on it.

   Write this into the report as an explicit **calibration finding for Phase 3**: is
   D ≈ 0.24 the right score for a competent amateur print against a professional render
   of the same object, or is it an artifact of normalization, palette gating, or
   comparing across instruments? That question is NOT Phase 2's to answer, and Phase 2
   does not fail for leaving it open — but it must be written down, with numbers, so
   Phase 3 inherits it rather than rediscovering it.

## What "improvement" means here — read before interpreting results

Task 12 measured the undegraded render's baseline at `0.0173`, well BELOW the flattened
run's best-achieved `0.0864`. The criterion is **measurable improvement**, not recovery
to the pristine floor. Do not write assertions implying the latter.

## If it fails

- Do **not** lower `epsilon_improve` to rescue the improve case, or raise it to rescue a
  decline case.
- If no single threshold set satisfies all four cases, **STOP and report it**. That is a
  finding about the fingerprint or the proposer — exactly like the Phase 0 exit finding,
  which was correctly reframed on evidence rather than tuned around. It is a legitimate
  and valuable outcome.

## Global constraints

- Branch `phase2-optimizer`. Do not commit to master. Do not push.
- No AI anywhere in this plan.
- Guardrail violations discard candidates; never a score term (§7).
- No test may be deselected to make the suite green; never `pytest.skip` a missing
  fixture — fail loudly naming what to run.
- **No tunable may be adjusted** (§3.7).
- Every degraded path logs and surfaces (§12).

## Verification

- `.venv/bin/python -m pytest tests/test_optimize_exit_criterion_live.py -v`
- `.venv/bin/python -m pytest -q` — full count, nothing deselected. **Baseline 425.**
  The suite takes ~4 min; these live tests will add to that. Report the real time.

## Report

Write to `.superpowers/sdd/2026-07-27-phase2-optimizer/task-14-report.md` AS YOU GO.
Record, for every run: baseline distance, best distance, iterations, convergence reason,
the recipe, and whether distance was still falling at the end.

Reply with only: status, commit SHA, one-line test summary, the measured numbers, and
concerns.
