# NukeX v4 — Additions & Stretch Pipeline Specification
*Design session March 2026 — supplement to core architecture document*

---

## Summary of confirmed design decisions from this session

| Item | Decision |
|---|---|
| Normalization reference | No reference frame. Best pixel selected from fitted distribution at each (x,y). ZDistribution.true_signal_estimate is the normalization anchor. |
| Dithering-aware classifier | Not needed. Distribution fitting catches hot pixels as spike outliers in Z-array without spatial context. |
| Linear fit clipping | Not needed. Distribution curve fitting produces a better answer than any clipping algorithm. No rejection step. |
| Variance map output | Include. Formula specified below. Falls back to Welford variance if FITS keywords absent. |
| PSF quality weight | Include. Computed free from alignment star catalog. Spec below. |
| Stretch pipeline | 11 ops (0–10). All fresh implementations. OTS GPU preferred, CPU fallback. Lumpton/RNC support both mono and joint-RGB modes. |

---

## Addition 1: Per-frame PSF quality weight

### Where it fits
Runs during the streaming frame load, after star detection for alignment, before the frame enters the ring buffer. Zero additional detection cost — reuses the alignment star catalog.

### Data structure
```cpp
struct FramePSFQuality {
    float fwhm_pixels;       // median FWHM across all fitted stars in this frame
    float fwhm_arcsec;       // fwhm_pixels * plate_scale_arcsec_per_pixel
    float eccentricity;      // median axis ratio b/a in [0,1]; 0 = perfect circle
    float snr_estimate;      // median peak/background SNR across fitted stars
    float psf_weight;        // derived weight factor applied to frame_final_weight
};
```

### PSF fitting procedure
For each star in the alignment catalog:
1. Extract a 15×15 pixel cutout centered on the star centroid (sub-pixel centered via bilinear interpolation)
2. Fit an elliptical Moffat profile using Levenberg-Marquardt (lmfit):
   ```
   f(x,y) = A * [1 + ((x-x0)²/αx² + (y-y0)²/αy²)]^(-β) + B
   ```
   Parameters: amplitude A, centroid (x0,y0), semi-axes (αx, αy), Moffat index β, background B
3. FWHM along each axis: `FWHMi = 2 * αi * sqrt(2^(1/β) - 1)`
4. Median FWHM = geometric mean of FWHMx and FWHMy, taken as median across all stars
5. Eccentricity = 1 - (min(αx,αy) / max(αx,αy))

Reject stars for PSF fitting if: peak ADU > 0.85 * saturation level, or fewer than 20 unsaturated pixels in cutout, or fit residual RMS > 15% of peak.

Require minimum 5 valid stars per frame for a reliable PSF measurement. If fewer than 5 valid stars, set `psf_weight = 1.0` (neutral — don't penalize a frame you can't measure).

### Weight computation
```cpp
// Computed after all frames have PSF measurements
// fwhm_best = minimum fwhm_pixels seen across all frames in the session
float fwhm_ratio = fwhm_pixels / fwhm_best;
psf_weight = expf(-0.5f * (fwhm_ratio - 1.0f) * (fwhm_ratio - 1.0f) / 0.25f);
// sigma=0.5 in fwhm_ratio space: best frame=1.0, +50% FWHM -> ~0.88, +100% FWHM -> ~0.61

// Applied multiplicatively to the classification-derived weight:
frame_final_weight[i] = classifications[i].weight * psf_quality[i].psf_weight;
```

`fwhm_best` is a running minimum updated as each frame completes PSF measurement in the ring buffer. For 1000-frame sessions spanning a full night, the best seeing typically occurs early; the running minimum is accurate within the first 50–100 frames.

### Plate scale
Read from FITS header keywords in priority order:
1. `PIXSCALE` (arcsec/pixel, direct)
2. Compute from `FOCALLEN` (mm) and `XPIXSZ` (μm): `plate_scale = 206.265 * XPIXSZ / FOCALLEN`
3. If neither available: store `fwhm_arcsec = 0` (unknown), still compute pixel-space weight

---

## Addition 2: Variance / noise map output

### What it is
A float32 output image, same dimensions as the stacked result, where each pixel value is the estimated 1σ noise at that position in physical units (ADU). Produced alongside the main stacked image and the quality map.

### Formula
For each pixel position (x,y), after weighted combination:

```cpp
float weighted_sum  = 0.f;
float weight_sum    = 0.f;
float variance_sum  = 0.f;

for (int i = 0; i < n_frames; i++) {
    float w = frame_final_weight[i];
    float v = values[i][channel];

    // Per-frame noise variance in ADU²
    // Poisson (shot) noise + read noise, converted to ADU²
    float sigma2;
    if (fits_headers[i].has_noise_keywords) {
        float read_noise = fits_headers[i].read_noise;  // electrons (FITS: RDNOISE)
        float gain       = fits_headers[i].gain;        // e-/ADU  (FITS: GAIN, EGAIN, ISOSPEED)
        sigma2 = (read_noise * read_noise / (gain * gain)) + (v / gain);
    } else {
        // Fallback: use Welford variance of Z-array for this pixel
        sigma2 = welford[x][y][channel].variance();
    }

    weighted_sum  += v * w;
    weight_sum    += w;
    variance_sum  += w * w * sigma2;
}

float result_pixel    = weighted_sum / weight_sum;
float output_variance = variance_sum / (weight_sum * weight_sum);
float output_sigma    = sqrtf(output_variance);  // noise map value at (x,y)
```

### FITS keyword search order for gain/read noise
```
Gain:       GAIN → EGAIN → ISOSPEED (convert ISO to gain via lookup) → 1.0 (fallback)
Read noise: RDNOISE → READNOISE → NOISE → sqrt(DARKCUR) → 3.0 electrons (fallback)
```

### Output
- Filename: `<stack_name>_noise.fits`
- Single channel float32
- FITS header: `BUNIT = 'ADU'`, `COMMENT = 'Per-pixel 1-sigma noise estimate'`
- SNR image (optional, derived): `snr[x,y] = result_pixel[x,y] / output_sigma[x,y]`

---

## Addition 3: Stretch pipeline — complete specification

### Architecture

The pipeline is an **ordered list of enabled StretchOp instances**. User controls position (order) and enabled state per op. The pipeline executes sequentially; each op receives the output of the previous op. All ops work on float32 normalized to [0,1].

```cpp
// Base class
class StretchOp {
public:
    bool    enabled  = false;
    int     position = 0;          // user-assigned order; pipeline sorts by this
    string  name;                  // display name

    // Apply stretch to image in-place. img is float32, normalized [0,1].
    // For mono: nChannels=1. For RGB: nChannels=3.
    virtual void apply(Image& img) const = 0;

    // Apply to a single float value (for LUT pre-computation where applicable)
    virtual float apply_scalar(float x) const { return x; }

    virtual ~StretchOp() = default;
};

// Pipeline container
class StretchPipeline {
public:
    vector<unique_ptr<StretchOp>> ops;

    void execute(Image& img) const {
        // Sort by position, then execute enabled ops in order
        vector<StretchOp*> ordered;
        for (auto& op : ops)
            if (op->enabled) ordered.push_back(op.get());
        sort(ordered.begin(), ordered.end(),
             [](auto a, auto b){ return a->position < b->position; });
        for (auto* op : ordered)
            op->apply(img);
    }

    // Quick preview stretch — always arcsinh, always on a copy, never modifies working data
    static Image quick_preview_stretch(const Image& linear_src, float alpha = 500.f) {
        Image preview = linear_src.clone();
        float norm = asinhf(alpha);
        preview.apply([&](float x){ return asinhf(x * alpha) / norm; });
        return preview;
    }
};
```

### Advisory ordering system

Warn (non-blocking) when the user's ordering would produce suboptimal results:

```cpp
enum class StretchCategory { PRIMARY, SECONDARY, FINISHER };

// Category assignments:
// PRIMARY:   GHS(2), ArcSinh(3), Log(4), Lumpton(5), SAS(9), Veralux(10)
// SECONDARY: Histogram(1), OTS(8), RNC(6), Photometric(7)
// FINISHER:  MTF(0)

// Advisory: warn if any SECONDARY precedes all PRIMARYs
// Advisory: warn if MTF precedes any PRIMARY or SECONDARY
// Never block — user can override
```

---

### Op 0: MTFStretch

**Category:** Finisher  
**Channel mode:** Per-channel (R, G, B independently) or luminance-only (user flag)  
**Default:** Disabled

```cpp
class MTFStretch : public StretchOp {
public:
    float midtone   = 0.25f;   // m: midtone balance point
    float shadows   = 0.0f;    // c0: black point
    float highlights = 1.0f;   // c1: white point

    float apply_scalar(float x) const override {
        // Clip to [shadows, highlights], rescale, then apply MTF
        float xc = clamp((x - shadows) / (highlights - shadows), 0.f, 1.f);
        if (midtone == 0.f) return 0.f;
        if (midtone == 1.f) return 1.f;
        return ((midtone - 1.f) * xc) / (((2.f * midtone - 1.f) * xc) - midtone);
    }
};
```

---

### Op 1: HistogramStretch

**Category:** Secondary  
**Channel mode:** Per-channel, or luminance-only with color preservation (user flag)  
**Default:** Disabled

Computes cumulative histogram (4096 bins), builds monotonic mapping to uniform distribution [0,1]. Clip level (default 0.001%) applied symmetrically at both ends before equalization to prevent noise spike dominance.

```cpp
class HistogramStretch : public StretchOp {
public:
    float clip_fraction = 0.00001f;  // fraction of pixels clipped at each end
    bool  luminance_only = true;     // true: stretch L, preserve hue/saturation
    int   n_bins = 4096;

    void apply(Image& img) const override;
    // For luminance_only=true on RGB: convert to Lab, stretch L, convert back
    // For luminance_only=false: apply independently per channel
};
```

---

### Op 2: GHStretch (Generalized Hyperbolic Stretch)

**Category:** Primary  
**Channel mode:** Per-channel or luminance-only (user flag)  
**Default:** Disabled  
**Reference:** Gharat & Treweek (2021)

The current state-of-the-art soft stretch for DSO data. Recommended as the first primary stretch in most workflows.

```cpp
class GHStretch : public StretchOp {
public:
    float D  = 5.0f;    // stretch intensity
    float b  = 5.0f;    // hyperbolic parameter; b→∞ approaches arcsinh
    float SP = 0.25f;   // symmetry point (focus of stretch)
    float LP = 0.0f;    // local protection — linear below this value
    float HP = 1.0f;    // highlight protection — linear above this value
    bool  luminance_only = false;

    float apply_scalar(float x) const override {
        if (x <= LP) return x;
        if (x >= HP) return x;

        // GHS transformation
        float q = (x - SP);
        float stretch;
        if (fabsf(b) < 1e-6f) {
            stretch = expf(D * q) - 1.f;
        } else {
            stretch = (sinhf(D * (b * q + asinhf(b))) - sinhf(D * asinhf(b))) /
                      (sinhf(D * (b * (1.f - SP) + asinhf(b))) - sinhf(D * asinhf(b)));
        }
        return LP + (HP - LP) * stretch;
    }
};
```

---

### Op 3: ArcSinhStretch

**Category:** Primary  
**Channel mode:** Luminance-only by default (color preserving); per-channel optional  
**Default:** Disabled

Color-preserving by design when applied to luminance. Best primary stretch for OSC data (ASI585MC) where hue preservation of star colors is important.

```cpp
class ArcSinhStretch : public StretchOp {
public:
    float alpha          = 500.f;  // stretch factor; higher = more aggressive
    bool  luminance_only = true;

    float apply_scalar(float x) const override {
        return asinhf(alpha * x) / asinhf(alpha);
    }
};
```

---

### Op 4: LogStretch

**Category:** Primary  
**Channel mode:** Per-channel or luminance-only  
**Default:** Disabled

```cpp
class LogStretch : public StretchOp {
public:
    float alpha          = 1000.f;
    bool  luminance_only = false;

    float apply_scalar(float x) const override {
        return log1pf(alpha * x) / log1pf(alpha);
    }
};
```

---

### Op 5: LumptonStretch (Lupton et al. 2004, SDSS-style)

**Category:** Primary  
**Channel mode:** Joint RGB (default) OR per-channel OR mono — controlled by `channel_mode` flag  
**Default:** Disabled  
**Reference:** Lupton et al. (2004), PASP 116, 133

The defining property: when operating in joint RGB mode, all three channels are mapped through the same arcsinh function of a shared intensity measure. This prevents hue rotation at faint signal levels — critical for blue reflection nebulosity in M16.

```cpp
enum class LumptonMode { JOINT_RGB, PER_CHANNEL, MONO };

class LumptonStretch : public StretchOp {
public:
    float       alpha       = 500.f;
    float       beta        = 0.f;   // softening parameter; 0 = pure arcsinh
    LumptonMode channel_mode = LumptonMode::JOINT_RGB;

    void apply(Image& img) const override {
        if (channel_mode == LumptonMode::JOINT_RGB && img.nChannels() == 3) {
            // Joint: I = (R + G + B) / 3, factor = arcsinh(alpha*I+beta) / (alpha*I+beta)
            for each pixel (r,g,b):
                float I      = (r + g + b) / 3.f;
                float factor = (I > 1e-10f) ?
                    asinhf(alpha * I + beta) / ((alpha * I + beta) * asinhf(alpha)) : 1.f;
                r *= factor; g *= factor; b *= factor;
        } else {
            // Per-channel or mono: independent arcsinh per channel
            apply_per_channel(img, alpha);
        }
    }
};
```

---

### Op 6: RNCStretch (Roger N. Clark)

**Category:** Secondary  
**Channel mode:** Per-channel (default) OR joint RGB OR mono — `channel_mode` flag  
**Default:** Disabled

```cpp
class RNCStretch : public StretchOp {
public:
    float       black_point  = 0.0f;
    float       white_point  = 1.0f;
    float       gamma        = 2.2f;
    LumptonMode channel_mode = LumptonMode::PER_CHANNEL;  // reuse enum

    float apply_scalar(float x) const override {
        float xc = clamp((x - black_point) / (white_point - black_point), 0.f, 1.f);
        return powf(xc, 1.f / gamma);
    }
};
```

---

### Op 7: PhotometricStretch

**Category:** Secondary  
**Channel mode:** Per-channel  
**Default:** Disabled  
**Requirement:** Valid photometric calibration keywords in FITS headers (`BSCALE`, `BZERO`, `MAGZERO` or `PHOTZP`)

Maps ADU pixel values to a perceptual display scale while preserving relative flux ratios. Unlike other stretches, this one is informed by the stacked image's FITS header, not just the pixel values.

```cpp
class PhotometricStretch : public StretchOp {
public:
    float mag_zero_point = 25.0f;  // read from FITS MAGZERO or PHOTZP
    float display_min_mag = 22.0f; // faintest magnitude mapped to 0
    float display_max_mag = 8.0f;  // brightest magnitude mapped to 1
    float flux_scale = 1.0f;       // from FITS BSCALE

    float apply_scalar(float x) const override {
        // x is ADU. Convert to magnitude, then to display [0,1]
        float flux = x * flux_scale;
        if (flux <= 0.f) return 0.f;
        float mag = mag_zero_point - 2.5f * log10f(flux);
        return clamp((display_min_mag - mag) / (display_min_mag - display_max_mag), 0.f, 1.f);
    }
};
```

If FITS photometric keywords are absent, this op should refuse to apply and log a warning rather than silently producing garbage output.

---

### Op 8: OTSStretch (Optimal Transport Stretch)

**Category:** Secondary  
**Channel mode:** Luminance-only (default) or per-channel  
**Default:** Disabled  
**GPU:** OpenCL preferred, CPU fallback

Finds the monotone mapping T that transforms the image histogram to a target distribution (default: perceptually uniform Munsell value scale) using 1D optimal transport. Since OT in 1D reduces to matching quantile functions, the algorithm is:

1. Compute source CDF from image histogram (2048 bins)
2. Compute target CDF from target distribution (pre-baked Munsell/perceptual table)
3. For each bin i: `T(i) = target_quantile(source_cdf(i))`
4. Apply T as a LUT to the image

```cpp
class OTSStretch : public StretchOp {
public:
    int   n_bins          = 2048;
    bool  luminance_only  = true;
    bool  use_gpu         = true;   // OpenCL if available, else CPU

    enum class TargetDistribution {
        MUNSELL_VALUE,     // perceptually uniform (default)
        SQRT,              // square root (gentle)
        UNIFORM,           // pure histogram equalization (same as HistogramStretch)
        GAUSSIAN           // Gaussian target (soft midtone emphasis)
    } target = TargetDistribution::MUNSELL_VALUE;

    void apply(Image& img) const override;

    // OpenCL kernel: histogram accumulation + LUT application
    // Histogram computation: parallel reduce over pixels, one work-item per bin
    // LUT application: embarrassingly parallel, one work-item per pixel
    // CPU fallback: std::for_each with execution_policy::par_unseq
};
```

**OpenCL kernel structure:**
```opencl
// Pass 1: histogram (local memory atomics, then global merge)
kernel void compute_histogram(
    global const float* img, global uint* hist, int n_pixels, int n_bins);

// Pass 2: LUT application
kernel void apply_lut(
    global float* img, global const float* lut, int n_pixels);
```

GPU memory: histogram is 2048 × 4 bytes = 8 KB — fits in local memory on all modern GPUs. LUT is also 8 KB. The image itself fits in VRAM at full resolution. No tiling needed for OTS.

---

### Op 9: SASStretch (Signal-Adaptive Stretch)

**Category:** Primary  
**Channel mode:** Luminance-only (default)  
**Default:** Disabled

Applies spatially and tonally varying stretch parameters: more aggressive stretch in regions where signal is faint (pillar edges, IFN, extended halos), gentler in bright regions (emission core, bright stars). Operates by:

1. Dividing image into overlapping tiles (default 256×256, 50% overlap)
2. Computing local histogram per tile
3. Fitting per-tile GHS parameters that bring local median to a target display value
4. Blending tile stretch maps with a smooth Gaussian weight kernel
5. Applying the spatially-varying map to the full image

```cpp
class SASStretch : public StretchOp {
public:
    int   tile_size      = 256;
    float tile_overlap   = 0.5f;
    float target_median  = 0.25f;  // where to place local background in display space
    float max_stretch    = 20.f;   // cap on local D parameter to prevent noise amplification
    float min_stretch    = 1.f;    // minimum local stretch (don't compress bright regions)
    bool  luminance_only = true;

    void apply(Image& img) const override;
    // Heavy: O(n_tiles × tile_pixels × GHS_iterations)
    // Recommend threading via PCL's thread pool: one tile per thread
};
```

---

### Op 10: VeraluxStretch

**Category:** Primary
**Channel mode:** Per-channel (default) or luminance-only
**Default:** Disabled
**Status:** Fresh implementation for v4 (same algorithm, new code)

Filmic tone curve with configurable toe/shoulder regions. The mapping applies
exposure scaling, then a smooth S-curve with controllable toe (shadow rolloff)
and shoulder (highlight rolloff) regions connected by a linear mid-section.

```cpp
class VeraluxStretch : public StretchOp {
public:
    float exposure         = 1.0f;   // pre-scale multiplier before tone curve
    float contrast         = 1.0f;   // slope of the linear mid-section
    float toeStrength      = 0.5f;   // [0,1] curvature of shadow rolloff
    float shoulderStrength = 0.5f;   // [0,1] curvature of highlight rolloff
    float blackPoint       = 0.0f;   // input value mapped to output 0
    float whitePoint       = 1.0f;   // input value mapped to output 1
    bool  luminance_only   = false;

    float apply_scalar(float x) const override {
        // 1. Apply exposure: x *= exposure
        // 2. Remap to [blackPoint, whitePoint] range
        // 3. Apply toe curve:  smooth quadratic blend for x < toe_threshold
        // 4. Linear mid-section: slope = contrast
        // 5. Apply shoulder curve: smooth quadratic blend for x > shoulder_threshold
        // 6. Clamp [0, 1]
        // Implementation: fresh for v4
    }

    void apply(Image& img) const override;
    // Per-channel or luminance-only based on flag
    // Must handle mono (nChannels=1) and RGB (nChannels=3)
};
```

---

## Pipeline ordering reference

Recommended orderings for common use cases. All are advisory — user can place ops in any order.

### Standard DSO (emission nebula, e.g. M16)
```
1. ArcSinh or GHS  [primary — brings faint pillars up]
2. SAS              [signal-adaptive — preserves pillar edge detail]
3. MTF              [finisher — set black point, refine midtone]
```

### OSC color preservation (ASI585MC)
```
1. Lumpton (joint RGB)  [primary — preserves hue at all signal levels]
2. GHS (luminance only) [secondary primary — refine stretch without hue shift]
3. MTF                  [finisher]
```

### Maximum faint structure (IFN, galaxy halos)
```
1. GHS (aggressive D)   [primary]
2. OTS (Munsell target) [secondary — perceptual uniformity in faint regions]
3. SAS                  [local adaptation]
4. MTF                  [finisher]
```

### Quick inspection (during stacking, classification preview)
```
quick_preview_stretch()   [built-in static method, never modifies working data]
```

---

## Pipeline integration with the rest of NukeX

The stretch pipeline receives its input from the pixel selector output — the float32 stacked image after weighted combination. It does not touch the working cube, the ring buffer, or any per-frame data. It is purely post-stack.

The **quick_preview_stretch** static method is called:
- By the subcube visualizer (for display-only overlay)
- By the classification inspector (to show the user what a pixel looks like stretched)
- During ring buffer processing if the user wants a live preview of partial stacks

The quick preview is always arcsinh with a single parameter, always on a copy, never in-place. This is a hard rule — the working data stays linear throughout.

---

## Veralux implementation notes for v4

Veralux is implemented fresh in v4 — no v3 code is used.

The algorithm is a filmic tone curve with these stages:
1. Exposure scaling (pre-multiply)
2. Black/white point remap to [0,1]
3. Toe region: smooth quadratic rolloff in shadows (controlled by toeStrength)
4. Linear mid-section: slope controlled by contrast parameter
5. Shoulder region: smooth quadratic rolloff in highlights (controlled by shoulderStrength)
6. Output clamp to [0,1]

Must implement both `apply_scalar(float)` and `apply(Image&)` for the StretchOp interface.
Handle mono (nChannels=1) and RGB (nChannels=3) via the luminance_only flag.

---

*End of additions spec*
