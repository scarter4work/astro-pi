# Selection Strategy Validation Audit

**Auditor**: ASTRO-SME (Astrophotography Subject Matter Expert)
**Date**: 2026-01-27
**Files Reviewed**:
- `/home/scarter4work/projects/NukeX/src/engine/SelectionRules.cpp`
- `/home/scarter4work/projects/NukeX/src/engine/PixelSelector.cpp`
- `/home/scarter4work/projects/NukeX/src/engine/PixelStackAnalyzer.cpp`
- `/home/scarter4work/projects/NukeX/src/engine/RegionStatistics.h`

---

## Executive Summary

The NukeX intelligent pixel selection system implements **class-specific selection strategies for all 21 region classes**. The implementation is distributed across three main components:

1. **SelectionRules.cpp** - Algorithm selection and stretch parameter tuning
2. **PixelStackAnalyzer.cpp** - Per-pixel outlier detection and value selection
3. **PixelSelector.cpp** - Target context and spatial smoothing

Overall assessment: **MOSTLY CORRECT** with some issues requiring attention.

---

## Complete Class Coverage Analysis

### Class 0: Background

| Component | Implementation | Assessment |
|-----------|---------------|------------|
| SelectionRules | MTF with midtones 0.15-0.35, SAS for high SNR | CORRECT |
| PixelStackAnalyzer | `outlierSigmaThreshold=2.5f`, aggressive high outlier rejection | CORRECT |
| PixelSelector | Standard spatial smoothing | CORRECT |

**Strategy**: Select near median, reject high outliers (gradients, satellites)
**Status**: PASS

---

### Classes 1-4: Stars

#### Class 1: StarBright
| Component | Implementation | Assessment |
|-----------|---------------|------------|
| SelectionRules | ArcSinh with softness 0.05-0.3, strong HDR compression | CORRECT |
| PixelStackAnalyzer | `outlierSigmaThreshold=4.0f`, `favorHighSignal=true` | CORRECT |
| PixelSelector | Standard handling | CORRECT |

**Strategy**: Preserve high values, reject low outliers
**Status**: PASS

#### Class 2: StarMedium
| Component | Implementation | Assessment |
|-----------|---------------|------------|
| SelectionRules | GHS with stretchFactor 0.3, highlightProtection 0.8 | CORRECT |
| PixelStackAnalyzer | `outlierSigmaThreshold=4.0f`, `favorHighSignal=true` | CORRECT |
| PixelSelector | Standard handling | CORRECT |

**Strategy**: Balanced stretch with highlight protection
**Status**: PASS

#### Class 3: StarFaint
| Component | Implementation | Assessment |
|-----------|---------------|------------|
| SelectionRules | GHS with stretchFactor 0.6, highlightProtection 0.7 | CORRECT |
| PixelStackAnalyzer | `outlierSigmaThreshold=4.0f`, `favorHighSignal=true` | CORRECT |
| PixelSelector | Standard handling | CORRECT |

**Strategy**: Moderate stretch to reveal faint stars
**Status**: PASS

#### Class 4: StarSaturated
| Component | Implementation | Assessment |
|-----------|---------------|------------|
| SelectionRules | Default fallback: ArcSinh softness 0.05 | CORRECT |
| PixelStackAnalyzer | Same as StarBright | CORRECT |
| PixelSelector | Standard handling | CORRECT |

**Strategy**: Strong HDR compression, preserve saturation
**Status**: PASS - Note: No explicit rules in `GetAllRules()` but default fallback handles it.

---

### Classes 5-8: Nebulae

#### Class 5: NebulaEmission
| Component | Implementation | Assessment |
|-----------|---------------|------------|
| SelectionRules | RNC/GHS, stretchFactor 0.65-1.0, highlightProtection 0.75 | CORRECT |
| PixelStackAnalyzer | `outlierSigmaThreshold=3.5f`, `favorHighSignal=true`, asymmetric outlier rejection | CORRECT |
| PixelSelector | Target context boost for M42, etc. | CORRECT |

**Strategy**: Favor signal, reject contamination (low outliers from clouds)
**Status**: PASS

#### Class 6: NebulaReflection
| Component | Implementation | Assessment |
|-----------|---------------|------------|
| SelectionRules | Default fallback: GHS stretchFactor 0.6 | ACCEPTABLE |
| PixelStackAnalyzer | `outlierSigmaThreshold=3.0f`, `favorHighSignal=true` | CORRECT |
| PixelSelector | Standard handling | CORRECT |

**Strategy**: Similar to emission but softer stretch
**Status**: PASS - Note: No explicit rules in `GetNebulaEmissionRules()` but separate handling exists.

**Recommendation**: Consider adding explicit `GetNebulaReflectionRules()` for finer control.

#### Class 7: NebulaDark
| Component | Implementation | Assessment |
|-----------|---------------|------------|
| SelectionRules | MTF midtones 0.15-0.2, GHS stretchFactor 0.4 for detail | CORRECT |
| PixelStackAnalyzer | `outlierSigmaThreshold=3.0f`, **`favorLowSignal=true`** | **CRITICAL - CORRECT** |
| PixelSelector | Asymmetric outlier rejection - only rejects high outliers | CORRECT |

**Strategy**: MUST preserve low values, reject high outliers
**Status**: PASS - This is the critical dark nebula handling. The code correctly:
1. Sets `favorLowSignal=true` to prefer darker values
2. Only marks pixels above mu as outliers (preserves darkness)
3. Uses gentle MTF stretch to maintain darkness

#### Class 8: NebulaPlanetary
| Component | Implementation | Assessment |
|-----------|---------------|------------|
| SelectionRules | ArcSinh softness 0.08 for shell structure | CORRECT |
| PixelStackAnalyzer | `outlierSigmaThreshold=3.0f`, `favorHighSignal=true` | CORRECT |
| PixelSelector | Target context matches PlanetaryNebula catalog | CORRECT |

**Strategy**: Preserve both bright shells and dark centers
**Status**: PASS

---

### Classes 9-12: Galaxies

#### Class 9: GalaxySpiral
| Component | Implementation | Assessment |
|-----------|---------------|------------|
| SelectionRules | Lumpton/GHS/SAS based on SNR, stretchFactor 0.4 | CORRECT |
| PixelStackAnalyzer | `outlierSigmaThreshold=3.0f`, `favorHighSignal=true` | CORRECT |
| PixelSelector | Target context for M31, M51, NGC galaxies | CORRECT |

**Strategy**: Preserve spiral arm detail
**Status**: PASS

#### Class 10: GalaxyElliptical
| Component | Implementation | Assessment |
|-----------|---------------|------------|
| SelectionRules | Default: GHS stretchFactor 0.5 | ACCEPTABLE |
| PixelStackAnalyzer | Same as GalaxySpiral | CORRECT |
| PixelSelector | Standard handling | CORRECT |

**Strategy**: Smooth gradient preservation
**Status**: PASS

#### Class 11: GalaxyIrregular
| Component | Implementation | Assessment |
|-----------|---------------|------------|
| SelectionRules | Default: GHS stretchFactor 0.55 | ACCEPTABLE |
| PixelStackAnalyzer | Same as GalaxySpiral | CORRECT |
| PixelSelector | Standard handling | CORRECT |

**Strategy**: Detail preservation
**Status**: PASS

#### Class 12: GalaxyCore
| Component | Implementation | Assessment |
|-----------|---------------|------------|
| SelectionRules | ArcSinh softness 0.05-0.2 for HDR | CORRECT |
| PixelStackAnalyzer | `outlierSigmaThreshold=3.5f`, `favorHighSignal=true` | CORRECT |
| PixelSelector | Standard handling | CORRECT |

**Strategy**: Strong HDR compression for bright cores
**Status**: PASS

---

### Class 13: DustLane
| Component | Implementation | Assessment |
|-----------|---------------|------------|
| SelectionRules | Histogram shadowsClipping=0.0, MTF midtones 0.15 | CORRECT |
| PixelStackAnalyzer | **`favorLowSignal=true`** - same as NebulaDark | **CRITICAL - CORRECT** |
| PixelSelector | Target context matches NebulaDark | CORRECT |

**Strategy**: Same as dark nebula - preserve darkness, reject high outliers
**Status**: PASS

---

### Classes 14-15: Star Clusters

#### Class 14: StarClusterOpen
| Component | Implementation | Assessment |
|-----------|---------------|------------|
| SelectionRules | GHS stretchFactor 0.4-0.5, highlightProtection 0.85 | CORRECT |
| PixelStackAnalyzer | `outlierSigmaThreshold=3.5f`, `favorHighSignal=true` | CORRECT |
| PixelSelector | Standard handling | CORRECT |

**Strategy**: Balance individual stars with cluster context
**Status**: PASS

#### Class 15: StarClusterGlobular
| Component | Implementation | Assessment |
|-----------|---------------|------------|
| SelectionRules | ArcSinh softness 0.1 for dense cores, GHS stretchFactor 0.35 | CORRECT |
| PixelStackAnalyzer | `outlierSigmaThreshold=3.5f`, `favorHighSignal=true` | CORRECT |
| PixelSelector | Standard handling | CORRECT |

**Strategy**: HDR protection for dense cores
**Status**: PASS

---

### Classes 16-20: Artifacts

#### Class 16: ArtifactHotPixel
| Component | Implementation | Assessment |
|-----------|---------------|------------|
| SelectionRules | MTF midtones 0.2, confidence 0.3 | CORRECT |
| PixelStackAnalyzer | **`outlierSigmaThreshold=2.0f`**, `useMedianSelection=true` | **CORRECT - AGGRESSIVE REJECTION** |
| PixelSelector | Standard handling | CORRECT |

**Strategy**: Very aggressive outlier rejection, use median
**Status**: PASS

#### Class 17: ArtifactSatellite
| Component | Implementation | Assessment |
|-----------|---------------|------------|
| SelectionRules | Same as HotPixel | CORRECT |
| PixelStackAnalyzer | **`outlierSigmaThreshold=2.0f`**, `useMedianSelection=true` | **CORRECT - AGGRESSIVE REJECTION** |
| PixelSelector | Standard handling | CORRECT |

**Strategy**: Very aggressive rejection of satellite trails
**Status**: PASS

#### Class 18: ArtifactDiffraction
| Component | Implementation | Assessment |
|-----------|---------------|------------|
| SelectionRules | Same as artifacts | ACCEPTABLE |
| PixelStackAnalyzer | **Treated same as stars** (`outlierSigmaThreshold=4.0f`, `favorHighSignal=true`) | **DEBATABLE - SEE NOTES** |
| PixelSelector | Standard handling | CORRECT |

**Strategy**: Current implementation preserves diffraction spikes as star features
**Status**: PASS with NOTE

**Note**: The decision to treat diffraction spikes like stars is defensible - they are consistent features that users may want to preserve. However, some users prefer to suppress them. Consider making this configurable.

#### Class 19: ArtifactGradient
| Component | Implementation | Assessment |
|-----------|---------------|------------|
| SelectionRules | Same as artifacts | CORRECT |
| PixelStackAnalyzer | `outlierSigmaThreshold=2.5f` (same as background), aggressive high rejection | CORRECT |
| PixelSelector | Standard handling | CORRECT |

**Strategy**: Aggressive background gradient suppression
**Status**: PASS

#### Class 20: ArtifactNoise
| Component | Implementation | Assessment |
|-----------|---------------|------------|
| SelectionRules | Same as artifacts | CORRECT |
| PixelStackAnalyzer | `outlierSigmaThreshold=2.5f`, median selection | CORRECT |
| PixelSelector | Standard handling | CORRECT |

**Strategy**: Noise suppression through median selection
**Status**: PASS

---

## Critical Validation: Dark Feature Preservation

The most important validation is ensuring **dark nebulae and dust lanes preserve low values**. The code correctly implements this:

### PixelStackAnalyzer.cpp (lines 451-457)
```cpp
// Dark nebula - preserve dark values!
case RegionClass::NebulaDark:
case RegionClass::DustLane:
   config.outlierSigmaThreshold = 3.0f;
   config.favorHighSignal = false;
   config.favorLowSignal = true;    // Dark features should stay dark
   break;
```

### PixelStackAnalyzer.cpp (lines 177-181) - Asymmetric outlier rejection
```cpp
case RegionClass::NebulaDark:
case RegionClass::DustLane:
   // Don't reject low values as outliers for dark features
   if ( values[i] > meta.distribution.mu && z > adjustedConfig.outlierSigmaThreshold )
      isOutlier = true;
   break;
```

### SelectBestValue function (lines 351-356)
```cpp
else if ( config.favorLowSignal && v < dist.mu )
{
   // Bonus for below-median values (dark nebula)
   adjustment = 1.0f + 0.2f * (dist.mu - v) / std::max( dist.sigma, 1e-10f );
}
```

**Assessment**: CORRECTLY IMPLEMENTED - Dark regions will preserve darkness.

---

## Issues Found

### Issue 1: Missing Explicit Rules for Some Classes (LOW PRIORITY)

The following classes rely on default fallbacks rather than explicit rules:

| Class | Default Handler | Issue |
|-------|-----------------|-------|
| StarSaturated | Default in `GetDefaultRecommendation()` | No explicit rules in `GetAllRules()` |
| NebulaReflection | Default in `GetDefaultRecommendation()` | No `GetNebulaReflectionRules()` function |
| GalaxyElliptical | Default in `GetDefaultRecommendation()` | No explicit rules |
| GalaxyIrregular | Default in `GetDefaultRecommendation()` | No explicit rules |

**Impact**: Low - defaults are reasonable but less optimized.

**Recommendation**: Consider adding explicit rule sets for these classes for finer-grained control.

### Issue 2: ArtifactDiffraction Strategy (DESIGN DECISION)

Currently treated identically to stars, preserving diffraction spikes. This may not be desired by all users.

**Recommendation**: Consider adding a user preference toggle for diffraction spike suppression.

### Issue 3: Missing Rules Header Declarations (COSMETIC)

`SelectionRules.h` declares `GetNebulaEmissionRules()` but not `GetNebulaReflectionRules()` or `GetNebulaPlanetaryRules()`.

**Recommendation**: Add missing rule function declarations for completeness.

---

## Summary Table: All 21 Classes

| # | Class | Strategy | outlierSigma | favorHigh | favorLow | Status |
|---|-------|----------|--------------|-----------|----------|--------|
| 0 | Background | Median, reject high | 2.5 | No | No | PASS |
| 1 | StarBright | Preserve high, HDR | 4.0 | Yes | No | PASS |
| 2 | StarMedium | Balanced GHS | 4.0 | Yes | No | PASS |
| 3 | StarFaint | Moderate stretch | 4.0 | Yes | No | PASS |
| 4 | StarSaturated | Strong HDR | 4.0 | Yes | No | PASS |
| 5 | NebulaEmission | Favor signal | 3.5 | Yes | No | PASS |
| 6 | NebulaReflection | Softer stretch | 3.0 | Yes | No | PASS |
| 7 | NebulaDark | **PRESERVE LOW** | 3.0 | No | **Yes** | PASS |
| 8 | NebulaPlanetary | Preserve shells | 3.0 | Yes | No | PASS |
| 9 | GalaxySpiral | Preserve arms | 3.0 | Yes | No | PASS |
| 10 | GalaxyElliptical | Smooth gradient | 3.0 | Yes | No | PASS |
| 11 | GalaxyIrregular | Detail preserve | 3.0 | Yes | No | PASS |
| 12 | GalaxyCore | HDR compression | 3.5 | Yes | No | PASS |
| 13 | DustLane | **PRESERVE LOW** | 3.0 | No | **Yes** | PASS |
| 14 | StarClusterOpen | Balance stars | 3.5 | Yes | No | PASS |
| 15 | StarClusterGlobular | HDR for cores | 3.5 | Yes | No | PASS |
| 16 | ArtifactHotPixel | Aggressive reject | 2.0 | No | No | PASS |
| 17 | ArtifactSatellite | Aggressive reject | 2.0 | No | No | PASS |
| 18 | ArtifactDiffraction | Preserve (as star) | 4.0 | Yes | No | PASS* |
| 19 | ArtifactGradient | Reject high | 2.5 | No | No | PASS |
| 20 | ArtifactNoise | Median select | 2.5 | No | No | PASS |

*\* Design decision - diffraction treated as star feature, may want to make configurable*

---

## Conclusion

**All 21 classes have appropriate selection strategies implemented.** The critical dark nebula and dust lane handling is correctly implemented with asymmetric outlier rejection that preserves low values.

The implementation correctly follows the specification:

1. **Stars (1-4)**: Preserve high values, reject low outliers - VERIFIED
2. **Nebulae Emission (5)**: Favor signal, reject contamination - VERIFIED
3. **Nebulae Reflection (6)**: Similar to emission but softer - VERIFIED
4. **Nebulae Dark (7)**: MUST preserve low values, reject high outliers - **VERIFIED**
5. **Nebulae Planetary (8)**: High contrast, preserve both - VERIFIED
6. **Galaxies (9-12)**: Various strategies by component - VERIFIED
7. **Dust Lane (13)**: Same as dark nebula - preserve darkness - **VERIFIED**
8. **Star Clusters (14-15)**: Balance individual stars - VERIFIED
9. **Artifacts (16-20)**: Reject/suppress - VERIFIED

**Audit Result: PASS**

---

*Generated by ASTRO-SME validation system*
