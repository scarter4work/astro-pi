# Task 2 Report: Guardrails — noise floor and clipping

## What was implemented

`src/autocontrast/optimize/guardrails.py`, exactly as specified in the brief:

- `GuardrailLimits` — frozen dataclass of trip thresholds (`noise_growth`,
  `shadow_clip_fraction`, `highlight_clip_fraction`, plus the still-unused
  `star_count_drop`, `star_fwhm_growth`, `star_ecc_growth`, `hue_invention_mass`,
  `channel_ratio_drift` fields reserved for Tasks 3/4 — declared but not consumed
  here, per the brief).
- `GuardrailVerdict` — frozen dataclass (`name`, `ok`, `reason`, `value`, `limit`).
- `mrs_noise_sigma(gray)` — MAD-based noise sigma from the finest starlet plane
  (`starlet_transform(gray, n_scales=1)`), scaled by 1.4826 (MAD→Gaussian sigma).
- `clipped_fraction(rgb, at)` — per-channel fraction of pixels exactly equal to
  `at`.
- `check_noise_floor(candidate, baseline, limits)` — trips when candidate's
  `mrs_noise_sigma` grows more than `limits.noise_growth` fractionally over the
  baseline's.
- `check_shadow_clipping` / `check_highlight_clipping` — trip when the worst
  per-channel fraction of pixels at 0.0 / 1.0 exceeds the configured limit.

All five functions/dataclasses are pure functions of pixel arrays — no
PixInsight dependency, per the global constraint. No aggregate `evaluate_guardrails`
was added (reserved for Task 4). No star-integrity, hue-invention, or
channel-ratio-drift checks were added (reserved for Tasks 3/4).

Test file `tests/test_optimize_guardrails.py` — copied verbatim from the brief.

## Test hygiene check (both directions)

Reviewed each guardrail test against the "assert both trip and pass" requirement
from Task 1's review lesson:

- `test_noise_floor_trips_when_noise_balloons`: asserts `check_noise_floor(baseline, baseline, ...).ok`
  (passes on good input) **and** `not check_noise_floor(noisy, baseline, ...).ok`
  (trips on bad input). Both directions present.
- `test_shadow_and_highlight_clipping_trip_independently`: asserts `clean` passes
  both shadow and highlight checks; `crushed` trips shadow but still passes
  highlight; `blown` trips highlight but still passes shadow. Both directions
  present for both guardrails, and independence (one tripping doesn't trip the
  other) is explicitly covered.

No additions were needed — the brief's tests already satisfy the both-directions
requirement for every guardrail.

## Commands run and actual output

### Step 2 — confirm failing for the stated reason

```
$ .venv/bin/python -m pytest tests/test_optimize_guardrails.py -v
============================= test session starts ==============================
platform linux -- Python 3.14.6, pytest-9.1.1, pluggy-1.6.0
collected 0 items / 1 error
==================================== ERRORS ====================================
______________ ERROR collecting tests/test_optimize_guardrails.py ______________
ImportError while importing test module '.../tests/test_optimize_guardrails.py'.
...
E   ModuleNotFoundError: No module named 'autocontrast.optimize.guardrails'
=========================== short test summary info ============================
ERROR tests/test_optimize_guardrails.py
=============================== 1 error in 0.09s ===============================
```

Matches the brief's expected failure exactly.

### Step 4 — confirm passing

```
$ .venv/bin/python -m pytest tests/test_optimize_guardrails.py -v
============================= test session starts ==============================
platform linux -- Python 3.14.6, pytest-9.1.1, pluggy-1.6.0
collected 5 items

tests/test_optimize_guardrails.py::test_mrs_noise_sigma_rises_with_injected_noise PASSED [ 20%]
tests/test_optimize_guardrails.py::test_noise_floor_trips_when_noise_balloons PASSED [ 40%]
tests/test_optimize_guardrails.py::test_clipped_fraction_counts_per_channel PASSED [ 60%]
tests/test_optimize_guardrails.py::test_shadow_and_highlight_clipping_trip_independently PASSED [ 80%]
tests/test_optimize_guardrails.py::test_verdict_carries_value_and_limit_for_reporting PASSED [100%]

============================== 5 passed in 0.14s ===============================
```

All 5 tests passed on the first implementation attempt — no thresholds or
fixtures required adjustment.

Additionally ran the guardrails tests together with Task 1's action-space tests
to confirm no interaction/regression:

```
$ .venv/bin/python -m pytest tests/test_optimize_actions.py tests/test_optimize_guardrails.py -v
...
============================== 14 passed in 0.13s ===============================
```

## Deviations from the brief

None. Implementation, test file, and commit message were used verbatim from the
brief. No numeric assertion needed recalibration — all synthetic-fixture
thresholds in the brief held as written against the real `starlet_transform`
implementation.

## Self-review

- Guardrails return only a `GuardrailVerdict` (pass/fail + diagnostic value/limit/reason)
  — never a numeric penalty folded into a score. Confirmed by reading back the
  three `check_*` functions: each computes an `ok` boolean and returns early
  logic based on it; nothing sums or weights guardrail outputs against a score.
- All three checks are pure functions of `np.ndarray` pixel data; the only
  external dependency is `autocontrast.fingerprint.starlet.starlet_transform`,
  which is itself pixel-only (no PI calls, verified by reading its source).
- No new dependencies — only `numpy`, `scipy` (transitively via `starlet_transform`),
  and the standard library `dataclasses`.
- `reason` is `""` on an `ok` verdict, and a descriptive string with measured vs.
  limit values otherwise — satisfies the "degraded path logs and surfaces, never
  silent" constraint since callers (Task 4's aggregator, later) will have a
  concrete string to log rather than needing to reconstruct one.
- Left `GuardrailLimits`' star/hue/channel-ratio fields declared-but-unused,
  matching the brief's interface list; did not implement or call them here.
- Committed only the two files named in the brief (`git add` was scoped
  explicitly, not `-A`).

## Fix round 1: noise guardrail fails open on degenerate baseline

**Finding (team-lead review, Important):** `check_noise_floor` (guardrails.py:75,
as originally committed) computed `growth = 0.0 if base <= 0 else (cand - base) /
base`. When the baseline's `mrs_noise_sigma` was non-positive (e.g. a perfectly
flat/constant baseline, whose finest starlet plane is all zeros), `growth` was
forced to `0.0`, which reads as "no growth" and passes the guardrail. This fails
*open*: on a degenerate baseline the guardrail silently disables itself and lets
any amount of candidate noise through, while still reporting that noise was
checked — violating the "no silent fallbacks" global constraint (design §12).

**Fix:** `check_noise_floor` now checks `base <= 0` explicitly and returns
`ok=False` with `value=float("inf")`, `limit=limits.noise_growth`, and a `reason`
string naming the non-positive baseline sigma (e.g. `"baseline noise sigma was
non-positive (0.0); noise growth could not be assessed"`), instead of computing
a growth ratio at all. No exception is raised — this stays a per-candidate
verdict so the optimizer discards this one candidate rather than aborting the
run.

**Test added:** `test_noise_floor_fails_closed_on_degenerate_baseline` in
`tests/test_optimize_guardrails.py`, using a perfectly flat baseline
(`np.full((128, 128, 3), 0.5)`). Measured directly in the test body — asserted
`mrs_noise_sigma(flat[..., 0]) == 0.0` — to confirm the fixture genuinely
exercises the `base <= 0` branch rather than merely asserting on it. This
measured value matched a standalone script run beforehand:

```
$ .venv/bin/python -c "
import numpy as np
from autocontrast.optimize.guardrails import mrs_noise_sigma, check_noise_floor, GuardrailLimits
flat = np.full((128,128), 0.5)
print('flat sigma:', mrs_noise_sigma(flat))
flat_rgb = np.stack([flat]*3, axis=-1)
RNG = np.random.default_rng(20260727)
noisy_rgb = np.clip(flat_rgb + RNG.normal(0, 0.05, flat_rgb.shape), 0, 1)
print(check_noise_floor(noisy_rgb, flat_rgb, GuardrailLimits()))
"
flat sigma: 0.0
GuardrailVerdict(name='noise_floor', ok=False, reason='baseline noise sigma was non-positive (0.0); noise growth could not be assessed', value=inf, limit=0.15)
```

**Command run (Step: re-run full test file):**

```
$ .venv/bin/python -m pytest tests/test_optimize_guardrails.py -v
============================= test session starts ==============================
platform linux -- Python 3.14.6, pytest-9.1.1, pluggy-1.6.0
collected 6 items

tests/test_optimize_guardrails.py::test_mrs_noise_sigma_rises_with_injected_noise PASSED [ 16%]
tests/test_optimize_guardrails.py::test_noise_floor_trips_when_noise_balloons PASSED [ 33%]
tests/test_optimize_guardrails.py::test_noise_floor_fails_closed_on_degenerate_baseline PASSED [ 50%]
tests/test_optimize_guardrails.py::test_clipped_fraction_counts_per_channel PASSED [ 66%]
tests/test_optimize_guardrails.py::test_shadow_and_highlight_clipping_trip_independently PASSED [ 83%]
tests/test_optimize_guardrails.py::test_verdict_carries_value_and_limit_for_reporting PASSED [100%]

============================== 6 passed in 0.13s ===============================
```

**Commit:** `0088758` — "optimize: fail closed on degenerate noise-floor
baseline" (2 files changed: `src/autocontrast/optimize/guardrails.py`,
`tests/test_optimize_guardrails.py`).

**Scope:** No other changes made. The two "not findings" from the review
(`_gray`'s unweighted channel mean, and exact float equality in
`clipped_fraction`) were left untouched, as instructed.
