# Phase 4: Stacking Engine — Distribution Fitting, Classification, Pixel Selection

*Design validated 2026-03-30*
*Supplements: NUKEX_MASTER_SPEC.md, 2026-03-29-nukex-v4-implementation-design.md*

---

## 1. Scope

Phase 4 implements the complete stacking pipeline: streaming frame accumulation,
distribution fitting, pixel classification, and output image assembly. This is the
scientific core of NukeX — where aligned frames become a stacked image.

**Four new libraries:**
- `lib/fitting` — MLE-based distribution fitting engine (Student-t, GMM, contamination, KDE)
- `lib/classify` — per-sample weight computation from frame metadata
- `lib/combine` — pixel selection, noise propagation, quality/noise map generation
- `lib/stacker` — streaming orchestrator (the frame loop + disk cache + analysis pass)

**Key architectural departures from the original master spec:**
- **MLE replaces least-squares curve fitting.** MLE is the statistically correct framework
  and produces proper log-likelihoods for AICc model selection.
- **Student-t replaces Gaussian as the primary model.** Student-t naturally handles heavy
  tails from seeing variation. When data is Gaussian, the MLE converges to ν → ∞ and
  recovers the Gaussian answer exactly.
- **Skew-normal is dropped.** The skewness parameter α is poorly estimated at any sample
  size relevant to this application. Asymmetric contamination is better modeled as a
  mixture (contamination model or GMM).
- **EM algorithm for Gaussian mixtures** instead of NLS histogram fitting. EM has closed-form
  M-step updates, operates on raw data (no binning), and converges monotonically.
- **AICc replaces AIC.** Burnham & Anderson (2002) recommend AICc universally when N/k < 40.
- **No reservoir sampling.** Aligned frames are cached to disk as uint16. The fitting engine
  operates on ALL N data points per pixel, not a K=64 subsample.
- **No tiled processing.** The Cube holds lightweight streaming accumulators (~6.8 GB for
  24.5 MP × 3ch). Phase B parallelizes over pixels via std::for_each(par_unseq).

---

## 2. Dependency Graph

```
core ← (leaf, no external deps)
  ├── io ← (core, CFITSIO)                    [DONE - Phase 2]
  ├── alignment ← (core, io, Eigen)           [DONE - Phase 3]
  ├── classify ← (core)                        ← NEW
  ├── fitting ← (core, Ceres, Eigen)           ← NEW
  └── combine ← (core, fitting, classify)      ← NEW

stacker ← (core, io, alignment, classify, fitting, combine)  ← NEW
```

**Build order:** fitting → classify → combine → stacker

**Third-party additions:**
- Ceres Solver 2.2+ (BSD-3) — MLE via GradientProblem, Gaussian fitting via least-squares
  Problem, covariance estimation. Build with MINIGLOG to avoid glog dependency.
- Already vendored: Eigen (via PCL), Catch2 v3

---

## 3. lib/fitting — Distribution Fitting Engine

### 3.1 The CurveFitter Interface

```cpp
struct FitResult {
    ZDistribution    distribution;    // The fitted distribution
    double           log_likelihood;  // For AICc computation
    int              n_params;        // k
    int              n_samples;       // N
    bool             converged;

    double aicc() const {
        double aic = 2.0 * n_params - 2.0 * log_likelihood;
        double correction = (2.0 * n_params * (n_params + 1.0))
                          / (n_samples - n_params - 1.0);
        return aic + correction;
    }
};

class CurveFitter {
public:
    virtual ~CurveFitter() = default;
    virtual FitResult fit(const float* values, const float* weights,
                          int n, double robust_location,
                          double robust_scale) = 0;
};
```

Every backend receives raw sample values, per-sample combined weights, count, and
pre-computed robust estimates (biweight location, MAD) as initial guess seeds.

### 3.2 Backend 1: StudentTFitter — MLE via Ceres GradientProblem

Fits Student-t(μ, σ, ν) by minimizing negative log-likelihood:

    -log L = Σᵢ [-log Γ((ν+1)/2) + log Γ(ν/2) + ½log(νπσ²)
              + ((ν+1)/2)·log(1 + (xᵢ-μ)²/(νσ²))]

**Parameters:** μ (location), σ (scale), ν (degrees of freedom)
**Bounds:** σ ∈ [1e-10, ∞), ν ∈ [2, 100]
**Initial guess:** μ = biweight location, σ = MAD × 1.4826, ν = 5
**Auto-diff:** Full auto-diff for μ and σ. Numeric differentiation for ν
  (lgamma is not available in Ceres Jet types).
**Classification:** ν > 30 → GAUSSIAN, ν ≤ 30 → HEAVY_TAILED
**Signal extraction:** μ from MLE (the location parameter)
**Uncertainty:** σ·√(ν/(ν-2))/√N from Fisher information

**Why Student-t is primary:** It subsumes Gaussian (ν → ∞). AICc automatically
selects Gaussian when tails are not heavy (the extra ν parameter is penalized).
It also functions as an implicit M-estimator — the MLE downweights outliers through
the heavy-tailed likelihood without explicit rejection.

**References:**
- Lange, Little & Taylor (1989), "Robust statistical modeling using the t distribution,"
  JASA, 84(408), 881-896.
- Liu & Rubin (1995), "ML estimation of the t distribution using EM and its extensions,"
  Statistica Sinica, 5, 19-39.

### 3.3 Backend 2: GaussianMixtureFitter — EM Algorithm

Fits a 2-component Gaussian mixture: p(x) = π·N(μ₁,σ₁²) + (1-π)·N(μ₂,σ₂²)

**Parameters:** μ₁, σ₁, μ₂, σ₂, π (mixing weight of component 1)
**Algorithm:** Expectation-Maximization with closed-form M-step:
  - E-step: γᵢₖ = πₖ·N(xᵢ|μₖ,σₖ²) / Σⱼ πⱼ·N(xᵢ|μⱼ,σⱼ²)
  - M-step: πₖ = Σᵢ γᵢₖ / N
            μₖ = Σᵢ γᵢₖ·xᵢ / Σᵢ γᵢₖ
            σₖ² = Σᵢ γᵢₖ·(xᵢ - μₖ)² / Σᵢ γᵢₖ

**Initialization:** Sort samples, split at largest gap between consecutive values,
  compute per-half mean and std_dev.
**Convergence:** |Δlog L| < 1e-8 or 100 iterations.
**Degenerate guard:** σₖ clamped above 1e-10 to prevent single-point collapse.
**Canonicalization:** After convergence, ensure μ₁ < μ₂ (break permutation symmetry).
**Classification:**
  - π_dominant ∈ [0.05, 0.95] → BIMODAL (two real populations)
  - π_dominant > 0.95 → SPIKE_OUTLIER (one tiny component is the outlier)
**Signal extraction (BIMODAL):** μ of the component with higher π (dominant population).
  If both components have similar weight (|π₁ - π₂| < 0.1), use the higher-value
  component — the lower-value population is likely cloud-attenuated frames.
**Signal extraction (SPIKE_OUTLIER):** μ of the dominant component.

**References:**
- Dempster, Laird & Rubin (1977), "Maximum likelihood from incomplete data via the
  EM algorithm," JRSS-B, 39(1), 1-38.
- McLachlan & Peel (2000), Finite Mixture Models, Wiley.
- Fraley & Raftery (2002), "Model-based clustering, discriminant analysis, and density
  estimation," JASA, 97(458), 611-631.

### 3.4 Backend 3: ContaminationFitter — MLE via Ceres GradientProblem

Fits the Hogg et al. contamination model: p(x) = (1-ε)·N(μ,σ²) + ε·U(a,b)

**Parameters:** μ (clean signal location), σ (clean signal scale), ε (contamination fraction)
**Fixed:** U(a,b) range set to [min_sample - 0.1·range, max_sample + 0.1·range]
**Initial guess:** μ = biweight location, σ = MAD × 1.4826, ε = 0.05
**Bounds:** σ ∈ [1e-10, ∞), ε ∈ [0.001, 0.5]
**Classification:** CONTAMINATED
**Signal extraction:** μ of the clean Gaussian component.

This model is specifically for satellite trail / cosmic ray contamination where a few
samples are wildly different from the clean population. The uniform component absorbs
the outliers without any explicit rejection.

**References:**
- Hogg, Bovy & Lang (2010), "Data Analysis Recipes: Fitting a Model to Data,"
  arXiv:1008.4686.

### 3.5 Backend 4: KDEFitter — ISJ Bandwidth + Mode Finding

Non-parametric fallback. Gaussian KDE with bandwidth selected by Improved Sheather-Jones.

**Algorithm:**
1. ISJ bandwidth selection via diffusion equation (DCT-based, O(N log N))
2. Evaluate KDE on a 512-point grid spanning [min - 3h, max + 3h]
3. Find mode: peak of the grid, refined by Newton's method on the KDE derivative
4. Bandwidth h serves as the scale estimate

**Classification:** UNIFORM
**Signal extraction:** Mode of the KDE
**Uncertainty:** h / √N (bandwidth as scale proxy)
**Confidence:** 0.5 (non-parametric penalty — we trust parametric results more)

ISJ is used instead of Silverman's rule because Silverman oversmooths multimodal
distributions. ISJ automatically adapts to multimodal structure.

**References:**
- Botev, Grotowski & Kroese (2010), "Kernel density estimation via diffusion,"
  Annals of Statistics, 38(5), 2916-2957.
- Sheather & Jones (1991), "A reliable data-based bandwidth selection method,"
  JRSS-B, 53(3), 683-690.

### 3.6 Emergency Fallback: Biweight Location

If all four backends fail (should never happen — KDE always produces a mode), compute
the Tukey biweight location estimate as an absolute last resort.

**Algorithm:** IRLS with tuning constant c = 6.0, seeded from median, 10 iterations.
**Classification:** UNKNOWN
**Signal extraction:** Biweight location
**Confidence:** 0.2

**References:**
- Beers, Flynn & Gebhardt (1990), "Measures of location and scale for velocities in
  clusters of galaxies," AJ, 100, 32-46.

### 3.7 ModelSelector — The Cascade

```cpp
class ModelSelector {
public:
    struct Config {
        double aicc_threshold = 2.0;  // Minimum ΔAIC to prefer complex model
        int    min_samples_for_gmm = 30;  // Don't try GMM below this N
    };

    // Run the full model selection cascade for one voxel channel.
    // Reads sample values from cache, computes weights, fits all models,
    // selects winner by AICc, extracts signal.
    void select(const float* values, const float* weights, int n,
                SubcubeVoxel& voxel, int channel);
};
```

**Cascade logic:**
1. Compute robust statistics: biweight location, MAD (seeds for all fits)
2. Fit Student-t → FitResult A (always runs)
3. If N ≥ 30: Fit 2-component GMM → FitResult B
4. Fit contamination model → FitResult C
5. Compare AICc. If ΔAICc < 2.0 between two models, prefer the simpler one.
6. If best parametric model did not converge → run KDE fallback
7. Write ZDistribution to voxel, set shape, extract true_signal_estimate

### 3.8 Updated DistributionShape Enum

```cpp
enum class DistributionShape : uint8_t {
    GAUSSIAN      = 0,  // Student-t with ν > 30 (recovered Gaussian)
    BIMODAL       = 1,  // 2-component GMM, both components significant
    HEAVY_TAILED  = 2,  // Student-t with ν ≤ 30
    CONTAMINATED  = 3,  // Gaussian + uniform contamination model
    SPIKE_OUTLIER = 4,  // GMM with one tiny component (π > 0.95)
    UNIFORM       = 5,  // KDE fallback (non-parametric)
    UNKNOWN       = 6   // All fits failed (emergency biweight fallback)
};
```

SKEWED_LOW and SKEWED_HIGH are replaced in-place (same numeric values 2 and 3) by
HEAVY_TAILED and CONTAMINATED. The existing codebase enum in distribution.hpp must
be updated: remove SKEWED_LOW and SKEWED_HIGH, add HEAVY_TAILED and CONTAMINATED
at the same positions. No new values are added — this is a rename of two existing
shapes to better describe the actual physics.

### 3.9 Updated ZDistribution

The existing ZDistribution in distribution.hpp must be updated to reflect the new
model set. Changes from the current codebase:
- **Add:** StudentTParams, ContaminationParams
- **Remove:** SkewNormalParams (skew-normal dropped), spike_main union member
- **Remove:** spike_value, spike_frame_index (spike detection handled by GMM)
- **Remove:** aic, bic fields → replaced by single aicc field
- **Keep:** GaussianParams, BimodalParams (unchanged)

```cpp
struct StudentTParams {
    float mu    = 0.0f;  // Location
    float sigma = 0.0f;  // Scale
    float nu    = 0.0f;  // Degrees of freedom (ν > 30 → effectively Gaussian)
};

struct GaussianParams {
    float mu        = 0.0f;
    float sigma     = 0.0f;
    float amplitude = 0.0f;  // Peak height (for mixture models)
};

struct BimodalParams {
    GaussianParams comp1;
    GaussianParams comp2;
    float          mixing_ratio = 0.0f;  // Weight of comp1
};

struct ContaminationParams {
    float mu                = 0.0f;  // Clean signal location
    float sigma             = 0.0f;  // Clean signal scale
    float contamination_frac = 0.0f; // ε: fraction of outlier samples
};

struct ZDistribution {
    DistributionShape shape = DistributionShape::UNKNOWN;

    union {
        StudentTParams      student_t;
        BimodalParams       bimodal;
        ContaminationParams contamination;
    } params;

    // Goodness of fit
    float r_squared = 0.0f;
    float aicc      = 0.0f;  // Replaces separate aic + bic fields

    // Signal extraction — THE ANSWER
    float true_signal_estimate = 0.0f;
    float signal_uncertainty   = 0.0f;
    float confidence           = 0.0f;

    // Non-parametric fallback
    float kde_mode      = 0.0f;
    float kde_bandwidth = 0.0f;
    bool  used_nonparametric = false;
};
```

---

## 4. lib/classify — Weight Computation

### 4.1 Purpose

Computes a combined quality weight for each sample from per-frame metadata. The
weights modulate the fitting engine's likelihood contributions — lower-weight samples
have less influence on the fitted distribution.

### 4.2 Weight formula

For each sample at pixel (x, y), channel ch, from frame f:

```
w = frame_weight × psf_weight × sigma_factor × cloud_factor

sigma_factor = exp(-0.5 × max(0, |sigma_score| - threshold)² / scale²)
cloud_factor = (lum_ratio < cloud_threshold) ? cloud_penalty : 1.0
w = max(w, weight_floor)
```

Where:
- `frame_weight` = alignment quality (0.5 if alignment failed, 1.0 otherwise)
- `psf_weight` = exp(-0.5 × (fwhm/fwhm_best - 1)² / 0.25) — from PSF quality
- `sigma_score` = |value - welford.mean| / welford.std_dev — deviation from mean
- `lum_ratio` = value / frame_median — cloud/transparency indicator

Per-frame values (`frame_weight`, `psf_weight`, `frame_median`) are looked up from
the FrameStats vector by `frame_index`. Per-pixel values (`sigma_score`) are computed
on the fly from the sample value and the voxel's Welford accumulator.

### 4.3 WeightComputer class

```cpp
struct WeightConfig {
    float sigma_threshold     = 3.0f;
    float sigma_scale         = 2.0f;
    float cloud_threshold     = 0.85f;
    float cloud_penalty       = 0.30f;
    float weight_floor        = 0.01f;
};

class WeightComputer {
public:
    explicit WeightComputer(const WeightConfig& config);

    float compute(float value, uint16_t frame_index,
                  float welford_mean, float welford_stddev,
                  const FrameStats& frame_stats) const;
};
```

### 4.4 Summary statistics

After computing per-sample weights, also compute per-voxel-channel summaries:
- `mad[ch]` — median absolute deviation of sample values
- `biweight_midvariance[ch]` — Tukey's biweight scale estimator
- `iqr[ch]` — interquartile range
- `cloud_frame_count` — count of samples below cloud_threshold
- `worst_sigma_score`, `best_sigma_score` — extremes
- `mean_weight` — average combined weight

These are written to the voxel for the quality map and as seeds for the fitting engine.

---

## 5. lib/combine — Pixel Selection & Output Assembly

### 5.1 Purpose

Extracts output pixel values from fitted distributions, propagates noise, computes
quality metrics, and assembles the three output images.

### 5.2 PixelSelector — signal extraction + noise propagation

```cpp
class PixelSelector {
public:
    void select(const ZDistribution& dist,
                const float* values, const float* weights, int n,
                const FrameStats* frame_stats_arr,
                const uint16_t* frame_indices,
                float& out_value, float& out_noise, float& out_snr);
};
```

**Output value:** Copied from `dist.true_signal_estimate` (set by the fitting engine).

**Noise propagation:** Per-sample variance from the CCD noise model:

    σ²_sample = (read_noise² / gain²) + (value / gain)

First term: read noise in ADU². Second term: Poisson shot noise.
If FITS noise keywords are absent, fall back to Welford variance.

Propagated noise through weighted combination:

    noise_sigma = sqrt(Σ wᵢ² × σ²ᵢ) / Σ wᵢ

**SNR:** output_value / noise_sigma, clamped to [0, 9999].

### 5.3 Quality score

Composite diagnostic metric:

    quality_score = confidence × (1 - cloud_frame_count / n_frames) × min(1, snr / 50)

### 5.4 Spatial context (post-selection)

After all pixels have output values, compute spatial context on the output image:

```cpp
class SpatialContext {
public:
    void compute(const Image& output, Cube& cube) const;
};
```

- `gradient_mag` — Sobel gradient magnitude (3×3 kernel), max across channels
- `local_background` — biweight location in a 15×15 box
- `local_rms` — MAD × 1.4826 in a 15×15 box

These are written back to voxels for the quality map.

### 5.5 OutputAssembler

```cpp
class OutputAssembler {
public:
    struct OutputImages {
        Image stacked;      // N channels, float32
        Image noise_map;    // N channels, float32
        Image quality_map;  // 4 channels: signal, uncertainty, confidence, shape
    };

    static OutputImages assemble(const Cube& cube);
};
```

Quality map channels:
- Channel 0: true_signal_estimate (normalized [0, 1])
- Channel 1: signal_uncertainty
- Channel 2: confidence [0, 1]
- Channel 3: DistributionShape cast to float (0.0–6.0)

---

## 6. lib/stacker — Streaming Orchestrator

### 6.1 Two-phase architecture

**Phase A — Streaming accumulation:** Read frames one at a time, calibrate, align,
cache aligned data to disk, accumulate lightweight statistics (Welford + Histogram)
into the in-memory Cube.

**Phase B — Analysis:** Stream pixel values from the disk cache, classify, fit
distributions using ALL N data points, select output values, assemble output images.

### 6.2 StackingEngine class

```cpp
class StackingEngine {
public:
    struct Config {
        FrameAligner::Config   aligner_config;
        WeightConfig           weight_config;
        ModelSelector::Config  fitting_config;
        std::string            cache_dir;          // Temp directory for frame cache
        int                    ring_buffer_slots;   // Not used — sequential for now
    };

    explicit StackingEngine(const Config& config);

    struct Result {
        Image stacked;
        Image noise_map;
        Image quality_map;
        int   n_frames_processed;
        int   n_frames_failed_alignment;
    };

    Result execute(const std::vector<std::string>& light_paths,
                   const std::vector<std::string>& flat_paths);
};
```

### 6.3 Phase A — Streaming accumulation

```
for each light frame f in light_paths:
    1. FITSReader::read(path) → image + metadata
    2. DebayerEngine::debayer(image) if Bayer pattern present
    3. FlatCalibration::apply(image, master_flat) if flats provided
    4. FrameAligner::align(image, f) → aligned image + alignment result
    5. cache.write_frame(f, aligned_image)  // uint16 encode + write to disk
    6. Compute frame median luminance
    7. Store FrameStats[f]: read_noise, gain, exposure, psf_weight,
       frame_weight, median_luminance, is_meridian_flipped
    8. For each pixel (x, y), each channel ch:
       - voxel.welford[ch].update(value)
       - voxel.histogram[ch].update(value)
    9. cube.n_frames_loaded++
```

Phase A memory: one frame in flight (~295 MB) + Cube (~6.8 GB) + master flat (~295 MB).
Total: ~7.4 GB.

### 6.4 Between phases — global statistics

After all frames are accumulated:

1. Compute `fwhm_best` = minimum across all frame_fwhm values.
2. Backfill `psf_weight` into FrameStats: for each frame,
   `psf_weight = exp(-0.5 × (fwhm/fwhm_best - 1)² / 0.25)`.
3. Initialize histogram ranges from Welford min/max (for any histograms that
   need recomputation — in practice, ranges are set during first frame).

### 6.5 Phase B — Analysis pass

```
// Parallel over all pixels
std::for_each(std::execution::par_unseq, pixel_range, [&](int idx) {
    auto& voxel = cube.at(idx);
    int x = idx % cube.width, y = idx / cube.width;

    for (int ch = 0; ch < cube.channel_config.n_channels; ++ch) {
        // 1. Read ALL N frame values for this pixel from disk cache
        float values[MAX_FRAMES];
        uint16_t frame_indices[MAX_FRAMES];
        int n = cache.read_pixel(x, y, ch, values, frame_indices);

        // 2. Compute per-sample weights
        float weights[n];
        WeightComputer classifier(config.weight_config);
        for (int i = 0; i < n; ++i) {
            weights[i] = classifier.compute(
                values[i], frame_indices[i],
                voxel.welford[ch].mean, voxel.welford[ch].std_dev(),
                frame_stats[frame_indices[i]]);
        }

        // 3. Compute robust statistics for fitting seeds
        compute_robust_stats(values, weights, n, voxel, ch);

        // 4. Fit — run model selection cascade
        fitter.select(values, weights, n, voxel, ch);

        // 5. Select — extract output value + noise
        selector.select(voxel.distribution[ch],
                        values, weights, n,
                        frame_stats_ptr, frame_indices,
                        output_stacked.at(x, y, ch),
                        output_noise.at(x, y, ch),
                        output_snr_val);
        voxel.snr[ch] = output_snr_val;
    }

    compute_dominant_shape(voxel);
});

// Spatial context (needs all output values populated first)
SpatialContext spatial;
spatial.compute(output_stacked, cube);

// Assemble quality map from voxel fields
auto quality_map = OutputAssembler::assemble_quality(cube);
```

### 6.6 FrameStats — per-frame metadata

FrameStats is a NEW type separate from FrameMetadata (which exists in lib/core).
FrameMetadata is populated by FITSReader during file loading and contains raw FITS
header data. FrameStats is populated by the stacker during Phase A and contains
computed per-frame quality metrics needed during Phase B.

`frame_index` is the 0-based position of the frame in the input `light_paths`
vector — i.e., the arrival order. It is stable and deterministic. The FrameCache
and FrameStats vector are both indexed by this same frame_index.

```cpp
struct FrameStats {
    // From FITS headers (copied from FrameMetadata during Phase A)
    float read_noise;           // electrons (from FITS RDNOISE)
    float gain;                 // e-/ADU (from FITS GAIN)
    float exposure;             // seconds (from FITS EXPTIME)
    bool  has_noise_keywords;   // true if RDNOISE + GAIN both present
    bool  is_meridian_flipped;

    // Computed during Phase A
    float frame_weight;         // alignment quality (0.5 if failed, 1.0 otherwise)
    float median_luminance;     // frame median (for lum_ratio computation)
    float fwhm;                 // median FWHM from star catalog

    // Computed between phases (needs global fwhm_best)
    float psf_weight;           // exp(-0.5 × (fwhm/fwhm_best - 1)² / 0.25)
};

// Indexed by frame_index (0-based arrival order in light_paths).
// Shared read-only during Phase B parallel execution.
std::vector<FrameStats> frame_stats;  // [n_frames]
```

---

## 7. FrameCache — Disk-Backed Aligned Frame Storage

### 7.1 Purpose

Stores all aligned frames on disk so the fitting engine can access ALL N data points
per pixel without holding N frames in RAM. Eliminates the need for reservoir sampling.

### 7.2 Encoding

Pixel values (float32 in [0, 1]) are stored as uint16_t:

    encode: uint16_t stored = (uint16_t)(value * 65535.0f + 0.5f)
    decode: float value = stored * (1.0f / 65535.0f)

Quantization error: ±7.6×10⁻⁶. This is 100× smaller than typical per-frame noise
(~0.01 in normalized units) and 100× smaller than the fitting engine's precision
for μ at N=300 (~0.0006). No scientifically relevant information is lost.

### 7.3 Disk layout — pixel-major

```
File layout: [pixel_0_ch0_frame0][pixel_0_ch0_frame1]...[pixel_0_ch0_frameN]
             [pixel_0_ch1_frame0]...[pixel_0_ch1_frameN]
             ...
             [pixel_M_chC_frame0]...[pixel_M_chC_frameN]

Offset for pixel (x, y), channel ch, frame f:
    byte_offset = (((y * width + x) * n_channels + ch) * n_frames + f) * 2

Size per uint16_t entry: 2 bytes
```

This layout ensures that `read_pixel(x, y, ch)` reads N contiguous uint16 values —
one sequential disk read per pixel per channel.

### 7.4 FrameCache class

```cpp
class FrameCache {
public:
    FrameCache(int width, int height, int n_channels,
               int max_frames, const std::string& cache_dir);
    ~FrameCache();  // deletes temp file

    // Phase A: write one aligned frame (float → uint16 encoding)
    // Scatters to all pixel positions in the cache file.
    void write_frame(int frame_index, const Image& aligned);

    // Phase B: read all frame values at one pixel, decode to float
    // Returns number of frames read.
    int read_pixel(int x, int y, int ch,
                   float* out_values, uint16_t* out_frame_indices) const;

    int n_frames() const;

private:
    int fd_;
    uint16_t* mapped_;   // mmap pointer
    int width_, height_, n_channels_, n_frames_;
};
```

Implementation uses mmap for memory-mapped I/O:
- Phase A writes: random access (writing to scattered pixel positions per frame).
  The OS handles page management. Modern SSDs handle random writes well.
- Phase B reads: sequential scan through pixels. Optimal for mmap prefetching.

### 7.5 SIMD batch decode

For Phase B inner loop, decode uint16 → float in batches:

```cpp
void decode_batch(const uint16_t* src, float* dst, int n) {
    constexpr float scale = 1.0f / 65535.0f;
    int i = 0;
    // AVX2: 8 uint16 → 8 float per iteration
    for (; i + 7 < n; i += 8) {
        __m128i u16 = _mm_loadu_si128((__m128i*)(src + i));
        __m256i u32 = _mm256_cvtepu16_epi32(u16);
        __m256  f32 = _mm256_cvtepi32_ps(u32);
        f32 = _mm256_mul_ps(f32, _mm256_set1_ps(scale));
        _mm256_storeu_ps(dst + i, f32);
    }
    for (; i < n; i++)
        dst[i] = src[i] * scale;
}
```

### 7.6 Disk space

| Scenario | Frames | Pixels | Channels | Cache size |
|----------|--------|--------|----------|------------|
| Short session | 100 | 24.5M | 3 | 15 GB |
| Typical | 300 | 24.5M | 3 | 44 GB |
| Marathon | 1000 | 24.5M | 3 | 147 GB |

Cache is temporary — deleted by FrameCache destructor after stacking completes.

---

## 8. Memory Budget

### Phase A (streaming)

| Component | Size | Notes |
|-----------|------|-------|
| Cube (Welford + Histogram) | 6.8 GB | 24.5M × 3ch × 92B |
| One frame in flight | 295 MB | 24.5M × 3ch × 4B |
| Master flat | 295 MB | Same dimensions |
| FrameStats vector | ~12 KB | 300 frames × 40B |
| Aligner state | ~50 MB | Star catalogs, reference |
| **Phase A total** | **~7.4 GB** | |

### Phase B (analysis)

| Component | Size | Notes |
|-----------|------|-------|
| Cube (Welford + Histogram) | 6.8 GB | Read-only during Phase B |
| Output stacked image | 295 MB | |
| Output noise map | 295 MB | |
| Output quality map | 393 MB | 4 channels |
| FrameStats vector | ~12 KB | Read-only |
| Per-thread working memory | ~50 KB/thread | Values + weights arrays |
| **Phase B total** | **~7.8 GB** | |

Total peak memory: ~7.8 GB. Comfortable on any modern workstation.

---

## 9. SubcubeVoxel — Revised

The voxel becomes dramatically simpler. Changes from the current codebase voxel:
- **Remove:** ReservoirSample arrays (replaced by FrameCache disk storage)
- **Remove:** output_value[MAX_CHANNELS] (written directly to output Image in Phase B)
- **Remove:** noise_sigma[MAX_CHANNELS] (written directly to noise map Image in Phase B)
- **Keep:** welford, histogram, distribution, all diagnostic/quality fields
- All other fields remain for quality map and diagnostic purposes.

```cpp
static constexpr int MAX_CHANNELS = 8;

struct SubcubeVoxel {
    // ── Streaming accumulators (Phase A) ─────────────────────────────
    WelfordAccumulator  welford[MAX_CHANNELS];
    PixelHistogram      histogram[MAX_CHANNELS];

    // ── Fitted distribution (Phase B, per-channel) ───────────────────
    ZDistribution       distribution[MAX_CHANNELS];

    // ── Per-channel robust statistics (Phase B) ──────────────────────
    float               mad[MAX_CHANNELS];
    float               biweight_midvariance[MAX_CHANNELS];
    float               iqr[MAX_CHANNELS];

    // ── Per-channel output (Phase B) ─────────────────────────────────
    float               snr[MAX_CHANNELS];

    // ── Classification summaries (Phase B) ───────────────────────────
    uint16_t            cloud_frame_count;
    uint16_t            trail_frame_count;
    float               worst_sigma_score;
    float               best_sigma_score;
    float               mean_weight;
    float               total_exposure;

    // ── Cross-channel quality (Phase B) ──────────────────────────────
    float               confidence;
    float               quality_score;
    DistributionShape   dominant_shape;

    // ── Spatial context (Phase B, post-selection) ────────────────────
    float               gradient_mag;
    float               local_background;
    float               local_rms;

    // ── PSF quality at this position (Phase B) ───────────────────────
    float               mean_fwhm;
    float               mean_eccentricity;
    float               best_fwhm;

    // ── Bookkeeping ──────────────────────────────────────────────────
    uint16_t            n_frames;
    uint8_t             n_channels;
    uint8_t             flags;

    bool has_flag(uint8_t flag) const { return (flags & flag) != 0; }
    void set_flag(uint8_t flag)       { flags |= flag; }
    void clear_flag(uint8_t flag)     { flags &= ~flag; }
};
```

**Removed:** ReservoirSample (replaced by disk cache), output_value and noise_sigma
arrays (written directly to output images during Phase B).

---

## 10. Channel Configuration — Narrowband as First-Class Citizen

The channel architecture treats narrowband modes (OSC_HAO3, OSC_S2O3) as equal
peers to broadband modes (OSC_RGB, MONO_LRGB), not as bolt-on special cases. The
entire pipeline — FrameCache, WeightComputer, ModelSelector, PixelSelector, and
OutputAssembler — operates uniformly on `n_channels` without any mode-specific
branching. The only mode-specific code is in channel detection and debayer/separation.

### 10.1 Auto-detection

Implemented in the stacker during Phase A setup:

```
1. Read FILTER keyword from first FITS header
2. If FILTER contains "HaO3" or "HaOIII" → OSC_HAO3 (2 channels: Ha, OIII)
3. If FILTER contains "S2O3" or "SIIOIII" → OSC_S2O3 (2 channels: SII, OIII)
4. If BAYERPAT keyword present and FILTER is broadband → OSC_RGB (3 channels)
5. If no BAYERPAT and FILTER in {L, R, G, B, Ha, OIII, SII} →
   scan all frame headers, group by FILTER → MONO_LRGB or appropriate mode
6. If ambiguous → default to MONO_L (1 channel), user can override
```

### 10.2 Narrowband channel separation

For OSC_HAO3 and OSC_S2O3, the debayered RGB frame contains both narrowband
signals mixed across the Bayer photosites. Channel separation extracts the
individual narrowband channels (Ha, OIII, SII) from the mixed RGB data.

The separation is a linear unmixing problem:

    [R]   [a_R_Ha  a_R_OIII] [Ha  ]
    [G] = [a_G_Ha  a_G_OIII] [OIII]
    [B]   [a_B_Ha  a_B_OIII]

where the mixing coefficients aᵢⱼ depend on the filter spectral transmission ×
camera QE curve at each Bayer position. The unmixing is the pseudo-inverse.

**Architecture requirements for narrowband:**
- ChannelConfig carries `n_channels = 2` for HaO3/S2O3 modes
- The FrameCache stores 2 channels per pixel (not 3) — the separation happens
  BEFORE caching, during Phase A after debayer
- Welford, Histogram, ZDistribution all operate per-channel identically — Ha and
  OIII are independent channels, same as R and G would be
- The fitting engine sees no difference between narrowband and broadband channels
- OutputAssembler uses `output_rgb_mapping` to map channels to display RGB

**Mixing coefficient implementation:**
- Default coefficients for common filter/camera combinations (Optolong L-eXtreme,
  Antlia ALP-T) will be built in
- Configurable via ProcessParameters for custom filter/camera setups
- The coefficient matrix is applied once per frame during Phase A, before caching

This means narrowband support is not a future bolt-on — the data path is identical
to broadband from the FrameCache onward. The only narrowband-specific code is the
linear unmixing step in Phase A between debayer and cache.write_frame().

---

## 11. Testing Strategy

### Unit tests (per library)

**lib/fitting:**
- Student-t MLE: known-answer tests with synthetic t-distributed samples (verify μ, σ, ν recovery)
- GMM EM: synthetic bimodal data (two well-separated Gaussians, verify component recovery)
- Contamination: synthetic Gaussian + 5% uniform outliers (verify μ recovery, ε estimation)
- KDE ISJ: synthetic data with known modes (verify mode finding accuracy)
- AICc: verify correct model selection on labeled synthetic data
- Edge cases: N=10 (minimum viable), all-identical values, single outlier, empty input

**lib/classify:**
- Weight computation: verify multiplicative formula, floor clamping
- Sigma factor: verify Gaussian falloff shape
- Cloud detection: verify threshold behavior

**lib/combine:**
- Noise propagation: verify CCD noise model against hand-computed values
- SNR computation: verify ratio, clamping
- Spatial context: verify Sobel gradient on known patterns

**lib/stacker:**
- Integration test with synthetic FITS frames of known signal + noise
- Verify end-to-end: known input signal → stacked output matches expected value
  within noise bounds
- Verify frame cache: write frames, read back, verify uint16 encode/decode roundtrip

### Integration tests (real FITS data)

Test data from `/home/scarter4work/projects/processing/`:
- M16 (88 frames, HaO3, multi-night) — BIMODAL detection from weather changes
- NGC 2244 (59 frames, "Satellite Cluster") — SPIKE_OUTLIER / CONTAMINATED detection
- m31 (28 frames, broadband) — GAUSSIAN baseline, low N behavior

---

## 12. References

### Statistical Estimation
- Casella & Berger (2002), Statistical Inference, 2nd ed., Duxbury.
- Kay (1993), Fundamentals of Statistical Signal Processing: Estimation Theory, Prentice Hall.
- Pawitan (2001), In All Likelihood, Oxford University Press.

### Student-t and Robust Estimation
- Lange, Little & Taylor (1989), JASA 84(408), 881-896.
- Huber & Ronchetti (2009), Robust Statistics, 2nd ed., Wiley.
- Beers, Flynn & Gebhardt (1990), AJ 100, 32-46.

### Mixture Models
- Dempster, Laird & Rubin (1977), JRSS-B 39(1), 1-38.
- McLachlan & Peel (2000), Finite Mixture Models, Wiley.
- Hogg, Bovy & Lang (2010), arXiv:1008.4686.

### Model Selection
- Burnham & Anderson (2002), Model Selection and Multimodel Inference, 2nd ed., Springer.
- Hurvich & Tsai (1989), Biometrika 76(2), 297-307.

### Kernel Density Estimation
- Botev, Grotowski & Kroese (2010), Annals of Statistics 38(5), 2916-2957.
- Sheather & Jones (1991), JRSS-B 53(3), 683-690.

### Astronomical Methods
- Zackay & Ofek (2017), ApJ 836, 187-188.
- Stetson (1987), PASP 99, 191-222.
- Foi et al. (2008), IEEE TIP 17(10), 1737-1754.

---

*End of Phase 4 design specification*
