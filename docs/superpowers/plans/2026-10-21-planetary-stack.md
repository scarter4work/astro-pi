# PlanetaryStack (stack engine) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A new `PlanetaryStack` process in the Planetary module that opens an AVI/SER, measures every frame, selects and aligns automatically (global + per-region lucky imaging), stacks with rejection, previews with zoom, and opens the stack via Execute — or gives a plain diagnosis when nothing is usable — measured against Scott's AutoStakkert and ASIStudio results.

**Architecture:** A PCL-free engine library `modules/planetary/src/stack/` (namespace `planetary::stack`, CMake target `planetary_stack`, links `planetary_ser`) does all image work behind small interfaces and is unit-tested in isolation and in the Rocky 9 container. The PCL module adds `PlanetaryStackProcess/Parameters/Instance/Interface`; the interface runs the engine on a worker thread with a progress bar and cancel. A test-only CLI `stack-probe` and a Python harness run the real-data benchmark.

**Tech Stack:** C++17, PCL (module only), std::thread worker pool (no OpenMP, no GPU), Catch2 (unit), PJSR probe + XTest GUI smoke on private Xvfb, Python + numpy + ffmpeg + astropy-free FITS/TIF reading via PIL/numpy (test-only).

**Spec:** `docs/superpowers/specs/2026-10-21-planetary-stacker-design.md` (read it, especially "The engine — required behaviour", "Yardstick" and the Rulings section).

## Global Constraints

- Branch `feat/planetary-stack`; commit per task; **no push, no merge to main, no release** (spec Ruling 4).
- `src/stack/` PCL-free C++17; no OpenMP; no GPU code; no new third-party dependency (anything needed, e.g. an FFT, is written in-tree or already vendored — check `modules/nukex/third_party/` and reuse only header-only, license-compatible code with its LICENSE noted in `THIRD_PARTY_NOTICES.txt`).
- Input only through `planetary::ser::OpenVideo(path, BayerChoice)`; `FrameSource` is not thread-safe — every worker thread opens its own reader.
- Never hold the whole video: peak resident ≤ 4 GB for 4000 × 1920×1080 8-bit; 60 s local-disk CPU budget for that case (spec; a miss is reported with a phase breakdown, Ruling 3).
- Automatic selection only — no percentage parameter, no user alignment points, anywhere (UI, parameters, scripting).
- No image from a capture with no usable frames: a diagnosis with numbers instead (spec "Diagnosis").
- Process id `PlanetaryStack`; parameters `filePath` (String) and `bayerPattern` (enum with element ids `BayerAuto, BayerNone, BayerRGGB, BayerGRBG, BayerGBRG, BayerBGGR`, default `BayerAuto` — same ids as SERViewer).
- Output: 32-bit float RGB (or mono for mono input) PixInsight image, linear; FITS keywords: source file, frames used/measured, INSTRUME from the sidecar, HISTORY summary; never `DATE-OBS` from an AVI.
- PixInsight launches only via the existing harness (`modules/planetary/test/harness.sh`, `tools/pi-headless.sh`); probe slot 96, GUI slot 97; GUI driver rules in `modules/planetary/test/gui_drive.py` (pointer rests after clicks; offsets measured from screenshots).
- `/mnt/qnap` is read-only for every task. Signing pass file: written from memory `ez_stretch_bsc_signing.md` when a PI run needs it, shredded after.
- Commit messages end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus

1. A planet that drifts partly out of frame in some frames: those frames are excluded or used only where the target is fully present — never a clipped limb stacked in (Task 2/5 tests).
2. A capture where the target is tiny (Saturn at 320×240, ~40 px) and one where it fills the frame (Moon surface): both stack without seams or doubled limbs (Task 5 synthetic + Task 9 real).
3. Cancel during each phase: no leaked threads, no partial image window, interface usable again (Task 6/8 tests).
4. Very short captures (≤ 30 frames) and very long ones (≥ 20 000 frames): selection still sensible and memory bounded (Task 3/6 tests).
5. Bayer input with an unknown pattern (no sidecar, Auto → mono): stacking still works on the raw CFA as mono and the result says so (Task 6 test).

---

### Task 1: Engine scaffold, luminance/demosaic

**Files:** Create `modules/planetary/src/stack/{CMakeLists.txt, StackTypes.h, Demosaic.h, Demosaic.cpp}`; modify `modules/planetary/CMakeLists.txt`/`src/CMakeLists.txt` to add the library; tests `modules/planetary/test/unit/test_demosaic_hq.cpp` (+ CMake registration).

**Interfaces (produces):**
- `StackTypes.h`: `struct Progress { std::function<void(const std::string& phase, int64_t done, int64_t total)> report; std::function<bool()> cancelled; };` and `class StackCancelled : public std::runtime_error`.
- `ser::FloatImage LumaForMeasure(const ser::Frame&, ser::ColorId, int normDepth);` — fast measurement luminance: Bayer → half-resolution 2×2 sum (W/2×H/2), mono → as is, RGB → mean; normalised to [0,1].
- `ser::FloatImage DemosaicHQ(const ser::Frame&, ser::ColorId, int normDepth);` — final-quality demosaic (builder chooses the method, e.g. Malvar-He-Cutler); mono/RGB pass through normalised.

- [ ] Tests first: luma of a known CFA equals hand-computed 2×2 sums; DemosaicHQ on a synthetic sharp black/white edge (all 4 patterns) has max colour error at the edge ≤ half of `ser::DebayerBilinear`'s and no zipper (alternating error) on a flat area; exact on a flat grey field.
- [ ] Run (FAIL) → implement → run all ctest (PASS) → commit `feat(planetary-stack): engine scaffold, measurement luma, HQ demosaic`.

### Task 2: Measure every frame

**Files:** Create `src/stack/Measure.{h,cpp}`; test `test/unit/test_measure.cpp`.

**Interfaces:**
- `struct FrameStats { int64_t index; bool targetFound; double cx, cy; int bx0, by0, bx1, by1; double quality; double brightness; double clippedFraction; };`
- `FrameStats MeasureFrame(const ser::FloatImage& luma, int64_t index);` — quality statistic robust to brightness and noise (builder's method; documented in the header with why).
- `using SourceFactory = std::function<std::unique_ptr<ser::FrameSource>()>;`
- `std::vector<FrameStats> MeasureAll(const SourceFactory&, const ser::FileInfo&, int threads, const Progress&);` — every frame, `threads` workers each with its own reader, progress phase "Measuring frames", honours cancel (throws `StackCancelled`).
- [ ] Tests first: quality strictly decreases with Gaussian blur σ = 0, 0.5, 1, 2 on a synthetic disc with texture, at three brightness scalings and with added noise; empty (noise-only) frame → `targetFound == false`; target centroid within 0.5 px of truth; a target cut by the frame edge is reported with its box touching the edge; clippedFraction exact on a synthetic saturated disc; MeasureAll over a synthetic 200-frame AVI (AviWriter) matches per-frame MeasureFrame and is identical for threads = 1 and 8; cancel mid-run throws and joins all threads.
- [ ] FAIL → implement → PASS → commit.

### Task 3: Automatic selection and diagnosis

**Files:** Create `src/stack/Select.{h,cpp}`; test `test/unit/test_select.cpp`.

**Interfaces:**
- `enum class DiagnosisKind { None, NoTarget, OutOfFocus, PoorSeeing, Clipped, TargetLeavesFrame };`
- `struct Diagnosis { DiagnosisKind kind; std::string message; std::vector<std::pair<std::string,double>> numbers; };`
- `struct Selection { std::vector<int64_t> frames; double threshold; std::string explanation; Diagnosis diagnosis; };` — `frames` empty iff `diagnosis.kind != None`.
- `Selection SelectFrames(const std::vector<FrameStats>&, const ser::FileInfo&);` — automatic, adaptive from the distribution (builder's rule; documented), explanation in one line in the spec's style ("used 612 of 4088 frames: the sharpest group, quality ≥ 0.71 of the best").
- [ ] Tests first: a bimodal distribution (steady vs poor seeing synthetic stats) keeps the good mode; a uniformly good capture keeps most; each DiagnosisKind triggered by synthetic stats with its numbers present; frames with `targetFound == false` or a box touching the edge are never selected (Review Focus 1); ≤ 30 frames and 20 000 frames behave sensibly (Review Focus 4).
- [ ] FAIL → implement → PASS → commit.

### Task 4: Global alignment

**Files:** Create `src/stack/Align.{h,cpp}` (+ any in-tree FFT `src/stack/Fft.{h,cpp}`); test `test/unit/test_align.cpp`.

**Interfaces:**
- `struct Shift { double dx, dy; double confidence; };`
- `ser::FloatImage BuildReference(const SourceFactory&, const ser::FileInfo&, const Selection&, const std::vector<FrameStats>&, const Progress&);` — reference from the best frames (builder's method), measurement-luma scale or full scale (document which).
- `Shift GlobalShift(const ser::FloatImage& reference, const ser::FloatImage& frameLuma, const FrameStats&);` — sub-pixel; confidence ∈ [0,1]; frames below a documented confidence are excluded later.
- [ ] Tests first: recovers synthetic integer and sub-pixel shifts (±0.1 px) on a textured disc and on a frame-filling lunar-like texture; noise robustness; a frame of a different scene → low confidence.
- [ ] FAIL → implement → PASS → commit.

### Task 5: Regions, local alignment, lucky per-region stacking, blending, rejection

**Files:** Create `src/stack/Regions.{h,cpp}`, `src/stack/Accumulate.{h,cpp}`; tests `test/unit/test_regions.cpp`, `test/unit/test_accumulate.cpp`.

**Interfaces:**
- `struct Region { int x0, y0, x1, y1; double weight; };` `std::vector<Region> PlaceRegions(const ser::FloatImage& reference, const FrameStats& refStats);` — automatic on structure; works for a small disc and a frame-filling surface.
- `struct RegionPlan { /* per region: chosen frames + local shifts */ };` built from a measurement pass over the selected frames (per-region quality + local shift vs reference).
- `class Accumulator` — streaming: `Add(frameIndex, const ser::FloatImage& fullResFrame, global shift, per-region shifts)`; `ser::FloatImage Finish();` with outlier rejection (builder's method, documented) and seamless region blending (feathered weights).
- [ ] Tests first: synthetic frames with a known base image + per-region random sub-pixel warps + per-frame per-region blur → result closer to the base (RMS) than the plain global-aligned mean of the same frames; a flat field produces no seam (max gradient across region borders ≤ noise); a single-frame outlier (bright streak) is rejected; small disc (40 px) and full-frame texture both pass; memory: Accumulator holds O(regions × region size + one frame), asserted via a size accessor.
- [ ] FAIL → implement → PASS → commit.

### Task 6: Engine orchestration

**Files:** Create `src/stack/Engine.{h,cpp}`, test-only CLI `modules/planetary/test/stack-probe.cpp` (not a ctest case); test `test/unit/test_engine.cpp`.

**Interfaces:**
- `struct StackSummary { int64_t measured, used; std::string selection, alignment; double seconds; std::vector<std::pair<std::string,double>> phaseSeconds; double readMBps; std::vector<std::string> warnings; };`
- `struct StackOutcome { std::optional<ser::FloatImage> image; Diagnosis diagnosis; StackSummary summary; ser::FileInfo info; };`
- `StackOutcome RunStack(const std::string& path, ser::BayerChoice, int threads, const Progress&);` — phases "Measuring frames" → "Aligning" → "Stacking" (progress totals per phase), two streaming passes, threads workers each with own reader, memory bound, cancel → `StackCancelled` with all threads joined. Bayer with unknown pattern → stacks the CFA as mono with a summary warning (Review Focus 5).
- `stack-probe <video> [--bayer RGGB|...] [--threads N] [--out stack.fits|.tif] [--json]` prints the summary JSON (incl. phase times, read throughput, peak RSS) and writes the image (simple FITS or 16/32-bit TIFF writer, test-only).
- [ ] Tests first: end-to-end synthetic AVI → image closer to truth than the best single frame; a no-target capture → diagnosis NoTarget and no image; cancel in each phase (Review Focus 3); unknown Bayer → mono + warning; threads 1 vs 8 results equal within 1e-6.
- [ ] FAIL → implement → PASS; quick real-data smoke with stack-probe on one NAS Jupiter (report time + RSS) → commit.

### Task 7: PCL process PlanetaryStack (headless)

**Files:** Create `modules/planetary/src/module/PlanetaryStack{Process,Parameters,Instance}.{h,cpp}`; modify `PlanetaryModule.cpp` (register), `src/module/CMakeLists.txt`, the module's link to `planetary_stack`; probe: extend `test/make-fixtures.cpp` + a new `test/probe-stack.js` + `test/run-probe-stack.sh` (or extend run-probe.sh cleanly).

**Interfaces:** parameters per Global Constraints; `ExecuteGlobal()` runs `RunStack` with `StandardStatus`/`StatusMonitor` progress and console abort, opens the image window with keywords, or throws with the diagnosis message (loud, no window). Shared Bayer/sidecar code with SERViewer is reused, not copied.
- [ ] Probe cases first: success on a synthetic Bayer AVI with sidecar (window, size, channels, keywords, a few pixel values within tolerance vs the engine's own output from stack-probe), mono input, each diagnosis (no window, message text), bad file. FAIL → implement → PASS (existing SERViewer probe still 36/36) → commit.

### Task 8: PCL interface

**Files:** Create `PlanetaryStackInterface.{h,cpp}` (+ a zoomable preview control, reusing `FramePreview`/`PreviewPixels` where sensible); GUI smoke `test/gui-smoke-stack.sh` + driver modes in `gui_drive.py`.

**Behaviour:** Open… (same dialog/filters as SERViewer), info line, Bayer combo + Save pattern... (shared code), **Stack!** runs `RunStack` on a worker thread (pcl::Thread or std::thread with a pcl::Timer polling progress — UI stays responsive), a progress bar with the phase text and **Cancel**; on success a preview (display-stretched only; data stays linear) with Fit / 100 % / 200 % / zoom in/out and drag-pan, the result line; **Execute** opens the image window (same as ExecuteGlobal); on diagnosis: no preview, the diagnosis text + numbers. Every handler Guarded; closing the interface cancels and joins a running stack.
- [ ] GUI smoke first (screenshots: opened, progress bar mid-run, preview fit, 100 %, 200 %, after Execute the window exists, a diagnosis case with no preview, cancel mid-run returns to idle). FAIL → implement → PASS 3/3 (look at every PNG) → commit.

### Task 9: Benchmark harness, metric, and the real-data run

**Files:** Create `modules/planetary/test/bench/{bench.py, metric.py, refs.py, README.md}`, `modules/planetary/test/run-stack-bench.sh`.

- [ ] Metric first (`metric.py`): one documented sharpness-at-equal-noise measure on the registered target crop (same crop/normalisation for all three images); validation tests: a blurred copy ranks below the original; added noise does not raise it; on `2_12_2023` AS_P20 vs (if present) a lower-percentage/higher-percentage pair it ranks as AS's selection implies — record the numbers.
- [ ] `refs.py`: load AS (`.fit`/`.tif`) and ASI (`.fit`/`.jpg`) references; detect drizzle from AS file names (`Drizzle15/30` → ×1.5/×3) and area-resample to native (Ruling 1); register each reference to our stack (sub-pixel shift + scale check) before measuring.
- [ ] Benchmark set: choose from the spec's folders (≥ 3 planet captures, ≥ 3 lunar if references exist), record exact paths and why in `test/bench/README.md`; include the empty 2026-01-30 Saturn AVI as a diagnosis case.
- [ ] `run-stack-bench.sh`: for each capture run `stack-probe` (timing, RSS), compute metrics for ours/AS/ASI, write `test/output/bench/<capture>/side-by-side.png` (ours | AS | ASI at the same scale and stretch, labelled) and one `summary.md` table. Output dir is gitignored.
- [ ] Run it. Any capture below 0.97 × best or with a visible failure mode: one investigation round on the engine (fix with a unit test reproducing the problem), re-run, record before/after numbers (Ruling 2). Commit harness + engine fixes.

### Task 10: Docs, container build, release-readiness (no release)

**Files:** `modules/planetary/doc/PlanetaryStack/PlanetaryStack.html` (what it does, automatic selection explained in plain words, diagnosis messages, what it reads/writes, no network, no external programs); `PlanetaryModule.cpp` `Description()` (PCL acknowledgment last, unchanged); `modules/planetary/README.md`; root `README.md` (Planetary line); `CLAUDE.md` (Planetary: PlanetaryStack on branch, unreleased; test commands); `release.sh` + its package checks so the Planetary tarball deploys BOTH `doc/tools/SERViewer/` and `doc/tools/PlanetaryStack/` (extend the doc-id handling without changing NukeX/PICopilot packages byte-for-byte).
- [ ] `tools/build-env/build-modules.sh` (container; all tests), `verify-portable.sh` on the three .so files, `python3 tools/check-pi-launches.py`, `./release.sh --package-test <dir> …` (package checks pass with both doc dirs), probe + GUI smoke against the container build. Version stays 0.2.1.0 in source (the release bump happens when Scott approves). Commit.
