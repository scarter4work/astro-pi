# PlanetaryStack planet processing with live sliders (0.5.0.0) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** For colour, broadband planet captures (Mercury–Neptune), PlanetaryStack opens a second, display-ready image, `<stack>_planet`. It has these steps:
- calibration to the planet's catalogue colour;
- background neutralisation;
- small-disc dispersion alignment;
- noise-limited sharpening;
- natural-colour saturation.

Saturation, Chroma blur and Sharpen are live sliders. The mineral moon is unchanged, bit for bit.

**Architecture:** The planet path is a new engine unit, `src/stack/Planet.{h,cpp}`. It reuses `Mineral.{h,cpp}`'s steps (`SegmentLunar`, `NeutraliseBackground`, `StarletLayers`, `SharpenLuminanceImage`, `ChromaLayer`, `MineralColour`, `DisplayTransfer`). Each step it changes gets an explicit option whose default is today's Moon behaviour:
- a sharpen profile;
- a linear display transfer;
- a pixel-count calibration floor.

The planet path keeps the prepare/finish split and its cache, so a slider re-runs only the finish step. In the module layer:
- four new parameters;
- a post-processing row that follows the Target;
- a `_planet` output window.

**Tech Stack:** C++17, PCL (module layer only), Catch2 (ctest), Python 3 + numpy (real-data checks), Rocky 9 build container.

**Spec:** `docs/superpowers/specs/2026-10-29-planetary-planet-processing-design.md` (calls 1–10, acceptance 1–9, rulings P1–P6). Read it with this plan.

**Ledger:** start `.superpowers/sdd/2026-10-29-planetary-planet/progress.md`. Number its rulings Q1, Q2, … so they don't collide with the stacker's R* and the spec's P*.

## Global Constraints

- **PCL string literals:** `const char*` literals are ASCII only (no em dash, ellipsis or µ).
- **Threading:** no OpenMP; use `src/stack/Parallel.h`.
- **Builds:** shipped builds come only from the Rocky 9 container (`tools/build-env/build-modules.sh`) and must pass `tools/build-env/verify-portable.sh`.
- **PixInsight:** never `make install`. Launch PixInsight only through `tools/pi-headless.sh`, or the existing harness scripts that use it.
- **Ids:** module id `Planetary` and process ids `PlanetaryStack` / `SERViewer` never change.
- **No outside code:** no `ProcessInstance` calls, third-party tools, external programs or network.
- **Samples:** no output sample above 1.0, and no new clipping.
- **Steps that cannot run:** a step that cannot run says why in HISTORY and the result line. It never invents data.
- **Mineral moon:** its parameters, defaults, HISTORY and pixels are unchanged. Test: T4's `mineral output bit-identical`.
- **NAS:** `/mnt/qnap` is read-only.
- **Release:** 0.5.0.0 ships after Scott picks the Sharpen and Chroma blur defaults (spec P5) and the final review is clean. It goes through `./release.sh`.

## Review Focus

1. **A planet that fills almost nothing** (Neptune about 5 px, or no disc at all): every step skips with its reason and the output is valid. Test: T4's `tiny or absent disc completes`.
2. **Saturn's rings at the limb:** sharpening must not ring around the ring tips and Cassini gap, and the rings stay in the disc mask. Tests: T2's `ring stays in the disc`, and T3's `no halo around a ringed disc`.
3. **A planet near the frame edge** (a cropped ROI, or drift): alignment and sharpening must not fringe or ring at the frame edge. Test: T2's `disc cut by the frame edge`.
4. **Target overridden to a planet on a Moon capture** (a huge disc): the planet path completes and the small-disc rules do not break a large disc. Test: T4's `frame-filling disc completes`.
5. **A slider dragged while a finish run is in flight**, then the Target switched to the Moon: there is no stale planet preview and no crash. Test: T6's GUI smoke steps.

---

### Task T1: Capture names and the planet gate

**Files:**
- Modify: `src/ser/CaptureName.{h,cpp}`
- Test: `test/unit/test_capture_name.cpp`

**Interfaces:**
- Produces:
  - `std::optional<std::string> PlanetRefusal(Target, FilterClass, const FileInfo&, const std::string& filterToken)`, beside `MineralMoonRefusal`.
  - `bool IsPlanet(Target)`, true for Mercury–Neptune.

- [ ] **Step 1: Tests (RED).**
  - `ParseCaptureName("2023-02-11-192403-Mars-Bin1 16.5F.avi")` gives `parsed` true, Mars, an empty filter token, and `FilterClass::Unknown`.
  - The same holds for the Venus and Neptune 2_12_2023 names, and for `2023-02-12-025133-Moon-Bin1 15.0F.avi`, which gives Moon.
  - `PlanetRefusal`:
    - nullopt for Saturn, Lqef, colour;
    - nullopt for Mars, Unknown, colour;
    - refused for Saturn HaO3 (its text names HaO3 and narrowband);
    - refused for an IR-pass token;
    - refused for mono;
    - refused for Moon (the text points to Mineral moon);
    - refused for Other (the text reads "pick the planet in the Target list").
  - Every existing `MineralMoonRefusal` test stays as it is.
- [ ] **Step 2:** Run `ctest -R capture_name` and confirm it FAILs.
- [ ] **Step 3: Implement.** The old form is `YYYY-MM-DD-HHMMSS-<Target>-Bin<n> <temp>F`. `PlanetRefusal` shares the colour and filter tests with `MineralMoonRefusal` through one internal helper; it does not copy them.
- [ ] **Step 4:** Run `ctest` and confirm everything is green.
- [ ] **Step 5:** Commit: `feat(planetary): parse ASIAIR's filterless capture names; the planet gate`.

### Task T2: Planet colour calibration, the ring check and small-disc dispersion

**Files:**
- Create: `src/stack/Planet.{h,cpp}` (add both to `src/stack/CMakeLists.txt`)
- Create: `test/unit/PlanetSynth.h`, a synthetic planet:
  - disc radius r, limb darkening and band texture;
  - optional ring (inner and outer radius, tilt, Cassini gap);
  - a per-channel tint, background, gradient and noise;
  - a known R and B shift;
  - optional placement cut by the frame edge;
  - built like `MoonSynth.h`.
- Create: `test/unit/test_planet.cpp` (register it in `test/CMakeLists.txt`)

**Interfaces:**
- Consumes: `LunarMasks`, `SegmentLunar`, `StepRecord`, `kClippedLuma` (Mineral.h, Measure.h).
- Produces (Planet.h):
  - `struct BodyColour { double bOverG, rOverG; };`
  - `std::optional<BodyColour> PlanetReflectance(ser::Target)`: spec call 3's table, exactly. nullopt for Moon and Other. A comment cites Mallama, Krobusek & Pavlov 2017, Icarus 282, Table 7.
  - `StepRecord CalibratePlanetColour(ser::FloatImage& rgb, const LunarMasks& m, ser::Target t)`:
    - It scales R and B about the sky plane, as `CalibrateColour` does, so the disc mean's B/G and R/G equal the reflectance.
    - Floor: at least 30 unsaturated disc pixels (spec P1).
    - Its HISTORY names the source, the targets, the measured before/after ratios, and the B,V,Rc approximation.
  - `StepRecord AlignPlanetChannels(ser::FloatImage& rgb, const LunarMasks& m, int threads)`: spec P1's small-disc method. Its HISTORY gives the shifts, their σ, and whether a resample happened.

- [ ] **Step 1: Tests (RED).**
  - **Calibration:**
    - `calibration hits the catalogue colour`: discs of r = 6, 10, 20 and 100 with a known cast. Afterwards, the disc mean's B/G and R/G are within 2 % of the target, for Mars, Jupiter and Saturn.
    - `calibration floor`: a disc of 20 unsaturated px skips, with the reason; 40 px runs.
    - `clipped pixels excluded`.
  - **Ring:** `ring stays in the disc`. On a ringed synth at tilt 4° and 25°, `SegmentLunar`'s disk mask contains ≥ 95 % of the ring pixels above the 4 % contrast line.
    - If this FAILs on the current code, record ruling Q1 with the numbers. The fix goes in `Planet.cpp` as a planet-only mask step (for example, the union of components whose bounding box touches the largest one's), not in `SegmentLunar`.
  - **Dispersion:**
    - `dispersion residual`: r = 6, 10, 20, 100, with an R and B shift of 0.5 px along a random axis. The residual is ≤ 0.1 px.
    - `no false shift`: zero shift, 6 seeds. No applied shift exceeds 0.1 px.
    - `disc cut by the frame edge`: the disc centre is 3 px from the left edge. The step runs or skips, but leaves no channel misregistered by more than 0.1 px and adds no NaN.
- [ ] **Step 2:** Run `ctest -R planet` and confirm it FAILs.
- [ ] **Step 3: Implement.** Dispersion is the builder's choice. One candidate: Gauss–Newton on the whole disc plus a 4 px ring outside it, with one gain and offset per channel and the uncertainty from the residual. Reuse `ShiftedPlane` / `CubicWeights` from Mineral.cpp, moving them to an internal header only if needed.
- [ ] **Step 4:** Run `ctest` and confirm everything is green.
- [ ] **Step 5:** Commit: `feat(planetary-stack): planet colour calibration to the catalogue reflectance, small-disc dispersion alignment`.

### Task T3: Sharpening for small discs (profile + disc-scaled fade)

**Files:**
- Modify: `src/stack/Mineral.{h,cpp}` (`SharpenLuminanceImage`)
- Test: `test/unit/test_planet.cpp`, and `test/unit/test_mineral.cpp` (untouched tests only)

**Interfaces:**
- Produces (Mineral.h):
  - `struct SharpenProfile { std::array<double,4> gain, extra; double fadeRadiusFraction; };`
  - `extern const SharpenProfile kMoonSharpen;` holds `{1.5,2.5,1,1}`, `{3,1,0,0}` and fade fraction 0, where 0 means today's reach-based fade.
  - `SharpenLuminanceImage(..., double amount, const SharpenProfile& profile = kMoonSharpen)`.
    - With a fade fraction f > 0, layer j's fade runs from min(reach_j, f·R) to 2·min(reach_j, f·R). R is the disc's equivalent radius, sqrt(diskCount/π).
- Produces (Planet.h): `extern const SharpenProfile kPlanetSharpen;`. Its initial values are the Moon's gains and f = 0.15; T5 retunes them.

- [ ] **Step 1: Tests (RED).**
  - `small-disc sharpening reduces error against the truth`: PlanetSynth r = 10, 20, 100, blurred by a Gaussian of 1.0 and 1.5 px, noise 0.005. At amount 1.5 with `kPlanetSharpen`, the disc's RMS error against the sharp truth falls.
  - `no halo around a ringed disc`: across the limb and the ring tips, the overshoot above the sky is ≤ 2 % of the disc mean.
  - `sky noise`: the sky's noise std rises ≤ 1.5×.
  - The existing `test_mineral` cases stay as they are and green.
- [ ] **Step 2:** Run `ctest -R planet` and confirm it FAILs. With today's fade at r = 10, layers 2–4 get no boost and the error does not fall.
- [ ] **Step 3: Implement.** `EdgeWeight` takes the effective reach. `kMoonSharpen` reproduces today's arithmetic exactly; no reordering of floating-point operations.
- [ ] **Step 4:** Run `ctest` and confirm everything is green.
- [ ] **Step 5:** Commit: `feat(planetary-stack): sharpen profiles; disc-scaled limb fade for planets`.

### Task T4: The planet pipeline (prepare/finish) and its colour

**Files:**
- Modify: `src/stack/Planet.{h,cpp}`; `src/stack/Mineral.{h,cpp}` (the `DisplayTransfer` option)
- Test: `test/unit/test_planet.cpp`, `test/unit/test_mineral.cpp`

**Interfaces:**
- Produces (Mineral.h): `DisplayTransfer(..., DisplayCurve* curve = nullptr, double medianTarget = 0.35)`. A `medianTarget` ≤ 0 means linear (balance 0.5), with the same white-point rule.
- Produces (Planet.h):
  - `constexpr double kPlanetSaturationDefault = 1.0, kPlanetSaturationMax = 3.0;`
  - `inline double kPlanetChromaBlurDefault = 1.0, kPlanetChromaBlurMax = 6.0, kPlanetSharpenDefault = kSharpenDefault, kPlanetSharpenMax = kSharpenMax;` These are placeholders that T5 replaces with constexpr measured values.
  - `struct PlanetParams { double saturation = kPlanetSaturationDefault, chromaBlurSigma = kPlanetChromaBlurDefault, sharpen = kPlanetSharpenDefault; };`
  - `constexpr const char* kPhasePlanet = "Planet";`
  - `MineralCache PreparePlanet(const ser::FloatImage& stack, ser::Target t, int threads, const Progress&, const StackNoise* = nullptr);`
    - Its steps, in order, each with a HISTORY line: Planet disc, Colour calibration (T2), Background, Display transfer (linear), Channel alignment (T2), and the sharpen noise exactly as PrepareMineral measures it.
    - Shared code is factored out of `PrepareMineral`, not copied.
    - Throws `std::invalid_argument` for a non-planet target, as well as for PrepareMineral's own refusals.
  - `MineralOutcome FinishPlanet(const MineralCache&, const PlanetParams&, int threads, const Progress& = {});`
    - It sharpens with `kPlanetSharpen`, then colours with `MineralColour`: strength = saturation, devClamp 1e3 (no clamp), and the fade off (satThreshold 1e3), with the clip guard as is.
    - HISTORY: "Saturation" in place of "Mineral colour".

- [ ] **Step 1: Tests (RED).**
  - **Saturation:**
    - `saturation 1 is the natural colour`: the processed disc's mean B/G and R/G are within 2 % of the calibrated values.
    - `saturation 0 is grey`: R = G = B within 1e-6.
  - **No clipping:** `no new clipping`, at saturation 3 on a clipped-highlight synth.
  - **Linear display:**
    - `linear display`: output/input is constant over unclipped disc pixels, within 1e-5.
    - The brightest unclipped sample is ≤ 0.95.
  - **Odd discs:**
    - `tiny or absent disc completes`: r = 2 and an empty frame give a valid image and every skip's reason.
    - `frame-filling disc completes`: a MoonSynth frame run as Jupiter.
  - **Cancel:** `cancel throws StackCancelled`.
  - **Slider speed:** `finish is cheap`: FinishPlanet at 1920 × 1080 on 32 threads takes ≤ 0.2 s (Release build only; skip under Debug).
  - **Moon unchanged:** `mineral output bit-identical`. `MakeMineralMoon` on the existing MoonSynth fixtures matches checksums recorded **before** this task's first edit. Record them in Step 0.
- [ ] **Step 0:** Before any edit, record the MakeMineralMoon output checksums for the fixtures, and on real 64.6F through `stack-probe --mineral`. Write them to the ledger.
- [ ] **Step 2:** Run `ctest -R planet` and confirm it FAILs.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run `ctest` and confirm everything is green. Run `stack-probe --mineral` on 64.6F and confirm it is identical to Step 0.
- [ ] **Step 5:** Commit: `feat(planetary-stack): the planet pipeline -- linear display, catalogue colour, natural-colour saturation`.

### Task T5: Real planets — probe, measured defaults, series for Scott

**Files:**
- Modify: `test/stack-probe.cpp`, adding `--planet`, `--planet-out`, `--planet-stages`, `--saturation`, `--chroma-blur` and `--sharpen`, mirroring the `--mineral*` flags.
- Create: `test/run-planet-real.sh` (modelled on `run-mineral-real.sh`) and `test/planet_check.py`.
- Modify: `src/stack/Planet.h` (the measured defaults and `kPlanetSharpen`)

**Interfaces:**
- Produces: `planet_check.py <run-dir>`. It prints acceptance 1–7 per capture, with PASS / FAIL / NOT JUDGED, using spec P2/P3's definitions for 4 and 5. It also writes the side-by-side PNGs (8-bit).

The captures are the spec's acceptance table, with Bayer RGGB explicit where there is no sidecar. 2026-10-05 Saturn HaO3 must be refused, with exit 4 and the narrowband text.

- [ ] **Step 1:** Write `planet_check.py`'s fail-closed test, `test/test_planet_check.py`. It runs on a synthetic run dir: a fringe case exits 1, a clean case exits 0. Register it like `fpn_verdict_fails_closed`. Run it and confirm RED, then GREEN.
- [ ] **Step 2:** Run every capture. Record each disc's measured size, whether the rings are in the mask on real Saturn (spec P6), and acceptance 1–7. Any FAIL goes into a ledger ruling, and the fix goes into the owning task's code with a RED test first.
- [ ] **Step 3: Measure the defaults.**
  - **Chroma blur:** the smallest σ (from 0, 0.5, 1, 1.5, 2, 2.5) that halves the sky's chroma noise on every capture with a sky.
  - **Sharpen:** a series of amounts 0, 1, 1.5, 2, 3 and 4 on Saturn 2026-10-23, Jupiter 11_3_2023 and Mars. Report each amount's crispness (limb/edge gradient), its noise gain, and the error against the truth on the synth. Retune `kPlanetSharpen` gains, extra and f only if the series shows a deficit at the fine layers. Raise the max if 4 is not enough.
  - Write the picks as ruling Q2.
- [ ] **Step 4: Send to Scott.** Put PNG grids on `~/Desktop/planet-series/` (8-bit, upscaled ×4 nearest so 20 px discs are visible): the Sharpen series, the Chroma blur series, and plain-vs-processed for Saturn, Jupiter and Mars. Tell Scott in one message that they are there, with the measured default marked. This is the only stop. Carry on with T6 while he looks, and put his pick in as ruling Q3.
- [ ] **Step 5:** Commit: `test(planetary-stack): real-planet acceptance runner; measured planet defaults (Q2)`.

### Task T6: Module wiring — parameters, row, window, probes

**Files:**
- Modify: `src/module/PlanetaryStackParameters.{h,cpp}`, `PlanetaryStackProcess.cpp`, `PlanetaryStackInstance.{h,cpp}`, `PlanetaryStackInterface.{h,cpp}`, and `StackCards.h`
- Test: `test/unit/test_stack_cards.cpp`, `test/probe-stack.js` / `run-probe-stack.sh`, and `test/gui-smoke-stack.sh` (+ `gui_drive.py` if needed)

**Interfaces:**
- Consumes: T1's `PlanetRefusal` and `IsPlanet`; T4's `PreparePlanet`, `FinishPlanet`, `PlanetParams` and the k*Planet* constants.
- Produces: these parameters, with ranges and defaults from the Planet.h constants.

  | Id | Type |
  |---|---|
  | `planetProcess` | bool, default false |
  | `planetSaturation` | double |
  | `planetChromaBlur` | double |
  | `planetSharpen` | double |

- [ ] **Step 1: Tests (RED).**
  - **`test_stack_cards`:** the planet window's HISTORY and keywords cards (OBJECT = the planet, one HISTORY line per step) and the result text.
  - **Headless probe (slot 96):**
    - `planetProcess` true on a Saturn fixture opens `<id>` and `<id>_planet`.
    - On an HaO3 Saturn and on Target Other, the run fails loudly with `PlanetRefusal`'s text.
    - The mineral probe is unchanged and passes.
- [ ] **Step 2:** Run the tests and confirm they FAIL.
- [ ] **Step 3: Implement.**
  - **Planet target:** the post-processing row shows the **Planet processing** checkbox plus the Saturation / Chroma blur / Sharpen controls.
  - **Moon target:** the row shows today's mineral controls.
  - **Visibility:** only the active set is visible.
  - **Sliders:** they re-run `FinishPlanet` on the cached `MineralCache` through the same worker the mineral sliders use. Factor the worker over the kind; do not copy it.
  - **Stale results:** a Target switch drops a planet result that is in flight.
  - **Tooltips:** in the mineral ones' style, ASCII.
  - **Window:** `<stack>_planet`, opened as `ShowMineralWindow` opens the mineral one. Factor it; do not copy it.
- [ ] **Step 4:** Extend the GUI smoke (slot 97):
  - Open the Saturn fixture; the row shows Planet processing, enabled.
  - Click Stack!, move each slider, and check that the preview changes.
  - Execute opens two windows.
  - Switch Target to Moon: the mineral row shows and there is no planet preview.
  - Also drag Sharpen and switch Target at once (Review Focus 5).
- [ ] **Step 5:** Run `ctest`, `run-probe-stack.sh` and `gui-smoke-stack.sh`, and confirm all are green. Commit: `feat(planetary-stack): Planet processing option with Saturation / Chroma blur / Sharpen sliders`.

### Task T7: Docs, version, portability, final review, release

**Files:**
- `doc/tools/PlanetaryStack/` (the planet section, the parameters, the colour source)
- `src/module/PlanetaryVersion.h` (0.5.0.0, today's date)
- `package-description.xml` (0.5.0.0 text)
- the root `CLAUDE.md` "What ships" line for Planetary

- [ ] **Step 1:** Update the docs and the version. Use ASCII in PCL literals.
- [ ] **Step 2:** Run the container build and `verify-portable.sh`, and confirm both pass. Then run the full ctest in the container, all probes against `build-portable`, and `run-planet-real.sh` and `run-mineral-real.sh`, and confirm every one is green.
- [ ] **Step 3:** Get a whole-branch review from a fresh reviewer on the most capable model. Fix every finding with a RED test first, then re-review the fixes.
- [ ] **Step 4:** After Scott's pick (Q3) is in and the review is clean: run `./release.sh`, commit the version bump with `repository/`, and push. Verify that the published `updates.xri` lists Planetary 0.5.0.0 with a matching sha1.
