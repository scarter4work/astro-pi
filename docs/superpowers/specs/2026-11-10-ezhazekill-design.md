# EZHazeKill: why it is too aggressive, the fix, and the preview

Date: 2026-11-10. Source: `scripts/ez-stretch/src/scripts/EZ Stretch BSC/EZHazeKill.js` 1.0.2.
Measurements: offline numpy reimplementation (`scratchpad/hk_lib.py`, `scratchpad/hk_run.py`) on six images. Raw numbers are in `scratchpad/hazekill/metrics.json` and the comparison images are in `scratchpad/hazekill/*.png`. PixInsight was not launched.

## 1. What 1.0.2 computes

There is no haze model: no dark channel, no gradient and no masks. The script applies a constant offset to each channel:

```
bg_c = median of channel c over the whole image          (img.median)
out  = clamp(x - bg_c + targetLevel, 0, 1)               (PixelMath, rescale off, truncate ON)
```

| Parameter | Default | Notes |
|---|---|---|
| `targetLevel` | **0.05** | Range 0-0.5. The one tunable. It is an absolute level, and every channel is moved to it. |
| target view | active view | |
| (console) cast threshold | 0.005 | Only drives the "cast present" message. |

- Measure prints the medians and the max-min spread. Apply runs the PixelMath.
- A process icon saves only `targetLevel`.
- History: `archive/pi-scripts/HazeKill.js` had a hard-coded `TARGET 0.05`. It became EZHazeKill 1.0.0 in `322e2472` on 2026-06-03. Commits 1.0.1 and 1.0.2 (`2f749c14` menu move, `9c8a7e28` repackage) did not change the math.
- The script's own header warns that "a frame-filling nebula biases the median high and this will over-subtract". Nothing checks for it.

## 2. Where the aggressiveness comes from (measured)

### Datasets

Linear masters, 3840x2160 ASI585MC Air:

| Id | Image | Prep |
|---|---|---|
| L1 | LDN 1235, Full (broadband) | Gradient removed GraXpert-style first: deg-3 polynomial with the pedestal kept, which is the documented use. |
| L2 | M 33, Lqef | Same prep as L1. |
| L3 | NGC 7000, HaO3 | Same prep. The nebula fills the frame. |

Stretched images:

| Id | Image |
|---|---|
| N1 | Your `Desktop/M33_RGB_9_8_2026.xisf` (GHS-stretched). |
| N2 | `astro_work/ic1396/masters/hoo_final.xisf`. |
| N3 | L3 after an unlinked AutoSTF stretch. |

### How each quantity is measured

- **"Darkest sky"**: the median of the darkest 5% of 64-px tiles.
- **σ**: the background MAD.
- **Faint mask**: Gaussian(3)-smoothed luminance between 5σ and 30σ of the smoothed noise, with stars excluded.
- **Faint retained**: the mean faint signal above what the tool now treats as background, divided by the signal above the darkest sky before the tool ran.

### Why I think you run it on stretched images

This is an inference. The history in `M33_RGB_9_8_2026.xisf` runs GHS ×10 → Curves → **BackgroundNeutralization → DBE** → Curves → Crop. So you do background work after stretching. The script says "LINEAR" but never checks.

### Root causes, in order of damage

**1. On stretched data, the absolute target 0.05 combined with the median estimator destroys data.**

- The whole-image median sits far above the sky: 31-46σ on N1 and 10-11σ on N2. The galaxy or nebula is in the median.
- So the script subtracts 0.10-0.20 from data whose sky is only 0.065-0.09.

| Image | Pixels clipped to 0 | Faint-mask px clipped | Background tiles at 0 | Faint signal retained |
|---|---|---|---|---|
| N1 M33 (yours) | **42.6%** | 19.0% | 84.9% | **21.9%** |
| N2 IC1396 HOO | 24.6% | 11.2% | 47.8% | 11.8% |
| N3 NGC7000 STF | 37.7% | 15.4% | 41.3% | 37.7% |

On N1 the darkest sky lands at −0.07 and is clamped to zero. The background spread drops from 0.046 to 0, which is a black crush.

**2. Hard truncation** (`truncate = true`) throws the clipped values away; nothing compresses them.

**3. On linear data it does not clip, but it raises the pedestal and neutralizes on a biased median.**

- With σ ≈ 0.0003 against offsets of at most 0.056, there were 0 clipped pixels on L1-L3.
- The sky sits below 0.05 on L2 (0.031 mean) and L3 (0.017). The script **adds** a pedestal of +0.019 on L2 and +0.033 on L3. Seen through the existing STF it is a white-out (`L2/L3 *.fixedSTF.png`). That is haze added, not killed.
- On NGC 7000 the R median is **3.5σ** above the darkest sky. Neutralizing to that median puts the faint Ha below the new background: faint retained is **−235%** and the faint-signal hue rotates 114°. After a re-autostretch, the dark lanes go teal (`L3 *.autoSTF.png`).
- On L1 and L2 the bias is only 0.4-0.8σ (faint retained 68% and 63%).

**Ruled out:**
- Highlight clipping: at most 0.0004% of pixels at 1.
- Colour shifts in bright regions: 0.4-3.2°.
- The default strength as such: there is no strength control. The damage is the estimator plus the domain-blind absolute target plus truncation.

## 3. Proposed algorithm (v1.1 math, same in either delivery)

1. **Sky estimator.**
   - Split the image into 64-px tiles and take a per-tile, per-channel median and MAD.
   - Rank the tiles by luminance and take `sky_c` as the median over the darkest **10%** of tiles.
   - `σ_c` is the median tile MAD × 1.4826 over those same tiles.
   - Optionally, a user-chosen background preview overrides the estimate, as BackgroundNeutralization does.
2. **Split the offset into two separately controlled parts.**
   - Cast: `cast_c = sky_c − mean(sky)`, scaled by **Neutralize**.
   - Pedestal: `ped = max(0, mean(sky) − T_eff)`, scaled by **Amount**, where `T_eff = max(Target, k·max σ)`.
   - The tool never raises the pedestal, and never pushes the sky into its own noise.
3. **Soft toe in place of truncation.** Use `f(u) = u` for `u ≥ ε`, else `ε·exp(u/ε − 1)`, with `ε = 2σ`. It is monotonic and C¹, and produces no zeros.
4. **Domain warning.** If the image looks stretched (sky above about 0.08), print it. Do not refuse, because your workflow does this.

| Parameter | Proposed default | Range |
|---|---|---|
| Amount (pedestal removal) | **0.5** | 0-1 |
| Neutralize (cast removal) | **1.0** | 0-1 |
| Target background | 0.05 | 0-0.5 |
| Noise floor k | **4σ** | 0-10 |
| Sky percentile (advanced) | **10%** | 1-50 |
| Background reference | Auto | Auto / preview |
| Soft toe | on | |

### Measured effect of the proposed defaults vs 1.0.2

| Image | 1.0.2: clipped / faint retained / sky after | v1.1 defaults: clipped / faint retained / sky after |
|---|---|---|
| L1 LDN1235 (lin) | 0% / 68.0% / 0.050 | 0% / **87.9%** / 0.067 (pedestal −50%) |
| L2 M33 Lqef (lin) | 0% / 63.4% / 0.050 (**raised** from 0.031) | 0% / **84.3%** / 0.031 (not raised, cast removed) |
| L3 NGC7000 (lin) | 0% / **−235%** / 0.050 (raised from 0.017) | 0% / **66.6%** / 0.017 |
| N1 M33 GHS (yours) | **42.6%** / 21.9% / <0 (crushed) | **0%** / **98.6%** / 0.067 (from 0.085) |
| N2 IC1396 HOO | 24.6% / 11.8% / <0 | 0% / **96.2%** / 0.056 |
| N3 NGC7000 STF | 37.7% / 37.7% / <0 | 0% / **93.0%** / unchanged (see note) |

- On N3, the 4σ floor is above the sky (σ = 0.036-0.067), so only the cast is removed. That is the protection working.
- Faint-signal hue shift falls to 0.2-6.6°. With 1.0.2 it was 3.5-114.6°.
- **Amount = 1.0** gives the same retention (N1 98.6%) with the sky at 0.048. It is the "full strength" setting for the preview to show.
- **Sky percentile 10% vs 25%**: faint retained was 88/84/67/99/96/93% at 10% against 81/82/51/94/80/78% at 25%, so 10% is the default.
- **Floor and toe ablation** (N1, Target 0, Amount 1):

| Variant | N1 pixels clipped | N2 pixels clipped |
|---|---|---|
| Neither floor nor toe | 12.1% | 7.7% |
| 4σ floor only | 0.46% | 0.027% |
| Floor + toe | 0% | 0% |

- **Caveat.** In linear data, faint signal is below σ per pixel, and L3 has no true sky in the frame. Read the linear retention numbers as a comparison between methods, not as absolute values.

**Images:** `scratchpad/hazekill/`

| File | Panels |
|---|---|
| `N1_M33_Scott_GHS_stretched.png` | before / 1.0.2 / proposed |
| `N2_IC1396_HOO_stretched.png` | before / 1.0.2 / proposed |
| `N3_NGC7000_HaO3_STFstretched.png` | before / 1.0.2 / proposed |
| `L1/L2/L3_*.fixedSTF.png` | the before-image STF kept, as PI shows after Apply |
| `L1/L2/L3_*.autoSTF.png` | re-autostretched |

## 4. Preview: PJSR dialog vs PCL Real-Time Preview

| | (a) PJSR, in-dialog preview | (b) PCL process + native RTP |
|---|---|---|
| Mechanism | A custom ScrollBox and Bitmap. EZStretch's `UnifiedPreviewControl` (`EZStretch.js:480-752`) fills its Bitmap with `setPixel` one pixel at a time. | `InterfaceFeature::RealTimeButton` plus `RequiresRealTimePreviewUpdate` / `GenerateRealTimePreview` (`ProcessInterface.h:150,1163`) and `RealTimePreview::SetOwner/Update` (`RealTimePreview.h:92,103`). |
| Preview quality | A downsampled copy. Linear data needs a reimplemented STF or it shows black. No zoom or pan into full resolution unless built, and no masks. | PI renders through the view's own STF and colour management, at any zoom, on previews, and with masks. It uses the same engine call as Execute, so the preview equals the result. |
| Other gains | none | A real process: icons, ProcessContainer, history replay, undo through `ExecuteOn`. |
| Effort | About 1-1.5 days | About 4-4.5 days, per the Gaia plan's sizing. Module skeleton and release wiring 1.5 d; engine and ctest 0.5 d; process, interface and RTP 1.5 d; CPD 6.1 doc 0.5 d; headless probe and GUI smoke (Planetary pattern) 0.5 d. |
| Fit with the repo | It stays in the EZ scripts. That runs against "no python no js". The EZ scripts still ship **no docs** (CPD re-audit item 5, open). | It matches the NukeX and Planetary Process/Instance/Interface/Parameters pattern, the container build and `release.sh`. It needs the same doc either way. |

**Recommendation: (b), a PCL process with native Real-Time Preview.**

- In this case the preview has to show the result through your current STF. That is exactly where 1.0.2's linear damage hides: a pedestal raise looks like a white-out under the STF you already have, and invisible after a re-autostretch. Only RTP does that faithfully.
- (b) also follows the pure-PCL direction.
- **Design constraint:** compute the sky and σ once, on the full target view, and cache them. The RTP image is a reduced rendition, so estimating on it would give different numbers. Recompute only when the view or the reference preview changes.
- **PCL version:** it needs nothing specific to PCL 2.13. Build it with whatever PCL the container pins when the Gaia port moves the pin to 2.13 (the current pin is `3a4af800`). It should not get a pin of its own.
- No module in the repo uses RTP yet, so this would be the first. PI's BackgroundNeutralization is the closest native analogue.

## 5. Decisions for you (my recommendation in bold)

1. **Delivery:** **PCL process with RTP**, rather than a PJSR preview dialog.
2. **Interim fix:** ship the section 3 math as script 1.1.0 now, about 40 lines and half a day plus a doc page. **Yes.** 1.0.2 clips 25-43% of the pixels in the stretched images you work on. Retire the script when the module ships.
3. **Module home:** **a new `EZTools` module** with process id `EZHazeKill`, kept stable for ever, so EZStretch and EZDonutRepair can follow it. The alternative is a single-purpose module. In the same release, drop the script package from the manifest so there is only one menu entry.
4. **Default Amount:** **0.5**, or 1.0. The preview makes pushing it trivial.
5. **Stretched input:** **support it**, with the σ floor making it domain-agnostic and a console note. The alternative is to refuse non-linear input.
6. **OSC quad-band green starvation** (the magenta cast in OIII regions on Lqef): this is object colour, not haze, and the sky cast removal does not fix it. Either add a Green-balance control as phase 2 of the PCL port, or drop it. **Phase 2 of the port.**
