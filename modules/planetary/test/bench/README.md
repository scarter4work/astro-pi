# PlanetaryStack benchmark harness: metric, references, benchmark set

Host-only test tooling for the PlanetaryStack engine (spec:
`docs/superpowers/specs/2026-10-21-planetary-stacker-design.md`, Yardstick section and Rulings 1-2).
It measures our stack against Scott's own AutoStakkert (AS) and ASIStudio (ASI) results of the same
captures. Nothing here ships; the NAS (`/mnt/qnap/astro_data`) is only ever read.

| File | What it does |
|---|---|
| `metric.py` | END, the judged sharpness-at-equal-noise measure (Ruling P28), and END_interior; DAN and DAN_interior (secondary); the target box, interior window and sky excess-noise check |
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
<venv>/bin/python -I -m pytest -q -p no:cacheprovider               # 175 tests, 2-4 min with the NAS
<venv>/bin/python -I -m pytest -q -p no:cacheprovider -m "not nas"  # 116 synthetic tests, ~1.5 min
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
* The 2023-03-05 Moon's ASIAIR sidecar says Bayer = RG with Flip=Vert, which the reader resolves to
  BGGR (Ruling 10). The bench's explicit RGGB wins over it.
* The 2026-01-30 Jupiter and the 2024-02-01 Moon carry SERViewer Save-pattern sidecars (Bayer = RG).

## The judged metric: END (Ruling P28)

The spec asks for "one documented sharpness-at-equal-noise metric". DAN (below) measured detail with
each image's noise removed, so it gave no credit for SNR: in Task 9 a stack with 6.7-9.7 x the
reference's per-band SNR "failed" on DAN. Ruling P28 made the judged metric literally sharpness at
equal noise. **END** ("equal-noise detail") asks: once each image is sharpened as far as its noise
allows, how much of the target's detail comes back? Full definition and reasoning: the `metric.py`
docstring. In short:

1. Same box, normalisation and Hann periodogram as DAN.
2. **One noise model for every image**: white noise of power sigma_frame^2 / k.
   * sigma_frame^2 is the capture's per-frame noise on the box, from a photon-transfer fit to its raw
     frames (`noise.py`; independent of any stacker).
   * k is the image's effective frame count. For ours, the frames used x the mean kept fraction over
     regions, from stack-probe's `regionDetail`. For a reference, its stacking percentage x the
     capture's frames; AutoStakkert's percentage is global, so this is approximate per alignment point.
   * Any smoothing (interpolation, drizzle, resampling, denoising) therefore counts as blur, on our
     side and the references' alike, as in DAN and Ruling 1.
3. **Plus any excess**: the white power of the image's own signal-free sky beyond the photon budget at
   the sky's level. A stack that is noisier than its frame count implies is charged for it. Detail on
   the target is never read as noise.
4. Per bin (0.06-0.30 cycles/px, steps of 0.03): SNR = max(P - N, 0) / N.
   END = sqrt( sum O(f) / sum O(f) / (1 + SNR) ) with the object prior O(f) = f^-3. That is the band's
   detail over the error of a matched Wiener restoration, as an amplitude ratio.
5. **END_interior**: the same on the interior window's high-pass, judged where DAN_interior was
   (the eroded disc is at least 20 px).
6. Ruling 2's 0.97 bar applies to END_full and, where judged, END_interior.

### Why not read each image's own noise (measured 2026-10-22)

These figures falsified every single-image noise estimate, and they are why END uses the physics
instead.

* **The box's own 0.40-0.50 cycles/px power is not noise.** Our engine stacked the even and odd
  frames separately, on the 2026-01-30 Jupiter and the 2023-02-12 Moon. The box's floor was 55 x and
  2.7 x the halves' difference: it is structure common to disjoint frames.
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
| AS_P20 | 471 | 13.27 | 0.06876 |
| AS_P50 | 1177 | 16.89 | 0.05266 |

The assertion moved to an unconfounded synthetic (`test_end.py`): one 200-frame capture, its sharpest
20 % and sharpest 50 % stacked. END must rank them as the exact matched restoration does, and the set
holds a case each way:
* where the seeing drifts smoothly (log-normal), P50 wins;
* where a quarter of the frames are steady moments, P20 wins.

DAN ranks P20 first in all four cases, wrongly in the log-normal ones.

### Validation (test_end.py, measured 2026-10-22)

| Property | Measured |
|---|---|
| Blur at equal noise ranks lower (seeing 1.0 / 1.5 / 2.0 / 3.0) | END 5.141 / 2.946 / 2.296 / 1.720 |
| Added noise never raises it (0.5 / 1 / 2 / 4 x the stack's noise) | 5.141 -> 4.877 / 4.114 / 3.161 / 2.390 (the photon budget alone would have risen to 5.30-9.75) |
| More frames, same blur (2 x / 4 x) | x1.191 / x1.481 (DAN x1.003) |
| Sharper but noisier, against the exact matched restoration | 4 of 4 cases rank as the restoration does, including A seeing 2.0 noise 1 vs B 1.2 / 3 (B), and 1.1 / 1 vs 1.0 / 4 (A) |
| Random scan: 157 clear pairs, object spectra f^-1.4..f^-4.4 | 157 / 157 agree with the restoration (log-capacity: 96 %; END's restoration form was chosen on this) |
| 20 % vs 50 % of one capture (4 cases, both outcomes) | 4 of 4 |
| Smoothing counts as blur: drizzle x3 + area resample / Gaussian 0.7 / B-spline half-pixel shift | x0.809 / x0.746 / x0.978. Reading the image's own floor as its noise would have raised it x2.06 / x3.08 / x1.62. That is the reference-side bias the photon budget removes, quantified. |
| Real AS_P20 (2_12_2023 Jupiter / Moon), blur 0 / 0.5 / 1 / 2 after the fact | 19.91 / 15.07 / 5.47 / 3.11 and 13.27 / 10.14 / 4.37 / 2.39 |
| Real AS_P20, added noise 0.005 / 0.01 / 0.02 of the signal | Jupiter 19.91 -> 14.16 / 9.59 / 6.48; Moon 13.27 -> 7.37 / 4.86 / 3.39 |
| Interior: an interior blur of sigma 1 behind a crisp limb / 2 x noise | 19.51 -> 15.62 / 10.70 |
| `noise.photon_transfer` on synthetic CFA frames (known line) | slope within 1 %; v(m) 4-11 % high on a textured disc drifting 0.5 px per frame |

**Split-half check** (Ruling P28, rule 3; validation only). The runner also stacks each capture's even
and odd frames separately. The summary compares the power of their difference with END's model,
sigma_frame^2 (1/k_A + 1/k_B) / 4:
* under 1 toward Nyquist is our interpolation's smoothing;
* far above 1, highest in the low bins, is signal the halves do not share. Each half's own frame
  choice and local alignment move fine structure by hundredths of a pixel.

The 2026-10-23 run, measured over model at 0.21-0.30 cycles/px with the judged k:
* **0.71-1.12 x:** the 2023-02-11 Jupiter and the 2023-02-12, 2023-08-22 and 2023-03-05 Moons. With
  the harmonic k the same captures read 0.33-0.88 x, so the halves favour the judged reading.
* **0.31 x:** Saturn, where our smoothing puts the noise under the model.
* **2.9-73 x:** the 2026-01-30 Jupiter, Mars and the 2024-02-01 Moon. On these, D's disc-to-sky power
  is 22-1 000 x the photon budget's, so the excess is signal the halves do not share, not noise.

Per-capture ratios are in summary.md.

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
20 px rule that capture is limb-only. 2026-01-30 Jupiter (28.5 px) and Saturn (23.5 px) are
judged on both scores.

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
| 2_12_2023 Moon 024708 | AS_P20 0.06876 | AS_P50 0.05266 (both native) | 1.31 | reported (asserted until Ruling P28: the pair is confounded, ap1 vs ap2; END ranks P50 first, 16.89 vs 13.27) |
| 2_12_2023 Moon 024708 | AS_P20 0.06876 | AS_P25 (x3, resampled) 0.05585 | 1.23 | recorded, confounded by the resample |
| 2_12_2023 Jupiter 191831 | AS_P20 0.32571 | AS_P25 (x3, resampled) 0.31714 | 1.027 | recorded, confounded by the resample |
| 3_6_2023 Moon 224808 | AS_P20 (x3) 0.02665 | AS_P50 (x1.5) 0.02637 | 1.011 | recorded (within noise) |
| 2026-01-30 Jupiter 195830 | ASI_40 0.18930 | ASI_50 0.18868 | 1.003 | recorded (within noise) |

The noise floors move the right way too: on the 2_12_2023 Moon, AS_P50's floor is half of AS_P20's
(8.7e-6 vs 1.7e-5), yet its DAN is lower. The metric is not paying for noise.

### Known limit: sharpening

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
is the interior window's equivalent diameter (>= 20 px: interior judged), from the references'
sharpest limb. Once our stack joins, the sharpest of ours and the references sets the erosion.
A sharper stack of ours can only enlarge the window; a softer one cannot shrink it. The window's
outline is ours' target mask, so the reviewer's own stack of the 2026-01-30 Jupiter measured 28.0 px
on its own limb, against 28.5 here. DAN values compare
reference against reference on the capture's common box; our stack joins in Task 9's run.

### Planets

| ID | Video | Frames, size, file | Bayer (source) | Eroded | References | DAN (interior) | Why |
|---|---|---|---|---|---|---|---|
| 2023-02-11-jupiter | `2_12_2023/2023-02-11-191831-Jupiter-Bin1 13.3F.avi` | 6516, 640x480, 2.0 GB | RGGB (camera spec; AS used BGGR) | 18.8 px: limb-only | `2_12_2023/AS_P20/2023-02-11-191831-Jupiter-Bin1 13.3F_lapl5_ap1.png` (x1); `2_12_2023/AS_P25/..._lapl5_ap1_Drizzle30.png` (x3) | 0.3257; 0.3171 | AS native and drizzled |
| 2026-01-30-jupiter | `processing-archive-2026-10-03/videos/2026-01-30-195830-Jupiter-Bin1 Lqef -4.2F.avi` | 4088, 480x360, 0.7 GB | RGGB (camera spec = Scott's Save pattern; ASI same) | 28.5 px | `.../ASIVideoStack_Output/2026-01-30-195830-Jupiter-Bin1 Lqef -4.2F_Planet_40.fit`; `..._Planet_50.fit` | 0.1893 (0.2291); 0.1887 (0.2283) | Larger Jupiter: interior judged |
| 2023-09-29-saturn | `9_30_2023/2023-09-29-204604-Saturn-Bin1 LPro 14.9F.avi` | 3973, 1280x960, 4.9 GB | RGGB (camera spec; ASI same) | 23.5 px | `9_30_2023/ASIVideoStack_Output/2023-09-29-204604-Saturn-Bin1 LPro 14.9F_Planet_74.fit` | 0.5828 (0.6038) | Rings; small target in a large frame |
| 2023-02-11-mars | `2_12_2023/2023-02-11-192403-Mars-Bin1 16.5F.avi` | 13 251, 320x240, 1.0 GB | RGGB (camera spec; AS used BGGR) | 7.4 px: limb-only | `2_12_2023/AS_P25/2023-02-11-192403-Mars-Bin1 16.5F_lapl5_ap1_Drizzle30.png` (x3; bright edge strip, cols 305-317) | 5.870 | Smallest disc; longest capture. **Limb-only: human review decides** |

### Lunar

All four are lunar discs or limbs with sky in the frame, not frame-filling surface.

| ID | Video | Frames, size, file | Bayer (source) | References | DAN (interior) | Why |
|---|---|---|---|---|---|---|
| 2023-02-12-moon | `2_12_2023/2023-02-12-024708-Moon-Bin1 14.9F.avi` | 2354, 1920x1080, 4.9 GB | RGGB (camera spec; AS used BGGR) | `2_12_2023/AS_P20/2023-02-12-024708-Moon-Bin1 14.9F_lapl5_ap1.png` (x1); `2_12_2023/AS_P50/..._lapl5_ap2.png` (x1); `2_12_2023/AS_P25/..._lapl5_ap2_Drizzle30.png` (x3) | 0.0688 (0.0732); 0.0527 (0.0587); 0.0559 (0.0603) | Metric validation: P20 and P50 both native |
| 2024-02-01-moon | `2_1_2024/2024-02-01-013847-Moon-Bin1 Lqef 14.9F.avi` | 1310, 1920x1080, 2.7 GB | RGGB (camera spec = its Save-pattern sidecar; AS and ASI same) | `2_1_2024/ASIVideoStack_Output/2024-02-01-013847-Moon-Bin1 Lqef 14.9F_Surface_40.fit`; `2_1_2024/AS_P60/..._lapl5_ap16_Drizzle15.fit` (named x1.5, size x3) | 0.1320 (0.1465); 0.1415 (0.1556) | Both tools on one capture |
| 2023-08-22-moon | `8_23_2023/2023-08-22-213239-FOV-Bin1 LPro 15.8F.avi` | 1352, 1920x1080, 2.8 GB | RGGB (camera spec; AS and ASI same) | `8_23_2023/ASIVideoStack_Output/2023-08-22-213239-FOV-Bin1 LPro 15.8F_Surface_35.fit`; `8_23_2023/AS_P20/..._lapl5_ap4_Drizzle30.fit` (x3) | 0.0300 (0.0248); 0.0352 (0.0288; band SNR 149) | Both tools; named "FOV" but a lunar limb (checked visually) |
| 2023-03-05-moon | `3_6_2023/moon/2023-03-05-224808-Moon-Bin1 18.3F.avi` | 1705, 1920x1080, 3.5 GB | RGGB (camera spec; explicit, over the sidecar's RG + Flip=Vert; AS same) | `3_6_2023/moon/AS_P20/2023-03-05-224808-Moon-Bin1 18.3F_lapl5_ap4_Drizzle15.fit` (named x1.5, size x3); `3_6_2023/moon/AS_P50/..._jupiter_lapl5_ap4_Drizzle15.png` (x1.5) | 0.0267 (0.0271); 0.0264 (0.0269) | Sidecar; a second AS P20/P50 pair |

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
    count, no usable sky). No verdict is drawn and the reason is printed.
  * **review**: the END verdict flips between our frame count read two ways (rule 5). The two readings
    are the mean kept fraction, which is judged, and the harmonic mean of the regions' kept counts.
    A capture also goes to review when the numbers pass but a review note applies (limb-only, noise
    regime, mosaic fallback).
  * **fail**: END_full, or END_interior where judged, is below 0.97 x the best reference under both
    readings.
  * **pass**: none of the above.

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

`compare --json` (with `--probe probe.json` for END) prints one object per image, ours first:
* END: `end, end_ratio, end_interior, end_interior_ratio, end_pure(_ratio), end_alt(_ratio),
  end_interior_alt(_ratio), end_frames, end_alt_frames, end_noise, end_noise_budget, end_excess,
  end_sky_tiles, end_snr [per bin], end_note, end_complete`;
* DAN: `dan, ratio_to_best_reference, dan_interior, interior_ratio_to_best_reference, dan_bar`;
* `band_snr, noise_floor, box, mosaic, interior_judged, interior_diameter, bar {full, interior},
  review [...], orientation, shift, scale, ncc`.

The pass bar is `bar` (END, both ratios >= 0.97; the interior only when judged). `dan_bar` is the same
test on DAN, for the record. A non-empty `review` (limb-only, noise regime, mosaic fallback) sends the
capture to Scott's side-by-side however the numbers fall. `compare` refuses diagnosis captures;
for those the script checks stack-probe's diagnosis kind against `expect` from `show`.

The Python functions behind it, for the side-by-side and summary code:

* `benchset.compare_capture(id, ours, probe=None, frame_noise=None) -> list[refs.Row]` -- `ours` is a
  path or an array; with stack-probe's JSON as `probe`, END is measured (the capture's photon transfer
  from its first frames, `benchset.ours_frames` / `ours_frames_alt` for our k); rows carry
  `label, path, factor, registration (orientation, shift, scale, ncc), result (metric.Result:
  value, interior, interior_judged, the common box, ...)`. `refs.rows_to_dicts(rows)` gives the JSON
  form; `refs.review_notes(rows)` the review list.
* `refs.compare(anchor, [path | (path, factor)], mosaic=False, anchor_label=None, frame_size=None,
  frame_noise=None, anchor_frames=None, capture_frames=None, anchor_frames_alt=None)` -- the general form.
* `noise.measure_video(path, (w, h)) -> (PhotonTransfer, raw mean)`, `noise.raw_under(raw_mean,
  stack_lum, box)`, `noise.box_noise(ptc, raw, window=None)`, `noise.sky_noise(ptc, raw)`.
* `refs.load_reference(path, factor=None, frame_size=None) -> Reference(image at native scale, factor, info)`,
  `refs.crop_registered(image, registration, box)` -- a reference's pixels under ours' box at the
  registered integer offset: the same crop the metric used, ready for a side-by-side at one scale.
* `refs.load_image(path)`, `refs.register(moving, anchor)`, `metric.target_box(img)`,
  `metric.interior_window(img, box, limb=None)`, `metric.measure(img, box, interior=window)`,
  `bayer.measure_video(path, (w, h))`.

## Limits

* Sharpened images: see "Known limit: sharpening" above.
* END's noise model:
  * Our k is the frames used x the mean kept fraction over regions. The exact (sum w)^2 / sum w^2 of
    the accumulator's weights needs per-frame region membership, which RunStack does not return. The
    harmonic reading is scored too, and a verdict that depends on the choice goes to review.
  * AutoStakkert's percentage is global, so it is approximate per alignment point.
  * sigma_frame comes from the first 128 frames.
  * Detail and fixed-pattern structure in the band count as signal for every image alike.
  * A tool that denoises non-linearly gains no budget (it is modelled from its frame count), but any
    real structure its processing adds in the band counts as detail.
* DAN's white floor (secondary): interpolated or resampled stacks have less noise near Nyquist than
  mid-band, so some of their mid-band noise counts as detail (measured above: 1.1-38 x).
* Small discs: below a 20 px eroded interior (Mars, and the 2_12_2023 Jupiter at 18.8 px) only the
  limb is measured, and an interior softened by sigma 0.7 behind a crisp limb passes DAN_full (numbers
  above). Those captures are marked limb-only, and Scott's review decides.
* Area-resampling a x3 drizzle to native is itself a 1-native-pixel box filter (Ruling 1), so
  references drizzled x3 carry that cost.
