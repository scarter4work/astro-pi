# RGB Stacking + Distribution-Aware Auto-Stretch

**Date**: 2026-03-08
**Status**: Approved

## Overview

Extend NukeXStack to handle RGB (3-channel) input subs from WBPP-debayered data,
stack each channel independently using the existing per-pixel distribution fitting
pipeline, and automatically select and apply the best stretch algorithm based on
the distribution type maps produced during stacking.

## Requirements

- Input: WBPP-debayered RGB FITS/XISF subs (3-channel)
- Backward-compatible: mono (1-channel) subs still work identically
- Alignment: star detection on channel 0 (Red), same offsets for all channels
- Stacking: per-channel PixelSelector pass (reuse 100% of existing fitters)
- Output: two image windows — `NukeX_stack` (linear) + `NukeX_stretched` (auto-stretched)
- Auto-stretch: select from all 11 algorithms using distribution type statistics
- Logging: verbose, readable console output + StatusMonitor progress bars
- Cancelable: ProcessEvents() between chunks, StatusMonitor supports abort

## Architecture

### 1. FrameLoader Changes

`LoadedFrames` stores per-channel pixel data:

```
struct LoadedFrames {
    vector<vector<vector<float>>> pixelData;  // [frame][channel][pixels]
    vector<SubMetadata> metadata;
    int width, height, numChannels;
};
```

`LoadRaw` reads `img.NumberOfChannels()` and iterates `img.PixelData(c)` for each.
Console logs channel count per frame.

### 2. Alignment (no changes to core)

Star detection runs on channel 0 data only. `alignFrames()` stays the same.

New helper function:

```
SubCube applyAlignment(const vector<vector<float>>& channelFrameData,
                       const vector<AlignmentResult>& offsets,
                       const CropRegion& crop, int width, int height);
```

Applies pre-computed offsets to any single channel's frame data, producing an
aligned SubCube. Called once per channel.

### 3. ExecuteGlobal Flow

```
Phase 1: Load frames (all channels)
  - StatusMonitor: "Loading frame N/40"
  - Console: per-frame filename, channels, dimensions

Phase 1b: Align (channel 0 only)
  - Console: matched stars, offsets per frame, crop dimensions

Phase 2: Quality weights (once, from metadata)

Phase 3: Per-channel stacking
  for ch = 0..numChannels-1:
    - Build SubCube from aligned channel data
    - Run PixelSelector (OpenMP, row-chunk progress)
    - Collect distType map (all channels, for auto-stretch)
    - Console: distribution summary per channel
    - StatusMonitor: "Stacking channel R/G/B: row N/H"
    - Free SubCube after extracting results

Phase 4: Create linear output (NukeX_stack)
  - 3-channel Image window

Phase 5: Auto-stretch selection
  - Analyze distType maps + per-channel stats
  - Console: selected algorithm + reasoning

Phase 6: Apply stretch (NukeX_stretched)
  - Clone linear image, apply selected algorithm
  - Console: stretch parameters used
```

### 4. Auto-Stretch Algorithm Selection

Function: `AutoSelectAlgorithm(distTypeMaps[], perChannelStats)`

Inputs:
- distType map per channel (uint8_t, H x W)
- Per-channel: median, MAD, mean

Decision tree:

1. Compute distribution fractions per channel:
   `fracGaussian[c], fracPoisson[c], fracSkewNormal[c], fracBimodal[c]`

2. Compute channel divergence:
   `divergence = max cross-channel difference in distribution fractions`

3. Selection:
   - divergence > 0.15 → **Lumpton** (channels have different character, preserve color ratios)
   - max(fracBimodal) > 0.15 → **ArcSinh** (HDR, two-population pixels)
   - max(fracSkewNormal) > 0.20 → **GHS** (asymmetric/nebulosity)
   - max(fracPoisson) > 0.40 AND bright outliers present → **Veralux** (faint + highlight)
   - max(fracPoisson) > 0.40 → **GHS** with aggressive parameters
   - channels similar AND broadband → **RNC** (natural color)
   - mostly Gaussian → **MTF** (clean, simple)
   - fallback → **GHS** (most versatile)

All 11 concrete algorithms are candidates. Pure decision tree, no ML.

### 5. Logging Strategy

**Console output** — narrative, readable:
```
═══════════════════════════════════════════════════
  NukeX v3 — Per-Pixel Statistical Inference Stacking
═══════════════════════════════════════════════════

Phase 1: Loading 40 frames...
  [1/40] M63_Light_001.fits — 3 channels, 3840 x 2160
  [2/40] M63_Light_002.fits — 3 channels, 3840 x 2160
  ...
  Loaded 40 frames in 12.3s

Phase 1b: Aligning frames...
  Reference: frame 1 (142 stars detected)
  [2/40] dx=+3, dy=-1 (138 stars, 45 matched, RMS=0.42)
  ...
  Crop: 3820 x 2148 (from 3840 x 2160)
  Alignment complete in 8.1s

Phase 2: Computing quality weights...
  Mode: Full (FWHM + Eccentricity + Sky + HFR + Altitude)
  Weight range: 0.72 — 1.00

Phase 3: Per-channel stacking...
  Channel R (1/3):
    Row 100/2148 (4.7%) ... Row 2148/2148 (100.0%)
    Distribution: 68% Gaussian, 18% Poisson, 9% Skew-Normal, 5% Bimodal
  Channel G (2/3):
    ...
  Channel B (3/3):
    ...
  Stacking complete in 4m 32s

Phase 4: Creating linear output...
  Window: NukeX_stack (3820 x 2148, RGB)

Phase 5: Auto-stretch selection...
  R: 68% Gaussian, 18% Poisson, 9% Skew-Normal, 5% Bimodal
  G: 71% Gaussian, 16% Poisson, 8% Skew-Normal, 5% Bimodal
  B: 65% Gaussian, 22% Poisson, 8% Skew-Normal, 5% Bimodal
  Channel divergence: 0.06 (similar)
  Selected: GHS (high Skew-Normal fraction indicates nebulosity)

Phase 6: Applying stretch...
  Algorithm: Generalized Hyperbolic Stretch
  Parameters: D=3.5, b=0.25, SP=0.0, HP=1.0
  Window: NukeX_stretched (3820 x 2148, RGB)

═══════════════════════════════════════════════════
  NukeX stacking complete — 5m 18s total
═══════════════════════════════════════════════════
```

**StatusMonitor** — progress bar for long operations:
- Frame loading: N/total
- Per-channel stacking: row/totalRows with channel label

### 6. Memory Budget

40 RGB subs at 3840x2160:
- Raw loaded: 40 × 3 × 3840 × 2160 × 4 = ~3.8 GB
- Per-channel SubCube: 40 × 3820 × 2148 × 4 = ~1.3 GB (freed after each channel)
- Output images: 2 × 3 × 3820 × 2148 × 4 = ~0.2 GB
- Peak: ~5.3 GB (system has 78 GB)

### 7. Files Modified

- `src/engine/FrameLoader.h/.cpp` — RGB-aware LoadRaw
- `src/engine/FrameAligner.h/.cpp` — add applyAlignment() helper
- `src/engine/AutoStretchSelector.h/.cpp` — NEW: distribution-aware algorithm selection
- `src/NukeXStackInstance.cpp` — RGB ExecuteGlobal with logging + auto-stretch
- `src/NukeXStackParameters.h/.cpp` — auto-stretch enable/disable parameter
- `src/NukeXStackInterface.h/.cpp` — auto-stretch checkbox in GUI
- `tests/` — RGB pipeline test, auto-stretch selector test
