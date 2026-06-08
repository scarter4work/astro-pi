# NukeX v4 — Complete Architecture & Implementation Specification
*Master document for Claude Code. Everything needed to implement from scratch.*
*Synthesized from design sessions: January–March 2026*

---

## 0. Project context

NukeX is a **PixInsight Process Module** (PCL/C++) for astronomical image stacking.
It is NOT a script. It compiles to a `.so` / `.dylib` / `.dll` loaded by PixInsight
at startup and appears as a first-class process in the Process menu.

**Development environment:**
- Language: C++17, PCL framework
- Build: PCL Makefile Generator (standard for PI modules)
- GPU: OpenCL (cross-vendor: AMD/Intel/NVIDIA) with CPU fallback on all GPU paths
- Linear algebra: Eigen 3.4+ (header-only, PCL already vendors it)
- Curve fitting: lmfit (single .h + .c, vendor directly) or Ceres tiny-solver
- Model selection: StatsLib (kthohr, header-only) for log-likelihood / AIC/BIC
- Star detection / PSF: custom implementation, no PI built-in tools
- FITS I/O: CFITSIO (already in PCL)
- A custom PJSR language server exists that gives Claude Code accurate PCL API
  signatures — use it aggressively to avoid hallucinating PCL APIs

**What NukeX does that no other stacker does:**
- Fits a statistical distribution curve to each pixel's Z-array across all frames
- Uses the distribution shape to drive pixel selection strategy — not rejection
- Keeps all frames, weights intelligently, nothing is ever discarded
- Handles 1000+ frames via streaming ring buffer without loading all into RAM
- Custom star-field homography alignment (no PI StarAlignment — it rejects frames)
- Meridian flip correction baked into H matrix, no separate processing path
- Unified single cube for all channels (no 3× RGB duplication)

---

## 1. Core philosophy — critical for Claude Code to internalize

**There is no rejection.** The word "rejection" should not appear in the code.
Every frame, every pixel contributes. Contaminated pixels contribute at reduced weight.
Near-zero weight is not rejection — it is very low contribution.

**The Z-array is not a time series.** It is a depth stack. Frame ordering matters only
for temporal consistency checks. The combiner treats it as a weighted set.

**Classification is annotation, not filtering.** All four classifiers run on every pixel.
Their results annotate the `PixelClassification` struct. Nothing is discarded.
Annotation feeds weight computation. Weight computation feeds the combiner.

**Normalization comes from the cube itself.** The `ZDistribution.true_signal_estimate`
at each pixel is the normalization anchor. There is no reference frame.

**The working data stays linear throughout.** Any stretch applied for display or
classification preview operates on a copy. The cube values are never stretched in-place.

---

## 2. Data structures

### 2.1 PixelClassification

One instance per frame slot per pixel position. Stores all classifier outputs.

```cpp
struct PixelClassification {
    // Continuous scores (raw classifier outputs)
    float sigma_score;      // deviation from Z-array median in sigma units
    float lum_ratio;        // pixel_value / frame_median; cloud: < 0.85 typical
    float gradient_mag;     // local Sobel gradient magnitude
    float temp_delta;       // |value - median(Z-array)|, temporal consistency

    // Boolean flags derived from scores + user thresholds
    bool  is_cloud;         // lum_ratio below cloud_threshold
    bool  is_trail;         // gradient spike + anomalously bright vs other frames
    bool  is_outlier;       // sigma_score beyond sigma_threshold
    bool  is_consistent;    // temp_delta within temporal_threshold

    // Final weight — what the combiner actually uses
    // [0.0, 1.0]. Never exactly 0 unless weight_floor = 0.
    float weight;
};
```

### 2.2 ZDistribution

One instance per channel per pixel position. Stores the fitted distribution.

```cpp
enum class DistributionShape : uint8_t {
    GAUSSIAN      = 0,   // clean data, single peak
    BIMODAL       = 1,   // two populations (cloud frames vs clear frames)
    SKEWED_LOW    = 2,   // left tail — partial cloud attenuation
    SKEWED_HIGH   = 3,   // right tail — partial trail contamination
    SPIKE_OUTLIER = 4,   // clean Gaussian + isolated spike (trail/cosmic ray)
    UNIFORM       = 5,   // no structure — noise floor or detector defect
    UNKNOWN       = 6    // fit failed or N too small to fit
};

struct GaussianParams   { float mu, sigma, amplitude; };
struct BimodalParams    { GaussianParams comp1, comp2; float mixing_ratio; };
struct SkewNormalParams { float mu, sigma, alpha; };  // alpha = skewness

struct ZDistribution {
    DistributionShape shape;

    union {
        GaussianParams   gaussian;
        BimodalParams    bimodal;
        SkewNormalParams skew_normal;
        GaussianParams   spike_main;   // SPIKE_OUTLIER: the clean Gaussian component
    } params;

    float spike_value;          // spike pixel value (valid when shape==SPIKE_OUTLIER)
    uint8_t spike_frame_index;  // which frame index the spike is in

    // Goodness of fit
    float r_squared;
    float aic;                  // Akaike Information Criterion: 2k - 2*ln(L)
    float bic;                  // Bayesian IC

    // What the pixel selector actually uses
    float true_signal_estimate; // best estimate of the real sky value at this pixel
    float signal_uncertainty;   // 1-sigma uncertainty on that estimate
    float confidence;           // [0,1] — how trustworthy is this pixel position
};
```

### 2.3 PixelStack

One instance per (x, y) position. The fundamental unit of the cube.

```cpp
// Maximum bounds — actual sizes set at runtime from loaded frames
static constexpr int MAX_FRAMES   = 1024;
static constexpr int MAX_CHANNELS = 3;     // 1 for mono, 3 for OSC

struct PixelStack {
    // Interleaved: values[frame][channel]
    // channel 0=R, 1=G, 2=B (or 0=L for mono)
    float values[MAX_FRAMES][MAX_CHANNELS];

    // ONE classification per frame — NOT per channel.
    // Cloud/trail is a frame-level event affecting all channels simultaneously.
    PixelClassification classifications[MAX_FRAMES];

    // ONE distribution fit per channel — but computed using the same frame weights.
    // Channels have different SNR and sky background, so distributions differ.
    // Frame selection is shared.
    ZDistribution distributions[MAX_CHANNELS];

    uint16_t n_frames;    // actual number of frames loaded for this pixel
    uint8_t  n_channels;  // 1 for mono, 3 for OSC
};
```

**Memory layout note:** For a 6000×4000 OSC image with 1000 frames:
- `values`: 6000 × 4000 × 1000 × 3 × 4 bytes = 288 GB — DO NOT store all frames
- Use Welford accumulators + histograms instead (see Section 4 on streaming)
- `PixelStack` in the live cube stores accumulators, not raw frame values
- Raw frame values only exist transiently in the ring buffer slots

### 2.4 WelfordAccumulator

Replaces the raw values array in the live cube. Updated incrementally as each frame
arrives. Numerically stable one-pass mean + variance.

```cpp
struct WelfordAccumulator {
    float    mean    = 0.f;
    float    M2      = 0.f;   // sum of squared deviations
    float    min_val = FLT_MAX;
    float    max_val = -FLT_MAX;
    uint32_t n       = 0;

    void update(float x) {
        n++;
        float delta  = x - mean;
        mean        += delta / n;
        float delta2 = x - mean;
        M2          += delta * delta2;
        min_val      = fminf(min_val, x);
        max_val      = fmaxf(max_val, x);
    }

    float variance() const { return (n > 1) ? M2 / (n - 1) : 0.f; }
    float std_dev()  const { return sqrtf(variance()); }
};
```

### 2.5 PixelHistogram

16-bin histogram per pixel per channel, for distribution shape detection.
Accumulated alongside Welford. 16 bins is sufficient for Gaussian/bimodal/skew
discrimination with N ≤ 1000 frames.

```cpp
struct PixelHistogram {
    static constexpr int N_BINS = 16;
    uint32_t bins[N_BINS] = {};
    float    range_min    = 0.f;
    float    range_max    = 1.f;

    void update(float x) {
        int bin = static_cast<int>(
            (x - range_min) / (range_max - range_min) * N_BINS);
        bin = std::clamp(bin, 0, N_BINS - 1);
        bins[bin]++;
    }

    // Called after the first frame to set the range based on the data
    void initialize_range(float observed_min, float observed_max) {
        range_min = observed_min - 0.05f * (observed_max - observed_min);
        range_max = observed_max + 0.05f * (observed_max - observed_min);
    }
};
```

### 2.6 FramePSFQuality

Per-frame quality metrics derived from the alignment star catalog.

```cpp
struct FramePSFQuality {
    float fwhm_pixels;    // median FWHM across fitted stars in this frame
    float fwhm_arcsec;    // fwhm_pixels * plate_scale; 0 if plate scale unknown
    float eccentricity;   // median axis ratio b/a in [0,1]; 0 = perfect circle
    float snr_estimate;   // median peak/background SNR across fitted stars
    float psf_weight;     // derived weight: exp(-0.5 * ((fwhm/fwhm_best - 1)/0.5)²)
};
```

### 2.7 FrameMetadata

FITS header data extracted per frame during loading.

```cpp
struct FrameMetadata {
    // Noise/calibration
    float read_noise    = 3.0f;   // electrons (FITS: RDNOISE)
    float gain          = 1.0f;   // e-/ADU   (FITS: GAIN, EGAIN)
    float exposure      = 0.f;    // seconds  (FITS: EXPTIME)
    float temperature   = 0.f;    // Celsius  (FITS: CCD-TEMP, FOCTEMP)

    // Astrometry / pointing
    float plate_scale   = 0.f;    // arcsec/pixel (FITS: PIXSCALE or derived)
    float altitude      = 0.f;    // degrees above horizon (FITS: OBJCTALT)

    // Flags
    bool  is_meridian_flipped = false;  // detected during alignment
    bool  has_noise_keywords  = false;  // RDNOISE + GAIN both present
    bool  has_plate_scale     = false;

    // Frame index in the session (0-based, arrival order)
    int   frame_index = 0;
};
```

---

## 3. The live cube (accumulated statistics)

The live cube stores accumulated statistics, not raw pixels. It is built incrementally
as frames stream through the ring buffer.

```cpp
struct LiveCube {
    int width, height, n_channels;

    // Per pixel per channel: running statistics
    // Layout: [y * width + x][channel]
    std::vector<std::array<WelfordAccumulator, MAX_CHANNELS>> welford;
    std::vector<std::array<PixelHistogram,     MAX_CHANNELS>> histograms;

    // Per pixel: one classification per frame (accumulated)
    // Storage: frame classifications are accumulated into per-pixel summary stats
    // Full per-frame classification per pixel would be MAX_FRAMES × W × H structs
    // For 1000 frames × 24MP: 24 billion PixelClassification structs — too large
    // Solution: accumulate frame-level quality into per-frame vectors (not per-pixel)
    std::vector<FrameMetadata>   frame_metadata;    // [n_frames]
    std::vector<FramePSFQuality> frame_psf;         // [n_frames]

    // Per-frame luminance statistics (for cloud detection — frame-level lum_ratio)
    std::vector<float> frame_median_luminance;      // [n_frames]

    // After all frames processed: filled by distribution fitting pass
    // Layout: [y * width + x][channel]
    std::vector<std::array<ZDistribution, MAX_CHANNELS>> distributions;
    std::vector<std::array<float,         MAX_CHANNELS>> output_pixels;
    std::vector<float>                                   noise_map;

    int n_frames_loaded = 0;
};
```

---

## 4. Streaming frame loader

### 4.1 Ring buffer

Never more than 8 frames in RAM simultaneously. The loader thread fills slots;
the processing thread drains them.

```cpp
struct FrameRingBuffer {
    static constexpr int SLOTS = 8;

    struct Slot {
        Image           pixels;       // raw debayered float32 frame
        FrameMetadata   metadata;
        std::atomic<bool> ready{false};
        std::atomic<bool> free{true};
    };

    Slot slots[SLOTS];
    std::atomic<int> head{0};  // next slot for loader to fill
    std::atomic<int> tail{0};  // next slot for processor to drain

    // Loader thread: reads FITS, debayers if needed, writes into next free slot
    // Processing thread: accumulates statistics into LiveCube, then marks slot free
};
```

### 4.2 Frame processing order

For each frame as it arrives in a ring buffer slot:

```
1. Read FITS header → populate FrameMetadata
2. Detect meridian flip (see Section 6)
3. Detect stars → build star catalog for this frame
4. Measure PSF quality from star catalog → populate FramePSFQuality
5. Compute H matrix (homography to reference frame) via star matching + RANSAC
6. Apply H matrix to frame pixels (bilinear interpolation, in-place in slot)
7. Compute frame_median_luminance for this frame
8. Accumulate into LiveCube: update Welford accumulators + histograms per pixel
9. Store FrameMetadata, FramePSFQuality, frame_median_luminance in LiveCube vectors
10. Mark ring buffer slot as free
```

### 4.3 Memory budget at peak (1000 frames, 24MP OSC)

```
Ring buffer (8 slots):     8 × 24MP × 3ch × 4B = 2.3 GB  (peak in-flight)
LiveCube Welford:           24MP × 3ch × 5 floats × 4B = 1.4 GB
LiveCube histograms:        24MP × 3ch × 16 × 4B = 1.1 GB
Frame metadata vectors:     1000 × ~100B = negligible
Distributions (post-fit):   24MP × 3ch × ~80B = 5.8 GB (tiled — not all at once)
Output pixels + noise map:  24MP × 4ch × 4B = 0.4 GB
Total peak:                 ~7 GB host RAM (distributions are tiled, not all live)
```

The distribution fitting pass runs **tile by tile** (512×512 tiles) after all frames
are accumulated. Each tile fits in VRAM on the 5070 Ti (16 GB). Process tile, write
distribution summary, free tile memory, move to next tile.

---

## 5. Classification engine

### 5.1 Overview

Four classifiers run on each pixel position using the accumulated LiveCube data.
Because raw per-pixel per-frame values are not stored (only Welford + histogram),
classification operates differently depending on the classifier:

- **Sigma clipping**: uses Welford mean + std_dev, computed per frame when frame
  is in the ring buffer (before accumulation into Welford — capture per-frame value
  and median during the streaming pass)
- **Luminance threshold**: uses frame_median_luminance (stored per frame)
- **Gradient detection**: computed during streaming pass, per frame
- **Temporal consistency**: uses Welford std_dev as proxy for consistency spread

**Implementation note:** Per-pixel per-frame raw values ARE needed for distribution
fitting. Use reservoir sampling (K=64) per pixel during the streaming pass to retain
a representative sample for the fitting step. This is the only persistent per-frame
data per pixel.

```cpp
// Add to LiveCube:
// Reservoir sample: K=64 values per pixel per channel, sampled from all N frames
struct ReservoirSample {
    static constexpr int K = 64;
    float values[K];
    int   frame_indices[K];  // which frame each sample came from
    int   count = 0;         // how many have been sampled so far
    std::mt19937 rng;

    void update(float x, int frame_idx) {
        if (count < K) {
            values[count] = x;
            frame_indices[count] = frame_idx;
            count++;
        } else {
            // Reservoir sampling: replace with probability K/n
            int j = rng() % (count + 1);
            if (j < K) {
                values[j] = x;
                frame_indices[j] = frame_idx;
            }
            count++;
        }
    }
};
// Layout: [y * width + x][channel]
std::vector<std::array<ReservoirSample, MAX_CHANNELS>> reservoir;
```

### 5.2 Weight computation

After all classifiers produce their flags and scores:

```cpp
struct WeightConfig {
    float cloud_weight_penalty      = 0.30f;
    float trail_weight_penalty      = 0.05f;
    float outlier_weight_penalty    = 0.40f;
    float inconsistent_penalty      = 0.50f;
    float sigma_scale               = 2.0f;
    float weight_floor              = 0.01f;  // never truly zero
};

float compute_weight(const PixelClassification& c,
                     const WeightConfig& cfg)
{
    float w = 1.0f;
    if (c.is_cloud)        w *= cfg.cloud_weight_penalty;
    if (c.is_trail)        w *= cfg.trail_weight_penalty;
    if (c.is_outlier)      w *= cfg.outlier_weight_penalty;
    if (!c.is_consistent)  w *= cfg.inconsistent_penalty;

    // Continuous sigma score contribution (in addition to flags)
    float sigma_factor = expf(-0.5f * c.sigma_score * c.sigma_score /
                              (cfg.sigma_scale * cfg.sigma_scale));
    w *= sigma_factor;

    return fmaxf(w, cfg.weight_floor);
}
```

---

## 6. Alignment — custom star-field homography

**No PI built-in alignment tools are used anywhere.** They reject frames.

### 6.1 Star detection

For each frame in the ring buffer:
1. Compute a quick-preview arcsinh stretch (on a copy) to make stars detectable
2. Find local maxima above SNR threshold (threshold = median + 5 * MAD)
3. Fit 2D Gaussian to each candidate centroid (sub-pixel accuracy)
4. Keep brightest N stars (N = min(200, detected_count))

### 6.2 PSF measurement from star catalog

For each detected star (reuse the catalog from 6.1 — zero extra cost):
1. Extract 15×15 pixel cutout centered on sub-pixel centroid
2. Fit elliptical Moffat profile via LM:
   `f(x,y) = A * [1 + ((x-x0)²/αx² + (y-y0)²/αy²)]^(-β) + B`
3. FWHM_i = `2 * αi * sqrt(2^(1/β) - 1)`
4. Reject stars: saturated (peak > 0.85 × saturation), poor fit residual > 15%
5. `FramePSFQuality.fwhm_pixels` = geometric mean FWHM, taken as median across stars
6. `FramePSFQuality.eccentricity` = 1 - min(αx,αy)/max(αx,αy), median across stars
7. Require minimum 5 valid stars. If fewer: set psf_weight = 1.0 (neutral)

**Plate scale** (for fwhm_arcsec):
- First try FITS keyword PIXSCALE (arcsec/pixel, direct)
- Then derive from FOCALLEN (mm) + XPIXSZ (μm): `206.265 * XPIXSZ / FOCALLEN`
- If unavailable: store fwhm_arcsec = 0, still compute pixel-space weight

### 6.3 Homography computation

```
1. Match star centroids to reference frame catalog (nearest-neighbor, max distance 5px)
2. RANSAC: 500 iterations, 8-DOF projective H, inlier threshold 1.5px
   Require minimum 8 matched pairs. If fewer: mark frame as alignment_failed.
   alignment_failed frames still enter the cube with weight reduced by 0.5 —
   they are NOT discarded.
3. Final H: DLT solve on inlier set only
4. Apply H to frame pixels: bilinear interpolation, border pixels get weight 0
```

Reference frame: first successfully aligned frame. Its H = identity.

### 6.4 Meridian flip detection and correction

```cpp
bool is_meridian_flipped(const cv::Mat& H) {
    // Extract rotation angle from H
    double angle = atan2(H.at<double>(1,0), H.at<double>(0,0)) * 180.0 / M_PI;
    return fabs(fabs(angle) - 180.0) < 10.0;
}

Mat correct_meridian_flip(const Mat& H, int width, int height) {
    // Pre-multiply H with 180-degree rotation about image center
    Mat flip = (Mat_<double>(3,3) <<
        -1,  0, width - 1,
         0, -1, height - 1,
         0,  0, 1);
    return flip * H;
}
```

Meridian-flipped frames are corrected at ingest. The corrected H is stored and used
for all subsequent processing. The flag `FrameMetadata.is_meridian_flipped = true`
is set for records only. No separate cube, no separate branch. After correction,
a flipped frame is identical to any other frame in the pipeline.

### 6.5 PSF weight computation

```cpp
// After all frames have PSF measurements — called once, after streaming is complete
float fwhm_best = *std::min_element(
    frame_psf.begin(), frame_psf.end(),
    [](const auto& a, const auto& b){ return a.fwhm_pixels < b.fwhm_pixels; }
).fwhm_pixels;

for (auto& psf : frame_psf) {
    float ratio = psf.fwhm_pixels / fwhm_best;
    // Gaussian penalty: sigma=0.5 in fwhm_ratio space
    // Best frame: weight=1.0. +50% FWHM: ~0.88. +100% FWHM: ~0.61
    psf.psf_weight = expf(-0.5f * (ratio - 1.f) * (ratio - 1.f) / 0.25f);
}
```

Final frame weight combining classification + PSF:
```cpp
// Applied during the pixel selection / combiner pass
float frame_final_weight(int frame_idx, const PixelClassification& cls) {
    return cls.weight * frame_psf[frame_idx].psf_weight;
}
```

---

## 7. Distribution fitting

Runs after all frames are accumulated. Operates tile by tile (512×512) for memory
efficiency. For each pixel, uses the reservoir sample (K=64 values) as input.

### 7.1 Model selection sequence

Try models in order of complexity (cheapest first). Use AIC to select.
AIC = 2k − 2·ln(L) where k = free parameters, L = likelihood.
Lower AIC wins.

```
1. Gaussian          (k=2: μ, σ)
2. Skew-normal       (k=3: μ, σ, α)
3. Bimodal Gaussian  (k=5: μ1, σ1, A1, μ2, σ2)
4. Spike + Gaussian  (k=3 for main Gaussian + spike location)
5. Uniform           (k=0, fallback — always valid)
```

Stop when AIC stops improving by more than 2.0 units. With N=64 sample points,
penalize complex models aggressively to avoid overfitting.

### 7.2 Signal extraction by shape

| Shape         | true_signal_estimate        | signal_uncertainty | confidence           |
|---------------|-----------------------------|--------------------|----------------------|
| GAUSSIAN      | μ                           | σ                  | R²                   |
| BIMODAL       | μ of dominant peak          | σ of dominant peak | R² × (1 − minor_mix)|
| SKEWED_LOW    | mode (not mean)             | MAD                | R² × 0.7             |
| SKEWED_HIGH   | mode                        | MAD                | R² × 0.8             |
| SPIKE_OUTLIER | μ of main Gaussian          | σ of main          | R²                   |
| UNIFORM       | median of reservoir         | IQR/2              | 0.2                  |

For bimodal: dominant peak = higher value peak AND more frames assigned to it.
If these disagree (more frames in lower peak, which would be cloud), lower confidence.

### 7.3 Pixel selector

```cpp
float select_pixel_value(int x, int y, int channel,
                         const LiveCube& cube,
                         const WeightConfig& weight_cfg)
{
    const ZDistribution& dist = cube.distributions[y*W+x][channel];
    const ReservoirSample& rs = cube.reservoir[y*W+x][channel];

    float weighted_sum = 0.f, weight_sum = 0.f;

    for (int i = 0; i < rs.count && i < ReservoirSample::K; i++) {
        int   fi = rs.frame_indices[i];
        float v  = rs.values[i];

        // Get classification-derived weight for this frame
        // (Frame-level classification: cloud/trail detected during streaming pass)
        float w = frame_final_weight(fi, get_frame_classification(fi, dist));

        // Shape-based selection modulation
        switch (dist.shape) {
        case DistributionShape::GAUSSIAN:
            // Down-weight values beyond 2σ from μ
            {
                float sigma_dev = fabsf(v - dist.params.gaussian.mu)
                                  / dist.params.gaussian.sigma;
                w *= expf(-0.5f * sigma_dev * sigma_dev);
            }
            break;
        case DistributionShape::BIMODAL:
            // Weight by posterior probability of belonging to signal peak
            w *= posterior_signal_probability(v, dist.params.bimodal);
            break;
        case DistributionShape::SPIKE_OUTLIER:
            // Zero out the spike frame
            if (fi == dist.spike_frame_index) w = weight_cfg.weight_floor;
            break;
        default:
            break;
        }

        weighted_sum += v * w;
        weight_sum   += w;
    }

    return (weight_sum > 1e-10f) ? weighted_sum / weight_sum : dist.true_signal_estimate;
}
```

---

## 8. Output images

NukeX produces three output images for every stack:

### 8.1 Stacked image
Float32, same dimensions as input frames. The main deliverable.

### 8.2 Quality map
Float32, 4 channels:
- Channel 0: `true_signal_estimate` (normalized [0,1])
- Channel 1: `signal_uncertainty`
- Channel 2: `confidence` [0,1]
- Channel 3: `shape` (cast from DistributionShape enum, [0,6])

This is a diagnostic image. The confidence channel shows spatially where the data
was weakest — cloud regions will show lower confidence. Viewable in PI as any FITS.

### 8.3 Noise map
Float32, 1 channel (or 3 for RGB). Per-pixel 1σ noise estimate in ADU.

```cpp
// For each pixel (x, y), channel c:
float weighted_sum  = 0.f;
float weight_sum    = 0.f;
float variance_sum  = 0.f;

for (int i = 0; i < rs.count; i++) {
    int   fi = rs.frame_indices[i];
    float v  = rs.values[i];
    float w  = frame_final_weight(fi, ...);

    float sigma2;
    if (frame_metadata[fi].has_noise_keywords) {
        float rn   = frame_metadata[fi].read_noise;  // electrons
        float gain = frame_metadata[fi].gain;        // e-/ADU
        sigma2 = (rn * rn / (gain * gain)) + (v / gain);
    } else {
        sigma2 = cube.welford[y*W+x][c].variance();
    }

    weighted_sum  += v * w;
    weight_sum    += w;
    variance_sum  += w * w * sigma2;
}

float result_pixel  = weighted_sum / weight_sum;
float output_sigma  = sqrtf(variance_sum / (weight_sum * weight_sum));
// noise_map[y*W+x] = output_sigma
```

FITS keywords for noise map: `BUNIT = 'ADU'`

FITS keyword search order for gain/read noise:
- Gain: GAIN → EGAIN → 1.0 (fallback)
- Read noise: RDNOISE → READNOISE → NOISE → 3.0 electrons (fallback)

---

## 9. Statistical library usage

| Purpose                          | Library              | Notes                                    |
|----------------------------------|----------------------|------------------------------------------|
| Robust scale (σ estimator)       | `pcl::BiweightMidvariance` | Use everywhere instead of std variance  |
| Robust scale (small N)           | `pcl::Sn`, `pcl::Qn`       | Better than BWMV for N < 30            |
| MAD                              | `pcl::MAD`                 | For cloud threshold anchor              |
| Median                           | `pcl::Median`              | Parallelized for large N in PCL        |
| Matrix math (fitting, H matrix)  | Eigen 3.4+                 | Header-only, PCL already vendors it    |
| LM curve fitting (distributions) | lmfit (vendor .h + .c)     | Sized for small N, simple API          |
| Log-likelihood for AIC/BIC       | StatsLib (kthohr)          | Header-only, `normal_log_pdf`          |
| RANSAC homography                | Custom (see Section 6)     | No OpenCV dependency required          |
| GPU kernels                      | OpenCL                     | CPU fallback via std::for_each par     |

**Replace all `std::` statistics** with PCL equivalents:
`std::variance` → `pcl::BiweightMidvariance`
`std::sort` (for median) → `pcl::Median` or `pcl::OrderStatistic`

---

## 10. GPU architecture (OpenCL)

Three kernel passes, run per 512×512 tile:

**Pass 1 — classify_pixels_kernel**
Input: reservoir sample values, frame weights, frame metadata
Output: classification flags + scores written back to reservoir metadata
Parallelism: one work-item per (pixel, frame) pair

**Pass 2 — compute_weights_kernel**
Input: classification structs
Output: weight values
Parallelism: one work-item per (pixel, frame) pair
Separated from Pass 1 so weight thresholds can be tuned without re-classifying.

**Pass 3 — stack_pixels_kernel**
Input: reservoir values + weights
Output: result image, noise map
Parallelism: one work-item per pixel (reduction along frame axis)

**CPU fallback:** Same three logical passes, `std::for_each` with
`std::execution::par_unseq` policy. Identical numerical results.

---

## 11. PCL Process Module structure

NukeX is a standard PCL Process Module. Key classes:

```
NukeXModule         : pcl::MetaModule        — module registration
NukeXProcess        : pcl::MetaProcess       — process metadata
NukeXInstance       : pcl::ProcessImplementation — core algorithm
NukeXInterface      : pcl::ProcessInterface  — UI panel
NukeXParameters     : pcl::ProcessParameter  — user-tunable values (one class per param)
```

**NukeXInstance::ExecuteOn(View& view)** is the main entry point.
The view passed in is typically a dummy or the first loaded frame.
NukeX opens its own file dialog for the frame list.

**NukeXInterface** hosts the PCL `Control` subclass for the subcube visualizer
(see Section 12). The interface panel is docked in PI's standard module panel area.

All parameters are exposed as `pcl::ProcessParameter` subclasses and serialized
to `.xpsm` process icon files automatically by PCL — no custom serialization needed.

### 11.1 ProcessParameter registry

Naming convention: `NX4` prefix. v3 used `NXS` for the stretch-only process;
v4 is a unified process so uses `NX4` to avoid collisions if both are loaded.

Every parameter below becomes a `pcl::MetaFloat`, `pcl::MetaInt32`, `pcl::MetaBoolean`,
or `pcl::MetaEnumeration` subclass in `NukeXParameters.h`.

#### Classification & weight parameters

| Parameter class         | ID string                 | Type       | Default | Min   | Max   | Precision |
|-------------------------|---------------------------|------------|---------|-------|-------|-----------|
| NX4CloudWeightPenalty   | "cloudWeightPenalty"      | MetaFloat  | 0.30    | 0.0   | 1.0   | 3         |
| NX4TrailWeightPenalty   | "trailWeightPenalty"      | MetaFloat  | 0.05    | 0.0   | 1.0   | 3         |
| NX4OutlierWeightPenalty | "outlierWeightPenalty"    | MetaFloat  | 0.40    | 0.0   | 1.0   | 3         |
| NX4InconsistentPenalty  | "inconsistentPenalty"     | MetaFloat  | 0.50    | 0.0   | 1.0   | 3         |
| NX4SigmaScale           | "sigmaScale"              | MetaFloat  | 2.0     | 0.5   | 10.0  | 2         |
| NX4WeightFloor          | "weightFloor"             | MetaFloat  | 0.01    | 0.0   | 0.5   | 4         |

#### Classification thresholds

| Parameter class            | ID string                | Type       | Default | Min   | Max    | Precision |
|----------------------------|--------------------------|------------|---------|-------|--------|-----------|
| NX4CloudThreshold          | "cloudThreshold"         | MetaFloat  | 0.85    | 0.5   | 1.0    | 3         |
| NX4SigmaThreshold          | "sigmaThreshold"         | MetaFloat  | 3.0     | 1.5   | 10.0   | 2         |
| NX4TemporalThreshold       | "temporalThreshold"      | MetaFloat  | 2.5     | 0.5   | 10.0   | 2         |

`temporalThreshold`: multiples of Welford σ — temp_delta within this many σ → is_consistent.

There is no trail gradient threshold. Trail detection is handled entirely by
the distribution fitting engine: a satellite trail produces a SPIKE_OUTLIER shape
in the Z-distribution, and the spike frame receives weight_floor. No separate
trail detector exists. The distribution fit is the answer.

#### Alignment parameters

| Parameter class          | ID string               | Type        | Default | Min  | Max   |
|--------------------------|-------------------------|-------------|---------|------|-------|
| NX4StarSNRMultiplier     | "starSNRMultiplier"     | MetaFloat   | 5.0     | 3.0  | 20.0  |
| NX4MaxStars              | "maxStars"              | MetaInt32   | 200     | 20   | 500   |
| NX4RANSACIterations      | "ransacIterations"      | MetaInt32   | 500     | 100  | 2000  |
| NX4RANSACInlierThreshold | "ransacInlierThreshold" | MetaFloat   | 1.5     | 0.5  | 5.0   |

#### Output options

| Parameter class         | ID string              | Type         | Default |
|-------------------------|------------------------|--------------|---------|
| NX4OutputNoiseMap       | "outputNoiseMap"       | MetaBoolean  | true    |
| NX4OutputQualityMap     | "outputQualityMap"     | MetaBoolean  | true    |

#### Stretch pipeline parameters

Each stretch op's parameters are namespaced by op prefix. All ops default `enabled = false`.
The pipeline ordering (`position`) and `enabled` state are stored per op.

**Per-op enable + position (11 ops × 2 params = 22 parameters):**

| Pattern                    | Type         | Default        |
|----------------------------|--------------|----------------|
| NX4{Op}Enabled             | MetaBoolean  | false          |
| NX4{Op}Position            | MetaInt32    | op number (0–10) |

Where `{Op}` = MTF, Histogram, GHS, ArcSinh, Log, Lumpton, RNC, Photometric, OTS, SAS, Veralux.

**Op-specific parameters:**

| Parameter class              | ID string            | Type       | Default | Min    | Max    |
|------------------------------|----------------------|------------|---------|--------|--------|
| *MTF*                        |                      |            |         |        |        |
| NX4MTFMidtone                | "mtfMidtone"         | MetaFloat  | 0.25    | 0.001  | 0.999  |
| NX4MTFShadows                | "mtfShadows"         | MetaFloat  | 0.0     | 0.0    | 0.5    |
| NX4MTFHighlights             | "mtfHighlights"      | MetaFloat  | 1.0     | 0.5    | 1.0    |
| NX4MTFLuminanceOnly          | "mtfLuminanceOnly"   | MetaBoolean| false   |        |        |
| *Histogram*                  |                      |            |         |        |        |
| NX4HistClipFraction          | "histClipFraction"   | MetaFloat  | 0.00001 | 0.0    | 0.01   |
| NX4HistLuminanceOnly         | "histLuminanceOnly"  | MetaBoolean| true    |        |        |
| NX4HistBins                  | "histBins"           | MetaInt32  | 4096    | 256    | 16384  |
| *GHS*                        |                      |            |         |        |        |
| NX4GHSD                      | "ghsD"               | MetaFloat  | 5.0     | 0.0    | 50.0   |
| NX4GHSb                      | "ghsB"               | MetaFloat  | 5.0     | 0.0    | 100.0  |
| NX4GHSSP                     | "ghsSP"              | MetaFloat  | 0.25    | 0.0    | 1.0    |
| NX4GHSLP                     | "ghsLP"              | MetaFloat  | 0.0     | 0.0    | 1.0    |
| NX4GHSHP                     | "ghsHP"              | MetaFloat  | 1.0     | 0.0    | 1.0    |
| NX4GHSLuminanceOnly          | "ghsLuminanceOnly"   | MetaBoolean| false   |        |        |
| *ArcSinh*                    |                      |            |         |        |        |
| NX4ArcSinhAlpha              | "arcSinhAlpha"       | MetaFloat  | 500.0   | 1.0    | 10000.0|
| NX4ArcSinhLuminanceOnly      | "arcSinhLuminanceOnly"| MetaBoolean| true   |        |        |
| *Log*                        |                      |            |         |        |        |
| NX4LogAlpha                  | "logAlpha"           | MetaFloat  | 1000.0  | 1.0    | 100000.0|
| NX4LogLuminanceOnly          | "logLuminanceOnly"   | MetaBoolean| false   |        |        |
| *Lumpton*                    |                      |            |         |        |        |
| NX4LumptonAlpha              | "lumptonAlpha"       | MetaFloat  | 500.0   | 1.0    | 10000.0|
| NX4LumptonBeta               | "lumptonBeta"        | MetaFloat  | 0.0     | 0.0    | 100.0  |
| NX4LumptonChannelMode        | "lumptonChannelMode" | MetaEnum   | JOINT_RGB | — | 3 items |
| *RNC*                        |                      |            |         |        |        |
| NX4RNCBlackPoint             | "rncBlackPoint"      | MetaFloat  | 0.0     | 0.0    | 0.5    |
| NX4RNCWhitePoint             | "rncWhitePoint"      | MetaFloat  | 1.0     | 0.5    | 1.0    |
| NX4RNCGamma                  | "rncGamma"           | MetaFloat  | 2.2     | 0.01   | 5.0    |
| NX4RNCChannelMode            | "rncChannelMode"     | MetaEnum   | PER_CHANNEL | — | 3 items |
| *Photometric*                |                      |            |         |        |        |
| NX4PhotoMagZero              | "photoMagZero"       | MetaFloat  | 25.0    | 0.0    | 40.0   |
| NX4PhotoDisplayMinMag        | "photoDisplayMinMag" | MetaFloat  | 22.0    | 10.0   | 30.0   |
| NX4PhotoDisplayMaxMag        | "photoDisplayMaxMag" | MetaFloat  | 8.0     | 0.0    | 20.0   |
| *OTS*                        |                      |            |         |        |        |
| NX4OTSBins                   | "otsBins"            | MetaInt32  | 2048    | 256    | 8192   |
| NX4OTSLuminanceOnly          | "otsLuminanceOnly"   | MetaBoolean| true    |        |        |
| NX4OTSUseGPU                 | "otsUseGPU"          | MetaBoolean| true    |        |        |
| NX4OTSTarget                 | "otsTarget"          | MetaEnum   | MUNSELL | —     | 4 items|
| *SAS*                        |                      |            |         |        |        |
| NX4SASTileSize               | "sasTileSize"        | MetaInt32  | 256     | 64    | 1024   |
| NX4SASTileOverlap            | "sasTileOverlap"     | MetaFloat  | 0.5     | 0.0   | 0.75   |
| NX4SASTargetMedian           | "sasTargetMedian"    | MetaFloat  | 0.25    | 0.05  | 0.5    |
| NX4SASMaxStretch             | "sasMaxStretch"      | MetaFloat  | 20.0    | 1.0   | 100.0  |
| NX4SASMinStretch             | "sasMinStretch"      | MetaFloat  | 1.0     | 0.1   | 10.0   |
| NX4SASLuminanceOnly          | "sasLuminanceOnly"   | MetaBoolean| true    |        |        |
| *Veralux*                    |                      |            |         |        |        |
| NX4VeraluxExposure           | "veraluxExposure"    | MetaFloat  | 0.0     | -3.0  | 3.0    |
| NX4VeraluxContrast           | "veraluxContrast"    | MetaFloat  | 1.0     | 0.5   | 2.0    |
| NX4VeraluxToeStrength        | "veraluxToeStrength" | MetaFloat  | 0.5     | 0.0   | 1.0    |
| NX4VeraluxShoulderStrength   | "veraluxShoulderStrength" | MetaFloat | 0.5  | 0.0   | 1.0    |
| NX4VeraluxBlackPoint         | "veraluxBlackPoint"  | MetaFloat  | 0.0     | 0.0   | 0.2    |
| NX4VeraluxWhitePoint         | "veraluxWhitePoint"  | MetaFloat  | 1.0     | 0.8   | 1.0    |

Veralux exposure is in EV-like units: `exposureFactor = pow(2, exposure)`.
Default 0.0 = no exposure change. Range [-3, 3] = 8× darken to 8× brighten.

Veralux is a fresh implementation in v4. Filmic tone curve:
exposure scaling → Hermite cubic toe → contrast mid-section → parabolic shoulder → clamp.
Re-implemented from scratch to conform to the StretchOp interface.

#### Hardcoded constants (NOT user-adjustable)

These are implementation choices, not tuning knobs:

| Constant              | Value | Rationale                                           |
|-----------------------|-------|-----------------------------------------------------|
| RING_BUFFER_SLOTS     | 8     | Balances memory vs throughput; 8 saturates I/O      |
| RESERVOIR_K           | 64    | Sufficient for Gaussian/bimodal/skew discrimination |
| HISTOGRAM_BINS        | 16    | Per-pixel histogram for shape detection (not stretch)|
| DIST_FIT_TILE_SIZE    | 512   | Fits in 5070 Ti VRAM with room for kernels          |
| GPU_CLASSIFY_TILE     | 512   | Matches distribution fit tile for cache coherence    |
| MIN_PSF_STARS         | 5     | Below this, psf_weight defaults to 1.0 (neutral)    |
| PSF_CUTOUT_RADIUS     | 7     | 15×15 cutout for Moffat fitting                    |
| AIC_DELTA_THRESHOLD   | 2.0   | Stop fitting more complex models when AIC stops improving |

---

## 12. Subcube visualizer (PCL Graphics)

Embedded in `NukeXInterface` as a custom `pcl::Control`.

```cpp
class SubcubeVisualizerControl : public pcl::Control {
    void OnPaint(Control& sender, const Rect& updateRect) override {
        Graphics g(sender);
        // Isometric projection: (frame_index, spatial_offset, weight) → 2D
        // X axis: frame index (0 to N)
        // Y axis: spatial neighbor offset (±5 pixels)
        // Z axis: weight (vertical, maps to Y in isometric)
        for (int i = 0; i < n_frames; i++) {
            float wx = (frame_idx * cos30 - x_off * cos30);
            float wy = (frame_idx * sin30 + x_off * sin30) - weight * height_scale;
            RGBA color = weight_to_color(frame_weights[i]);  // purple→amber→coral
            g.FillEllipse(Rect(wx-3, wy-3, wx+3, wy+3), color);
        }
        // Draw distribution curve overlay on weight axis
        draw_distribution_curve(g, current_channel, current_distribution);
    }
};
```

**Triggered by:** user clicking a pixel in any open PI view while the NukeX
interface is active. PCL provides `ProcessInterface::WantsImageNotifications()`
and `ProcessInterface::ImageFocused()` for this pattern.

**Channel selector:** R/G/B radio buttons in the interface. Switching channels
re-draws the distribution curve but keeps the same frame weights (they are shared).

**Shape of the point cloud tells the user:**
- Tight flat plane near weight=1.0 → clean Gaussian data
- Most points at 1.0, a few scattered low → isolated trails/cosmic rays
- Many points below 0.5 → heavy cloud contamination
- Gradual slope → transparency drift across the night

---

## 13. Stretch pipeline

The pipeline is post-stack only. It never touches the working cube.

```cpp
class StretchOp {
public:
    bool   enabled  = false;
    int    position = 0;   // user-assigned order; pipeline sorts by this
    string name;
    virtual void  apply(Image& img) const = 0;
    virtual float apply_scalar(float x)   const { return x; }
    virtual ~StretchOp() = default;
};

class StretchPipeline {
public:
    vector<unique_ptr<StretchOp>> ops;

    void execute(Image& img) const {
        vector<StretchOp*> ordered;
        for (auto& op : ops)
            if (op->enabled) ordered.push_back(op.get());
        sort(ordered.begin(), ordered.end(),
             [](auto a, auto b){ return a->position < b->position; });
        for (auto* op : ordered)
            op->apply(img);
    }

    // Quick preview — always arcsinh, always on a COPY, NEVER modifies working data
    static Image quick_preview_stretch(const Image& linear_src,
                                        float alpha = 500.f) {
        Image preview = linear_src.clone();
        float norm = asinhf(alpha);
        // Apply per-pixel: preview[i] = asinh(alpha * src[i]) / norm
        return preview;
    }
};
```

**The 11 stretch operations and their key properties:**

| # | Name             | Class              | Category  | Channel Mode                 | Default  |
|---|------------------|--------------------|-----------|------------------------------|----------|
| 0 | MTF              | MTFStretch         | Finisher  | Per-channel or lum-only      | Disabled |
| 1 | Histogram        | HistogramStretch   | Secondary | Per-channel or lum-only      | Disabled |
| 2 | GHS              | GHStretch          | Primary   | Per-channel or lum-only      | Disabled |
| 3 | ArcSinh          | ArcSinhStretch     | Primary   | Lum-only default             | Disabled |
| 4 | Log              | LogStretch         | Primary   | Per-channel or lum-only      | Disabled |
| 5 | Lumpton          | LumptonStretch     | Primary   | Joint RGB / per-ch / mono    | Disabled |
| 6 | RNC              | RNCStretch         | Secondary | Per-channel / joint RGB / mono| Disabled|
| 7 | Photometric      | PhotometricStretch | Secondary | Per-channel                  | Disabled |
| 8 | OTS              | OTSStretch         | Secondary | Lum-only default             | Disabled |
| 9 | SAS              | SASStretch         | Primary   | Lum-only default             | Disabled |
|10 | Veralux          | VeraluxStretch     | Primary   | Per-channel or lum-only      | Disabled |

**All ops default disabled.** MTF must be explicitly placed by the user.

Advisory warnings (non-blocking):
- Warn if any Secondary op precedes all Primary ops
- Warn if MTF precedes any Primary or Secondary
- Never block — user can override

**Key formulas:**

MTF: `((m-1)*x) / ((2m-1)*x - m)` where m = midtone parameter

ArcSinh: `asinh(α*x) / asinh(α)`

Log: `log1p(α*x) / log1p(α)`

GHS: Generalized Hyperbolic Stretch (Gharat & Treweek 2021)
- Parameters: D (intensity), b (hyperbolic), SP (symmetry point), LP, HP
- Full formula in additions spec

Lumpton (joint RGB mode):
- `I = (R+G+B)/3`
- `factor = asinh(α*I+β) / ((α*I+β) * asinh(α))`
- `R *= factor; G *= factor; B *= factor`

OTS: 1D optimal transport on histogram (2048 bins)
- Source CDF from image histogram
- Target CDF from pre-baked Munsell value scale (perceptually uniform)
- Quantile matching: `T(i) = target_quantile(source_cdf(i))`
- Apply as LUT
- OpenCL: histogram (local atomics) + LUT application (embarrassingly parallel)

SAS: Spatially-varying GHS
- Tile the image (256×256, 50% overlap)
- Fit per-tile GHS parameters to bring local median to target display value
- Blend with Gaussian weight kernel
- Thread via PCL thread pool (one tile per thread)

**Veralux:** Filmic tone curve (exposure → toe → linear → shoulder → rolloff).
Re-implemented fresh for v4 as a StretchOp subclass with apply_scalar(float).
Parameters: exposure, contrast, toeStrength, shoulderStrength, blackPoint, whitePoint.
Must handle mono (nChannels=1) and RGB (nChannels=3).

**Photometric:** Requires FITS `MAGZERO` or `PHOTZP` keyword.
If absent: refuse to apply, log warning, do not silently produce garbage.

---

## 14. Quick-preview stretch rules (HARD RULES)

1. `quick_preview_stretch()` is the ONLY path used for display during classification
   and subcube visualization
2. It ALWAYS operates on a clone/copy — never in-place
3. The working cube values are NEVER stretched before the stack is complete
4. Any code path that calls a StretchOp on the live cube before output is a bug
5. These rules must be enforced architecturally (const Image& inputs to all stretch ops
   during preview; Image& only permitted for the final post-stack pipeline)

---

## 15. v4 is a fresh codebase — no v3 code

NukeX v4 is implemented from scratch. Do NOT copy, port, or adapt any v3 source.
v3 may be read as *reference* for understanding the algorithm intent behind Veralux,
but all v4 code — including Veralux — is written fresh to the v4 architecture.

Before implementing stretch functionality, Claude Code should:
1. Understand the Veralux filmic tone curve algorithm (exposure → toe → linear → shoulder → rolloff)
2. Implement `VeraluxStretch` as a new `StretchOp` subclass with `apply_scalar(float)`
3. Use PCL's `Image` class API: `NumberOfChannels()`, `SelectChannel(ch)`, `Image::sample_iterator`
4. Ensure all stretch ops handle mono (nChannels=1) and RGB (nChannels=3)
5. Verify no code path stretches the live cube — only post-stack output

---

## 16. Things that are explicitly NOT in NukeX

- No calls to PI's `StarAlignment` process (it rejects frames)
- No calls to PI's `ImageIntegration` process (defeats the purpose)
- No sigma-clipping rejection step (the distribution fit is the answer)
- No reference frame for normalization (the cube is its own reference)
- No separate RGB subcubes (one unified cube, channels share frame weights)
- No dithering-aware spatial classifier (distribution fitting handles hot pixels)
- No linear fit clipping classifier (distribution fit is a better answer)
- No WebView UI (PCL Graphics only — WebView was evaluated and ruled out)
- No Python components (PCL/C++ only, real process module not a script)
- No ML/neural network components in v4 (pure statistical inference)
- No v3 code — v4 is a complete fresh implementation (v3 is reference-only for algorithm understanding)

---

*End of master specification*
*Supplements: nukex_additions_spec.md (PSF weight detail, variance map formula,
stretch op full formulas including GHS)*
