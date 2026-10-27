# PlanetaryStack edge fix (0.3.0.2) + target detection and Mineral moon (0.4.0.0) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove the left-edge chroma noise (0.3.0.2). Then teach PlanetaryStack to recognise the target from the capture name and, for broadband colour Moon captures, produce a finished mineral-moon image beside the plain stack (0.4.0.0).

**Architecture:** All image work lives in the PCL-free stack engine (`modules/planetary/src/stack/`), with unit tests outside PixInsight.
- Name parsing and eligibility go in `src/ser/`, next to `FilenameStart`.
- The mineral pipeline is a new engine unit, `Mineral.{h,cpp}`. It runs on `StackOutcome::image` after `RunStack`.
- The module layer (`src/module/`) adds two parameters, the Target combo, the checkbox, and a second output window.

**Tech Stack:** C++17, PCL (module layer only), Catch2 (ctest), Python 3 + numpy (real-data checks), Rocky 9 build container.

**Spec:** `docs/superpowers/specs/2026-10-27-planetary-mineral-moon-design.md` (approved by Scott 2026-10-27). Read it with this plan; requirement ids B1–B8 below are its "mineral-moon steps" 1–8.

**Ledger:** continue `.superpowers/sdd/2026-10-21-planetary-stack/progress.md` for Part A (rulings R8+). Start `.superpowers/sdd/2026-10-27-mineral-moon/progress.md` for Part B.

## Global Constraints

- PCL `const char*` string literals are ASCII only (no em dash, no ellipsis, no µ).
- No OpenMP; threading through `src/stack/Parallel.h` (`ParallelBands`).
- Shipped builds come only from the Rocky 9 container (`tools/build-env/build-modules.sh`) and must pass `tools/build-env/verify-portable.sh`.
- Never `make install`. Never launch PixInsight except through `tools/pi-headless.sh`, or the existing harness scripts that use it.
- Module id `Planetary` and process ids `PlanetaryStack` / `SERViewer` never change.
- No calls to other PixInsight processes (`ProcessInstance`), third-party tools, external programs or network.
- Neither output may contain a sample above 1.0.
- A step that cannot run says why in HISTORY and the result line. It never invents data.
- Releases (`./release.sh`, version bump, package description, push of `repository/`) only on Scott's explicit go-ahead.
- NAS (`/mnt/qnap`) is read-only.

## Review Focus

1. **The Moon touches the frame edge on the left (64.6F) and top and bottom.** Mineral output must show no edge colour fringe. Test: B8's `edge chroma survives the pipeline`.
2. **A thin crescent or tiny disk**, where the lit disk is under 1 % of pixels. Calibration must skip with a HISTORY line, not blow up a colour cast. Test: B4's `tiny disk skips the white reference`.
3. **A capture with large saturated areas.** The white reference excludes them, and the pipeline adds no new clipping. Test: B4's `clipped pixels excluded`, plus B8's `no new clipping`.
4. **The user overrides Target to Moon on a planet file**, a small disc in a big black frame. The pipeline completes with valid output. Test: B8's `small disc completes`.
5. **Cancel during the mineral steps** leaves no window and no partial image. Tests: B8's `cancel throws StackCancelled`, and the B9 GUI smoke Cancel step.

---

# Part A — Planetary 0.3.0.2 (branch `fix/planetary-edge-chroma` from main)

### Task A1: Measure the edge chroma defect and find its cause

**Files:**
- Create: `modules/planetary/test/edge_chroma.py`, a numpy CLI over a `stack-probe --out x.fits` stack.
- Create: `modules/planetary/test/test_edge_chroma.py`, a synthetic check that the tool fails closed. Register it in `test/CMakeLists.txt` like `fpn_verdict_fails_closed` (skip code 77 without numpy).
- Modify: the ledger (Part A section).

**Interfaces:**
- Produces: `edge_chroma.py <stack.fits> [--rows 320:760] [--edge 0:6] [--ref 20:40] [--side left|right|top|bottom] [--json]`.
  - It prints, per ratio (B/G, R/G), the std in the edge band, the std in the reference band, and their ratio.
  - It exits 0 when both ratios are ≤ 1.5 and 1 otherwise.
  - Ratios are computed per pixel as channel/G, over pixels whose G ≥ 0.05 (disk only), and the std is taken over the band.

- [ ] **Step 1:** Write `test_edge_chroma.py`. It builds a synthetic 3-channel FITS with grey disk content. One variant puts chroma noise of std 0.03 in x 0–5 and 0.008 elsewhere: the tool exits 1 and reports a ratio > 3. A clean variant exits 0.
- [ ] **Step 2:** Run it before `edge_chroma.py` exists, and confirm it FAILs.
- [ ] **Step 3:** Implement `edge_chroma.py`, then run ctest `-R edge_chroma` and confirm it PASSes.
- [ ] **Step 4:** Reproduce on real data. Run `build/test/stack-probe "/mnt/qnap/astro_data/2026-10-23/Moon/2026-10-23-203401-Moon-Bin1 Lqef 64.6F.avi" --out <scratch>/a.fits`, then `edge_chroma.py <scratch>/a.fits`. The expected result is a B/G ratio of about 4 and an R/G ratio of about 2.8, the 2026-10-26 figures, with exit 1. Record the numbers.
- [ ] **Step 5: Find the cause.** At minimum, separate these three candidates, each with a number:
  - **(a) Demosaic border.** Run `DemosaicHQ` on single frames of the capture, or on synthetic noisy flat Bayer frames, and measure the edge/ref chroma ratio. Above about 1.5 on one frame means the excess exists before stacking.
  - **(b) Warp or resampling at the border.** Stack with frames shifted by a known sub-pixel amount versus zero shift, and compare.
  - **(c) Per-channel coverage.** Count samples per channel per column in the accumulator, or from `--sample` probes, at x 0–5 vs 20–40.

  Write ruling R8 in the ledger: the cause, the numbers, and which branch of A2 applies.
- [ ] **Step 6:** Commit the tool, its test and the ledger entry: `test(planetary-stack): edge chroma measurement and cause (R8)`.

### Task A2: RED test at the cause, then the fix

**Files:** these depend on R8.
- **Demosaic border:** `src/stack/Demosaic.cpp` (`DemosaicHQ` border handling); test in `test/unit/test_demosaic_hq.cpp`.
- **Warp/resampling:** `src/stack/Align.cpp` or `src/stack/Accumulate.cpp` (the border sample path); test in `test_align.cpp` or `test_accumulate.cpp`.
- **Coverage:** the edge coverage-quantile design (stacker review Q1, R4/R5); test in `test_accumulate.cpp` and `test_regions.cpp`.

**Interfaces:** no public signature changes, unless the coverage branch needs them. If it does, record them in the ledger before coding.

- [ ] **Step 1:** Write a RED unit test that reproduces R8's mechanism on synthetic data and asserts the spec bar: chroma std (B/G and R/G) in x 0–5 ≤ 1.5 × that in x 20–40. Name it after the mechanism, e.g. `"DemosaicHQ: chroma at the left border is no noisier than the interior"`.
  - **Coverage branch:** also add R5's bar. Edge-band (strip up to ~64 px) noise is ≤ 1.3 × the interior, by split-half.
- [ ] **Step 2:** Run it on the current code and confirm it FAILs, with the measured ratio in the message.
- [ ] **Step 3:** Fix it at the cause.
- [ ] **Step 4:** Run the new test (PASS). Then run the full `ctest --output-on-failure` three times; everything PASSes each time.
- [ ] **Step 5:** Mutation-check: revert the fix line(s) and confirm the test FAILs again. Then restore.
- [ ] **Step 6:** Commit: `fix(planetary-stack): <mechanism> (R8)`, with the RED test in a preceding `test(...): RED -- ...` commit.

### Task A3: Real-data acceptance, re-review, re-freeze, bench

**Files:**
- Modify: `modules/planetary/test/bench/calibration.json` (re-freeze), the ledger, and `modules/planetary/test/bench/README.md` if the calibration figures it quotes change.

- [ ] **Step 1:** Run `edge_chroma.py` on the fixed stack of 64.6F, with `--side left`, `--side top` and `--side bottom`. All must exit 0. Record the numbers.
- [ ] **Step 2: Scoped re-review.** Dispatch a fresh reviewer on fix round 5 (`7cba77e8..9e952d92` and `9be53d59..d22a1c6b`) plus this task's fix commits. Address its findings under the no-deferring rule, each with a ledger line.
- [ ] **Step 3: Re-freeze the calibration** (F2: `halves.calibrate`, as documented in `bench/README.md` "Calibration") on the 0.3.0.2 engine. Values that move must be explained in the ledger.
- [ ] **Step 4: Bench.** Run `bash test/run-stack-bench.sh`. Verdicts must be no worse than 4 pass / 4 review / 0 fail, and the 1080p gate ≤ 60 s.
- [ ] **Step 5:** Run the probes: `bash test/run-probe-stack.sh` and `bash test/gui-smoke-stack.sh <scratch>`. Both PASS.
- [ ] **Step 6:** Commit: `test(planetary-stack): re-freeze calibration on the 0.3.0.2 engine (F2)`.

### Task A4: Release 0.3.0.2 (on Scott's go-ahead only)

**Files:**
- Modify: `modules/planetary/src/module/PlanetaryVersion.h` (0.3.0.2, today's date).
- Modify: the Planetary package description, which names 0.3.0.2.
- Modify: `repository/`, through release.sh. This includes removing `20261024-linux-x64-Planetary-0.3.0.0.tar.gz` and its manifest entry if still listed.

- [ ] **Step 1:** Ask Scott, then stop until he answers.
- [ ] **Step 2:** Bump the version and description, then run `./release.sh`. It must pass: container build, `verify-portable.sh`, signing, manifest integrity.
- [ ] **Step 3:** Merge to main (ff), commit the version bump with the `repository/` artifacts together, and push.
- [ ] **Step 4:** Verify against the server: the served `updates.xri` equals the committed one, and the served tarball's sha1 matches. Record this in the ledger and in project memory (`planetary-module.md`).

---

# Part B — Planetary 0.4.0.0 (branch `feat/planetary-mineral-moon` from main after A4)

Shared scenes: `test/unit/MoonSynth.h`, created in B3, with `MoonScene MakeMoonScene(const MoonSceneSpec&)`.
- `MoonSceneSpec`: width, height, disk centre, disk radius, phase (terminator angle), maria fraction, true tints, colour cast, pedestal, gradient, blur sigma, noise std, clipped-region flag, edge-band chroma noise, seed.
- `MoonScene` returns:
  - `truth`: sharp, untinted-by-cast, linear `ser::FloatImage` RGB;
  - `observed`: cast, pedestal, gradient, blur and noise applied;
  - `diskTruth` / `skyTruth`: `std::vector<uint8_t>` masks;
  - `mariaTruth`: mask.
- True tints: maria (R,G,B) ∝ (0.97, 1.00, 1.04); highlands ∝ (1.04, 1.00, 0.95).

### Task B1: Capture-name parsing and eligibility

**Files:**
- Create: `modules/planetary/src/ser/CaptureName.h`, `CaptureName.cpp`; add the .cpp to `src/ser/CMakeLists.txt`.
- Test: `modules/planetary/test/unit/test_capture_name.cpp`, registered with `planetary_ser`.

**Interfaces (Produces):**
```cpp
namespace planetary::ser {
enum class Target { Other, Moon, Mercury, Venus, Mars, Jupiter, Saturn, Uranus, Neptune };
enum class FilterClass { Unknown, Broadband, Narrowband };
struct CaptureName {
    bool parsed = false;          // the ASIAIR pattern matched
    Target target = Target::Other;
    std::string targetToken;      // as written, e.g. "Moon", "Betelgeuse"
    std::string filterToken;      // e.g. "Lqef", "HaO3"; empty when absent
    FilterClass filter = FilterClass::Unknown;
};
CaptureName ParseCaptureName(const std::string& fileName);   // base name or full path
const char* TargetName(Target);                              // "Moon", "Jupiter", ..., "Other"
// nullopt when Mineral moon may run; else the ASCII reason shown in the tooltip and the failure
std::optional<std::string> MineralMoonRefusal(Target target, FilterClass filter, const FileInfo& info);
}
```

Rules:
- The pattern is `YYYY-MM-DD-HHMMSS-<Target>-Bin<n> <Filter> <temp>F` followed by the extension. Trailing PIPP-style suffixes such as `_pipp` before the extension are allowed.
- Target matching is case-insensitive. Unknown target tokens map to `Other`, with `parsed` still true.
- Narrowband tokens, case-insensitive: HaO3, S2O3, Ha, OIII, O3, SII, S2, Hb, D1, D2, Duo, NBZ, ALP-T, L-eNhance, L-eXtreme, L-Ultimate.
- Broadband tokens: Lqef, LPro, L-Pro, L, UVIR, UV/IR, IRCut, None, Clear.
- Any other token is `Unknown`, which is allowed.
- Refusal reasons:
  - target not Moon → `"Mineral moon is for the Moon; the target is <name>."`
  - mono capture (`info` resolves to 1 channel and no Bayer pattern) → `"Mineral moon needs a colour capture; this one is mono (set the Bayer pattern if it is a colour camera)."`
  - narrowband → `"Mineral moon needs a broadband filter; <token> is narrowband, so its colours come from the filter."`

**Spec correction (made here, recorded in the spec):** PIPP keeps the ASIAIR name, so `..._pipp.ser` parses to its real target. Only renamed files fall to Other.

- [ ] **Step 1:** Write `test_capture_name.cpp` with these cases. Each asserts target, filter token and class:
  - `2026-10-23-203401-Moon-Bin1 Lqef 64.6F.avi` → Moon, Lqef, Broadband
  - `2024-02-01-013058-Moon-Bin1 HaO3 16.2F.avi` → Moon, HaO3, Narrowband
  - `2023-08-30-220451-Moon-Bin1 LPro 14.0F.avi` → Moon, LPro, Broadband
  - `2026-01-30-193142-Moon-Bin1 Lqef -4.0F_pipp.ser` → Moon (negative temperature, PIPP suffix)
  - `2026-01-30-195137-Jupiter-Bin1 Lqef -4.2F.avi` → Jupiter
  - Saturn, Neptune and Mars names → their targets
  - `2026-02-10-215957-Betelgeuse-Bin1 Lqef -4.0F.avi` → Other, parsed, token "Betelgeuse"
  - `moon_stack_final.ser` and the empty string → parsed false, Other
  - a full path with directories → same as the base name

  `MineralMoonRefusal` gets four cases:
  - Moon + Broadband + Bayer FileInfo → nullopt
  - Moon + Narrowband → the narrowband text
  - Saturn → the target text
  - Moon + mono FileInfo → the mono text
- [ ] **Step 2:** Run `ctest -R test_capture_name` and confirm it FAILs to compile/link.
- [ ] **Step 3:** Implement `CaptureName.{h,cpp}`.
- [ ] **Step 4:** Run `ctest -R test_capture_name` and confirm it PASSes.
- [ ] **Step 5:** Edit the spec's two `_pipp.ser` lines to match this correction.
- [ ] **Step 6:** Commit: `feat(planetary): target and filter from the capture name; Mineral moon eligibility`.

### Task B2: Lunar disk and sky segmentation (B1 of the spec)

**Files:**
- Create: `modules/planetary/src/stack/Mineral.h`, `Mineral.cpp`; add to `src/stack/CMakeLists.txt`.
- Create: `test/unit/MoonSynth.h`.
- Test: `test/unit/test_mineral.cpp`, registered with `planetary_stack`.

**Interfaces (Produces):**
```cpp
namespace planetary::stack {
struct LunarMasks {
    int width = 0, height = 0;
    std::vector<uint8_t> disk, sky;   // 1 = member; disjoint; neither = limb transition / unsure
    size_t diskCount = 0, skyCount = 0;
};
LunarMasks SegmentLunar(const ser::FloatImage& rgb, int threads);
}
```

- [ ] **Step 1:** Write tests in `test_mineral.cpp`, using `MakeMoonScene`:
  - **`segment: full disk`.** ≥ 98 % of `diskTruth` is in `disk`; ≤ 0.5 % of `skyTruth` is in `disk`; ≥ 95 % of `skyTruth` is in `sky`.
  - **`segment: disk cut by the frame top and bottom`.** A 1920×1080 frame with radius 700, like 64.6F. Same bars.
  - **`segment: crescent with terminator`.** Lit pixels ≥ 10 % of the lit median are ≥ 95 % in `disk`.
  - **`segment: no sky`.** The disk fills the frame: `skyCount` < 1 % of pixels, and nothing throws.
- [ ] **Step 2:** Run them and confirm they FAIL.
- [ ] **Step 3:** Implement `SegmentLunar`. One line on approach: luminance threshold from the image's own histogram, with morphological clean-up. The engine's disc measurement in `Measure.h` is a starting point.
- [ ] **Step 4:** Run them and confirm they PASS.
- [ ] **Step 5:** Commit: `feat(planetary-stack): lunar disk and sky segmentation`.

### Task B3: Colour calibration and background (spec B2, B3)

**Interfaces:**
- Consumes: `LunarMasks`.
- Produces:
```cpp
struct StepRecord { std::string history; bool skipped = false; };   // one ASCII HISTORY line
StepRecord CalibrateColour(ser::FloatImage& rgb, const LunarMasks& m);      // white = lit disk (unsaturated), bg = sky
StepRecord NeutraliseBackground(ser::FloatImage& rgb, const LunarMasks& m); // per-channel pedestal + linear gradient on sky only
```

- [ ] **Step 1:** Write the tests:
  - **`calibrate: cast removed`.** Cast (1.2, 1.0, 0.8) and pedestal 0.02. After both steps, disk mean R, G and B are equal within 1 %.
  - **`background: sky neutral and flat`.** Gradient 0.01 across the frame. Sky mean per channel ≤ 0.5 % of disk mean. Residual sky gradient (plane fit) ≤ 10 % of the input gradient.
  - **`calibrate: clipped pixels excluded`.** 3 % of disk pixels set to 1.0 in R only. The result is still within 1 %.
  - **`calibrate: tiny disk skips the white reference`.** Disk is 0.5 % of pixels: `skipped` is true, `history` says why, and the image is unchanged.
  - **`background: no sky`.** `skipped` is true, `history` names the reason, and nothing is invented.
  - **`history ASCII`.** Every `history` byte is < 0x80.
- [ ] **Step 2:** Run them and confirm they FAIL.
- [ ] **Step 3:** Implement. The disk-fraction threshold for the skip is 1 % of pixels, and the sky threshold is 1 % (spec B2).
- [ ] **Step 4:** Run them and confirm they PASS.
- [ ] **Step 5:** Commit: `feat(planetary-stack): mineral moon colour calibration and background`.

### Task B4: Luminance wavelet sharpening (spec B4)

**Interfaces:**
- Produces: `StepRecord SharpenLuminance(ser::FloatImage& rgb, const LunarMasks& m, int threads);`
- Approach: luminance only, with chroma ratios preserved. Starlet (à trous, B3-spline) layers 1–4, with each layer's gain limited by its measured noise. These are the routines increment 2's sliders will reuse, so expose them:
```cpp
std::vector<ser::FloatImage> StarletLayers(const ser::FloatImage& lum1, int layers, int threads);  // last = residual
double LayerNoise(const ser::FloatImage& layer, const std::vector<uint8_t>& region);              // robust (MAD) std
```

- [ ] **Step 1:** Write the tests:
  - **`sharpen: closer to the truth`.** Blur σ 1.5 and noise 0.005: RMS error against `truth` luminance on the disk falls by ≥ 10 %.
  - **`sharpen: noise-only gain fails`.** Flat field plus noise: sky noise std rises ≤ 1.5×.
  - **`sharpen: no limb halo`.** On the radial profile across the limb, sky-side overshoot ≤ 2 % of the disk mean.
  - **`sharpen: chroma untouched`.** Per-pixel R/G and B/G change by ≤ 1e-4 wherever G ≥ 0.05.
  - **`starlet: layers sum to the input`.** Max abs error ≤ 1e-5.
- [ ] **Step 2:** Run them and confirm they FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run them and confirm they PASS.
- [ ] **Step 5:** Commit: `feat(planetary-stack): noise-aware luminance wavelet sharpening`.

### Task B5: Chroma noise reduction (spec B5)

**Interfaces:**
- Produces: `StepRecord DenoiseChroma(ser::FloatImage& rgb, const LunarMasks& m, int threads);`
- Approach: luminance/chroma split (the colour space is the builder's call). Chroma is smoothed at larger scales and luminance is left untouched.

- [ ] **Step 1:** Write the tests:
  - **`chroma: error halved`.** Truth plus per-channel noise 0.01: chroma error against truth (RMS of R/G and B/G differences on the disk) falls ≥ 50 %.
  - **`chroma: luminance kept`.** Luminance RMS error against truth moves ≤ 2 %.
  - **`chroma: edge band`.** Chroma noise std 0.03 in x 0–5 with the disk touching the left edge: after the step, `edge/ref` chroma std ≤ 1.5× (the same bands as `edge_chroma.py`).
  - **`chroma: maria/highland tint survives`.** Mean maria B/R minus highland B/R keeps ≥ 90 % of its input value.
- [ ] **Step 2:** Run them and confirm they FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run them and confirm they PASS.
- [ ] **Step 5:** Commit: `feat(planetary-stack): chroma noise reduction`.

### Task B6: Saturation and highlight clean-up (spec B6, B7)

**Interfaces:**
- Produces:
```cpp
StepRecord BoostSaturation(ser::FloatImage& rgb, const LunarMasks& m);   // iterative, soft roll-off, strength from the image
StepRecord CleanHighlights(ser::FloatImage& rgb, const LunarMasks& m);   // out-of-gamut bright pixels toward neutral
```
- **Strength reference:** Scott's three S-curves and his highlight PixelMath, from journey 240 in `~/.local/share/PICopilot/journeys/journeys.sqlite3`. Read them, and record the values in the ledger. They are the yardstick for "done" strength (target: output chroma on the disk within ±25 % of what his curves give on the same input). They are not hard-coded.

- [ ] **Step 1:** Write the tests:
  - **`saturation: maria blue, highlands orange`.** Output mean maria B > R and highlands R > B, by ≥ 3× the input differences.
  - **`saturation: adaptive`.** Two scenes whose input tints differ by 2× reach output disk chroma within 20 % of each other.
  - **`saturation: in gamut`.** No sample > 1.0, and no sample < 0.
  - **`highlights: no cyan cast`.** In the brightest 1 % of disk pixels after both steps, the mean of ((G+B)/2 − R)/L is ≤ 0.02.
  - **`history`.** Each step records its passes and the strength it chose.
- [ ] **Step 2:** Run them and confirm they FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run them and confirm they PASS.
- [ ] **Step 5:** Commit: `feat(planetary-stack): gentle iterative saturation and highlight clean-up`.

### Task B7: The pipeline and display transfer (spec B8)

**Interfaces:**
- Consumes: B2–B6.
- Produces:
```cpp
struct MineralOutcome {
    ser::FloatImage image;               // display-ready RGB, [0,1]
    std::vector<std::string> history;    // one line per step, in order (ASCII)
    std::vector<std::pair<std::string, double>> stepSeconds;
};
// Throws std::invalid_argument for a non-3-channel image; StackCancelled when progress cancels.
MineralOutcome MakeMineralMoon(const ser::FloatImage& stack, int threads, const Progress& progress);
constexpr const char* kPhaseMineral = "Mineral moon";
```
- Order: SegmentLunar → CalibrateColour → NeutraliseBackground → SharpenLuminance → DenoiseChroma → BoostSaturation → CleanHighlights → display transfer.
- Display transfer: disk median 0.35 ± 0.10 after the transfer. Samples that were ≥ 1.0 in the stack are the only ones allowed at 1.0.

- [ ] **Step 1:** Write the tests:
  - **`pipeline: display ready`.** Disk median is in [0.25, 0.45].
  - **`pipeline: no new clipping`.** The count of samples ≥ 0.999 is ≤ the input's count, and no sample > 1.0.
  - **`pipeline: edge chroma survives the pipeline`.** The B5 edge scene, run through everything: edge/ref chroma ≤ 1.5×.
  - **`pipeline: small disc completes`.** A 640×480 frame with disc radius 60: valid output, no sample > 1.
  - **`pipeline: cancel throws StackCancelled`.** Progress cancels at `kPhaseMineral`, and no image is returned.
  - **`pipeline: mono refused`.** A 1-channel input throws `invalid_argument`.
  - **`pipeline: history order`.** Eight lines, in step order.
- [ ] **Step 2:** Run them and confirm they FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run them and confirm they PASS. Run the full ctest three times.
- [ ] **Step 5:** Commit: `feat(planetary-stack): mineral moon pipeline`.

### Task B8: Module wiring — parameters, outputs, interface

**Files:**
- Modify: `src/module/PlanetaryStackParameters.{h,cpp}`. Add `PSTarget` (enum: `Auto`=0, `Other`, `Moon`, `Mercury`, `Venus`, `Mars`, `Jupiter`, `Saturn`, `Uranus`, `Neptune`; default `Auto`; ids `target`) and `PSMineralMoon` (bool, default false; id `mineralMoon`).
- Modify: `src/module/PlanetaryStackInstance.{h,cpp}`.
  - Add `p_target` and `p_mineralMoon`. `Assign` and `LockParameter` cover them.
  - `ExecuteGlobal` resolves `Auto` through `ParseCaptureName(filePath)`. If `p_mineralMoon` is set and `MineralMoonRefusal` returns a reason, it throws `Error(reason)` before stacking.
  - After `OpenStackWindow`, when mineral: `MakeMineralMoon`, then `ShowNewImageWindow(..., ViewIdStem(path) + "_mineral", ...)`.
- Modify: `src/module/StackCards.h`.
  - `OBJECT` card = `TargetName(target)` unless `Other`, plus a HISTORY line
    `PlanetaryStack target: <T> (from file name|set by user)`.
  - `MineralHistory(const MineralOutcome&)` gives `HISTORY` lines prefixed `PlanetaryStack mineral: `.
  - `StackInfoLine` notes "target from file name: <T>", or "target not in file name".
- Modify: `src/module/PlanetaryStackInterface.{h,cpp}`.
  - Add the Target combo, preselected from the parsed name on Open, with user override.
  - Add a "Mineral moon" checkbox, enabled only when `MineralMoonRefusal` returns nullopt; the tooltip shows the reason when disabled.
  - When the box is checked, the preview shows `MakeMineralMoon`'s image after Stack!. Unchecking shows the stack again without restacking.
- Test:
  - `test/unit/test_stack_cards.cpp`: OBJECT, the mineral HISTORY prefix and order, the info-line text.
  - `test/probe-stack.js` and `test/run-probe-stack.sh`: a Moon-named synthetic capture with `mineralMoon=true` gives windows `*_stack` and `*_mineral`, and HISTORY holds the 8 mineral lines. An HaO3-named capture with `mineralMoon=true` makes `executeGlobal` fail with the narrowband text. A Saturn-named capture gives `OBJECT='Saturn'`.
  - `test/gui-smoke-stack.sh`: Open the Moon fixture: Target shows Moon and the checkbox is enabled. Check it, Stack!, and screenshot the preview. Execute gives two windows. Cancel during the mineral phase leaves no window. Open the HaO3 fixture: the checkbox is disabled and the tooltip text is checked.
  - `test/make-fixtures.cpp`: emit Moon / HaO3-Moon / Saturn named fixtures.

- [ ] **Step 1:** Write the stack_cards unit tests and the probe/GUI-smoke assertions above.
- [ ] **Step 2:** Run them and confirm they FAIL: `ctest -R test_stack_cards`, `bash test/run-probe-stack.sh`.
- [ ] **Step 3:** Implement the parameters, instance, cards and interface.
- [ ] **Step 4:** Run ctest, `run-probe-stack.sh` and `gui-smoke-stack.sh <scratch>`. All PASS, and the screenshots are reviewed.
- [ ] **Step 5:** Commit: `feat(planetary-stack): Target and Mineral moon in PlanetaryStack`.

### Task B9: Real-data acceptance

**Files:**
- Modify: `test/stack-probe.cpp`.
  - Add `--target auto|<name>` and `--mineral`.
  - Add `--mineral-out x.fits`, the mineral image.
  - Add `--mineral-stages DIR`, which writes the linear luminance before and after sharpening and before the display transfer, as FITS for DAN_interior.
- Create: `test/run-mineral-real.sh` (host-only, NAS read-only) and `test/mineral_check.py`.
  - The script runs every capture below and prints each spec requirement's number with PASS/FAIL. That includes:
    - `edge_chroma.py` on the mineral output;
    - DAN_interior rise from `bench/metric.py` on the stages;
    - the `≤ 5 s` mineral timing, from `stepSeconds`, at 32 threads.
  - It writes a side-by-side PNG per capture. For 64.6F, the side-by-side is against `~/Desktop/Mineral_Moon_10_26_2026.png`, registered by cross-correlation and cropped to the common field.
- Captures (paths under `/mnt/qnap/astro_data/`):
  - `2026-10-23/Moon/` 64.6F and 70.0F
  - `2026-05-28/Moon/` ×2
  - `2_1_2024/2024-02-01-013847-Moon-Bin1 Lqef 14.9F.avi`
  - `2_1_2024/2024-02-01-014259-Moon-Bin1 Lqef 14.0F.avi`, run with `--bayer RGGB`
  - `2_20_2024/2024-02-19-191344-Moon-Bin1 Lqef 16.0F.avi`, run with `--bayer RGGB`
  - `8_31_2023/2023-08-30-220451-Moon-Bin1 LPro 14.0F.avi`, run with `--bayer RGGB`
  - Gating run: `2_1_2024/2024-02-01-013058-Moon-Bin1 HaO3 16.2F.avi` with `--mineral` must exit non-zero with the narrowband text.

- [ ] **Step 1:** Implement the probe flags and the script.
- [ ] **Step 2:** Run `bash test/run-mineral-real.sh <scratch>/mineral`. Every numeric requirement must PASS on every capture. Any FAIL gets one investigation round with its numbers recorded in the ledger before any change.
- [ ] **Step 3:** Commit the scripts and the ledger numbers: `test(planetary-stack): mineral moon real-data acceptance`.
- [ ] **Step 4:** Show Scott the side-by-side PNGs inline. His judgement is the bar.

### Task B10: Docs, portability, branch review, release (release on Scott's go-ahead only)

**Files:**
- Modify: `modules/planetary/doc/tools/PlanetaryStack/` (Target, Mineral moon: what each step does, when the option is offered, the two outputs).
- Modify: `PlanetaryVersion.h` (0.4.0.0), the Planetary package description, `CLAUDE.md`'s "What ships" Planetary line, and project memory.

- [ ] **Step 1:** Write the docs, using ASCII in any PCL literal.
- [ ] **Step 2:** Run `tools/build-env/build-modules.sh`, then `tools/build-env/verify-portable.sh`, then the probes against the container build (`PLANETARY_SO=build-portable/...`). All PASS.
- [ ] **Step 3:** Dispatch a fresh reviewer on the most capable model for a whole-branch review. Fix its findings under the no-deferring rule.
- [ ] **Step 4:** Ask Scott for the release go-ahead. Then: version bump, `./release.sh`, ff-merge, push, and verify the served manifest and sha1.
