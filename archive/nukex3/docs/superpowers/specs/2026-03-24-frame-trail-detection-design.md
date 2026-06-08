# Frame-Level Trail Detection — Seed-and-Verify

**Date:** 2026-03-24
**Status:** Draft

## Problem

Satellite trails, airplane lights, and meteor streaks contaminate individual frames. The current pre-stack rejection (Phase 1b.5, median+MAD per pixel-frame) catches bright outliers but has no spatial awareness — it doesn't know what a "line" is. Faint trails that don't exceed the MAD threshold slip through and appear as faint lines in the stacked result.

## Goal

Detect trail pixels **per-frame** on raw aligned data, mark them in `SubCube::m_masks`, and let the existing `selectBestZ()` skip them during Phase 3 stacking. Trails never enter the composite — preventive, not corrective.

## Approach: Seed-and-Verify with Collinearity Pre-filter

### Phase 1: Seed Detection

For each aligned frame `z`, scan for **spatial outliers** — pixels significantly brighter than their local neighborhood.

**Precondition:** `SubCube::m_masks` must be allocated before the trail detector runs. Alignment (`FrameAligner::alignAndCrop`) guarantees this.

- Skip pixels already masked by alignment (out-of-bounds pixels are zero-filled and would produce garbage local statistics)
- Compute local median in a small window (e.g., 7×7) around each pixel, excluding alignment-masked pixels from the window
- Compute local MAD in the same window
- Use sliding-window histogram (Huang's algorithm) for O(W×H×window) median computation instead of naive O(W×H×window²)
- A pixel is a **seed** if: `pixel > local_median + k * local_MAD` (default `k = 3.0`)
- Stars will also trigger as seeds — that's fine, they get filtered in Phase 2

### Phase 2: Collinearity Clustering

Group seeds into clusters that are roughly collinear. This rejects:
- **Cosmic rays** — isolated single-pixel seeds (no cluster)
- **Stars** — round clusters with no preferred direction
- **Noise** — random scatter, not collinear

Algorithm:
1. Dilate seed mask by `gapTolerance` pixels (morphological dilation) to bridge small gaps in a single trail
2. Connected-component labeling on dilated seed mask (8-connectivity)
3. For each cluster with ≥ 3 seeds (counted on original undilated mask):
   - Compute principal axis via simple eigenvector of the 2D covariance matrix
   - Compute **linearity score**: `1 - (λ_min / λ_max)` where λ are eigenvalues
   - Keep clusters with linearity ≥ 0.9 (strongly elongated)
   - Reject clusters where the extent along the principal axis is < 20 pixels (too short to be a trail)

### Phase 3: Line Estimation and Walk

For each surviving cluster:
1. Fit a line through the cluster seeds (least-squares or principal axis direction + centroid)
2. **Walk the entire line** across the frame (edge to edge, not just the cluster extent)
3. At each pixel along the line, check against **cross-line neighbors** (perpendicular to the line direction):
   - Sample 2-3 pixels on each side of the line (perpendicular offset ±3-5 pixels)
   - At frame borders: use one-sided neighbors when perpendicular samples would be out of bounds
   - Compute median of cross-line neighbors
   - If the on-line pixel exceeds `neighbor_median + confirm_sigma * neighbor_MAD`: **confirmed trail pixel**
   - `confirmSigma` (2.5) is intentionally lower than `seedSigma` (3.0): seeds are found aggressively, then confirmed with a more sensitive cross-line check since the line geometry is already established
   - If the on-line pixel is consistent with neighbors (e.g., inside a star): **not a trail pixel at this location, but keep walking** — the line continues through the star

### Phase 4: Mask and Dilate

- All confirmed trail pixels for frame `z` → `SubCube::m_masks(z, y, x) = 1`
- Dilate the trail mask by 2-3 pixels perpendicular to the line direction (trail PSF extends slightly beyond the geometric line)
- If a frame has no confirmed lines, no masks are set

### Integration Point

- Runs after alignment (Phase 1b) and before stacking (Phase 3)
- Operates on each aligned frame independently — trivially parallelizable with OpenMP
- Results stored in `SubCube::m_masks` (already allocated and respected by `selectBestZ()`)
- Replaces Phase 1b.5 median+MAD rejection for trail-type outliers (Phase 1b.5 can still catch non-trail bright outliers like cosmic rays, or be removed entirely since collinearity clustering already separates cosmic rays from trails)

## Configuration

```cpp
struct TrailDetectorConfig
{
    double seedSigma       = 3.0;   // spatial outlier threshold (local MAD units)
    int    seedWindowSize  = 7;     // local neighborhood window (pixels, odd)
    double linearityMin    = 0.9;   // minimum linearity score for cluster
    int    minClusterLen   = 20;    // minimum extent along principal axis (pixels)
    double confirmSigma    = 2.5;   // cross-line neighbor confirmation threshold
    int    crossLineOffset = 4;     // perpendicular neighbor distance (pixels)
    double dilateRadius    = 2.0;   // perpendicular dilation (pixels)
    int    gapTolerance    = 3;     // max gap between seeds in connected components (pixels)
};
```

## Data Flow

```
Per frame z (parallelizable):
  aligned_frame[z]
    → seed detection (spatial outliers)
    → connected-component clustering
    → linearity filter (eigenvalue ratio)
    → for each linear cluster:
        → fit line (principal axis)
        → walk line edge-to-edge
        → cross-line neighbor check at each pixel
        → confirmed pixels → SubCube::m_masks(z, y, x) = 1
        → dilate perpendicular to line

Phase 3 stacking:
  selectBestZ(zColumnPtr, nSubs, qualityScores, maskColumnPtr)
    → skips frames where mask == 1 at that pixel
    → fits distributions on clean frames only
    → output: clean stacked value
```

## Edge Cases

- **Trail through a star:** Cross-line neighbors are also bright (star) → trail pixel not confirmed at that location. But the line geometry is established from the non-star portions, so the walk continues through the star. The cross-line check will fail at the star center (pixel matches neighbors), so those pixels stay unmasked — which is correct since the star dominates and `selectBestZ()` handles the multi-modal distribution. **Known limitation:** If a bright satellite crosses a faint star, the trail may add comparable signal — but masking those pixels would remove valid star data from the frame, which is typically worse for a 15-30 frame stack.
- **Multiple trails in one frame:** Each forms a separate linear cluster; each gets its own line fit and walk. Handled independently.
- **Very faint trails (below seed threshold):** Won't generate seeds, won't be detected. These are below noise and stacking statistics handle them via sigma-clipping in `selectBestZ()`.
- **Dense star fields:** Many seeds, but star clusters are round (low linearity score), filtered out in Phase 2.
- **Short trails (meteor fragments):** Below `minClusterLen` → rejected. Too few pixels to meaningfully mask, and stacking statistics handle them.
- **Bright nebula edges:** Sharp H-alpha ridges could produce elongated seed clusters with high linearity. The cross-line verification step rejects these — nebula brightness extends perpendicular to the "line," so cross-line neighbors are similarly bright and the confirmation fails.
- **Frame with many masked pixels (>30% masked):** Likely a bad frame entirely. Log a warning; the frame still participates at non-masked pixels.

## Relationship to Existing Code

- **Phase 1b.5 (median+MAD):** Replaced by the new trail detector. Cosmic rays (isolated single-pixel outliers) are rejected by the collinearity filter in Phase 2 — they never form linear clusters. The existing Phase 1b.5 code is removed.
- **Phase 7a trail detection (Hough on stretched image):** Becomes unnecessary for trail handling. Dust and vignetting detection in Phase 7a remain untouched.
- **Phase 7b trail remediation (GPU re-selection):** Unnecessary if trails are masked pre-stack. Dead code candidate after this feature ships.
- **`SubCube::m_masks`:** Already exists with the right shape and layout. `selectBestZ()` already accepts `maskColumn`. No infrastructure changes needed.

## New Files

- `src/engine/TrailDetector.h` — `TrailDetectorConfig` struct + `TrailDetector` class
- `src/engine/TrailDetector.cpp` — implementation
- `tests/unit/test_trail_detector.cpp` — unit tests

## Performance Estimate

Per frame (4000×3000 pixels):
- Seed scan: O(W×H×window) with Huang's sliding-window histogram ≈ 12M × 7 = ~84M ops — ~50-80ms per frame
- Connected components: O(seed_count) — typically <0.1% of pixels are seeds (up to 1-5% in dense star fields, still <10ms)
- Linearity filter: O(cluster_count) — trivial
- Line walk + cross-line check: O(line_length) per line — sub-millisecond
- Total per frame: ~80-150ms
- 20 frames on 8 cores (OpenMP): ~200-400ms total

## Testing Strategy

1. **Synthetic trail injection:** Draw a 1-2 pixel wide line at known angle on flat background, verify detection and mask placement
2. **Trail through synthetic star:** Verify line geometry extends through the star, mask covers the trail but not the star core
3. **Cosmic ray rejection:** Single bright pixel, verify it does NOT generate a line
4. **Dense seed field:** Random scatter, verify no false lines (linearity filter works)
5. **Multiple trails:** Two crossing trails in one frame, verify both detected independently
6. **Faint trail at threshold:** Trail at exactly `seedSigma` — verify marginal behavior
