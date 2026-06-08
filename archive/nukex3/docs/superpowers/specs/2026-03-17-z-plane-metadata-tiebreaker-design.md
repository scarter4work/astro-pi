# Z-Plane Metadata Tiebreaker Design

**Date**: 2026-03-17
**Status**: Draft
**Author**: Scott Carter + Claude

## Problem

NukeX v3 has a `QualityWeights` system that computes per-frame scalar weights from FITS metadata (FWHM, eccentricity, sky background, HFR, altitude). These weights are threaded through the entire `PixelSelector` API but are **never used in any computation** — they are dead code. The `qualityWeights` parameter is accepted by `selectBestZ`, `processPixel`, `processImage`, and `processImageGPU` but never referenced.

Meanwhile, `ComputeFrameMetrics` (PSF fitting via `StarDetector` + `PSFFit`) appears to be failing silently on uncalibrated subs — all frames receive identical quality scores, producing equal weights of ~0.03.

Frame-level quality weights are also architecturally mismatched with NukeX's per-pixel statistical inference approach. A frame with bad seeing has soft stars but perfectly good background pixels. A frame with a satellite trail is bad for 0.1% of pixels but fine everywhere else. Per-pixel outlier rejection (MAD → ESD → distribution fitting → shortest-half mode) already handles these cases at the right granularity.

## Solution

Replace frame-level quality weights with **per-Z-plane metadata annotations** that act as a **tiebreaker** after statistical pixel selection. The statistics remain authoritative; metadata only disambiguates when multiple Z-values are statistically indistinguishable.

### Core Concept

1. Phase 3 runs the full statistical pipeline as today (MAD → ESD → distribution fitting → AICc → shortest-half mode)
2. Shortest-half mode selects a value and finds the closest frame
3. **New**: Compute the MAD of the shortest-half cluster. Scan for alternative frames whose values fall within 1× MAD of the selected value.
4. **New**: Among the selected frame and any alternatives within the tolerance band, pick the one with the highest precomputed `qualityScore` from metadata.
5. If all scores are equal or zero, behavior is identical to today (graceful degradation).

## Data Model Changes

### SubMetadata (modified)

Add one field to the existing `SubMetadata` struct in `SubCube.h`:

```cpp
struct SubMetadata {
    double fwhm = 0;
    double eccentricity = 0;
    double skyBackground = 0;
    double hfr = 0;
    double altitude = 0;
    double exposure = 0;
    double gain = 0;
    double ccdTemp = 0;
    double qualityScore = 0.0;  // composite tiebreaker (0 = unknown, higher = better)
    std::string object;
    std::string filter;
};
```

### Quality Score Computation

Precomputed once per frame during Phase 2:

```
qualityScore = 0.6 × (1 / (1 + fwhm)) + 0.4 × (1 / (1 + eccentricity))
```

- FWHM weighted 60% (seeing dominates image quality)
- Eccentricity weighted 40% (tracking quality)
- Inverse scoring: lower FWHM/eccentricity → higher score
- Score = 0.0 when metrics are unavailable (no tiebreaker advantage)

### Deletions

Remove entirely:
- `QualityWeights.h` / `QualityWeights.cpp`
- `WeightConfig` struct
- `ComputeQualityWeights()` function
- `NXSQualityWeightMode` enumeration parameter class
- Instance parameters: `p_fwhmWeight`, `p_eccentricityWeight`, `p_skyBackgroundWeight`, `p_hfrWeight`, `p_altitudeWeight`, `p_qualityWeightMode`
- UI: per-metric weight sliders and quality mode combobox

## API Changes

### PixelSelector (modified signatures)

Remove `qualityWeights` parameter, add `qualityScores`:

```cpp
// Before:
PixelResult selectBestZ(const float* zColumnPtr, size_t nSubs,
                        const std::vector<double>& qualityWeights,
                        const uint8_t* maskColumn = nullptr);

// After:
PixelResult selectBestZ(const float* zColumnPtr, size_t nSubs,
                        const double* qualityScores = nullptr,
                        const uint8_t* maskColumn = nullptr);
```

Same pattern for `processPixel`, `processImage`, `processImageGPU` — replace `const std::vector<double>& qualityWeights` with `const double* qualityScores`.

The scores array is extracted from `SubCube::metadata(z).qualityScore` at the `processImage` call site into a contiguous `std::vector<double>`.

### Config (modified)

```cpp
struct Config {
    int maxOutliers = 3;
    double outlierAlpha = 0.05;
    bool adaptiveModels = false;
    bool useGPU = false;
    bool enableMetadataTiebreaker = true;  // replaces useQualityWeights
};
```

## Tiebreaker Algorithm

Inserted into `selectBestZ` after step 8 (find frame closest to shortest-half mode value):

```
Input: selectedValue, bestZ, cleanIndices[], zValues[], qualityScores[], originalIndices[]
       shortestHalf[] (the sorted values in the densest half-interval)

1. Compute MAD of shortestHalf:
   mad = 1.4826 × median(|shortestHalf[i] - median(shortestHalf)|)

2. If mad == 0 or qualityScores == nullptr:
   return (no tiebreaker possible)
   Note: mad == 0 when halfN == 1 (< 4 clean subs), so tiebreaker
   naturally disables for very small stacks.

3. bestScore = qualityScores[originalIndices[bestZ_cleanIndex]]

4. For each idx in cleanIndices where idx was in the shortest-half interval:
   if |zValues[idx] - selectedValue| ≤ mad:
     score = qualityScores[originalIndices[idx]]
     if score > bestScore:
       bestScore = score
       bestZ = originalIndices[idx]

5. Return updated bestZ
```

The `selectedValue` (shortest-half mode mean) is NOT changed — only the frame provenance changes. The pixel value remains the statistically optimal estimate.

## Phase 2 Refactor

Phase 2 changes from "Compute quality weights" to "Compute frame metadata scores":

1. Run `ComputeFrameMetrics` per frame (existing PSF fitting)
2. Compute `qualityScore` from FWHM + eccentricity
3. Store in `SubMetadata::qualityScore`
4. Extract into `std::vector<double> qualityScores` for pixel selector
5. Console output: per-frame diagnostics + score range

### ComputeFrameMetrics Fix

Current thresholds are too aggressive for uncalibrated subs:

```cpp
// Before:
detector.SetSensitivity( 0.5 );
detector.SetMinSNR( 10.0 );

// After:
detector.SetSensitivity( 0.1 );
detector.SetMinSNR( 5.0 );
```

**Trade-off note**: Lower thresholds may detect noise peaks as stars on well-calibrated subs. This is acceptable for NukeX's use case (uncalibrated subs are the norm) and the quality score is advisory, not authoritative — false star detections add noise to the score but don't corrupt the statistical pixel selection.

Add diagnostic output:
```cpp
console.WriteLn( String().Format(
    "  Sub %d: %d stars, FWHM=%.2f, Ecc=%.3f, Score=%.4f",
    z, count, meta.fwhm, meta.eccentricity, meta.qualityScore ) );
```

## GPU Path

### Current State

The GPU kernel in `CudaPixelSelector.cu` does NOT track provenance (frame indices). It outputs only pixel values and distribution types. Provenance is only tracked on the CPU path. Additionally, provenance is currently not consumed by any downstream pipeline phase — it is diagnostic metadata only.

### CudaPixelSelector.h / CudaPixelSelector.cu

- Add `const double* qualityScores` field to `GpuStackConfig` (host-side pointer, copied to device)
- Add `uint32_t* provenanceOut` field to `GpuStackConfig` (device output buffer, optional)
- Host copies scores array to device before kernel launch (one `double` per sub, max 64 values)
- Kernel implements same tiebreaker algorithm as CPU path after shortest-half mode selection
- Kernel writes winning frame index to `provenanceOut` if non-null

**Note**: Since provenance is not currently consumed downstream, the GPU tiebreaker's observable effect is limited to the tiebreaker changing which frame's value is "closest" — but the pixel output is still the shortest-half mode mean. The GPU tiebreaker is included for parity with the CPU path so behavior is consistent regardless of execution path.

### CudaRemediation.h / CudaRemediation.cu

- Remove the unused `qualityWeights` parameter

## UI Changes

- Remove: quality mode combobox, per-metric weight sliders (fwhm, eccentricity, skyBackground, hfr, altitude)
- Rename: "Enable Quality Weighting" checkbox → "Enable Metadata Tiebreaker"
- Keep: single checkbox to enable/disable the tiebreaker
- Update tooltip to describe the tiebreaker behavior

## Tests

### New Tests

1. **Tiebreaker picks better score**: Two frames within MAD tolerance, different `qualityScore` → selects higher-scored frame
2. **Equal scores — no change**: All scores equal → same frame selected as without tiebreaker
3. **Zero scores — graceful degradation**: All scores 0.0 → identical behavior to current code
4. **Single candidate**: Only one frame in tolerance band → no change to selection
5. **Null scores pointer**: `qualityScores == nullptr` → identical behavior to current code

### Modified Tests

- All existing `PixelSelector` tests: update signatures to remove `qualityWeights`, add `qualityScores` (nullptr for unchanged behavior)
- Remove `QualityWeights` unit tests (file deleted)

### Diagnostic Test

- `ComputeFrameMetrics` on a synthetic image with known stars → verify non-zero FWHM and eccentricity

## File Change Summary

| File | Action |
|------|--------|
| `src/engine/SubCube.h` | Add `qualityScore` to `SubMetadata` |
| `src/engine/QualityWeights.h` | Delete |
| `src/engine/QualityWeights.cpp` | Delete |
| `src/engine/PixelSelector.h` | Replace `qualityWeights` → `qualityScores`, update Config |
| `src/engine/PixelSelector.cpp` | Replace param, add tiebreaker after step 8 |
| `src/engine/FrameLoader.cpp` | Fix `ComputeFrameMetrics` thresholds |
| `src/engine/cuda/CudaPixelSelector.h` | Add `qualityScores` and `provenanceOut` to `GpuStackConfig` |
| `src/engine/cuda/CudaPixelSelector.cu` | Add tiebreaker + scores param, provenance output |
| `src/engine/cuda/CudaRemediation.cu` | Remove unused `qualityWeights` param |
| `src/engine/cuda/CudaRemediation.h` | Remove unused `qualityWeights` param |
| `src/NukeXStackInstance.h` | Remove weight params, add score extraction |
| `src/NukeXStackInstance.cpp` | Refactor Phase 2, update Phase 3 calls |
| `src/NukeXStackInterface.h` | Remove weight slider GUI members |
| `src/NukeXStackInterface.cpp` | Simplify Quality section UI |
| `src/NukeXStackParameters.h` | Remove weight parameter classes |
| `src/NukeXStackParameters.cpp` | Remove weight parameter implementations |
| `src/NukeXStackProcess.cpp` | Remove weight parameter registration |
| `tests/unit/test_quality_weights.cpp` | Delete |
| `tests/unit/test_pixel_selector.cpp` | Update signatures, add tiebreaker tests |
| `tests/integration/test_full_pipeline.cpp` | Update to remove QualityWeights dependency |
| `tests/CMakeLists.txt` | Remove QualityWeights sources and test file |
| `CMakeLists.txt` | Remove QualityWeights sources |

## Behavioral Guarantees

1. **Statistics remain authoritative**: The shortest-half mode value is never changed by the tiebreaker. Only the provenance (which frame) can change.
2. **Graceful degradation**: When scores are all zero, all equal, or nullptr, behavior is identical to v3.1.0.10.
3. **No performance regression**: The tiebreaker is a single scan of the shortest-half cluster (≤15 values typically). Negligible cost.
4. **Backward compatible output**: The stacked pixel values will be identical in the common case (same shortest-half mode). Only provenance may differ when a better-scored alternative exists within the MAD tolerance band.
