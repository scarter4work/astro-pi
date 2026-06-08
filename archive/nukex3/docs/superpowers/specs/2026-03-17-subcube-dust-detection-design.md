# Subcube-Based Dust Mote Detection Design

**Date**: 2026-03-17
**Status**: Approved
**Author**: Scott Carter + Claude

## Problem

Phase 7c dust detection operates on the stretched image using a difference-of-smoothing deficit map with a static sigma threshold. This produces massive false positives (552 blobs, 43k pixels) because star halos, noise, and edge artifacts all create positive deficits at the mote scale. The fundamental issue: single-image spatial analysis cannot distinguish dust from other dark features.

## Solution

Move dust detection to a new **Phase 3b** that operates on the linear stacked image AND verifies candidates against the subcube. A dust mote has a unique subcube fingerprint: the same spatial deficit at position (x,y) in **every** frame, with low inter-frame variance. This cannot be faked by noise, stars, or transient artifacts.

## Algorithm

### Step 1: Spatial Candidate Detection (linear stacked image)

1. Compute difference-of-smoothing deficit on the linear stacked image:
   - `smallSmooth = boxFilter(stacked, dustMinDiameter)`
   - `largeSmooth = boxFilter(stacked, dustMaxDiameter * 1.5)`
   - `deficit = largeSmooth - smallSmooth`
2. Threshold: `median(deficit) + dustDetectionSigma × 1.4826 × MAD(deficit)`
3. Brightness exclusion: skip pixels where `stacked[i] > largeSmooth[i]`
4. Connected component labeling (4-connected flood fill)
5. Blob filtering: diameter in [dustMinDiameter, dustMaxDiameter], circularity >= dustCircularityMin

### Step 2: Subcube Consistency Verification

For each candidate blob:
1. Sample up to 20 pixels within the blob (evenly spaced grid, or all if blob < 20 pixels)
2. For each sampled pixel at (x, y), compute `neighborRadius = max(5, blob.radius / 2)`:
   - Collect Z-values of neighbors: mean of Z-columns at (x±neighborRadius, y±neighborRadius) corners (4 points)
   - For each frame z: `deficit_z = neighborMean_z - subcube[z, y, x]`
   - Compute `medianDeficit = median(deficit_0..deficit_N)`
   - Compute `madDeficit = 1.4826 × MAD(deficit_0..deficit_N)`
   - Pixel passes if: `medianDeficit > 0` (consistently darker) AND `madDeficit < medianDeficit * 0.5` (variation < half the deficit)
3. Blob passes verification if >50% of sampled pixels pass
4. Only verified blobs are written to the dust mask

### Step 3: Store Mask

The dust mask (uint8, W×H, 1=dust) is stored as a member on the detection result and passed to Phase 7c for remediation.

## Pipeline Changes

### New Phase 3b

Runs after Phase 3 (stacking), before Phase 4 (create output window). Has access to:
- The per-channel stacked results (linear float arrays)
- The per-channel SubCubes (still in memory)

Produces:
- A dust mask per channel (or a single luminance-based mask applied to all channels)

### Phase 7c Changes

Phase 7c becomes **remediation-only**. It receives the dust mask from Phase 3b instead of running its own detection. The `detectDust` method in ArtifactDetector is no longer called from Phase 7.

The remediation logic (neighbor brightness correction) stays unchanged, but uses `dustNeighborRadius = 85` (already updated) to reach outside the mote.

## File Changes

| File | Action |
|------|--------|
| `src/engine/ArtifactDetector.h` | Add `detectDustSubcube()` method taking subcube + stacked image |
| `src/engine/ArtifactDetector.cpp` | Implement `detectDustSubcube()` with DoS + subcube verification |
| `src/NukeXStackInstance.cpp` | Add Phase 3b between stacking and output; pass dust mask to Phase 7c |
| `tests/unit/test_artifact_detector.cpp` | Add subcube-verified dust detection tests |

## Behavioral Guarantees

1. **No false positives on stars**: Stars are brighter than background (brightness exclusion) and have negative per-frame deficit (consistency check fails)
2. **No false positives on noise**: Random noise has high inter-frame variance (MAD > 0.5 × median check fails)
3. **Real dust detected**: Consistent 5-10% deficit across all frames passes both spatial and consistency checks
4. **Graceful degradation**: If no subcube data available (e.g., future single-image mode), falls back to spatial-only detection

## Future: Two-pass adaptive (noted for later)

If single-scale detection misses motes significantly larger or smaller than the kernel range, a two-pass approach would detect candidates first, measure each blob's actual radius, then re-estimate background with per-blob adaptive kernels.
