# PlanetaryStack benchmark harness: metric, references, benchmark set

Host-only test tooling for the PlanetaryStack engine (spec:
`docs/superpowers/specs/2026-10-21-planetary-stacker-design.md`, Yardstick section and Rulings 1-2).
It measures our stack against Scott's own AutoStakkert (AS) and ASIStudio (ASI) results of the same
captures. Nothing here ships; the NAS (`/mnt/qnap/astro_data`) is only ever read.

| File | What it does |
|---|---|
| `metric.py` | The one sharpness-at-equal-noise measure, DAN ("detail above noise"), and the target box |
| `refs.py` | Loads AS/ASI/our images, area-resamples drizzled AS output to native (Ruling 1), registers each reference to an anchor (orientation, sub-pixel shift, measured scale), measures all on one box |
| `benchset.py` | The benchmark set below (as data), `compare_capture` for run-stack-bench.sh, and a survey that checks every reference against its own video |
| `test_metric.py`, `test_refs.py`, `test_benchset.py`, `conftest.py` | pytest; real-data tests skip when the NAS is not mounted |

## Setup

Python 3.14 in a venv outside the repo (any location; the session used its scratchpad):

```bash
python3 -m venv <venv>
<venv>/bin/pip install numpy scipy astropy tifffile Pillow opencv-python-headless pytest
cd modules/planetary/test/bench
<venv>/bin/python -I -m pytest -q -p no:cacheprovider          # 74 tests, 1-3 min with the NAS (cache-dependent)
<venv>/bin/python -I benchset.py survey ../output/bench-survey  # survey.json + thumbnails (gitignored)
```

Versions used: numpy 2.5.4, scipy 1.18.1, astropy 8.0.2, tifffile 2026.10.15, Pillow 13.0.0,
opencv-python-headless 5.0.0, pytest 9.1.1. ffmpeg/ffprobe are needed for the survey and
`test_benchset.py` (frame counts, first frames).

**Why OpenCV for PNG:** every AS PNG on the NAS is 16-bit RGB (IHDR bit depth 16, colour type 2).
Pillow reads those as 8-bit RGB, keeping only the high byte (verified on the 2_12_2023 AS_P20
Jupiter: Pillow's array equals OpenCV's `>> 8` exactly). `refs.load_image` reads PNG with OpenCV;
a test pins the 16-bit round trip.

## The metric: DAN, detail above noise

Full definition and reasoning are in the `metric.py` docstring. In short, for each image on the
same registered target box:

1. luminance (channel mean); 2. one crop for all images, chosen on the anchor (the target's
largest bright component plus a 15 % margin of sky; a lunar disc gets the frame minus a 2 % inset);
3. normalise: subtract the background b (1st percentile of the crop low-passed at sigma 4) and
divide by the mean target signal (mean - b), so gain and offset drop out and blur cannot move it;
4. Hann-windowed power spectrum in variance-per-pixel units; 5. noise floor = median power in the
0.40-0.50 cycles/px annulus / ln 2; 6. **DAN = sqrt(sum over 0.06-0.30 cycles/px of (P - floor) / n)**:
the RMS amplitude of the detail on 3-17 px scales, as a fraction of the mean target signal, with
the noise power removed.

Why: seeing and misalignment are low-pass filters and take power out of that band; a plain
gradient or Laplacian energy would reward noise (a stack of fewer frames would "win" by being
noisier), whereas DAN subtracts each image's own noise floor, so a 20 % stack and a 50 % stack are
compared on the detail they carry. It is translation invariant, so references are cropped at their
registered integer offset and never interpolated. Flat (unweighted) band power was chosen over
frequency-weighted variants after measuring both: f^2 weighting is more blur-sensitive (-25-26 % vs
-16 % at sigma 1) but its seed-to-seed scatter under heavy noise is up to +-7 % against +-1-3 %
for flat, and the pass bar is 3 %.

Undeclared Bayer (no sidecar: our stack is raw CFA as mono): every image of that capture is
measured with `mosaic=True`, which scales the crop's four CFA sites to a common mean. Measured on
raw NAS frames the checkerboard moves DAN by only 0.2-1.7 % (its power sits at Nyquist, outside the
band) but inflates the noise floor (synthetic: 8.9e-5 vs 5.0e-5); on a debayered reference the
flattening changes DAN by < 0.2 %.

### Validation (measured 2026-10-22)

Blur (Gaussian sigma added, px) -- strictly decreasing everywhere:

| Image | sigma 0 | 0.5 | 1.0 | 2.0 |
|---|---|---|---|---|
| synthetic textured disc | 0.09804 | 0.09420 | 0.08237 | 0.05472 |
| synthetic lunar field | 0.02286 | 0.02192 | 0.01901 | 0.01252 |
| 2_12_2023 Jupiter AS_P20 | 0.32571 | 0.31766 | 0.29300 | 0.23749 |
| 2_12_2023 Moon AS_P20 | 0.06876 | 0.06620 | 0.05817 | 0.03894 |

Added white noise (sigma as a fraction of the mean target signal) -- never raises DAN; it stays
within 1.1 % while the band SNR falls by up to two orders of magnitude:

| Image | none | 0.005 | 0.01 | 0.02 |
|---|---|---|---|---|
| synthetic disc (abs. sigma) | 0.09804 | 0.09788 | 0.09760 | 0.09697 (band SNR 449 -> 4.6) |
| synthetic lunar (abs. sigma) | 0.02286 | 0.02277 | 0.02270 | 0.02270 (band SNR 144 -> 1.5) |
| 2_12_2023 Jupiter AS_P20 | 0.32571 | 0.32541 | 0.32521 | 0.32457 |
| 2_12_2023 Moon AS_P20 | 0.06876 | 0.06872 | 0.06869 | 0.06862 (band SNR 1035 -> 42) |

A blurred (sigma 1) copy plus noise 0.02 still measures < 0.9 x the original (noise cannot buy back blur).

Repeatability on planet-sized crops (synthetic disc, 20 noise seeds, same box): boxes of 33-106 px
give a seed-to-seed sd of 0.07-0.14 % at noise 0.002, 0.18-0.38 % at 0.005 and 0.35-0.75 % at
0.01; the full range never exceeds 2.8 %. Measurement noise alone stays inside Ruling 2's 3 %.

AutoStakkert's own selection (fewer, sharper frames -> more detail), same capture, one box:

| Capture | Fewer frames | More frames | Ratio | Asserted |
|---|---|---|---|---|
| 2_12_2023 Moon 024708 | AS_P20 0.06876 | AS_P25 (x3, resampled) 0.05585; AS_P50 0.05266 | 1.23; 1.31 | yes |
| 2_12_2023 Jupiter 191831 | AS_P20 0.32571 | AS_P25 (x3, resampled) 0.31714 | 1.027 | yes |
| 3_6_2023 Moon 224808 | AS_P20 (x3) 0.02665 | AS_P50 (x1.5) 0.02637 | 1.011 | recorded only (within noise) |
| 2026-01-30 Jupiter 195830 | ASI_40 0.18930 | ASI_50 0.18868 | 1.003 | recorded only (within noise) |

The noise floors move the right way too: on the 2_12_2023 Moon, AS_P50's floor is half of AS_P20's
(8.7e-6 vs 1.7e-5) yet its DAN is lower -- the metric is not paying for noise.

## References: facts measured on the NAS

* **Drizzle names lie for AS `.fit` output.** `2_1_2024/AS_P60/..._Drizzle15.fit` (5688x3168) and
  `3_6_2023/moon/AS_P20/..._Drizzle15.fit` (5664x3120) are x3, not x1.5: after a x1.5 resample the
  scale check measures 2.004 (against ASIStudio's `.fit` of the same capture) and 2.000 (against
  the video). Their factor is recorded as 3.0 in `benchset.py` and their label says
  "named x1.5, measured x3". AS PNGs match their names.
* **Orientation differs between tools.** AS `.fit` files are flipped vertically against the video
  and against ASIStudio's `.fit` (2_1_2024, 8_23_2023, 3_6_2023); AS PNGs and ASI `.fit` match the
  video. Registration chooses among identity/flipud/fliplr/rot180 and records the choice. The one
  exception, Venus (AS PNGs read rot180), is a featureless clipped 17 px disc whose orientation the
  data cannot settle; it is a diagnosis case and never compared.
* AS `_conv` files are AutoStakkert's sharpened output and ASI `_Sharp...` `.jpg` files are
  sharpened and stretched; neither is a stack, and `refs.compare` refuses them.
* ASIStudio produced an image (`..._Planet_40.fit`) from the empty 2026-01-30 Saturn capture; it
  registers to nothing (best coarse NCC 0.296).

Registration of every reference against the mean of its video's first 32 frames (survey,
2026-10-22): lunar scale residuals 0.04-0.31 %, NCC 0.76-0.998; planets 0.2-4.7 % (against a
blurred raw mean the small-disc scale reads high; the tolerance is max(1 %, 1 px / target size),
which still separates x1 / x1.5 / x3 -- the only factors the tools produce).

## The benchmark set

Frame counts and sizes from ffprobe; sidecar = `<video>.txt` beside the AVI (checked by
`test_benchset.py`). Paths are under `/mnt/qnap/astro_data/`. Target size is the bright
component of the first reference, in native px. DAN values are reference against reference on that
capture's common box (our stack joins in Task 9's run).

### Planets

| ID | Video | Frames, size, file | Bayer | Target | References (label, factor, orientation vs video) | DAN | Why |
|---|---|---|---|---|---|---|---|
| 2023-02-11-jupiter | `2_12_2023/2023-02-11-191831-Jupiter-Bin1 13.3F.avi` | 6516, 640x480, 2.0 GB | no sidecar (mosaic) | 29x27 | `2_12_2023/AS_P20/2023-02-11-191831-Jupiter-Bin1 13.3F_lapl5_ap1.png` (AS_P20, x1, identity); `2_12_2023/AS_P25/..._lapl5_ap1_Drizzle30.png` (AS_P25, x3, identity) | 0.3257; 0.3171 | Metric-validation capture: AS native and drizzled |
| 2026-01-30-jupiter | `processing-archive-2026-10-03/videos/2026-01-30-195830-Jupiter-Bin1 Lqef -4.2F.avi` | 4088, 480x360, 0.7 GB | sidecar (SERViewer Save pattern: RG) | 37x41 | `.../ASIVideoStack_Output/2026-01-30-195830-Jupiter-Bin1 Lqef -4.2F_Planet_40.fit` (ASI_40, identity); `..._Planet_50.fit` (ASI_50, identity) | 0.1893; 0.1887 | Bayer known: colour against ASIStudio; current camera |
| 2023-09-29-saturn | `9_30_2023/2023-09-29-204604-Saturn-Bin1 LPro 14.9F.avi` | 3973, 1280x960, 4.9 GB | no sidecar (mosaic) | 43x24 | `9_30_2023/ASIVideoStack_Output/2023-09-29-204604-Saturn-Bin1 LPro 14.9F_Planet_74.fit` (ASI_74, identity) | 0.5828 | Rings; small target in a large frame |
| 2023-02-11-mars | `2_12_2023/2023-02-11-192403-Mars-Bin1 16.5F.avi` | 13 251, 320x240, 1.0 GB | no sidecar (mosaic) | 13x13 | `2_12_2023/AS_P25/2023-02-11-192403-Mars-Bin1 16.5F_lapl5_ap1_Drizzle30.png` (AS_P25, x3, identity; carries a bright edge strip, cols 305-317) | 5.870 | Smallest disc; longest capture |

### Lunar

All four are lunar discs or limbs with sky in the frame, not frame-filling surface.

| ID | Video | Frames, size, file | Bayer | Target | References (label, factor, orientation vs video) | DAN | Why |
|---|---|---|---|---|---|---|---|
| 2023-02-12-moon | `2_12_2023/2023-02-12-024708-Moon-Bin1 14.9F.avi` | 2354, 1920x1080, 4.9 GB | no sidecar (mosaic) | 895x1314 | `2_12_2023/AS_P20/2023-02-12-024708-Moon-Bin1 14.9F_lapl5_ap1.png` (AS_P20, x1, identity); `2_12_2023/AS_P50/..._lapl5_ap2.png` (AS_P50, x1, identity); `2_12_2023/AS_P25/..._lapl5_ap2_Drizzle30.png` (AS_P25, x3, identity) | 0.0688; 0.0527; 0.0559 | Metric validation: P20 and P50 both native |
| 2024-02-01-moon | `2_1_2024/2024-02-01-013847-Moon-Bin1 Lqef 14.9F.avi` | 1310, 1920x1080, 2.7 GB | no sidecar (mosaic) | 696x893 | `2_1_2024/ASIVideoStack_Output/2024-02-01-013847-Moon-Bin1 Lqef 14.9F_Surface_40.fit` (ASI_40, identity); `2_1_2024/AS_P60/..._lapl5_ap16_Drizzle15.fit` (AS_P60, named x1.5, measured x3, flipud) | 0.1320; 0.1415 | Both tools on one capture |
| 2023-08-22-moon | `8_23_2023/2023-08-22-213239-FOV-Bin1 LPro 15.8F.avi` | 1352, 1920x1080, 2.8 GB | no sidecar (mosaic) | 800x1716 | `8_23_2023/ASIVideoStack_Output/2023-08-22-213239-FOV-Bin1 LPro 15.8F_Surface_35.fit` (ASI_35, identity); `8_23_2023/AS_P20/..._lapl5_ap4_Drizzle30.fit` (AS_P20, x3, flipud) | 0.0300; 0.0352 (AS band SNR 149) | Both tools; named "FOV" but a lunar limb (checked visually) |
| 2023-03-05-moon | `3_6_2023/moon/2023-03-05-224808-Moon-Bin1 18.3F.avi` | 1705, 1920x1080, 3.5 GB | sidecar (ASI2400MC, RG) | 1040x1285 | `3_6_2023/moon/AS_P20/2023-03-05-224808-Moon-Bin1 18.3F_lapl5_ap4_Drizzle15.fit` (AS_P20, named x1.5, measured x3, flipud); `3_6_2023/moon/AS_P50/..._jupiter_lapl5_ap4_Drizzle15.png` (AS_P50, x1.5, identity) | 0.0267; 0.0264 | Bayer known; a second AS P20/P50 pair |

### Diagnosis cases (no image expected)

| ID | Video | Frames | Expected | References | Notes |
|---|---|---|---|---|---|
| 2026-01-30-saturn-empty | `processing-archive-2026-10-03/videos/2026-01-30-203627-Saturn-Bin1 Lqef -3.8F.avi` | 21, 1920x1080, 44 MB | NoTarget | `.../ASIVideoStack_Output/2026-01-30-203627-Saturn-Bin1 Lqef -3.8F_Planet_40.fit` | Frames hold only background (measured 2026-10-21); ASIStudio still wrote an image |
| 2023-02-11-venus-clipped | `2_12_2023/2023-02-11-190452-Venus-Bin1 15.8F.avi` | 6485, 640x480, 2.0 GB | Clipped | `2_12_2023/AS_P25/..._Drizzle30.png`, `2_12_2023/AS_P50/..._Drizzle30.png` | Every frame >= 25 % saturated (Task 3); a 17 px clipped blob. Its DAN (P25 3.33 < P50 3.42) is the edge of a saturated disc, not detail, and is not used |

Bayer sidecars: the 2023 Moon AVIs and the 2026-01-30 captures have none, except
`2026-01-30-195830-Jupiter-Bin1 Lqef -4.2F.avi.txt`, which SERViewer's Save pattern wrote
(Bayer = RG); `3_6_2023/moon/2023-03-05-224808-Moon-Bin1 18.3F.avi.txt` is the ASIAIR's own.
Captures without a sidecar are stacked as raw CFA read as mono, so `mosaic` is set for them.

### Considered, not in the set

* `2_12_2023` Neptune (AS_P25 drizzled only): a fourth tiny disc adds little over Mars.
* `9_16_2023` Saturn and Jupiter, `9_22/Video` Jupiter (ASI only; 9_22's ASI output is mono):
  Saturn is covered by 9_30_2023, which has the larger frame.
* `4_7_2023` Moons (ASI only, 7.4-7.6 GB each), `5_27_2023` Moon (AS_P20 only, 7.5 GB),
  `2_20_2024` Moon (AS_P50 only), `2_2_23/Moon` (AS_P50 only): the chosen four lunar captures
  already cover both tools, both drizzle factors and a sidecar.
* `8_16/Video` 2022 Jupiters: 88-91 % of the disc saturated (measured by `avi_real_check.py`).
* The other 2026-01-30 Jupiters (ASI only, no sidecar): 195830 is the one with a known pattern.
* `3_31_2023/Moon`: no usable reference (spec Ruling 5).

## API for run-stack-bench.sh

The shell script calls one entry point per capture:

```bash
B=modules/planetary/test/bench
python3 -I $B/benchset.py list                       # id, kind, expect, mosaic, video (TSV)
python3 -I $B/benchset.py show <id>                  # the capture as JSON (video, refs, factors)
python3 -I $B/benchset.py compare <id> <ours.fits|ours.tif> --json
```

`compare --json` prints one object per image, ours first: `label, path, factor, dan, band_snr,
noise_floor, box, mosaic, ratio_to_best_reference, orientation, shift, scale, ncc`. The pass bar
(Ruling 2) is ours' `ratio_to_best_reference >= 0.97`. Diagnosis captures are refused by `compare`;
the script checks stack-probe's diagnosis kind against `expect` from `show`.

The Python functions behind it, for the side-by-side and summary code:

* `benchset.compare_capture(id, ours) -> list[refs.Row]` -- `ours` is a path or an array; rows carry
  `label, path, factor, registration (orientation, shift, scale, ncc), result (metric.Result, with
  the common box)`.
* `refs.compare(anchor, [path | (path, factor)], mosaic=False, anchor_label=None)` -- the general form.
* `refs.load_reference(path, factor=None) -> Reference(image at native scale, factor, info)`,
  `refs.crop_registered(image, registration, box)` -- a reference's pixels under ours' box at the
  registered integer offset: the same crop the metric used, ready for a side-by-side at one scale.
* `refs.load_image(path)`, `refs.register(moving, anchor)`, `metric.target_box(img)`,
  `metric.measure(img, box, mosaic=False)`, `refs.rows_to_dicts(rows)`.

## Limits

* The noise floor assumes roughly white noise. Interpolated or resampled stacks have less noise
  near Nyquist than mid-band, so some of their mid-band noise can count as detail; `band_snr` is
  reported so a low-SNR comparison is visible (lowest among the benchmark references: the
  8_23_2023 AS_P20, 149).
* On discs under ~50 px (all four planets here), DAN is dominated by the limb: it measures how sharp
  the disc edge and its few resolved features are, which is what separates stacks of those
  captures, but it says little about belts. Scott's side-by-side review is the check for that.
* Area-resampling a x3 drizzle to native is itself a 1-native-pixel box filter (Ruling 1);
  references drizzled x3 carry that cost.
