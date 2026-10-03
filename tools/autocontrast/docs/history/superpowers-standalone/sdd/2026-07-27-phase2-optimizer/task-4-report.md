# Task 4 report: hue-invention and channel-ratio-drift guardrails

## What was implemented

Appended to `src/autocontrast/optimize/guardrails.py`:
- `_chroma_hist(rgb)` — converts RGB to Lab via `rgb_to_lab`, then calls
  `chroma_histogram(a, b, bins=CHROMA_BINS, extent=CHROMA_EXTENT)`.
- `check_hue_invention(candidate, source, limits) -> GuardrailVerdict` — sums
  candidate chroma mass landing in a*/b* histogram cells where the source's
  histogram is `<= 0.0`; fails if that mass exceeds `limits.hue_invention_mass`.
- `_channel_ratios(rgb)` — mean per-channel level normalized to sum 1.
- `check_channel_ratio_drift(candidate, source, limits) -> GuardrailVerdict` —
  L2 distance between candidate and source channel-ratio vectors; fails if it
  exceeds `limits.channel_ratio_drift`.
- `evaluate_guardrails(candidate, baseline, source, limits) -> list[GuardrailVerdict]`
  — runs all six checks (noise floor, star integrity, shadow clipping,
  highlight clipping, hue invention, channel ratio drift) and returns every
  verdict; does not short-circuit.

Appended to `tests/test_optimize_guardrails.py`: the four tests exactly as
given in the brief's Step 1 (imports, `test_hue_invention_passes_when_color_only_intensifies`,
`test_hue_invention_trips_on_color_with_no_source_support`,
`test_channel_ratio_drift_trips_when_ratios_move`,
`test_evaluate_guardrails_returns_every_verdict_not_just_the_first`).

## One deviation from the brief's literal text

`check_hue_invention`'s reason string, as given in the brief's Step 3, was:

```
f"{mass:.4f} chroma mass appeared in a*/b* cells with no support "
f"in the source (limit {limits.hue_invention_mass:.4f})"
```

This never contains the substring "hue", but the brief's own Step 1 test
requires `"hue" in verdict.reason.lower()`. That test failed on the passing
run (see below) purely on string content — `ok` was correctly `False` and
`mass` was correctly `1.0`. I changed only the message prefix to
`"hue invention: ..."`. No threshold, no comparison logic, no test assertion
was touched.

## Exact commands and real output

### Step 2 — confirm failing for the expected reason

```
$ .venv/bin/python -m pytest tests/test_optimize_guardrails.py -k "hue or ratio or evaluate" -v
```
```
ERROR collecting tests/test_optimize_guardrails.py
ImportError: cannot import name 'check_channel_ratio_drift' from 'autocontrast.optimize.guardrails'
=========================== short test summary info ============================
ERROR tests/test_optimize_guardrails.py
!!!!!!!!!!!!!!!!!!!! Interrupted: 1 error during collection !!!!!!!!!!!!!!!!!!!!
=============================== 1 error in 0.20s ===============================
```
(ImportError names `check_channel_ratio_drift` rather than `check_hue_invention`
because Python resolves the first-listed missing name in the import
statement — same root cause the brief predicted, correct failure mode.)

### First full run after implementation (before the wording fix)

```
$ .venv/bin/python -m pytest tests/test_optimize_guardrails.py -v
```
```
tests/test_optimize_guardrails.py::test_hue_invention_passes_when_color_only_intensifies PASSED
tests/test_optimize_guardrails.py::test_hue_invention_trips_on_color_with_no_source_support FAILED
tests/test_optimize_guardrails.py::test_channel_ratio_drift_trips_when_ratios_move PASSED
tests/test_optimize_guardrails.py::test_evaluate_guardrails_returns_every_verdict_not_just_the_first PASSED
...
FAILED tests/test_optimize_guardrails.py::test_hue_invention_trips_on_color_with_no_source_support
AssertionError: assert 'hue' in '1.0000 chroma mass appeared in a*/b* cells with no support in the source (limit 0.0010)'
========================= 1 failed, 13 passed in 0.16s ==========================
```

### Step 4 — after fixing only the reason-string wording

```
$ .venv/bin/python -m pytest tests/test_optimize_guardrails.py -v
```
```
tests/test_optimize_guardrails.py::test_mrs_noise_sigma_rises_with_injected_noise PASSED
tests/test_optimize_guardrails.py::test_noise_floor_trips_when_noise_balloons PASSED
tests/test_optimize_guardrails.py::test_noise_floor_fails_closed_on_degenerate_baseline PASSED
tests/test_optimize_guardrails.py::test_clipped_fraction_counts_per_channel PASSED
tests/test_optimize_guardrails.py::test_shadow_and_highlight_clipping_trip_independently PASSED
tests/test_optimize_guardrails.py::test_verdict_carries_value_and_limit_for_reporting PASSED
tests/test_optimize_guardrails.py::test_detect_stars_finds_the_planted_stars PASSED
tests/test_optimize_guardrails.py::test_star_integrity_passes_on_an_untouched_frame PASSED
tests/test_optimize_guardrails.py::test_star_integrity_trips_when_stars_are_destroyed PASSED
tests/test_optimize_guardrails.py::test_star_integrity_passes_but_says_so_when_baseline_is_starless PASSED
tests/test_optimize_guardrails.py::test_hue_invention_passes_when_color_only_intensifies PASSED
tests/test_optimize_guardrails.py::test_hue_invention_trips_on_color_with_no_source_support PASSED
tests/test_optimize_guardrails.py::test_channel_ratio_drift_trips_when_ratios_move PASSED
tests/test_optimize_guardrails.py::test_evaluate_guardrails_returns_every_verdict_not_just_the_first PASSED
============================== 14 passed in 0.16s ==============================
```

(14, not the brief's stated "12 tests" — that count was already stale: there
were 10 pre-existing tests, not 8, before this task's 4 were appended.)

## The flagged risk: did it materialize?

**No.** The dispatch message's specific concern was that
`check_hue_invention`'s binned comparison would count legitimate outward
spill from an existing color bias as "invented" mass, falsely tripping
`test_hue_invention_passes_when_color_only_intensifies`. That test passed on
the first implementation attempt, unmodified.

Measured directly (`_smooth_image()` with `source[..., 0] *= 1.2`, then
`lab_ish = clip((source - 0.5) * 1.3 + 0.5, 0, 1)`):

```
intensify: src empty bins 254 / 256
intensify: mass in src-empty bins 0.0
```

So even though the source occupies only ~2 of the 256 a*/b* histogram cells
(a nearly-monochromatic red bias), and 254 cells are empty, none of the
candidate's chroma mass after the 1.3x saturation boost landed outside the
already-occupied cells. The reason: `CHROMA_EXTENT=100` over `CHROMA_BINS=16`
gives each cell a width of 12.5 in a*/b* units, and this fixture's chroma
magnitude and boost factor are small enough that the boosted pixels' a*/b*
coordinates stay inside the same cell(s) the source already occupied — no
outward spill across a bin boundary occurs at this magnitude.

For contrast, the fabrication test (`test_hue_invention_trips_on_color_with_no_source_support`,
neutral source + green cast) was confirmed to genuinely exercise the
target condition:

```
src empty bins: 255 / 256
cand mass in src-empty bins: 1.0
cand total mass: 1.0
```

**Conclusion / recommendation:** with the brief's exact fixture and
`GuardrailLimits()` defaults, the risk is real in principle (binned
adjacency can misfire) but did not trigger here because the test's chosen
saturation boost keeps mass inside the source's bin. I did not implement the
dilated-support-mask alternative described in the dispatch, since the
literal brief passed as written and the risk didn't manifest — changing the
check now would be scope creep against an untriggered hypothetical. I'd
flag this as a known edge case worth a dedicated future test (a coarser
histogram grid or a larger boost factor that pushes mass across a bin edge
would trip it), but do not think it should be fixed speculatively without a
failing test proving the false positive.

## Fixture verification (test hygiene)

- `test_hue_invention_trips_on_color_with_no_source_support`: confirmed the
  neutral source really has an all-zero chroma histogram (255/256 bins
  read `<= 0.0`, the 256th is exactly the zero-chroma degenerate bin at
  a*=b*=0) and the green-cast candidate dumps its entire mass (1.0) into
  those empty cells.
- `test_channel_ratio_drift_trips_when_ratios_move`: confirmed numerically —
  source ratios `[0.3333, 0.3333, 0.3333]`, drifted ratios
  `[0.2859, 0.2859, 0.4281]`, L2 drift `0.116`, exceeding the default limit
  `0.10`.
- `test_evaluate_guardrails_returns_every_verdict_not_just_the_first`: the
  fixture crushes rows 0-30 to 0.0 and 30-60 to 1.0 on top of `_smooth_image()`,
  and the test asserts both `shadow_clipping` and `highlight_clipping` are
  present in the failed set simultaneously, which only happens if
  `evaluate_guardrails` ran every check rather than stopping at the first
  failure.

## Self-review

- Scope held to exactly `check_hue_invention`, `check_channel_ratio_drift`,
  `evaluate_guardrails`, plus the one reason-string wording fix needed to
  make the brief's own test pass (not a logic or threshold change).
- No new dependencies; numpy/scipy only, consistent with the rest of the
  module.
- `evaluate_guardrails` confirmed non-short-circuiting by direct test.
- Guardrail violations remain pass/fail + reason; no score terms introduced.
- Did not touch `check_noise_floor`, `check_star_integrity`,
  `check_shadow_clipping`, `check_highlight_clipping`, `GuardrailLimits`, or
  `GuardrailVerdict` — all consumed as-is per the brief.

## Commit

```
3d35367 optimize: hue-invention and channel-ratio guardrails
```
Branch: `phase2-optimizer`. Files: `src/autocontrast/optimize/guardrails.py`,
`tests/test_optimize_guardrails.py`.

---

## Fix round 1 (review response)

### Finding: the intensification test was vacuous

Reviewer + team-lead independently measured that `test_hue_invention_passes_when_color_only_intensifies`'s
source and candidate map to the identical two histogram bins `(8,8)` and `(9,8)`,
so mass in previously-empty bins is exactly `0.0` regardless of how the
support/boundary logic is implemented. The test could not have caught a
boundary-logic defect. Confirmed and agreed.

### What I did in this round

1. **Cosmetic (committed, `212c841`):** moved the hue-invention/channel-drift
   imports (`rgb_to_lab`, `CHROMA_BINS`, `CHROMA_EXTENT`, `chroma_histogram`)
   from mid-file into the top import blocks of both
   `src/autocontrast/optimize/guardrails.py` and
   `tests/test_optimize_guardrails.py`.
2. **Docstring (committed, `212c841`):** added a paragraph to
   `check_hue_invention`'s docstring stating explicitly that a neutral
   source tripping on *any* candidate chroma is intentional per SS2.1
   (no color in the data means no color to legitimately intensify), not an
   incidental side effect.
3. **Built the required boundary-crossing fixture, ran it, did NOT commit
   a resolution** — see below. Per the review's explicit instruction ("If
   `.ok` goes False: STOP... I will rule"), I stopped short of adding any
   test assertion or guardrail change that would lock in an answer to an
   open design question.

### The boundary-crossing fixture: exact measurements

`CHROMA_BINS=16`, `CHROMA_EXTENT=100.0` → bin width 12.5, with a bin edge at
`a*=12.5` (edges run ..., 0.0, 12.5, 25.0, ...).

Fixture: `_smooth_image()` (the existing spatially-varying sinusoidal
fixture, not a degenerate flat patch) with `source[..., 0] *= 1.18`, which
places the source's entire a* range at `[5.76, 11.56]` — a single occupied
histogram bin, `(8, 8)`, deliberately close to (0.94 units from) the 12.5
edge. The "legitimate saturation boost" is modeled as
`candidate = clip(gray + (source - gray) * boost, 0, 1)` where
`gray = source.mean(axis=-1, keepdims=True)` — a standard boost-about-the-neutral-axis
saturation stretch, the same shape of operation `ColorSaturation` performs.

```
source a* range:      [5.755, 11.564]   (b* range [2.135, 4.312])
source occupied bins: [(8, 8)]

--- boost=1.8 ---
candidate a* range:      [10.549, 21.208]
candidate occupied bins: [(8, 8), (9, 8)]
mass landing in source-empty bins: 0.9140
verdict: ok=False, reason="hue invention: 0.9140 chroma mass appeared in
  a*/b* cells with no support in the source (limit 0.0010)"

--- boost=2.0 ---
candidate a* range:      [11.763, 23.648]
candidate occupied bins: [(8, 8), (9, 8)]
mass landing in source-empty bins: 0.9739
verdict: ok=False, reason="hue invention: 0.9739 chroma mass appeared in
  a*/b* cells with no support in the source (limit 0.0010)"
```

I also swept boost ∈ {1.5, 1.8, 2.0, 2.2} against this same source; every
value tripped the check (`ok=False`), with mass-in-empty-bins ranging from
0.72 (boost 1.5) to 1.00 (boost 2.2). A degenerate fully-uniform-color
variant of the same fixture (single flat patch instead of `_smooth_image`)
produces the same failure at 100% mass for every boost tested, confirming
the effect is not an artifact of the spatial-variation fixture choice.

The fixture satisfies the review's validity requirement (mass in
source-empty bins is provably `> 0`, in fact the dominant fraction of the
candidate's total mass), so it is a genuine boundary crossing, not an
artifact of a poorly-chosen test.

### Result: `.ok` went False — STOPPING per instructions

This is the branch the review flagged as a real finding, not a green
light. **I have not modified `check_hue_invention`, `hue_invention_mass`,
or added a dilated-support mask.** I have not committed a test asserting
either outcome — doing so would embed an unruled design decision (either
"this guardrail correctly rejects near-edge saturation moves" or "the
binned check needs tolerance" — both are real positions, and the choice
isn't mine to make unilaterally).

### My recommendation (for the ruling)

The failure is not narrow or hard to hit: at 16 bins over a ±100 a*/b*
range (12.5-unit bins), any source chroma sitting within roughly a
boost-dependent margin of a bin edge trips this guardrail for entirely
ordinary saturation increases — and it tripped at every boost from 1.5x to
2.2x in this fixture, not just at extreme values. Real astrophotography
chroma is continuously distributed, not centered in bins by construction,
so some meaningful fraction of any given source's pixels will sit near
some edge. Given `ColorSaturation` is a real optimizer action (strong =
0.85 strength per `actions.py`), this guardrail as currently specified
will reject a non-trivial share of legitimate saturation moves whenever
their source chroma happens to abut a bin edge — which is not a rare
coincidence but a structural property of any fixed histogram grid.

I recommend the dilated-support-mask approach the original dispatch
described as the principled fix: treat a source cell as "supporting" its
own occupied bins *and* their immediate neighbors (e.g., a 3x3 dilation of
the occupied-bin mask) before computing unsupported mass, so intensifying
existing color can spill one bin outward without being flagged, while
color appearing several bins away from any source support (the actual
fabrication case) still trips. This preserves the guardrail's purpose
(bar hues with no real support) while removing the artifact of comparing
against a rigid, unaligned bin grid. I have not implemented this — it
requires your ruling first, per the review's explicit instruction.

### Commit

```
212c841 optimize: cosmetic import cleanup + document neutral-source intent
```
(cosmetic + docstring only; the boundary-crossing finding above is
reported, not fixed, pending ruling)

---

## Fix round 2 (ruling: reimplement on hue angle)

### Ruling received

Team-lead + user independently measured the round-1 finding is real and severe:
a 1.3x saturation boost already trips (0.0054 mass vs limit 1e-3), 1.8x puts
0.952 of the mass in "unsupported" cells. Ruling: reimplement
`check_hue_invention` on hue ANGLE instead of a*/b* cell position, per the
now-authoritative spec section `docs/superpowers/specs/2026-07-27-phase2-optimizer-design.md`
§5.1.

### Implementation

`check_hue_invention` (and its helper, renamed `_chroma_hist` → `_hue_hist`) now:

1. Converts RGB → Lab, computes `chroma = hypot(a, b)` and
   `hue = degrees(atan2(b, a)) % 360`.
2. Drops any pixel with `chroma <= HUE_CHROMA_FLOOR = 1.0` before histogramming
   — a pixel this close to neutral has a numerically meaningless hue angle
   (atan2 of two small, noisy numbers), and letting it vote would make
   "unsupported" a measure of noise, not color. 1.0 sits below the ~2.3 CIELAB
   delta-E commonly cited as the smallest perceptible color difference.
3. Builds a chroma-weighted, normalized 1D histogram over hue angle with
   `NUM_HUE_BINS = 120` (3-degree bins).
4. Flags candidate mass landing in bins where the source histogram is `<= 0`,
   exactly mirroring the old cell-based logic's structure — only the grid
   changed, from 2D a*/b* position to 1D hue angle.

`check_channel_ratio_drift` and `evaluate_guardrails` are untouched. Moved the
new imports (`rgb_to_lab`) to the top of `guardrails.py` — `CHROMA_BINS`,
`CHROMA_EXTENT`, and `chroma_histogram` are no longer used at all (the 1D hue
histogram is built directly with `np.histogram`, not the 2D a*/b* utility),
so those three imports were removed rather than moved.

### Why 120 bins of 3 degrees, not the a*/b* grid's 16

A legitimate radial-only move (saturation) is **exactly** hue-preserving under
a*/b* scaling — a pixel's hue angle does not change at all when only its
a*/b* magnitude is scaled about the same L*. That means, at ANY bin
resolution, a purely radial move can never leave the bin it started in — no
alignment luck required, unlike the old a*/b* position grid where radial
motion crosses cells by construction.

What has to reliably escape its bin is a **genuine** hue shift: fabrication,
or a boost so strong it clips a channel on the round trip back to sRGB and
distorts the actual color. Bin width has to be strictly smaller than the
smallest genuine shift the acceptance table requires catching, so that
`shift > bin_width` mathematically guarantees `floor((x+shift)/w) != floor(x/w)`
for *any* starting position `x` — not merely for a favorably-placed source.
Measured (below): the smallest such shift in the required acceptance set is
3.58 degrees (the 3.0x clipping case). 3-degree bins (120 bins over 360
degrees) sit strictly under that with headroom, while the "genuinely
in-gamut" cases measure **exactly 0.0 degrees** of drift, so they can never
be at risk regardless of resolution.

### A defect I found and fixed in my own diagnostic method, mid-round

My first attempt to build the acceptance fixtures used the na√Øve check
`raw_rgb.min() >= 0.0` to confirm a boost "genuinely stayed in gamut." This is
wrong: the sRGB gamma-encoding step (`_linear_to_srgb`) floors negative LINEAR
light to 0 *before* gamma-encoding (raising a negative number to a fractional
power is undefined), so the function's *output* can never be negative even
when the pre-floor value genuinely went out of gamut. Checking gamut against
the final sRGB array is a silent false negative. I caught this because my
first fixture (`biasLow=0.5`) showed hue drift starting at boost=1.8 (0.85
degrees) and 2.2 (3.43 degrees) — i.e., clipping that my own gamut check had
failed to detect. I fixed this by exposing `_lab_to_linear` (the pre-floor
linear-light value) separately in the test file and checking gamut against
*that*, then re-swept the fixture parameter to find a bias where 1.3/1.8/2.2
are genuinely, verifiably unclipped in linear space.

### Final fixture and measured acceptance-case numbers

Fixture: a uniform 64x64 patch, `rgb = [0.5, 0.5, 0.32]` (blue channel at
`0.5 * 0.64`, a real, existing blue-deficiency color bias). Boost = scale Lab
`a*`, `b*` by the factor, round-trip back to sRGB, clip to `[0, 1]`.

```
source hue = 106.844 degrees, chroma = 26.10

boost=1.3: linear-space min/max = [0.058, ...] (NO clip) -- delta_hue = 0.000000000
boost=1.8: linear-space min/max = [0.023, ...] (NO clip) -- delta_hue = 0.000000000
boost=2.2: linear-space min/max = [0.003, ...] (NO clip) -- delta_hue = 0.000000000
boost=3.0: linear-space min      = -0.0219    (CLIPS)    -- delta_hue = 3.578432...
```

`check_hue_invention` verdicts on this fixture:

| Case | linear-space clip | delta_hue (deg) | `.ok` | Required |
|---|---|---|---|---|
| 1.3x boost | No | 0.000 | **True** | pass |
| 1.8x boost | No | 0.000 | **True** | pass |
| 2.2x boost | No | 0.000 | **True** | pass |
| 3.0x boost | Yes | 3.578 | **False** | trip |
| green cast on neutral source | n/a | undefined (no source hue at all) | **False** | trip |

All five required acceptance cases hold, verified by direct assertion in the
test suite (not just inspected by hand):

- `test_hue_invention_passes_on_legitimate_saturation_boosts[1.3/1.8/2.2]` —
  parametrized; each asserts genuine (linear-space) non-clipping, asserts
  `delta_hue == 0.0` to 1e-9, then asserts `verdict.ok`.
- `test_hue_invention_trips_on_a_boost_that_clips_and_shifts_hue` — asserts
  genuine linear-space clipping, asserts `delta_hue > 3.0` (measured 3.578),
  then asserts `not verdict.ok` and `"hue" in verdict.reason.lower()`.
- `test_hue_invention_trips_on_color_with_no_source_support` — pre-existing,
  still valid unchanged: neutral source (chroma = 0 everywhere, below the
  floor) + green cast, `not verdict.ok`.
- `test_lab_to_rgb_round_trips_rgb_to_lab` — new, confirms the hand-written
  Lab→RGB inverse used to build these fixtures is actually correct (max abs
  error < 1e-9 against `rgb_to_lab`), so a wrong inverse can't silently
  fabricate the "boost preserves hue exactly" premise the other tests rely on.

### Exact commands and real output

```
$ .venv/bin/python -m pytest tests/test_optimize_guardrails.py -v
```
```
tests/test_optimize_guardrails.py::test_mrs_noise_sigma_rises_with_injected_noise PASSED
tests/test_optimize_guardrails.py::test_noise_floor_trips_when_noise_balloons PASSED
tests/test_optimize_guardrails.py::test_noise_floor_fails_closed_on_degenerate_baseline PASSED
tests/test_optimize_guardrails.py::test_clipped_fraction_counts_per_channel PASSED
tests/test_optimize_guardrails.py::test_shadow_and_highlight_clipping_trip_independently PASSED
tests/test_optimize_guardrails.py::test_verdict_carries_value_and_limit_for_reporting PASSED
tests/test_optimize_guardrails.py::test_detect_stars_finds_the_planted_stars PASSED
tests/test_optimize_guardrails.py::test_star_integrity_passes_on_an_untouched_frame PASSED
tests/test_optimize_guardrails.py::test_star_integrity_trips_when_stars_are_destroyed PASSED
tests/test_optimize_guardrails.py::test_star_integrity_passes_but_says_so_when_baseline_is_starless PASSED
tests/test_optimize_guardrails.py::test_lab_to_rgb_round_trips_rgb_to_lab PASSED
tests/test_optimize_guardrails.py::test_hue_invention_passes_on_legitimate_saturation_boosts[1.3] PASSED
tests/test_optimize_guardrails.py::test_hue_invention_passes_on_legitimate_saturation_boosts[1.8] PASSED
tests/test_optimize_guardrails.py::test_hue_invention_passes_on_legitimate_saturation_boosts[2.2] PASSED
tests/test_optimize_guardrails.py::test_hue_invention_trips_on_a_boost_that_clips_and_shifts_hue PASSED
tests/test_optimize_guardrails.py::test_hue_invention_trips_on_color_with_no_source_support PASSED
tests/test_optimize_guardrails.py::test_channel_ratio_drift_trips_when_ratios_move PASSED
tests/test_optimize_guardrails.py::test_evaluate_guardrails_returns_every_verdict_not_just_the_first PASSED
============================== 18 passed in 0.15s ==============================
```

Full project suite, to confirm nothing else referenced the removed
`CHROMA_BINS`/`CHROMA_EXTENT`/`chroma_histogram` imports:
```
$ .venv/bin/python -m pytest -q
325 passed in 32.34s
```

### Self-review

- Did not raise `hue_invention_mass`. The fix is entirely in what's being
  measured (angle vs. position), not in loosening the limit.
- Did not touch `check_channel_ratio_drift` or `evaluate_guardrails`.
- The bin-width choice (3 degrees) is derived from measured numbers (0.0 vs
  3.578 degrees), not picked to make a specific test pass — the same
  discipline SS3.7 required of the original limit.
- Caught and fixed my own diagnostic bug (linear-vs-sRGB gamut check) before
  it produced a wrong "clean separation" claim, by tracing why the actual
  pytest run disagreed with an earlier scratch measurement instead of
  adjusting the test to match the wrong number.
- One acknowledged limitation, stated plainly rather than papered over: this
  guardrail's bin width is calibrated against a hand-built Lab-radial-scaling
  proxy for "saturation boost," since the real `ColorSaturation` executor
  (Task 6, not yet built) does not exist yet. If the real executor's
  hue-preservation is less exact than this proxy (e.g., measurable jitter of
  a degree or more from working in a different color space or lower
  precision), the 3-degree bin may need revisiting once Task 6 lands and can
  be measured directly -- consistent with SS5.1 itself being a revision made
  "on measurement" rather than upfront.

### Commit

```
bdc5020 optimize: reimplement hue invention on hue angle, not a*/b* position (SS5.1)
```

---

## Fix round 3 (correction: angular tolerance, clipping is not this guardrail's job)

### What changed and why

Team-lead verified round 2's implementation against an independent, spatially
varying fixture and found 1.3x, 1.8x, and 2.2x ALL tripped on a fixture
confirmed not to clip at any boost. Root cause: saturation is applied in RGB,
but CIELAB hue angle is a nonlinear function of RGB, so a legitimate boost
genuinely drifts the hue angle by a few degrees even with zero clipping. My
round-2 fixture used an angle-EXACT Lab-radial boost (scaling a*/b* directly
in Lab space), which is not how a real saturation operation works and hid
this drift — that's why 3-degree bins "worked" there and nowhere else. The
round-2 "3.0x must trip" requirement itself was also withdrawn: it was
asking the hue-invention guardrail to detect gamut clipping, which is
`check_shadow_clipping`'s and `check_highlight_clipping`'s job, not this
one's.

This is now documented as the authority in
`docs/superpowers/specs/2026-07-27-phase2-optimizer-design.md` §5.1,
"Corrected 2026-07-27, second iteration."

### New implementation

`check_hue_invention` no longer uses exact-bin membership. It now:

1. Builds a chroma-weighted histogram over hue angle at 1-degree resolution
   (`NUM_HUE_BINS = 360`) for both source and candidate — a measurement grid
   only, far finer than the tolerance, so discretization error is negligible.
2. Marks a source bin "occupied" if it has any above-floor chroma mass.
3. Dilates the occupied-bin mask **circularly** (`_dilate_circular`, using
   `np.roll` in both directions, which wraps at the array ends — hue is a
   circle, so 359° is adjacent to 1°, not 358° away) by
   `ceil(limits.hue_tolerance_deg / bin_width_deg)` bins — 20 bins at the
   default 20°/1° resolution.
4. Sums normalized candidate mass landing outside that dilated "supported"
   region.

`GuardrailLimits` gained a new field: `hue_tolerance_deg: float = 20.0`.

`check_channel_ratio_drift` and `evaluate_guardrails` remain untouched. The
`hue_invention_mass` limit (1e-3) was **not** raised.

### Scaffolding removed

Per instruction, all clipping/gamut-detection machinery built for round 2's
(now-withdrawn) "3.0x must clip and trip" requirement was removed:

- `_lab_to_linear`, `_linear_to_srgb`, `_lab_to_rgb`, `_XYZ_TO_RGB`, and the
  `test_lab_to_rgb_round_trips_rgb_to_lab` round-trip test (all test-file-only
  scaffolding for building an angle-exact Lab-space fixture).
- The "confirm this boost genuinely clips in linear space" diagnostic logic.
- The docstring's earlier framing of clipping as part of this guardrail's
  job — the new docstring states explicitly that gamut clipping is deliberately
  NOT this guardrail's concern.

Nothing in the shipped `guardrails.py` module ever contained gamut/clipping
logic for this check (the pre-floor linear diagnostic was test-file-only), so
there was nothing to remove from the production module itself beyond the
histogram/bin-membership logic already replaced.

### Minimum-chroma floor (kept, unchanged)

`HUE_CHROMA_FLOOR = 1.0` (a*/b* units), same as round 2: below this, a
pixel's `atan2(b, a)` is dominated by numerical noise rather than real color,
and 1.0 sits below the ~2.3 CIELAB delta-E commonly cited as the smallest
perceptible color difference.

### Neutral-source-blocks-colorization (kept, unchanged)

An all-neutral source has no bin with above-floor chroma, so the dilated
"supported" mask is all-`False` regardless of tolerance — any candidate
chroma above the floor is unsupported by construction. Verified by the
`test_hue_invention_trips_on_color_with_no_source_support` case below
(mass = 1.0000).

### Fixtures: spatially varying, not flat (per instruction)

All boost/injection fixtures use `_smooth_image()` (a spatially varying
sinusoidal luminance field) with a red channel bias, not a single uniform
patch — a flat-hue fixture is exactly what hid the real RGB-space drift in
round 2. `_saturation_boost(rgb, boost) = gray + (rgb - gray) * boost` is a
real RGB-space saturation stretch (not an angle-exact Lab operation).

### Measured displacement / mass for all six acceptance cases

`GuardrailLimits()` defaults: `hue_tolerance_deg=20.0`, `hue_invention_mass=0.001`.

| Case | Max/mean circular displacement | Mass (verdict.value) | `.ok` | Required |
|---|---|---|---|---|
| 1.3x saturation boost (unclipped) | 0.3487° | 0.000000 | **True** | pass |
| 1.8x saturation boost (unclipped) | 0.9668° | 0.000000 | **True** | pass |
| 2.2x saturation boost (unclipped) | 1.4938° | 0.000000 | **True** | pass |
| 3.0x saturation boost (unclipped) | 2.6293° | 0.000000 | **True** | pass |
| Green injected into a red image | 103.9466° | 1.000000 | **False** | trip |
| Green cast on a neutral source | no supported hue at all | 1.000000 | **False** | trip |

Additional required property — 0/360 wraparound handled correctly:

| Wraparound fixture | Naive `\|h1-h2\|` | True circular distance | mass | `.ok` |
|---|---|---|---|---|
| source hue 3.76°, candidate hue 355.90° | 352.14° (looks like fabrication) | 7.86° (well within the 20° tolerance) | 0.000000 | **True** (correctly passes) |

All boosts genuinely stayed in gamut (`candidate.min() >= 0` and
`candidate.max() <= 1`), confirmed by direct assertion in each test, not
assumed.

### Exact commands and real output

```
$ .venv/bin/python -m pytest tests/test_optimize_guardrails.py -v
```
```
tests/test_optimize_guardrails.py::test_mrs_noise_sigma_rises_with_injected_noise PASSED
tests/test_optimize_guardrails.py::test_noise_floor_trips_when_noise_balloons PASSED
tests/test_optimize_guardrails.py::test_noise_floor_fails_closed_on_degenerate_baseline PASSED
tests/test_optimize_guardrails.py::test_clipped_fraction_counts_per_channel PASSED
tests/test_optimize_guardrails.py::test_shadow_and_highlight_clipping_trip_independently PASSED
tests/test_optimize_guardrails.py::test_verdict_carries_value_and_limit_for_reporting PASSED
tests/test_optimize_guardrails.py::test_detect_stars_finds_the_planted_stars PASSED
tests/test_optimize_guardrails.py::test_star_integrity_passes_on_an_untouched_frame PASSED
tests/test_optimize_guardrails.py::test_star_integrity_trips_when_stars_are_destroyed PASSED
tests/test_optimize_guardrails.py::test_star_integrity_passes_but_says_so_when_baseline_is_starless PASSED
tests/test_optimize_guardrails.py::test_hue_invention_passes_on_legitimate_saturation_boosts[1.3] PASSED
tests/test_optimize_guardrails.py::test_hue_invention_passes_on_legitimate_saturation_boosts[1.8] PASSED
tests/test_optimize_guardrails.py::test_hue_invention_passes_on_legitimate_saturation_boosts[2.2] PASSED
tests/test_optimize_guardrails.py::test_hue_invention_passes_on_legitimate_saturation_boosts[3.0] PASSED
tests/test_optimize_guardrails.py::test_hue_invention_trips_on_green_injected_into_a_red_image PASSED
tests/test_optimize_guardrails.py::test_hue_invention_trips_on_color_with_no_source_support PASSED
tests/test_optimize_guardrails.py::test_hue_invention_handles_wraparound_at_zero_degrees PASSED
tests/test_optimize_guardrails.py::test_channel_ratio_drift_trips_when_ratios_move PASSED
tests/test_optimize_guardrails.py::test_evaluate_guardrails_returns_every_verdict_not_just_the_first PASSED
============================== 19 passed in 0.21s ==============================
```

Full project suite:
```
$ .venv/bin/python -m pytest -q
326 passed in 31.68s
```

### Self-review

- Did not raise `hue_invention_mass`.
- Did not touch `check_channel_ratio_drift` or `evaluate_guardrails`.
- Removed all clipping/gamut diagnostic scaffolding built for the now-withdrawn
  round-2 requirement, rather than leaving dead code around "just in case."
- Used a spatially varying fixture (per explicit instruction) after the flat
  uniform-patch fixture in round 2 concealed the real RGB-space hue drift.
- Wraparound is tested with a fixture specifically engineered to straddle the
  0/360 seam (naive diff 352°, true distance 7.9°) rather than asserted
  without ever exercising the failure mode a bug would produce.
- `hue_tolerance_deg` is a named, visible `GuardrailLimits` field, not a
  magic number buried in the function body.
- One thing I did NOT independently re-derive: the 20° default tolerance
  itself. That number comes from the team-lead's own measurement (bounded
  drift ~6°, invention ~116°) documented in the now-authoritative spec
  section; my own fixture's measured drift (max 2.63°) is consistent with
  "bounded and well under 20" but uses a different source bias, so it
  corroborates the tolerance's safety margin rather than re-deriving the
  20° figure from scratch.

### Commit

```
24730d8 optimize: hue invention as angular tolerance, not exact-bin membership (SS5.1)
```
