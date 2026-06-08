# Spec Amendment: Aggregate Subcube Verification

**Date**: 2026-03-18
**Status**: Approved
**Amends**: 2026-03-17-subcube-dust-detection-design.md, Step 2

## Problem

The original spec's per-pixel per-frame verification (Step 2) fails on real data. The criterion `madDeficit < medianDeficit * 0.5` requires frame-to-frame variance to be less than half the signal at each individual pixel. With 30 uncalibrated subs:

- Per-pixel dust deficit: ~0.001-0.006 (9% of background ~0.07)
- Per-pixel per-frame noise: ~0.002-0.004 (photon + read noise)
- Typical ratio (MAD/median): 2-50× (always >>0.5)

This was tested on both linear (v3.1.0.14) and stretched (v3.1.0.17) detection with identical results — the verification operates on the linear subcube regardless of detection domain, and individual pixel SNR is always <2 in individual frames.

## Root Cause

The 9% optical deficit IS present in every frame, but at the per-pixel level it's buried in per-frame noise. The stacked image detects it because stacking averages noise down by √N (√30 ≈ 5.5×). Individual frames don't have this benefit.

## Solution

Replace per-pixel verification with **aggregate spatial verification**: average the deficit across all sample pixels per frame, then check consistency of the per-frame means.

Spatial averaging over K pixels reduces noise by √K:
- K=20 samples: noise drops 4.5×, SNR improves from ~1 to ~4.5
- K=40 samples: noise drops 6.3×, SNR improves from ~1 to ~6.3

For the known dust mote (deficit ~0.006, noise ~0.003):
- Per-pixel SNR: ~2 (fails 0.5 threshold)
- After 20-pixel averaging: ~9 (passes easily)

## Amended Algorithm (Step 2)

For each candidate blob:
1. Sample up to 40 pixels within the blob
2. For each frame z, compute `frameAvgDeficit[z] = mean(neighborMean_z - pixel_z)` across all sample pixels
3. Compute `medianAggDef = median(frameAvgDeficit)` and `madAggDef = 1.4826 × MAD(frameAvgDeficit)`
4. Blob passes if: `medianAggDef > 0` AND `madAggDef / medianAggDef < 1.0`

The threshold is relaxed from 0.5 to 1.0 because aggregate deficits have residual variance from spatial non-uniformity within the blob.

## Behavioral Guarantees (updated)

1. **Stars**: Still rejected — brightness exclusion in Step 1 prevents star halos from becoming candidates
2. **Noise clusters**: Still rejected — random noise has zero expected deficit when averaged spatially; aggregate median will be ~0
3. **Real dust**: Now detectable — consistent ~9% deficit produces positive aggregate deficit with low variance across frames
4. **Transient artifacts (trails)**: Still rejected — present in only a few frames, producing high MAD in aggregate

## Interaction with Post-Stretch Detection

This amendment combines with the Phase 3b→7a move (post-stretch detection):
- **Step 1 (spatial)**: Runs on stretched image where MTF amplifies the deficit (8.6σ proven)
- **Step 2 (verification)**: Runs on linear subcube with aggregate spatial averaging
- Together they address both problems: candidate quality AND per-frame SNR
