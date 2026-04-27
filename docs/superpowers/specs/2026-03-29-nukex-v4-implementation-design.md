# NukeX v4 — Implementation Design Document
*Brainstormed and validated 2026-03-29*
*Supplements: NUKEX_MASTER_SPEC.md, nukex_additions_spec.md*

---

## 1. Project Identity

NukeX v4 is a PixInsight Process Module (PCL/C++17) for astronomical image stacking.
It is the user's flagship contribution to the PixInsight community — a pièce de résistance
that must be scientifically rigorous, production-grade, and correct.

**Core thesis:** At each pixel position (x,y), the distribution of values across all
aligned frames tells the story of what happened at that pixel. A Gaussian says clean sky
signal. A bimodal says something changed halfway through the session. A spike says one
frame got hit by a satellite. The fitted curve IS the answer — it gives the pixel value,
the uncertainty, and the context. No averages. No rejection. No shortcuts.

**What failed in v3 and must not repeat:**
- Stretch implementations were mathematically incorrect
- Shortcuts and simplifications produced wrong results
- Code quality was not rigorously verified
- The user cannot PR C++ code at the level needed — Claude Code is the sole quality gate

---

## 2. Non-Negotiable Rules

These rules govern all implementation work. No exceptions.

1. **Scientific rigor.** Every formula verified against its published reference. No hand-waving.
2. **No shortcuts.** Every implementation is thorough and complete.
3. **No skipping.** Every step, every test, every verification — completed in full.
4. **Double-check everything.** First question: "Am I sure?" Verify before asserting.
5. **Full deploy pipeline always.** Build → sign → package → push to GitHub → PI repo install. Never test by dumping a .so into PI locally.
6. **No averages to simplify.** Pixel values come from fitted distribution parameters, never from mean/median of raw samples.
7. **No stubs, no TODOs.** Every function is complete and production-ready when written.
8. **No v3 code.** v4 is 100% fresh. v3 is reference-only for algorithm understanding.
9. **Heavy from the start.** Default to capable, full-featured dependencies. Light does not make right.
10. **No data thrown away.** Every frame, every pixel contributes. Contaminated pixels contribute at reduced weight derived from the fitted distribution. Near-zero weight is not rejection.

---

## 3. Architecture Overview

### 3.1 The SubcubeVoxel — Data Structure of Record

One struct per (x,y) pixel position containing ALL per-pixel data. No parallel arrays.
Everything about a pixel lives in its voxel.

Processing may use temporary shadow data structures (SoA views, GPU buffers, flat arrays)
for cache coherence and GPU coalescing. These are transient projections. All computed
results are written back to the voxel. The voxel is canonical truth.

```cpp
static constexpr int MAX_CHANNELS = 8;  // L, R, G, B, Ha, OIII, SII + spare

struct SubcubeVoxel {
    // ── Per-channel accumulated statistics (streaming pass) ──────────
    WelfordAccumulator  welford[MAX_CHANNELS];
    PixelHistogram      histogram[MAX_CHANNELS];
    ReservoirSample     reservoir[MAX_CHANNELS];
    ZDistribution       distribution[MAX_CHANNELS];

    // ── Per-channel output values (from pixel selector) ──────────────
    float               output_value[MAX_CHANNELS];
    float               noise_sigma[MAX_CHANNELS];
    float               snr[MAX_CHANNELS];

    // ── Per-channel robust statistics ────────────────────────────────
    float               mad[MAX_CHANNELS];
    float               biweight_midvariance[MAX_CHANNELS];
    float               iqr[MAX_CHANNELS];

    // ── Classification summary (accumulated across ALL frames) ───────
    uint16_t            cloud_frame_count;
    uint16_t            trail_frame_count;
    float               worst_sigma_score;
    float               best_sigma_score;
    float               mean_weight;
    float               total_exposure;

    // ── Cross-channel quality ────────────────────────────────────────
    float               confidence;
    float               quality_score;
    DistributionShape   dominant_shape;

    // ── Spatial context ──────────────────────────────────────────────
    float               gradient_mag;
    float               local_background;
    float               local_rms;

    // ── PSF quality at this position ─────────────────────────────────
    float               mean_fwhm;
    float               mean_eccentricity;
    float               best_fwhm;

    // ── Bookkeeping ──────────────────────────────────────────────────
    uint16_t            n_frames;
    uint8_t             n_channels;
    uint8_t             flags;  // border, saturated, etc.
};
```

### 3.2 Self-Describing Reservoir Samples

Each retained sample carries its own frame context. No frame-level array lookups
during combination — every pixel knows what it is, top to bottom.

```cpp
struct ReservoirSample {
    static constexpr int K = 64;

    struct Sample {
        float    value;
        float    frame_weight;
        float    psf_weight;
        float    read_noise;       // electrons
        float    gain;             // e-/ADU
        float    exposure;         // seconds
        float    lum_ratio;        // cloud indicator
        float    sigma_score;      // deviation from Z-median
        uint16_t frame_index;      // arrival order (temporal checks)
        bool     is_meridian_flipped;
    };

    Sample  samples[K];
    int     count = 0;
};
```

### 3.3 Distribution Fitting — The Curve Tells the Story

The Z-distribution at each pixel position is a fingerprint of what happened at that
pixel across all aligned frames. The fitted curve directly produces the pixel value.

```cpp
struct ZDistribution {
    DistributionShape shape;

    // Fit parameters (tagged union)
    union {
        GaussianParams   gaussian;
        BimodalParams    bimodal;
        SkewNormalParams skew_normal;
        GaussianParams   spike_main;
    } params;

    float spike_value;
    uint8_t spike_frame_index;

    // Goodness of fit
    float r_squared;
    float aic;
    float bic;

    // Signal extraction — these come directly from the fitted curve
    float true_signal_estimate;  // the pixel value
    float signal_uncertainty;    // the uncertainty
    float confidence;            // how trustworthy

    // Non-parametric fallback
    float kde_mode;
    float kde_bandwidth;
    bool  used_nonparametric;
};
```

**Signal extraction by shape:**

| Shape | true_signal_estimate | Source |
|-------|---------------------|--------|
| GAUSSIAN | μ from Gaussian fit | Fitted parameter |
| BIMODAL | μ of dominant signal peak | Fitted parameter of dominant component |
| SKEWED_LOW | mode of fitted skew-normal | Computed from fitted (μ, σ, α) |
| SKEWED_HIGH | mode of fitted skew-normal | Computed from fitted (μ, σ, α) |
| SPIKE_OUTLIER | μ of clean Gaussian component | Fitted parameter (spike excluded by the fit) |
| UNIFORM | KDE mode (re-fit) | UNIFORM triggers KDE re-fit; there is always a mode |
| KDE (non-parametric) | mode of kernel density estimate | Peak of fitted KDE |

There is no fallback to weighted mean. If parametric models all fail AIC threshold,
KDE produces a mode. KDE always produces an answer. The fit is always the answer.

### 3.4 The Cube

```cpp
struct Cube {
    int width, height;
    StackingMode mode;
    ChannelConfig channel_config;
    std::vector<SubcubeVoxel> voxels;  // [y * width + x]

    int n_frames_loaded = 0;

    SubcubeVoxel&       at(int x, int y)       { return voxels[y * width + x]; }
    const SubcubeVoxel& at(int x, int y) const { return voxels[y * width + x]; }
};
```

Frame-level metadata vectors exist transiently during the streaming pass as working
memory for the loader. After streaming completes, all relevant information has been
folded into the voxels (via reservoir sample metadata and accumulated summaries).

---

## 4. Channel Configuration & Stacking Modes

```cpp
enum class StackingMode : uint8_t {
    MONO_L,      // 1 channel: Luminance only
    MONO_LRGB,   // 4 channels: L, R, G, B from separate filter runs
    OSC_RGB,     // 3 channels: debayered R, G, B
    OSC_HAO3,    // 2 channels: Ha, OIII extracted from dual-narrowband Bayer
    OSC_S2O3,    // 2 channels: SII, OIII extracted from dual-narrowband Bayer
    CUSTOM       // N channels: user-defined mapping
};

struct ChannelConfig {
    StackingMode    mode;
    uint8_t         n_channels;
    std::string     channel_names[MAX_CHANNELS];
    uint8_t         output_rgb_mapping[3];

    enum class BayerPattern : uint8_t {
        NONE, RGGB, BGGR, GRBG, GBRG
    } bayer = BayerPattern::NONE;

    static ChannelConfig from_fits_headers(const FITSHeaderSet& headers);
    static ChannelConfig from_user(StackingMode mode);
};
```

**Auto-detection from FITS headers:**
1. Read FILTER keyword — if HaO3/S2O3/narrowband → set mode accordingly
2. Read BAYERPAT — if present with broadband filter → OSC_RGB
3. If no Bayer and FILTER is L/R/G/B → MONO_LRGB (scan all frames to confirm)
4. If ambiguous → ask user via interface

**Narrowband dual-filter decomposition (HaO3, S2O3):**
OIII signal lands on multiple Bayer photosites. Channel separation requires mixing
coefficients derived from filter spectral transmission × camera QE curves.
Before implementing, research best practices from:
- PixInsight forums (SHO from OSC workflows)
- Filter manufacturer spectral data (Optolong, Antlia)
- Published dual-narrowband characterization papers

Mixing coefficients will be configurable with sensible defaults for common filter/camera
combinations. Implementation must not assume equal weights across Bayer positions.

**Stacking is per-channel independently.** Channels never interact during accumulation,
fitting, or pixel selection. Frame-level weights are shared (cloud affects all channels).
Channel interaction only at output (RGB composition, palette mapping).

**For LRGB mono:** Each frame contributes to exactly one channel based on its FILTER keyword.
Per-channel frame counts tracked via `welford[ch].n`.

---

## 5. Library Decomposition

Domain-driven modules, each an independently compiled and tested static library:

```
src/
├── lib/
│   ├── core/           Data structures, math primitives, WelfordAccumulator,
│   │                   ReservoirSample, PixelHistogram, SubcubeVoxel, Cube,
│   │                   ChannelConfig, StackingMode
│   │
│   ├── fitting/        CurveFitter interface, Ceres backend, all parametric
│   │                   distribution models (Gaussian, bimodal, skew-normal,
│   │                   spike+Gaussian), KDE non-parametric, spline fitting,
│   │                   model selection (AIC/BIC), StatsLib integration
│   │
│   ├── alignment/      Star detection, Moffat PSF fitting, star catalog matching,
│   │                   homography computation (RANSAC + DLT), meridian flip
│   │                   detection/correction, FramePSFQuality computation
│   │
│   ├── io/             FITS loading/writing (via CFITSIO/PCL), ring buffer,
│   │                   debayer engine (RGGB + dual-narrowband channel separation),
│   │                   frame metadata extraction, flat field calibration,
│   │                   master flat generation
│   │
│   ├── classify/       Sigma scoring, luminance ratio, gradient detection,
│   │                   temporal consistency, weight computation, classification
│   │                   summary accumulation into voxels
│   │
│   ├── stretch/        StretchPipeline, StretchOp base, all 11 ops (MTF,
│   │                   Histogram, GHS, ArcSinh, Log, Lumpton, RNC, Photometric,
│   │                   OTS, SAS, Veralux), advisory ordering, quick_preview_stretch
│   │
│   ├── gpu/            OpenCL context management, kernel compilation, tiled
│   │                   executor (GPU and CPU fallback), shadow buffer management,
│   │                   voxel ↔ GPU transfer
│   │
│   └── combine/        Pixel selector (distribution-informed value extraction),
│                       noise map generation, SNR computation, quality map
│                       generation, output FITS assembly
│
├── module/             PCL module shell: NukeXModule, NukeXProcess, NukeXInstance,
│                       NukeXInterface, NukeXParameters (all process parameters)
│
├── ui/                 Simple interface: light file list + optional flat file list,
│                       execute button, subcube visualizer (secondary/collapsible)
│
└── test/
    ├── unit/           Per-lib Catch2 test executables
    ├── integration/    Multi-lib tests with real FITS data
    └── data/           Synthetic test fixtures with known answers
```

**Dependency graph:**
```
core ← (no deps, leaf node)
  ├── io ← (core)
  ├── fitting ← (core, Ceres, Eigen, StatsLib, LBFGSpp)
  ├── alignment ← (core, Eigen)
  ├── classify ← (core)
  ├── stretch ← (core)
  └── gpu ← (core, OpenCL)
       │
combine ← (core, fitting, classify, gpu)
       │
module ← (all libs, PCL)
  └── ui ← (core, stretch, module, PCL)
```

---

## 6. Build System (CMake)

**Structure:**
```
CMakeLists.txt (root)
├── cmake/
│   ├── PCLConfig.cmake           Find PCL, set platform-specific compiler flags
│   ├── CeresConfig.cmake         Configure vendored Ceres
│   └── PackageAndSign.cmake      sign → tarball → SHA1 → XRI update → XRI sign
├── src/lib/*/CMakeLists.txt      One per lib (STATIC library targets)
├── src/module/CMakeLists.txt     MODULE library target (the .so/.dylib/.dll)
├── test/CMakeLists.txt           Test executables linked against Catch2
├── third_party/                  Vendored dependencies
└── repository/                   PI distribution artifacts
```

**Targets:**
- `nukex4_core`, `nukex4_fitting`, etc. — STATIC libraries
- `NukeX` — MODULE (shared) library, links all static libs + PCL + OpenCL
- Output: `NukeX-pxm.so` (Linux), `NukeX-pxm.dylib` (macOS), `NukeX-pxm.dll` (Windows)
- `ctest` — runs all unit + integration tests
- `make package` — full deploy pipeline: sign module, create tarball, update SHA1, sign XRI

**Cross-platform:**
- Linux: GCC, `-fPIC -fvisibility=hidden -fvisibility-inlines-hidden`
- macOS: AppleClang, framework linking
- Windows: MSVC, DLL export macros
- PCL defines: `__PCL_LINUX` / `__PCL_MACOSX` / `__PCL_WINDOWS` + `__PCL_BUILDING_MODULE -D_REENTRANT`

**Third-party vendored:**

| Library | Purpose | License |
|---------|---------|---------|
| Ceres Solver 2.2+ | NLS, auto-differentiation, robust loss functions | BSD-3 |
| Catch2 v3 | Unit + integration testing | BSL-1.0 |
| StatsLib (kthohr) | Log-likelihood, PDF/CDF for AIC/BIC | Apache-2.0 |
| LBFGSpp | General-purpose optimization | MIT |

Eigen 3.4+ provided by PCL. CFITSIO provided by PCL. OpenCL from system driver.

Vendor rules: only `include/` and `LICENSE`. Never docs/tests/examples. No Boost.

---

## 7. Processing Pipeline (End-to-End Flow)

When the user selects light frames, optionally selects flats, and hits Execute:

### Phase 1 — Setup
1. Read FITS headers from first light frame
2. Auto-detect StackingMode + ChannelConfig (Bayer pattern, filter, resolution)
3. Allocate Cube (width × height × SubcubeVoxels, channels configured)
4. If flats provided:
   a. Load all flat frames
   b. Median-combine into master flat (per channel)
   c. Normalize master flat (divide by its median)
   d. Store for division during streaming

### Phase 2 — Streaming (ring buffer, frame by frame)
For each light frame arriving in a ring buffer slot:

5. Read FITS → raw pixels + header metadata
6. Debayer if OSC (RGGB standard or dual-narrowband channel separation)
7. Divide by normalized master flat if available
8. Detect stars on arcsinh preview copy → star catalog
9. Fit Moffat PSF to each star → FramePSFQuality
10. Match stars to reference catalog → homography H (RANSAC, 500 iter, 8-DOF)
    - First frame: H = identity, becomes reference
    - Detect meridian flip from H rotation angle, correct if needed
    - Alignment failure (<8 matches): flag frame, reduce weight × 0.5, best-effort H
11. Apply H to frame pixels (bilinear interpolation)
12. Compute frame median luminance
13. For each pixel (x,y), each active channel:
    a. Update welford[ch], histogram[ch], reservoir[ch]
    b. Reservoir sample carries: value + frame_weight + psf_weight + read_noise + gain + exposure + lum_ratio + sigma_score + frame_index + meridian_flip flag
    c. Accumulate classification summary into voxel: cloud_frame_count, trail_frame_count, worst/best sigma_score, mean_weight, total_exposure
14. Mark ring buffer slot free

### Phase 3 — Post-streaming global computations
15. Compute fwhm_best across all frames
16. Backfill PSF weights into all reservoir samples across all voxels
17. Compute per-voxel robust statistics from reservoir samples: mad[ch], biweight_midvariance[ch], iqr[ch]

### Phase 4 — Distribution fitting (tiled, GPU/CPU)
For each 512×512 tile:

18. Extract tile voxels into shadow buffers
19. For each voxel, each channel — fit reservoir samples:
    a. Model selection: Gaussian → Skew-normal → Bimodal → Spike+Gaussian
    b. Select by AIC (stop when ΔAIC < 2.0)
    c. If all parametric models fail: KDE non-parametric fit
    d. Extract: true_signal_estimate (from fitted curve parameters), signal_uncertainty, confidence
    e. Write ZDistribution back to voxel
20. Compute dominant_shape across channels

### Phase 5 — Pixel selection (tiled, GPU/CPU)
For each tile:

21. For each voxel, each channel — the fitted distribution directly provides the answer:
    - GAUSSIAN: output_value = μ from fit
    - BIMODAL: output_value = μ of dominant signal peak from fit
    - SKEWED: output_value = mode of fitted skew-normal
    - SPIKE_OUTLIER: output_value = μ of clean Gaussian component from fit
    - KDE: output_value = mode of kernel density estimate
22. Compute noise_sigma from variance propagation (per-sample read noise + gain from reservoir)
23. Compute snr = output_value / noise_sigma
24. Compute spatial context: gradient_mag, local_background, local_rms
25. Compute quality_score composite

### Phase 6 — Output
26. Generate stacked image (float32 FITS) from output_values
27. Generate quality map (4-channel FITS): true_signal_estimate, signal_uncertainty, confidence, shape
28. Generate noise map (float32 FITS) from noise_sigma
29. Apply stretch pipeline if ops enabled (post-stack only, on output image)
30. Write all output FITS with proper headers and processing history

### Phase 7 — Display
31. Open stacked result in PI as new ImageWindow
32. Quality map and noise map opened as separate windows if output options enabled

---

## 8. Stretch Pipeline — Scientific Repair

v3 stretches were found to be incorrect. Every v4 stretch op is built with this process:

1. **Cite the reference** — original paper or canonical formula
2. **Derive the formula** — verify edge cases analytically (identity, boundary, monotonicity)
3. **Implement with known-answer tests** — minimum 20 assertions per op
4. **Cross-validate** — compare against reference implementation where possible

### 8.1 The 11 Stretch Operations

| # | Name | Reference | Category | Key Verification |
|---|------|-----------|----------|------------------|
| 0 | MTF | PixInsight STF (Vizier) | Finisher | apply(0)=0, apply(1)=1, apply(m)=0.5 |
| 1 | Histogram | Standard CDF equalization | Secondary | Monotonic, output CDF uniform |
| 2 | GHS | Gharat & Treweek 2021 (JOSS) | Primary | Reduces to arcsinh when b→∞, identity when D=0 |
| 3 | ArcSinh | Lupton et al. 1999 | Primary | apply(0)=0, apply(1)=1, hue-preserving in lum mode |
| 4 | Log | Standard log1p | Primary | apply(0)=0, apply(1)=1 |
| 5 | Lumpton | Lupton et al. 2004 PASP 116:133 | Primary | Joint RGB preserves hue, factor→1 as I→0 |
| 6 | RNC | Roger N. Clark methodology | Secondary | Pure power law with black/white point remap |
| 7 | Photometric | Pogson magnitude scale | Secondary | Flux-preserving, refuses without MAGZERO |
| 8 | OTS | Villani 2003 (optimal transport) | Secondary | Source→target CDF matching, monotonic |
| 9 | SAS | Spec-defined (novel) | Primary | Local GHS convergence, smooth tile blending |
| 10 | Veralux | Filmic tone mapping (Hable/Narkowicz) | Primary | Smooth toe/shoulder, C1 continuity |

### 8.2 Pipeline Architecture

```cpp
class StretchOp {
public:
    bool   enabled  = false;
    int    position = 0;
    string name;
    virtual void  apply(Image& img) const = 0;
    virtual float apply_scalar(float x)   const { return x; }
    virtual ~StretchOp() = default;
};

class StretchPipeline {
public:
    vector<unique_ptr<StretchOp>> ops;
    void execute(Image& img) const;

    // Quick preview — arcsinh, always on a COPY, never modifies working data
    static Image quick_preview_stretch(const Image& linear_src, float alpha = 500.f);
};
```

Advisory ordering warnings (non-blocking): warn if Secondary precedes all Primaries,
warn if MTF precedes Primary/Secondary. Never block user's choice.

---

## 9. Calibration

Flats only. No darks. No bias.

If flat frames provided:
1. Load all flats
2. Median-combine into master flat per channel
3. Normalize: master_flat /= median(master_flat) per channel
4. During streaming: each light frame divided by normalized master flat after debayer, before alignment

If no flats: lights processed as-is (assumed pre-calibrated).

Architecture does not preclude adding darks/bias later, but no code paths or UI
elements for them exist in v4 initial release.

---

## 10. Alignment Engine

Custom implementation — no PI StarAlignment (it rejects frames, violating core philosophy).

### 10.1 Current scope (v4 initial release)
- Star detection: local maxima above SNR threshold, 2D Gaussian centroid fit
- PSF measurement: Moffat profile fitting, FWHM/eccentricity from star catalog
- Star matching: nearest-neighbor to reference catalog
- Homography: 8-DOF projective, RANSAC (500 iter, 1.5px inlier threshold), DLT solve on inliers
- Meridian flip: detected from H rotation angle, corrected by pre-multiplying H with 180° rotation
- Failed alignment: frame keeps weight × 0.5, never discarded
- Reference frame: first successfully aligned frame (H = identity)

### 10.2 Future scope (not implemented now)
- Differing resolution support (different binning, different cameras)
- The coordinate transform interface should not hardcode same-resolution assumptions,
  but only same-resolution homography is implemented initially

---

## 11. GPU Architecture (OpenCL)

Cross-vendor: AMD, Intel, NVIDIA. CPU fallback on all GPU paths (identical numerical results).

Three kernel passes per 512×512 tile:
1. **classify_pixels** — classification scores from reservoir samples
2. **compute_weights** — weight values from classification (separated for threshold tuning)
3. **select_pixels** — distribution-informed pixel selection + noise propagation

Shadow buffers: voxel data extracted into SoA layout for GPU coalesced access,
results written back to voxels after kernel completion.

Tiled execution: 512×512 tiles fit in RTX 5070 Ti VRAM (16 GB) with room for kernels.
Process tile → write back → free → next tile.

---

## 12. User Interface

Minimal. The power is in the algorithm, not in knobs.

**Primary UI:**
- File list control for light frames
- Optional file list control for flat frames
- Execute button

**Everything else is automatic:**
- Stacking mode auto-detected from FITS headers
- All classification thresholds use intelligent defaults
- Stretch pipeline has sensible defaults (or auto-selection based on data)

**Secondary (collapsible):**
- Subcube visualizer (click a pixel → see its Z-distribution story)
- Advanced parameter panel (ProcessParameters for power users)
- Channel configuration override

All parameters are PCL ProcessParameters — serializable to .xpsm process icons.

---

## 13. Testing Strategy

### 13.1 Unit tests (Catch2, per lib)
Every numerical function tested against known-answer inputs.
Minimum 20 assertions per stretch op. Edge cases verified: x=0, x=1, division by zero,
empty inputs, overflow, NaN propagation.

### 13.2 Integration tests (real FITS data)
Test data from `/home/scarter4work/projects/processing/`:
- M16 (88 frames, HaO3 narrowband, multi-night) — distribution fitting validation
- M63 (231 frames) — ring buffer stress testing
- NGC 2244 (59 frames, "Satellite Cluster") — SPIKE_OUTLIER verification
- m31 (28 frames, Lqef broadband) — OSC RGB baseline

### 13.3 Full pipeline tests
End-to-end: load FITS → flat calibrate → align → accumulate → fit → select → stretch → output.
Verify output FITS loads in PI via the signed repository pipeline.

### 13.4 Cross-validation
Where possible, compare stretch op outputs against Python/numpy reference implementations
to verify mathematical correctness independently of the C++ code.

---

## 14. Cross-Platform Builds

NukeX v4 ships on all three platforms:
- Linux: `NukeX-pxm.so` (GCC)
- macOS: `NukeX-pxm.dylib` (AppleClang)
- Windows: `NukeX-pxm.dll` (MSVC)

CMake handles platform detection and flag selection.
No platform-specific APIs without `#ifdef` guards.
Packaging pipeline produces platform-specific tarballs for PI repository.

---

## 15. Deployment Pipeline

Every build that reaches PixInsight goes through:

1. `cmake --build . --target all` — compile
2. `ctest --output-on-failure` — all tests pass
3. Sign module: `PixInsight.sh --sign-module-file=NukeX-pxm.so ...`
4. Package: create tarball with `bin/NukeX-pxm.so` + `bin/NukeX-pxm.xsgn`
5. Update `repository/updates.xri` with SHA1 of tarball
6. Sign XRI: `PixInsight.sh --sign-xml-file=updates.xri ...`
7. Commit version bump + package artifacts
8. Push to GitHub
9. PI users install from: `https://raw.githubusercontent.com/scarter4work/<repo>/main/repository/`

No local testing by copying .so into PI. The signed repository pipeline is the only path.

---

*End of implementation design document*
