# PlanetaryStack — planet processing with live sliders (design)

Date: 2026-10-29 · Builds on Planetary 0.4.0.1 (main 80e62fd0). Mineral-moon design:
`docs/superpowers/specs/2026-10-27-planetary-mineral-moon-design.md` (its steps, rulings and code are
reused here). Stacker ledger: `.superpowers/sdd/2026-10-21-planetary-stack/progress.md`; mineral
ledger: `.superpowers/sdd/2026-10-27-planetary-mineral-moon/`.

## Intent

Scott tried the 0.4.0.1 mineral moon sliders on the 2026-10-23 64.6F stack. He ran it without BXT and
judged it "works as intended" (`~/Desktop/Mineral_Moon2.png`). He then asked for the same kind of
sliders on planet stacks: "lets go ahead and make that work for both mineral moon and planetary then
(sliders)".

Today a planet stack comes out of PlanetaryStack as the linear stack only. It has no sharpening, no
colour work and no dispersion correction. The mineral moon's engine already has the pieces a planet
needs. This project gives planets their own post-processing option, built on those pieces, with
live sliders.

## Decided by Scott (2026-10-29)

Scott approved this plan in chat: "lets go ahead and make that work".
- **Sharpen** and **Chroma blur** sliders for planets, on the same engine as the mineral moon.
  Their planet defaults are measured on real planet data, not copied from the Moon.
- A **Saturation** slider in place of Strength, defaulting to 1× (natural colour). The Moon's ×8
  mineral exaggeration does not apply to planets.
- **R/B channel alignment** (atmospheric dispersion) runs automatically for planets. It has no
  slider.
- **The mineral moon stays as it is.** Its parameters, defaults and output do not change.
- **The Sun stays out of scope.** He has no solar filter (the mineral design's non-goal stands).

## Builder's calls already made (Claude's; Scott may veto)

1. **Which captures are eligible.** The option is offered when all of the following hold:
   - the Target is Mercury, Venus, Mars, Jupiter, Saturn, Uranus or Neptune;
   - the capture is colour;
   - the filter is broadband.

   The colour and filter tests are the ones the mineral moon uses (`ser::MineralMoonRefusal`), so
   IR-pass, methane and narrowband captures are refused, as are mono ones. A tooltip says why,
   exactly as for the mineral moon. Target **Other** is refused, because the colour reference
   (call 3) needs a known body; the tooltip tells the user to pick the target in the dropdown.

   Mono and IR-pass planet captures do not get a Sharpen-only path in this project (non-goal).

2. **One pipeline beside the mineral moon, not a copy of it.** The planet path reuses these
   engine steps unchanged:
   - `SegmentLunar`, the disc/sky segmentation, which is not Moon-specific;
   - `NeutraliseBackground`;
   - `AlignChannels`;
   - `StarletLayers` and `SharpenLuminanceImage`, with the noise map, `LayerNoiseShape`, and the
     limb fade;
   - `ChromaLayer`;
   - the prepare/finish split and its cache, so the sliders re-run only the cheap part.

   Only these things differ: the colour calibration target (call 3), the display transfer (call 4),
   the sharpening's layer gains (call 5) and the colour transfer's settings (call 6). Shared code is
   parameterised by a target kind. It is not forked. Rename the Moon-specific names (`LunarMasks`,
   `MineralCache`, …) only where the planet path makes the old name wrong. Keep each rename
   mechanical, and do it in its own commit.

3. **White balance: the planet's own colour, not grey.** The mineral moon makes the disc's mean
   neutral. Doing that to a planet would turn Mars grey and Saturn white. Instead, the planet path
   scales R and B about the background, as `CalibrateColour` does. The target is that the disc's
   mean B/G and R/G equal the planet's reflectance colour relative to sunlight. That colour is the
   ratio of its geometric albedos in the Johnson B, Johnson V and Cousins Rc bands.

   The values are from Mallama, Krobusek & Pavlov 2017, "Comprehensive wide-band magnitudes and
   albedos for the planets", *Icarus* 282 (arXiv:1609.05048), Table 7, and were checked against the
   paper on 2026-10-29.

   | Planet | A_B | A_V | A_Rc | B/G = A_B/A_V | R/G = A_Rc/A_V |
   |---|---|---|---|---|---|
   | Mercury | 0.105 | 0.142 | 0.158 | 0.739 | 1.113 |
   | Venus | 0.658 | 0.689 | 0.658 | 0.955 | 0.955 |
   | Mars | 0.088 | 0.170 | 0.250 | 0.518 | 1.471 |
   | Jupiter | 0.443 | 0.538 | 0.513 | 0.823 | 0.954 |
   | Saturn | 0.339 | 0.499 | 0.646 | 0.679 | 1.295 |
   | Uranus | 0.561 | 0.488 | 0.264 | 1.150 | 0.541 |
   | Neptune | 0.562 | 0.442 | 0.226 | 1.271 | 0.511 |

   - **Saturn:** the paper's reference magnitudes are at ring inclination 0 (its Eq. A-6.1, C0), so
     they are effectively the globe's. The rings were near edge-on in 2025–26. At wider ring openings
     the disc mask's mean includes ring light, and the error that follows is accepted.
   - **Approximation:** the camera's B, G and R channels are treated as B, V and Rc. HISTORY's
     calibration line says so, and names the source and the target ratios.
   - **Exclusions:** clipped pixels are left out, as in `CalibrateColour`.
   - **Skips:** the same rules as `CalibrateColour`, with a HISTORY line giving the reason.

4. **Display transfer: linear for planets.** Planet captures are exposed to fill the histogram, and
   planetary images are shown linear. The planet path applies no midtones stretch: the transfer
   has balance 0.5. Its white point follows the existing rule: the brightest sample not clipped in
   the stack lands at or under 0.95. That rule is what keeps a dim capture (Neptune) from coming out
   dark. The Moon keeps its 0.35-median stretch.

5. **Sharpen for planets.** The slider has the same meaning as on the Moon: 0 is off and larger
   values sharpen more. The noise limit holds at every amount. A planet disc is a few hundred px
   across rather than the Moon's frame-filling ~1000+, so its detail sits in the finest layers.
   Three things are left to measurement:
   - the planet layer gains (nominal gains and per-amount extra);
   - the slider's range;
   - its default.

   They are set from a Sharpen series on the real planet captures listed under Acceptance. The
   default is chosen by the measured crispness/noise trade, as Ruling R42 was. Only the default's
   look is Scott's call: the series goes to him as side-by-side PNGs when it is ready, and his pick
   becomes a ruling. The work continues while he looks. If the series shows that 4 is not enough,
   the range goes up.

6. **Colour for planets: saturation as a multiple of the natural colour.** The planet path uses the
   same ratio transfer as the mineral moon, `out_c = clip(L · (1 + k · w · d_c), 0, 1)`, with
   `k` = Saturation:
   - **No ±6 % clamp on d** (`devClamp`). Planet colour differences are far larger than the Moon's;
     Mars's B/G is about half its R/G.
   - **No highlight fade.** Jupiter's zones and Saturn's rings keep their colour.
   - **The per-pixel clip guard stays.** No new clipping: where a channel would pass 0.998, the
     pixel's saturation is reduced and its hue is kept.

   Saturation 1 gives the natural colour, `L · C_c / mean(C)`: hue and saturation from the blurred
   chroma, detail from the sharpened L. Saturation 0 is grey and 3 is the maximum.

   Chroma blur keeps the Moon's meaning, but its planet default and minimum are measured. The
   planet's colour detail (belts, the GRS) is a few px, so the Moon's 2.5 px default may smear it.
   0 must be allowed. The default is chosen on the real captures as the smallest blur that brings
   the sky's chroma noise down by half (the mineral bar's B5 measure).

7. **Parameters.** These are new process parameters, kept separate from the mineral moon's so that
   a Moon icon and a planet icon each keep their own settings. They are scriptable, and headless
   `ExecuteGlobal` honours them.

   | Id | Type | Default | Range |
   |---|---|---|---|
   | `planetProcess` | bool | false | |
   | `planetSaturation` | double | 1.0 | 0–3 |
   | `planetChromaBlur` | double | measured (call 6) | 0 to measured max |
   | `planetSharpen` | double | measured (call 5) | 0 to measured max |

   If `planetProcess` is true for a capture that is not eligible, the run fails loudly with the
   refusal's text, exactly as `mineralMoon` does.

8. **Interface.** The post-processing row follows the Target. For the Moon it shows the Mineral
   moon checkbox and the Strength/Chroma blur/Sharpen sliders. For a planet it shows a checkbox
   labelled **Planet processing** and the Saturation/Chroma blur/Sharpen sliders. Only the
   selected target's set is visible.

   Slider moves re-run only the cheap finish step on the cached data, with a live preview, exactly
   as the mineral sliders do today. Each slider has a tooltip in the same style as the mineral
   ones.

9. **Output.** Execute opens the plain stack as today. With Planet processing checked, it also
   opens `<stack>_planet`: 32-bit float and display-ready. Its keywords and HISTORY follow the
   mineral window's pattern, with one HISTORY line per step that names its measured values. The
   result line follows the mineral moon's (for example, the sharpen line).

10. **Version and release.** The release is 0.5.0.0, because it adds parameters and an output.
    Process and module ids do not change. Scott's "go ahead" on 2026-10-29 covers the release: ship
    through `./release.sh` once the final review is clean, as was done for 0.4.0.0. Update
    `package-description.xml` and `doc/tools/PlanetaryStack/`.

## Acceptance (the bar)

The real captures, read-only from the NAS:

| Night | Capture |
|---|---|
| 2026-10-23 | `Saturn/` |
| 2026-10-05 | `Saturn/` |
| 2026-09-06 | `Saturn/` |
| 8_13_2023 | `...Jupiter-Bin1 LPro 15.3F.avi` |
| 11_3_2023 | `...Jupiter-Bin1 LPro 13.8F.avi` |
| 2_12_2023 | `...Mars-Bin1 16.5F.avi` |
| 2_12_2023 | `...Venus-Bin1 15.8F.avi` |
| 2_12_2023 | `...Neptune-Bin1 14.9F.avi` |
| 2026-10-23 | `Neptune/` |

The two 2026-10-23 entries are directories: use the capture inside each. Bayer captures with no
sidecar run with RGGB set explicitly (mineral Ruling 3).

On every capture, a probe measures and reports these:
1. **Calibration:** after calibration, the disc's mean R/G and B/G are within 2 % of the catalogue
   target. If calibration skips, HISTORY gives the reason.
2. **Background:** the sky's mean per channel is ≤ 0.5 % of the disc's mean and equal across
   channels within noise. This is the mineral B3 measure.
3. **Dispersion:** after alignment, the measured R–G and B–G shift is ≤ 0.1 px wherever the step ran.
   Where it skipped (small discs), HISTORY gives the reason.
4. **No limb fringe or halo:**
   - The profile across the limb overshoots the sky by no more than 2 % of the disc's mean (B4).
   - On the processed image, the limb's chroma, measured in a ring 1–4 px outside the half-contrast
     line, is within 1.5× of the sky's chroma noise.
5. **Sharpening:** it raises DAN_interior (`test/bench/metric.py`) on the linear luminance; the sky's
   noise std rises by no more than 1.5×. Both are measured at the default.
6. **Clipping:** no sample is above 1.0, and there is no new clipping.
7. **Saturation:** Saturation 1 leaves hue and saturation at the natural colour's. The processed
   image's mean R/G and B/G over the disc are within 2 % of the calibrated values. Saturation 0 gives
   R = G = B.
8. **Timing:** a slider move takes ≤ 0.2 s at 1920×1080 on 32 threads.
9. **Mineral moon unchanged:** the mineral moon's output on 64.6F is bit-identical to 0.4.0.1's at
   the same parameters.

**Scott's judgement.** He sees side-by-sides of the plain stack and the processed output for
Saturn, Jupiter and Mars. He also gets the Sharpen series (call 5). There should be no fringe, no
halo, no blown highlights, and colour that looks like the planet.

**Gating, on real names:**
- `...Saturn...`, `...Jupiter...`, `...Mars...` with a broadband filter: the option is enabled.
- A `HaO3` capture: refused.
- An IR-pass filter token: refused.
- Mono: refused.
- Target Other: refused, with the tooltip telling the user to pick a target.
- Moon: the mineral row, unchanged.

## Testing and proof

1. **Unit (ctest, synthetic, also in the Rocky 9 container):**
   - every changed or new step against its requirement, on synthetic planet discs:
     - known colour cast;
     - known dispersion shift;
     - noise;
     - clipped highlights;
     - a disc with a ring;
     - a tiny disc (≤ 40 px);
   - the gate;
   - parameter round trips;
   - the mineral path's existing tests, unchanged and green.
2. **Headless probe (slot 96):** `ExecuteGlobal` with `planetProcess` true on a Saturn fixture.
   Check the windows, keywords and HISTORY. With an ineligible capture, the run must fail loudly.
   Re-run the existing mineral probe.
3. **GUI smoke (slot 97):**
   - Open a Saturn fixture. The Planet processing row shows, and the checkbox is enabled.
   - Click Stack!. Move each slider; the preview updates.
   - Execute opens two windows.
   - Switch the Target to Moon: the mineral row shows.
4. **Real data:** run the acceptance captures above (host-only, NAS read-only) and collect the
   numbers and PNGs.
5. **Portability:** the container build, `verify-portable.sh`, and ASCII-only PCL string literals.

## Non-goals

- The Sun.
- Mono or IR-pass planet sharpening (a 1-channel path).
- Derotation (WinJUPOS-style), drizzle, and planet mosaics.
- Local-contrast enhancement, deconvolution with a measured PSF, and BXT/NXT calls.
- Any change to the mineral moon's look, defaults or parameters.
- A natural-colour (non-mineral) Moon mode.

## Anti-goals

- **No grey Mars.** No white balance that erases a planet's real colour.
- **No defects hidden.** A step that cannot run (a tiny disc, no sky) says why in HISTORY and in the
  result line. It never guesses.
- **No sharpening artefacts.** No ringing halo around the limb or the rings, and no amplified noise
  passed off as detail.
- **No new clipping.** No samples above 1.0, and none clipped that the stack did not clip.
- **No mineral-moon regression.**

## Left to the builder

These are left to the builder:
- the planet layer gains, the Sharpen range and default (measured, call 5), and the Chroma blur
  default (measured, call 6);
- how the shared code is parameterised by target kind;
- the slider layout details and tooltips;
- the probe's measurement code.

All of it must meet the measured requirements above.

## Rulings after the builder check (Claude's calls, 2026-10-29; Scott may veto)

**P1. Planet discs are small, and the planet path adapts to them.**

The discs in the acceptance captures are 12–50 px across. Measured: 2026-10-23 Saturn, rings
included, is 36 × 17 px on a 320 × 240 ROI. The reviewer's estimates for the others are about 12 px
(Mars), 19 px (Venus) and 41–49 px (Jupiter). Scott's future captures through the 127 Mak will be
about 2× larger.

As written, three steps would skip or do nothing on these discs:
- calibration (the 1 %-of-frame rule, `Mineral.cpp:395,404`);
- `AlignChannels` (16 windows, about 60 px);
- the limb fade (one to two layer reaches, 2/6/14/30 px).

So the planet path does not reuse these three unchanged. It adapts them as follows. The Moon path is
untouched.

- **Calibration:** the floor is an absolute one: at least 30 unsaturated disc pixels. The 1 %
  fraction is not used.
- **Dispersion:** a small-disc method. It measures one sub-pixel shift per channel against G over
  the whole disc and its limb, with an uncertainty, and resamples when the shift is over 0.1 px and
  over 3σ. The algorithm is the builder's. The 60 px-and-up windowed method may stay for large discs.
  - Required, on synthetic 12, 20, 40 and 200 px discs with a known 0.5 px R and B shift: the
    residual is ≤ 0.1 px.
  - Required, with no true shift: no applied shift of more than 0.1 px (6 seeds).
- **Sharpen's limb fade:** scaled to the disc, for example with each layer's fade width capped at a
  fraction of the disc radius, so that the finest layers act on a 20 px disc.
  - Required: the limb overshoot bar (acceptance 4) still holds.
  - Required, on synthetic blurred discs of 20, 40 and 200 px with known truth: the RMS error
    against the sharp truth falls at the default amount. This is mineral Ruling 2's ground-truth
    test.
- **No disc:** a capture where no disc is found, such as a faint Neptune, runs every step as a skip
  with its reason. This is an accepted outcome, not a failure.

**P2. Acceptance 5 is replaced.** DAN always rises under sharpening (`metric.py:120`) and is not
judged under 20 px. The sharpening bar becomes:
- P1's ground-truth tests;
- on real data, sky noise ≤ 1.5×;
- Scott's look.

**P3. Acceptance 4 (limb chroma) is measured as dispersion asymmetry, not absolute chroma.**
Dispersion shows up as opposite colours on opposite limbs; real planet colour does not.
- **Measure:** the difference in (R − B) / L between the two limb sides along the measured
  dispersion axis, each side in a ring 1–4 px outside the half-contrast line.
- **Bar:** after the planet path, that difference is ≤ 1.5× the same measure on a zero-dispersion
  synthetic disc of the same size and noise. On the real captures, it is reported before and after
  alignment.

**P4. The capture-name parser learns ASIAIR's older form, which has no filter token.** For example:
`2023-02-11-192403-Mars-Bin1 16.5F.avi`. The target is parsed and the filter is unknown; an unknown
filter is allowed (`CaptureName.h:32-33`).

The 2_12_2023 Moon captures then resolve to Moon under Auto, which is the correct behaviour. Mineral
moon's rule, defaults and output do not change.

**P5. Release order.** Two things go to Scott as soon as they exist:
- the Sharpen and Chroma blur series;
- the plain-versus-processed side-by-sides for Saturn, Jupiter and Mars.

0.5.0.0 ships once he has picked the defaults and the final review is clean. His pick is the only
point where the work waits on him.

**P6. Acceptance captures.**
- 2026-10-05 `Saturn/` holds only `...HaO3 9.0F.avi`, so it serves as the HaO3 gating test, not as a
  processing capture.
- The rings' place in the disc mask is measured on real Saturn, not assumed. If `SegmentLunar`
  splits them off, the planet path keeps them in the disc.
