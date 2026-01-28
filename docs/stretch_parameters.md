# NukeX Stretch Parameters Reference

## Overview

This document defines the optimal stretch parameters for each of the 21 segmentation classes in NukeX. The parameters are designed to:

1. **Preserve** the unique characteristics of each astronomical feature
2. **Enhance** visibility while avoiding artifacts
3. **Enable** seamless blending when composing the final image

The stretch functions available are:
- **ArcSinh**: Inverse hyperbolic sine - excellent for high dynamic range compression
- **GHS**: Generalized Hyperbolic Stretch - fine-grained tonal control
- **MTF**: Midtones Transfer Function - predictable, balanced stretching
- **Linear**: Simple rescaling with optional gamma

---

## Class-by-Class Stretch Parameters

### Class 0: Background

**Recommended Stretch**: MTF

| Parameter | Value | Range | Description |
|-----------|-------|-------|-------------|
| midtone | 0.15-0.25 | 0.01-0.99 | Where the median will map to ~0.5 |
| black_point | 0.0 | 0.0-0.1 | Clip shadows at this level |
| white_point | 1.0 | 0.9-1.0 | Clip highlights at this level |

**Visual Goals**:
- Smooth, even appearance without banding
- Subtle gradient preservation for aesthetic sky
- Noise suppression without losing faint signal

**SNR-Dependent Tuning**:
| SNR | midtone | Rationale |
|-----|---------|-----------|
| < 8 | 0.20-0.25 | Conservative to avoid noise amplification |
| 8-15 | 0.18-0.22 | Moderate stretch |
| > 15 | 0.15-0.18 | Can push harder with clean data |

**Common Problems**:
- **Too aggressive (midtone < 0.1)**: Background becomes washed out, gradient artifacts appear
- **Too conservative (midtone > 0.4)**: Image appears dark, faint features lost
- **Black point too high**: Lose subtle gradient information

---

### Class 1: Star (Bright)

**Recommended Stretch**: ArcSinh

| Parameter | Value | Range | Description |
|-----------|-------|-------|-------------|
| scale | 0.05 | 0.02-0.15 | Lower = more aggressive compression |
| black_point | 0.0 | 0.0 | Do not clip star shadows |

**Visual Goals**:
- Prevent core blowout while maintaining star shape
- Preserve color information in the core
- Natural transition to surrounding halo

**Brightness-Dependent Tuning**:
| Star Peak | scale | Effect |
|-----------|-------|--------|
| > 0.95 | 0.02-0.03 | Maximum compression for near-saturated |
| 0.8-0.95 | 0.04-0.06 | Strong compression |
| 0.6-0.8 | 0.06-0.10 | Moderate compression |

**Common Problems**:
- **Scale too low (< 0.02)**: Stars appear flat, unnatural "donut" effect
- **Scale too high (> 0.2)**: Insufficient compression, core blows out
- **Wrong function (using GHS)**: Can create harsh transitions in bright cores

---

### Class 2: Star (Medium)

**Recommended Stretch**: GHS

| Parameter | Value | Range | Description |
|-----------|-------|-------|-------------|
| D (stretch factor) | 0.3-0.4 | 0.1-1.0 | Overall stretch amount |
| b (symmetry) | 0.0 | -0.5-0.5 | Balance (0 = symmetric) |
| HP (highlight protection) | 0.80-0.90 | 0.5-0.98 | Protect bright values |
| SP (shadow protection) | 0.0 | 0.0-0.3 | Protect dark values |
| LP (local point) | 0.0 | 0.0-0.5 | Focus point for stretch |

**Visual Goals**:
- Enhance star visibility without creating halos
- Maintain color saturation
- Smooth gradient from core to background

**Common Problems**:
- **D too high**: Creates artificial "glow" effect
- **HP too low**: Core becomes blown out
- **Symmetry bias (b != 0)**: Creates uneven star profiles

---

### Class 3: Star (Faint)

**Recommended Stretch**: GHS

| Parameter | Value | Range | Description |
|-----------|-------|-------|-------------|
| D | 0.5-0.7 | 0.3-1.5 | More stretch to reveal faint stars |
| b | 0.0 | -0.2-0.2 | Keep symmetric |
| HP | 0.70-0.85 | 0.5-0.95 | Less protection needed |
| SP | 0.0 | 0.0-0.1 | No shadow protection |

**Visual Goals**:
- Reveal faint stars without noise amplification
- Maintain proper star shape (not bloated)
- Natural appearance integrated with background

**SNR-Dependent Tuning**:
| SNR | D | HP | Notes |
|-----|---|----|----|
| < 5 | 0.4-0.5 | 0.85 | Conservative to avoid noise |
| 5-10 | 0.5-0.6 | 0.75 | Moderate enhancement |
| > 10 | 0.6-0.8 | 0.70 | Can push harder |

**Common Problems**:
- **D too high with low SNR**: Noise amplification, false "stars" appear
- **Not enough stretch**: Faint stars invisible or barely visible
- **Using ArcSinh**: Compresses faint signal too much

---

### Class 4: Star (Saturated)

**Recommended Stretch**: ArcSinh

| Parameter | Value | Range | Description |
|-----------|-------|-------|-------------|
| scale | 0.02 | 0.01-0.05 | Very aggressive compression |
| normalize | true | - | Ensure output in 0-1 |

**Visual Goals**:
- Recover any possible detail in bloomed cores
- Prevent pure white blobs
- Maintain natural appearance despite saturation

**Special Considerations**:
- Saturated stars often have no recoverable core detail
- Focus on smooth transition to unsaturated regions
- Consider this class more for "damage control" than enhancement

**Common Problems**:
- **Any stretch too strong**: Creates obvious "rings" around saturated area
- **Hard boundary with halo class**: Visible seam

---

### Class 5: Nebula (Emission)

**Recommended Stretch**: GHS

| Parameter | Value | Range | Description |
|-----------|-------|-------|-------------|
| D | 1.0-1.5 | 0.5-2.0 | Moderate to strong stretch |
| b | 0.05-0.15 | 0.0-0.3 | Slight bias toward brighter features |
| HP | 0.80-0.90 | 0.7-0.95 | Protect bright emission cores |
| SP | 0.0 | 0.0-0.1 | No shadow protection |
| LP | 0.0 | 0.0-0.2 | Optional: set to median for local contrast |

**Visual Goals**:
- Reveal faint emission structures
- Preserve bright emission knots without clipping
- Maintain color balance (especially H-alpha red)
- Show filamentary detail

**Target-Specific Tuning**:
| Target Type | D | b | HP | Notes |
|-------------|---|---|----|----|
| Bright (M42 core) | 0.8 | 0.05 | 0.90 | Protect bright core |
| Medium (M16) | 1.2 | 0.10 | 0.85 | Balanced |
| Faint (Sh2-xxx) | 1.5 | 0.15 | 0.75 | Push harder |
| Extremely faint | 2.0 | 0.20 | 0.70 | Maximum enhancement |

**Common Problems**:
- **D too high**: Emission becomes washed out, loses contrast
- **HP too low**: Bright cores (like Trapezium region) blow out
- **Wrong color treatment**: RGB not stretched identically loses Ha signature

---

### Class 6: Nebula (Reflection)

**Recommended Stretch**: GHS

| Parameter | Value | Range | Description |
|-----------|-------|-------|-------------|
| D | 0.7-1.0 | 0.4-1.2 | Gentler than emission |
| b | 0.0-0.05 | 0.0-0.1 | Minimal bias |
| HP | 0.85-0.95 | 0.8-0.98 | Strong highlight protection |
| SP | 0.0 | 0.0 | No shadow protection |

**Visual Goals**:
- Preserve blue color (scattered starlight)
- Maintain smooth gradients typical of reflection nebulae
- Avoid creating false detail from noise

**Special Considerations**:
- Reflection nebulae are inherently lower contrast than emission
- Blue channel typically weakest - avoid over-stretching
- Often adjacent to bright stars causing halos

**Common Problems**:
- **D too high**: Creates mottled, noisy appearance
- **Different per-channel stretch**: Destroys characteristic blue color
- **Overcompensating for blue**: Creates unnatural cyan cast

---

### Class 7: Nebula (Dark)

**Recommended Stretch**: Linear (or minimal stretch)

| Parameter | Value | Range | Description |
|-----------|-------|-------|-------------|
| black_point | 0.0 | 0.0 | CRITICAL: Never clip shadows |
| white_point | 0.8-1.0 | 0.5-1.0 | May limit highlights |
| gamma | 1.0 | 0.8-1.2 | Keep near linear |

**Visual Goals**:
- **PRESERVE DARKNESS** - This is the defining characteristic
- Show subtle internal structure without lifting overall level
- Maintain sharp boundary contrast with background/emission

**Critical Rules**:
1. NEVER use aggressive stretches (GHS D > 0.3, ArcSinh)
2. NEVER apply shadow protection or black point adjustment
3. Dark nebulae ARE dark - resist the urge to "reveal" them

**Common Problems**:
- **Any aggressive stretch**: Destroys dark nebula - appears as muddy gray
- **Black point > 0**: Clips the very structure you're trying to preserve
- **Treating as "low signal"**: Dark nebulae are not faint, they're DARK

---

### Class 8: Nebula (Planetary)

**Recommended Stretch**: GHS or ArcSinh (depending on morphology)

| Parameter | Value | Range | Description |
|-----------|-------|-------|-------------|
| **GHS** | | | |
| D | 0.8-1.2 | 0.5-1.5 | Moderate stretch |
| b | 0.1-0.2 | 0.0-0.3 | Slight brightness bias for shell |
| HP | 0.85-0.95 | 0.8-0.98 | Protect central star |
| **ArcSinh** | | | |
| scale | 0.06-0.12 | 0.04-0.2 | Moderate compression |

**Visual Goals**:
- Reveal shell structure
- Handle extreme brightness of central star
- Show OIII/Ha color variations

**Morphology-Based Selection**:
| Type | Function | Rationale |
|------|----------|-----------|
| Ring (M57) | GHS | Need fine shell detail |
| Bipolar (M27) | GHS | Complex structure needs tonal control |
| Compact (NGC 7027) | ArcSinh | Very bright, needs compression |
| Diffuse | GHS | Low contrast needs enhancement |

**Common Problems**:
- **Central star blowout**: Use ArcSinh or high HP
- **Shell lost in noise**: D too low or SNR insufficient
- **Color banding**: Insufficient bit depth in processing

---

### Class 9: Galaxy (Spiral)

**Recommended Stretch**: GHS or Lumpton

| Parameter | Value | Range | Description |
|-----------|-------|-------|-------------|
| **GHS** | | | |
| D | 0.8-1.2 | 0.5-1.5 | Balanced stretch |
| b | 0.05-0.15 | 0.0-0.2 | Bias toward arm structures |
| HP | 0.85-0.92 | 0.75-0.95 | Protect bright core |
| **Lumpton** | | | |
| Q | 8-12 | 5-20 | Overall stretch strength |
| alpha | varies | - | Per-channel scaling |

**Visual Goals**:
- Reveal spiral arm structure
- Maintain core-to-arm brightness relationship
- Preserve dust lane visibility
- Show HII regions and star-forming areas

**Galaxy-Specific Considerations**:
| Orientation | D | HP | Notes |
|-------------|---|----|----|
| Face-on (M51) | 1.0-1.2 | 0.88 | Show arm structure |
| Edge-on (NGC 891) | 0.9-1.0 | 0.92 | Preserve dust lane |
| Inclined (M31) | 0.8-1.0 | 0.90 | Balanced |

**Common Problems**:
- **Core-arm separation too extreme**: Creates "bulls-eye" effect
- **Arms washed out**: D too low, not enough stretch
- **Core blowout**: HP too low, especially for Seyfert/AGN cores

---

### Class 10: Galaxy (Elliptical)

**Recommended Stretch**: GHS

| Parameter | Value | Range | Description |
|-----------|-------|-------|-------------|
| D | 0.7-1.0 | 0.4-1.2 | Gentler stretch than spiral |
| b | 0.0-0.05 | 0.0-0.1 | Keep symmetric |
| HP | 0.88-0.95 | 0.8-0.98 | Protect bright core |
| SP | 0.0 | 0.0 | No shadow protection |

**Visual Goals**:
- Smooth, gradient appearance
- Reveal subtle structural features (shells, dust)
- Natural falloff from core to halo

**Special Considerations**:
- Ellipticals have less internal structure than spirals
- Focus on smooth gradients, not detail enhancement
- Often have subtle features (shells, jets) at very faint levels

**Common Problems**:
- **Over-stretching**: Creates artificial banding/rings
- **Noise amplification**: Elliptical halos are inherently smooth
- **False structure**: Noise interpreted as features

---

### Class 11: Galaxy (Irregular)

**Recommended Stretch**: GHS

| Parameter | Value | Range | Description |
|-----------|-------|-------|-------------|
| D | 0.9-1.3 | 0.6-1.5 | Moderate to strong |
| b | 0.05-0.15 | 0.0-0.2 | Slight bias for bright regions |
| HP | 0.82-0.90 | 0.7-0.95 | Moderate protection |

**Visual Goals**:
- Reveal chaotic structure
- Show star-forming regions (often bright blue)
- Maintain color variations

**Special Considerations**:
- Irregular galaxies often contain very bright HII regions
- Wide dynamic range typical
- Color information is crucial for structure

**Common Problems**:
- **HII blowout**: HP too low for bright star-forming regions
- **Structure lost**: D too low for faint outer regions
- **Color desaturation**: Non-uniform per-channel stretch

---

### Class 12: Galaxy Core

**Recommended Stretch**: ArcSinh

| Parameter | Value | Range | Description |
|-----------|-------|-------|-------------|
| scale | 0.05-0.10 | 0.02-0.2 | Strong compression |

**Visual Goals**:
- Prevent core blowout
- Maintain smooth transition to disk/halo
- Preserve any detectable core structure (jets, dust)

**Core-Type Considerations**:
| Core Type | scale | Notes |
|-----------|-------|-------|
| Normal | 0.08-0.12 | Moderate compression |
| AGN/Seyfert | 0.03-0.05 | Very bright, strong compression |
| With dust lane | 0.10-0.15 | Less compression to show lane |

**Common Problems**:
- **Core blowout**: scale too high
- **Transition artifacts**: Poor blending with galaxy arm class
- **Over-compression**: Creates flat, unnatural core appearance

---

### Class 13: Dust Lane

**Recommended Stretch**: Linear (with histogram limits)

| Parameter | Value | Range | Description |
|-----------|-------|-------|-------------|
| black_point | 0.0 | 0.0 | NEVER clip shadows |
| white_point | 0.6-0.9 | 0.5-1.0 | May limit to show contrast |
| gamma | 1.0 | 0.9-1.1 | Keep near linear |

**Visual Goals**:
- **PRESERVE** the darkness that defines dust lanes
- Show subtle internal structure
- Maintain crisp edges against bright background

**Critical Rules** (Same philosophy as Dark Nebula):
1. Dust lanes are DARK by nature - do not "reveal" them
2. Never apply aggressive stretches
3. Focus on preserving contrast, not enhancing brightness

**Common Problems**:
- **Any aggressive stretch**: Dust lane becomes gray, loses definition
- **Boundary artifacts**: Poor transition to galaxy arm/core
- **Treating as "missing signal"**: Dust is obscuring, not absent

---

### Class 14: Star Cluster (Open)

**Recommended Stretch**: ArcSinh or GHS

| Parameter | Value | Range | Description |
|-----------|-------|-------|-------------|
| **ArcSinh** | | | |
| scale | 0.10-0.15 | 0.05-0.2 | Moderate compression |
| **GHS** | | | |
| D | 0.4-0.6 | 0.2-0.8 | Gentle stretch |
| HP | 0.80-0.90 | 0.7-0.95 | Protect bright members |

**Visual Goals**:
- Individual stars should be visible and separated
- Color variation (stellar types) preserved
- Background integration should be natural

**Cluster-Type Considerations**:
| Cluster Type | Function | Notes |
|--------------|----------|-------|
| Bright (M45) | ArcSinh (0.08) | Compress bright members |
| Dense (M11) | ArcSinh (0.12) | Many stars, need separation |
| Sparse | GHS (D=0.5) | Fewer constraints |

**Common Problems**:
- **Stars merge**: Insufficient resolution or over-stretch
- **Bright members dominate**: scale/HP not protective enough
- **Background contamination**: Poor segmentation includes field stars

---

### Class 15: Star Cluster (Globular)

**Recommended Stretch**: ArcSinh

| Parameter | Value | Range | Description |
|-----------|-------|-------|-------------|
| scale | 0.06-0.10 | 0.03-0.15 | Strong compression for dense core |

**Visual Goals**:
- Resolve individual stars where possible
- Core compression without losing structure
- Smooth transition from dense core to resolved halo

**Morphology Considerations**:
| Region | Approach |
|--------|----------|
| Core | Maximum compression (scale ~ 0.05) |
| Half-light radius | Moderate compression (scale ~ 0.10) |
| Outer halo | Gentle stretch (can use GHS) |

**Note**: Globular clusters benefit from adaptive approaches where the stretch varies with distance from center. In practice, the blending system handles this automatically when proper masks are generated.

**Common Problems**:
- **Core blowout**: Insufficient compression
- **Over-compression**: Core becomes flat, loses structure
- **Halo lost**: Too much focus on core, outer stars invisible

---

### Classes 16-20: Artifacts

#### Class 16: Hot Pixel

**Recommended Stretch**: Linear (suppression)

| Parameter | Value | Description |
|-----------|-------|-------------|
| white_point | 0.3-0.5 | Suppress bright values |
| black_point | 0.0 | Keep minimum |

**Goal**: Minimize visibility; these should ideally be removed, not stretched.

---

#### Class 17: Satellite Trail

**Recommended Stretch**: Linear (suppression)

| Parameter | Value | Description |
|-----------|-------|-------------|
| white_point | 0.2-0.4 | Strong suppression |
| black_point | 0.0 | Keep minimum |

**Goal**: Hide the trail as much as possible.

---

#### Class 18: Diffraction Spike

**Recommended Stretch**: ArcSinh

| Parameter | Value | Description |
|-----------|-------|-------------|
| scale | 0.12-0.18 | Mild compression |

**Goal**: Reduce prominence while maintaining natural appearance. Some prefer to keep spikes for aesthetic reasons.

---

#### Class 19: Gradient

**Recommended Stretch**: MTF

| Parameter | Value | Description |
|-----------|-------|-------------|
| midtone | 0.25-0.35 | Conservative stretch |

**Goal**: Smooth out gradient while not creating banding. Ideally, gradients should be removed in calibration.

---

#### Class 20: Noise

**Recommended Stretch**: MTF

| Parameter | Value | Description |
|-----------|-------|-------------|
| midtone | 0.20-0.30 | Conservative |

**Goal**: Avoid amplifying noise patterns; blend naturally with background.

---

## Decision Tree for Stretch Selection

```
START: Given class and image statistics
  |
  +-- Is class an ARTIFACT (16-20)?
  |     YES -> Use Linear/MTF suppression
  |     NO  -> Continue
  |
  +-- Is class a DARK FEATURE (7: nebula_dark, 13: dust_lane)?
  |     YES -> Use Linear with gamma=1.0, NO shadow clipping
  |     NO  -> Continue
  |
  +-- Is class a BRIGHT POINT SOURCE (1: star_bright, 4: star_saturated, 12: galaxy_core)?
  |     YES -> Use ArcSinh
  |     |      +-- Peak > 0.9? -> scale = 0.02-0.05
  |     |      +-- Peak > 0.7? -> scale = 0.05-0.10
  |     |      +-- else       -> scale = 0.10-0.15
  |     NO  -> Continue
  |
  +-- Is SNR < 5?
  |     YES -> Use conservative stretch (MTF or GHS with low D)
  |     NO  -> Continue
  |
  +-- Is dynamic range > 3.0 (log)?
  |     YES -> Consider ArcSinh for compression
  |     NO  -> Continue
  |
  +-- Default: Use GHS with parameters based on class table
        +-- Compute D based on median brightness (inverse relationship)
        +-- Compute HP based on p95 percentile
        +-- Set b based on class characteristics
```

### SNR-Based Parameter Adjustments

| SNR Range | Stretch Adjustment |
|-----------|-------------------|
| < 5 | Reduce D by 30%, increase HP by 10% |
| 5-10 | Standard parameters |
| 10-20 | Can increase D by 10-20% |
| > 20 | Full flexibility, consider RNC for color |

### Dynamic Range Adjustments

| Dynamic Range | Adjustment |
|---------------|------------|
| < 1.5 | Low contrast; may need increased D |
| 1.5-3.0 | Normal range; use standard parameters |
| > 3.0 | Consider ArcSinh or split processing |

---

## Blending Considerations

### Feather/Blur Widths

The `blend_sigma` parameter controls the Gaussian blur applied to hard masks before blending.

| Boundary Type | Recommended Sigma | Notes |
|---------------|-------------------|-------|
| Background <-> Nebula | 3.0-5.0 | Wider for smooth transition |
| Background <-> Star | 2.0-3.0 | Tighter to preserve star shape |
| Star <-> Nebula | 2.0-4.0 | Medium; prevents halos bleeding |
| Nebula (emission) <-> Nebula (dark) | 3.0-5.0 | Smooth boundary important |
| Galaxy core <-> Galaxy arm | 4.0-6.0 | Wider to prevent "bullseye" |
| Dust lane <-> Galaxy | 2.0-3.0 | Tighter to preserve sharp edges |
| Any <-> Artifact | 1.0-2.0 | Minimize artifact spread |

### Composition Order

The order in which layers are composed affects the final result. General principle: **background first, stars last**.

**Recommended Order**:
1. **Background** (class 0) - Base layer
2. **Nebula Dark** (class 7) - Over background
3. **Dust Lane** (class 13) - Dark features early
4. **Nebula Emission/Reflection/Planetary** (classes 5, 6, 8)
5. **Galaxy components** (classes 9-11)
6. **Galaxy Core** (class 12) - Needs to sit on top of galaxy body
7. **Star Clusters** (classes 14, 15)
8. **Faint Stars** (class 3)
9. **Medium Stars** (class 2)
10. **Bright Stars** (class 1)
11. **Saturated Stars** (class 4) - Last, highest priority

**Rationale**:
- Stars should NEVER be occluded by nebular processing
- Dark features need to stay dark (early composition prevents stretching artifacts)
- Bright features need their protection applied after fainter features

### Problem Boundaries to Watch

| Boundary | Potential Issue | Mitigation |
|----------|-----------------|------------|
| Star halo overlapping nebula | Blue halo contaminates red Ha | Larger sigma, star-first composition |
| Galaxy core -> spiral arm | Discontinuity in brightness | Larger sigma (5-6), gradual HP transition |
| Emission -> dark nebula | Loss of dark nebula contrast | Ensure dark nebula is LINEAR stretch |
| Any class -> artifact | Artifact spreading | Tight sigma (1-2), artifact last |
| Background -> any feature | Background "eating into" feature | Ensure feature mask is slightly dilated |

---

## Parameter Quick Reference

### All 21 Classes at a Glance

| ID | Class | Function | Key Parameters |
|----|-------|----------|----------------|
| 0 | background | MTF | midtone=0.15-0.25 |
| 1 | star_bright | ArcSinh | scale=0.05 |
| 2 | star_medium | GHS | D=0.3-0.4, HP=0.80-0.90 |
| 3 | star_faint | GHS | D=0.5-0.7, HP=0.70-0.85 |
| 4 | star_saturated | ArcSinh | scale=0.02 |
| 5 | nebula_emission | GHS | D=1.0-1.5, b=0.1, HP=0.80-0.90 |
| 6 | nebula_reflection | GHS | D=0.7-1.0, HP=0.85-0.95 |
| 7 | nebula_dark | Linear | gamma=1.0, NO clipping |
| 8 | nebula_planetary | GHS | D=0.8-1.2, HP=0.85-0.95 |
| 9 | galaxy_spiral | GHS | D=0.8-1.2, b=0.1, HP=0.85-0.92 |
| 10 | galaxy_elliptical | GHS | D=0.7-1.0, HP=0.88-0.95 |
| 11 | galaxy_irregular | GHS | D=0.9-1.3, HP=0.82-0.90 |
| 12 | galaxy_core | ArcSinh | scale=0.05-0.10 |
| 13 | dust_lane | Linear | gamma=1.0, NO clipping |
| 14 | star_cluster_open | ArcSinh | scale=0.10-0.15 |
| 15 | star_cluster_globular | ArcSinh | scale=0.06-0.10 |
| 16 | artifact_hot_pixel | Linear | white_point=0.3-0.5 |
| 17 | artifact_satellite | Linear | white_point=0.2-0.4 |
| 18 | artifact_diffraction | ArcSinh | scale=0.12-0.18 |
| 19 | artifact_gradient | MTF | midtone=0.25-0.35 |
| 20 | artifact_noise | MTF | midtone=0.20-0.30 |

---

## Implementation Notes

### Python Reference (compose_segments.py)

```python
STRETCH_CONFIG = {
    0: ('mtf', {'midtone': 0.15}),
    1: ('arcsinh', {'scale': 0.05}),
    2: ('ghs', {'D': 0.35, 'HP': 0.85}),
    3: ('ghs', {'D': 0.6, 'HP': 0.75}),
    4: ('arcsinh', {'scale': 0.02}),
    5: ('ghs', {'D': 1.2, 'b': 0.1, 'HP': 0.85}),
    6: ('ghs', {'D': 0.8, 'HP': 0.9}),
    7: ('linear', {'gamma': 1.0}),
    8: ('ghs', {'D': 1.0, 'HP': 0.9}),
    9: ('ghs', {'D': 1.0, 'b': 0.1, 'HP': 0.88}),
    10: ('ghs', {'D': 0.85, 'HP': 0.92}),
    11: ('ghs', {'D': 1.1, 'HP': 0.85}),
    12: ('arcsinh', {'scale': 0.08}),
    13: ('linear', {'gamma': 1.0}),
    14: ('arcsinh', {'scale': 0.12}),
    15: ('arcsinh', {'scale': 0.08}),
    16: ('linear', {'white_point': 0.4}),
    17: ('linear', {'white_point': 0.3}),
    18: ('arcsinh', {'scale': 0.15}),
    19: ('mtf', {'midtone': 0.3}),
    20: ('mtf', {'midtone': 0.25}),
}
```

### C++ Reference (SelectionRules.cpp)

The C++ implementation uses the same fundamental parameters but with dynamic adjustment based on RegionStatistics. See `ParameterTuning` namespace in `SelectionRules.cpp` for the adaptive algorithms.

---

## Revision History

| Date | Version | Changes |
|------|---------|---------|
| 2026-01-27 | 1.0 | Initial documentation |

---

*This document is the authoritative reference for NukeX stretch parameter tuning. Parameters may be adjusted based on v8 training results and user feedback.*
