# NukeX v5 Outstanding Program — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. **Do NOT use subagent-driven-development on this plan** — the operator's environment instructions forbid dispatching the Agent tool unless explicitly requested.

**Goal:** Close all eight outstanding NukeX v5 items — the stacker's no-data defect, six deferred minors, LRGB-mono stacking, session-drift alignment, GPU host-RAM sizing, narrowband chroma, the updater-install proof and auto-stretch faintness — and ship the result as one release.

**Architecture:** Four of the eight move E2E pixel output, so the plan is ordered by *blast radius*, not by size: every golden-moving change lands before a single re-baseline at the end, so the multi-hour E2E corpus runs once rather than four times. The two architectural items (LRGB-mono frame sets, chained alignment) get their own design documents before code, because each rewrites a data path that the GPU and CPU must continue to agree on.

**Tech Stack:** C++17, CMake, Catch2 v3 (amalgamated in `third_party/catch2`), nlohmann/json, Eigen, cfitsio (FetchContent), vendored SQLite, OpenCL, PCL, Python 3 (tools), PJSR (E2E harness).

**Spec:** This plan is its own spec for Tasks 1–4 and 7–9; Tasks 5 and 6 each begin by writing a design document, named in the task. Prior art: `docs/superpowers/specs/2026-09-04-per-channel-registration-design.md`, `docs/superpowers/plans/2026-09-02-color-science-path-to-done.md`.

## Global Constraints

- **Release workflow (CLAUDE.md):** never `make install`; bump `src/module/NukeXVersion.h` + release date; clean build; ctest; `tools/release.sh package`; commit version bump + package together; push. `tools/release.sh` refuses a stale build and needs `/tmp/.pi_codesign_pass`.
- **No `lenient()`, no stubs, no TODOs, no silent fallbacks. Loud errors. Root-cause fixes only.**
- **Never put non-ASCII in a PCL string literal** — PCL reads `const char*` as ISO-8859-1. Scan for ESCAPED bytes (`\xHH`, octal, `\uXXXX`) too, not just visible characters.
- **Never `sudo` into `/opt/PixInsight`** — it is user-owned; root-owned files break PI's own updater half-way.
- **Tests register in `test/CMakeLists.txt`** via `nukex_add_test(<name> <path-relative-to-test/> <libs...>)`.
- **Baseline before starting:** `main` at `deb73bf`, tag `v5.0.1.1`, 73/73 ctest green.
- **Commit trailer** on every commit:
  ```
  Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01TZPeLuJe7qz1F9NFnnhTNZ
  ```

## Execution order and the single-re-baseline rule

| Wave | Tasks | Moves goldens? | Why here |
|---|---|---|---|
| 1 | 1 (chroma), 2 (CoverageMask) | **Yes, both** | Independent subsystems (compose vs stacker). Both are root-caused already. |
| 2 | 3 (six minors), 4 (GPU RAM cap) | 3: no. 4: must be *proven* no | Task 3 touches files Task 2 edits, so it follows Task 2. Task 4 carries an output-invariance gate. |
| 3 | 5 (LRGB-mono) | **Yes** | Design doc first. Largest data-path change; needs GPU/CPU agreement re-proof. |
| 4 | 6 (chained alignment) | **Yes** | Design doc first. Depends on nothing above, but is sequenced last among code tasks to keep one re-baseline. |
| 5 | 7 (stretch faintness) | Yes (stretched only) | Measurement before tuning. |
| 6 | 8 (re-baseline + release) | — | The ONE golden regeneration. Then package, sign, tag, push. |
| 7 | 9 (updater GUI proof) | — | Must be last: it deletes the dev-staged module every E2E run depends on. Needs the operator's hands. |

**The rule:** no task between 1 and 7 regenerates a golden. They record *expected* drift in their own commit message; Task 8 regenerates all of them once and attributes each delta.

---

## Task 1: Narrowband chroma — hue must not depend on brightness

**The defect:** `ColorComposer::compose_pixel` computes `total_w` and then uses it **only as a guard** — the emission weights are never divided by it. So Lab chroma carries absolute signal instead of the line ratio. Measured on `bayer_nb_hao3_m16` (12 frames, 24.5 MP): the composed window has **10 non-zero green pixels out of 24,507,450**, a magenta background, and `NUKEX_GAMUT_CLIPPED` reports 24,516,516 of 24,543,024 pixels against 126 on the plain-OSC corpus of the same camera and size.

**The standard, and why this is a bug rather than a taste question:**

- **Lupton et al. 2004, PASP 116, 133** ("Preparing Red-Green-Blue Images from CCD Data") states the failure directly: *"for any non-linear function F, an object's color in the composite image depends upon its brightness."* Their correction defines `I = (r+g+b)/3` and sets `R = r*f(I)/I`, `G = g*f(I)/I`, `B = b*f(I)/I`, so a given astronomical colour maps to a unique image colour. For out-of-gamut they divide **all three channels by `max(R,G,B)`** — *"the intensity is clipped at unity, but the color is correct."* If `I == 0`, output is black.
- **Rector, Levay, Frattare, English & Pu'uohau-Pummill 2004, AJ** ("Image-Processing Techniques for the Creation of Presentation-Quality Astronomical Images") documents STScI practice: each narrowband layer is colorized at a **fixed hue with saturation set to 100%** — saturation is a per-layer constant, never a function of the pixel's absolute signal. On M16 specifically they note the layers are *"balanced so that the Hα data doesn't dominate and turn the image completely green."*

NukeX has Rector's exact failure rotated by one line: Hα dominates and turns the frame completely red. Both references therefore call for (a) hue as a line ratio and (b) a pre-combination layer balance.

**Files:**
- Modify: `src/lib/compose/include/nukex/compose/color_composer.hpp`
- Modify: `src/lib/compose/src/color_composer.cpp:110-142`
- Modify: `test/unit/compose/test_color_composer.cpp`

**Interfaces:**
- Produces: `void ColorComposer::set_chroma_gate(double snr_floor)`; `double ColorComposer::last_pixel_chroma_scale() const`; unchanged `sRGBPixel compose_pixel(const DerivedSlots&)`.
- Consumes: `Palette::for_line(EmissionLineId)` → `LabColor{L,a,b}`, values Ha `{0,+50,+10}`, OIII `{0,-15,-35}`, SII `{0,+60,+25}`.

**Two findings from reading the existing tests, which change the implementation:**

1. The existing case `"ColorComposer: gamut soft-clip preserves hue, increments counter"` asserts only `out.r >= out.g` and `out.r >= out.b` for a pure-Hα pixel. Independent per-channel clamping satisfies that by luck, so **the test never tested hue**. The Step 6 test below is the real one; the existing case keeps passing under the Lupton rescale.
2. **Normalising by `total_w` has a cliff at small `total_w`.** The guard `if (total_w > 0.0)` is exact-zero only, so a pixel left at `1e-9` after continuum subtraction would divide down to the *full* palette vector — saturated colour manufactured from noise, which is a worse defect than the one being fixed. The chroma gate is therefore **not optional polish**; it is what makes normalisation safe, and the engine must supply its floor from measured noise rather than defaulting silently.

- [ ] **Step 1: Write the failing brightness-invariance test**

This is the Lupton property stated as a test: scaling every emission line by a common factor must not change hue.

```cpp
TEST_CASE("emission hue is a line ratio, independent of brightness", "[color_composer]") {
    ColorComposer faint, bright;
    DerivedSlots s_faint; s_faint.Ha = 0.02;  s_faint.OIII = 0.01;
    DerivedSlots s_bright; s_bright.Ha = 0.60; s_bright.OIII = 0.30;

    faint.compose_pixel(s_faint);
    bright.compose_pixel(s_bright);

    // Same 2:1 Ha:OIII ratio -> identical chrominance, whatever the brightness.
    REQUIRE(faint.last_pixel_emission_a()
            == Catch::Approx(bright.last_pixel_emission_a()).margin(1e-9));
    REQUIRE(faint.last_pixel_emission_b()
            == Catch::Approx(bright.last_pixel_emission_b()).margin(1e-9));
}

TEST_CASE("OIII-dominant pixel renders teal, not blue-black", "[color_composer]") {
    ColorComposer c;
    DerivedSlots s; s.Ha = 0.01; s.OIII = 0.09; s.L = 0.35;
    sRGBPixel out = c.compose_pixel(s);
    // Teal = green present and comparable to blue. The pre-fix code gave g == 0.
    REQUIRE(out.g > 0.02);
    REQUIRE(out.b > out.r);
}
```

- [ ] **Step 2: Run and watch them fail**

Run: `cd build && make test_color_composer -j$(nproc) && ./test/test_color_composer`
Expected: FAIL. The first because `emission_a` scales with brightness (faint ~1, bright ~30); the second because green is identically zero.

- [ ] **Step 3: Normalise the emission chrominance**

In `src/lib/compose/src/color_composer.cpp`, replace the emission block:

```cpp
    double emission_a = 0.0, emission_b = 0.0;
    double chroma_scale = 0.0;
    if (total_w > 0.0) {
        // Lupton et al. 2004 PASP 116,133: hue must be invariant under a
        // common scaling of the inputs. Dividing by total_w makes the Lab
        // chrominance a pure ratio of the emission lines; brightness is
        // carried by L alone, exactly as STScI colorizes each narrowband
        // layer at a fixed hue and saturation (Rector et al. 2004 AJ).
        const double inv_w = 1.0 / total_w;
        emission_a = (w_ha * pal_ha.a + w_oiii * pal_oiii.a + w_sii * pal_sii.a) * inv_w;
        emission_b = (w_ha * pal_ha.b + w_oiii * pal_oiii.b + w_sii * pal_sii.b) * inv_w;

        // Rector et al. 2004: faint pixels must not be coloured as boldly as
        // real signal. This is now an EXPLICIT gate rather than the accident
        // of an absent division: below the floor chroma ramps to zero.
        chroma_scale = (snr_floor_ > 0.0)
            ? std::min(1.0, total_w / snr_floor_)
            : 1.0;
        emission_a *= chroma_scale;
        emission_b *= chroma_scale;
    }
    last_chroma_scale_ = chroma_scale;
```

- [ ] **Step 4: Declare the gate in the header**

In `color_composer.hpp`, add to the public section:

```cpp
    // Chroma gate: pixels whose total emission weight is below this floor
    // have their chrominance ramped down linearly to zero, so noise is not
    // rendered as saturated colour. 0 disables the gate.
    void set_chroma_gate(double snr_floor)       { snr_floor_ = snr_floor; }
    double last_pixel_chroma_scale() const       { return last_chroma_scale_; }
```

and to the private section:

```cpp
    double snr_floor_        = 0.0;
    double last_chroma_scale_ = 0.0;
```

Add `#include <algorithm>` to the .cpp if not already present.

- [ ] **Step 5: Run and watch them pass**

Run: `cd build && make test_color_composer -j$(nproc) && ./test/test_color_composer`
Expected: PASS.

- [ ] **Step 6: Write the failing hue-preserving gamut test**

Per-channel clamping changes hue, which is the second half of the same defect.

```cpp
TEST_CASE("out-of-gamut pixels keep their hue", "[color_composer]") {
    ColorComposer c;
    double r = 1.6, g = 0.8, b = 0.4;
    REQUIRE(c.clip_to_gamut_for_test(r, g, b));
    // Lupton: divide all three by max, do not clamp independently.
    REQUIRE(r == Catch::Approx(1.0));
    REQUIRE(g == Catch::Approx(0.5));
    REQUIRE(b == Catch::Approx(0.25));
}
```

- [ ] **Step 7: Make the gamut clip hue-preserving**

Replace `ColorComposer::clip_to_gamut`:

```cpp
bool ColorComposer::clip_to_gamut(double& r, double& g, double& b) {
    bool clipped = false;
    // Negative energy is not a hue, it is an absence: floor it first.
    if (r < 0.0) { r = 0.0; clipped = true; }
    if (g < 0.0) { g = 0.0; clipped = true; }
    if (b < 0.0) { b = 0.0; clipped = true; }

    // Lupton et al. 2004: rescale all three by the common maximum so the
    // intensity is clipped at unity while the colour stays correct.
    const double m = std::max(r, std::max(g, b));
    if (m > 1.0) {
        const double inv = 1.0 / m;
        r *= inv; g *= inv; b *= inv;
        clipped = true;
    }
    return clipped;
}
```

Expose it for the test by adding to the public section of the header:

```cpp
    // Test seam: gamut handling is a pure function of its arguments.
    bool clip_to_gamut_for_test(double& r, double& g, double& b) {
        return clip_to_gamut(r, g, b);
    }
```

- [ ] **Step 8: Wire the gate's floor from measured noise, not a default**

`src/module/NukeXInstance.cpp` must call `set_chroma_gate()` with a floor derived from the composed stack's background level, so the gate is driven by the data. A hardcoded default here would be exactly the silent fallback the project rules forbid.

- [ ] **Step 9: Run the whole suite**

Run: `cd build && ctest --output-on-failure`
Expected: 73/73 pass. If `test_color_composer`'s existing single-line cases fail, read them before changing them: cases asserting "pure OIII drives cyan-blue" must still pass, because a single-line pixel normalises to exactly its palette entry.

- [ ] **Step 10: Commit**

```bash
git add src/lib/compose src/module/NukeXInstance.cpp test/unit/compose
git commit -m "$(cat <<'EOF'
fix(compose): make emission hue a line ratio, not a function of brightness

ColorComposer computed total_w and used it only as a guard, so Lab
chrominance carried absolute signal. On the M16 HaO3 corpus that left 10
non-zero green pixels of 24.5M, a magenta background, OIII rendering blue
instead of teal, and 24,516,516 of 24,543,024 pixels reported gamut-clipped
against 126 on the plain-OSC corpus of the same camera and size.

Lupton et al. 2004 (PASP 116,133) states the rule this violates: for any
non-linear mapping an object's colour must not depend on its brightness.
Chrominance is now divided by total_w, so hue is a pure ratio of the
emission lines and brightness is carried by L alone -- matching STScI
practice, where each narrowband layer is colorized at a fixed hue and
saturation (Rector et al. 2004, AJ).

Two consequences, both deliberate:

- The "faint pixels stay neutral" behaviour that the missing division
  provided by accident is now an EXPLICIT chroma gate (set_chroma_gate),
  off by default. Rector et al. balance layers so one line cannot dominate;
  an accident of scaling is not that balance.
- Gamut handling no longer clamps channels independently, which changes
  hue. All three are rescaled by their common maximum, per Lupton: the
  intensity is clipped at unity but the colour is correct.

Moves the composed output on every narrowband corpus. Goldens are
re-baselined once, in the program's final task, not here.
EOF
)"
```

---

## Task 2: Per-channel CoverageMask — the stacker cannot tell absent data from black

**The defect, already root-caused and measured:** `warp` leaves 0 outside the source's coverage, and 0 is a legal pixel value, so the four accumulation loops — which call `route_sample_idx`, which calls `welford.update()` and `histogram.update()` unconditionally — average absent samples in as though they were measured darkness.

Measured on the operator's shipped M3 stack (6072x4042, 53 frames): **268,802 exactly-zero pixels**; the ring at depth 1 sits at **48.6% of interior brightness**; contamination reaches **48 px deep** all round, matching the session's dither and drift excursion. On the frozen mono golden: 31,158 exactly-zero pixels with the first 8 px at 95% of interior brightness. The visible black edge is the least of it — depths 3 to 48 are merely too dark and look plausible.

**Design, already ruled:** an explicit **per-channel `CoverageMask`**, NOT a NaN or negative sentinel. The consumers were traced first: the aligned image goes to a disk cache via `write_frame` and to `compute_frame_median`, so a sentinel would leak into the cache and into Phase B. Per-channel rather than per-frame is required because channel registration can push one plane out of the source while the others stay in.

**Files:**
- Create: `src/lib/alignment/include/nukex/alignment/coverage_mask.hpp`
- Create: `src/lib/alignment/src/coverage_mask.cpp`
- Modify: `src/lib/alignment/src/homography.cpp` (warp writes coverage)
- Modify: `src/lib/alignment/include/nukex/alignment/homography.hpp`
- Modify: `src/lib/stacker/src/stacking_engine.cpp` (accumulation loops consult coverage)
- Modify: `src/lib/alignment/CMakeLists.txt`
- Create: `test/unit/alignment/test_coverage_mask.cpp`
- Modify: `test/CMakeLists.txt`

- [ ] **Step 1: Read the four accumulation loops and `route_sample_idx` before writing anything**

Run: `grep -n "route_sample_idx" src/lib/stacker/src/stacking_engine.cpp`
Record each call site and what it indexes. The mask must be consulted at the same granularity the sample is routed at, or the guard will be applied to the wrong channel.

- [ ] **Step 2: Write the failing test — an uncovered sample must not enter the statistics**

```cpp
TEST_CASE("a sample outside source coverage is not accumulated", "[coverage]") {
    // Two frames, one pixel. Frame 0 covers it with value 0.8;
    // frame 1's warp places it outside the source, so it is absent.
    CoverageMask mask(1, 1, 1);
    mask.set_covered(0, 0, 0, true);

    CoverageMask absent(1, 1, 1);
    absent.set_covered(0, 0, 0, false);

    Welford w;
    if (mask.covered(0, 0, 0))   w.update(0.8);
    if (absent.covered(0, 0, 0)) w.update(0.0);

    REQUIRE(w.count() == 1);
    REQUIRE(w.mean() == Catch::Approx(0.8));   // NOT 0.4
}
```

- [ ] **Step 3: Run and watch it fail**

Run: `cd build && cmake .. && make test_coverage_mask -j$(nproc) && ./test/test_coverage_mask`
Expected: FAIL to compile — `coverage_mask.hpp` does not exist.

- [ ] **Step 4: Write `CoverageMask`**

One bit per (channel, y, x), stored as `std::vector<std::uint8_t>` with 8 pixels per byte. Constructor `CoverageMask(int width, int height, int n_channels)` zero-initialises to "not covered". Accessors `bool covered(int c, int y, int x) const` and `void set_covered(int c, int y, int x, bool)`. Add `int width()`, `int height()`, `int channels()`, and `std::int64_t covered_count(int c) const` for diagnostics.

- [ ] **Step 5: Make `warp` populate the mask**

Add the overload `warp(..., CoverageMask& coverage)`. Every output pixel the existing bounds check accepts sets covered; every one it rejects leaves it clear. Do not change the bounds arithmetic — `46ad8ac` already corrected it and the frozen golden's 47-pixel drift is attributed to that change.

- [ ] **Step 6: Consult the mask at each accumulation site**

At each of the four sites recorded in Step 1, skip the `welford.update()` / `histogram.update()` when the sample's channel is not covered. A voxel whose every sample is uncovered keeps `count() == 0`; verify what the downstream fitter does with a zero-count voxel and make it explicit rather than letting it divide by zero.

- [ ] **Step 7: Write the integration test on real geometry**

Assert that a synthetic two-frame stack with a known 10 px offset produces an interior mean equal to the source value — not a rim at 50% — and that the uncovered corner has `count() == 0` rather than a fabricated black.

- [ ] **Step 8: Run the whole suite**

Run: `cd build && ctest --output-on-failure`
Expected: all pass. The e2e goldens are NOT regenerated here.

- [ ] **Step 9: Commit**

Message must state that all three goldens will move, with the measured rim figures above as the justification, and that regeneration happens in the program's final task.

---

## Task 3: The six deferred minors

Queued here because they touch `frame_aligner.cpp`, `stacking_engine.cpp` and `homography.*` — the same files Task 2 edits, so parallel branches would collide. None changes pixel output; each is either a test or a clarity fix.

- [ ] **Step 1: Comment the 1e-9 denominator guard's magnitude**

`src/lib/alignment/src/channel_registration.cpp` — state what the guard protects against and why 1e-9 is the right order for these units.

- [ ] **Step 2: Boundary test for the per-star SNR gate**

The existing test erases signal entirely, which is a different condition from present-but-below-threshold. Add a star just below and just above the gate and assert inclusion flips.

- [ ] **Step 3: Test the clip-drops-below-`min_stars_affine` corner**

Construct a match set that passes the star count before clipping and falls below it after, and assert the fallback ladder descends rather than fitting an under-determined affine.

- [ ] **Step 4: Test the MAD-degenerate `sigma == 0` corner**

Identical residuals make MAD zero; assert no division by zero and that the ladder's decision is defined.

- [ ] **Step 5: Test `warp`'s `width() < 2` guard**

A 1-px-wide source cannot be bilinearly interpolated. Assert the guard fires rather than reading out of bounds.

- [ ] **Step 6: Derive the detection channel and the registration reference channel from one expression**

Two separate expressions can disagree silently if a caller ever sets `StarDetector::Config::channel`. Give them one source of truth, as `kNegligibleChannelShiftPx` already has.

- [ ] **Step 7: Remove the dead `if (!desc.empty())` guard**

In `stacking_engine.cpp` it is unreachable given `applied || gave_up`.

- [ ] **Step 8: Run the whole suite and commit**

Run: `cd build && ctest --output-on-failure`. One commit, `test(alignment): close the six deferred minors from per-channel registration`.

---

## Task 4: Cap the GPU batch by host RAM — with an output-invariance gate first

**The defect:** `GPUContext::estimate_batch_size` (`src/lib/gpu/src/gpu_context.cpp:207`) sizes the batch as `device_info_.global_mem_bytes * 85 / 100 / per_voxel` — that is **GPU VRAM**. `ShadowBuffers::allocate` then sizes roughly twenty `std::vector`s to that same batch **in host RAM**, dominated by `pixel_values` and `pixel_weights` at `n_channels * n_frames * batch` floats each. Nothing consults host RAM. On this box: 16303 MiB VRAM against 30 GB system RAM, so for `bayer_rgb_m27_2023` the batch is ~10.4 M voxels, the shadow buffers take ~13.6 GB beside a 14.8 GB cube, and the run swaps at 29 GB resident with 26 GB of swap in use.

- [ ] **Step 1: PROVE whether batch size changes pixel output — before any fix**

This gate comes first because if batch size moves output, every golden moves with it and the change must be attributed in Task 8 rather than discovered there. Run the same small corpus twice with two very different batch sizes and hash the outputs.

Run: `cd build && ctest --output-on-failure -R gpu_agreement` first to confirm the existing agreement harness is green, then force two batch sizes and compare stack hashes.
Expected: identical hashes, because the kernels should be per-voxel independent. **If they differ, stop and report** — that is a correctness defect in the kernels, not a memory-sizing question, and it outranks this task.

- [ ] **Step 2: Write the failing test for the host-RAM cap**

Assert `estimate_batch_size` returns a batch whose implied `ShadowBuffers` footprint plus the already-allocated cube fits within a supplied host-memory budget, for a budget deliberately smaller than VRAM.

- [ ] **Step 3: Read host free memory and cap `available` by it**

Take the minimum of the VRAM-derived figure and a host budget that subtracts the cube's existing allocation and leaves headroom. The batch is a staging window, so a smaller one costs more kernel launches, not correctness.

- [ ] **Step 4: Re-run the GPU agreement suite and the small corpus**

Expected: hashes unchanged from Step 1, resident memory materially lower.

- [ ] **Step 5: Commit**

---

## Task 5: LRGB-mono — per-channel frame sets through Phase B

**Currently refused, not fixed.** A batch with more than one mono filter produced one populated channel of four and three exactly-zero ones; the engine now refuses it loudly. This blocks the first capability the product advertises.

**Why it is not a small fix:** `FrameCache` is keyed on post-debayer geometry (`CacheSig` is `(width, height, n_channels)`), so every mono frame is `(W,H,1)` whatever its filter and L, R, G and B all land in one cache file. `ShadowBuffers`, the OpenCL weight kernels and the CPU fallback all index `frame_stats[fi]` with the same `fi` they use for `pixel_values` — every channel is assumed to share one frame set in one order. There is a second defect in the same area: `FrameCache::write_frame` writes at the **global** frame index while `read_pixel` returns slots `0..n_frames_written-1`, so any batch producing more than one cache reads mostly-unwritten slots.

- [ ] **Step 1: Write the design document**

Create `docs/superpowers/specs/2026-09-05-per-channel-frame-sets-design.md` covering: the cache key change; per-channel `frame_stats` indexing through `ShadowBuffers`; the OpenCL kernel signature change and its CPU-fallback twin; how GPU/CPU agreement is re-proven; and per-frame debayering, which is what allows a mixed Bayer/mono batch and which lands on this same limitation. **Do not start code until this document exists** — it is the artifact a reviewer needs to reject the approach cheaply.

- [ ] **Step 2: Fix `FrameCache::write_frame` index mismatch first, with its own test**

It is independently wrong and independently testable, and it affects the mono-L + Bayer-HaO3 integration case today.

- [ ] **Step 3: Implement per-channel frame sets per the design, one layer at a time, tests first at each layer**

- [ ] **Step 4: Re-prove GPU/CPU agreement**

Run: `cd build && ctest --output-on-failure -R "gpu_agreement|gpu_cpu_fallback"`

- [ ] **Step 5: Un-refuse the batch and un-skip the E2E case**

Remove the refusal in `StackingEngine::execute`; set `mono_lrgb_m27_2025` to `skip: false` in `test/fixtures/e2e_manifest.json`.

- [ ] **Step 6: Fold in the two formal-only voxel notes**

`SubcubeVoxel::channels_()` omits `std::launder` on the pointer it reinterpret_casts out of the record's trailing bytes, and `construct_voxel` placement-news the channels individually rather than as an array. Neither changes behaviour today; both were deliberately deferred to whichever task next touches this code, which is this one.

- [ ] **Step 7: Run the whole suite and commit**

---

## Task 6: Chained or multi-reference alignment

**The measurement:** `mono_lrgb_m27_2025` aligns 51 of 72 after the reference fix. The 21 failures are the two temporal ends of a continuous seven-hour session — chronological frames 0-19 and 66-71, nothing in the middle. Header `RA`/`DEC` drift 0.028 deg, which at 1.221 arcsec/px is **~110 px** of cumulative motion. Ruled out by measurement, not assumption: not a meridian flip (`PIERSIDE` is 0 on every frame), not filter mismatch, not reference choice (the selector already picks the temporal midpoint), not matcher tolerance (sweeping `descriptor_tolerance` 0.003-0.05 and `scale_tolerance_log` 0.02-0.1 changes nothing).

**The mechanism:** far frames are not short of correspondences — at distance 36 they get 55 matches and the homography rejects **every one**. Drift reshuffles which stars land in the top-K, so triangle descriptors increasingly match the wrong stars.

- [ ] **Step 1: Write the design document**

Create `docs/superpowers/specs/2026-09-05-chained-alignment-design.md`. Decide between chaining (align each frame to a temporal neighbour and compose transforms) and multi-reference (carry several references across the session), and state how composed transforms avoid accumulating resampling error — the existing architecture resamples once, and chaining must not become chained resampling.

- [ ] **Step 2: Onward steps follow from the design; do not pre-commit them here**

The plan deliberately stops at the design gate rather than inventing steps for an approach not yet chosen. Writing them now would be the "similar to Task N" placeholder this plan forbids.

- [ ] **Step 3: Ratchet the E2E floor**

`min_frames_ok_alignment` for `mono_lrgb_m27_2025` rises from 51 to whatever the new implementation achieves, so a regression turns the E2E red.

---

## Task 7: Auto-stretch faintness

**Measurement first.** M16 stretched is p50 0.084 / p99.9 0.101; M27 drew "a bit faint, but not too far out" on first real data. This is **not** a v5 regression — the NGC7635 stretched output is bit-identical to v4's, so the stretch path has not moved — and it is distinct from Task 1's chroma question.

- [ ] **Step 1: Measure the current stretch on all three corpora before changing a parameter**

Record p50, p99, p99.9 per channel for each corpus's stretched output. Tuning without this table is guessing.

- [ ] **Step 2: Adjust the auto-stretch target parameters, and re-measure**

The optimised per-filter defaults live in the Phase 8 tuning work; move the target, do not add a new stretch.

- [ ] **Step 3: Commit with the before/after table in the message**

---

## Task 8: The single golden re-baseline, then release

- [ ] **Step 1: Rebuild clean and run the unit suite**

Run: `cd build && cmake .. && make clean && make -j$(nproc) && ctest --output-on-failure`

- [ ] **Step 2: Build the release binary in `build-release/` and install it into `/opt/PixInsight/bin/`, signed**

Never `sudo`. Back up `NukeX-pxm.so`, `NukeX-pxm.xsgn` and `/opt/PixInsight/share/qe_database.json` first.

```bash
/opt/PixInsight/bin/PixInsight.sh --sign-module-file=/opt/PixInsight/bin/NukeX-pxm.so \
  --xssk-file=$HOME/projects/keys/scarter4work_keys.xssk --xssk-password=<see CLAUDE.md>
```

- [ ] **Step 3: Run the full E2E and regenerate every golden ONCE**

- [ ] **Step 4: Attribute every delta**

For each of the four outputs on each corpus, name which task moved it and why. An unexplained delta is a defect, not a re-baseline. The expected attributions: Task 1 moves `composed` on every corpus; Task 2 moves all four on every corpus (rim and interior); Task 5 changes `mono_lrgb_m27_2025` from skipped to populated; Task 6 changes its aligned-frame count; Task 7 moves `stretched`.

- [ ] **Step 5: Re-measure the channel-registration acceptance ratio**

Run: `tools/measure_channel_registration.py` on the M3 set. The bar is red-green / blue-green <= 1.5x; it was 0.93x at v5.0.1.1. Task 2 changes edge pixels, so confirm it did not regress.

- [ ] **Step 6: Bump version and release date, package, sign, commit, tag, push**

Follow the CLAUDE.md release workflow exactly. `tools/release.sh package`.

- [ ] **Step 7: Verify the published artefact**

Confirm the repository URL returns 200 and the downloaded tarball's sha1 matches the signed manifest.

---

## Task 9: Prove the published package installs through PixInsight's updater

**This task needs the operator's hands on the GUI and cannot be done headless.** It is sequenced last because it begins by deleting the dev-staged module every E2E run depends on.

- [ ] **Step 1: Remove the dev-staged copies so the proof is real**

```bash
rm -f /opt/PixInsight/share/qe_database.json
```
and in PixInsight, Process → Modules → Install Modules... → remove the `NukeX-pxm.so` entry. Without this the proof is worthless — the files would already be present whatever the updater does.

- [ ] **Step 2: Install through the updater**

Resources → Updates → Manage Repositories → add `https://raw.githubusercontent.com/scarter4work/nukex5/main/repository/` → Check for Updates → install → restart PixInsight.

- [ ] **Step 3: Verify the layout**

```bash
ls -la /opt/PixInsight/share/qe_database.json /opt/PixInsight/bin/NukeX-pxm.so
```
and in PixInsight, Process Explorer → NukeX → version reads the shipped version.

- [ ] **Step 4: Run one 12-frame M16 HaO3 stack from the interface**

The Process Console must show no QE-database error and the `NukeX_composed` window must appear.

- [ ] **Step 5: If `share/qe_database.json` is absent, ship the JSON under `bin/`**

Change `PIShareRoot()` accordingly and re-package. The plan calls this a release blocker if the layout is not honoured. Report which happened.
