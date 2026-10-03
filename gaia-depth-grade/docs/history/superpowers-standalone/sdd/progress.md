# Gaia Depth Grade — Phase 1 progress ledger

Branch: feature/phase1-star-depth
Plan: docs/superpowers/plans/2026-06-23-gaia-depth-grade-phase1.md
Branch base: d4a646972b89f355d13540e0b3eb3963506fb0d7

## Tasks
(Task N: complete (commits <base7>..<head7>, review clean) — appended as finished)
Task 1: complete (commits d4a6469..ad675e0, review clean)
  Minor (final-review triage): _apply uses instance not class for fields(); _apply return value unused.
Task 2: complete (commits ad675e0..767d47e, review clean)
  Minor (final-review triage): unused import SkyCoord in wcs.py.
Task 3: complete (commits 767d47e..65ea7d0, review clean; photutils shim judged warranted)
  Minor (final-review triage): unused `mean` from sigma_clipped_stats in detect.py:48.
Task 4: complete (commits 65ea7d0..47e8c8d, review clean; loud-error contract verified)
  Minor (final-review triage): no test for TAP-failure->RuntimeError branch (coverage gap on a load-bearing constraint); plan Step4 said "5 passed" but 4 tests; unused numpy import in test; sha1 usedforsecurity.
Task 5: complete (commits 47e8c8d..97292f3, review clean after plan-contradiction resolution)
  DECISION: reviewer "Needs fixes" was a plan prose/code self-contradiction (neutral_strength_distance param).
    Resolved by keeping code (param is dead scope; neutral handled in transform) and fixing plan text. No code change.
  Minor (final-review triage): dead conditional `0.0 if n else 0.0` (match.py:43); no test for empty-detections or two-catalog-sources-competing-for-one-detection.
Task 6: complete (commits 78e7e7a..8d2ff7c, review clean after fix)
  IMPORTANT (fixed): degenerate distance range returned +1.0 (whole-field foreground boost) instead of neutral; brief's attenuation test used identical distances and self-contradicted. Fixed branch to float(neutral), corrected test to distinct distances, added test_degenerate_equal_distances_are_neutral. Plan text updated.
Task 7: complete (commits 8d2ff7c..759d7ce, review clean)
  Minor (final-review triage): Modulation frozen dataclass holds mutable ndarrays (frozen blocks reassignment, not content mutation) — informational.
Task 8: complete (commits 56f5408..b14d8b1, review clean after 2 plan fixes)
  PLAN FIXES (both root-cause, not workarounds): (1) size/glow halo area-normalized -> peak-normalized so size visibly widens footprint; (2) test footprint metric relative max*0.5 -> fixed 0.1 threshold (relative threshold masked widening). Render math was correct; metric was the bug.
  Minor (final-review triage): no-mutation guarantee tested only via np.shares_memory in 1 of 4 tests (no value-equality assertion).
Task 9: complete (commits b14d8b1..2e18159, review clean after fix)
  IMPORTANT (fixed): DEPTHTAG truncated to 68 chars (HONESTY[:68]) dropped "gas", violating verbatim-honesty-tag constraint; test's OR assertion hid it. Fixed: store full HONESTY (astropy CONTINUE, verified round-trip), test asserts DEPTHTAG==HONESTY independently. Plan text updated.
  Low (final-review triage): `debug` subcommand runs identical logic to `grade` (brief underspecified debug behavior).
Task 10: complete (commits 2e18159..HEAD; PJSR has no CI — manual harness validation)
  Fixed PixelMath recombine: combine(a,b,op_screen) (invalid) -> screen formula ~((~A)*(~B)).
  MANUAL-VERIFY (final-review / first harness run): SXT headless scriptability; ImageSolver.SolveImage usage; ImageWindow.saveAs 5-bool signature; StarXTerminator property names (unscreen). These cannot be tested in CI.
FINAL REVIEW: complete (review-package d4a6469..1597319). Verdict: merge with fixes (no Critical).
  Important fixes applied (commit f310c85): debug subcommand now fails loudly (no silent grade); added test_tap_failure_raises_runtimeerror (loud-error coverage); strengthened render no-mutation assertion.
  Minor fixes applied: dropped dead conditional in match; removed unused SkyCoord import; _,median,std in detect; sha1 usedforsecurity=False.
  PROD-READINESS fix (commit after f310c85): package was not installed in venv -> `python -m gaia_depth_grade.cli` (used by PJSR) failed; added `uv pip install -e .` to setup+Task1+README. Verified module entrypoint runs.
  REMAINING MANUAL-VERIFY (first live PI harness run, cannot be CI-tested): StarXTerminator headless + _stars window naming + unscreen property; ImageSolver.SolveImage; ImageWindow.saveAs signature; screen-blend PixelMath.
