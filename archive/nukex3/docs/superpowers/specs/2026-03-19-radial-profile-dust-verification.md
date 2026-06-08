# Radial Profile Dust Verification

**Date**: 2026-03-19
**Status**: Approved
**Replaces**: Subcube consistency verification (Step 2 of 2026-03-17 spec + 2026-03-18 amendment)

## Problem

All previous subcube verification approaches failed because they compared absolute deficit values across frames. With uncalibrated subs, per-frame sky brightness variations dominate the signal. Eight iterations (v3.1.0.14 through v3.1.0.21) proved this conclusively.

## Solution

Replace subcube verification with a **radial deficit ratio** test. For each spatial candidate, compute the ratio of mean Z-value inside the blob vs. a surrounding ring, per frame. A real dust mote produces a ratio consistently < 1.0 (center darker than surroundings). This works because the ratio normalizes out per-frame sky brightness — both inner and outer scale together.

## Algorithm

For each candidate blob from spatial detection:

1. **Inner sample**: collect the blob's member pixel positions (up to 100, subsampled if larger)
2. **Outer ring**: collect pixels at radius [dustMaxDiameter/2, dustMaxDiameter] from blob center, subsampled every 5th pixel in each dimension
3. For each frame z:
   - `innerMean_z` = mean of subcube values at inner pixel positions
   - `outerMean_z` = mean of subcube values at outer ring positions
   - `ratio_z` = innerMean_z / outerMean_z (skip if outerMean < 1e-10)
4. Compute `medianRatio = median(ratio_0 ... ratio_N)`
5. Blob passes if `medianRatio < 0.97` (at least 3% deficit)

## Why This Works

- **Sky normalization**: ratio cancels per-frame brightness differences (the fatal flaw in all previous approaches)
- **High SNR**: averaging over 100+ inner pixels and 1000+ outer pixels gives per-frame SNR > 40
- **Simple threshold**: dust has ~9% deficit (ratio ~0.91), noise has ~0% (ratio ~1.0). Clean 6% gap.
- **No MAD/variance check needed**: the ratio itself is the signal. If median ratio < 0.97, it's dust.

## False Positive Analysis

- **Stars**: brighter than surroundings → ratio > 1.0 → rejected
- **Noise blobs**: no consistent deficit → ratio ≈ 1.0 ± 0.01 → rejected
- **Galaxy structure**: gradients exist but are smooth over dustMaxDiameter scale → ratio ≈ 1.0 → rejected
- **Real dust**: 9% optical attenuation → ratio ≈ 0.91 → accepted
