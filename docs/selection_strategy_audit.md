# Selection Strategy Validation Audit (8-Class Taxonomy)

**Auditor**: ASTRO-SME (Astrophotography Subject Matter Expert)
**Date**: 2026-02-19
**Supersedes**: Previous 21-class audit dated 2026-01-27
**NukeX Version**: v2.2.x (8-class taxonomy since v2.0)

**Files Reviewed**:
- `/home/scarter4work/projects/NukeX/src/engine/SelectionRules.cpp`
- `/home/scarter4work/projects/NukeX/src/engine/SelectionRules.h`
- `/home/scarter4work/projects/NukeX/src/engine/PixelSelector.cpp`
- `/home/scarter4work/projects/NukeX/src/engine/PixelStackAnalyzer.cpp`
- `/home/scarter4work/projects/NukeX/src/engine/PixelStackAnalyzer.h`
- `/home/scarter4work/projects/NukeX/src/engine/RegionStatistics.h`

---

## Executive Summary

The NukeX intelligent pixel selection system implements class-specific strategies for all **8 region classes** introduced in the v2.0 taxonomy. The implementation spans three components:

1. **SelectionRules.cpp** -- Stretch algorithm selection and parameter tuning per class (used for region-aware stretching)
2. **PixelStackAnalyzer.cpp** -- Per-pixel outlier detection (via `IsClassSpecificOutlier`) and best-value selection (via `SelectBestValue`), plus class-adjusted sigma thresholds (via `GetClassAdjustedConfig`)
3. **PixelSelector.cpp** -- Orchestration layer: target context adjustments, spatial smoothing in transition zones, per-frame segmentation consensus

### Architecture Note

The selection pipeline has two distinct class-aware subsystems:

- **Pixel selection** (stacking): `PixelStackAnalyzer::GetClassAdjustedConfig()` sets outlier thresholds and signal preference flags; `IsClassSpecificOutlier()` implements asymmetric rejection logic; `SelectBestValue()` chooses the pixel value from the stack.
- **Stretch algorithm selection** (post-stacking): `SelectionRules::DefaultRules` maps each class to an optimal stretch algorithm (MTF, GHS, ArcSinh, RNC, SAS) with tuned parameters.

Both subsystems receive the same `RegionClass` from the ML segmentation map but operate independently.

**Overall Assessment: PASS** -- All 8 classes have astrophysically sound strategies. Two minor issues and three improvement recommendations are noted below.

---

## Per-Class Analysis

### Class 0: Background

**Description**: Sky background, gradients, noise

| Subsystem | Parameter / Behavior | Value |
|-----------|---------------------|-------|
| **Outlier sigma** | `GetClassAdjustedConfig` | **3.0** |
| **Outlier direction** | `IsClassSpecificOutlier` | Symmetric + 0.85x tighter on high side |
| **Signal preference** | `favorHighSignal` / `favorLowSignal` | false / false |
| **Selection strategy** | `SelectBestValue` (default case) | Closest to median of valid values |
| **Stretch algorithm** | `GetBackgroundRules` | SAS (SNR>15, median<0.1), MTF (SNR<8), MTF default midtones=0.25 |
| **Stretch default** | `GetDefaultRecommendation` | MTF midtones=0.20 |

**Astrophysical Assessment: CORRECT**

Background pixels should converge toward the median to suppress gradients, light pollution, and satellite contamination. The asymmetric high-side penalty (0.85x multiplier on the sigma threshold for values above mu) is well-motivated: high outliers in the background are more likely to be contamination (satellites, aircraft, gradient artifacts) than legitimate signal. Low outliers in the background are less concerning since they are simply slightly darker sky. The SAS stretch for high-SNR background is appropriate for revealing faint IFN (integrated flux nebulae) or galactic cirrus.

**Status: PASS**

---

### Class 1: BrightCompact

**Description**: Bright/saturated stars, diffraction spikes

| Subsystem | Parameter / Behavior | Value |
|-----------|---------------------|-------|
| **Outlier sigma** | `GetClassAdjustedConfig` | **4.0** |
| **Outlier direction** | `IsClassSpecificOutlier` | Reject LOW only (below mu) |
| **Signal preference** | `favorHighSignal` | true |
| **Selection strategy** | `SelectBestValue` | Select MAXIMUM (weighted by frame quality) |
| **Stretch algorithm** | `GetBrightCompactRules` | ArcSinh: softness 0.02 (median>0.9), dynamic (DR>3), 0.15 (clip>1%), 0.1 default |
| **Stretch default** | `GetDefaultRecommendation` | ArcSinh softness=0.05 |

**Astrophysical Assessment: CORRECT**

Bright stars are the most consistent features in an imaging session. A dim measurement of a bright star is almost always an error (cloud passage, tracking loss, poor seeing spreading the PSF). The 4.0-sigma threshold is appropriately permissive since bright stars have high intrinsic variance from scintillation. Selecting the maximum value preserves the true stellar brightness. ArcSinh compression is the correct stretch for handling the extreme dynamic range of stellar cores without clipping.

The low-only rejection correctly handles the scenario where a subset of frames have reduced flux (clouds, poor transparency). A bright star reading 20% below the mean is far more suspicious than one reading 20% above.

**Status: PASS**

---

### Class 2: FaintCompact

**Description**: Medium/faint stars, star clusters

| Subsystem | Parameter / Behavior | Value |
|-----------|---------------------|-------|
| **Outlier sigma** | `GetClassAdjustedConfig` | **3.5** |
| **Outlier direction** | `IsClassSpecificOutlier` | Symmetric |
| **Signal preference** | `favorHighSignal` | true |
| **Selection strategy** | `SelectBestValue` | Probability-weighted with +0.2 upward bias |
| **Stretch algorithm** | `GetFaintCompactRules` | GHS: stretchFactor 0.3 (median>0.5), 0.5 (SNR>8), dynamic default |
| **Stretch default** | `GetDefaultRecommendation` | GHS stretchFactor=0.4, highlightProtection=0.85 |

**Astrophysical Assessment: CORRECT**

Faint compact objects (dim stars, resolved star cluster members) benefit from a nuanced approach. Unlike bright stars where maximum selection is appropriate, faint stars sit closer to the noise floor, making pure maximum selection risky (noise spikes masquerade as signal). The probability-weighted selection with a mild +0.2 upward bias is a good compromise: it favors values slightly above the mean (where real signal is more likely) while penalizing large deviations that are more likely noise.

Symmetric outlier rejection at 3.5-sigma is appropriate. Unlike bright stars, faint compact sources can have anomalously high values from cosmic rays or hot pixel bleed, so rejecting both tails is correct.

GHS stretch with moderate highlight protection preserves faint stellar photometry while preventing the brighter cluster members from washing out.

**Status: PASS**

---

### Class 3: BrightExtended

**Description**: Emission/reflection nebulae, galaxies, cirrus

| Subsystem | Parameter / Behavior | Value |
|-----------|---------------------|-------|
| **Outlier sigma** | `GetClassAdjustedConfig` | **3.5** |
| **Outlier direction** | `IsClassSpecificOutlier` | Reject LOW only (below mu) |
| **Signal preference** | `favorHighSignal` | true |
| **Selection strategy** | `SelectBestValue` | Probability-weighted with +0.3 upward bias |
| **Stretch algorithm** | `GetBrightExtendedRules` | RNC (SNR>20, median>0.3), GHS (DR>2.5), ArcSinh (median>0.8), GHS default 0.65/0.75 |
| **Stretch default** | `GetDefaultRecommendation` | GHS stretchFactor=0.55, highlightProtection=0.75 |
| **Target context** | `ApplyTargetContextAdjustments` | Confidence boost when `expectsBrightExtended` matches |

**Astrophysical Assessment: CORRECT**

Extended emission (nebulae, galaxies, IFN) is the primary science target for most deep-sky astrophotographers. The low-only rejection is correct: a frame where nebula signal drops (clouds, transparency variation) should be penalized, but a frame where nebula signal is stronger (better conditions) should never be rejected as an outlier.

The +0.3 upward bias is the strongest signal-favoring factor in the system, which is appropriate -- nebular signal is precious and the goal is to maximize the signal-to-noise ratio in the final stack. The probability weighting prevents pure maximum selection, which would introduce noise.

The RNC (Rational Noise Compression) stretch choice for high-SNR extended objects is excellent for color preservation in nebulae. The ArcSinh fallback for very bright cores (median>0.8) correctly handles galaxy nuclei and bright emission knots.

The target context system correctly boosts confidence when FITS OBJECT keywords match known emission nebulae (M42, NGC 7000, etc.) or galaxies (M31, M51, etc.), reinforcing the ML classification.

**Status: PASS**

---

### Class 4: DarkExtended

**Description**: Dark nebulae, dust lanes

| Subsystem | Parameter / Behavior | Value |
|-----------|---------------------|-------|
| **Outlier sigma** | `GetClassAdjustedConfig` | **3.0** |
| **Outlier direction** | `IsClassSpecificOutlier` | Reject HIGH only (above mu) |
| **Signal preference** | `favorLowSignal` | true |
| **Selection strategy** | `SelectBestValue` | Select MINIMUM (weighted by frame quality) |
| **Stretch algorithm** | `GetDarkExtendedRules` | MTF midtones=0.15 (median<0.03), GHS 0.4/0.8 (SNR>8, median<0.1), MTF 0.2 default |
| **Stretch default** | `GetDefaultRecommendation` | MTF midtones=0.15 |
| **Target context** | `ApplyTargetContextAdjustments` | Confidence boost when `expectsDarkExtended` matches |

**Astrophysical Assessment: CORRECT -- CRITICAL CLASS**

This is the most astrophysically delicate class in the system. Dark nebulae (Barnard objects, Bok globules) and dust lanes (across galaxy disks, in H II regions) are defined by the *absence* of light. Their scientific and aesthetic value comes from being genuinely dark against brighter backgrounds.

The implementation is correct on all counts:

1. **High-only rejection**: A dark region that suddenly appears bright in one frame is contaminated (light pollution gradient, satellite, moonlight scatter). A dark region that appears even darker is likely more accurate.
2. **Minimum selection**: Choosing the darkest valid frame maximizes the contrast between the dark feature and surrounding emission/stars. This is the correct strategy.
3. **`favorLowSignal=true`**: Reinforces the selection preference.
4. **3.0-sigma threshold**: Slightly tighter than BrightCompact (4.0) because dark features have less intrinsic variance. Consistent readings should cluster tightly; outliers are more suspicious.
5. **MTF stretch with low midtones**: Preserves the darkness while gently lifting any embedded structure.

The target context correctly identifies known dark nebula targets (Horsehead, Barnard catalog) to boost classification confidence.

**Status: PASS**

---

### Class 5: Artifact

**Description**: Hot pixels, satellite trails

| Subsystem | Parameter / Behavior | Value |
|-----------|---------------------|-------|
| **Outlier sigma** | `GetClassAdjustedConfig` | **2.0** |
| **Outlier direction** | `IsClassSpecificOutlier` | Symmetric, aggressive |
| **Signal preference** | `favorHighSignal` / `favorLowSignal` | false / false |
| **Selection strategy** | `SelectBestValue` (default case) | Closest to median of valid values |
| **Stretch algorithm** | `GetArtifactRules` | MTF midtones=0.05 (median>0.3), SAS (SNR<5), MTF 0.05 default |
| **Stretch default** | `GetDefaultRecommendation` | MTF midtones=0.05, confidence=0.4 |

**Astrophysical Assessment: CORRECT**

Artifacts should be suppressed as aggressively as possible. The 2.0-sigma threshold is the tightest in the system, which is appropriate: hot pixels and satellite trails are transient features that should not survive stacking. Symmetric rejection catches both bright artifacts (hot pixels, satellites) and dark artifacts (cold pixels, shadow artifacts).

Median selection ensures that even if multiple frames contain an artifact at the same position (common for persistent hot pixels), the artifact is averaged away by the artifact-free majority. The very low MTF midtones (0.05) further suppresses any residual artifact signal in the stretch.

The SAS rule for low-SNR artifact regions is a thoughtful addition -- noisy regions classified as artifacts benefit from statistical smoothing.

**Status: PASS**

---

### Class 6: StarHalo

**Description**: Diffuse glow around stars

| Subsystem | Parameter / Behavior | Value |
|-----------|---------------------|-------|
| **Outlier sigma** | `GetClassAdjustedConfig` | **3.5** |
| **Outlier direction** | `IsClassSpecificOutlier` | Symmetric |
| **Signal preference** | `favorHighSignal` | true |
| **Selection strategy** | `SelectBestValue` | Probability-weighted with +0.15 upward bias |
| **Stretch algorithm** | `GetStarHaloRules` | GHS 0.7/0.8 (SNR>15, median>0.2), GHS 0.5/0.7 (SNR<8, median<0.1), GHS 0.6/0.75 default |
| **Stretch default** | `GetDefaultRecommendation` | GHS stretchFactor=0.6, highlightProtection=0.75 |

**Astrophysical Assessment: CORRECT**

Star halos are diffuse, low-surface-brightness features surrounding bright stars, caused by optical scattering (telescope optics, atmospheric dispersion, sensor blooming). The key challenge is preserving the smooth radial gradient from stellar core to background.

The symmetric rejection is appropriate: star halos should not have large frame-to-frame variation if conditions are stable. The mild +0.15 upward bias preserves the halo signal without over-emphasizing it (which would create artificially bloated halos).

GHS stretch is the correct choice for star halos because it provides smooth, continuous tone mapping that preserves gradients. The highlight protection prevents the halo from merging into the stellar core.

**Status: PASS**

---

### Class 7: Vignette

**Description**: Optical vignetting (corner/edge light falloff)

| Subsystem | Parameter / Behavior | Value |
|-----------|---------------------|-------|
| **Outlier sigma** | `GetClassAdjustedConfig` | **3.5** |
| **Outlier direction** | `IsClassSpecificOutlier` | Reject LOW only (below mu) |
| **Signal preference** | `favorHighSignal` | true |
| **Selection strategy** | `SelectBestValue` | Select MAXIMUM (weighted by frame quality) |
| **Stretch algorithm** | No explicit rules in `GetAllRules()` | Falls through to default |
| **Stretch default** | `GetDefaultRecommendation` (default case) | MTF midtones=0.18, confidence=0.3 |

**Astrophysical Assessment: CORRECT (with note)**

Vignetting causes systematic light falloff toward image corners/edges due to optical path geometry. The degree of vignetting varies between frames due to sensor tilt, filter wheel position, and flexure. Selecting the maximum (least-vignetted) frame at each pixel is the correct strategy: the brightest reading represents the frame with the least optical attenuation at that position.

Low-only rejection is correct: an anomalously dim value in a vignetted region is a frame with worse-than-average vignetting or additional gradient contamination. A brighter value is simply a frame with less vignetting.

**Note**: The Vignette class has **no explicit stretch rules** in `DefaultRules::GetAllRules()` -- it is missing from the set. This means it falls through to the generic `GetDefaultRecommendation()` default case (MTF midtones=0.18, confidence=0.3). While this is acceptable since vignetted regions will typically be flat-corrected before stretching, explicit rules would be more robust. See Recommendations.

**Status: PASS (with note on missing stretch rules)**

---

## Cross-Cutting Mechanisms

### Iterative Sigma Clipping

All class-aware paths (`AnalyzePixelWithClass`, `AnalyzePixelWithClassAndAnomalies`) perform 2 additional passes of sigma clipping after the initial outlier detection. The clipping uses MAD-based sigma floor to prevent "death spiral" where sigma shrinks each pass and progressively rejects valid data. The minimum valid frame count is `max(minFramesForStats, N/2)`, preventing over-rejection.

**Assessment: CORRECT** -- The sigma floor mechanism is critical for small stacks (3-10 frames) where a single outlier can distort the sigma estimate dramatically.

### Per-Frame Anomaly Detection

When per-frame segmentation is available, `AnalyzePixelWithClassAndAnomalies` applies a 0.7x multiplier to the outlier threshold for frames whose segmentation class disagrees with the consensus. This is well-motivated: if a frame classifies a pixel as Background while all other frames classify it as BrightExtended, the frame likely had a transparency problem.

**Assessment: CORRECT**

### Spatial Smoothing at Transition Zones

`PixelSelector::GetSpatialContext` detects transition zones where fewer than 5 of 8 neighbors share the center pixel's class. In transition zones, `ApplySpatialSmoothing` blends the selected value toward the median based on transition strength.

**Assessment: CORRECT** -- Prevents hard boundaries between regions with different selection strategies.

### Target Context

`TargetContext::InferExpectedFeatures` parses FITS OBJECT and FILTER keywords to set `expectsBrightExtended` and `expectsDarkExtended` flags. Matching classes receive a confidence boost of `contextWeight`. Known object catalogs include Messier emission nebulae, galaxies, NGC objects, and planetary nebulae.

**Assessment: CORRECT** -- Narrowband filter detection correctly infers `expectsBrightExtended` since narrowband signal is almost exclusively emission nebulosity.

---

## Summary Table

| ID | Class | Outlier Sigma | Outlier Direction | Selection Strategy | Favor Signal | Stretch Algorithm | Status |
|----|-------|:---:|---|---|---|---|:---:|
| 0 | Background | 3.0 | Symmetric + 0.85x high penalty | Closest to median | Neither | MTF / SAS | PASS |
| 1 | BrightCompact | 4.0 | Low only | Maximum | High | ArcSinh | PASS |
| 2 | FaintCompact | 3.5 | Symmetric | Prob-weighted +0.2 bias | High | GHS | PASS |
| 3 | BrightExtended | 3.5 | Low only | Prob-weighted +0.3 bias | High | RNC / GHS / ArcSinh | PASS |
| 4 | DarkExtended | 3.0 | High only | Minimum | Low | MTF / GHS | PASS |
| 5 | Artifact | 2.0 | Symmetric | Closest to median | Neither | MTF / SAS | PASS |
| 6 | StarHalo | 3.5 | Symmetric | Prob-weighted +0.15 bias | High | GHS | PASS |
| 7 | Vignette | 3.5 | Low only | Maximum | High | (no explicit rules*) | PASS* |

*Vignette falls through to generic default stretch (MTF midtones=0.18, confidence=0.3). See Issue 1.

---

## Issues Found

### Issue 1: Missing Vignette Stretch Rules (LOW PRIORITY)

`DefaultRules::GetAllRules()` aggregates rules from 7 `Get*Rules()` functions but there is no `GetVignetteRules()`. The Vignette class falls through to the generic default case in `GetDefaultRecommendation()`, which returns MTF midtones=0.18 with confidence=0.3 (the lowest confidence in the system).

**Impact**: Low in practice because vignetted regions should be corrected by flat fielding before stretching. However, if flat correction is imperfect or unavailable, the low-confidence generic stretch may not be optimal.

**Recommendation**: Add `GetVignetteRules()` with a gentle MTF stretch (midtones 0.20-0.25) that matches the background treatment, since vignetted regions are essentially background with systematic attenuation.

### Issue 2: RegionStatistics.h Comment Mismatch (COSMETIC)

Line 25 of `RegionStatistics.h` reads: `// 7 total classes (v2.0 taxonomy)`. The actual count is 8 (the `Count` enumerator equals 8, and the Vignette class was added in v2.2).

**Impact**: None (cosmetic only, the code is correct).

**Recommendation**: Update the comment to `// 8 total classes (v2.2 taxonomy)`.

---

## Recommendations

### Recommendation 1: Add Vignette Stretch Rules

Create a `GetVignetteRules()` function in `DefaultRules` namespace:

- Default: MTF midtones=0.22 (similar to background)
- Low SNR: Gentle MTF midtones=0.18 to avoid amplifying noise in dimmer corners
- High SNR: SAS (matching background behavior for consistency)

This aligns the stretch behavior with the already-correct pixel selection behavior and eliminates the low-confidence fallback.

### Recommendation 2: Consider Adaptive Upward Bias for BrightExtended

The +0.3 upward bias in the BrightExtended probability-weighted selection is static. For very faint extended emission (IFN, outer spiral arms), this bias may not be aggressive enough, while for bright emission cores (M42 Trapezium region), it may be too aggressive (risking selection of noise peaks near saturation).

Consider making the bias adaptive based on the distribution mean:
- `mu < 0.1` (faint extended): bias +0.4 to better extract faint signal
- `mu > 0.5` (bright extended): bias +0.15 to avoid saturation risk
- Otherwise: current +0.3

### Recommendation 3: Frame Weight Interaction with Maximum Selection

For BrightCompact and Vignette classes, `SelectBestValue` selects the maximum of `value * frameWeight`. When frame weights vary significantly (e.g., a high-weight frame with slightly lower signal vs. a low-weight frame with higher signal), the weighting could override the astrophysical intent of "select the brightest actual measurement."

Consider a two-stage approach for maximum-selection classes:
1. First filter: reject frames below a minimum weight threshold
2. Then select maximum value from the remaining frames (ignoring weight in the selection itself)

This preserves the intent of maximum selection while still excluding poor-quality frames.

### Recommendation 4: DarkExtended Minimum Selection and Noise Floor

Selecting the minimum value for dark features is correct in principle but carries a risk: in very noisy data (low integration time, few frames), the minimum valid value may be a noise trough rather than a genuine dark absorption feature. The current sigma clipping mitigates this, but a secondary guard could be added: if the selected minimum is more than 2-sigma below the median of valid values, fall back to median selection for that pixel.

### Recommendation 5: StarHalo Gradient Consistency Check

Star halos have a defining characteristic: smooth radial decline from the stellar core. The current symmetric outlier rejection and mild upward bias treat each pixel independently. A potential improvement would be to incorporate the spatial context more strongly for StarHalo pixels: if a pixel's selected value would create a non-monotonic radial profile (brighter than a pixel closer to the star core), it could be flagged for smoothing. This would require spatial awareness beyond the current 8-neighbor context.

---

## Conclusion

All 8 classes in the v2.2 taxonomy have astrophysically appropriate pixel selection strategies implemented across the three-component pipeline. The system correctly handles the two most critical astrophotographic challenges:

1. **Dark feature preservation** (DarkExtended): High-only rejection + minimum selection ensures dark nebulae and dust lanes remain dark. **VERIFIED.**
2. **Signal preservation** (BrightExtended, BrightCompact): Low-only rejection + maximum/upward-biased selection ensures nebular and stellar signal is not suppressed. **VERIFIED.**

The one concrete gap (missing Vignette stretch rules) has minimal practical impact and is straightforward to address.

**Audit Result: PASS**

---

*Generated by ASTRO-SME validation system, 2026-02-19*
