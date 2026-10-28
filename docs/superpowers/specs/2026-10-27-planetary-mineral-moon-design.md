# PlanetaryStack — target detection and the Mineral moon option (design)

Date: 2026-10-27 · Builds on Planetary 0.3.0.1 (main 544c7818). Stacker design:
`docs/superpowers/specs/2026-10-21-planetary-stacker-design.md`. Stacker ledger and rulings:
`.superpowers/sdd/2026-10-21-planetary-stack/progress.md`.

## Intent

After stacking the 2026-10-23 Moon (64.6F) on 0.3.0.0, Scott made a mineral moon by hand. The steps
were ColorCalibration, BackgroundNeutralization, GraXpert, BXT, NXT, three Saturation curves, and
PI Copilot clean-up of clipped highlands (journey 240). The full result is
`~/Desktop/Mineral_Moon_10_26_2026.png` (1630×1080: the 1920-wide stack with the left edge cropped
off). He wants PlanetaryStack to do that work for him: "post processing that is selected by checking
a mineral moon option should do something like this". PlanetaryStack also works out what the
capture is from the file, so the option appears only where it makes sense.

The project is delivered in two parts, in order:

- **Part A, Planetary 0.3.0.2:** the left-edge chroma fix. Mineral moon cannot pass its own bar
  without it, because saturation turns that chroma noise into a colour fringe.
- **Part B, Planetary 0.4.0.0:** target detection and the Mineral moon option.

## Decided by Scott (2026-10-26 / 27)

- With **Mineral moon** checked, the stacker delivers a finished mineral-moon image, not a starting
  point.
- **No BXT, NXT or GraXpert** inside the process: "i'm ok with not calling nxt/bxt during this
  process."
- The process, as approved ("your process sounds good"):
  1. colour calibration,
  2. background,
  3. sharpening,
  4. chroma noise reduction,
  5. gentle, iterative saturation that does not blow out the highlands,
  6. clean-up of highlights that still clip.
- **Narrowband Moon captures get no mineral option.** HaO3 colour comes from the filter, not the
  surface.
- **The Sun is out of scope.** Scott has no solar filter yet.
- **The bar:** on the 64.6F capture, the result is close to his journey-240 image without BXT, with
  no edge fringe and no blown highlands. It does not have to match the BXT result.

## Builder's calls already made (Claude's; Scott may veto)

1. **Own code, no process calls.** Every mineral-moon step is written in the PCL-free stack engine
   (`src/stack/`). None of them calls other PixInsight processes through `ProcessInstance`. This
   keeps the steps unit-testable outside PixInsight, as the engine is today. It also avoids
   process-parameter names that could change with a PixInsight update.
2. **Sharpening is wavelet-based** (à trous / starlet layers, the standard planetary method). It is
   applied to luminance only, so colour noise is not amplified. It is noise-aware: a layer's gain is
   limited by that layer's measured noise.
3. **Target detection from the file name.** ASIAIR names captures as
   `<yyyy-mm-dd>-<hhmmss>-<Target>-Bin<n> <Filter> <temp>F.avi`, for example
   `2026-10-23-203401-Moon-Bin1 Lqef 64.6F.avi`. The `.avi.txt` sidecar and the SER header carry no
   target. `src/ser/AsiairSidecar.h` already parses the start time from this name (`FilenameStart`);
   target and filter parsing go beside it. The interface shows a **Target** dropdown (Moon, Jupiter,
   Saturn, Mars, Venus, Mercury, Uranus, Neptune, Other), preselected from the name, and the user
   can override it. A name that does not parse (a renamed file; PIPP keeps the ASIAIR name, so `_pipp.ser` parses to its real target) selects Other and says
   so on the info line.
4. **When Mineral moon is offered.** The checkbox is enabled only when all three hold:
   - the Target is Moon;
   - the capture is colour (Bayer or RGB, not mono);
   - the filter is not narrowband. Narrowband tokens are HaO3, S2O3, Ha, OIII, SII and other
     dual-band names. Broadband (Lqef, LPro, L, UV/IR) or no filter token counts as allowed.

   When it is disabled, a tooltip says why.
5. **Planets get detection only.** The dropdown records the target in FITS (`OBJECT`) and HISTORY.
   No planet-specific processing in this project.
6. **Two outputs.** Execute opens the plain stack exactly as today (linear, 32-bit float). With
   Mineral moon checked, it also opens `<stack>_mineral`: 32-bit float, display-ready (no STF
   needed), with HISTORY lines naming each step and the values it measured or chose. This fits the
   stacker design's two-output plan for increment 2. The preview shows the mineral image when the
   box is checked.
7. **Scriptable parameters.** Two new parameters, `target` (enum, plus `auto` = resolve from the
   file name; default `auto`) and `mineralMoon` (bool, default false). Headless `ExecuteGlobal`
   honours both. If `mineralMoon` is true for a capture that isn't eligible, the run fails loudly
   with the reason; it does not skip the step silently.
8. **Edge-fix release first.** Part A ships as 0.3.0.2 before Part B starts, so Scott tests the
   edge on its own. The open 0.3.0.1 items fold into Part A's release:
   - the scoped re-review of fix round 5,
   - calibration re-freeze F2 and the bench run,
   - dropping the 0.3.0.0 tarball from `repository/`.

   Each release needs Scott's go-ahead (global rule: PixInsight releases are not under standing
   authorisation).
9. **Version.** Part B is 0.4.0.0: new parameters and a new output. Process ids `PlanetaryStack` and
   `SERViewer` and module id `Planetary` do not change.

## Part A — the left-edge chroma fix (0.3.0.2)

**Defect (Scott found it, Claude validated it, 2026-10-26).** On the 64.6F capture, where the Moon
touches the left frame edge (rows 320–760), the leftmost ~5 columns carry chroma noise:

| Ratio | x = 0 | x ≥ 6 (B/G) / x 20–40 (R/G) |
|---|---|---|
| B/G std | 0.034 | 0.008 |
| R/G std | 0.036 | ~0.013 |

Luma noise is not raised there, and the top and bottom edges, which the Moon also touches, are
clean. Ruling R5 recorded the cause as the edge trade: edge regions rank only 108–230 of 2297
frames. But the concentration in x < 6 is not proven. It could be per-channel coverage at the edge
or Bayer warp near the border.

**Required:**
1. Establish the cause with a measurement before changing code (systematic debugging), and record it
   in the ledger.
2. Fix it at the cause. The planned approach is the edge coverage-quantile design (review Q1, R4,
   R5). If the measured cause is the Bayer warp instead, fix that.
3. **Acceptance:** on 64.6F, chroma std (B/G and R/G) in x 0–5 is within 1.5× of x 20–40, over rows
   320–760. *Retired 2026-10-27 (Ruling R12).* The 64.6F edge colour is real lunar colour. In the
   70.0F capture of the same night, those locations sit 27–47 px inside the frame, with every frame
   covering them. There the colour correlates 0.98–0.99 with matching amplitude. A raw colour ratio
   on real data compares terrains, not edge against interior. Part A's acceptance is now:
   - a still-capture test: the demosaic border error at column 0 (2.33× on 0.3.0.1, fixed);
   - a drift no-regression test, at ≤ 1.5×;
   - on real data, split-half chroma *noise* at the edge ≤ 1.5× the interior.

   The fix is a 4 px frame guard band, plus a crop of only the pixels no frame reaches. No regression elsewhere: the existing ctest, probes, bench and the D1/D2/clip tests stay
   green, and the bench verdicts are no worse than 4 pass / 4 review / 0 fail.
4. A RED test that fails on 0.3.0.1 first, then goes green.

## Part B — target detection and Mineral moon (0.4.0.0)

### The mineral-moon steps (required behaviour; algorithms are the builder's)

The input is the finished linear stack, 32-bit float in [0, 1]. Each step has a requirement that is
checked by measurement.

1. **Lunar disk and sky.** Find the lit disk (terminator side included) and the sky. Handle a disk
   clipped by the frame on any side; 64.6F is cut off top and bottom. The engine's target
   measurement is the place to start.
2. **Colour calibration.** The white reference is the whole lit disk, excluding saturated pixels.
   The background reference is the sky. After calibration, the disk's mean R, G and B are equal
   within 1 %. If the frame has too little sky to measure (fewer than about 1 % of pixels), the
   background is taken from the darkest disk-free border pixels, or is skipped with a HISTORY line
   saying so. It is never invented.
3. **Background.** Neutralise the sky pedestal per channel and remove any linear gradient measured
   on sky pixels only. Afterwards, sky mean per channel ≤ 0.5 % of disk mean and equal across
   channels within noise.
4. **Sharpen** (luminance, wavelet, noise-aware). Detail rises measurably, by the stacker
   design's documented sharpness metric. The sky's noise std rises by no more than 1.5×. There is
   no ringing halo at the limb: the profile across the limb overshoots the sky level by no more
   than 2 % of the disk mean.
5. **Chroma noise reduction**, before saturation. Chroma (colour-difference) std in the sky and in
   flat mare regions falls by at least half. Luminance detail is unchanged; the sharpness metric
   moves by no more than 2 %.
6. **Saturation, iterative and gentle.** The boost is applied in chroma, in several small passes,
   with a soft roll-off so no channel exceeds 1.0. The target strength is chosen from the image's
   own chroma statistics, not from fixed curve points. Scott's three S-curves are the reference for
   how strong "done" looks. Afterwards, the maria read blue and the highlands read orange/tan, as
   in his PNG.
7. **Highlight clean-up.** Bright highland pixels that saturation pushed out of gamut (Scott's
   journey: cyan highlands, mask max > 0.55) are pulled back toward neutral. Afterwards, no samples
   are above 1.0, and no saturated cyan cast remains in the brightest 1 % of disk pixels.
8. **Display-ready output.** The mineral image opens looking right without an STF: the disk's
   median is at a normal display level and nothing is clipped that was not clipped in the stack.

**Timing:** the mineral steps add ≤ 5 s to a 1920×1080 stack on 32 threads (CPU only, no OpenMP;
use the engine's `Parallel.h`).

### Acceptance (the bar)

On `2026-10-23/Moon/2026-10-23-203401-Moon-Bin1 Lqef 64.6F.avi`:

- every numbered requirement above, measured and reported by a probe;
- no edge fringe: the B5/B7 synthetic edge-band tests, with truth known, pass. On real data, the
  mineral output's split-half chroma noise at each edge is ≤ 1.5× its interior. Real lunar colour at
  an edge is not a fringe (Ruling R12);
- **Scott's judgement:** a side-by-side of our mineral image next to
  `~/Desktop/MineralMoon.png` (2026-10-27, Scott's ratio-algorithm result; supersedes the 10-26 PNG), cropped to the same field. It should be close, with no
  fringe and no blown highlands. Less sharp than BXT is acceptable.

Also run on the other broadband Moon captures:
- `2026-10-23/Moon/...70.0F.avi`
- `2026-05-28/Moon/` ×2
- `2_1_2024/...Lqef 14.9F.avi` and `...14.0F.avi`
- `2_20_2024/...Lqef 16.0F.avi`
- `8_31_2023/...LPro 14.0F.avi`

On all of them, no step may fail or produce a broken image (out-of-range samples, colour cast,
halo). Side-by-sides go to Scott.

Gating tests, on real file names:
- `2024-02-01-013058-Moon-Bin1 HaO3 16.2F.avi` → Mineral moon disabled (narrowband).
- `2026-02-10-...-Betelgeuse-...avi` → Target Other.
- `...Saturn...`, `...Jupiter...`, `...Neptune...` → the right planet, option disabled.
- `moon_stack_final.ser` (renamed) → Other, with the info-line note; `..._pipp.ser` keeps the ASIAIR name and parses to its real target.

### Testing and proof

1. **Unit (ctest, synthetic, also in the Rocky 9 container):**
   - file-name parsing: every NAS name pattern above, plus garbage names;
   - the gating rule;
   - each mineral step against its measured requirement on synthetic discs (known colour cast,
     gradient, noise, clipped highlights, disk cut by the frame edge, no-sky frame).
2. **Headless probe (slot 96):** `ExecuteGlobal` with `mineralMoon` true and false, and with an
   ineligible target, which must fail loudly. Check windows, keywords and HISTORY.
3. **GUI smoke (slot 97):** Open a Moon file. The dropdown shows Moon and the checkbox is enabled.
   Then Stack!, preview shows mineral, Execute opens two windows. Then open an HaO3 Moon: the
   checkbox is disabled, with its tooltip.
4. **Real data** (host-only, NAS read-only): the acceptance runs above, with numbers and PNGs.
5. **Portability:** container build, `verify-portable.sh`, ASCII-only PCL string literals.
6. **Docs:** update `doc/tools/PlanetaryStack/` for Target and Mineral moon.

## Non-goals

- Sun processing, and anything for the Sun beyond the Target dropdown (no solar filter yet).
- Colour or other processing for planets.
- Local-contrast enhancement (HDRMT/LHE-style). It was in the generic guide Scott pasted but not in
  his approved process.
- The increment-2 sliders. The sharpen and chroma-denoise code should be written so the sliders can
  reuse it later.
- Derotation, drizzle, Moon mosaics.

## Anti-goals

- Calling BXT, NXT, GraXpert or any third-party process, or any external program or network.
- A mineral image that hides a defect: a step that cannot run reports why in HISTORY and in the
  result line. It never guesses.
- Mineral colour on narrowband or mono data.
- Samples above 1.0, or new clipping, in either output.

## Rulings after the builder check (Claude's calls, 2026-10-27; Scott may veto)

1. **Part A follows the measurement, not R5.** The 0.3.0.2 work starts by finding the cause of the
   x < 6 chroma noise. The defect is chroma-only and confined to the first 5–6 columns, which points
   to a demosaic or warp border effect rather than frame coverage, but this is unproven.
   - **If the cause is per-channel coverage at the edge:** the edge coverage-quantile design (R5,
     review Q1) ships in 0.3.0.2 and is held to both bars: R5's edge-band noise within 1.3× of the
     interior (split-half, strip up to ~64 px), and this spec's x 0–5 chroma ratio within 1.5×.
   - **Otherwise:** the fix is made at the measured cause (for example a demosaic or warp border
     fix). It is held to the x 0–5 chroma bar. The quantile design is then re-judged on its own
     evidence, as a separate ledger ruling, and does not ride along.
2. **The sharpening and denoise metrics.**
   - **Real data:** the bar uses `test/bench/metric.py`'s DAN_interior, computed on the linear
     luminance before the display transfer. END is not used here, because any linear gain raises
     END, so it cannot fail.
   - **C++ unit tests:** these use ground truth instead of a sharpness score. A synthetic sharp
     lunar-like scene is blurred with a known PSF and noise is added. Sharpening must reduce RMS
     error against the sharp truth, so a gain that only adds noise fails. Chroma denoise must cut
     chroma error against the truth by ≥ 50 % while luminance RMS error moves by ≤ 2 %.
   - Requirements B4 and B5 read "sharpness metric" in that sense.
3. **The gate uses the resolved Bayer pattern.** Three acceptance captures have no sidecar and
   resolve to mono under Auto:
   - `2_1_2024 …14.0F`
   - `2_20_2024 …16.0F`
   - `8_31_2023 …LPro`

   Their runs set RGGB explicitly; Scott's colour cameras are RGGB. With Auto they stay mono and
   the option stays disabled. That is correct behaviour and is tested as such.
4. **Reference values.** Scott's exact Saturation curve points and his highlight PixelMath are in
   journey 240 in `~/.local/share/PICopilot/journeys/journeys.sqlite3`. They are the reference for
   how strong "done" looks (B6) and for the clean-up (B7). They are not hard-coded.

## Left to the builder

- Every algorithm:
  - disk and sky segmentation,
  - calibration statistics,
  - gradient model,
  - wavelet layers and gains,
  - chroma space and denoise method,
  - the saturation schedule and roll-off,
  - highlight clean-up,
  - the display transfer.
- The narrowband token list beyond the names above.
- The interface layout of the dropdown and checkbox.
- How the preview switches.

All of these must meet the measured requirements.

## Amendment 2026-10-27: the mineral-moon algorithm (Scott's pasted spec)

Scott supplied an algorithm spec for steps 5–7. It is adopted. Where it conflicts with the sections above, this amendment wins.

**Inputs.** The calibrated, background-neutralised, display-transferred RGB, plus a sharp, denoised luminance L. L comes from step 4's sharpening of the stack's own luminance (the mean of the channels).

**Steps 5–7 become one ratio-based colour transfer:**
1. `C` = each RGB channel Gaussian-blurred by `chroma_blur_sigma` (default 2.5 px at 1–2k width, scaled with resolution). This *is* the chroma noise reduction of step 5.
2. `d = clip(C / (mean(C) + eps) − 1, ±dev_clamp)`, with `eps` = 0.002 and `dev_clamp` = 0.06.
3. Highlight fade: `w = clip((sat_threshold − max(C_R, C_G)) / sat_ramp, 0, 1)`.
   - `sat_threshold` is set automatically: 0.92 × the lowest per-channel 99.99th percentile of the RGB.
   - `sat_ramp` = 0.12.

   This replaces step 7's clean-up.
4. `out = clip(L · (1 + k · w · d), 0, 1)`, with gain `k` default 8.

**Why ratios.** Hue is preserved, all detail comes from the sharp L, and colour can be lower resolution than luminance (as in LRGB). HSV or Lab saturation pushed this far shifts hues and amplifies noise.

**Changes to the decisions above:**
- **Strength is a user control, not solved from the image.** The process parameter is `mineralStrength` (k), with a slider from 0 to 15 and a default of 8. `chroma_blur_sigma` is also exposed. `eps`, `dev_clamp` and `sat_ramp` stay internal, and `sat_threshold` is automatic.
- **New optional step 4b: align R and B to G.** This corrects atmospheric dispersion. The step runs only when the measured inter-channel shift exceeds 0.1 px, and HISTORY reports the shift. On 64.6F the measured shift is ≤ 0.02 px, so the step is a no-op there. Dispersion left uncorrected shows up as a blue or red limb fringe once the colour is amplified.
- **Limb fade: Claude's call, Scott may veto.** `w` → 0 within a few px of the limb, applied only if the real-data runs show a limb fringe after step 4b.
- **Optional final mild S-curve on L.** It is not added in this increment; the bar is judged without it. No second saturation step.
