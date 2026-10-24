# PlanetaryStack benchmark harness: metric, references, benchmark set

Host-only test tooling for the PlanetaryStack engine (spec:
`docs/superpowers/specs/2026-10-21-planetary-stacker-design.md`, Yardstick section and Rulings 1-2).
It measures our stack against Scott's own AutoStakkert (AS) and ASIStudio (ASI) results of the same
captures. Nothing here ships; the NAS (`/mnt/qnap/astro_data`) is only ever read.

| File | What it does |
|---|---|
| `metric.py` | END, the judged sharpness-at-equal-noise measure (Ruling P28), and END_interior; DAN and DAN_interior (secondary); the target box and interior window |
| `noise.py` | The capture's per-frame noise: a photon-transfer fit to its raw frames, and sigma_frame^2 on a stack's box (END's noise model) |
| `halves.py` | The split-half validation of END's noise model: even/odd SER split, and the check of the halves' difference against the model |
| `refs.py` | Loads AS/ASI/our images, area-resamples drizzled AS output to native (Ruling 1; the factor from the reference's size), registers each reference to an anchor (orientation, sub-pixel shift, measured scale), measures all on one box, and lists what needs human review |
| `bayer.py` | The CFA pattern of a capture, measured on its raw frames in the reader's layout |
| `bench.py` | run-stack-bench.sh's run plan, the Ruling 2 / P28 status (END judged, flip check, unmeasured), side-by-side PNGs and summary.md |
| `benchset.py` | The benchmark set below (as data, with each capture's Bayer pattern and its source), `compare_capture` for run-stack-bench.sh, and a survey that checks every reference against its own video |
| `test_*.py`, `conftest.py` | pytest. Markers: `nas` (reads the NAS; `-m "not nas"` deselects them), `ffmpeg` (skipped when ffmpeg/ffprobe are not on PATH) |

## Setup

Python 3.14 in a venv outside the repo (any location; the session used its scratchpad):

```bash
python3 -m venv <venv>
<venv>/bin/pip install numpy scipy astropy tifffile Pillow opencv-python-headless pytest
cd modules/planetary/test/bench
<venv>/bin/python -I -m pytest -q -p no:cacheprovider               # 213 tests + 32 strict xfails (C1), ~3.2 min with the NAS (final fix round, b35e770c); the engine tests need the built probes
<venv>/bin/python -I -m pytest -q -p no:cacheprovider -m "not nas"  # 160 tests + 26 strict xfails without the NAS, ~1.5 min
<venv>/bin/python -I benchset.py survey ../output/bench-survey       # survey.json + thumbnails (gitignored)
```

Versions used: numpy 2.5.4, scipy 1.18.1, astropy 8.0.2, tifffile 2026.10.15, Pillow 13.0.0,
opencv-python-headless 5.0.0, pytest 9.1.1. ffmpeg/ffprobe are needed for the survey, `bayer.py`
and the frame-count checks.

**Why OpenCV for PNG:** every AS PNG on the NAS is 16-bit RGB (IHDR bit depth 16, colour type 2).
Pillow reads those as 8-bit RGB, keeping only the high byte (verified on the 2_12_2023 AS_P20
Jupiter: Pillow's array equals OpenCV's `>> 8` exactly). `refs.load_image` reads PNG with OpenCV;
a test pins the 16-bit round trip.

## The benchmark stacks debayered

Every stacked capture is stacked with its Bayer pattern set (`stack-probe --bayer <pattern>`,
`Capture.bayer`). Comparing a raw-CFA mono stack with debayered references is not valid: the review
re-mosaicked AS_P20 in all four CFA phases and measured the mono-vs-RGB bias at x1.019-1.026 on
Jupiter and x0.930-1.094 on the Moon (2x2 binning: Jupiter +-0.8 %, Moon 0.969-1.067), beyond the
3 % bar. `mosaic=True` remains only as a fallback, and every row it produces carries a "not a
pass/fail" review note. The product itself still stacks an undeclared mosaic as raw mono (Ruling
P9). The benchmark measures the debayered path because that is what a user who sets the pattern gets.

The pattern is given in the reader's frame layout, because an explicit pattern is applied as it
stands (`ser::ResolveBayer`: "An explicit choice wins"), and `avi_real_check.py` proves the reader's
frames equal ffmpeg's bytes.

**Every colour capture is RGGB** (Scott, 2026-10-22). Each one came from a ZWO ASI585MC (Pro/Air) or
an ASI2400MC Pro, and ZWO's specs give RGGB for both sensors (IMX585, IMX410). A physical check on two
2026-09-07 Saturn AVIs from the same camera agrees: RGGB, top-down, even ROI origin.

`bayer.py` measures each capture's green phase with `avi_real_check.py`'s method: per-site means over
the unsaturated disc, the greens being the more equal diagonal. The green phase is unambiguous on
every capture (greens 0.0-1.7 % apart) and is the RGGB one everywhere. Red against blue comes from
the camera spec. The warm-target rule that `bayer.py` also applies (red is the brighter non-green
site) is not a valid source, and through the Lqef filter it does not apply at all.

* An R/B swap leaves the luminance metrics unchanged. The engine's Hamilton-Adams demosaic applies
  the same rule at red and blue sites, so a swap only exchanges the R and B planes; Task 9 measured
  DAN equal to four digits on BGGR and RGGB stacks of the 2026-01-30 Jupiter. The side-by-side
  colours do change.
* The references' own colour balance shows which pattern each tool used (a reference's R/B equals the
  raw site ratio of the sites it took as red):
  * AutoStakkert debayered the 2023-02-11 Jupiter, the 2023-02-12 Moon and Mars BGGR, so their red
    and blue are swapped against ours.
  * Every ASIStudio file is RG (`VSBAYER = 0`).
* The 2023-03-05 Moon's ASIAIR sidecar says Bayer = RG with Flip = Vert(unimplemented). ASIAIR never
  applied that flip, so the reader's Auto is RGGB too, the same as the bench's explicit pattern (spec
  amendment 2026-10-23; Ruling 10's BGGR was withdrawn).
* The 2026-01-30 Jupiter and the 2024-02-01 Moon carry SERViewer Save-pattern sidecars (Bayer = RG).

## The judged metric: END (Ruling P28)

The spec asks for "one documented sharpness-at-equal-noise metric". DAN (below) measured detail with
each image's noise removed, so it gave no credit for SNR: in Task 9 a stack with 6.7-9.7 x the
reference's per-band SNR "failed" on DAN. Ruling P28 made the judged metric literally sharpness at
equal noise. **END** ("equal-noise detail") asks: once each image is sharpened as far as its noise
allows, how much of the target's detail comes back? Full definition and reasoning: the `metric.py`
docstring. In short:

1. Same box, normalisation and Hann periodogram as DAN.
2. **One noise model for every image**, computed identically for ours and every reference: white
   noise of power sigma_frame^2 / k.
   * sigma_frame^2 is the capture's per-frame noise on the box, from a photon-transfer fit to its raw
     frames (`noise.py`; independent of any stacker).
   * k is the image's effective frame count.
     * For ours it is the **noise-power k over the box**: 1 / (the box's Hann^2-weighted mean of
       sum_f alpha_f^2 / (sum_f alpha_f)^2), with alpha_f(p) the accumulator's blend weight of frame f
       at pixel p. `stack-region-probe --keff-box` computes it from the engine's own plan (per-frame
       kept flags, `AxisWeightAt`). That probe repeats RunStack's selection steps, so its stacked and
       per-region kept counts must equal stack-probe's (RunStack's own), or END is unmeasured.
     * For a reference it is the stacking percentage x the capture's frames. AutoStakkert's
       percentage is global, so this is approximate per alignment point.
   * Any smoothing (interpolation, drizzle, resampling, denoising) therefore counts as blur, on our
     side and the references' alike, as in DAN and Ruling 1.
3. Per bin (0.06-0.30 cycles/px, steps of 0.03): SNR = max(P - N, 0) / N.
   END = sqrt( sum O(f) / sum O(f) / (1 + SNR) ) with the object prior O(f) = f^-3. That is the band's
   detail over the error of a matched Wiener restoration, as an amplitude ratio.
4. **END_interior**: the same on the interior window's high-pass, judged where DAN_interior was
   (the eroded disc is at least 20 px).
5. Ruling 2's 0.97 bar applies to END_full and, where judged, END_interior.

### Known limit: noise and sharpening the budget does not hold (C1)

END counts as detail any power the photon budget does not explain. Noise beyond the budget raises
END, whether uniform, shot-like or on the target only, and so does a linear sharpening. This holds on
our stack and on the references alike. `test_end.py` pins every case as a strict xfail: if a fix lands
they XPASS and fail, so this README and `metric.py` get revisited.

Measured, on synthetic stacks (the noise in multiples of the stack's own):

| Stack | Uniform x1 / x2 | Shot-like x1 / x2 | Target-only x1 / x2 | Unsharp mask 0.5 / 1 |
|---|---|---|---|---|
| Unsmoothed | x1.19 / x1.55 | x1.19 / x1.34 | x1.18 / x1.55 | x1.45 / x1.88 |
| Ours-like (B-spline frames) | x1.20 / x1.56 | x1.20 / x1.35 | x1.19 / x1.56 | x1.45 / x1.88 |
| Reference-like (drizzled) | x1.23 / x1.75 | x1.24 / x1.46 | x1.23 / x1.75 | x1.32 / x1.72 |

On real AutoStakkert P20 stacks:
* uniform noise of 0.005 / 0.02 of the signal raises END x1.02 / x1.38 on the Jupiter and x1.09 /
  x1.75 on the Moon;
* an unsharp mask raises it x1.94 and x1.92.

Seeing 2.0 plus 3 x the budget's noise scores 5.67, above seeing 1.0's 5.14: unmodelled noise can buy
back blur.

**No single-image estimator can fix this.** Two cases are not separable:
* A linear sharpening G of a stack with its own kernel K is, spectrum for spectrum, the image of an
  honest stack with kernel G K. END must rank that honest stack higher whenever G K <= 1.
* Noise added to a smoothed stack cannot be told from the stack's own noise kernel, which is unknown.

Tried during Ruling P28 and not shipped: a guard that raised the noise to the box's own fitted white
level (a joint fit of a smooth signal roll-off plus white noise), and a sky guard. The first caught
every case on an unsmoothed stack, but a stack's own smoothing leaves headroom below its budget, and
the guard was blind inside it. Neither guard's code is in the tree, so its measurements are not kept
here (final review, bench M3).

**How the bench judges despite it (Ruling P28b).** Ours is not a single image: the split halves
measure its noise. So ours is charged its measured noise per END bin, budget x max(1, R/R_cal) (below),
and the references stay on the photon budget. The metric, crop, registration and normalisation stay
identical; only the noise charge differs, and only ever against ours.

C1 can then only inflate a reference's END, which lowers ours' ratio:
* a numeric **pass** is sound (Scott's side-by-side is still the spec's final word);
* a numeric **fail** is sound only against a reference whose own files record no processing.
  The bench applies that to the reference that sets the bar -- the best END, full or interior
  (`bench._setters`): a fail stands only when every such reference is unprocessed. So when ours is
  below 0.97 x an unprocessed AutoStakkert stack but an ASIStudio stack of unknown processing scores
  higher still, the capture is review, although the fail would be sound against AutoStakkert alone
  (the 2024-02-01 and 2023-08-22 Moons, with both tools, are where it can happen). That errs the
  safe way -- no fail is claimed from a comparison C1 may have decided -- and stays (Ruling F3).

The references' provenance (`refs.provenance`):
* **AutoStakkert:** raw (non-`_conv`) stacks whose `.as3` session files record no row/column
  calibration, no pre-blur and no sharpening of that file. That reads as "none".
* **ASIStudio:** each `.fit` header records PN (noise reduction) and SHARPEN export settings (1 / 20 up
  to 100 / 37) without saying whether they reached the `.fit`. That reads as "unknown", and a fail
  against it is review.

### Why not read each image's own noise (measured 2026-10-22)

These figures falsified every single-image noise estimate, and they are why END uses the physics
instead.

* **The box's own 0.40-0.50 cycles/px power is not noise.** Our engine stacked the even and odd
  frames separately, on the 2026-01-30 Jupiter and the 2023-02-12 Moon. The box's floor was 55 x and
  2.7 x the halves' difference: it is structure common to disjoint frames.
  * On the Jupiter it is real horizontal structure (the belts and the limb's horizontal arcs, which
    ASIStudio's stack shows too), plus an isotropic part that is neither target-locked nor
    sensor-fixed. See "What the interleaved halves share" below.
* **Every stack's noise is coloured, ours included.** Sky noise at 0.21-0.24 cycles/px is 1.1-38 x its
  0.40-0.50 floor. A white floor under-reads the in-band noise by that factor, differently per tool.
  * Under the white floor, ASIStudio led the 2026-01-30 Jupiter 4.5 x: its target floor is 80 x below
    ours.
  * Its sky floor equals the 1-DN quantisation variance exactly.
* **The sky is not a proxy for the target.**
  * Sky noise is 600-54 000 x below the target noise: the raw 8-bit sky is a flat ~3 DN.
  * Its spectral shape is not the target's.
  * References' sky noise does not scale with their frame counts: ASIStudio's 40 % stack has 0.85 x
    our sky noise.

The verdicts under each of these models are in the P28 report.

### The plan's AS P20 > P50 check (a departure from the plan's wording)

The plan asked that, on `2_12_2023`, AS_P20 rank above AS_P50 "as AutoStakkert's selection implies".
That pair is confounded: P20 is `lapl5_ap1` and P50 `lapl5_ap2` (different alignment-point settings),
so it does not isolate the share of frames. Under Ruling P28, rule 4, it is now a reported figure,
not an assertion:

| 2_12_2023 Moon | k | END | DAN |
|---|---|---|---|
| AS_P20 | 471 | 13.20 | 0.06876 |
| AS_P50 | 1177 | 16.80 | 0.05266 |

(`test_metric.py::test_real_2_12_2023_moon_p20_p50_pair_is_a_reported_figure`, which prints them;
measured at 067bd83d. The 13.28 / 16.89 printed here before did not reproduce.)

The assertion moved to a synthetic (`test_end.py`): one 200-frame capture, its sharpest 20 % and
sharpest 50 % stacked. END must rank them as the exact matched restoration does, and the set holds a
case each way:
* where the seeing drifts smoothly (log-normal, median sigma 1.4 px, log-sd 0.45), P50 wins;
* where a quarter of the frames are steady moments (bimodal, 1.0 / 3.0 px), P20 wins.

DAN ranks P20 first in all four cases, wrongly in the log-normal ones.

Ruling 4 asked for "a capture with real seeing spread". The seeing here is parametric, not taken from
a real capture's per-frame quality: stack-probe does not export per-frame quality, so the two shapes
stand in for it.

### Validation (test_end.py, measured 2026-10-22/23)

| Property | Measured |
|---|---|
| Blur at equal noise ranks lower (seeing 1.0 / 1.5 / 2.0 / 3.0) | END 5.141 / 2.946 / 2.296 / 1.720 |
| Added noise or sharpening never raises it | **fails, C1** (table above; strict xfail) |
| More frames, same blur (2 x / 4 x) | x1.191 / x1.481. DAN moves x1.003 and fails the test. |
| Sharper but noisier, against the exact matched restoration | 4 of 4 agree, with a win each way. DAN gets at least one wrong (asserted). |
| 20 % vs 50 % of one capture (4 cases, both outcomes) | 4 of 4. DAN fails the 2 log-normal ones (asserted). |
| Smoothing counts as blur: drizzle x3 + area resample / Gaussian 0.7 / B-spline half-pixel shift | x0.809 / x0.746 / x0.978. Taking the box's own 0.40-0.50 floor as the noise instead raises it x2.12 / x3.21 / x1.84: the reference-side bias the photon budget removes. |
| Real AS_P20 (2_12_2023 Jupiter / Moon), blur 0 / 0.5 / 1 / 2 after the fact (`test_real_blur_and_smoothing_rank_lower`, at 067bd83d) | 20.80 / 15.74 / 5.71 / 3.16 and 13.20 / 10.08 / 4.36 / 2.39 |
| Interior: an interior blur of sigma 1 behind a crisp limb | 19.75 -> 15.62 (2 x noise raises it to 21.34: C1) |
| `noise.photon_transfer` (fix round 3: free intercept, Theil-Sen start): textured disc drifting 0.5 px per frame / a 3 px disc in 640x480 / lunar texture under 0-1.5 px jitter / a sky on a 3 DN black pedestal | a +7 % / +60 % / within 0.5 % / a +4 %, zero at 3.00 DN (with c clamped to 0, a reads x0.26) |
| The calibration's pedestal synthetics (3 DN, c 0.05; fix round 4) | disc (12 runs, from calibration.json): a 0.1469-0.1752 m (j0.3 0.1469-0.1499; j0.7 0.1606-0.1752, 7-17 % high: the jitter read-high that sits in R_cal), c -0.468 to -0.317; lunar (12 runs): a 0.1487-0.1497, c -0.322 to -0.313; true 0.15 m - 0.317 |
| P28d tile shifts: an engine-like local shift between the halves / noise only / B at twice the noise / a 2 % (5 %) gain bump at a limb | charge at most 1.21 / 0.90-1.04 of the true model / charge 2.47-2.66 (2.5) / never less than the bump itself charges (gain-only): 1.17 against 1.18 (1.92 against 1.82) in the lowest bin (bench6; until then the reference was the global fit, which read the bump partly as a shift: Ruling F5) |
| Ruling F5: a smooth 3 % gain field between the halves, no shift (seeds 3 / 7) | the 0.21-0.30 band over the noise-only pair x1.006 / x0.998 against gain-only x1.005 / x0.994 (x1.512 / x2.016 before bench6); the global fit reads no shift (within 0.0022 px) |
| `noise.photon_transfer` refuses: fewer than half the bins on the line; a collapsed slope; `box_noise` past 1.5 x the fitted levels | NoiseUnmeasurable (tests) |
| `stack-region-probe --keff-selftest`: one region keeping 37 of 100 / the fallback / two blended nested regions (weights written from the layout geometry, not AxisWeightAt) | 37 / 100 / 62.83, all exact |
| End to end through the engine: synthetic RGGB, known noise, the measured photon-transfer line, the region probe's k; both cases held out of the calibration | disc x0.960 (calibrated 0.862); lunar with per-frame seeing (30 % steady frames, the rest a further sigma 1.5 px; ruling E3), k 83 and 78 = its steady frames per half, x0.785 (0.861). With the half's 300 frames as k it would read x2.93. (Fix round 3 read k 18 and 16 on a static scene, an artefact of the generator's wrapped edge.) |
| Ours charged its true added noise (the Ruling P28b mechanism) | uniform x1 / x2, target-only x2: END no longer rises |

### Split-half check and our measured noise (Ruling P28 rule 3, Ruling P28b)

The runner also stacks each capture's even and odd frames separately. Both halves' noise-power k are
taken over half A's target box, where the difference is measured, B's in its own grid (A's box moved
by the registration's integer shift; reviews N-m3, N2-m5). B is registered to A, moved by an exact
Fourier shift, and refined by least squares: A ~ g B + dy dA/dy + dx dA/dx. D is (A - g B') / 2 on the
box, and its power per END bin is compared with the model, sigma_frame^2 (1/k_A + 1/k_B) / 4. Without
the refinement, a residual misregistration of hundredths of a pixel leaks signal into D: on the 13 px
Mars disc, 0.017 px of it read as x10 the model. The shift (dy, dx) is fitted jointly with each 32 px
tile's level field, a cubic that stays in D (Ruling F5, below).

**Local registration is not charged (Ruling P28d, fix round 4).** After the global refinement, each
32 px tile of the box has its own shift field removed: D ~ L(u, v) + dy(u, v) dA/dy + dx(u, v) dA/dx,
with dy and dx first order in the tile's coordinates and L the tile's level field. Only the shift terms
are removed; L is fitted so a level offset is not read as a shift, and stays in D. The engine's region
stacks register region by region and blend with sin^2 weights between half-overlapping windows, so the
halves' shift difference varies smoothly node to node; a first-order field per tile follows it, one
constant shift per tile does not. Measured (`test_end.py`, a synthetic pair with that field: nodes 64 px
apart, 0.1 px sd per node, drawn 4 x finer and area-binned; the noise-only and local-shift tests print
these rows, at bench6 with the cubic level field):

| D per END bin over the noise model | 0.06-0.09 | 0.09-0.12 | 0.12-0.15 | 0.15-0.30 |
|---|---|---|---|---|
| Noise only, no tile shifts | 0.97 | 0.91 | 0.93 | 0.93-1.03 |
| Noise only, tile shifts removed | 0.99 | 0.90 | 0.94 | 0.92-1.04 |
| Local shift, no tile shifts | 5.44 | 3.11 | 1.94 | 1.04-1.51 |
| Local shift, one constant shift per tile (fix round 4, one intercept per tile; a variant not in the tree) | 2.77 | 1.69 | 1.31 | 1.04-1.15 |
| Local shift, first-order field per tile (the bench) | 1.19 | 0.98 | 0.97 | 0.95-1.05 |

So the local-shift pair charges at most 1.21 against the noise-only pair, and B at twice the noise
still charges 2.47-2.66 (2.5 expected). The fit costs 6 of each tile's 1024 noise degrees of freedom,
mostly in the lowest bins; the calibration measures with it, so that cost sits in R_cal as in R.
(Fix round 4, with one intercept per tile, read 0.90 / 0.89 / 0.93 for noise only and 4.75 / 1.09 for
the local shift without / with the tile shifts in the lowest bin.)

**A level or gain difference is charged as itself (Ruling F5, bench6).** The halves' kept subsets
differ in level and gain the way their shifts differ, smoothly across the frame. A level that slopes or
bends within a tile correlates with the shift terms wherever A's own brightness has a slope, so with one
intercept per tile (and none in the global fit) a smooth gain difference was fitted as a shift, and its
removal wrote A's gradient into D. Each shift fit now carries a cubic level field per tile (the global
one by Frisch-Waugh: both sides less their tiles' level fit); a cubic holds a field of period >= 128 px
(nodes 64 px apart) across a 32 px tile to ~2 %. Measured on synthetics with known truth only
(`test_end.py`; B = A x (1 + a 3 % field), no shift, noise 0.4 % of the mean, the 0.21-0.30 band over the
noise-only pair, seeds 3 / 7):

| Shift fits' level field | one intercept (until bench6) | first order | second order | cubic (the bench) | gain-only difference (the truth) |
|---|---|---|---|---|---|
| 0.21-0.30 band over the noise-only pair | x1.512 / x2.016 | x1.147 / x1.303 | x1.025 / x1.031 | x1.006 / x0.998 | x1.005 / x0.994 |

The global fit read the field as a (+0.0989, -0.1810) px shift (seed 3) before, within 0.0022 px now. A
2 % gain bump at a limb charges x1.17 in the lowest bin with the tiles against x1.18 for the bump itself
(gain-only); before bench6 the shift fits read it as x5.05 (tiles) and x2.79 (global only).
The cost depends on the box. The calibration's disc boxes hold 16 tiles each (calibration.json tiles.n); every real disc
box is one tile (48x48 to 57x48). Measured at c3762719 (noise-only synthetic disc pairs, tile fit
against global refinement only, mean of 16 noise seeds): one tile (56x58 box) reads 1.001 1.004 0.999
in the three lowest bins, 16 tiles (130x130 and 145x145 boxes) 0.918 / 0.907, 0.951 / 0.974, 0.980 /
0.977, and 0.99-1.00 above 0.15. So R_cal's 0.06-0.09 bin is about 9 % lower than a single-tile real
box would give, and the lowest charges on the real disc boxes are about 10 % high there, 3-5 % in the
next bin: the safe direction (ours is over-charged, never under-charged). No disc verdict sits near
that: all four disc captures are review for other reasons.

**Calibration** (`halves.calibrate`, frozen in `bench/calibration.json`, fix round 4):
* 48 synthetic RGGB captures, 24 per target kind (a banded disc; a lunar field with a limb);
* 6 static scenes per kind, each at 0.3 and 0.7 px of per-frame jitter, each without and with a 3 DN
  black pedestal (review N3-m6: every real capture but the 2026-01-30 Jupiter fits a negative
  intercept; with the pedestal the synthetic sky rounds to one 8-bit value and the fitted line is
  recovered on lunar at a 0.1487-0.1497 m, c -0.322 to -0.313, and on the disc at a 0.1469-0.1752 m,
  c -0.468 to -0.317 (a 7-17 % high at 0.7 px jitter, the read-high below), true 0.15 m - 0.317;
  figures from calibration.json at c3762719);
* 8-bit, noise a s + c, 600 frames; shifted as a mirror extension, so no edge wraps (ruling E3);
* split, stacked by stack-probe, k from the region probe over A's box (B's in its own grid);
* D with the tile shifts removed (P28d, above);
* the ratio taken with the MEASURED photon-transfer line (`noise.photon_transfer` on the first 128
  frames, as a real capture is measured; review I-F), so the estimator's read-high sits in R_cal as
  it sits in R.

There D holds only the engine's own noise. The synthetics are static, so the halves never choose
different frames: unshared signal is absent by construction. On real data it can only raise R. With
the E3 fix every half keeps 294-300 of its 300 frames over the box (fix round 3: 5-32 on 6 of the 12
lunar runs, an artefact of the generator's wrapped edge, `e3-report.md`).

| Kind | 0.21-0.30: mean / sd (ddof 1, 24 runs); range | log R: mean / sd | Tolerance exp(mean ± 5.81 sd) | R_cal per END bin (0.06 ... 0.30) |
|---|---|---|---|---|
| disc | 0.862 / 0.078; 0.719-1.026 | -0.152 / 0.091 | 0.507-1.456 | 0.82 0.80 0.89 0.84 0.96 0.93 0.86 0.80 |
| lunar | 0.861 / 0.133; 0.582-1.003 | -0.163 / 0.176 | 0.305-2.366 | 0.94 0.85 0.86 0.90 0.88 0.90 0.85 0.84 |

* 5.81 is the two-sided normal tolerance factor (Howe) at 99 % coverage and 95 % confidence for n = 6,
  the number of scenes: the scene dominates the spread (review N2-m2), so the 24 runs are not 24
  independent draws. The lunar scene 2 reads 0.582-0.590 at both jitters and both pedestals.
* The pedestal does not move the ratio: disc 0.863 without, 0.861 with; lunar 0.861 and 0.861.
* Jitter moves the disc: 0.905 at 0.3 px, 0.819 at 0.7 px, where the photon-transfer line reads
  +4 to +17 % high (a moving textured disc). It barely moves the lunar runs: 0.865 and 0.858.
* **The interval is taken on log R** (review N3-m1). R is a ratio of powers, positive, and a leak
  scales it, so its spread is multiplicative. Fix round 3 took mean ± k sd on R, and the lunar lower
  end came out below 0 (clamped to 0), so below-band review could never fire on lunar. On log R both
  ends are real: the lunar band's lower end is 0.305, about half the lowest calibration run (0.582).

**What "before any real data is read" guarantees, and what it does not:**
* `calibrate` opens no capture (`synthetic_frames` only).
* `calibration.json` is committed on its own, before the real-data run that uses it, which git
  history shows (fix round 3: 3ff1167f, re-frozen in ee79ddfd after the photon-transfer fit changed;
  fix round 4: 862a0cd9, before the round-4 real run).
* Not claimed: that the procedure predates real data. Earlier rounds read real split-half ratios, and
  several procedure changes were made after reading real results:
  * the global refinement above, after Mars's real x10.3 (review N2-m7);
  * the photon-transfer fit's free intercept (fix round 3), after a through-origin refit had put
    Mars's real END ratio at 0.950 (round 2 had 1.188; the final fit gave 1.218; re-review 3 N3-m2).
    Re-review 3 judged the free intercept physically correct and found no verdict hinging on it, but
    it was adopted after that real result was read;
  * fix round 4's tile shifts (Ruling P28d), log-scale tolerance (N3-m1) and pedestal runs (N3-m6)
    were each decided after fix round 3's real split-half ratios had been read;
  * after the round-4 real run (2026-10-23 14:42-14:56; frozen 14:41), the 2023-02-12 Moon's clipped
    south limb showed the tile fit can raise D. Two alternatives were measured then and not adopted
    (a smoothly interpolated shift field under-charges a level offset; keeping each tile's mean leaves
    more of a local shift), and the procedure is unchanged: the run stands on the frozen calibration;
  * bench6 changed `refined_difference` after the final fix round's real run (b35e770c) had been read:
    every shift fit now carries a cubic level field per tile (Ruling F5, above). The defect was found
    on synthetics (the final fix round's report, concern 1; measured again by its re-review) and the
    change was decided and proven on synthetics with known truth only (`test_end.py`: the gain-only
    pairs, the shift-only and noise-only tests unchanged and passing); no real capture was opened to
    choose it. `calibration.json` was re-frozen with it before the bench6 real run.

**Use:**
* **The charge (Ruling P28b).** Ours is charged N(f) = budget x max(1, R(f) / R_cal(f)) per END bin,
  with R this capture's measured ratio and R_cal its kind's. Unmodelled noise, unshared signal and
  any sharpening of ours all raise R, and are charged. The references stay on the budget.
* **The tolerance (review I-G).** A capture whose 0.21-0.30 ratio lies outside its kind's band, in
  either direction, cannot pass. Above it, the model does not hold what the halves measure. Below it,
  ours smooths more than the honest engine or sigma_frame is over-read; either way the charge is not
  confirmed. It does not block a fail, which has its own checks (P28c, provenance).
* **P28c.** A numeric fail under the charge is sound only if it also holds with ours on the bare
  budget, on both readings of our k (review N2-m1); otherwise it is review.

**The fix-round-4 run** (2026-10-23, the E1/E2 engine at b0f3bb43, calibration 862a0cd9, P28d tile
shifts). The 0.21-0.30 ratio of fix round 3 is beside it; what moved it is in the last column.

| Capture | Kind | 0.21-0.30 measured / model (round 3) | Tolerance | Ours' charge, 0.06-0.09 ... 0.27-0.30 | Without the tile shifts: 0.21-0.30 / 0.06-0.09 |
|---|---|---|---|---|---|
| 2023-02-11 Jupiter | disc | x1.14 (x1.12) | agrees (0.51-1.46) | 6.30 2.48 1.06 2.77 1.59 1.12 1.28 1.58 | x1.12 / x8.19 |
| 2026-01-30 Jupiter | disc | x3.72 (x3.95) | outside | 11.7 11.9 10.7 13.4 2.13 5.58 4.58 2.56 | x3.95 / x31.9 |
| Saturn | disc | x0.73 (x0.72) | agrees | 1.88 1.00 1.21 1.92 1.59 1.02 1.00 1.00 | x0.72 / x2.08 |
| Mars | disc | x1.74 (x2.18) | outside | 6.42 5.00 4.51 3.64 1.45 1.53 2.39 2.19 | x2.18 / x6.18 |
| 2023-02-12 Moon | lunar | x1.27 (x1.22) | agrees (0.31-2.37) | 15.8 8.94 5.76 3.44 2.49 1.78 1.43 1.18 | x1.23 / x30.9 |
| 2024-02-01 Moon | lunar | x0.95 (x2.17) | agrees | 1.24 1.27 1.20 1.15 1.15 1.09 1.14 1.07 | x2.20 / x15.1 |
| 2023-08-22 Moon | lunar | x0.82 (x0.82) | agrees | 1.66 1.39 1.20 1.06 1.04 1.00 1.00 1.00 | x0.83 / x2.47 |
| 2023-03-05 Moon | lunar | x0.48 (x0.56) | agrees | 1.12 1.18 1.00 1.00 1.00 1.00 1.00 1.00 | x0.56 / x18.3 |

* Without the tile shifts the ratios are fix round 3's to within 0.03: the E1/E2 engine changes
  moved the halves little. The movement is the tile shifts'.
* On the disc boxes (48-57 px) the box is a single tile, so the first-order field is one global
  affine (scale, rotation, shear) difference between the halves; it takes Mars from x2.18 to x1.74 and
  the 2026-01-30 Jupiter's lowest bin from x31.9 to x9.56. Both stay outside.
* The 2023-03-05 Moon (x0.48) is below every lunar calibration run (0.582) and inside the band.
* The 2023-02-12 Moon's south limb is clipped in the raw frames (54 % of the samples at 255 in a
  window over it). There the halves differ by about 10 % in level (A 0.92 against B 0.81 at x 1235),
  and on 25 of its 1632 tiles the shift fit returns more than 1 px, which is not a shift; with them
  the fit raises D's total power by 0.7 %. Measured at c3762719 (the capture's halves re-measured with
  those 25 tiles' fields left unremoved, or capped at 1 px, everything else frozen): the 25 fits lower
  the charge in the 0.06-0.15 bins (19.1 -> 15.8 in the lowest) and raise it in 0.15-0.30 (1.13 -> 1.18
  in the top bin). Net on END they are neutral for ours: END_interior 1.020 with them against 1.018
  without (+0.0017), and 1.059 with the field capped at 1 px.

**The final fix round's run** (2026-10-23 18:23-18:38; the engine with the final review's fixes,
calibration re-frozen at b35e770c, generated 18:23:10, with its identity) gives every verdict and every
number of the table above again: 4 pass, 4 review, 0 fail; the diagnosis cases pass. Against the
round-4 run each stack differs by at most 1.9e-6 per pixel and each END ratio by at most 5e-5 (float
rounding of the threaded sums), the frames used and our k are the same, and every split-half ratio
agrees within 4e-5. None of the engine fixes changes a frame choice on these captures (the Unaligned
change acts only on selections under 2 x kMinStackFrames).

### What the excess is (`triage.py`; fix round 3, review I-E; re-measured in the final fix round)

`triage.py halves` re-stacks both halves with `stack-region-probe --fits-out`: the region stack (the
engine's flow) and the global-only stack of the same frames (no regions, so no kept subsets and no
level offsets between them; Task 9 Investigation 2). It prints the bench's R for each (tile shifts
removed, Ruling P28d) and decomposes D of each tile by tile (32 px) in the END band into a gain on A (a
level or gain offset between kept subsets multiplies the texture), a shift (the halves registered
differently, locally or as a whole) and a local blur. D is taken after the registration and one global
gain only: fix round 3's decomposition took it after `refined_difference`'s global gain-and-shift fit,
which turned a smooth gain field into a pseudo-shift until bench6 (Ruling F5), so it read a level offset as a local shift (a pure
3 % gain field: 0.13 gain / 0.82 shift; `test_triage.py`, final review M4). Re-measured with the fixed
decomposition on the final fix round's run (calibration b35e770c; `triage.py halves`, at 047d3c50's
triage):

| Capture | Region stacks: 0.21-0.30 / 0.06-0.09 | Global-only: 0.21-0.30 / 0.06-0.09 | Region stacks' D explained by gain / shift / local blur | Global-only's D: gain / shift / blur |
|---|---|---|---|---|
| 2024-02-01 Moon | x0.95 / x1.17 | x0.94 / x1.17 | 0.008 / 0.854 / 0.009 | 0.022 / 0.230 / 0.033 |
| 2023-02-12 Moon | x1.27 / x14.9 | x1.92 / x5.18 | 0.121 / 0.597 / 0.078 | 0.026 / 0.138 / 0.039 |
| 2026-01-30 Jupiter | x3.72 / x9.56 | x1.77 / x7.71 | 0.001 / 0.820 / 0.001 | 0.002 / 0.713 / 0.000 |
| Mars (no regions) | x1.74 / x5.23 | the same | 0.003 / 0.956 / 0.004 | the same |

* **The 2024-02-01 Moon.** The tile shifts already took its excess out (x0.95, inside the band), and
  what is left of D is still mostly shift (0.85), a gain on A 0.8 %: with the fixed decomposition a
  level offset would read as gain, so the region stacks' level offsets are not what D holds here.
  Fix round 3's reading (0.847 shift, 0.008 gain, before the tile shifts) stands.
* **The 2023-02-12 Moon.** Mostly shift too (0.60), but a gain on A carries 12 %: the capture
  Investigation 2 found the level offsets on. Its low bins stay charged (x14.9) after the tile shifts.
* **The 2026-01-30 Jupiter.** Shift explains 82 % of the region stacks' D and 71 % of the global-only
  stacks'. On a 52 x 56 box the tile is the whole box, so this "shift" is the halves' residual global
  and affine registration (README, round-4 run). Fix round 3 read 44 % (an untested guess at the gap: the global fit it decomposed after had
  already taken the global part).
* **Mars** has no regions. Shift explains 96 % of D: the same global / affine residual on a one-tile
  48 px box; fix round 3, decomposing after the global fit, found no term over 12 %.

A shift between the two half-stacks is registration, not noise; in the full stack it is a geometric
offset that END does not see, and P28d's per-tile first-order fit removes it from R. On the disc
boxes that fit is one global affine field; the 2026-01-30 Jupiter and Mars still read outside their
band after it (x3.72 / x1.74), and what the rest of their D is, is not established.

### What the interleaved halves share (fix round 2, review I-D)

Interleaved halves share structure that D cannot see. On the 2026-01-30 Jupiter, the off-axis
0.30-0.50 power of a half is 122 x the halves' difference.

Round 1 called it a sensor-fixed pattern (pixel-response non-uniformity or warm pixels). **That is
withdrawn: a discriminating test falsifies it.** A pattern fixed in sensor coordinates must follow the
sensor drift between the capture's first and second halves (about (-1.6, +1.4) px). The test
cross-correlated the off-axis high-pass of the two time-half stacks, registered on the target, on the
disc:
* the correlation is 0.033 at zero shift;
* it is at most 0.008, and mostly negative, at every shift of 1-2 px along the drift;
* the time halves therefore share neither target-locked structure (only 3 %) nor sensor-locked
  structure.

Frame differences at lags 1 to 32 show the per-pixel noise is temporally white: the disc's
high-passed difference variance is 2.77-2.91 DN² at every lag. The leading hypothesis, **untested**,
is that adjacent frames see nearly the same atmosphere, so even and odd frames average the same
residual seeing distortions while the two time halves do not. No dark or flat for that night is on
the NAS (`processing-archive-2026-10-03`).

Whatever it is, the interleaved split cannot measure it. Ours carries it uncharged, and so do the
references, which stacked the same frames.

**Row banding: withdrawn (fix round 3, `triage.py rows`).** Round 2 said the 2026-01-30 Jupiter's raw
frames carry row banding and that it was most of the split-half excess. A discriminating test
falsifies that. Sensor banding is constant along a sensor row, so the sky left and right of the disc
in the same rows would share it:
* raw frames: the covariance of the two sky segments' lag-1 difference row means is -3.74e-6 DN²
  (29 disc rows x 159 differences; re-run in fix round 4, and noise.py's "-4e-7" corrected to it),
  so there is no shared row offset;
* D: re-run on the round-4 halves (`triage.py rows`), the sky segments' covariance is -1.98e-14 stack
  units² against 6.03e-09 for the disc's left and right halves, a ratio of -3.3e-6 (fix round 3's halves
  read 8.5e-7), so D carries no shared row offset either.

Round 2's 0.0063-0.023 DN² was the variance of whole-row means, disc included, so it measured the
disc's frame-to-frame change, not the sky's. The test ran on this capture only; whether the other
captures' raw frames carry banding is not tested. The review's arithmetic (sky-level additive banding one
to two orders too small for x28) holds with room to spare.

What D does hold on the disc is row structure that scales with level. Its variance of row means peaks
at -8 deg, and Jupiter's belts at -9 deg. A fit of belt-aligned plus sensor-row parts gives the
sensor rows 0.61 of it, leaving 0.45 of it unfitted. The raw frames' consecutive differences on the
disc are 0.86 belt-aligned. So part of it follows the target. Whether the rest is level-scaled
sensor-row structure is not established (an untested guess: scintillation read out by the rolling
shutter). Either cause raises R, so it is charged to ours and not to a reference.

## The secondary metric: DAN and DAN_interior

DAN was Ruling 2's yardstick until Ruling P28. It stays in every table as a secondary figure and
decides nothing.


Full definitions and reasoning are in the `metric.py` docstring. In short, for each image on the
same registered target box:

1. luminance (channel mean); 2. one crop for all images, chosen on the anchor (the target's
largest bright component plus a 15 % margin of sky; a lunar disc gets the frame minus a 2 % inset);
3. normalise: subtract the background b (1st percentile of the crop low-passed at sigma 4) and
divide by the mean target signal (mean - b), so gain and offset drop out and blur cannot move it;
4. Hann-windowed power spectrum in variance-per-pixel units; 5. noise floor = median power in the
0.40-0.50 cycles/px annulus / ln 2; 6. **DAN = sqrt(sum over 0.06-0.30 cycles/px of (P - floor) / n)**:
the RMS amplitude of the detail on 3-17 px scales, as a fraction of the mean target signal, with
the noise power removed.

**DAN_interior** asks the same question of the surface alone. The window is the anchor's target
mask eroded by max(3 px, 2 x the limb's blur sigma), with a 2 px cosine edge. The limb sigma is
the **sharpest of the capture**: ours and every reference, each measured on its registered crop.
The window, and whether the interior is judged at all, is then a property of the capture, not of
whoever is anchor. Without this, a softer stack of ours would erode itself under 20 px and escape
the interior bar: a synthetic R 13 disc at seeing 2.5 gets 19.7 px from its own limb and 23.8 px
from the capture's, where it fails the bar at 0.656 (`test_a_softer_ours_cannot_escape_the_interior_bar`). Inside it, x is
high-passed against its sigma-3 mean over target pixels only (normalised convolution, so the limb
step does not leak inward). The result is that high-pass's RMS minus its own noise (the interior's
noise floor, measured in the interior). It is judged only when the eroded disc is at least 20 px
across; below that the comparison is "limb-only: human review decides".

Why: seeing and misalignment are low-pass filters and take power out of that band. A plain
gradient or Laplacian energy would reward noise (a stack of fewer frames would "win" by being
noisier), whereas DAN subtracts each image's own noise floor, so a 20 % stack and a 50 % stack are
compared on the detail they carry. It is translation invariant, so references are cropped at their
registered integer offset and never interpolated. Flat (unweighted) band power was chosen over
frequency-weighted variants after measuring both: f^2 weighting is more blur-sensitive (-25-26 % vs
-16-17 % at sigma 1), but its seed-to-seed scatter under heavy noise is up to +-7 %, against +-1-3 %
for flat, and the pass bar is 3 %.

### Why the interior score (measured)

DAN_full is limb-weighted on small discs. Synthetic banded disc, noise 0.003, ratio to the sharp
disc as full / interior:

| Disc radius (eroded diameter) | Interior smeared s1, limb crisp | Interior smeared s3, limb crisp | Global blur s0.7 | Global blur s1 |
|---|---|---|---|---|
| R 7 (11 px: limb-only) | 0.995 / not judged | 1.006 / not judged | 0.957 | 0.924 |
| R 15 (26 px) | 0.927 / 0.884 | 0.827 / 0.751 | 0.918 / 0.927 | 0.853 / 0.873 |
| R 21 (37 px) | 0.851 / 0.858 | 0.540 / 0.595 | 0.899 / 0.920 | 0.812 / 0.852 |
| R 50 (93 px) | 0.829 / 0.814 | 0.457 / 0.435 | 0.892 / 0.894 | 0.805 / 0.813 |

At R 7 the metric cannot see an interior softened even by sigma 3: it measures the limb only. On
the real 29 px 2_12_2023 AS_P20 Jupiter, blurring only the interior (more than 2 px inside the
limb) costs DAN_full 2.7 % at sigma 0.7, 5.2 % at sigma 1 and 15 % at sigma 2. So an interior
softened by sigma 0.7 behind a crisp limb passes the 0.97 bar on DAN_full. Its limb sigma
estimates 2.33 px, so the interior window is eroded 4.7 px, and the eroded disc is 18.8 px: by the
20 px rule that capture is limb-only. In the bench only the 2026-01-30 Jupiter carries the interior
bar (27.98 px with ours as anchor); Saturn, 23.5 px from its reference alone, measures 17.37 px in the
bench and is limb-only, as are the 2023-02-11 Jupiter (18.15 px) and Mars (6.28 px) -- the final fix
round's run at b35e770c, each run's compare.json `interior_diameter` (summary.md's limb-only notes).
Why Saturn shrinks: see "The benchmark set" below (ruling F1).

### Validation (measured 2026-10-22)

Blur (Gaussian sigma added, px) -- strictly decreasing everywhere:

| Image | sigma 0 | 0.5 | 1.0 | 2.0 |
|---|---|---|---|---|
| synthetic textured disc | 0.09804 | 0.09420 | 0.08237 | 0.05472 |
| synthetic lunar field | 0.02286 | 0.02192 | 0.01901 | 0.01252 |
| 2_12_2023 Jupiter AS_P20 | 0.32571 | 0.31766 | 0.29300 | 0.23749 |
| 2_12_2023 Moon AS_P20 | 0.06876 | 0.06620 | 0.05817 | 0.03894 |

Added white noise (sigma as a fraction of the mean target signal) never raises DAN. It stays
within 1.1 % while the band SNR falls by up to two orders of magnitude:

| Image | none | 0.005 | 0.01 | 0.02 |
|---|---|---|---|---|
| synthetic disc (abs. sigma) | 0.09804 | 0.09788 | 0.09760 | 0.09697 (band SNR 449 -> 4.6) |
| synthetic lunar (abs. sigma) | 0.02286 | 0.02277 | 0.02270 | 0.02270 (band SNR 144 -> 1.5) |
| 2_12_2023 Jupiter AS_P20 | 0.32571 | 0.32541 | 0.32521 | 0.32457 |
| 2_12_2023 Moon AS_P20 | 0.06876 | 0.06872 | 0.06869 | 0.06862 (band SNR 1035 -> 42) |

A blurred (sigma 1) copy plus noise 0.02 still measures < 0.9 x the original (noise cannot buy back
blur). DAN_interior drops 13 % for a global blur of sigma 1 and stays within 1.3 % with noise 0.01.

Repeatability on planet-sized crops (synthetic disc, 20 noise seeds, same box): boxes of 33-106 px
give a seed-to-seed sd of 0.07-0.14 % at noise 0.002, 0.18-0.38 % at 0.005 and 0.35-0.75 % at
0.01; the full range never exceeds 2.8 %. Measurement noise alone stays inside Ruling 2's 3 %.

AutoStakkert's own selection (fewer, sharper frames -> more detail), same capture, one box:

| Capture | Fewer frames | More frames | Ratio | Status |
|---|---|---|---|---|
| 2_12_2023 Moon 024708 | AS_P20 0.06876 | AS_P50 0.05266 (both native) | 1.31 | reported (asserted until Ruling P28: the pair is confounded, ap1 vs ap2; END ranks P50 first, 16.80 vs 13.20, as tabled above) |
| 2_12_2023 Moon 024708 | AS_P20 0.06876 | AS_P25 (x3, resampled) 0.05585 | 1.23 | recorded, confounded by the resample |
| 2_12_2023 Jupiter 191831 | AS_P20 0.32571 | AS_P25 (x3, resampled) 0.31714 | 1.027 | recorded, confounded by the resample |
| 3_6_2023 Moon 224808 | AS_P20 (x3) 0.02665 | AS_P50 (x1.5) 0.02637 | 1.011 | recorded (within noise) |
| 2026-01-30 Jupiter 195830 | ASI_40 0.18930 | ASI_50 0.18868 | 1.003 | recorded (within noise) |

The noise floors move the right way too: on the 2_12_2023 Moon, AS_P50's floor is half of AS_P20's
(8.7e-6 vs 1.7e-5), yet its DAN is lower. The metric is not paying for noise.

### Known limit: sharpening

Under END, a sharpened image gains directly: an unsharp mask raises END x1.3-1.9 (see "Known limit:
noise and sharpening the budget does not hold (C1)" above). The rest of this section is DAN's case.

DAN is valid only between **unsharpened linear stacks**. A disc softened sigma 1.6 and then
unsharp-masked scores 0.95-1.01 x the genuinely sharp disc (above the bar at R 7 and R 15; pinned
as a known-limit test). No automatic detector separates the two:

* the noise-floor/band-power ratio is unchanged by unsharp masking (3.05e-4 vs 3.00e-4 at R 15),
  while the references' own ratios span 1e-6..7e-3;
* the limb undershoot moves only +1.2-1.5 %, while the references sit at -0.4..-38 % from glow;
* the sky-noise spectral tilt moves 0.74 -> 0.87-0.95, while the references sit at 0.001..0.6.

The bench therefore relies on what is known of each image's history:
* Our stack is unsharpened by construction in this increment: cubic B-spline interpolation, no
  deconvolution. Sharpening arrives with increment 2's sliders.
* ASIStudio's `.fit` is its unsharpened stack. Side by side with its `.jpg`, only the `.jpg` shows
  ringing halos and crisp craters (`test/output/bench-survey/asi-fit-vs-jpg.png`). The `.fit`
  header's `SHARPEN`, `PB`, `PC`, `PN`, `PS` match the `.jpg` name and record the export settings.
* AutoStakkert's `_conv` files and every `.jpg` are refused.

`compare` also flags ours for human review when its floor/band ratio lies outside
[min/30, max x 30] of the references'. Real tools differ from each other by up to 17.7x (8_23_2023),
so 10x would flag them. The note says it does not detect sharpening.

## References: facts measured on the NAS

* **Resampling factors come from size.** `factor_from_size` takes the nearest of {1, 1.5, 3} to the
  reference's width and height over the capture's frame (both within 10 %), and the label records
  any disagreement with the AutoStakkert name. Two `.fit` files named Drizzle15 are x3:
  `2_1_2024/AS_P60/...Drizzle15.fit` (5688x3168: 2.96 / 2.93) and `3_6_2023/moon/AS_P20/...Drizzle15.fit`
  (5664x3120). Their labels read "named x1.5, size x3". AS PNGs match their names.
* **ASIStudio debayers RG.** Every ASI `.fit` header carries `VSBAYER = 0` (0:RG 1:BG 2:GR 3:GB).
* **Orientation differs between tools.** AS `.fit` files are flipped vertically against the video
  and against ASIStudio's `.fit` (2_1_2024, 8_23_2023, 3_6_2023); AS PNGs and ASI `.fit` match the
  video. Registration chooses among identity/flipud/fliplr/rot180 and records the choice. The one
  exception is Venus, whose AS PNGs read rot180: a featureless clipped 17 px disc whose orientation
  the data cannot settle. It is a diagnosis case and never compared.
* ASIStudio produced an image (`..._Planet_40.fit`) from the empty 2026-01-30 Saturn capture. It
  registers to nothing (best coarse NCC 0.296).

The survey registered every reference against the mean of its video's first 32 frames
(2026-10-22). Lunar scale residuals were 0.04-0.31 %, NCC 0.76-0.998. Planets were 0.2-4.7 %:
against a blurred raw mean the small-disc scale reads high. The tolerance is max(1 %, 1 px /
target size), which still separates x1 / x1.5 / x3.

## The benchmark set

Frame counts, sizes, sidecars, reference files, size-derived factors and recorded patterns are
checked against the NAS by `test_benchset.py`. Paths are under `/mnt/qnap/astro_data/`. "Eroded"
is the interior window's equivalent diameter (>= 20 px: interior judged) from the references alone
(`benchset.py survey`, re-run at b35e770c: 18.8 / 28.5 / 23.5 / 7.4 px). Once our stack joins, the
sharpest limb of ours and the references sets how far the window is eroded, but the outline it erodes
is ours' own target mask (`refs.compare`: the anchor's 15 % contour). So a sharper stack of ours
cannot shrink the window through its limb, but a stack of ours with a TIGHTER contour shrinks it all
the same, and can take a capture under 20 px and out of the END_interior bar: Saturn reads 23.5 px
from ASIStudio's own mask and 17.37 px in the bench (ours' mask), so in the bench it is limb-only, and
only the 2026-01-30 Jupiter (27.98 px) carries the interior bar; the 2023-02-11 Jupiter reads 18.15 px
and Mars 6.28 px (final fix round's run at b35e770c, compare.json `interior_diameter`). This is what
the code does, stated rather than changed after real results were read (ruling F1). DAN values compare
reference against reference on the capture's common box; our stack joins in Task 9's run.

### Planets

| ID | Video | Frames, size, file | Bayer (source) | Eroded | References | DAN (interior) | Why |
|---|---|---|---|---|---|---|---|
| 2023-02-11-jupiter | `2_12_2023/2023-02-11-191831-Jupiter-Bin1 13.3F.avi` | 6516, 640x480, 2.0 GB | RGGB (camera spec; AS used BGGR) | 18.8 px: limb-only | `2_12_2023/AS_P20/2023-02-11-191831-Jupiter-Bin1 13.3F_lapl5_ap1.png` (x1); `2_12_2023/AS_P25/..._lapl5_ap1_Drizzle30.png` (x3) | 0.3257; 0.3171 | AS native and drizzled |
| 2026-01-30-jupiter | `processing-archive-2026-10-03/videos/2026-01-30-195830-Jupiter-Bin1 Lqef -4.2F.avi` | 4088, 480x360, 0.7 GB | RGGB (camera spec = Scott's Save pattern; ASI same) | 28.5 px | `.../ASIVideoStack_Output/2026-01-30-195830-Jupiter-Bin1 Lqef -4.2F_Planet_40.fit`; `..._Planet_50.fit` | 0.1893 (0.2291); 0.1887 (0.2283) | Larger Jupiter: interior judged |
| 2023-09-29-saturn | `9_30_2023/2023-09-29-204604-Saturn-Bin1 LPro 14.9F.avi` | 3973, 1280x960, 4.9 GB | RGGB (camera spec; ASI same) | 23.5 px (17.37 px in the bench: limb-only) | `9_30_2023/ASIVideoStack_Output/2023-09-29-204604-Saturn-Bin1 LPro 14.9F_Planet_74.fit` | 0.5828 (0.6038) | Rings; small target in a large frame |
| 2023-02-11-mars | `2_12_2023/2023-02-11-192403-Mars-Bin1 16.5F.avi` | 13 251, 320x240, 1.0 GB | RGGB (camera spec; AS used BGGR) | 7.4 px: limb-only | `2_12_2023/AS_P25/2023-02-11-192403-Mars-Bin1 16.5F_lapl5_ap1_Drizzle30.png` (x3; bright edge strip, cols 305-317) | 5.870 | Smallest disc; longest capture. **Limb-only: human review decides** |

### Lunar

All four are lunar discs or limbs with sky in the frame, not frame-filling surface.

| ID | Video | Frames, size, file | Bayer (source) | References | DAN (interior) | Why |
|---|---|---|---|---|---|---|
| 2023-02-12-moon | `2_12_2023/2023-02-12-024708-Moon-Bin1 14.9F.avi` | 2354, 1920x1080, 4.9 GB | RGGB (camera spec; AS used BGGR) | `2_12_2023/AS_P20/2023-02-12-024708-Moon-Bin1 14.9F_lapl5_ap1.png` (x1); `2_12_2023/AS_P50/..._lapl5_ap2.png` (x1); `2_12_2023/AS_P25/..._lapl5_ap2_Drizzle30.png` (x3) | 0.0688 (0.0732); 0.0527 (0.0587); 0.0559 (0.0603) | Metric validation: P20 and P50 both native |
| 2024-02-01-moon | `2_1_2024/2024-02-01-013847-Moon-Bin1 Lqef 14.9F.avi` | 1310, 1920x1080, 2.7 GB | RGGB (camera spec = its Save-pattern sidecar; AS and ASI same) | `2_1_2024/ASIVideoStack_Output/2024-02-01-013847-Moon-Bin1 Lqef 14.9F_Surface_40.fit`; `2_1_2024/AS_P60/..._lapl5_ap16_Drizzle15.fit` (named x1.5, size x3) | 0.1320 (0.1465); 0.1415 (0.1556) | Both tools on one capture |
| 2023-08-22-moon | `8_23_2023/2023-08-22-213239-FOV-Bin1 LPro 15.8F.avi` | 1352, 1920x1080, 2.8 GB | RGGB (camera spec; AS and ASI same) | `8_23_2023/ASIVideoStack_Output/2023-08-22-213239-FOV-Bin1 LPro 15.8F_Surface_35.fit`; `8_23_2023/AS_P20/..._lapl5_ap4_Drizzle30.fit` (x3) | 0.0300 (0.0248); 0.0352 (0.0288; band SNR 149) | Both tools; named "FOV" but a lunar limb (checked visually) |
| 2023-03-05-moon | `3_6_2023/moon/2023-03-05-224808-Moon-Bin1 18.3F.avi` | 1705, 1920x1080, 3.5 GB | RGGB (camera spec; explicit, = the sidecar's RG, Flip = Vert(unimplemented) never applied; AS same) | `3_6_2023/moon/AS_P20/2023-03-05-224808-Moon-Bin1 18.3F_lapl5_ap4_Drizzle15.fit` (named x1.5, size x3); `3_6_2023/moon/AS_P50/..._jupiter_lapl5_ap4_Drizzle15.png` (x1.5) | 0.0267 (0.0271); 0.0264 (0.0269) | Sidecar; a second AS P20/P50 pair |

### Diagnosis cases (no image expected; stack-probe runs with Auto)

| ID | Video | Frames | Expected | References | Notes |
|---|---|---|---|---|---|
| 2026-01-30-saturn-empty | `processing-archive-2026-10-03/videos/2026-01-30-203627-Saturn-Bin1 Lqef -3.8F.avi` | 21, 1920x1080, 44 MB | NoTarget | `.../ASIVideoStack_Output/2026-01-30-203627-Saturn-Bin1 Lqef -3.8F_Planet_40.fit` | Frames hold only background (measured 2026-10-21); ASIStudio still wrote an image |
| 2023-02-11-venus-clipped | `2_12_2023/2023-02-11-190452-Venus-Bin1 15.8F.avi` | 6485, 640x480, 2.0 GB | Clipped | `2_12_2023/AS_P25/..._Drizzle30.png`, `2_12_2023/AS_P50/..._Drizzle30.png` | Every frame >= 25 % saturated (Task 3); the Bayer pattern is unmeasurable (91 % of the disc saturated); its DAN is the edge of a saturated disc and is not used |

Bayer sidecars: the 2023 Moon AVIs and the other 2026-01-30 captures have none. The exceptions are
`2026-01-30-195830-Jupiter-Bin1 Lqef -4.2F.avi.txt` and `2_1_2024/2024-02-01-013847-Moon-Bin1 Lqef
14.9F.avi.txt`, which SERViewer's Save pattern wrote (Bayer = RG; the second on 2026-10-22 23:11), and
`3_6_2023/moon/2023-03-05-224808-Moon-Bin1 18.3F.avi.txt`, which is the ASIAIR's own.

### Considered, not in the set

* `2_12_2023` Neptune (AS_P25 drizzled only): a fourth tiny disc adds little over Mars.
* `9_16_2023` Saturn and Jupiter, `9_22/Video` Jupiter (ASI only; 9_22's ASI output is mono):
  Saturn is covered by 9_30_2023, which has the larger frame.
* `4_7_2023` Moons (ASI only, 7.4-7.6 GB each), `5_27_2023` Moon (AS_P20 only, 7.5 GB),
  `2_20_2024` Moon (AS_P50 only), `2_2_23/Moon` (AS_P50 only): the chosen four lunar captures
  already cover both tools, both drizzle factors and a sidecar.
* `8_16/Video` 2022 Jupiters: 88-91 % of the disc saturated (measured by `avi_real_check.py`).
* The other 2026-01-30 Jupiters (ASI only, no sidecar): 195830 is the one with Scott's pattern.
* `3_31_2023/Moon`: no usable reference (spec Ruling 5).

## Task 9's run and the change of yardstick

Task 9's run judged on DAN and sent the 2026-01-30 Jupiter to review whatever the numbers said. The
reason given was that ASIStudio's suppressed Nyquist power makes its white noise floor read small.
Two captures failed on DAN although ours carried 2.1-9.7 x the reference's per-band SNR (the 2023-02-11
Jupiter at 0.915, the 2023-08-22 Moon at 0.945 / 0.968). Ruling P28 replaced the judged metric with
END, whose noise model does not read any image's own floor, and dropped the always-review rule: its
stated cause no longer bears on the verdict. The noise-regime review note (floor/band unlike every
reference) still fires on its own terms.

## The real-data run: run-stack-bench.sh

```bash
PLANETARY_BENCH_PYTHON=<venv>/bin/python PLANETARY_BENCH_STAGE=~/.cache/planetary-bench-stage \
    bash modules/planetary/test/run-stack-bench.sh [--report-only] [run-id ...]
```

`bench.py plan` lists one run per capture above, every colour capture stacked RGGB (the camera spec).
No sidecar is written.

For each run, stack-probe writes `test/output/bench/<run>/` (gitignored): `ours.fits`, `probe.json`
(selection, phase times, peak RSS, warnings, per-region kept counts), `probe.log`, `wall.txt` and
`source.txt`.

Unless `--no-split` is given, the runner then stacks the capture's even and odd frames separately,
for the split-half check. `halves.py split` writes two SER files to the stage directory (or the run
directory), and they are deleted afterwards; `halfA` and `halfB` (`.fits`, `.json`, `.log`) are kept.
For ours (on the compare box, from `bench.py box`) and for each half (on its own target box),
`stack-region-probe --plan-only --keff-box` writes the noise-power k (`keff.json`,
`halfA/halfB.keff.json`).

`bench.py report` then writes `compare.json` (every row of `compare --json`, plus the status, the
photon-transfer fit and the split-half record) and `side-by-side.png`. It builds one `summary.md`
over all runs:
* the main table;
* every image;
* the per-frame noise and the split-half check;
* the timing with phases;
* the diagnosis cases.

* **Status (Ruling 2 with Ruling P28's yardstick).**
  * **unmeasured**: END is missing for ours or for any reference (no per-frame noise, an unknown frame
    count, a region-probe plan that differs from RunStack's). No verdict is drawn and the reason is
    printed.
  * **fail**: END_full, or END_interior where judged, is below 0.97 x the best reference under both
    readings of our k, and that best reference -- every one setting the bar -- has provenance "none"
    (Ruling F3, above). The two readings are the box's noise-power k, which is judged, and the
    unweighted mean kept fraction, the flip check.
  * **review**: everything else, with its reasons:
    * a verdict that flips between the two readings (rule 5);
    * a numeric fail against a reference whose provenance is not "none" (C1);
    * a numeric fail under the charge that does not also hold with ours on the bare budget, on both
      readings (P28c);
    * a pass with ours uncharged on either reading (no split halves);
    * a pass whose split halves lie outside their kind's tolerance, in either direction (review I-G);
    * a review note (limb-only, noise regime, mosaic fallback) on a pass. On a fail the note is shown
      and the fail stands.
  * **pass**: END >= 0.97 under ours' measured-noise charge, on both readings, split halves within the
    tolerance, no review note.

  DAN is shown and decides nothing. Every capture still needs Scott's look at its side-by-side.
* **Side-by-side.** Ours is shown, then each reference, all cropped from the same region of ours'
  grid at the registered integer offset, at one scale and with one stretch per row. Each channel's
  black is its own 1st percentile, so a sky pedestal does not tint the sky. One white, the 99.5th
  percentile of the brightest channel, is applied to all channels, so each tool keeps its colour
  balance.
  * Planets get a context row (gamma 1/2.2: halos, moons, sky artefacts) and a zoom row of the metric
    box (nearest-neighbour, linear, so the belts are not compressed).
  * Lunar captures get an overview row and two 1:1 tiles at x2, on the most detailed parts of the
    surface that stay clear of the limb.
* **Timing.** With `PLANETARY_BENCH_STAGE`, each video is copied to local disk, synced and evicted
  from the page cache (`dd iflag=nocache`) before stacking. That makes it a cold local-disk read,
  the spec's condition. The copy and the sidecar are deleted afterwards. Without it, the runner reads
  straight from the NAS, and `source.txt` says so. AutoStakkert's time on the same file is not
  available.
* **Exit status.** 1 on a harness or engine error, otherwise 0. Pass, fail and review are in the
  summary.

`metric.target_box` grows a box under 48 px (`MIN_BOX_SIDE`) about the target, with sky. The first
run's Mars could not be compared at all: our sharper 13 px disc gave a 29 px box, under `compare`'s
32 px floor (`test_compare_runs_on_a_sharp_tiny_disc`). The extra sky is the same for every image,
so the ratios hold; absolute DAN values on small discs shift (2023-02-11 Jupiter AS_P20 0.3178 ->
0.4230 on the 48 px box).

`stack-probe --keep-best F` is an investigation knob, never a product one. It hands the engine only
the ceil(F N) frames of highest whole-frame quality, ranked by the engine's own MeasureAll. The
engine then selects among them as usual. Ruling 2's investigation round used it to test whether a
gap to AutoStakkert's P20 comes from how many frames the selection keeps.

## API for run-stack-bench.sh

```bash
B=modules/planetary/test/bench
python3 -I $B/benchset.py list                  # id, kind, expect, bayer (or auto), video (TSV)
python3 -I $B/benchset.py show <id>             # JSON: video, size, bayer + bayer_source, expect, references
stack-probe "<video>" --bayer <bayer> --out ours.fits --json    # a diagnosis case: --bayer auto
python3 -I $B/benchset.py compare <id> ours.fits --json
```

`compare --json` (with `--probe probe.json --keff keff.json` for END) prints one object per image,
ours first:
* END: `end, end_ratio, end_interior, end_interior_ratio, end_alt(_ratio), end_interior_alt(_ratio),
  end_frames, end_alt_frames, end_noise_budget, end_snr [per bin], end_note, end_complete`;
* DAN: `dan, ratio_to_best_reference, dan_interior, interior_ratio_to_best_reference, dan_bar`;
* `band_snr, noise_floor, box, mosaic, interior_judged, interior_diameter, bar {full, interior},
  review [...], orientation, shift, scale, ncc`.

The pass bar is `bar` (END, both ratios >= 0.97; the interior only when judged). `dan_bar` is the same
test on DAN, for the record. A non-empty `review` (limb-only, noise regime, mosaic fallback) sends the
capture to Scott's side-by-side however the numbers fall. `compare` refuses diagnosis captures;
for those the script checks stack-probe's diagnosis kind against `expect` from `show`.

The Python functions behind it, for the side-by-side and summary code:

* `benchset.compare_capture(id, ours, probe=None, frame_noise=None, keff=None) -> list[refs.Row]` --
  `ours` is a path or an array; with stack-probe's JSON as `probe` and stack-region-probe's as `keff`,
  END is measured. The capture's photon transfer comes from its first frames, our k from
  `benchset.ours_frames_box`, and the flip check from `ours_frames_mean`. Rows carry
  `label, path, factor, registration (orientation, shift, scale, ncc), result (metric.Result:
  value, interior, interior_judged, the common box, ...)`. `refs.rows_to_dicts(rows)` gives the JSON
  form; `refs.review_notes(rows)` the review list.
* `refs.compare(anchor, [path | (path, factor)], mosaic=False, anchor_label=None, frame_size=None,
  frame_noise=None, anchor_frames=None, capture_frames=None, anchor_frames_alt=None,
  anchor_frames_why=...)` -- the general form.
* `noise.measure_video(path, (w, h)) -> (PhotonTransfer, raw mean)`, `noise.raw_under(raw_mean,
  stack_lum, box)`, `noise.box_noise(ptc, raw, window=None)`; `halves.calibrate(work, stack_probe,
  region_probe)`, `halves.check(run_dir, frame_noise)`.
* `refs.load_reference(path, factor=None, frame_size=None) -> Reference(image at native scale, factor, info)`,
  `refs.crop_registered(image, registration, box)` -- a reference's pixels under ours' box at the
  registered integer offset: the same crop the metric used, ready for a side-by-side at one scale.
* `refs.load_image(path)`, `refs.register(moving, anchor)`, `metric.target_box(img)`,
  `metric.interior_window(img, box, limb=None)`, `metric.measure(img, box, interior=window)`,
  `bayer.measure_video(path, (w, h))`.

## Limits

* Sharpened images: see "Known limit: sharpening" above.
* END's noise model:
  * C1: noise and sharpening beyond the photon budget raise END, and no single-image estimator can
    see them. Ours is charged what its split halves measure, and the references stay on the budget,
    so a pass is sound and a fail only against an unprocessed reference.
  * The interleaved split cannot see what adjacent frames share; see "What the interleaved halves
    share".
  * Our k comes from stack-region-probe, which repeats RunStack's selection steps. END is unmeasured
    when its kept counts differ from stack-probe's.
  * The flip check reads our k as the frames used x the unweighted mean kept fraction over regions.
  * AutoStakkert's percentage is global, so it is approximate per alignment point.
  * sigma_frame comes from the first 128 frames.
* DAN's white floor (secondary): interpolated or resampled stacks have less noise near Nyquist than
  mid-band, so some of their mid-band noise counts as detail (measured above: 1.1-38 x).
* Small discs: below a 20 px eroded interior (Mars; the 2_12_2023 Jupiter at 18.8 px; Saturn in the
  bench, where ours' tighter mask makes 23.5 px 17.37) only the limb is measured, and an interior softened by sigma 0.7 behind a crisp limb passes DAN_full (numbers
  above). Those captures are marked limb-only, and Scott's review decides.
* Area-resampling a x3 drizzle to native is itself a 1-native-pixel box filter (Ruling 1), so
  references drizzled x3 carry that cost.
