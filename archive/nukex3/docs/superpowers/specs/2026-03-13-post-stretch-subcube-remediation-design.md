# Post-Stretch Subcube Remediation

**Date:** 2026-03-13
**Status:** Approved
**Replaces:** Phase 1a (FlatEstimator / self-flat), Phase 1c (TrailDetector / Hough pre-stack)

## Problem

The current trail detection (Phase 1c) runs a Hough transform on linear aligned frames. Faint satellite trails at SNR ~2-3 in linear space don't survive background subtraction + Sobel edge thresholding and slip through into the final image. The self-flat correction (Phase 1a) uses a blunt median-stack + Gaussian blur approach for dust/vignetting that works but is a coarse approximation.

Both problems share a root cause: detection and correction happen too early in the pipeline, before the nonlinear stretch amplifies faint artifacts into visibility.

## Solution

Replace Phase 1a and Phase 1c with a unified post-stretch remediation phase (Phase 7) that:

1. Detects artifacts in the stretched image where they are visible
2. Remediates through the subcube — the authoritative data source — using two mechanisms:
   - **Trails:** Exclude contaminated frame(s) from the Z-column, re-run `selectBestZ()`
   - **Dust motes / vignetting:** Correct using neighbor brightness ratios from subcube-selected values
3. Runs on GPU (RTX 5070 Ti / CUDA) with CPU fallback

## Pipeline Changes

### Removed Phases

- **Phase 1a (FlatEstimator):** Unregistered median stack + Gaussian blur for synthetic flat. Replaced by Phase 7c neighbor-ratio correction.
- **Phase 1c (TrailDetector):** Hough transform on linear aligned frames with per-frame mask generation. Replaced by Phase 7a+7b post-stretch detection and re-selection.

### New Pipeline

```
Phase 1:  Load frames (debayer, metrics)
Phase 1b: Align (triangle asterism matching, integer shifts, autocrop)
Phase 2:  Quality weights
Phase 3:  Per-channel stacking (PixelSelector: MAD -> ESD -> fitting -> AICc -> median)
Phase 4:  Linear output
Phase 5:  Auto-stretch selection
Phase 6:  Apply stretch
Phase 7:  Post-stretch subcube remediation (NEW)
  7a: Detect artifacts in stretched image
  7b: Trail remediation (GPU) -- re-select from Z-column
  7c: Dust/vignetting remediation (GPU) -- neighbor brightness correction
  7d: Patch linear output, re-stretch corrected pixels
```

### Subcube Lifetime

The per-channel subcubes must remain allocated through Phase 7. Currently, Phase 3 creates per-channel subcubes as local variables in a loop — channel 0 reuses the aligned cube, channels 1-2 build new subcubes via `applyAlignment()`, and each goes out of scope at loop end.

**Change:** Store all per-channel subcubes in a `std::vector<SubCube> channelCubes(numChannels)`. Phase 3 populates them; Phase 7 reads from them; they are freed after Phase 7d.

**Memory impact:** For RGB, this triples subcube memory: ~1.8GB per channel x 3 = ~5.4GB total. This is within the 64GB system RAM budget. The subcubes are uploaded to GPU VRAM one channel at a time during Phase 7b (serial per-channel processing), so VRAM usage stays at ~1.8GB + kernel overhead.

Quality weights (from Phase 2) and `channelResults` vectors (from Phase 3) must also remain alive through Phase 7.

### Auto-Stretch Dependency

Phase 7 requires a stretched image for detection. If auto-stretch is disabled by the user, Phase 7 is skipped entirely. The `enableRemediation` parameter is independent — if both auto-stretch and remediation are enabled, Phase 7 runs.

## Phase 7a: Artifact Detection (CPU)

Detection operates on the stretched 2D image where artifacts have high contrast. Three detectors run sequentially, producing pixel masks.

**Channel strategy:** Detection runs on luminance (mean of RGB channels for color images, or the single channel for mono). Trail and dust artifacts are spatial, not spectral — they affect all channels at the same pixel positions. The resulting masks are shared across all channels during remediation.

### Trail Detector

- Hough transform on the stretched image (same algorithm concept as the old Phase 1c, but on high-contrast stretched data)
- Background subtraction (block median) to isolate trail signal
- Sobel edge detection + threshold
- Hough accumulator with peak detection and clustering
- Dilate detected lines by configurable radius (default 5px)
- Output: binary mask of trail pixels + line parameters (rho, theta) for diagnostics
- Note: detection mask is in pixel coordinate space, shared between linear and stretched images (same geometry, different intensity mapping)

### Dust Mote Detector

- Compute local background brightness using an annular ring around each candidate region
- Flag pixels significantly darker than local background (configurable sigma threshold)
- Cluster flagged pixels into connected components
- Filter by circularity: compute eccentricity of each blob, reject non-circular regions (dark nebulae, galaxy dust lanes have irregular shapes)
- Minimum/maximum blob diameter constraints (dust motes are typically 10-100 pixels)
- With dithered data, optional subcube validation: at dust-candidate pixels, check if attenuation correlates with dither offsets (only some frames affected = dust mote on sensor; all frames affected = real sky feature)
- Output: binary mask of dust pixels + per-blob metadata (center, radius, mean attenuation)

### Vignetting Detector

- Mask bright sources (stars, galaxy cores) and detected artifacts (trails, dust)
- Fit a low-order radial polynomial to the remaining background brightness
- Deviation from flat = vignetting gradient
- Compute per-pixel correction factor: `flat_ratio = fitted_center_brightness / fitted_brightness_at(x,y)`
- Output: per-pixel vignetting correction map (floating point, >= 1.0 everywhere, 1.0 at center)

## Phase 7b: Trail Remediation (GPU)

For each trail pixel (x, y), re-run the statistical pixel selection excluding contaminated frames. Runs per-channel: upload one channel's subcube to VRAM, process all trail pixels for that channel, repeat.

### Algorithm

1. **Allocate masks:** Call `SubCube::allocateMasks()` if not already allocated (Phase 1c no longer does this)
2. **Read Z-column:** Load all N frame values at (x, y) from the subcube (contiguous in memory, column-major layout)
3. **Identify bad frame(s):** Compare each Z value to the column median. Frame(s) with values > median + k*MAD are trail-contaminated (k configurable, default 3.0)
4. **Mask:** Set `SubCube.m_masks(z_bad, y, x) = 1` for identified frames
5. **Re-select:** Run the full `selectBestZ()` pipeline on the masked Z-column:
   - MAD pre-filter (excluding masked frames)
   - ESD outlier detection
   - 4-way distribution fitting (Gaussian, Poisson, Skew-Normal, Bimodal)
   - AICc model selection
   - Median of clean data as output value
   - Quality weights from Phase 2 are passed through
6. **Store:** Write the new selected value to the channel result

### GPU Kernel Design

- New CUDA kernel `trailRemediationKernel` in `CudaRemediation.cu` — **not** a reuse of the existing `pixelSelectionKernel`, since that kernel processes the full image and does not support masks
- The new kernel implements mask-aware `selectBestZ()` device functions (MAD, ESD, distribution fitting, AICc) based on the same mathematical logic as the CPU path and existing GPU device functions, but with mask support added
- One CUDA thread per trail pixel
- Trail pixel coordinates passed as a compact list (not a full-image mask) since trails are sparse (~0.1-1% of pixels)
- Each thread reads its Z-column (stride-1 access pattern, cache-friendly)
- Thread block size: 256
- Distribution fitting runs entirely in registers/local memory
- **CPU fallback:** When no GPU is available, loop over trail pixels on CPU calling `PixelSelector::selectBestZ()` with mask column. The CPU path already supports masks.

## Phase 7c: Dust & Vignetting Remediation (GPU)

For each affected pixel, compute a multiplicative brightness correction from neighboring clean pixels. Runs per-channel, same serial upload strategy as Phase 7b.

### Algorithm

1. **Find clean neighbors:** Starting from the affected pixel (x, y), search outward in a configurable radius (default 5-10 pixels) for pixels that are NOT flagged as dust, trail, star, or vignetting-affected
2. **Read neighbor selected values:** These are the `channelResults[ch]` vectors (row-major float arrays) from Phase 3 — the per-pixel selected values that went through the full statistical pipeline
3. **Compute correction ratio:** `ratio = mean(neighbor_selected_values) / selected_value_at(x, y)`
4. **Clamp ratio:** If `selected_value_at(x, y)` is near zero (deep shadow), clamp the ratio to a configurable maximum (default 10.0) to prevent division-by-near-zero blowup
5. **Apply multiplicatively:** `corrected = selected_value * ratio`
6. **Store:** Write corrected value to channel result

### Dust Mote Specifics

- Dust motes are localized (10-100px diameter), so clean neighbors are nearby
- The correction is sharp at the boundary and smooth inside, matching real dust shadow morphology
- For the center of large dust motes where clean neighbors are distant: compute correction factors at the boundary ring (where clean neighbors exist), then interpolate inward using distance-weighted radial interpolation from the boundary toward the center

### Vignetting Specifics

- Vignetting affects the entire image, so "clean neighbors" don't exist in the traditional sense
- Instead, use the radial polynomial fit from detection as the correction map directly
- Per-pixel correction: `corrected = selected_value * vignetting_correction_factor(x, y)`
- This is a single-pass multiplication kernel over the full image
- **Limitation:** Correcting vignetting post-stack means the distribution fitting in Phase 3 operated on vignetted data. The fitted distributions are biased toward lower values in vignetted regions. This is acceptable because: (a) vignetting is a smooth multiplicative factor, so it shifts distributions uniformly without changing their shape; (b) model selection (AICc) is invariant to uniform scaling; (c) the median selection is also scale-invariant. The post-stack correction recovers the correct absolute brightness.

### GPU Kernel Design

- **Dust kernel:** One thread per dust pixel. Each thread scans a neighborhood stencil, skipping flagged pixels, averages clean neighbor values, computes clamped ratio. Sparse launch (only dust pixels).
- **Vignetting kernel:** One thread per pixel (full image). Each thread reads its selected value and the pre-computed vignetting correction factor, multiplies. Dense launch, very fast.
- **CPU fallback:** Same algorithms in a simple OpenMP parallel loop over affected pixels.

### Overlap Resolution

If a pixel is flagged by multiple detectors (e.g., a trail crosses a dust mote):
1. **Phase 7b runs first** (trail re-selection). This produces a new clean selected value.
2. **Phase 7c runs second** (dust/vignetting correction). If the pixel was also flagged as dust, the correction applies to the Phase 7b output.
3. Order matters: trail remediation produces a statistically valid value from the subcube; dust correction then adjusts its brightness relative to neighbors. This is correct because the trail fix addresses which frame was selected, while the dust fix addresses sensor-level attenuation.

## Phase 7d: Patch and Re-Stretch

1. **Patch linear output:** Write all corrected values (from 7b and 7c) back into the Phase 4 linear PCL Image
2. **Re-stretch:** Re-apply the same stretch algorithm (selected in Phase 5) to the corrected linear image, using the same AutoConfigure parameters from the original stretch
3. **Replace stretched output:** Update the stretched output window with the remediated result

## Data Flow

```
Phase 3 output: channelResults[ch] (row-major float vectors) + per-channel SubCubes
  |
Phase 4-6 (linear output -> auto-stretch selection -> apply stretch)
  |
  v
Stretched 2D image (luminance for detection)
  |
  +---> Trail Detector -----> trail pixel mask
  +---> Dust Mote Detector --> dust pixel mask + attenuation
  +---> Vignetting Detector -> vignetting correction map
  |
  v
GPU Remediation Kernels (per-channel, serial subcube upload)
  |
  +---> Trail pixels: read Z-column from per-channel subcube,
  |     mask bad frame, re-run selectBestZ() with quality weights
  |     -> new selected value
  |
  +---> Dust pixels: read neighbor values from channelResults[ch],
  |     compute clamped ratio, apply multiplicative correction
  |
  +---> Vignetting pixels: apply radial polynomial correction factor
  |
  v
Patched linear image -> re-stretch -> final output
  |
  v
Free per-channel subcubes (no longer needed)
```

## Configuration Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `enableRemediation` | true | Master switch for Phase 7 (requires auto-stretch enabled) |
| `enableTrailRemediation` | true | Trail detection + re-selection |
| `enableDustRemediation` | true | Dust mote detection + correction |
| `enableVignettingRemediation` | true | Vignetting detection + correction |
| `trailDilateRadius` | 5.0 | Pixels to dilate around detected trail lines |
| `trailOutlierSigma` | 3.0 | Sigma threshold for identifying trail frame in Z-column |
| `dustMinDiameter` | 10 | Minimum dust blob diameter (pixels) |
| `dustMaxDiameter` | 100 | Maximum dust blob diameter (pixels) |
| `dustCircularityMin` | 0.7 | Minimum circularity (1.0 = perfect circle) |
| `dustDetectionSigma` | 2.0 | Sigma below local background to flag as dust |
| `dustNeighborRadius` | 10 | Search radius for clean neighbors |
| `dustMaxCorrectionRatio` | 10.0 | Clamp ratio to prevent division-by-near-zero blowup |
| `vignettingPolyOrder` | 3 | Order of radial polynomial for vignetting fit |

## Files to Create/Modify

### New Files

- `src/engine/ArtifactDetector.h/.cpp` — Phase 7a: trail, dust, and vignetting detection on stretched images
- `src/engine/cuda/CudaRemediation.h/.cu` — Phase 7b/7c: GPU kernels for trail re-selection (mask-aware selectBestZ) and dust/vignetting correction

### Modified Files

- `src/NukeXStackInstance.cpp` — Remove Phase 1a/1c calls, store per-channel subcubes in `std::vector<SubCube>`, keep `channelResults` and `qualityWeights` alive, add Phase 7 orchestration
- `src/NukeXStackParameters.h/.cpp` — Add remediation parameters, remove old `enableTrailDetection`/`enableSelfFlat` parameters
- `src/NukeXStackInterface.h/.cpp` — Update GUI: remove old checkboxes, add remediation section
- `src/engine/SubCube.h` — No structural changes; mask infrastructure stays (allocated in Phase 7b on demand)

### Removed Files

- `src/engine/TrailDetector.h/.cpp` — Replaced by ArtifactDetector trail detection
- `src/engine/FlatEstimator.h/.cpp` — Replaced by Phase 7c

### Build System

- `CMakeLists.txt` — Remove TrailDetector/FlatEstimator sources, add ArtifactDetector and CudaRemediation
- `Makefile` — Same changes for direct g++ build

## Testing

- Unit tests for each detector (trail, dust, vignetting) with synthetic test images
- Unit tests for GPU remediation kernels with known subcube data
- Unit test for overlap resolution (trail + dust at same pixel)
- Integration test: full pipeline with synthetic trails injected into test frames
- Regression test: M63 dataset that currently shows surviving trails
- Performance benchmark: GPU remediation kernel timing vs old Phase 1a+1c

## Risks and Mitigations

| Risk | Mitigation |
|------|------------|
| Dust detection flags real dark sky features | Circularity filter + diameter constraints + optional subcube dither validation |
| Vignetting polynomial fits real large-scale structure | Mask bright sources before fitting. Low polynomial order (3) can't fit complex structure. |
| Subcube memory tripled for RGB (~5.4GB) | Within 64GB system RAM. Subcubes freed after Phase 7d. |
| VRAM budget for subcube upload | Serial per-channel processing: ~1.8GB per channel, well within 16GB VRAM |
| Division-by-near-zero in dust correction | Clamp correction ratio to configurable maximum (default 10.0) |
| GPU kernel for mask-aware selectBestZ is new code | Same mathematical logic as CPU path. CPU fallback available. Extensive unit testing. |
| Vignetting correction post-stack biases distributions | Model selection and median are scale-invariant. Post-stack multiplicative correction recovers correct brightness. |
