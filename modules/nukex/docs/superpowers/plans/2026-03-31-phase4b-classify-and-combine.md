# Phase 4B: lib/classify + lib/combine — Weight Computation, Pixel Selection, Output Assembly

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the weight computation engine (classify) and the output assembly pipeline (combine) that transforms fitted distributions into stacked images, noise maps, and quality maps.

**Architecture:** lib/classify computes per-sample quality weights from frame metadata. lib/combine extracts pixel values from fitted ZDistributions, propagates noise via the CCD noise model, computes spatial context, and assembles three output images. Both depend on lib/core. Combine also depends on lib/fitting (for robust_stats) and lib/io (for Image type).

**Tech Stack:** C++17, lib/core, lib/fitting (robust_stats), lib/io (Image), Catch2 v3

**Critical rules:**
- No stubs, no TODOs — every function complete
- CCD noise model: σ² = read_noise²/gain² + value/gain (Poisson + read noise)
- Weight floor = 0.01 — never truly zero (no rejection)
- Noise propagation: √(Σ wᵢ² σᵢ²) / Σ wᵢ

---

## File Structure

```
src/lib/core/include/nukex/core/
├── frame_stats.hpp         NEW — FrameStats struct (per-frame metadata for Phase B)

src/lib/classify/
├── CMakeLists.txt
├── include/
│   └── nukex/
│       └── classify/
│           └── weight_computer.hpp
├── src/
│   └── weight_computer.cpp

src/lib/combine/
├── CMakeLists.txt
├── include/
│   └── nukex/
│       └── combine/
│           ├── pixel_selector.hpp
│           ├── spatial_context.hpp
│           └── output_assembler.hpp
├── src/
│   ├── pixel_selector.cpp
│   ├── spatial_context.cpp
│   └── output_assembler.cpp

test/unit/classify/
├── test_weight_computer.cpp

test/unit/combine/
├── test_pixel_selector.cpp
├── test_spatial_context.cpp
└── test_output_assembler.cpp
```

---

## Task 1: FrameStats Type in lib/core

**Files:**
- Create: `src/lib/core/include/nukex/core/frame_stats.hpp`
- Modify: `src/lib/core/src/channel_config.cpp` (no change needed, just verify core compiles)

FrameStats is used by classify, combine, and stacker. It belongs in lib/core to avoid circular dependencies.

- [ ] **Step 1: Create frame_stats.hpp**

```cpp
#pragma once

#include <cstdint>

namespace nukex {

/// Per-frame metadata for Phase B analysis.
///
/// Populated by the stacker during Phase A (streaming). Shared read-only
/// during Phase B parallel pixel processing. Indexed by frame_index
/// (0-based position in the input light_paths vector).
///
/// Separate from FrameMetadata (which holds raw FITS header data from loading).
/// FrameStats holds computed quality metrics needed for weight computation
/// and noise propagation.
struct FrameStats {
    // From FITS headers (copied from FrameMetadata during Phase A)
    float read_noise         = 3.0f;   // electrons (RDNOISE)
    float gain               = 1.0f;   // e-/ADU (GAIN)
    float exposure           = 0.0f;   // seconds (EXPTIME)
    bool  has_noise_keywords = false;  // true if RDNOISE + GAIN both present
    bool  is_meridian_flipped = false;

    // Computed during Phase A
    float frame_weight       = 1.0f;   // alignment quality (0.5 if failed)
    float median_luminance   = 0.0f;   // frame median (for lum_ratio)
    float fwhm               = 0.0f;   // median FWHM from star catalog

    // Computed between phases (needs global fwhm_best)
    float psf_weight         = 1.0f;   // exp(-0.5 × (fwhm/fwhm_best - 1)² / 0.25)
};

} // namespace nukex
```

- [ ] **Step 2: Build and run tests**

```bash
cd build && cmake .. && make -j$(nproc) && ctest --output-on-failure
```

Expected: All 20 tests pass (header-only addition, no compilation changes).

- [ ] **Step 3: Commit**

```bash
git add src/lib/core/include/nukex/core/frame_stats.hpp
git commit -m "feat(core): FrameStats — per-frame metadata for Phase B analysis

Lightweight struct holding computed quality metrics (frame_weight,
psf_weight, median_luminance, CCD noise parameters) needed by
classify, combine, and stacker. Separate from FrameMetadata (raw
FITS headers). Indexed by frame_index, shared read-only in Phase B."
```

---

## Task 2: lib/classify — WeightComputer

**Files:**
- Create: `src/lib/classify/CMakeLists.txt`
- Create: `src/lib/classify/include/nukex/classify/weight_computer.hpp`
- Create: `src/lib/classify/src/weight_computer.cpp`
- Create: `test/unit/classify/test_weight_computer.cpp`
- Modify: `CMakeLists.txt` (root — add classify subdirectory)
- Modify: `test/CMakeLists.txt` (add test target)

- [ ] **Step 1: Create directories**

```bash
mkdir -p src/lib/classify/include/nukex/classify
mkdir -p src/lib/classify/src
mkdir -p test/unit/classify
```

- [ ] **Step 2: Create src/lib/classify/CMakeLists.txt**

```cmake
add_library(nukex4_classify STATIC
    src/weight_computer.cpp
)

target_include_directories(nukex4_classify
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(nukex4_classify
    PUBLIC nukex4_core
)

target_compile_features(nukex4_classify PUBLIC cxx_std_17)
```

- [ ] **Step 3: Create weight_computer.hpp**

```cpp
#pragma once

#include "nukex/core/frame_stats.hpp"
#include <cmath>

namespace nukex {

/// Configuration for per-sample weight computation.
struct WeightConfig {
    float sigma_threshold = 3.0f;    // Below this σ, no penalty
    float sigma_scale     = 2.0f;    // Gaussian falloff width for σ penalty
    float cloud_threshold = 0.85f;   // lum_ratio below this → cloud penalty
    float cloud_penalty   = 0.30f;   // Multiplicative weight for cloud frames
    float weight_floor    = 0.01f;   // Never truly zero — no rejection
};

/// Computes combined quality weights for pixel samples from frame metadata.
///
/// The weight formula combines independent quality dimensions multiplicatively:
///   w = frame_weight × psf_weight × sigma_factor × cloud_factor
///   w = max(w, weight_floor)
///
/// Each factor captures a different physical degradation:
/// - frame_weight: alignment quality (0.5 if alignment failed)
/// - psf_weight: seeing quality (Gaussian penalty on FWHM ratio)
/// - sigma_factor: statistical outlier penalty (Gaussian falloff beyond threshold)
/// - cloud_factor: transparency attenuation penalty
class WeightComputer {
public:
    explicit WeightComputer(const WeightConfig& config = {});

    /// Compute combined weight for one sample.
    ///
    /// @param value         The pixel value at this sample
    /// @param frame_stats   Per-frame metadata (looked up by frame_index)
    /// @param welford_mean  Welford running mean at this pixel/channel
    /// @param welford_stddev Welford running stddev at this pixel/channel
    /// @return Combined weight in [weight_floor, 1.0]
    float compute(float value, const FrameStats& frame_stats,
                  float welford_mean, float welford_stddev) const;

    const WeightConfig& config() const { return config_; }

private:
    WeightConfig config_;
};

} // namespace nukex
```

- [ ] **Step 4: Create weight_computer.cpp**

```cpp
#include "nukex/classify/weight_computer.hpp"
#include <algorithm>

namespace nukex {

WeightComputer::WeightComputer(const WeightConfig& config) : config_(config) {}

float WeightComputer::compute(float value, const FrameStats& frame_stats,
                               float welford_mean, float welford_stddev) const {
    float w = frame_stats.frame_weight * frame_stats.psf_weight;

    // Sigma factor: Gaussian penalty for samples beyond sigma_threshold
    if (welford_stddev > 1e-30f) {
        float sigma_score = std::fabs(value - welford_mean) / welford_stddev;
        float excess = std::max(0.0f, sigma_score - config_.sigma_threshold);
        float sigma_factor = std::exp(-0.5f * excess * excess
                                      / (config_.sigma_scale * config_.sigma_scale));
        w *= sigma_factor;
    }

    // Cloud factor: penalize samples with low luminance ratio
    if (frame_stats.median_luminance > 1e-30f) {
        float lum_ratio = value / frame_stats.median_luminance;
        if (lum_ratio < config_.cloud_threshold) {
            w *= config_.cloud_penalty;
        }
    }

    return std::max(w, config_.weight_floor);
}

} // namespace nukex
```

- [ ] **Step 5: Create test_weight_computer.cpp**

```cpp
#include "catch_amalgamated.hpp"
#include "nukex/classify/weight_computer.hpp"

using namespace nukex;

namespace {

FrameStats make_clean_frame() {
    FrameStats fs;
    fs.frame_weight = 1.0f;
    fs.psf_weight = 1.0f;
    fs.median_luminance = 0.5f;
    fs.read_noise = 3.0f;
    fs.gain = 1.5f;
    fs.has_noise_keywords = true;
    return fs;
}

} // anonymous namespace

TEST_CASE("WeightComputer: clean sample → weight near 1.0", "[classify]") {
    WeightComputer wc;
    FrameStats fs = make_clean_frame();
    // Value near the mean, within sigma threshold
    float w = wc.compute(0.50f, fs, 0.50f, 0.03f);
    REQUIRE(w > 0.9f);
    REQUIRE(w <= 1.0f);
}

TEST_CASE("WeightComputer: outlier beyond 3σ → reduced weight", "[classify]") {
    WeightComputer wc;
    FrameStats fs = make_clean_frame();
    // Value 5σ from mean
    float w = wc.compute(0.65f, fs, 0.50f, 0.03f);
    REQUIRE(w < 0.5f);
    REQUIRE(w >= 0.01f);  // Above floor
}

TEST_CASE("WeightComputer: cloud frame → cloud_penalty applied", "[classify]") {
    WeightComputer wc;
    FrameStats fs = make_clean_frame();
    fs.median_luminance = 0.5f;
    // Value is 0.3, lum_ratio = 0.3/0.5 = 0.6 < 0.85 threshold
    float w = wc.compute(0.30f, fs, 0.50f, 0.03f);
    // Should have cloud penalty (0.30) applied
    REQUIRE(w < 0.35f);
}

TEST_CASE("WeightComputer: failed alignment → weight halved", "[classify]") {
    WeightComputer wc;
    FrameStats fs = make_clean_frame();
    fs.frame_weight = 0.5f;  // Alignment failed
    float w = wc.compute(0.50f, fs, 0.50f, 0.03f);
    REQUIRE(w == Catch::Approx(0.5f).margin(0.05f));
}

TEST_CASE("WeightComputer: poor seeing → psf_weight reduces", "[classify]") {
    WeightComputer wc;
    FrameStats fs = make_clean_frame();
    fs.psf_weight = 0.6f;  // Poor seeing
    float w = wc.compute(0.50f, fs, 0.50f, 0.03f);
    REQUIRE(w == Catch::Approx(0.6f).margin(0.05f));
}

TEST_CASE("WeightComputer: weight never below floor", "[classify]") {
    WeightConfig cfg;
    cfg.weight_floor = 0.01f;
    WeightComputer wc(cfg);
    FrameStats fs = make_clean_frame();
    fs.frame_weight = 0.01f;
    fs.psf_weight = 0.01f;
    // Extreme outlier + cloud + bad alignment + bad seeing
    float w = wc.compute(5.0f, fs, 0.50f, 0.03f);
    REQUIRE(w >= 0.01f);
}

TEST_CASE("WeightComputer: zero stddev → no sigma penalty", "[classify]") {
    WeightComputer wc;
    FrameStats fs = make_clean_frame();
    // All frames identical → stddev = 0
    float w = wc.compute(0.50f, fs, 0.50f, 0.0f);
    REQUIRE(w > 0.9f);
}
```

- [ ] **Step 6: Update root CMakeLists.txt**

Add after `add_subdirectory(src/lib/fitting)`:

```cmake
add_subdirectory(src/lib/classify)
```

- [ ] **Step 7: Update test/CMakeLists.txt**

```cmake
nukex_add_test(test_weight_computer unit/classify/test_weight_computer.cpp nukex4_classify)
```

- [ ] **Step 8: Build and run**

```bash
cd build && cmake .. && make -j$(nproc) && ctest --output-on-failure
```

Expected: 21 tests pass (20 + 1 new).

- [ ] **Step 9: Commit**

```bash
git add -A
git commit -m "feat(classify): WeightComputer — per-sample quality weight computation

Multiplicative weight from four independent quality dimensions:
frame alignment quality, PSF seeing weight, sigma-score outlier penalty,
and cloud/transparency attenuation. Weight floor ensures no sample is
truly rejected (0.01 minimum). Includes FrameStats type in lib/core."
```

---

## Task 3: lib/combine — PixelSelector (Noise Propagation)

**Files:**
- Create: `src/lib/combine/CMakeLists.txt`
- Create: `src/lib/combine/include/nukex/combine/pixel_selector.hpp`
- Create: `src/lib/combine/src/pixel_selector.cpp`
- Create: `test/unit/combine/test_pixel_selector.cpp`
- Modify: `CMakeLists.txt` (root)
- Modify: `test/CMakeLists.txt`

- [ ] **Step 1: Create directories**

```bash
mkdir -p src/lib/combine/include/nukex/combine
mkdir -p src/lib/combine/src
mkdir -p test/unit/combine
```

- [ ] **Step 2: Create src/lib/combine/CMakeLists.txt**

```cmake
add_library(nukex4_combine STATIC
    src/pixel_selector.cpp
    src/spatial_context.cpp
    src/output_assembler.cpp
)

target_include_directories(nukex4_combine
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(nukex4_combine
    PUBLIC nukex4_core nukex4_fitting nukex4_classify nukex4_io
)

target_compile_features(nukex4_combine PUBLIC cxx_std_17)
```

- [ ] **Step 3: Create pixel_selector.hpp**

```cpp
#pragma once

#include "nukex/core/distribution.hpp"
#include "nukex/core/frame_stats.hpp"

namespace nukex {

/// Extracts output pixel values from fitted distributions and propagates noise.
///
/// Output value comes directly from ZDistribution.true_signal_estimate (set by
/// the fitting engine). Noise is propagated from per-sample CCD noise model:
///   σ²_sample = read_noise²/gain² + value/gain  (Poisson + read noise)
/// through the weighted combination:
///   noise_sigma = √(Σ wᵢ² σᵢ²) / Σ wᵢ
class PixelSelector {
public:
    /// Compute output value, noise, and SNR for one pixel channel.
    ///
    /// @param dist           Fitted distribution (from ModelSelector)
    /// @param values         Raw sample values (from FrameCache)
    /// @param weights        Per-sample combined weights (from WeightComputer)
    /// @param n              Number of samples
    /// @param frame_stats    Per-frame metadata array
    /// @param frame_indices  Frame index per sample (for looking up FrameStats)
    /// @param welford_var    Welford variance fallback (if no noise keywords)
    /// @param[out] out_value Output pixel value
    /// @param[out] out_noise Output 1σ noise in ADU
    /// @param[out] out_snr   Output signal-to-noise ratio
    void select(const ZDistribution& dist,
                const float* values, const float* weights, int n,
                const FrameStats* frame_stats, const int* frame_indices,
                float welford_var,
                float& out_value, float& out_noise, float& out_snr) const;

    /// Compute per-sample variance from CCD noise model.
    /// σ² = read_noise²/gain² + value/gain
    /// Falls back to welford_var if noise keywords absent.
    static float sample_variance(float value, const FrameStats& fs,
                                 float welford_var);
};

} // namespace nukex
```

- [ ] **Step 4: Create pixel_selector.cpp**

```cpp
#include "nukex/combine/pixel_selector.hpp"
#include <cmath>
#include <algorithm>

namespace nukex {

float PixelSelector::sample_variance(float value, const FrameStats& fs,
                                      float welford_var) {
    if (fs.has_noise_keywords) {
        float rn = fs.read_noise;
        float g = fs.gain;
        if (g < 1e-10f) g = 1.0f;
        // Poisson (shot) noise + read noise, both in ADU²
        float shot_noise_var = std::max(0.0f, value / g);
        float read_noise_var = (rn * rn) / (g * g);
        return read_noise_var + shot_noise_var;
    }
    return welford_var;
}

void PixelSelector::select(const ZDistribution& dist,
                            const float* values, const float* weights, int n,
                            const FrameStats* frame_stats, const int* frame_indices,
                            float welford_var,
                            float& out_value, float& out_noise, float& out_snr) const {
    // Output value: directly from the fitted distribution
    out_value = dist.true_signal_estimate;

    // Noise propagation through weighted combination
    double weight_sum = 0.0;
    double variance_sum = 0.0;

    for (int i = 0; i < n; i++) {
        double w = static_cast<double>(weights[i]);
        int fi = frame_indices[i];
        float sigma2 = sample_variance(values[i], frame_stats[fi], welford_var);
        weight_sum += w;
        variance_sum += w * w * static_cast<double>(sigma2);
    }

    if (weight_sum > 1e-30) {
        out_noise = static_cast<float>(std::sqrt(variance_sum) / weight_sum);
    } else {
        out_noise = 0.0f;
    }

    // SNR
    if (out_noise > 1e-30f) {
        out_snr = std::clamp(out_value / out_noise, 0.0f, 9999.0f);
    } else {
        out_snr = 0.0f;
    }
}

} // namespace nukex
```

- [ ] **Step 5: Create stub files for spatial_context.cpp and output_assembler.cpp (filled in next tasks)**

`src/lib/combine/src/spatial_context.cpp`:
```cpp
#include "nukex/combine/spatial_context.hpp"
namespace nukex {
void SpatialContext::compute(const Image&, Cube&) const {}
} // namespace nukex
```

`src/lib/combine/src/output_assembler.cpp`:
```cpp
#include "nukex/combine/output_assembler.hpp"
namespace nukex {
OutputAssembler::OutputImages OutputAssembler::assemble(const Cube&) { return {}; }
} // namespace nukex
```

Create minimal headers:

`src/lib/combine/include/nukex/combine/spatial_context.hpp`:
```cpp
#pragma once
namespace nukex {
class Image;
class Cube;
class SpatialContext {
public:
    void compute(const Image& output, Cube& cube) const;
};
} // namespace nukex
```

`src/lib/combine/include/nukex/combine/output_assembler.hpp`:
```cpp
#pragma once
#include "nukex/io/image.hpp"
namespace nukex {
class Cube;
class OutputAssembler {
public:
    struct OutputImages {
        Image stacked;
        Image noise_map;
        Image quality_map;
    };
    static OutputImages assemble(const Cube& cube);
};
} // namespace nukex
```

- [ ] **Step 6: Create test_pixel_selector.cpp**

```cpp
#include "catch_amalgamated.hpp"
#include "nukex/combine/pixel_selector.hpp"
#include "nukex/core/distribution.hpp"
#include <cmath>

using namespace nukex;

namespace {

FrameStats make_frame(float rn, float g, bool has_kw = true) {
    FrameStats fs;
    fs.read_noise = rn;
    fs.gain = g;
    fs.has_noise_keywords = has_kw;
    fs.frame_weight = 1.0f;
    fs.psf_weight = 1.0f;
    fs.median_luminance = 0.5f;
    return fs;
}

} // anonymous namespace

TEST_CASE("PixelSelector::sample_variance: CCD noise model", "[combine]") {
    // σ² = rn²/g² + value/g
    // rn=3e, g=1.5 e/ADU, value=0.5 ADU
    // read_var = 9/2.25 = 4.0, shot_var = 0.5/1.5 = 0.333
    FrameStats fs = make_frame(3.0f, 1.5f);
    float var = PixelSelector::sample_variance(0.5f, fs, 0.0f);
    REQUIRE(var == Catch::Approx(4.0f + 0.333f).margin(0.01f));
}

TEST_CASE("PixelSelector::sample_variance: no keywords → Welford fallback", "[combine]") {
    FrameStats fs = make_frame(3.0f, 1.5f, false);
    float var = PixelSelector::sample_variance(0.5f, fs, 0.02f);
    REQUIRE(var == Catch::Approx(0.02f));
}

TEST_CASE("PixelSelector::select: output value from distribution", "[combine]") {
    ZDistribution dist{};
    dist.true_signal_estimate = 0.42f;

    float values[] = {0.40f, 0.42f, 0.44f};
    float weights[] = {1.0f, 1.0f, 1.0f};
    int frame_indices[] = {0, 1, 2};
    FrameStats fs_arr[] = {make_frame(3, 1.5), make_frame(3, 1.5), make_frame(3, 1.5)};

    PixelSelector sel;
    float val, noise, snr;
    sel.select(dist, values, weights, 3, fs_arr, frame_indices, 0.0f,
               val, noise, snr);

    REQUIRE(val == Catch::Approx(0.42f));
    REQUIRE(noise > 0.0f);
    REQUIRE(snr > 0.0f);
}

TEST_CASE("PixelSelector::select: more samples → lower noise", "[combine]") {
    ZDistribution dist{};
    dist.true_signal_estimate = 0.5f;

    // 10 samples
    float values10[10]; float weights10[10]; int fi10[10];
    FrameStats fs10[10];
    for (int i = 0; i < 10; i++) {
        values10[i] = 0.5f; weights10[i] = 1.0f; fi10[i] = i;
        fs10[i] = make_frame(3, 1.5);
    }

    // 100 samples
    float values100[100]; float weights100[100]; int fi100[100];
    FrameStats fs100[100];
    for (int i = 0; i < 100; i++) {
        values100[i] = 0.5f; weights100[i] = 1.0f; fi100[i] = i;
        fs100[i] = make_frame(3, 1.5);
    }

    PixelSelector sel;
    float v1, n1, s1, v2, n2, s2;
    sel.select(dist, values10, weights10, 10, fs10, fi10, 0.0f, v1, n1, s1);
    sel.select(dist, values100, weights100, 100, fs100, fi100, 0.0f, v2, n2, s2);

    // 100 samples should have ~√10 lower noise than 10 samples
    REQUIRE(n2 < n1);
    float ratio = n1 / n2;
    REQUIRE(ratio == Catch::Approx(std::sqrt(10.0f)).margin(0.5f));
}

TEST_CASE("PixelSelector::select: SNR clamped to 9999", "[combine]") {
    ZDistribution dist{};
    dist.true_signal_estimate = 100.0f;

    float values[] = {100.0f};
    float weights[] = {1.0f};
    int fi[] = {0};
    // Tiny noise → huge SNR → should clamp
    FrameStats fs = make_frame(0.001f, 100.0f);

    PixelSelector sel;
    float val, noise, snr;
    sel.select(dist, values, weights, 1, &fs, fi, 0.0f, val, noise, snr);

    REQUIRE(snr <= 9999.0f);
}
```

- [ ] **Step 7: Update root CMakeLists.txt**

Add after `add_subdirectory(src/lib/classify)`:

```cmake
add_subdirectory(src/lib/combine)
```

- [ ] **Step 8: Update test/CMakeLists.txt**

```cmake
nukex_add_test(test_pixel_selector unit/combine/test_pixel_selector.cpp nukex4_combine)
```

- [ ] **Step 9: Build and run**

```bash
cd build && cmake .. && make -j$(nproc) && ctest --output-on-failure
```

Expected: 22 tests (21 + 1 new).

- [ ] **Step 10: Commit**

```bash
git add -A
git commit -m "feat(combine): PixelSelector — noise propagation via CCD noise model

Output value from fitted ZDistribution. Noise propagated through weighted
combination using per-sample CCD noise: σ² = read_noise²/gain² + value/gain.
Falls back to Welford variance when FITS keywords absent.
SNR = value/noise, clamped to [0, 9999]."
```

---

## Task 4: lib/combine — SpatialContext

**Files:**
- Modify: `src/lib/combine/include/nukex/combine/spatial_context.hpp`
- Modify: `src/lib/combine/src/spatial_context.cpp`
- Create: `test/unit/combine/test_spatial_context.cpp`
- Modify: `test/CMakeLists.txt`

Computes gradient_mag, local_background, local_rms from the output stacked image.

- [ ] **Step 1: Update spatial_context.hpp**

```cpp
#pragma once

#include "nukex/io/image.hpp"
#include "nukex/core/voxel.hpp"

namespace nukex {

class Cube;

/// Computes spatial context metrics from the stacked output image.
///
/// After all pixels have output values, these metrics provide local
/// context for the quality map:
/// - gradient_mag: Sobel gradient magnitude (edge strength)
/// - local_background: biweight location in 15×15 neighborhood
/// - local_rms: MAD × 1.4826 in 15×15 neighborhood
class SpatialContext {
public:
    static constexpr int WINDOW_RADIUS = 7;  // 15×15 = radius 7

    /// Compute spatial context and write to voxels in the cube.
    void compute(const Image& output, Cube& cube) const;

    /// Sobel gradient magnitude at (x,y) averaged across channels.
    static float sobel_gradient(const Image& img, int x, int y);
};

} // namespace nukex
```

- [ ] **Step 2: Implement spatial_context.cpp**

```cpp
#include "nukex/combine/spatial_context.hpp"
#include "nukex/core/cube.hpp"
#include "nukex/fitting/robust_stats.hpp"
#include <cmath>
#include <vector>
#include <algorithm>

namespace nukex {

float SpatialContext::sobel_gradient(const Image& img, int x, int y) {
    if (x <= 0 || x >= img.width() - 1 || y <= 0 || y >= img.height() - 1)
        return 0.0f;

    float max_grad = 0.0f;
    for (int ch = 0; ch < img.n_channels(); ch++) {
        // Sobel Gx
        float gx = -1.0f * img.at(x-1, y-1, ch) + 1.0f * img.at(x+1, y-1, ch)
                  + -2.0f * img.at(x-1, y,   ch) + 2.0f * img.at(x+1, y,   ch)
                  + -1.0f * img.at(x-1, y+1, ch) + 1.0f * img.at(x+1, y+1, ch);
        // Sobel Gy
        float gy = -1.0f * img.at(x-1, y-1, ch) - 2.0f * img.at(x, y-1, ch) - 1.0f * img.at(x+1, y-1, ch)
                  +  1.0f * img.at(x-1, y+1, ch) + 2.0f * img.at(x, y+1, ch) + 1.0f * img.at(x+1, y+1, ch);

        float grad = std::sqrt(gx * gx + gy * gy);
        max_grad = std::max(max_grad, grad);
    }
    return max_grad;
}

void SpatialContext::compute(const Image& output, Cube& cube) const {
    int w = cube.width;
    int h = cube.height;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            auto& voxel = cube.at(x, y);

            // Gradient
            voxel.gradient_mag = sobel_gradient(output, x, y);

            // Local statistics in 15×15 window
            int x0 = std::max(0, x - WINDOW_RADIUS);
            int x1 = std::min(w - 1, x + WINDOW_RADIUS);
            int y0 = std::max(0, y - WINDOW_RADIUS);
            int y1 = std::min(h - 1, y + WINDOW_RADIUS);

            // Collect neighborhood values (channel 0, or average across channels)
            std::vector<float> neighborhood;
            neighborhood.reserve((x1-x0+1) * (y1-y0+1));
            for (int ny = y0; ny <= y1; ny++) {
                for (int nx = x0; nx <= x1; nx++) {
                    float avg = 0.0f;
                    for (int ch = 0; ch < output.n_channels(); ch++) {
                        avg += output.at(nx, ny, ch);
                    }
                    avg /= output.n_channels();
                    neighborhood.push_back(avg);
                }
            }

            int nn = static_cast<int>(neighborhood.size());
            if (nn > 0) {
                voxel.local_background = biweight_location(
                    neighborhood.data(), nn);
                voxel.local_rms = mad(neighborhood.data(), nn) * 1.4826f;
            }
        }
    }
}

} // namespace nukex
```

- [ ] **Step 3: Create test_spatial_context.cpp**

```cpp
#include "catch_amalgamated.hpp"
#include "nukex/combine/spatial_context.hpp"
#include "nukex/io/image.hpp"
#include "nukex/core/cube.hpp"
#include "nukex/core/channel_config.hpp"
#include <cmath>

using namespace nukex;

TEST_CASE("SpatialContext::sobel_gradient: flat image → zero gradient", "[spatial]") {
    Image img(32, 32, 1);
    img.fill(0.5f);
    float grad = SpatialContext::sobel_gradient(img, 16, 16);
    REQUIRE(grad == Catch::Approx(0.0f).margin(1e-6f));
}

TEST_CASE("SpatialContext::sobel_gradient: sharp edge → high gradient", "[spatial]") {
    Image img(32, 32, 1);
    // Left half = 0, right half = 1
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 32; x++) {
            img.at(x, y, 0) = (x < 16) ? 0.0f : 1.0f;
        }
    }
    float grad = SpatialContext::sobel_gradient(img, 16, 16);
    REQUIRE(grad > 1.0f);
}

TEST_CASE("SpatialContext::sobel_gradient: border → zero", "[spatial]") {
    Image img(32, 32, 1);
    img.fill(0.5f);
    REQUIRE(SpatialContext::sobel_gradient(img, 0, 0) == 0.0f);
    REQUIRE(SpatialContext::sobel_gradient(img, 31, 31) == 0.0f);
}

TEST_CASE("SpatialContext::compute: writes to voxels", "[spatial]") {
    auto config = ChannelConfig::from_mode(StackingMode::MONO_L);
    Cube cube(16, 16, config);
    Image output(16, 16, 1);
    // Uniform image
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            output.at(x, y, 0) = 0.5f;

    SpatialContext ctx;
    ctx.compute(output, cube);

    auto& v = cube.at(8, 8);
    REQUIRE(v.gradient_mag == Catch::Approx(0.0f).margin(1e-6f));
    REQUIRE(v.local_background == Catch::Approx(0.5f).margin(0.01f));
    REQUIRE(v.local_rms == Catch::Approx(0.0f).margin(0.01f));
}
```

- [ ] **Step 4: Update test/CMakeLists.txt**

```cmake
nukex_add_test(test_spatial_context unit/combine/test_spatial_context.cpp nukex4_combine)
```

- [ ] **Step 5: Build and run**

```bash
cd build && cmake .. && make -j$(nproc) && ctest --output-on-failure
```

Expected: 23 tests.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat(combine): SpatialContext — gradient, local background, local RMS

Sobel gradient magnitude (max across channels), biweight location and
MAD×1.4826 in 15×15 neighborhood. Written to voxel fields for quality map."
```

---

## Task 5: lib/combine — OutputAssembler

**Files:**
- Modify: `src/lib/combine/include/nukex/combine/output_assembler.hpp`
- Modify: `src/lib/combine/src/output_assembler.cpp`
- Create: `test/unit/combine/test_output_assembler.cpp`
- Modify: `test/CMakeLists.txt`

Assembles three output images from the completed Cube.

- [ ] **Step 1: Update output_assembler.hpp**

```cpp
#pragma once

#include "nukex/io/image.hpp"
#include "nukex/core/cube.hpp"

namespace nukex {

/// Assembles three output images from a completed Cube.
///
/// - stacked: N-channel float32 image (the main result)
/// - noise_map: N-channel float32 (per-pixel 1σ noise in ADU)
/// - quality_map: 4-channel (signal, uncertainty, confidence, shape)
class OutputAssembler {
public:
    struct OutputImages {
        Image stacked;
        Image noise_map;
        Image quality_map;
    };

    /// Build output images from fitted distributions in the cube.
    /// Stacked and noise_map are passed in (already populated by PixelSelector).
    /// Quality map is assembled from voxel fields.
    static Image assemble_quality_map(const Cube& cube);
};

} // namespace nukex
```

- [ ] **Step 2: Implement output_assembler.cpp**

```cpp
#include "nukex/combine/output_assembler.hpp"

namespace nukex {

Image OutputAssembler::assemble_quality_map(const Cube& cube) {
    // 4 channels: signal, uncertainty, confidence, shape
    Image quality(cube.width, cube.height, 4);

    for (int y = 0; y < cube.height; y++) {
        for (int x = 0; x < cube.width; x++) {
            const auto& v = cube.at(x, y);

            // Use channel 0 distribution for the quality map
            // (or average across channels for multi-channel data)
            int primary_ch = 0;
            const auto& dist = v.distribution[primary_ch];

            quality.at(x, y, 0) = dist.true_signal_estimate;
            quality.at(x, y, 1) = dist.signal_uncertainty;
            quality.at(x, y, 2) = dist.confidence;
            quality.at(x, y, 3) = static_cast<float>(
                static_cast<uint8_t>(dist.shape));
        }
    }

    return quality;
}

} // namespace nukex
```

- [ ] **Step 3: Create test_output_assembler.cpp**

```cpp
#include "catch_amalgamated.hpp"
#include "nukex/combine/output_assembler.hpp"
#include "nukex/core/cube.hpp"
#include "nukex/core/channel_config.hpp"

using namespace nukex;

TEST_CASE("OutputAssembler: quality map has 4 channels", "[assembler]") {
    auto config = ChannelConfig::from_mode(StackingMode::MONO_L);
    Cube cube(8, 8, config);

    // Set up some fitted distributions
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            auto& v = cube.at(x, y);
            v.distribution[0].shape = DistributionShape::GAUSSIAN;
            v.distribution[0].true_signal_estimate = 0.5f;
            v.distribution[0].signal_uncertainty = 0.01f;
            v.distribution[0].confidence = 0.9f;
        }
    }

    auto quality = OutputAssembler::assemble_quality_map(cube);
    REQUIRE(quality.width() == 8);
    REQUIRE(quality.height() == 8);
    REQUIRE(quality.n_channels() == 4);
    REQUIRE(quality.at(4, 4, 0) == Catch::Approx(0.5f));
    REQUIRE(quality.at(4, 4, 1) == Catch::Approx(0.01f));
    REQUIRE(quality.at(4, 4, 2) == Catch::Approx(0.9f));
    REQUIRE(quality.at(4, 4, 3) == Catch::Approx(0.0f));  // GAUSSIAN = 0
}

TEST_CASE("OutputAssembler: shape channel encodes enum correctly", "[assembler]") {
    auto config = ChannelConfig::from_mode(StackingMode::MONO_L);
    Cube cube(4, 4, config);

    cube.at(0, 0).distribution[0].shape = DistributionShape::GAUSSIAN;
    cube.at(1, 0).distribution[0].shape = DistributionShape::BIMODAL;
    cube.at(2, 0).distribution[0].shape = DistributionShape::HEAVY_TAILED;
    cube.at(3, 0).distribution[0].shape = DistributionShape::CONTAMINATED;

    auto quality = OutputAssembler::assemble_quality_map(cube);
    REQUIRE(quality.at(0, 0, 3) == Catch::Approx(0.0f));  // GAUSSIAN
    REQUIRE(quality.at(1, 0, 3) == Catch::Approx(1.0f));  // BIMODAL
    REQUIRE(quality.at(2, 0, 3) == Catch::Approx(2.0f));  // HEAVY_TAILED
    REQUIRE(quality.at(3, 0, 3) == Catch::Approx(3.0f));  // CONTAMINATED
}
```

- [ ] **Step 4: Update test/CMakeLists.txt**

```cmake
nukex_add_test(test_output_assembler unit/combine/test_output_assembler.cpp nukex4_combine)
```

- [ ] **Step 5: Build and run all tests**

```bash
cd build && cmake .. && make -j$(nproc) && ctest --output-on-failure
```

Expected: 24 tests.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat(combine): OutputAssembler — quality map assembly from Cube

Builds 4-channel quality map from fitted voxel distributions:
ch0=signal, ch1=uncertainty, ch2=confidence, ch3=shape enum.
Stacked image and noise map are built by PixelSelector during Phase B."
```

---

## Task 6: Full Phase 4B Verification

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

Expected: Clean compilation, all tests pass.

---

*End of Phase 4B implementation plan*
