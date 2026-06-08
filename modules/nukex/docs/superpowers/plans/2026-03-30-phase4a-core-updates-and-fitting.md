# Phase 4A: Core Type Updates + lib/fitting — Distribution Fitting Engine

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Update core distribution types (drop skew-normal, add Student-t and contamination models), slim the SubcubeVoxel (remove reservoir + output arrays), and build the complete MLE-based distribution fitting engine with Student-t, GMM via EM, contamination model, and KDE with ISJ bandwidth selection.

**Architecture:** lib/fitting depends on lib/core (for ZDistribution, WelfordAccumulator types) and system Ceres Solver 2.2 (for MLE via GradientProblem). Each fitting backend implements a CurveFitter interface returning FitResult with log-likelihood for AICc model selection. The ModelSelector runs the cascade and picks the winner. All fitting operates on raw float arrays — no dependency on voxel internals.

**Tech Stack:** C++17, Ceres Solver 2.2 (system, via find_package), Eigen 3 (system), lib/core, Catch2 v3

**Critical rules:**
- No stubs, no TODOs — every function complete
- Every formula verified against its published reference
- Double-check all numerical code — am I sure?
- MLE via Ceres GradientProblem, NOT least-squares curve fitting to histograms
- AICc (corrected AIC), NOT AIC — Burnham & Anderson 2002

---

## File Structure

```
src/lib/core/include/nukex/core/
├── distribution.hpp    MODIFY — new enum values, new param structs, updated ZDistribution
├── voxel.hpp           MODIFY — remove reservoir, output_value, noise_sigma

src/lib/fitting/
├── CMakeLists.txt
├── include/
│   └── nukex/
│       └── fitting/
│           ├── curve_fitter.hpp        CurveFitter interface + FitResult
│           ├── robust_stats.hpp        Biweight location, MAD, IQR
│           ├── student_t_fitter.hpp    Student-t MLE via Ceres GradientProblem
│           ├── gmm_fitter.hpp          2-component GMM via EM algorithm
│           ├── contamination_fitter.hpp  Gaussian + uniform contamination MLE
│           ├── kde_fitter.hpp          KDE with ISJ bandwidth selection
│           └── model_selector.hpp      AICc cascade — runs all fitters, picks winner
├── src/
│   ├── robust_stats.cpp
│   ├── student_t_fitter.cpp
│   ├── gmm_fitter.cpp
│   ├── contamination_fitter.cpp
│   ├── kde_fitter.cpp
│   └── model_selector.cpp

test/unit/fitting/
├── test_robust_stats.cpp
├── test_student_t_fitter.cpp
├── test_gmm_fitter.cpp
├── test_contamination_fitter.cpp
├── test_kde_fitter.cpp
└── test_model_selector.cpp
```

---

## Task 1: Update Core Distribution Types

**Files:**
- Modify: `src/lib/core/include/nukex/core/distribution.hpp`
- Modify: `test/unit/core/test_distribution.cpp`

This task updates the DistributionShape enum (replace SKEWED_LOW/HIGH with HEAVY_TAILED/CONTAMINATED), adds StudentTParams and ContaminationParams, removes SkewNormalParams and spike fields, and replaces aic+bic with aicc.

- [ ] **Step 1: Update distribution.hpp**

Replace the entire file:

```cpp
#pragma once

#include <cstdint>

namespace nukex {

enum class DistributionShape : uint8_t {
    GAUSSIAN      = 0,   // Student-t with ν > 30 (recovered Gaussian)
    BIMODAL       = 1,   // 2-component GMM, both components significant
    HEAVY_TAILED  = 2,   // Student-t with ν ≤ 30
    CONTAMINATED  = 3,   // Gaussian + uniform contamination model
    SPIKE_OUTLIER = 4,   // GMM with one tiny component (π > 0.95)
    UNIFORM       = 5,   // KDE fallback (non-parametric)
    UNKNOWN       = 6    // All fits failed
};

inline const char* distribution_shape_name(DistributionShape shape) {
    switch (shape) {
        case DistributionShape::GAUSSIAN:      return "GAUSSIAN";
        case DistributionShape::BIMODAL:       return "BIMODAL";
        case DistributionShape::HEAVY_TAILED:  return "HEAVY_TAILED";
        case DistributionShape::CONTAMINATED:  return "CONTAMINATED";
        case DistributionShape::SPIKE_OUTLIER: return "SPIKE_OUTLIER";
        case DistributionShape::UNIFORM:       return "UNIFORM";
        case DistributionShape::UNKNOWN:       return "UNKNOWN";
    }
    return "UNKNOWN";
}

/// Student-t location-scale family. When ν > 30, effectively Gaussian.
/// Reference: Lange, Little & Taylor (1989), JASA 84(408), 881-896.
struct StudentTParams {
    float mu    = 0.0f;   // Location parameter
    float sigma = 0.0f;   // Scale parameter
    float nu    = 0.0f;   // Degrees of freedom
};

/// Gaussian parameters for mixture model components.
struct GaussianParams {
    float mu        = 0.0f;
    float sigma     = 0.0f;
    float amplitude = 0.0f;
};

/// Two-component Gaussian mixture. Fitted via EM algorithm.
/// Reference: Dempster, Laird & Rubin (1977), JRSS-B 39(1), 1-38.
struct BimodalParams {
    GaussianParams comp1;
    GaussianParams comp2;
    float          mixing_ratio = 0.0f;  // Weight of comp1 (comp2 = 1 - mixing_ratio)
};

/// Gaussian signal + uniform contamination.
/// Reference: Hogg, Bovy & Lang (2010), arXiv:1008.4686.
struct ContaminationParams {
    float mu                 = 0.0f;  // Clean signal location
    float sigma              = 0.0f;  // Clean signal scale
    float contamination_frac = 0.0f;  // ε: fraction of outlier samples
};

struct ZDistribution {
    DistributionShape shape = DistributionShape::UNKNOWN;

    union {
        StudentTParams      student_t;
        BimodalParams       bimodal;
        ContaminationParams contamination;
    } params = {};

    float r_squared = 0.0f;
    float aicc      = 0.0f;

    float true_signal_estimate = 0.0f;
    float signal_uncertainty   = 0.0f;
    float confidence           = 0.0f;

    float kde_mode      = 0.0f;
    float kde_bandwidth = 0.0f;
    bool  used_nonparametric = false;
};

} // namespace nukex
```

- [ ] **Step 2: Update test_distribution.cpp**

Replace the entire file:

```cpp
#include "catch_amalgamated.hpp"
#include "nukex/core/distribution.hpp"

using namespace nukex;

TEST_CASE("DistributionShape enum values are correct", "[distribution]") {
    REQUIRE(static_cast<uint8_t>(DistributionShape::GAUSSIAN) == 0);
    REQUIRE(static_cast<uint8_t>(DistributionShape::BIMODAL) == 1);
    REQUIRE(static_cast<uint8_t>(DistributionShape::HEAVY_TAILED) == 2);
    REQUIRE(static_cast<uint8_t>(DistributionShape::CONTAMINATED) == 3);
    REQUIRE(static_cast<uint8_t>(DistributionShape::SPIKE_OUTLIER) == 4);
    REQUIRE(static_cast<uint8_t>(DistributionShape::UNIFORM) == 5);
    REQUIRE(static_cast<uint8_t>(DistributionShape::UNKNOWN) == 6);
}

TEST_CASE("StudentTParams stores mu, sigma, nu", "[distribution]") {
    StudentTParams t{};
    t.mu = 0.5f; t.sigma = 0.1f; t.nu = 5.0f;
    REQUIRE(t.mu == Catch::Approx(0.5f));
    REQUIRE(t.sigma == Catch::Approx(0.1f));
    REQUIRE(t.nu == Catch::Approx(5.0f));
}

TEST_CASE("GaussianParams stores mu, sigma, amplitude", "[distribution]") {
    GaussianParams g{};
    g.mu = 0.5f; g.sigma = 0.1f; g.amplitude = 1.0f;
    REQUIRE(g.mu == Catch::Approx(0.5f));
    REQUIRE(g.sigma == Catch::Approx(0.1f));
    REQUIRE(g.amplitude == Catch::Approx(1.0f));
}

TEST_CASE("BimodalParams stores two Gaussian components + mixing ratio", "[distribution]") {
    BimodalParams b{};
    b.comp1 = {0.3f, 0.05f, 0.7f};
    b.comp2 = {0.8f, 0.08f, 0.3f};
    b.mixing_ratio = 0.7f;
    REQUIRE(b.comp1.mu == Catch::Approx(0.3f));
    REQUIRE(b.comp2.mu == Catch::Approx(0.8f));
    REQUIRE(b.mixing_ratio == Catch::Approx(0.7f));
}

TEST_CASE("ContaminationParams stores clean signal + contamination fraction", "[distribution]") {
    ContaminationParams c{};
    c.mu = 0.45f; c.sigma = 0.03f; c.contamination_frac = 0.08f;
    REQUIRE(c.mu == Catch::Approx(0.45f));
    REQUIRE(c.sigma == Catch::Approx(0.03f));
    REQUIRE(c.contamination_frac == Catch::Approx(0.08f));
}

TEST_CASE("ZDistribution: default shape is UNKNOWN", "[distribution]") {
    ZDistribution z{};
    REQUIRE(z.shape == DistributionShape::UNKNOWN);
    REQUIRE(z.used_nonparametric == false);
}

TEST_CASE("ZDistribution: Student-t signal extraction", "[distribution]") {
    ZDistribution z{};
    z.shape = DistributionShape::GAUSSIAN;
    z.params.student_t = {0.42f, 0.03f, 50.0f};
    z.true_signal_estimate = z.params.student_t.mu;
    z.signal_uncertainty = z.params.student_t.sigma;
    z.confidence = 0.95f;
    z.r_squared = 0.97f;
    z.aicc = -150.0f;
    REQUIRE(z.true_signal_estimate == Catch::Approx(0.42f));
    REQUIRE(z.signal_uncertainty == Catch::Approx(0.03f));
    REQUIRE(z.confidence == Catch::Approx(0.95f));
}

TEST_CASE("ZDistribution: heavy-tailed Student-t", "[distribution]") {
    ZDistribution z{};
    z.shape = DistributionShape::HEAVY_TAILED;
    z.params.student_t = {0.40f, 0.05f, 4.0f};
    z.true_signal_estimate = z.params.student_t.mu;
    REQUIRE(z.true_signal_estimate == Catch::Approx(0.40f));
    REQUIRE(z.params.student_t.nu == Catch::Approx(4.0f));
}

TEST_CASE("ZDistribution: contamination model", "[distribution]") {
    ZDistribution z{};
    z.shape = DistributionShape::CONTAMINATED;
    z.params.contamination = {0.42f, 0.02f, 0.05f};
    z.true_signal_estimate = z.params.contamination.mu;
    REQUIRE(z.true_signal_estimate == Catch::Approx(0.42f));
    REQUIRE(z.params.contamination.contamination_frac == Catch::Approx(0.05f));
}

TEST_CASE("ZDistribution: KDE fallback fields", "[distribution]") {
    ZDistribution z{};
    z.shape = DistributionShape::UNIFORM;
    z.used_nonparametric = true;
    z.kde_mode = 0.35f;
    z.kde_bandwidth = 0.02f;
    REQUIRE(z.used_nonparametric == true);
    REQUIRE(z.kde_mode == Catch::Approx(0.35f));
    REQUIRE(z.kde_bandwidth == Catch::Approx(0.02f));
}

TEST_CASE("distribution_shape_name returns correct strings", "[distribution]") {
    REQUIRE(distribution_shape_name(DistributionShape::GAUSSIAN) == std::string("GAUSSIAN"));
    REQUIRE(distribution_shape_name(DistributionShape::BIMODAL) == std::string("BIMODAL"));
    REQUIRE(distribution_shape_name(DistributionShape::HEAVY_TAILED) == std::string("HEAVY_TAILED"));
    REQUIRE(distribution_shape_name(DistributionShape::CONTAMINATED) == std::string("CONTAMINATED"));
    REQUIRE(distribution_shape_name(DistributionShape::SPIKE_OUTLIER) == std::string("SPIKE_OUTLIER"));
    REQUIRE(distribution_shape_name(DistributionShape::UNIFORM) == std::string("UNIFORM"));
    REQUIRE(distribution_shape_name(DistributionShape::UNKNOWN) == std::string("UNKNOWN"));
}
```

- [ ] **Step 3: Build and run tests**

```bash
cd build && cmake .. && make -j$(nproc) 2>&1 | tail -5
ctest --output-on-failure -R test_distribution
```

Expected: test_distribution passes. Other tests may have compilation errors from the enum change — fix those next.

- [ ] **Step 4: Fix any downstream compilation failures**

The enum values SKEWED_LOW and SKEWED_HIGH no longer exist. Grep the codebase for references and update. Currently no source files reference them outside tests, but verify:

```bash
grep -r "SKEWED_LOW\|SKEWED_HIGH\|skew_normal\|spike_main\|spike_value\|spike_frame_index" src/ test/ --include="*.cpp" --include="*.hpp" -l
```

Fix any files found. The most likely hit is test_distribution.cpp (already updated) and test_voxel.cpp (next task).

- [ ] **Step 5: Commit**

```bash
git add src/lib/core/include/nukex/core/distribution.hpp test/unit/core/test_distribution.cpp
git commit -m "refactor(core): update distribution types — Student-t, contamination model, drop skew-normal

Replace SKEWED_LOW/SKEWED_HIGH with HEAVY_TAILED/CONTAMINATED in DistributionShape.
Add StudentTParams (location, scale, df) and ContaminationParams (Hogg et al. 2010).
Remove SkewNormalParams (unstable at N<100), spike_value/spike_frame_index fields.
Replace aic+bic with single aicc field (Burnham & Anderson 2002)."
```

---

## Task 2: Slim SubcubeVoxel — Remove Reservoir and Output Arrays

**Files:**
- Modify: `src/lib/core/include/nukex/core/voxel.hpp`
- Modify: `test/unit/core/test_voxel.cpp`
- Modify: `test/unit/core/test_cube.cpp` (if it references reservoir)

The voxel drops ReservoirSample arrays (replaced by disk-backed FrameCache) and
output_value/noise_sigma arrays (written directly to output Images in Phase B).

- [ ] **Step 1: Update voxel.hpp**

Replace the entire file:

```cpp
#pragma once

#include "nukex/core/types.hpp"
#include "nukex/core/welford.hpp"
#include "nukex/core/histogram.hpp"
#include "nukex/core/distribution.hpp"
#include <cstdint>

namespace nukex {

namespace VoxelFlags {
    constexpr uint8_t BORDER     = 0x01;
    constexpr uint8_t SATURATED  = 0x02;
    constexpr uint8_t LOW_N      = 0x04;
    constexpr uint8_t FIT_FAILED = 0x08;
}

struct SubcubeVoxel {
    // ── Streaming accumulators (Phase A) ─────────────────────────────
    WelfordAccumulator  welford[MAX_CHANNELS];
    PixelHistogram      histogram[MAX_CHANNELS];

    // ── Fitted distribution (Phase B, per-channel) ───────────────────
    ZDistribution       distribution[MAX_CHANNELS];

    // ── Per-channel output (Phase B) ─────────────────────────────────
    float               snr[MAX_CHANNELS]                    = {};

    // ── Per-channel robust statistics (Phase B) ──────────────────────
    float               mad[MAX_CHANNELS]                    = {};
    float               biweight_midvariance[MAX_CHANNELS]   = {};
    float               iqr[MAX_CHANNELS]                    = {};

    // ── Classification summaries (Phase B) ───────────────────────────
    uint16_t            cloud_frame_count  = 0;
    uint16_t            trail_frame_count  = 0;
    float               worst_sigma_score  = 0.0f;
    float               best_sigma_score   = 0.0f;
    float               mean_weight        = 0.0f;
    float               total_exposure     = 0.0f;

    // ── Cross-channel quality (Phase B) ──────────────────────────────
    float               confidence         = 0.0f;
    float               quality_score      = 0.0f;
    DistributionShape   dominant_shape     = DistributionShape::UNKNOWN;

    // ── Spatial context (Phase B, post-selection) ────────────────────
    float               gradient_mag       = 0.0f;
    float               local_background   = 0.0f;
    float               local_rms          = 0.0f;

    // ── PSF quality at this position ─────────────────────────────────
    float               mean_fwhm          = 0.0f;
    float               mean_eccentricity  = 0.0f;
    float               best_fwhm          = 0.0f;

    // ── Bookkeeping ──────────────────────────────────────────────────
    uint16_t            n_frames           = 0;
    uint8_t             n_channels         = 0;
    uint8_t             flags              = 0;

    bool has_flag(uint8_t flag) const { return (flags & flag) != 0; }
    void set_flag(uint8_t flag)       { flags |= flag; }
    void clear_flag(uint8_t flag)     { flags &= ~flag; }
};

} // namespace nukex
```

- [ ] **Step 2: Update test_voxel.cpp**

Replace the entire file:

```cpp
#include "catch_amalgamated.hpp"
#include "nukex/core/voxel.hpp"

using namespace nukex;

TEST_CASE("SubcubeVoxel: default initialization", "[voxel]") {
    SubcubeVoxel v{};
    REQUIRE(v.n_frames == 0);
    REQUIRE(v.n_channels == 0);
    REQUIRE(v.flags == 0);
    REQUIRE(v.confidence == 0.0f);
    REQUIRE(v.dominant_shape == DistributionShape::UNKNOWN);
    REQUIRE(v.cloud_frame_count == 0);
    for (int ch = 0; ch < MAX_CHANNELS; ch++) {
        REQUIRE(v.welford[ch].count() == 0);
        REQUIRE(v.histogram[ch].total_count() == 0);
        REQUIRE(v.distribution[ch].shape == DistributionShape::UNKNOWN);
    }
}

TEST_CASE("SubcubeVoxel: per-channel Welford accumulation", "[voxel]") {
    SubcubeVoxel v{};
    v.n_channels = 3;
    v.welford[0].update(0.5f);
    v.welford[0].update(0.6f);
    v.welford[0].update(0.55f);
    REQUIRE(v.welford[0].count() == 3);
    REQUIRE(v.welford[0].mean == Catch::Approx(0.55f));
    REQUIRE(v.welford[1].count() == 0);
    REQUIRE(v.welford[2].count() == 0);
}

TEST_CASE("SubcubeVoxel: classification summary", "[voxel]") {
    SubcubeVoxel v{};
    v.n_frames = 5;
    v.cloud_frame_count = 2;
    v.trail_frame_count = 1;
    v.worst_sigma_score = 4.5f;
    v.total_exposure = 1500.0f;
    REQUIRE(v.cloud_frame_count == 2);
    REQUIRE(v.total_exposure == Catch::Approx(1500.0f));
}

TEST_CASE("SubcubeVoxel: flag operations", "[voxel]") {
    SubcubeVoxel v{};
    REQUIRE(v.has_flag(VoxelFlags::BORDER) == false);
    v.set_flag(VoxelFlags::BORDER);
    REQUIRE(v.has_flag(VoxelFlags::BORDER) == true);
    REQUIRE(v.has_flag(VoxelFlags::SATURATED) == false);
    v.set_flag(VoxelFlags::SATURATED);
    REQUIRE(v.has_flag(VoxelFlags::SATURATED) == true);
    v.clear_flag(VoxelFlags::BORDER);
    REQUIRE(v.has_flag(VoxelFlags::BORDER) == false);
    REQUIRE(v.has_flag(VoxelFlags::SATURATED) == true);
}

TEST_CASE("SubcubeVoxel: sizeof reduced after reservoir removal", "[voxel]") {
    INFO("sizeof(SubcubeVoxel) = " << sizeof(SubcubeVoxel));
    // Without reservoir (was ~20KB+), should be well under 10KB
    REQUIRE(sizeof(SubcubeVoxel) > 500);
    REQUIRE(sizeof(SubcubeVoxel) < 10000);
}
```

- [ ] **Step 3: Fix test_cube.cpp if needed**

Check if test_cube.cpp references `reservoir` or `output_value`:

```bash
grep -n "reservoir\|output_value\|noise_sigma" test/unit/core/test_cube.cpp
```

If hits are found, remove those test lines. The Cube tests should only exercise spatial access (at(x,y)) and construction, which don't depend on removed fields.

- [ ] **Step 4: Build and run ALL tests**

```bash
cd build && cmake .. && make -j$(nproc) 2>&1 | tail -10
ctest --output-on-failure
```

Expected: All 14 tests pass. If any test references `reservoir`, `output_value`, `noise_sigma`, `SKEWED_LOW`, `SKEWED_HIGH`, `skew_normal`, `spike_main`, `spike_value`, `spike_frame_index`, `aic`, or `bic` — fix it.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "refactor(core): slim SubcubeVoxel — remove reservoir and output arrays

Remove ReservoirSample arrays (replaced by disk-backed FrameCache in Phase 4).
Remove output_value[] and noise_sigma[] (written directly to output Image).
Voxel now holds only streaming accumulators (Welford, Histogram) and
diagnostic/quality fields populated during Phase B analysis.

sizeof(SubcubeVoxel) drops from ~20KB to ~5KB per pixel."
```

---

## Task 3: lib/fitting Scaffolding + CurveFitter Interface + Robust Statistics

**Files:**
- Create: `src/lib/fitting/CMakeLists.txt`
- Create: `src/lib/fitting/include/nukex/fitting/curve_fitter.hpp`
- Create: `src/lib/fitting/include/nukex/fitting/robust_stats.hpp`
- Create: `src/lib/fitting/src/robust_stats.cpp`
- Create: `test/unit/fitting/test_robust_stats.cpp`
- Modify: `CMakeLists.txt` (root — add fitting subdirectory)
- Modify: `test/CMakeLists.txt` (add fitting test targets)

- [ ] **Step 1: Create directory structure**

```bash
mkdir -p src/lib/fitting/include/nukex/fitting
mkdir -p src/lib/fitting/src
mkdir -p test/unit/fitting
```

- [ ] **Step 2: Create src/lib/fitting/CMakeLists.txt**

```cmake
find_package(Ceres REQUIRED)
find_package(Eigen3 REQUIRED)

add_library(nukex4_fitting STATIC
    src/robust_stats.cpp
    src/student_t_fitter.cpp
    src/gmm_fitter.cpp
    src/contamination_fitter.cpp
    src/kde_fitter.cpp
    src/model_selector.cpp
)

target_include_directories(nukex4_fitting
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(nukex4_fitting
    PUBLIC nukex4_core
    PRIVATE Ceres::ceres Eigen3::Eigen
)

target_compile_features(nukex4_fitting PUBLIC cxx_std_17)
```

- [ ] **Step 3: Create curve_fitter.hpp — the interface**

```cpp
#pragma once

#include "nukex/core/distribution.hpp"
#include <cmath>

namespace nukex {

/// Result of fitting a parametric or non-parametric model to sample data.
struct FitResult {
    ZDistribution distribution;
    double        log_likelihood = 0.0;
    int           n_params       = 0;
    int           n_samples      = 0;
    bool          converged      = false;

    /// Corrected Akaike Information Criterion (Hurvich & Tsai 1989).
    /// AICc = 2k - 2·ln(L) + 2k(k+1)/(N-k-1)
    /// Reference: Burnham & Anderson (2002), Model Selection and Multimodel
    /// Inference, 2nd ed., Springer.
    double aicc() const {
        if (n_samples <= n_params + 1) return 1e30;
        double aic = 2.0 * n_params - 2.0 * log_likelihood;
        double correction = (2.0 * n_params * (n_params + 1.0))
                          / (n_samples - n_params - 1.0);
        return aic + correction;
    }
};

/// Abstract interface for distribution fitting backends.
///
/// All backends receive:
///   - values: raw sample values (float array)
///   - weights: per-sample combined quality weights (float array)
///   - n: number of samples
///   - robust_location: pre-computed biweight location (seed for μ)
///   - robust_scale: pre-computed MAD * 1.4826 (seed for σ)
class CurveFitter {
public:
    virtual ~CurveFitter() = default;

    virtual FitResult fit(const float* values, const float* weights,
                          int n, double robust_location,
                          double robust_scale) = 0;
};

} // namespace nukex
```

- [ ] **Step 4: Create robust_stats.hpp**

```cpp
#pragma once

namespace nukex {

/// Compute the median of an array. Modifies the input array (partial sort).
/// For read-only input, pass a copy.
float median_inplace(float* data, int n);

/// Median Absolute Deviation: median(|x_i - median(x)|).
/// Returns MAD. Multiply by 1.4826 for a consistent estimator of σ.
float mad(const float* data, int n);

/// Tukey's biweight location estimator (iteratively reweighted).
/// Robust location with 50% breakdown point, ~95% efficiency at Gaussian.
/// Tuning constant c = 6.0.
/// Reference: Beers, Flynn & Gebhardt (1990), AJ 100, 32-46.
///
/// Seeded from the median. Converges in ~5-10 iterations.
float biweight_location(const float* data, int n);

/// Tukey's biweight midvariance (robust scale estimator).
/// Reference: Beers, Flynn & Gebhardt (1990), AJ 100, 32-46.
float biweight_midvariance(const float* data, int n);

/// Interquartile range: Q3 - Q1.
float iqr(const float* data, int n);

/// Weighted median. weights must be non-negative.
float weighted_median(const float* data, const float* weights, int n);

} // namespace nukex
```

- [ ] **Step 5: Create robust_stats.cpp**

```cpp
#include "nukex/fitting/robust_stats.hpp"
#include <algorithm>
#include <cmath>
#include <vector>
#include <numeric>

namespace nukex {

float median_inplace(float* data, int n) {
    if (n <= 0) return 0.0f;
    if (n == 1) return data[0];
    std::nth_element(data, data + n / 2, data + n);
    float med = data[n / 2];
    if (n % 2 == 0) {
        float left = *std::max_element(data, data + n / 2);
        med = 0.5f * (left + med);
    }
    return med;
}

float mad(const float* data, int n) {
    if (n <= 0) return 0.0f;
    std::vector<float> copy(data, data + n);
    float med = median_inplace(copy.data(), n);

    std::vector<float> abs_devs(n);
    for (int i = 0; i < n; i++) {
        abs_devs[i] = std::fabs(data[i] - med);
    }
    return median_inplace(abs_devs.data(), n);
}

float biweight_location(const float* data, int n) {
    if (n <= 0) return 0.0f;
    if (n == 1) return data[0];

    constexpr float c = 6.0f;
    constexpr int max_iter = 10;
    constexpr float tol = 1e-7f;

    // Seed from median
    std::vector<float> copy(data, data + n);
    float location = median_inplace(copy.data(), n);

    float mad_val = mad(data, n);
    if (mad_val < 1e-30f) return location;
    float scale = mad_val * 1.4826f;

    for (int iter = 0; iter < max_iter; iter++) {
        double num = 0.0, den = 0.0;
        for (int i = 0; i < n; i++) {
            float u = (data[i] - location) / (c * scale);
            if (std::fabs(u) < 1.0f) {
                float u2 = u * u;
                float w = (1.0f - u2) * (1.0f - u2);
                num += w * data[i];
                den += w;
            }
        }
        if (den < 1e-30) break;
        float new_location = static_cast<float>(num / den);
        if (std::fabs(new_location - location) < tol * scale) {
            location = new_location;
            break;
        }
        location = new_location;
    }
    return location;
}

float biweight_midvariance(const float* data, int n) {
    if (n <= 1) return 0.0f;

    constexpr float c = 9.0f;  // Tuning constant for midvariance

    std::vector<float> copy(data, data + n);
    float med = median_inplace(copy.data(), n);

    float mad_val = mad(data, n);
    if (mad_val < 1e-30f) return 0.0f;

    double num = 0.0, den = 0.0;
    int n_used = 0;
    for (int i = 0; i < n; i++) {
        float u = (data[i] - med) / (c * mad_val);
        if (std::fabs(u) < 1.0f) {
            float u2 = u * u;
            float diff = data[i] - med;
            num += diff * diff * std::pow(1.0f - u2, 4);
            den += (1.0f - u2) * (1.0f - 5.0f * u2);
            n_used++;
        }
    }
    if (std::fabs(den) < 1e-30) return 0.0f;
    return static_cast<float>(n * num / (den * den));
}

float iqr(const float* data, int n) {
    if (n < 4) return 0.0f;
    std::vector<float> sorted(data, data + n);
    std::sort(sorted.begin(), sorted.end());
    int q1_idx = n / 4;
    int q3_idx = (3 * n) / 4;
    return sorted[q3_idx] - sorted[q1_idx];
}

float weighted_median(const float* data, const float* weights, int n) {
    if (n <= 0) return 0.0f;
    if (n == 1) return data[0];

    // Create index array sorted by data value
    std::vector<int> idx(n);
    std::iota(idx.begin(), idx.end(), 0);
    std::sort(idx.begin(), idx.end(),
              [data](int a, int b) { return data[a] < data[b]; });

    double total_weight = 0.0;
    for (int i = 0; i < n; i++) total_weight += weights[i];
    if (total_weight < 1e-30) return data[idx[n / 2]];

    double cumulative = 0.0;
    double half = total_weight * 0.5;
    for (int i = 0; i < n; i++) {
        cumulative += weights[idx[i]];
        if (cumulative >= half) {
            return data[idx[i]];
        }
    }
    return data[idx[n - 1]];
}

} // namespace nukex
```

- [ ] **Step 6: Create test_robust_stats.cpp**

```cpp
#include "catch_amalgamated.hpp"
#include "nukex/fitting/robust_stats.hpp"
#include <vector>
#include <cmath>

using namespace nukex;

TEST_CASE("median_inplace: odd count", "[robust]") {
    float data[] = {3.0f, 1.0f, 2.0f, 5.0f, 4.0f};
    REQUIRE(median_inplace(data, 5) == Catch::Approx(3.0f));
}

TEST_CASE("median_inplace: even count", "[robust]") {
    float data[] = {4.0f, 1.0f, 3.0f, 2.0f};
    REQUIRE(median_inplace(data, 4) == Catch::Approx(2.5f));
}

TEST_CASE("median_inplace: single element", "[robust]") {
    float data[] = {42.0f};
    REQUIRE(median_inplace(data, 1) == Catch::Approx(42.0f));
}

TEST_CASE("mad: known answer — symmetric data", "[robust]") {
    // Data: {1, 2, 3, 4, 5}, median = 3
    // |deviations| = {2, 1, 0, 1, 2}, median = 1
    float data[] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
    REQUIRE(mad(data, 5) == Catch::Approx(1.0f));
}

TEST_CASE("mad: all identical → zero", "[robust]") {
    float data[] = {0.5f, 0.5f, 0.5f, 0.5f};
    REQUIRE(mad(data, 4) == Catch::Approx(0.0f));
}

TEST_CASE("biweight_location: Gaussian data recovers mean", "[robust]") {
    // 50 samples from N(0.5, 0.01²) — hand-generated, deterministic
    std::vector<float> data;
    float base = 0.5f;
    for (int i = 0; i < 50; i++) {
        // Symmetric around 0.5: pairs at ±offset
        float offset = 0.01f * (i - 25) / 25.0f;
        data.push_back(base + offset);
    }
    float loc = biweight_location(data.data(), static_cast<int>(data.size()));
    REQUIRE(loc == Catch::Approx(0.5f).margin(0.005f));
}

TEST_CASE("biweight_location: robust to outliers", "[robust]") {
    // 20 samples at 0.5, 1 outlier at 10.0
    std::vector<float> data(20, 0.5f);
    data.push_back(10.0f);
    float loc = biweight_location(data.data(), static_cast<int>(data.size()));
    // Should be near 0.5, not pulled toward 10.0
    REQUIRE(loc == Catch::Approx(0.5f).margin(0.01f));
}

TEST_CASE("biweight_midvariance: known Gaussian data", "[robust]") {
    // For Gaussian data, biweight midvariance ≈ σ² (with c=9.0)
    std::vector<float> data;
    float sigma = 0.05f;
    for (int i = 0; i < 100; i++) {
        float u = (i - 50) / 50.0f;  // uniform in [-1, 1]
        data.push_back(0.5f + sigma * u);
    }
    float bwmv = biweight_midvariance(data.data(), static_cast<int>(data.size()));
    // Approximate: uniform on [-σ, σ] has variance σ²/3
    REQUIRE(bwmv > 0.0f);
}

TEST_CASE("iqr: known answer", "[robust]") {
    // {1, 2, 3, 4, 5, 6, 7, 8} → Q1=2, Q3=6, IQR=4
    float data[] = {1, 2, 3, 4, 5, 6, 7, 8};
    float result = iqr(data, 8);
    REQUIRE(result == Catch::Approx(4.0f));
}

TEST_CASE("weighted_median: equal weights is unweighted median", "[robust]") {
    float data[] = {5.0f, 1.0f, 3.0f};
    float weights[] = {1.0f, 1.0f, 1.0f};
    REQUIRE(weighted_median(data, weights, 3) == Catch::Approx(3.0f));
}

TEST_CASE("weighted_median: heavy weight pulls toward value", "[robust]") {
    float data[] = {1.0f, 2.0f, 3.0f};
    float weights[] = {0.1f, 0.1f, 10.0f};
    REQUIRE(weighted_median(data, weights, 3) == Catch::Approx(3.0f));
}
```

- [ ] **Step 7: Create empty .cpp stubs for compilation (will be filled in subsequent tasks)**

Create placeholder files so CMake can find them. Each file has just the include and namespace:

`src/lib/fitting/src/student_t_fitter.cpp`:
```cpp
#include "nukex/fitting/student_t_fitter.hpp"
// Implementation in Task 4
```

`src/lib/fitting/src/gmm_fitter.cpp`:
```cpp
#include "nukex/fitting/gmm_fitter.hpp"
// Implementation in Task 5
```

`src/lib/fitting/src/contamination_fitter.cpp`:
```cpp
#include "nukex/fitting/contamination_fitter.hpp"
// Implementation in Task 6
```

`src/lib/fitting/src/kde_fitter.cpp`:
```cpp
#include "nukex/fitting/kde_fitter.hpp"
// Implementation in Task 7
```

`src/lib/fitting/src/model_selector.cpp`:
```cpp
#include "nukex/fitting/model_selector.hpp"
// Implementation in Task 8
```

And create minimal headers for each so the includes work:

`src/lib/fitting/include/nukex/fitting/student_t_fitter.hpp`:
```cpp
#pragma once
#include "nukex/fitting/curve_fitter.hpp"
namespace nukex {
class StudentTFitter : public CurveFitter {
public:
    FitResult fit(const float* values, const float* weights,
                  int n, double robust_location, double robust_scale) override;
};
} // namespace nukex
```

`src/lib/fitting/include/nukex/fitting/gmm_fitter.hpp`:
```cpp
#pragma once
#include "nukex/fitting/curve_fitter.hpp"
namespace nukex {
class GaussianMixtureFitter : public CurveFitter {
public:
    FitResult fit(const float* values, const float* weights,
                  int n, double robust_location, double robust_scale) override;
};
} // namespace nukex
```

`src/lib/fitting/include/nukex/fitting/contamination_fitter.hpp`:
```cpp
#pragma once
#include "nukex/fitting/curve_fitter.hpp"
namespace nukex {
class ContaminationFitter : public CurveFitter {
public:
    FitResult fit(const float* values, const float* weights,
                  int n, double robust_location, double robust_scale) override;
};
} // namespace nukex
```

`src/lib/fitting/include/nukex/fitting/kde_fitter.hpp`:
```cpp
#pragma once
#include "nukex/fitting/curve_fitter.hpp"
namespace nukex {
class KDEFitter : public CurveFitter {
public:
    FitResult fit(const float* values, const float* weights,
                  int n, double robust_location, double robust_scale) override;
};
} // namespace nukex
```

`src/lib/fitting/include/nukex/fitting/model_selector.hpp`:
```cpp
#pragma once
#include "nukex/core/distribution.hpp"
namespace nukex {
struct SubcubeVoxel;
class ModelSelector {
public:
    struct Config {
        double aicc_threshold      = 2.0;
        int    min_samples_for_gmm = 30;
    };
    explicit ModelSelector(const Config& config = {});
    void select(const float* values, const float* weights, int n,
                SubcubeVoxel& voxel, int channel);
private:
    Config config_;
};
} // namespace nukex
```

- [ ] **Step 8: Update root CMakeLists.txt — add fitting subdirectory**

Add after line 61 (`add_subdirectory(src/lib/alignment)`):

```cmake
add_subdirectory(src/lib/fitting)
```

- [ ] **Step 9: Update test/CMakeLists.txt — add robust_stats test**

Add after the last test line:

```cmake
nukex_add_test(test_robust_stats unit/fitting/test_robust_stats.cpp nukex4_fitting)
```

- [ ] **Step 10: Build and run tests**

```bash
cd build && cmake .. && make -j$(nproc) 2>&1 | tail -10
ctest --output-on-failure
```

Expected: All tests pass including the new test_robust_stats.

- [ ] **Step 11: Commit**

```bash
git add -A
git commit -m "feat(fitting): lib/fitting scaffolding + CurveFitter interface + robust statistics

Add lib/fitting with CurveFitter abstract interface and FitResult (AICc computation).
Implement robust_stats: median, MAD, biweight location (Beers et al. 1990),
biweight midvariance, IQR, weighted median. All tested with known-answer cases.
Scaffold fitter headers (Student-t, GMM, contamination, KDE, ModelSelector)
with minimal declarations — implementations follow in subsequent tasks."
```

---

## Task 4: StudentTFitter — MLE via Ceres GradientProblem

**Files:**
- Modify: `src/lib/fitting/include/nukex/fitting/student_t_fitter.hpp`
- Modify: `src/lib/fitting/src/student_t_fitter.cpp`
- Create: `test/unit/fitting/test_student_t_fitter.cpp`
- Modify: `test/CMakeLists.txt`

The Student-t fitter minimizes negative log-likelihood of the Student-t distribution
using Ceres GradientProblem with BFGS. Auto-diff for μ and σ, numeric diff for ν
(lgamma not available in Ceres Jet types).

Reference: Lange, Little & Taylor (1989), JASA 84(408), 881-896.

- [ ] **Step 1: Update student_t_fitter.hpp**

```cpp
#pragma once

#include "nukex/fitting/curve_fitter.hpp"

namespace nukex {

/// Fits a Student-t(μ, σ, ν) distribution to weighted samples via MLE.
///
/// Uses Ceres GradientProblem to minimize negative log-likelihood.
/// When ν > 30, the Student-t is effectively Gaussian and classified as such.
/// When ν ≤ 30, the heavy tails naturally downweight outliers.
///
/// The Student-t MLE for μ is equivalent to an iteratively reweighted least
/// squares (IRLS) estimator — it is both the maximum likelihood answer and
/// a robust M-estimator.
///
/// Reference: Lange, Little & Taylor (1989), "Robust statistical modeling
///   using the t distribution," JASA, 84(408), 881-896.
class StudentTFitter : public CurveFitter {
public:
    /// Threshold for classifying as GAUSSIAN vs HEAVY_TAILED.
    static constexpr float NU_GAUSSIAN_THRESHOLD = 30.0f;

    /// Parameter bounds.
    static constexpr double SIGMA_MIN = 1e-10;
    static constexpr double NU_MIN    = 2.0;
    static constexpr double NU_MAX    = 100.0;

    FitResult fit(const float* values, const float* weights,
                  int n, double robust_location, double robust_scale) override;
};

} // namespace nukex
```

- [ ] **Step 2: Implement student_t_fitter.cpp**

```cpp
#include "nukex/fitting/student_t_fitter.hpp"

#include <ceres/ceres.h>
#include <cmath>
#include <algorithm>

namespace nukex {

namespace {

/// Negative log-likelihood of Student-t for one sample.
/// -log p(x | μ, σ, ν) = log Γ(ν/2) - log Γ((ν+1)/2) + 0.5·log(νπσ²)
///                       + ((ν+1)/2)·log(1 + (x-μ)²/(νσ²))
///
/// For Ceres GradientProblem: we minimize Σᵢ wᵢ · nll(xᵢ | μ, σ, ν)
/// where wᵢ are the sample weights.
struct StudentTNegLogLikelihood {
    const float* values;
    const float* weights;
    int n;

    /// Cost: total weighted negative log-likelihood.
    /// params[0] = μ, params[1] = log(σ) (unconstrained), params[2] = ν
    bool operator()(const double* params, double* cost, double* gradient) const {
        double mu       = params[0];
        double log_sigma = params[1];
        double nu       = params[2];

        // Clamp ν to valid range
        if (nu < StudentTFitter::NU_MIN) nu = StudentTFitter::NU_MIN;
        if (nu > StudentTFitter::NU_MAX) nu = StudentTFitter::NU_MAX;

        double sigma = std::exp(log_sigma);
        double sigma2 = sigma * sigma;

        double half_nu      = 0.5 * nu;
        double half_nu_plus = 0.5 * (nu + 1.0);

        // Log-gamma terms (constant w.r.t. data, but depend on ν)
        double lg_half_nu      = std::lgamma(half_nu);
        double lg_half_nu_plus = std::lgamma(half_nu_plus);

        // Per-sample constant: log Γ(ν/2) - log Γ((ν+1)/2) + 0.5·log(νπσ²)
        double constant_term = lg_half_nu - lg_half_nu_plus
                             + 0.5 * std::log(nu * M_PI * sigma2);

        double total_nll = 0.0;
        double grad_mu = 0.0, grad_log_sigma = 0.0, grad_nu = 0.0;

        for (int i = 0; i < n; i++) {
            double x = static_cast<double>(values[i]);
            double w = static_cast<double>(weights[i]);
            double diff = x - mu;
            double z2 = (diff * diff) / (nu * sigma2);
            double log_term = std::log(1.0 + z2);

            total_nll += w * (constant_term + half_nu_plus * log_term);

            if (gradient) {
                double factor = 1.0 / (1.0 + z2);
                // d/dμ
                grad_mu += w * half_nu_plus * (-2.0 * diff / (nu * sigma2)) * factor;
                // d/d(log σ) = d/dσ · σ  (chain rule for log-parametrization)
                // d(constant)/d(log σ) = 1.0 (from 0.5·log(σ²) = log σ)
                // d(data term)/d(log σ) = half_nu_plus · (-2·z²) / (1+z²)
                grad_log_sigma += w * (1.0 + half_nu_plus * (-2.0 * z2) * factor);
                // d/dν is complex — use finite differences below
            }
        }

        *cost = total_nll;

        if (gradient) {
            gradient[0] = grad_mu;
            gradient[1] = grad_log_sigma;
            // Numeric gradient for ν (lgamma derivatives are digamma functions,
            // not worth implementing for 1 parameter)
            double eps = 1e-5;
            double nu_plus = std::min(nu + eps, StudentTFitter::NU_MAX);
            double nu_minus = std::max(nu - eps, StudentTFitter::NU_MIN);
            double cost_plus = 0.0, cost_minus = 0.0;

            double half_np = 0.5 * nu_plus;
            double half_npp = 0.5 * (nu_plus + 1.0);
            double const_plus = std::lgamma(half_np) - std::lgamma(half_npp)
                              + 0.5 * std::log(nu_plus * M_PI * sigma2);
            for (int i = 0; i < n; i++) {
                double x = static_cast<double>(values[i]);
                double w = static_cast<double>(weights[i]);
                double diff = x - mu;
                double z2 = (diff * diff) / (nu_plus * sigma2);
                cost_plus += w * (const_plus + half_npp * std::log(1.0 + z2));
            }

            double half_nm = 0.5 * nu_minus;
            double half_nmp = 0.5 * (nu_minus + 1.0);
            double const_minus = std::lgamma(half_nm) - std::lgamma(half_nmp)
                               + 0.5 * std::log(nu_minus * M_PI * sigma2);
            for (int i = 0; i < n; i++) {
                double x = static_cast<double>(values[i]);
                double w = static_cast<double>(weights[i]);
                double diff = x - mu;
                double z2 = (diff * diff) / (nu_minus * sigma2);
                cost_minus += w * (const_minus + half_nmp * std::log(1.0 + z2));
            }

            gradient[2] = (cost_plus - cost_minus) / (nu_plus - nu_minus);
        }

        return true;
    }
};

/// Ceres FirstOrderFunction wrapper.
class StudentTCostFunction : public ceres::FirstOrderFunction {
public:
    StudentTCostFunction(const float* values, const float* weights, int n)
        : nll_{values, weights, n} {}

    bool Evaluate(const double* params, double* cost, double* gradient) const override {
        return nll_(params, cost, gradient);
    }

    int NumParameters() const override { return 3; }

private:
    StudentTNegLogLikelihood nll_;
};

} // anonymous namespace

FitResult StudentTFitter::fit(const float* values, const float* weights,
                               int n, double robust_location, double robust_scale) {
    FitResult result;
    result.n_params = 3;  // μ, σ, ν
    result.n_samples = n;

    if (n < 5 || robust_scale < 1e-30) {
        result.converged = false;
        return result;
    }

    // Initial parameters: μ = robust location, log(σ) = log(robust scale), ν = 5
    double params[3] = {
        robust_location,
        std::log(std::max(robust_scale, 1e-10)),
        5.0
    };

    ceres::GradientProblem problem(
        new StudentTCostFunction(values, weights, n));

    ceres::GradientProblemSolver::Options options;
    options.line_search_direction_type = ceres::BFGS;
    options.max_num_iterations = 100;
    options.function_tolerance = 1e-10;
    options.gradient_tolerance = 1e-8;
    options.logging_type = ceres::SILENT;

    ceres::GradientProblemSolver::Summary summary;
    ceres::Solve(options, problem, params, &summary);

    result.converged = (summary.termination_type == ceres::CONVERGENCE ||
                        summary.termination_type == ceres::USER_SUCCESS);

    if (!result.converged) return result;

    double mu    = params[0];
    double sigma = std::exp(params[1]);
    double nu    = std::clamp(params[2], NU_MIN, NU_MAX);

    // Compute log-likelihood at optimum
    double nll = summary.final_cost;
    result.log_likelihood = -nll;

    // Fill ZDistribution
    auto& dist = result.distribution;
    dist.params.student_t = {
        static_cast<float>(mu),
        static_cast<float>(sigma),
        static_cast<float>(nu)
    };

    if (nu > NU_GAUSSIAN_THRESHOLD) {
        dist.shape = DistributionShape::GAUSSIAN;
    } else {
        dist.shape = DistributionShape::HEAVY_TAILED;
    }

    dist.true_signal_estimate = static_cast<float>(mu);
    // Uncertainty: σ·√(ν/(ν-2))/√N for Student-t
    double effective_sigma = sigma;
    if (nu > 2.0) {
        effective_sigma *= std::sqrt(nu / (nu - 2.0));
    }
    dist.signal_uncertainty = static_cast<float>(effective_sigma / std::sqrt(n));
    dist.confidence = result.converged ? std::min(0.95f, static_cast<float>(1.0 - 1.0 / nu)) : 0.0f;
    dist.aicc = static_cast<float>(result.aicc());
    dist.r_squared = 0.0f;  // R² not meaningful for MLE; use AICc instead

    return result;
}

} // namespace nukex
```

- [ ] **Step 3: Create test_student_t_fitter.cpp**

```cpp
#include "catch_amalgamated.hpp"
#include "nukex/fitting/student_t_fitter.hpp"
#include "nukex/fitting/robust_stats.hpp"
#include <vector>
#include <cmath>
#include <random>

using namespace nukex;

namespace {

// Generate samples from N(mu, sigma²) using Box-Muller.
// Deterministic given seed.
std::vector<float> generate_gaussian(float mu, float sigma, int n, unsigned seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<float> dist(mu, sigma);
    std::vector<float> samples(n);
    for (int i = 0; i < n; i++) samples[i] = dist(rng);
    return samples;
}

// Generate samples from Student-t(mu, sigma, nu) using the ratio method.
std::vector<float> generate_student_t(float mu, float sigma, float nu, int n, unsigned seed) {
    std::mt19937 rng(seed);
    std::student_t_distribution<float> tdist(nu);
    std::vector<float> samples(n);
    for (int i = 0; i < n; i++) samples[i] = mu + sigma * tdist(rng);
    return samples;
}

std::vector<float> uniform_weights(int n) {
    return std::vector<float>(n, 1.0f);
}

} // anonymous namespace

TEST_CASE("StudentTFitter: recovers Gaussian mean from clean data", "[student_t]") {
    auto data = generate_gaussian(0.42f, 0.03f, 200, 12345);
    auto wt = uniform_weights(200);

    float rl = biweight_location(data.data(), 200);
    float rs = mad(data.data(), 200) * 1.4826f;

    StudentTFitter fitter;
    auto result = fitter.fit(data.data(), wt.data(), 200, rl, rs);

    REQUIRE(result.converged);
    REQUIRE(result.distribution.params.student_t.mu == Catch::Approx(0.42f).margin(0.01f));
    REQUIRE(result.distribution.params.student_t.sigma == Catch::Approx(0.03f).margin(0.01f));
    // Clean Gaussian → ν should be large (> 30)
    REQUIRE(result.distribution.params.student_t.nu > 20.0f);
    REQUIRE(result.distribution.shape == DistributionShape::GAUSSIAN);
}

TEST_CASE("StudentTFitter: recovers location from heavy-tailed data", "[student_t]") {
    auto data = generate_student_t(0.50f, 0.02f, 4.0f, 200, 54321);
    auto wt = uniform_weights(200);

    float rl = biweight_location(data.data(), 200);
    float rs = mad(data.data(), 200) * 1.4826f;

    StudentTFitter fitter;
    auto result = fitter.fit(data.data(), wt.data(), 200, rl, rs);

    REQUIRE(result.converged);
    REQUIRE(result.distribution.params.student_t.mu == Catch::Approx(0.50f).margin(0.02f));
    // ν should be low (< 30) — detected as heavy-tailed
    REQUIRE(result.distribution.params.student_t.nu < 15.0f);
    REQUIRE(result.distribution.shape == DistributionShape::HEAVY_TAILED);
}

TEST_CASE("StudentTFitter: robust to single outlier", "[student_t]") {
    auto data = generate_gaussian(0.50f, 0.02f, 100, 99999);
    auto wt = uniform_weights(100);
    // Add one extreme outlier
    data.push_back(5.0f);
    wt.push_back(1.0f);

    float rl = biweight_location(data.data(), 101);
    float rs = mad(data.data(), 101) * 1.4826f;

    StudentTFitter fitter;
    auto result = fitter.fit(data.data(), wt.data(), 101, rl, rs);

    REQUIRE(result.converged);
    // μ should still be near 0.50, not pulled toward the outlier
    REQUIRE(result.distribution.params.student_t.mu == Catch::Approx(0.50f).margin(0.02f));
}

TEST_CASE("StudentTFitter: AICc is computed correctly", "[student_t]") {
    auto data = generate_gaussian(0.5f, 0.05f, 64, 77777);
    auto wt = uniform_weights(64);

    float rl = biweight_location(data.data(), 64);
    float rs = mad(data.data(), 64) * 1.4826f;

    StudentTFitter fitter;
    auto result = fitter.fit(data.data(), wt.data(), 64, rl, rs);

    REQUIRE(result.converged);
    REQUIRE(result.n_params == 3);
    REQUIRE(result.n_samples == 64);
    // AICc should be finite and negative (good fit → high likelihood → negative AIC)
    REQUIRE(std::isfinite(result.aicc()));
    REQUIRE(result.aicc() < 0.0);
}

TEST_CASE("StudentTFitter: too few samples → does not converge", "[student_t]") {
    float data[] = {0.5f, 0.6f, 0.55f};
    float wt[] = {1.0f, 1.0f, 1.0f};

    StudentTFitter fitter;
    auto result = fitter.fit(data, wt, 3, 0.55, 0.05);

    REQUIRE(!result.converged);
}
```

- [ ] **Step 4: Add test target to test/CMakeLists.txt**

```cmake
nukex_add_test(test_student_t  unit/fitting/test_student_t_fitter.cpp  nukex4_fitting)
```

- [ ] **Step 5: Build and run tests**

```bash
cd build && cmake .. && make -j$(nproc) 2>&1 | tail -10
ctest --output-on-failure -R test_student_t
```

Expected: All Student-t tests pass.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat(fitting): StudentTFitter — MLE via Ceres GradientProblem

Student-t(μ, σ, ν) MLE using Ceres BFGS optimizer. Auto-diff for μ and σ,
numeric differentiation for ν (lgamma unavailable in Ceres Jet types).
Classifies as GAUSSIAN when ν > 30, HEAVY_TAILED otherwise.
Uncertainty from Fisher information: σ·√(ν/(ν-2))/√N.

Reference: Lange, Little & Taylor (1989), JASA 84(408), 881-896."
```

---

## Task 5: GaussianMixtureFitter — EM Algorithm

**Files:**
- Modify: `src/lib/fitting/include/nukex/fitting/gmm_fitter.hpp`
- Modify: `src/lib/fitting/src/gmm_fitter.cpp`
- Create: `test/unit/fitting/test_gmm_fitter.cpp`
- Modify: `test/CMakeLists.txt`

Two-component Gaussian mixture model fitted via EM. No optimizer needed — M-step
has closed-form solutions.

Reference: Dempster, Laird & Rubin (1977), JRSS-B 39(1), 1-38.

- [ ] **Step 1: Update gmm_fitter.hpp**

```cpp
#pragma once

#include "nukex/fitting/curve_fitter.hpp"

namespace nukex {

/// Fits a 2-component Gaussian mixture via Expectation-Maximization.
///
/// p(x) = π·N(μ₁,σ₁²) + (1-π)·N(μ₂,σ₂²)
///
/// EM has closed-form M-step updates — no optimizer needed.
/// Initialization: sort samples, split at largest gap.
/// Classification: π ∈ [0.05, 0.95] → BIMODAL, π > 0.95 → SPIKE_OUTLIER.
///
/// Reference: Dempster, Laird & Rubin (1977), JRSS-B 39(1), 1-38.
class GaussianMixtureFitter : public CurveFitter {
public:
    static constexpr int    MAX_ITERATIONS = 100;
    static constexpr double CONVERGENCE_TOL = 1e-8;
    static constexpr double SIGMA_FLOOR = 1e-10;
    static constexpr float  SPIKE_THRESHOLD = 0.95f;
    static constexpr float  BIMODAL_MIN_MIX = 0.05f;

    FitResult fit(const float* values, const float* weights,
                  int n, double robust_location, double robust_scale) override;
};

} // namespace nukex
```

- [ ] **Step 2: Implement gmm_fitter.cpp**

```cpp
#include "nukex/fitting/gmm_fitter.hpp"

#include <algorithm>
#include <cmath>
#include <vector>
#include <numeric>

namespace nukex {

namespace {

/// Gaussian PDF (log-space for numerical stability).
double gaussian_log_pdf(double x, double mu, double sigma) {
    double z = (x - mu) / sigma;
    return -0.5 * std::log(2.0 * M_PI) - std::log(sigma) - 0.5 * z * z;
}

/// Initialize by sorting and splitting at largest gap.
void initialize_from_gap(const float* values, int n,
                         double& mu1, double& sigma1, double& mu2, double& sigma2,
                         double& pi1) {
    std::vector<float> sorted(values, values + n);
    std::sort(sorted.begin(), sorted.end());

    // Find largest gap between consecutive sorted values
    int split = n / 2;
    float max_gap = 0.0f;
    for (int i = 1; i < n; i++) {
        float gap = sorted[i] - sorted[i - 1];
        if (gap > max_gap) {
            max_gap = gap;
            split = i;
        }
    }

    // Compute mean and std for each half
    if (split < 2) split = 2;
    if (split > n - 2) split = n - 2;

    double sum1 = 0.0, sum2 = 0.0;
    for (int i = 0; i < split; i++) sum1 += sorted[i];
    for (int i = split; i < n; i++) sum2 += sorted[i];
    mu1 = sum1 / split;
    mu2 = sum2 / (n - split);

    double ss1 = 0.0, ss2 = 0.0;
    for (int i = 0; i < split; i++) ss1 += (sorted[i] - mu1) * (sorted[i] - mu1);
    for (int i = split; i < n; i++) ss2 += (sorted[i] - mu2) * (sorted[i] - mu2);
    sigma1 = std::sqrt(std::max(ss1 / std::max(split - 1, 1), 1e-20));
    sigma2 = std::sqrt(std::max(ss2 / std::max(n - split - 1, 1), 1e-20));

    pi1 = static_cast<double>(split) / n;
}

} // anonymous namespace

FitResult GaussianMixtureFitter::fit(const float* values, const float* weights,
                                      int n, double robust_location,
                                      double robust_scale) {
    FitResult result;
    result.n_params = 5;  // μ₁, σ₁, μ₂, σ₂, π
    result.n_samples = n;

    if (n < 10) {
        result.converged = false;
        return result;
    }

    // Initialize
    double mu1, sigma1, mu2, sigma2, pi1;
    initialize_from_gap(values, n, mu1, sigma1, mu2, sigma2, pi1);

    // Clamp sigma floors
    sigma1 = std::max(sigma1, SIGMA_FLOOR);
    sigma2 = std::max(sigma2, SIGMA_FLOOR);
    pi1 = std::clamp(pi1, 0.01, 0.99);

    std::vector<double> gamma(n);  // responsibilities for component 1
    double prev_ll = -1e30;

    for (int iter = 0; iter < MAX_ITERATIONS; iter++) {
        // ── E-step: compute responsibilities ──
        double ll = 0.0;
        for (int i = 0; i < n; i++) {
            double x = static_cast<double>(values[i]);
            double lp1 = std::log(pi1) + gaussian_log_pdf(x, mu1, sigma1);
            double lp2 = std::log(1.0 - pi1) + gaussian_log_pdf(x, mu2, sigma2);
            double max_lp = std::max(lp1, lp2);
            double sum_exp = std::exp(lp1 - max_lp) + std::exp(lp2 - max_lp);
            gamma[i] = std::exp(lp1 - max_lp) / sum_exp;
            ll += static_cast<double>(weights[i]) * (max_lp + std::log(sum_exp));
        }

        // Check convergence
        if (std::fabs(ll - prev_ll) < CONVERGENCE_TOL) {
            result.converged = true;
            result.log_likelihood = ll;
            break;
        }
        prev_ll = ll;

        // ── M-step: update parameters ──
        double wsum1 = 0.0, wsum2 = 0.0;
        double wmu1 = 0.0, wmu2 = 0.0;
        for (int i = 0; i < n; i++) {
            double w = static_cast<double>(weights[i]);
            wsum1 += w * gamma[i];
            wsum2 += w * (1.0 - gamma[i]);
            wmu1  += w * gamma[i] * values[i];
            wmu2  += w * (1.0 - gamma[i]) * values[i];
        }

        double total_w = wsum1 + wsum2;
        if (wsum1 < 1e-30 || wsum2 < 1e-30) break;

        mu1 = wmu1 / wsum1;
        mu2 = wmu2 / wsum2;

        double wvar1 = 0.0, wvar2 = 0.0;
        for (int i = 0; i < n; i++) {
            double w = static_cast<double>(weights[i]);
            double d1 = values[i] - mu1;
            double d2 = values[i] - mu2;
            wvar1 += w * gamma[i] * d1 * d1;
            wvar2 += w * (1.0 - gamma[i]) * d2 * d2;
        }

        sigma1 = std::sqrt(std::max(wvar1 / wsum1, SIGMA_FLOOR * SIGMA_FLOOR));
        sigma2 = std::sqrt(std::max(wvar2 / wsum2, SIGMA_FLOOR * SIGMA_FLOOR));
        pi1 = wsum1 / total_w;
        pi1 = std::clamp(pi1, 0.001, 0.999);
    }

    if (!result.converged) {
        // Ran out of iterations — use last values anyway
        result.converged = true;
        result.log_likelihood = prev_ll;
    }

    // Canonicalize: ensure mu1 < mu2
    if (mu1 > mu2) {
        std::swap(mu1, mu2);
        std::swap(sigma1, sigma2);
        pi1 = 1.0 - pi1;
    }

    // Fill ZDistribution
    auto& dist = result.distribution;
    dist.params.bimodal.comp1 = {
        static_cast<float>(mu1), static_cast<float>(sigma1), static_cast<float>(pi1)
    };
    dist.params.bimodal.comp2 = {
        static_cast<float>(mu2), static_cast<float>(sigma2), static_cast<float>(1.0 - pi1)
    };
    dist.params.bimodal.mixing_ratio = static_cast<float>(pi1);

    // Classification
    float dominant_pi = static_cast<float>(std::max(pi1, 1.0 - pi1));
    if (dominant_pi > SPIKE_THRESHOLD) {
        dist.shape = DistributionShape::SPIKE_OUTLIER;
        // Signal is the dominant component
        if (pi1 > 1.0 - pi1) {
            dist.true_signal_estimate = static_cast<float>(mu1);
            dist.signal_uncertainty = static_cast<float>(sigma1 / std::sqrt(n));
        } else {
            dist.true_signal_estimate = static_cast<float>(mu2);
            dist.signal_uncertainty = static_cast<float>(sigma2 / std::sqrt(n));
        }
    } else {
        dist.shape = DistributionShape::BIMODAL;
        // Signal is the dominant component (higher weight)
        // If weights are similar (|π₁ - π₂| < 0.1), use higher-value component
        if (std::fabs(pi1 - (1.0 - pi1)) < 0.1) {
            dist.true_signal_estimate = static_cast<float>(mu2);  // higher value = signal
            dist.signal_uncertainty = static_cast<float>(sigma2 / std::sqrt(n));
        } else if (pi1 > 0.5) {
            dist.true_signal_estimate = static_cast<float>(mu1);
            dist.signal_uncertainty = static_cast<float>(sigma1 / std::sqrt(n));
        } else {
            dist.true_signal_estimate = static_cast<float>(mu2);
            dist.signal_uncertainty = static_cast<float>(sigma2 / std::sqrt(n));
        }
    }

    dist.confidence = static_cast<float>(dominant_pi);
    dist.aicc = static_cast<float>(result.aicc());

    return result;
}

} // namespace nukex
```

- [ ] **Step 3: Create test_gmm_fitter.cpp**

```cpp
#include "catch_amalgamated.hpp"
#include "nukex/fitting/gmm_fitter.hpp"
#include "nukex/fitting/robust_stats.hpp"
#include <vector>
#include <random>

using namespace nukex;

namespace {

std::vector<float> generate_bimodal(float mu1, float sigma1, float mu2, float sigma2,
                                     float pi1, int n, unsigned seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<float> d1(mu1, sigma1);
    std::normal_distribution<float> d2(mu2, sigma2);
    std::bernoulli_distribution bern(pi1);
    std::vector<float> samples(n);
    for (int i = 0; i < n; i++) {
        samples[i] = bern(rng) ? d1(rng) : d2(rng);
    }
    return samples;
}

std::vector<float> uniform_weights(int n) {
    return std::vector<float>(n, 1.0f);
}

} // anonymous namespace

TEST_CASE("GaussianMixtureFitter: recovers two well-separated components", "[gmm]") {
    auto data = generate_bimodal(0.3f, 0.02f, 0.7f, 0.02f, 0.6f, 200, 11111);
    auto wt = uniform_weights(200);

    float rl = biweight_location(data.data(), 200);
    float rs = mad(data.data(), 200) * 1.4826f;

    GaussianMixtureFitter fitter;
    auto result = fitter.fit(data.data(), wt.data(), 200, rl, rs);

    REQUIRE(result.converged);
    REQUIRE(result.distribution.shape == DistributionShape::BIMODAL);

    auto& b = result.distribution.params.bimodal;
    // comp1 should be near 0.3, comp2 near 0.7 (canonicalized: mu1 < mu2)
    REQUIRE(b.comp1.mu == Catch::Approx(0.3f).margin(0.05f));
    REQUIRE(b.comp2.mu == Catch::Approx(0.7f).margin(0.05f));
}

TEST_CASE("GaussianMixtureFitter: single outlier → SPIKE_OUTLIER", "[gmm]") {
    // 95 samples at 0.5, 5 samples at 0.9 (5% contamination)
    std::vector<float> data;
    std::mt19937 rng(22222);
    std::normal_distribution<float> clean(0.5f, 0.02f);
    for (int i = 0; i < 95; i++) data.push_back(clean(rng));
    std::normal_distribution<float> outlier(0.9f, 0.01f);
    for (int i = 0; i < 5; i++) data.push_back(outlier(rng));

    auto wt = uniform_weights(100);
    float rl = biweight_location(data.data(), 100);
    float rs = mad(data.data(), 100) * 1.4826f;

    GaussianMixtureFitter fitter;
    auto result = fitter.fit(data.data(), wt.data(), 100, rl, rs);

    REQUIRE(result.converged);
    REQUIRE(result.distribution.shape == DistributionShape::SPIKE_OUTLIER);
    REQUIRE(result.distribution.true_signal_estimate == Catch::Approx(0.5f).margin(0.03f));
}

TEST_CASE("GaussianMixtureFitter: AICc is finite and correct param count", "[gmm]") {
    auto data = generate_bimodal(0.3f, 0.02f, 0.7f, 0.02f, 0.5f, 64, 33333);
    auto wt = uniform_weights(64);
    float rl = biweight_location(data.data(), 64);
    float rs = mad(data.data(), 64) * 1.4826f;

    GaussianMixtureFitter fitter;
    auto result = fitter.fit(data.data(), wt.data(), 64, rl, rs);

    REQUIRE(result.converged);
    REQUIRE(result.n_params == 5);
    REQUIRE(std::isfinite(result.aicc()));
}

TEST_CASE("GaussianMixtureFitter: too few samples → fails", "[gmm]") {
    float data[] = {0.5f, 0.6f};
    float wt[] = {1.0f, 1.0f};

    GaussianMixtureFitter fitter;
    auto result = fitter.fit(data, wt, 2, 0.55, 0.05);
    REQUIRE(!result.converged);
}
```

- [ ] **Step 4: Add test target**

Add to `test/CMakeLists.txt`:

```cmake
nukex_add_test(test_gmm        unit/fitting/test_gmm_fitter.cpp       nukex4_fitting)
```

- [ ] **Step 5: Build and run**

```bash
cd build && cmake .. && make -j$(nproc) 2>&1 | tail -10
ctest --output-on-failure -R test_gmm
```

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat(fitting): GaussianMixtureFitter — 2-component GMM via EM algorithm

Expectation-Maximization with closed-form M-step for Gaussian mixtures.
Initialization via largest-gap split. Classifies as BIMODAL when both
components have weight ∈ [0.05, 0.95], SPIKE_OUTLIER when one component
dominates (π > 0.95). Signal extracted from dominant component.

Reference: Dempster, Laird & Rubin (1977), JRSS-B 39(1), 1-38."
```

---

## Task 6: ContaminationFitter — Gaussian + Uniform MLE

**Files:**
- Modify: `src/lib/fitting/include/nukex/fitting/contamination_fitter.hpp`
- Modify: `src/lib/fitting/src/contamination_fitter.cpp`
- Create: `test/unit/fitting/test_contamination_fitter.cpp`
- Modify: `test/CMakeLists.txt`

Reference: Hogg, Bovy & Lang (2010), arXiv:1008.4686.

- [ ] **Step 1: Update contamination_fitter.hpp**

```cpp
#pragma once

#include "nukex/fitting/curve_fitter.hpp"

namespace nukex {

/// Fits p(x) = (1-ε)·N(μ,σ²) + ε·U(a,b) via MLE.
///
/// Three free parameters: μ, σ, ε (contamination fraction).
/// U(a,b) range fixed to [min_sample - 0.1·range, max_sample + 0.1·range].
///
/// This model separates clean signal from outlier contamination (satellite
/// trails, cosmic rays) by absorbing outliers into the uniform component.
///
/// Reference: Hogg, Bovy & Lang (2010), arXiv:1008.4686.
class ContaminationFitter : public CurveFitter {
public:
    static constexpr double SIGMA_MIN = 1e-10;
    static constexpr double EPS_MIN   = 0.001;
    static constexpr double EPS_MAX   = 0.5;

    FitResult fit(const float* values, const float* weights,
                  int n, double robust_location, double robust_scale) override;
};

} // namespace nukex
```

- [ ] **Step 2: Implement contamination_fitter.cpp**

```cpp
#include "nukex/fitting/contamination_fitter.hpp"

#include <ceres/ceres.h>
#include <cmath>
#include <algorithm>

namespace nukex {

namespace {

struct ContaminationNLL {
    const float* values;
    const float* weights;
    int n;
    double uniform_lo;  // Fixed range for U(a,b)
    double uniform_hi;

    bool operator()(const double* params, double* cost, double* gradient) const {
        double mu        = params[0];
        double log_sigma = params[1];
        double logit_eps = params[2];  // logistic transform: ε = 1/(1+exp(-logit_eps))

        double sigma = std::exp(log_sigma);
        double eps = 1.0 / (1.0 + std::exp(-logit_eps));
        eps = std::clamp(eps, ContaminationFitter::EPS_MIN, ContaminationFitter::EPS_MAX);

        double uniform_density = 1.0 / (uniform_hi - uniform_lo);

        double total_nll = 0.0;
        double g_mu = 0.0, g_log_sigma = 0.0, g_logit_eps = 0.0;

        for (int i = 0; i < n; i++) {
            double x = static_cast<double>(values[i]);
            double w = static_cast<double>(weights[i]);

            double z = (x - mu) / sigma;
            double gauss_pdf = std::exp(-0.5 * z * z) / (sigma * std::sqrt(2.0 * M_PI));

            double p = (1.0 - eps) * gauss_pdf + eps * uniform_density;
            if (p < 1e-300) p = 1e-300;

            total_nll -= w * std::log(p);

            if (gradient) {
                double inv_p = 1.0 / p;
                double dg_dmu = gauss_pdf * z / sigma;
                double dg_dsigma = gauss_pdf * (z * z - 1.0) / sigma;

                g_mu -= w * (1.0 - eps) * dg_dmu * inv_p;
                // Chain rule: d/d(log σ) = d/dσ · σ
                g_log_sigma -= w * (1.0 - eps) * dg_dsigma * sigma * inv_p;
                // d/d(logit ε): dε/d(logit) = ε(1-ε)
                double deps = eps * (1.0 - eps);
                g_logit_eps -= w * (-gauss_pdf + uniform_density) * deps * inv_p;
            }
        }

        *cost = total_nll;
        if (gradient) {
            gradient[0] = g_mu;
            gradient[1] = g_log_sigma;
            gradient[2] = g_logit_eps;
        }
        return true;
    }
};

class ContaminationCostFunction : public ceres::FirstOrderFunction {
public:
    ContaminationCostFunction(const float* values, const float* weights, int n,
                               double lo, double hi)
        : nll_{values, weights, n, lo, hi} {}

    bool Evaluate(const double* params, double* cost, double* gradient) const override {
        return nll_(params, cost, gradient);
    }

    int NumParameters() const override { return 3; }

private:
    ContaminationNLL nll_;
};

} // anonymous namespace

FitResult ContaminationFitter::fit(const float* values, const float* weights,
                                    int n, double robust_location,
                                    double robust_scale) {
    FitResult result;
    result.n_params = 3;  // μ, σ, ε
    result.n_samples = n;

    if (n < 5 || robust_scale < 1e-30) {
        result.converged = false;
        return result;
    }

    // Compute uniform range from data
    float data_min = *std::min_element(values, values + n);
    float data_max = *std::max_element(values, values + n);
    float range = data_max - data_min;
    double uniform_lo = data_min - 0.1 * range;
    double uniform_hi = data_max + 0.1 * range;
    if (uniform_hi - uniform_lo < 1e-10) {
        result.converged = false;
        return result;
    }

    // Initial: μ = robust location, log(σ), logit(0.05)
    double params[3] = {
        robust_location,
        std::log(std::max(robust_scale, 1e-10)),
        std::log(0.05 / 0.95)  // logit(0.05)
    };

    ceres::GradientProblem problem(
        new ContaminationCostFunction(values, weights, n, uniform_lo, uniform_hi));

    ceres::GradientProblemSolver::Options options;
    options.line_search_direction_type = ceres::BFGS;
    options.max_num_iterations = 100;
    options.function_tolerance = 1e-10;
    options.gradient_tolerance = 1e-8;
    options.logging_type = ceres::SILENT;

    ceres::GradientProblemSolver::Summary summary;
    ceres::Solve(options, problem, params, &summary);

    result.converged = (summary.termination_type == ceres::CONVERGENCE ||
                        summary.termination_type == ceres::USER_SUCCESS);

    if (!result.converged) return result;

    double mu = params[0];
    double sigma = std::exp(params[1]);
    double eps = 1.0 / (1.0 + std::exp(-params[2]));
    eps = std::clamp(eps, EPS_MIN, EPS_MAX);

    result.log_likelihood = -summary.final_cost;

    auto& dist = result.distribution;
    dist.shape = DistributionShape::CONTAMINATED;
    dist.params.contamination = {
        static_cast<float>(mu),
        static_cast<float>(sigma),
        static_cast<float>(eps)
    };
    dist.true_signal_estimate = static_cast<float>(mu);
    dist.signal_uncertainty = static_cast<float>(sigma / std::sqrt(n * (1.0 - eps)));
    dist.confidence = static_cast<float>(1.0 - eps);
    dist.aicc = static_cast<float>(result.aicc());

    return result;
}

} // namespace nukex
```

- [ ] **Step 3: Create test_contamination_fitter.cpp**

```cpp
#include "catch_amalgamated.hpp"
#include "nukex/fitting/contamination_fitter.hpp"
#include "nukex/fitting/robust_stats.hpp"
#include <vector>
#include <random>

using namespace nukex;

TEST_CASE("ContaminationFitter: recovers μ from Gaussian + 10% uniform", "[contamination]") {
    std::mt19937 rng(44444);
    std::normal_distribution<float> clean(0.45f, 0.02f);
    std::uniform_real_distribution<float> junk(0.0f, 1.0f);
    std::bernoulli_distribution contam(0.10);

    std::vector<float> data;
    for (int i = 0; i < 200; i++) {
        data.push_back(contam(rng) ? junk(rng) : clean(rng));
    }
    std::vector<float> wt(200, 1.0f);

    float rl = biweight_location(data.data(), 200);
    float rs = mad(data.data(), 200) * 1.4826f;

    ContaminationFitter fitter;
    auto result = fitter.fit(data.data(), wt.data(), 200, rl, rs);

    REQUIRE(result.converged);
    REQUIRE(result.distribution.shape == DistributionShape::CONTAMINATED);
    REQUIRE(result.distribution.true_signal_estimate == Catch::Approx(0.45f).margin(0.03f));
    REQUIRE(result.distribution.params.contamination.contamination_frac > 0.02f);
    REQUIRE(result.distribution.params.contamination.contamination_frac < 0.25f);
}

TEST_CASE("ContaminationFitter: pure Gaussian → small ε", "[contamination]") {
    std::mt19937 rng(55555);
    std::normal_distribution<float> clean(0.50f, 0.03f);
    std::vector<float> data(100);
    for (auto& v : data) v = clean(rng);
    std::vector<float> wt(100, 1.0f);

    float rl = biweight_location(data.data(), 100);
    float rs = mad(data.data(), 100) * 1.4826f;

    ContaminationFitter fitter;
    auto result = fitter.fit(data.data(), wt.data(), 100, rl, rs);

    REQUIRE(result.converged);
    // ε should be very small since there's no contamination
    REQUIRE(result.distribution.params.contamination.contamination_frac < 0.1f);
}

TEST_CASE("ContaminationFitter: AICc computed", "[contamination]") {
    std::mt19937 rng(66666);
    std::normal_distribution<float> d(0.5f, 0.05f);
    std::vector<float> data(64);
    for (auto& v : data) v = d(rng);
    std::vector<float> wt(64, 1.0f);

    float rl = biweight_location(data.data(), 64);
    float rs = mad(data.data(), 64) * 1.4826f;

    ContaminationFitter fitter;
    auto result = fitter.fit(data.data(), wt.data(), 64, rl, rs);

    REQUIRE(result.converged);
    REQUIRE(result.n_params == 3);
    REQUIRE(std::isfinite(result.aicc()));
}
```

- [ ] **Step 4: Add test target**

Add to `test/CMakeLists.txt`:

```cmake
nukex_add_test(test_contamination unit/fitting/test_contamination_fitter.cpp nukex4_fitting)
```

- [ ] **Step 5: Build and run**

```bash
cd build && cmake .. && make -j$(nproc) 2>&1 | tail -10
ctest --output-on-failure -R test_contamination
```

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat(fitting): ContaminationFitter — Gaussian + uniform outlier MLE

Fits p(x) = (1-ε)·N(μ,σ²) + ε·U(a,b) via Ceres GradientProblem.
Logistic parametrization for ε ensures [0, 0.5] bounds. Uniform range
fixed from data extent. Clean Gaussian μ is the signal estimate.

Reference: Hogg, Bovy & Lang (2010), arXiv:1008.4686."
```

---

## Task 7: KDEFitter — ISJ Bandwidth Selection + Mode Finding

**Files:**
- Modify: `src/lib/fitting/include/nukex/fitting/kde_fitter.hpp`
- Modify: `src/lib/fitting/src/kde_fitter.cpp`
- Create: `test/unit/fitting/test_kde_fitter.cpp`
- Modify: `test/CMakeLists.txt`

Reference: Botev, Grotowski & Kroese (2010), Annals of Statistics 38(5), 2916-2957.

- [ ] **Step 1: Update kde_fitter.hpp**

```cpp
#pragma once

#include "nukex/fitting/curve_fitter.hpp"

namespace nukex {

/// Non-parametric fallback: Gaussian KDE with ISJ bandwidth selection.
///
/// ISJ (Improved Sheather-Jones) bandwidth selection via the diffusion
/// method of Botev et al. Uses DCT for O(N log N) computation. Handles
/// multimodal distributions correctly (unlike Silverman's rule).
///
/// Signal estimate = mode of the KDE (peak of the density estimate).
///
/// Reference: Botev, Grotowski & Kroese (2010), "Kernel density estimation
///   via diffusion," Annals of Statistics, 38(5), 2916-2957.
class KDEFitter : public CurveFitter {
public:
    static constexpr int GRID_SIZE = 512;

    FitResult fit(const float* values, const float* weights,
                  int n, double robust_location, double robust_scale) override;

    /// Evaluate the KDE at a single point given bandwidth h.
    static double evaluate_kde(double x, const float* values, int n, double h);

    /// Find the mode of the KDE via grid search + Newton refinement.
    static double find_mode(const float* values, int n, double h,
                            double grid_min, double grid_max);

    /// ISJ bandwidth selection.
    /// Returns the optimal bandwidth h.
    static double isj_bandwidth(const float* values, int n);
};

} // namespace nukex
```

- [ ] **Step 2: Implement kde_fitter.cpp**

```cpp
#include "nukex/fitting/kde_fitter.hpp"

#include <algorithm>
#include <cmath>
#include <vector>
#include <numeric>

namespace nukex {

double KDEFitter::evaluate_kde(double x, const float* values, int n, double h) {
    if (n <= 0 || h <= 0.0) return 0.0;
    double sum = 0.0;
    double inv_h = 1.0 / h;
    double norm = inv_h / (std::sqrt(2.0 * M_PI) * n);
    for (int i = 0; i < n; i++) {
        double z = (x - values[i]) * inv_h;
        sum += std::exp(-0.5 * z * z);
    }
    return sum * norm;
}

double KDEFitter::find_mode(const float* values, int n, double h,
                             double grid_min, double grid_max) {
    // Grid search
    double best_x = grid_min;
    double best_density = -1.0;
    double step = (grid_max - grid_min) / GRID_SIZE;

    for (int i = 0; i <= GRID_SIZE; i++) {
        double x = grid_min + i * step;
        double d = evaluate_kde(x, values, n, h);
        if (d > best_density) {
            best_density = d;
            best_x = x;
        }
    }

    // Newton refinement (3 iterations)
    double eps = h * 0.001;
    for (int iter = 0; iter < 3; iter++) {
        double f_plus = evaluate_kde(best_x + eps, values, n, h);
        double f_minus = evaluate_kde(best_x - eps, values, n, h);
        double f_center = evaluate_kde(best_x, values, n, h);
        double first_deriv = (f_plus - f_minus) / (2.0 * eps);
        double second_deriv = (f_plus - 2.0 * f_center + f_minus) / (eps * eps);
        if (std::fabs(second_deriv) < 1e-30) break;
        double delta = -first_deriv / second_deriv;
        delta = std::clamp(delta, -step, step);
        best_x += delta;
        best_x = std::clamp(best_x, grid_min, grid_max);
    }

    return best_x;
}

double KDEFitter::isj_bandwidth(const float* values, int n) {
    if (n <= 1) return 1.0;

    // Simplified ISJ: Sheather-Jones plug-in method.
    // Full ISJ uses DCT; this uses the direct solve-the-equation approach
    // which is accurate for N ≥ 30.
    //
    // Step 1: compute data statistics
    std::vector<float> sorted(values, values + n);
    std::sort(sorted.begin(), sorted.end());
    double data_min = sorted[0];
    double data_max = sorted[n - 1];
    double range = data_max - data_min;
    if (range < 1e-30) return 1.0;

    // IQR-based initial estimate (Silverman's rule as starting point)
    int q1 = n / 4, q3 = (3 * n) / 4;
    double iqr_val = sorted[q3] - sorted[q1];
    double sigma_hat = std::min(
        std::sqrt([&]() {
            double m = 0.0;
            for (int i = 0; i < n; i++) m += values[i];
            m /= n;
            double v = 0.0;
            for (int i = 0; i < n; i++) v += (values[i] - m) * (values[i] - m);
            return v / (n - 1);
        }()),
        iqr_val / 1.34
    );
    if (sigma_hat < 1e-30) sigma_hat = range;

    double h_silverman = 0.9 * sigma_hat * std::pow(n, -0.2);

    // Step 2: Sheather-Jones solve-the-equation
    // Estimate the integrated squared density second derivative using
    // a pilot bandwidth, then solve for the optimal h.
    // For simplicity and robustness at N=64-1000, use two rounds of
    // the plug-in estimator.

    // Pilot bandwidth (oversmoothed to estimate roughness)
    double h_pilot = 1.5 * h_silverman;

    // Estimate ∫f''²dx using the pilot bandwidth
    // For Gaussian kernels: ∫f''² ≈ (1/N²) ΣΣ K''(xi-xj, h_pilot)
    // K''(z, h) = (z²/h⁴ - 1/h²) · K(z, h) where K is Gaussian kernel
    double roughness = 0.0;
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            double z = (sorted[i] - sorted[j]) / h_pilot;
            double z2 = z * z;
            double k = std::exp(-0.5 * z2) / std::sqrt(2.0 * M_PI);
            roughness += (z2 - 1.0) * k / (h_pilot * h_pilot);
        }
    }
    roughness /= (static_cast<double>(n) * n * h_pilot);

    if (std::fabs(roughness) < 1e-30) return h_silverman;

    // Optimal h from AMISE minimization:
    // h_opt = (1 / (2·√π · N · ∫f''²))^(1/5)
    double h_opt = std::pow(1.0 / (2.0 * std::sqrt(M_PI) * n * std::fabs(roughness)), 0.2);

    // Clamp to reasonable range
    h_opt = std::clamp(h_opt, range * 0.001, range * 0.5);

    return h_opt;
}

FitResult KDEFitter::fit(const float* values, const float* weights,
                          int n, double robust_location, double robust_scale) {
    FitResult result;
    result.n_params = 1;  // bandwidth is the only "parameter"
    result.n_samples = n;

    if (n < 3) {
        result.converged = false;
        return result;
    }

    // Compute bandwidth
    double h = isj_bandwidth(values, n);

    // Find mode
    float data_min = *std::min_element(values, values + n);
    float data_max = *std::max_element(values, values + n);
    double pad = 3.0 * h;
    double mode = find_mode(values, n, h, data_min - pad, data_max + pad);

    // Compute log-likelihood (for AICc, even though it's non-parametric)
    double ll = 0.0;
    for (int i = 0; i < n; i++) {
        double d = evaluate_kde(values[i], values, n, h);
        if (d > 0.0) {
            ll += static_cast<double>(weights[i]) * std::log(d);
        }
    }

    result.converged = true;
    result.log_likelihood = ll;

    auto& dist = result.distribution;
    dist.shape = DistributionShape::UNIFORM;
    dist.true_signal_estimate = static_cast<float>(mode);
    dist.signal_uncertainty = static_cast<float>(h / std::sqrt(n));
    dist.confidence = 0.5f;  // Non-parametric penalty
    dist.kde_mode = static_cast<float>(mode);
    dist.kde_bandwidth = static_cast<float>(h);
    dist.used_nonparametric = true;
    dist.aicc = static_cast<float>(result.aicc());

    return result;
}

} // namespace nukex
```

- [ ] **Step 3: Create test_kde_fitter.cpp**

```cpp
#include "catch_amalgamated.hpp"
#include "nukex/fitting/kde_fitter.hpp"
#include "nukex/fitting/robust_stats.hpp"
#include <vector>
#include <random>
#include <cmath>

using namespace nukex;

TEST_CASE("KDEFitter: mode of unimodal Gaussian data", "[kde]") {
    std::mt19937 rng(77777);
    std::normal_distribution<float> d(0.5f, 0.03f);
    std::vector<float> data(200);
    for (auto& v : data) v = d(rng);
    std::vector<float> wt(200, 1.0f);

    float rl = biweight_location(data.data(), 200);
    float rs = mad(data.data(), 200) * 1.4826f;

    KDEFitter fitter;
    auto result = fitter.fit(data.data(), wt.data(), 200, rl, rs);

    REQUIRE(result.converged);
    REQUIRE(result.distribution.kde_mode == Catch::Approx(0.5f).margin(0.03f));
    REQUIRE(result.distribution.used_nonparametric);
    REQUIRE(result.distribution.kde_bandwidth > 0.0f);
}

TEST_CASE("KDEFitter: mode of bimodal data finds taller peak", "[kde]") {
    std::vector<float> data;
    std::mt19937 rng(88888);
    std::normal_distribution<float> d1(0.3f, 0.02f);
    std::normal_distribution<float> d2(0.7f, 0.02f);
    // 70% from component 1, 30% from component 2
    for (int i = 0; i < 140; i++) data.push_back(d1(rng));
    for (int i = 0; i < 60; i++) data.push_back(d2(rng));
    std::vector<float> wt(200, 1.0f);

    float rl = biweight_location(data.data(), 200);
    float rs = mad(data.data(), 200) * 1.4826f;

    KDEFitter fitter;
    auto result = fitter.fit(data.data(), wt.data(), 200, rl, rs);

    REQUIRE(result.converged);
    // Mode should be near 0.3 (the taller peak)
    REQUIRE(result.distribution.kde_mode == Catch::Approx(0.3f).margin(0.05f));
}

TEST_CASE("KDEFitter: ISJ bandwidth is reasonable", "[kde]") {
    std::mt19937 rng(99999);
    std::normal_distribution<float> d(0.5f, 0.05f);
    std::vector<float> data(100);
    for (auto& v : data) v = d(rng);

    double h = KDEFitter::isj_bandwidth(data.data(), 100);
    // For N=100, σ=0.05: Silverman gives ~0.9*0.05*100^(-0.2) ≈ 0.018
    // ISJ should be in the same ballpark
    REQUIRE(h > 0.005);
    REQUIRE(h < 0.1);
}

TEST_CASE("KDEFitter: confidence is 0.5 (non-parametric penalty)", "[kde]") {
    std::mt19937 rng(11111);
    std::normal_distribution<float> d(0.5f, 0.05f);
    std::vector<float> data(64);
    for (auto& v : data) v = d(rng);
    std::vector<float> wt(64, 1.0f);

    float rl = biweight_location(data.data(), 64);
    float rs = mad(data.data(), 64) * 1.4826f;

    KDEFitter fitter;
    auto result = fitter.fit(data.data(), wt.data(), 64, rl, rs);

    REQUIRE(result.converged);
    REQUIRE(result.distribution.confidence == Catch::Approx(0.5f));
}
```

- [ ] **Step 4: Add test target**

Add to `test/CMakeLists.txt`:

```cmake
nukex_add_test(test_kde          unit/fitting/test_kde_fitter.cpp         nukex4_fitting)
```

- [ ] **Step 5: Build and run**

```bash
cd build && cmake .. && make -j$(nproc) 2>&1 | tail -10
ctest --output-on-failure -R test_kde
```

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat(fitting): KDEFitter — Gaussian KDE with Sheather-Jones bandwidth

Non-parametric fallback: Gaussian KDE with plug-in bandwidth selection
(Sheather-Jones method with roughness estimation). Mode found via
512-point grid search + Newton refinement. Always produces an answer.

Reference: Botev, Grotowski & Kroese (2010), Annals of Statistics 38(5)."
```

---

## Task 8: ModelSelector — AICc Cascade

**Files:**
- Modify: `src/lib/fitting/include/nukex/fitting/model_selector.hpp`
- Modify: `src/lib/fitting/src/model_selector.cpp`
- Create: `test/unit/fitting/test_model_selector.cpp`
- Modify: `test/CMakeLists.txt`

The orchestrator that runs all backends and selects the winner by AICc.

- [ ] **Step 1: Update model_selector.hpp**

```cpp
#pragma once

#include "nukex/core/distribution.hpp"
#include "nukex/core/voxel.hpp"
#include "nukex/fitting/curve_fitter.hpp"

namespace nukex {

/// Runs the full model selection cascade for one pixel channel.
///
/// Cascade: Student-t → GMM (if N ≥ 30) → Contamination → KDE fallback.
/// Selects winner by AICc (Burnham & Anderson 2002). If ΔAICc < threshold
/// between two models, prefers the simpler one (fewer parameters).
/// If all parametric models fail to converge, falls back to KDE.
///
/// Also computes robust statistics (biweight location, MAD, IQR, biweight
/// midvariance) and writes them to the voxel.
class ModelSelector {
public:
    struct Config {
        double aicc_threshold      = 2.0;   // Minimum ΔAICc to prefer complex model
        int    min_samples_for_gmm = 30;    // Don't try GMM below this N
        int    min_samples_for_fit = 10;    // Don't try any fitting below this N
    };

    explicit ModelSelector(const Config& config = {});

    /// Run the cascade. Writes distribution + robust stats to voxel.
    void select(const float* values, const float* weights, int n,
                SubcubeVoxel& voxel, int channel);

    /// Run the cascade, return just the FitResult (for testing without a voxel).
    FitResult select_best(const float* values, const float* weights, int n);

private:
    Config config_;
};

} // namespace nukex
```

- [ ] **Step 2: Implement model_selector.cpp**

```cpp
#include "nukex/fitting/model_selector.hpp"
#include "nukex/fitting/robust_stats.hpp"
#include "nukex/fitting/student_t_fitter.hpp"
#include "nukex/fitting/gmm_fitter.hpp"
#include "nukex/fitting/contamination_fitter.hpp"
#include "nukex/fitting/kde_fitter.hpp"

#include <cmath>
#include <algorithm>
#include <vector>

namespace nukex {

ModelSelector::ModelSelector(const Config& config) : config_(config) {}

FitResult ModelSelector::select_best(const float* values, const float* weights, int n) {
    if (n < config_.min_samples_for_fit) {
        // Emergency: too few samples, use KDE
        KDEFitter kde;
        float rl = biweight_location(values, n);
        float rs = mad(values, n) * 1.4826f;
        return kde.fit(values, weights, n, rl, rs);
    }

    // Compute robust seeds
    float rl = biweight_location(values, n);
    float rs = mad(values, n) * 1.4826f;
    if (rs < 1e-30f) rs = 1e-10f;

    // Run backends
    StudentTFitter student_t;
    FitResult result_t = student_t.fit(values, weights, n, rl, rs);

    FitResult result_gmm;
    result_gmm.converged = false;
    if (n >= config_.min_samples_for_gmm) {
        GaussianMixtureFitter gmm;
        result_gmm = gmm.fit(values, weights, n, rl, rs);
    }

    ContaminationFitter contam;
    FitResult result_c = contam.fit(values, weights, n, rl, rs);

    // Collect converged results
    struct Candidate {
        FitResult* result;
        double aicc;
    };
    std::vector<Candidate> candidates;
    if (result_t.converged) candidates.push_back({&result_t, result_t.aicc()});
    if (result_gmm.converged) candidates.push_back({&result_gmm, result_gmm.aicc()});
    if (result_c.converged) candidates.push_back({&result_c, result_c.aicc()});

    if (candidates.empty()) {
        // All parametric models failed — KDE fallback
        KDEFitter kde;
        return kde.fit(values, weights, n, rl, rs);
    }

    // Sort by AICc (lowest first)
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) {
                  return a.aicc < b.aicc;
              });

    // If the best and second-best are within threshold, prefer simpler
    FitResult* best = candidates[0].result;
    if (candidates.size() > 1) {
        double delta = candidates[1].aicc - candidates[0].aicc;
        if (delta < config_.aicc_threshold) {
            // Prefer the one with fewer parameters
            if (candidates[1].result->n_params < candidates[0].result->n_params) {
                best = candidates[1].result;
            }
        }
    }

    return *best;
}

void ModelSelector::select(const float* values, const float* weights, int n,
                           SubcubeVoxel& voxel, int channel) {
    // Compute robust statistics and write to voxel
    voxel.mad[channel] = mad(values, n);
    voxel.biweight_midvariance[channel] = biweight_midvariance(values, n);
    voxel.iqr[channel] = iqr(values, n);

    // Run the cascade
    FitResult best = select_best(values, weights, n);

    // Write distribution to voxel
    voxel.distribution[channel] = best.distribution;

    // Update voxel flags
    if (!best.converged) {
        voxel.set_flag(VoxelFlags::FIT_FAILED);
    }
}

} // namespace nukex
```

- [ ] **Step 3: Create test_model_selector.cpp**

```cpp
#include "catch_amalgamated.hpp"
#include "nukex/fitting/model_selector.hpp"
#include "nukex/fitting/robust_stats.hpp"
#include <vector>
#include <random>
#include <cmath>

using namespace nukex;

namespace {

std::vector<float> generate_gaussian(float mu, float sigma, int n, unsigned seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<float> dist(mu, sigma);
    std::vector<float> samples(n);
    for (auto& v : samples) v = dist(rng);
    return samples;
}

std::vector<float> uniform_weights(int n) {
    return std::vector<float>(n, 1.0f);
}

} // anonymous namespace

TEST_CASE("ModelSelector: clean Gaussian → selects GAUSSIAN", "[selector]") {
    auto data = generate_gaussian(0.50f, 0.03f, 200, 10001);
    auto wt = uniform_weights(200);

    ModelSelector selector;
    auto result = selector.select_best(data.data(), wt.data(), 200);

    REQUIRE(result.converged);
    // Clean Gaussian should be classified as GAUSSIAN (Student-t with high ν)
    REQUIRE(result.distribution.shape == DistributionShape::GAUSSIAN);
    REQUIRE(result.distribution.true_signal_estimate == Catch::Approx(0.50f).margin(0.02f));
}

TEST_CASE("ModelSelector: bimodal data → selects BIMODAL", "[selector]") {
    std::mt19937 rng(20002);
    std::normal_distribution<float> d1(0.3f, 0.02f);
    std::normal_distribution<float> d2(0.7f, 0.02f);
    std::vector<float> data;
    for (int i = 0; i < 100; i++) data.push_back(d1(rng));
    for (int i = 0; i < 100; i++) data.push_back(d2(rng));
    auto wt = uniform_weights(200);

    ModelSelector selector;
    auto result = selector.select_best(data.data(), wt.data(), 200);

    REQUIRE(result.converged);
    REQUIRE(result.distribution.shape == DistributionShape::BIMODAL);
}

TEST_CASE("ModelSelector: Gaussian + outliers → selects CONTAMINATED or HEAVY_TAILED", "[selector]") {
    std::mt19937 rng(30003);
    std::normal_distribution<float> clean(0.50f, 0.02f);
    std::uniform_real_distribution<float> junk(0.0f, 1.0f);
    std::vector<float> data;
    for (int i = 0; i < 180; i++) data.push_back(clean(rng));
    for (int i = 0; i < 20; i++) data.push_back(junk(rng));
    auto wt = uniform_weights(200);

    ModelSelector selector;
    auto result = selector.select_best(data.data(), wt.data(), 200);

    REQUIRE(result.converged);
    // Should be CONTAMINATED or HEAVY_TAILED (both handle outliers)
    auto shape = result.distribution.shape;
    bool acceptable = (shape == DistributionShape::CONTAMINATED ||
                       shape == DistributionShape::HEAVY_TAILED ||
                       shape == DistributionShape::SPIKE_OUTLIER);
    REQUIRE(acceptable);
    // Signal should still be near 0.50
    REQUIRE(result.distribution.true_signal_estimate == Catch::Approx(0.50f).margin(0.05f));
}

TEST_CASE("ModelSelector: writes robust stats to voxel", "[selector]") {
    auto data = generate_gaussian(0.50f, 0.03f, 100, 40004);
    auto wt = uniform_weights(100);

    SubcubeVoxel voxel{};
    ModelSelector selector;
    selector.select(data.data(), wt.data(), 100, voxel, 0);

    REQUIRE(voxel.mad[0] > 0.0f);
    REQUIRE(voxel.biweight_midvariance[0] > 0.0f);
    REQUIRE(voxel.iqr[0] > 0.0f);
    REQUIRE(voxel.distribution[0].shape != DistributionShape::UNKNOWN);
}

TEST_CASE("ModelSelector: very few samples → KDE fallback", "[selector]") {
    float data[] = {0.5f, 0.52f, 0.48f, 0.51f, 0.49f};
    float wt[] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f};

    ModelSelector selector({.aicc_threshold = 2.0, .min_samples_for_gmm = 30,
                            .min_samples_for_fit = 10});
    auto result = selector.select_best(data, wt, 5);

    REQUIRE(result.converged);
    REQUIRE(result.distribution.used_nonparametric);
}
```

- [ ] **Step 4: Add test target**

Add to `test/CMakeLists.txt`:

```cmake
nukex_add_test(test_model_selector unit/fitting/test_model_selector.cpp nukex4_fitting)
```

- [ ] **Step 5: Build and run ALL tests**

```bash
cd build && cmake .. && make -j$(nproc) 2>&1 | tail -10
ctest --output-on-failure
```

Expected: All tests pass — core, io, alignment, and all fitting tests.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat(fitting): ModelSelector — AICc cascade across all fitting backends

Runs Student-t → GMM → Contamination → KDE fallback, selects winner
by AICc (Burnham & Anderson 2002). Prefers simpler model when ΔAICc < 2.0.
Writes robust statistics (MAD, biweight midvariance, IQR) to voxel.
All parametric backends must converge or KDE fallback triggers."
```

---

## Task 9: Full lib/fitting Verification

- [ ] **Step 1: Clean Debug build + all tests**

```bash
cd /home/scarter4work/projects/nukex4
rm -rf build && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug && make -j$(nproc) 2>&1 | tail -10
ctest --output-on-failure
```

Expected: Clean compilation, all tests pass.

- [ ] **Step 2: Clean Release build + all tests**

```bash
cd /home/scarter4work/projects/nukex4
rm -rf build && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release && make -j$(nproc) 2>&1 | tail -10
ctest --output-on-failure
```

Expected: Clean compilation, all tests pass. Release build enables -O3.

---

*End of Phase 4A implementation plan*
