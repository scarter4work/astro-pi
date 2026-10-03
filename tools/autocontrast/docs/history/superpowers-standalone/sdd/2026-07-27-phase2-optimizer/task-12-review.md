# Task 12 review — offline end-to-end over real reference fingerprints

Base: 8d8f727  Head: 4bc020c  File: `tests/test_optimize_offline_real.py` (new, 140 lines)

## Method

Read the brief, the report, and the diff. Verified the three controller corrections against the
diff text directly. Cross-checked the cache-naming claim against `src/autocontrast/db/discover/discover.py`
(`_cached_image`, line 263-270: `cache_dir / f"{entry_id}{suffix}"`, called with `entry.id` at
line 332) — confirmed, and `data/discovery_cache/eso1103a.jpg` exists.

For the named exhaustion risk, re-ran the recovery scenario standalone (outside pytest) with
per-iteration instrumentation over `session.guardrail_log`, and separately computed
`available_actions(...)` for the actual pixel_scale/psf_fwhm/n_scales this fixture uses, to get an
exact menu size rather than the report's "14-22" estimate. Also checked `test_optimize_loop.py` to
see whether spec §7.2's third bullet ("guardrail-hostile input → must terminate via condition 3")
is already covered elsewhere.

## Spec Compliance

✅ §7.2 bullets 1-2 (flatten→reduce D, pro render→decline) — both real-data tests match design intent.
✅ §7.2 bullet 3 (condition-3 termination "and say so") — already unit-tested synthetically in
`tests/test_optimize_loop.py:210-235` (`test_exhausting_every_branch_is_not_dressed_up_as_success`).
Task 12's brief correctly does not duplicate that; it happens to observe condition 3 on real data
as a side effect, which is a bonus, not a gap.
✅ §7.4 — module docstring (`tests/test_optimize_offline_real.py:1-20`) and both test docstrings
state the "proves the loop, not the beauty" distinction explicitly.
✅ §3.7 — no tunable touched. Diff is additive-only, one new file; confirmed no other file appears
in `review-8d8f727..4bc020c.diff`.
✅ §7/§12 — no guardrail violation is scored, only discarded (unchanged code); every degraded path
(missing store record, missing cached raster) fails loudly via `pytest.fail`, never `pytest.skip`.
✅ Controller correction 1 (`provenance` has no `cache_file`) — honored; `_cached_raster_path`
derives the path from `rid` via `discover.py`'s own convention, not from provenance.
✅ Controller correction 2 (`esa_hubble:opo9545a1` absent) — `_reference()` tries it second and
falls through; report and diff both show resolution lands on `eso1103a`.
✅ Controller correction 3 (decline test must use the SAME rid's own render) — `test_a_real_professional_render_is_declined`
calls `_cached_raster_path(rid)` with the fixture's own `rid`, never a `sorted(glob(...))[0]` pick
from an unrelated file. Docstring states the reasoning correctly.
✅ Palette-gate coverage gap — documented in the module docstring (lines ~15-19), not only in the report.

## The Exhaustion Question

Confirmed and worth stating plainly: **the recovery run terminates by exhaustion (condition 3)
while distance is still monotonically falling** (0.08794 → 0.08643 at the last successful
iteration), and the measured best (0.08643) is a floor on what this fixture shows, not a ceiling
on what the loop can do.

Re-instrumented the run to see *why*, since "exhaustion" is ambiguous between "the menu ran dry"
and "the bounded retry (§3.3) stopped trying before the menu ran dry." It's squarely the latter:

- The ranked menu (`propose_actions` with `top_k=None`) for this fixture's geometry
  (`pixel_scale_arcsec≈1.775`, `psf_fwhm_arcsec=2.0`, `n_scales=7`) groups down to **18 distinct
  (priority, band, kind) groups** out of 50 raw actions (6 bands × {local_contrast, local_equalize}
  = 12, plus tonal_reshape/black_point/core_hdr/chroma = 4, plus background_neutralize/star_split = 2).
- `max_attempts = 3 × top_k = 9`. Every branch stops after 9 attempts — **half the ranked menu is
  never tried**, by design (§3.3's own bound), not because it ran out of options.
- `applied_kinds` is **not** the cause: it only withholds `ONCE_ONLY` kinds (`star_split`,
  `background_neutralize`), neither of which this recipe ever used. The full 18-group ranked menu
  was available, unshrunk, every iteration.
- Per-iteration guardrail failures escalate as the recipe accumulates pushes: iteration 5 already
  shows `star_integrity: 12`; iteration 6 hits the attempt cap on all 3 branches holding 2/3 live
  candidates each (2 survive combined, beam still non-empty); iteration 7 hits the attempt cap on
  all 3 branches holding **0/3** each — `noise_floor: 12, star_integrity: 15, shadow_clipping: 18,
  highlight_clipping: 12` — every one of the ~9 highest-priority moves tried on every branch fails
  some guardrail, and the beam empties.

So: the cause is real guardrails (not `applied_kinds`), but the termination is bounded-retry
exhaustion of the *attempted* menu, not exhaustion of the *available* menu — 9 of 18 groups per
branch were never tried at the point the beam died. This doesn't mean those 9 remaining groups
would have survived (they're lower-priority for a reason, and the guardrail failure rate was
already climbing across every guardrail simultaneously, consistent with the image genuinely
approaching the guardrail boundary from several directions at once) — but it means the number
0.08643 documents "as far as a 9-attempt-per-branch search got," not "as far as this menu can go."

**Do the tests notice?** No, and that's a real weakness, though a scoped one. Neither
`test_flattening_a_real_render_is_recovered` nor the report's own "OPEN calibration questions"
section engages with this. The report discusses `iteration_cap=20` at length ("never approached...
stays OPEN") — but `iteration_cap` (20) is not what bound this run; `max_attempts` (9, i.e. §3.3's
bounded retry) is, and the report's framing points the reader at the wrong tunable. The test itself
asserts only `improved is True` and `distance < baseline_distance`, which is what the brief asked
for and is sufficient to prove the loop's core correctness (§7.4) — but it means a future change
that made the run decline by *exhaustion 2 iterations earlier*, at a worse distance, would pass this
test identically. That's not a defect in what was built (asserting on `convergence_reason` or
`iterations` wasn't requested and would risk coupling the test to today's specific guardrail/menu
interaction), but it is information Task 14 needs and didn't get from this report: **the live PI
test's "must measurably improve" exit criterion should be read as a lower bound set by a
9-attempts-per-branch search, not as evidence the optimizer found the best available recipe.**

## Strengths

- The cache-path derivation is traced to the actual writer (`discover.py:_cached_image`), not
  guessed — verified independently, matches.
- The controller's three corrections are all honored precisely, including the subtle one (decline
  test must self-compare) — with a docstring that states *why*, not just *what*.
- No skip anywhere; every missing-fixture path fails loudly with the Phase 1 remediation command,
  exactly as briefed.
- Real gap in offline palette-gate coverage is disclosed in the file itself, not just the report.
- Diff is minimal and additive — nothing else touched, so no tunable drift risk.

## Issues

### Critical (Must Fix)
None.

### Important (Should Fix)
- `tests/test_optimize_offline_real.py` (module docstring) and `task-12-report.md`: the
  convergence-by-exhaustion finding on the recovery run is real data the file's own author
  produced, but it's absorbed into "OPEN calibration questions" framed around the wrong tunable
  (`iteration_cap`, never binding here) rather than the one that actually bound the run
  (`max_attempts`/§3.3's bounded retry, which cut off 9 of 18 menu groups per branch while distance
  was still falling). Recommend a short addendum — either in this module's docstring or wherever
  Task 14's exit criterion gets written up — stating plainly: this fixture's 0.08643 is a floor set
  by the bounded retry, not evidence of the best achievable recipe. This doesn't require touching
  the tunable (§3.7 still applies) or the test assertions — just naming the mechanism correctly so
  Task 14 doesn't read "improved: true, converged" as "found the optimum."

### Minor (Nice to Have)
- None beyond the above.

## Assessment

**Task quality:** Approved
**Reasoning:** The test file is correct, honors every controller correction precisely, asserts real
behavior on real data with no stubbing or skipping, and stays within the scope the brief and spec
§7.2/§7.4 actually ask for. The one real gap is analytical, not code: the report mischaracterizes
which tunable bounded the recovery run's termination (pointing at `iteration_cap` when it was
`max_attempts`), which could mislead how Task 14's exit criterion gets interpreted. That's worth
fixing in the writeup, not the test, and doesn't block merging this task.
