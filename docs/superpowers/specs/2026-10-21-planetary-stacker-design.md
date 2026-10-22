# Planetary module — increment 3: PlanetaryStack, the stack engine (design)

Date: 2026-10-21 · Builds on Planetary 0.2.1.0 (SERViewer: SER + 8-bit AVI input, ASIAIR sidecar Bayer,
Save as SER, Save pattern...).

## Intent

Scott stacks planetary and lunar video today with ASIStudio ("easier, but hit and miss") and
AutoStakkert ("the UI sucks"). He wants one simple PixInsight window where the program does the work:
"the stupid program can find the best images, stack them, align them... all that. No need for me to
align craters on the moon." Flow, in his words: load the video, **Stack!** (with a progress bar) →
preview with zoom controls → (later) sliders → **Execute** generates the image.

The stacker is delivered in four increments, each usable on its own (agreed with Scott 2026-10-21):
1. **This document: the stack engine and window** — open, Stack!, automatic frame and region
   selection, automatic global + local alignment, stacking, zoomable preview, Execute, and a plain
   diagnosis when a capture has nothing usable.
2. Live sliders on the stacked image (Luminar-Neo-style: fine sharpening, medium sharpening, denoise,
   contrast, R, G, B — quick, obvious, instant).
3. Derotation, applied automatically only when the capture is long enough to need it.
4. PI Copilot diagnosis of failed captures (what to change next session). Never AI-generated image
   content: an image "salvaged" by a model would be invented detail.

## Decided by Scott

- No "keep N %" control. "Software can determine how much to keep. Run stats on the frame (all of
  them), then determine if that is a good candidate or not. We do that with NukeX." Selection is
  automatic from the statistics.
- No manual alignment points, ever.
- A progress bar while stacking.
- If the video has no usable frames: no preview — a diagnosis instead.
- Derotation only if it makes sense (increment 3 of the stacker, not here).
- Sliders work only on the stacked image (increment 2).
- Moon mosaics and the Sun are out of scope.
- Yardstick: his own AutoStakkert and ASIStudio results of the same captures (below).

## Builder's calls already made (Scott may veto)

- New process **`PlanetaryStack`** in the Planetary module (SERViewer stays the viewer). The process
  id becomes permanent at its first release.
- Input is read directly through the existing `FrameSource` (`OpenVideo`): AVI or SER, no conversion
  step. Bayer resolution exactly as SERViewer (sidecar, Flip rule, Bayer combo, Save pattern...).
- **Execute** opens the stacked image as a PixInsight image window (in this increment: the stack
  itself, linear, 32-bit float). A second button **Open stack** is reserved for increment 2, when
  Execute starts applying the sliders; in this increment there is one output.
- Timing target: a 4000-frame 1920×1080 8-bit capture stacks in **≤ 60 s** on Scott's machine
  (32 threads, RTX 5070 Ti) when the file is on local disk. From the NAS the floor is the network
  (≈ 8 GB per full read on 1 GbE ≈ 70 s); the result log reports read throughput so a slow run can be
  attributed. A run that misses the budget on local disk is a bug.
- Memory: never hold the whole video; peak resident memory ≤ 4 GB for the 4000×1080p case.

## What the user sees

Process ▸ Planetary ▸ **PlanetaryStack**:
1. **Open…** (same dialog and filters as SERViewer) → info line (frames, size, colour/Bayer + source,
   warnings) and the Bayer pattern combo / Save pattern... exactly as in SERViewer.
2. **Stack!** → a progress bar with the current phase ("Measuring frames 1234/4088", "Aligning",
   "Stacking"), and Cancel. The interface stays responsive; cancelling leaves nothing behind.
3. On success: a **preview** of the stack with zoom controls (Fit, 100 %, 200 %, zoom in/out, pan by
   drag) and a short result line: frames measured, frames used (and how many regions were lucky in
   how many frames, in plain words), alignment quality, time taken.
4. **Execute** → the stacked image opens as a normal PixInsight window with FITS keywords (source
   file, frames used, BAYERPAT handled as debayered RGB, INSTRUME from the sidecar, HISTORY lines with
   the selection/alignment summary). Never `DATE-OBS` from an AVI (no trustworthy clock).
5. On failure (no usable frames): no preview; the result area states the diagnosis in plain words
   and the numbers behind it (see Diagnosis).

Process parameters (scriptable, for headless tests): `filePath`, `bayerPattern` (same enum as
SERViewer). `ExecuteGlobal()` runs the whole stack and opens the stack image, or fails loudly with
the diagnosis.

## The engine — required behaviour

The builder designs the algorithms; these are the requirements they must meet, measured.

1. **Measure every frame** (progress bar phase 1). Per frame: a sharpness/quality statistic robust
   to brightness and noise, target presence and position, brightness, clipped fraction. Statistics
   are recorded for the diagnosis and the HISTORY summary.
2. **Select automatically** from the distribution of the statistic — no fixed percentage. The rule
   must adapt: a steady night keeps many frames, a poor night few. It must be explainable in one
   result line ("used 612 of 4088 frames: the sharpest group, quality ≥ 0.71 of the best").
3. **Lucky imaging per region**: the image is split into regions automatically placed on structure
   (no user alignment points); each region is ranked and stacked from its own best frames, and the
   regions are blended seamlessly. Must work for a planet disc (target smaller than the frame,
   drifting) and for a lunar surface (structure filling the frame).
4. **Align automatically**: global (follow the target as it drifts) and local (per-region shifts
   from seeing), with sub-pixel accuracy. Frames that cannot be aligned are excluded, not smeared in.
5. **Stack** with outlier rejection (satellites, hot pixels, a bird); Bayer input is demosaiced with a
   method that does not leave zipper or colour-fringe artifacts at the planet's limb (bilinear from
   SERViewer is not good enough for a final image).
6. **Diagnosis** when nothing is usable, each with its own message and numbers:
   - no target in the frames (e.g. the 2026-01-30 Saturn AVI — its frames hold only background,
     measured 2026-10-21);
   - out of focus (no frame reaches a minimum detail level);
   - seeing too poor (the best frames are still below the usable threshold);
   - clipped (the target is saturated in most frames);
   - target leaves the frame (drift) in too many frames.

## Yardstick (the definition of "good enough")

Scott's NAS already holds competing results of the same captures (surveyed 2026-10-21):
AutoStakkert (`AS_P20/25/50/60` folders, with `.as3` settings files) in `2_12_2023`, `2_1_2024`,
`2_20_2024`, `5_27_2023`, `8_23_2023`, `8_16/Video`, `3_6_2023/moon`, `2_2_23/Moon`; ASIStudio
(`ASIVideoStack_Output`) in `2_1_2024`, `8_23_2023`, `9_30_2023`, `9_16_2023`, `4_7_2023`,
`3_31_2023/Moon`, `9_22/Video` and `processing-archive-2026-10-03/videos` (2026-01-30 night).

- **Benchmark set:** the captures that have an AS and/or ASI result, Moon and planets, chosen by the
  builder to cover planet disc and lunar surface (at least 3 of each kind if available), listed in
  the plan with their paths.
- **Objective measure:** one documented sharpness-at-equal-noise metric, computed identically on
  our stack, the AS result and the ASI result (same crop/registration of the target, same
  normalisation); the builder validates the metric first (e.g. it ranks a deliberately blurred copy
  below the original and AS_P20 vs AS_P50 the way AS's own selection implies).
- **Pass:** on every benchmark capture, our stack's measure ≥ the better of AS and ASI, and no
  visible failure mode (doubled limb, smeared region, seams between regions, misalignment, colour
  fringing). Scott judges side-by-side images (ours / AS / ASI) produced by the test harness.
- **Physical sanity:** Jupiter shows its two equatorial belts; Saturn its rings (Cassini division
  when the seeing allows); the Moon's craters are sharp edge to edge, with no seams.
- **Timing:** the 60 s local-disk target above, reported per benchmark capture.

## Testing and proof

1. Unit (ctest, synthetic, also in the Rocky 9 container): quality statistic monotone in blur;
   selection adapts to distributions (steady vs poor); global + local alignment recover known
   sub-pixel shifts on synthetic frames; per-region stacking with seamless blending (no seam in a
   flat field); outlier rejection; each diagnosis case from synthetic captures; cancel leaves nothing.
2. Headless PixInsight probe (slot 96): `ExecuteGlobal` on synthetic captures (success, each
   diagnosis) — window, keywords, failure text.
3. GUI smoke (slot 97, existing driver rules): Open → Stack! (progress bar visible mid-run, screenshot)
   → preview → zoom 100 %/200 % → Execute → window; a diagnosis case shows no preview.
4. Real data (host-only, NAS read-only): the benchmark set — metric table (ours / AS / ASI), timings,
   side-by-side PNGs for Scott; plus the diagnosis on the empty 2026-01-30 Saturn AVI.
5. Portability: container build + `verify-portable.sh`; no OpenMP; no GPU code in this increment (Ruling 3).

## Non-goals (this increment)

Sliders and any post-processing (increment 2), derotation (increment 3), Copilot integration
(increment 4), Moon mosaics, the Sun, drizzle, manual alignment points, a percentage control.

## Anti-goals

- Any user-placed alignment point or percentage knob.
- Producing an image from a capture with no usable frames (no "best effort" mush, no AI-made image).
- External programs or network access at run time (CPD rules; the module promises no network).
- Silent failure: every exclusion and every diagnosis is reported with its numbers.

## Rulings after the builder check (made while Scott was away, 2026-10-21; he may overrule)

1. **Drizzled AutoStakkert references.** Most AS results on the NAS are drizzled (1.5× or 3×); only
   the `2_12_2023` Jupiter and Moon AS_P20 are not. Drizzle stays a non-goal. The harness
   downsamples a drizzled reference to native scale (area-averaging, documented) before the metric
   is computed; no AutoStakkert re-run. Reported per capture as "AS (drizzled ×N, resampled)".
2. **The pass bar has a tolerance and a human.** Per benchmark capture: ours ≥ 0.97 × the better of
   AS/ASI on the metric (3 % covers metric noise and settings luck), and none of the visible failure
   modes. A capture that misses gets one investigation round with its numbers recorded; the final
   word is Scott's side-by-side review. Not an open-ended gate.
3. **Timing is CPU-only.** The 60 s local-disk budget applies to the CPU path on 32 threads. GPU is
   out of scope for this increment (no CUDA/OpenCL code). A measured miss is reported with the phase
   breakdown; it is a finding, not a blocker for Scott's review.
4. **No release in this increment.** It ends on branch `feat/planetary-stack`, release-ready (tests,
   container build, portability, docs), not merged: the first release makes `PlanetaryStack`
   permanent and needs Scott's go-ahead after his side-by-side review.
5. `3_31_2023/Moon` has no usable reference (empty ASI folder; Polaris only) — dropped from the list.

## Left to the builder

The quality statistic, the selection rule (as long as it is automatic, adaptive and explainable),
region placement and blending, alignment method, demosaic method, rejection method, threading,
the interface layout, and the metric — all within the requirements and measured
against the yardstick.

## Timing, revised by Scott (2026-10-21)

"AutoStakkert takes a long time to stack bigger video files... we might want to consider that going
forward." The hard 60 s target above is replaced: **quality comes first; a stack should take no longer
than AutoStakkert needs for the same file on the same machine (order of magnitude), with a progress bar
throughout.** Speed work is welcome only when it costs no detail. The benchmark reports our time per
capture next to the capture's size; a capture far slower than that bar is a finding, not a blocker.
