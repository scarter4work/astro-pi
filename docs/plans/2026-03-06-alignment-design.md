# Image Alignment/Registration Design

**Date:** 2026-03-06
**Status:** Approved

## Problem

Without alignment, per-pixel Z-columns in the SubCube sample different sky positions across subs due to dither offsets and tracking drift. This makes the statistical distribution fitting in PixelSelector meaningless — stars "bounce" across pixel positions.

PixInsight's built-in StarAlignment rejects subs that don't match well. We want zero rejection — every sub gets used.

## Constraints

- Same imaging session: one night, one rig, one target
- EQ mount with 0.7" RMS tracking, Askar 71F at 490mm FL (~1.5-2"/pixel)
- Dithering every ~10 frames (5-20 pixel offsets between dither groups)
- Transforms are pure translation — no field rotation (EQ mount), no scale changes
- Zero sub rejection
- Must preserve original noise statistics for downstream distribution fitting

## Approach: Star Centroid Matching + Integer Pixel Shifts

### Why Integer Shifts

- At 1.5-2"/pixel, 0.5 pixel error is under 1 arcsec — well within typical seeing (2-3")
- Sub-pixel interpolation introduces pixel-to-pixel noise correlation that biases distribution fitting
- Simpler, faster, no resampling artifacts

### Component 1: StarDetector

**File:** `src/engine/StarDetector.h/.cpp`

Detect stars in linear (unstretched) frame data:

1. Compute background: median and MAD of entire frame
2. Threshold: flag pixels > median + k*MAD (k ~ 5-8)
3. Connected components: group adjacent flagged pixels into blobs
4. Filter: reject hot pixels (<3px), nebula cores (>50px), trails (eccentricity > 0.7)
5. Centroid: intensity-weighted (x, y) for each surviving blob

**Output:** `std::vector<StarPosition>` with `{x, y, flux}` per star.

### Component 2: TriangleMatcher

**File:** `src/engine/TriangleMatcher.h/.cpp`

Match star lists between frames using triangle asterism descriptors:

1. Select top N brightest stars per frame (N ~ 50-100)
2. Form triangles from triplet combinations
3. Normalize: compute side ratios (b/a, c/a where a >= b >= c) — invariant to translation, rotation, scale
4. Build sorted list of descriptors from reference frame
5. Match candidate triangles against reference within tolerance
6. Vote: each matched triangle votes for star-to-star correspondence
7. Compute translation: median of (dx, dy) from confirmed star pairs

**Output:** `AlignmentResult` with `{int dx, int dy, int numMatchedStars, double convergenceRMS}`.

### Component 3: FrameAligner

**File:** `src/engine/FrameAligner.h/.cpp`

Orchestrator that ties detection and matching together:

1. Run StarDetector on all frames
2. Select reference frame (best quality score from QualityWeights pre-pass)
3. Run TriangleMatcher for each frame against reference
4. Compute autocrop bounding box from all offsets:
   - `x_min = max(0, max(all dx))`
   - `y_min = max(0, max(all dy))`
   - `x_max = min(W, W + min(all dx))`
   - `y_max = min(H, H + min(all dy))`
5. Allocate SubCube at cropped dimensions
6. Copy shifted+cropped pixel data into SubCube (no interpolation)

### Integration Point

Modified `FrameLoader` / `ExecuteGlobal` pipeline:

```
FrameLoader::Load()
  -> Load raw frames into temporary buffer
  -> StarDetector::Detect() on each frame
  -> TriangleMatcher::Match() against reference
  -> Compute crop region
  -> Allocate SubCube at cropped dimensions
  -> Copy shifted+cropped pixel data into SubCube
-> QualityWeights (unchanged)
-> PixelSelector (unchanged - gets properly aligned Z-columns)
-> Output image at cropped dimensions
```

The rest of the pipeline is unaware alignment happened — it just sees a slightly smaller SubCube.

## Expected Data Loss from Cropping

With 5-20 pixel dither offsets on a typical astro sensor (e.g. 6248x4176), cropping removes ~20-40 pixels per edge — well under 1% of the frame.

## Future Extensions (Deferred)

- Sub-pixel interpolation as an option (for users who prefer tighter stars over noise preservation)
- Multi-session alignment with rotation/scale
- Distortion correction (optical field curvature)
- GPU-accelerated star detection
