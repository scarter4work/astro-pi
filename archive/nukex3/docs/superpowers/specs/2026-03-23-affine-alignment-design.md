# Affine Alignment Design

## Problem

The current aligner extracts only integer (dx, dy) translation from matched star pairs. Multi-night data, meridian flips, and camera rotation changes produce frames that are rotated/flipped relative to the reference — the triangle matcher finds wrong matches and the frames are stacked at wrong positions.

## Solution

Upgrade alignment to solve for a **similarity transform** (rotation + uniform scale + translation + optional flip) from matched star pairs. Resample target frames into reference coordinates using bilinear interpolation. Mask invalid regions per-frame. Reject frames with RMS > threshold or scale outside [0.95, 1.05].

## Design Decisions (approved)

- **Similarity transform** (4 params) not full affine (6 params) — same scope/camera, no shear
- **Bilinear interpolation** — accept minor noise correlation; benefit of including rotated frames far outweighs
- **Per-frame masking** for invalid corners — crop to reference frame footprint, not intersection of all frames
- **Flip detection** — solve twice (normal + horizontally flipped), take lower RMS
- **Frame rejection** — RMS > 2.0 pixels or scale outside [0.95, 1.05]

## Components

### SimilarityTransform struct
```
struct SimilarityTransform {
    double a, b;     // a = s*cos(θ), b = s*sin(θ)
    double tx, ty;   // translation
    bool flipped;    // horizontal flip applied before rotation
    double rms;      // residual RMS of fit
    double rotation; // derived: atan2(b, a) in degrees
    double scale;    // derived: sqrt(a*a + b*b)
};
```

Forward transform: `ref = A * target + t`
```
ref_x = a * tgt_x - b * tgt_y + tx
ref_y = b * tgt_x + a * tgt_y + ty
```

### Transform Solver
Input: N matched star pairs (ref_i, target_i), minimum N=2.
Method: Least-squares via normal equations (2x2 system for [a,b], then tx/ty from residuals).
Flip detection: run solver twice — original coords and flipped coords (W - x, y). Take lower RMS.
Output: SimilarityTransform.

### Image Resampler
For each output pixel (x_ref, y_ref):
1. Compute inverse transform → (x_src, y_src) in target frame
2. If outside bounds → mask = 1 (invalid)
3. Otherwise → bilinear interpolation from 4 neighbors
Applied per-channel independently.

### Updated FrameAligner Flow
1. Detect stars (unchanged)
2. Match triangles (unchanged — already rotation-invariant)
3. **New:** Solve similarity transform from matched pairs (with flip detection)
4. **New:** Reject frame if RMS > 2.0 or scale outside [0.95, 1.05]
5. **New:** Resample frame using inverse transform + bilinear
6. **New:** Set validity mask for out-of-bounds pixels
7. Crop to reference frame footprint (not intersection)

### AlignmentResult Changes
Add: rotation (degrees), scale, flipped (bool). Keep dx/dy for logging (derived from tx/ty).

### Crop Region
Reference frame defines the crop bounds. Frames with pure translation may still shrink the crop (as before). Rotated frames contribute valid pixels where the transform maps inside the source, and masked pixels elsewhere.
