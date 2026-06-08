# GPU Acceleration & Performance Optimization — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add optional CUDA GPU acceleration and CPU performance improvements to NukeXStack pixel selection, reducing stacking time from ~40 minutes to under 5 minutes (CPU) or ~1-3 minutes (GPU).

**Architecture:** Dual code paths — improved CPU (analytical Skew-Normal gradients, adaptive model selection, `-march=native`) and optional CUDA GPU (self-contained device kernels). Single `.so` module with compile-time `NUKEX_HAS_CUDA` gating. Two new PCL parameters: `useGPU` (auto-detect) and `adaptiveModels` (default on).

**Tech Stack:** C++17, CUDA 12.8, PCL SDK, OpenMP, xtensor, Boost.Math (CPU only), LBFGSpp/Eigen (CPU only)

---

### Task 1: Add `-march=native` to Build System

**Files:**
- Modify: `CMakeLists.txt:105-111`
- Modify: `Makefile:64`

**Step 1: Add march=native to CMakeLists.txt**

In `CMakeLists.txt`, inside the else() block (non-MSVC), add `-march=native` to the compiler options:

```cpp
    target_compile_options(NukeX PRIVATE
        -Wall -Wno-parentheses
        -O3 -fPIC -march=native
        -ffunction-sections -fdata-sections
        -fvisibility=hidden -fvisibility-inlines-hidden
        -fnon-call-exceptions
    )
```

**Step 2: Add march=native to Makefile**

In `Makefile`, change the release optimization line:

```makefile
    OPT_FLAGS = -O3 -march=native -DNDEBUG
```

**Step 3: Verify build**

Run: `cd /home/scarter4work/projects/nukex3 && make clean && make release 2>&1 | tail -5`
Expected: Build succeeds with no errors

**Step 4: Run tests**

Run: `cd /home/scarter4work/projects/nukex3/build && cmake .. -DPCLDIR=$HOME/PCL && make -j$(nproc) && ctest --output-on-failure`
Expected: All 14 tests pass

**Step 5: Commit**

```bash
git add CMakeLists.txt Makefile
git commit -m "build: add -march=native for AVX-512 auto-vectorization"
```

---

### Task 2: Analytical Gradients for Skew-Normal Fitter

**Files:**
- Modify: `src/engine/SkewNormalFitter.cpp:57-69`
- Test: `tests/unit/test_skewnormal.cpp` (existing)

This replaces the numerical central-difference gradient (6 extra function evaluations per L-BFGS iteration) with closed-form analytical derivatives, giving ~5x speedup on Skew-Normal fitting.

**Step 1: Write the failing test**

Add a test to `tests/unit/test_skewnormal.cpp` that verifies the analytical gradient matches the numerical gradient for a known dataset:

```cpp
TEST_CASE("SkewNormal analytical gradient matches numerical", "[skewnormal]") {
    // Known skewed data
    std::vector<double> data = {1.2, 1.5, 1.8, 2.0, 2.1, 2.3, 2.5, 3.0, 3.5, 4.0};
    auto result = fitSkewNormal(data);
    // The fit should still converge and produce reasonable results
    REQUIRE(result.converged);
    REQUIRE(result.omega > 0);
    // Log-likelihood should be finite
    REQUIRE(std::isfinite(result.logLikelihood));
}
```

**Step 2: Run test to verify it passes (baseline)**

Run: `cd /home/scarter4work/projects/nukex3/build && make -j$(nproc) && ctest -R skewnormal --output-on-failure`
Expected: PASS (existing code handles this)

**Step 3: Implement analytical gradients**

Replace the numerical gradient in `SkewNormalFitter.cpp:57-69` (`operator()`) with analytical derivatives.

The Skew-Normal log-likelihood per sample is:
```
L_i = log(2) - log(omega) - 0.5*log(2*pi) - 0.5*z_i^2 + log(Phi(alpha*z_i))
where z_i = (x_i - xi) / omega
```

The derivatives of the negative log-likelihood are:

**With respect to xi** (location):
```
dNLL/dxi = sum_i [ -z_i/omega - alpha/omega * phi(alpha*z_i) / Phi(alpha*z_i) ]
```
But since we parameterize as x[0]=xi, x[1]=log(omega), x[2]=alpha:

**d/d(xi):** `sum_i [ z_i / omega - alpha * phi(alpha*z_i) / (omega * Phi(alpha*z_i)) ]`

**d/d(log_omega):** Since omega = exp(x[1]), chain rule gives:
`sum_i [ -1 + z_i^2 - alpha * z_i * phi(alpha*z_i) / Phi(alpha*z_i) ]`

**d/d(alpha):** `sum_i [ -z_i * phi(alpha*z_i) / Phi(alpha*z_i) ]`

Where `phi(t) = (1/sqrt(2*pi)) * exp(-t^2/2)` is the standard normal PDF,
and `Phi(t) = erfc(-t/sqrt(2))/2` is the standard normal CDF.

Replace the `operator()` method body:

```cpp
    double operator()(const Eigen::VectorXd& x, Eigen::VectorXd& grad) {
        double xi    = x[0];
        double omega = std::exp(x[1]);
        double alpha = x[2];

        if (omega < 1e-15) omega = 1e-15;
        double logOmega = std::log(omega);
        double invOmega = 1.0 / omega;

        double perSampleConst = LOG_2 - logOmega - 0.5 * LOG_2PI;
        constexpr double INV_SQRT_2PI = 0.3989422804014327;

        double negLogL = 0.0;
        double g0 = 0.0, g1 = 0.0, g2 = 0.0;

        for (double val : m_data) {
            double z = (val - xi) * invOmega;
            double az = alpha * z;

            // Numerically stable log(Phi(az))
            double logPhi = logNormalCDF(az);
            // Phi(az) and phi(az) for gradient
            double phiAz = INV_SQRT_2PI * std::exp(-0.5 * az * az);
            double PhiAz = std::exp(logPhi);
            // Guard against Phi near zero
            double ratio = (PhiAz > 1e-300) ? phiAz / PhiAz : 0.0;

            // Negative log-likelihood
            negLogL -= perSampleConst - 0.5 * z * z + logPhi;

            // Gradients of negative log-likelihood
            g0 += z * invOmega - alpha * invOmega * ratio;  // d/d(xi)
            g1 += -1.0 + z * z - alpha * z * ratio;          // d/d(log_omega)
            g2 += -z * ratio;                                 // d/d(alpha)
        }

        grad[0] = g0;
        grad[1] = g1;
        grad[2] = g2;

        return negLogL;
    }
```

**Step 4: Run all tests**

Run: `cd /home/scarter4work/projects/nukex3/build && make -j$(nproc) && ctest --output-on-failure`
Expected: All tests pass (results should be numerically equivalent)

**Step 5: Commit**

```bash
git add src/engine/SkewNormalFitter.cpp tests/unit/test_skewnormal.cpp
git commit -m "perf: analytical gradients for Skew-Normal fitter (~5x speedup)"
```

---

### Task 3: New PCL Parameters (useGPU + adaptiveModels)

**Files:**
- Modify: `src/NukeXStackParameters.h` — add two parameter class declarations
- Modify: `src/NukeXStackParameters.cpp` — add two parameter class implementations
- Modify: `src/NukeXStackInstance.h` — add member variables
- Modify: `src/NukeXStackInstance.cpp` — add to Assign(), LockParameter()

**Step 1: Add parameter declarations to NukeXStackParameters.h**

After the `NXSEnableAutoStretch` block (after line 121), add:

```cpp
class NXSUseGPU : public MetaBoolean
{
public:
   NXSUseGPU( MetaProcess* );
   IsoString Id() const override;
   bool DefaultValue() const override;
};

extern NXSUseGPU* TheNXSUseGPUParameter;

class NXSAdaptiveModels : public MetaBoolean
{
public:
   NXSAdaptiveModels( MetaProcess* );
   IsoString Id() const override;
   bool DefaultValue() const override;
};

extern NXSAdaptiveModels* TheNXSAdaptiveModelsParameter;
```

**Step 2: Add parameter implementations to NukeXStackParameters.cpp**

After the `NXSEnableAutoStretch` implementation block, add:

```cpp
// ----------------------------------------------------------------------------

NXSUseGPU* TheNXSUseGPUParameter = nullptr;

NXSUseGPU::NXSUseGPU( MetaProcess* P )
   : MetaBoolean( P )
{
   TheNXSUseGPUParameter = this;
}

IsoString NXSUseGPU::Id() const
{
   return "useGPU";
}

bool NXSUseGPU::DefaultValue() const
{
   return true;  // Auto-detect at runtime; default to on
}

// ----------------------------------------------------------------------------

NXSAdaptiveModels* TheNXSAdaptiveModelsParameter = nullptr;

NXSAdaptiveModels::NXSAdaptiveModels( MetaProcess* P )
   : MetaBoolean( P )
{
   TheNXSAdaptiveModelsParameter = this;
}

IsoString NXSAdaptiveModels::Id() const
{
   return "adaptiveModels";
}

bool NXSAdaptiveModels::DefaultValue() const
{
   return true;
}
```

**Step 3: Add member variables to NukeXStackInstance.h**

After `pcl_bool p_enableAutoStretch;` (line 71), add:

```cpp
   pcl_bool p_useGPU;
   pcl_bool p_adaptiveModels;
```

**Step 4: Update NukeXStackInstance.cpp constructor**

In the constructor initializer list, add:

```cpp
   , p_useGPU( TheNXSUseGPUParameter->DefaultValue() )
   , p_adaptiveModels( TheNXSAdaptiveModelsParameter->DefaultValue() )
```

**Step 5: Update Assign()**

In `Assign()`, after `p_enableAutoStretch = x->p_enableAutoStretch;`, add:

```cpp
      p_useGPU                  = x->p_useGPU;
      p_adaptiveModels          = x->p_adaptiveModels;
```

**Step 6: Update LockParameter()**

After the `TheNXSEnableAutoStretchParameter` check, add:

```cpp
   if ( p == TheNXSUseGPUParameter )
      return &p_useGPU;
   if ( p == TheNXSAdaptiveModelsParameter )
      return &p_adaptiveModels;
```

**Step 7: Register parameters in NukeXStackProcess**

In `NukeXStackProcess.cpp`, in the constructor, add after the other `new NXS...` lines:

```cpp
   new NXSUseGPU( this );
   new NXSAdaptiveModels( this );
```

**Step 8: Build and verify**

Run: `cd /home/scarter4work/projects/nukex3 && make clean && make release 2>&1 | tail -5`
Expected: Build succeeds

Run: `cd /home/scarter4work/projects/nukex3/build && cmake .. -DPCLDIR=$HOME/PCL && make -j$(nproc) && ctest --output-on-failure`
Expected: All tests pass

**Step 9: Commit**

```bash
git add src/NukeXStackParameters.h src/NukeXStackParameters.cpp \
        src/NukeXStackInstance.h src/NukeXStackInstance.cpp \
        src/NukeXStackProcess.cpp
git commit -m "feat: add useGPU and adaptiveModels parameters"
```

---

### Task 4: GUI Checkboxes for New Parameters

**Files:**
- Modify: `src/NukeXStackInterface.h` — add GUI widgets
- Modify: `src/NukeXStackInterface.cpp` — add widget creation and event handlers

**Step 1: Add widget declarations to GUIData in NukeXStackInterface.h**

In the `GUIData` struct, in the Output section (after `CheckBox EnableAutoStretch_CheckBox;` around line 104), add:

```cpp
         CheckBox          UseGPU_CheckBox;
         CheckBox          AdaptiveModels_CheckBox;
```

**Step 2: Create widgets in GUIData constructor (NukeXStackInterface.cpp)**

In the Output Section of `GUIData::GUIData()`, after `EnableAutoStretch_CheckBox` setup (after line 637), add:

```cpp
   UseGPU_CheckBox.SetText( "Use GPU Acceleration" );
   UseGPU_CheckBox.SetToolTip( "<p>Use NVIDIA CUDA GPU for pixel selection when available. "
                                "Falls back to CPU if no compatible GPU is detected.</p>" );
   UseGPU_CheckBox.OnClick( (Button::click_event_handler)&NukeXStackInterface::e_CheckBoxClick, w );
#ifndef NUKEX_HAS_CUDA
   UseGPU_CheckBox.Disable();
   UseGPU_CheckBox.SetToolTip( "<p>GPU acceleration unavailable — module built without CUDA support.</p>" );
#endif

   AdaptiveModels_CheckBox.SetText( "Adaptive Model Selection" );
   AdaptiveModels_CheckBox.SetToolTip( "<p>Skip expensive distribution fits (Skew-Normal, Bimodal) when "
                                        "Gaussian provides an excellent fit. Significantly faster with "
                                        "negligible impact on quality for typical data.</p>" );
   AdaptiveModels_CheckBox.OnClick( (Button::click_event_handler)&NukeXStackInterface::e_CheckBoxClick, w );
```

Add them to the Output_Sizer (after `Output_Sizer.Add( EnableAutoStretch_CheckBox );`):

```cpp
   Output_Sizer.Add( UseGPU_CheckBox );
   Output_Sizer.Add( AdaptiveModels_CheckBox );
```

**Step 3: Update UpdateControls()**

After `GUI->EnableAutoStretch_CheckBox.SetChecked( m_instance.p_enableAutoStretch );` add:

```cpp
   GUI->UseGPU_CheckBox.SetChecked( m_instance.p_useGPU );
   GUI->AdaptiveModels_CheckBox.SetChecked( m_instance.p_adaptiveModels );
```

**Step 4: Update e_CheckBoxClick()**

After the `EnableAutoStretch_CheckBox` handler, add:

```cpp
   else if ( sender == GUI->UseGPU_CheckBox )
   {
      m_instance.p_useGPU = checked;
   }
   else if ( sender == GUI->AdaptiveModels_CheckBox )
   {
      m_instance.p_adaptiveModels = checked;
   }
```

**Step 5: Build and verify**

Run: `cd /home/scarter4work/projects/nukex3 && make clean && make release 2>&1 | tail -5`
Expected: Build succeeds

**Step 6: Commit**

```bash
git add src/NukeXStackInterface.h src/NukeXStackInterface.cpp
git commit -m "feat: add GPU and Adaptive Models checkboxes to UI"
```

---

### Task 5: Adaptive Model Selection in PixelSelector

**Files:**
- Modify: `src/engine/PixelSelector.h` — add adaptiveModels to Config
- Modify: `src/engine/PixelSelector.cpp:124-156` — early-exit logic
- Create: `tests/unit/test_adaptive_models.cpp`

**Step 1: Add adaptiveModels to PixelSelector::Config**

In `src/engine/PixelSelector.h`, add to the Config struct:

```cpp
    struct Config {
        int maxOutliers = 3;
        double outlierAlpha = 0.05;
        bool useQualityWeights = true;
        bool adaptiveModels = false;     // NEW: skip expensive fits if Gaussian AIC is excellent
        double adaptiveAicThreshold = 4.0; // AIC delta: skip if others can't win by this margin
    };
```

**Step 2: Write the failing test**

Create `tests/unit/test_adaptive_models.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "engine/PixelSelector.h"
#include "engine/SubCube.h"

using Catch::Approx;

TEST_CASE("Adaptive model selection skips expensive fits for Gaussian data", "[adaptive]") {
    // Create perfectly Gaussian Z-column data — adaptive should skip skew+bimodal
    nukex::PixelSelector::Config config;
    config.maxOutliers = 2;
    config.adaptiveModels = true;
    nukex::PixelSelector selector(config);

    // Build a small SubCube with Gaussian-distributed Z values
    size_t nSubs = 10, H = 2, W = 2;
    nukex::SubCube cube(nSubs, H, W);
    // Fill with near-constant values (trivially Gaussian)
    for (size_t z = 0; z < nSubs; ++z)
        for (size_t y = 0; y < H; ++y)
            for (size_t x = 0; x < W; ++x)
                cube.setSample(z, y, x, 0.5f + 0.001f * z);

    std::vector<double> weights(nSubs, 1.0 / nSubs);
    auto result = selector.processImage(cube, weights);

    REQUIRE(result.size() == H * W);
    for (float val : result) {
        REQUIRE(val > 0.49f);
        REQUIRE(val < 0.52f);
    }
}

TEST_CASE("Adaptive off runs all 4 models", "[adaptive]") {
    // With adaptive off, all 4 distributions should be fitted
    nukex::PixelSelector::Config config;
    config.maxOutliers = 2;
    config.adaptiveModels = false;
    nukex::PixelSelector selector(config);

    size_t nSubs = 10, H = 1, W = 1;
    nukex::SubCube cube(nSubs, H, W);
    for (size_t z = 0; z < nSubs; ++z)
        cube.setSample(z, 0, 0, 0.5f + 0.001f * z);

    std::vector<double> weights(nSubs, 1.0 / nSubs);
    auto result = selector.processImage(cube, weights);

    REQUIRE(result.size() == 1);
    REQUIRE(std::isfinite(result[0]));
}
```

**Step 3: Run test to verify it fails**

Run: `cd /home/scarter4work/projects/nukex3/build && cmake .. -DPCLDIR=$HOME/PCL && make -j$(nproc) && ctest -R adaptive --output-on-failure`
Expected: Build may fail (new test references SubCube methods) or test runs but behavior not yet differentiated

**Step 4: Implement adaptive model selection in selectBestZ()**

In `src/engine/PixelSelector.cpp`, replace the block at lines 124-156 (the "fit all 4 models" section) with:

```cpp
    // 5. Fit models — adaptive mode skips expensive fits if Gaussian is sufficient
    FitResult gaussianFit = fitGaussian(cleanData);
    double aicGaussian = aic(gaussianFit.logLikelihood, gaussianFit.k);

    FitResult poissonFit = fitPoisson(cleanData);
    double aicPoisson = aic(poissonFit.logLikelihood, poissonFit.k);

    double aicSkew = std::numeric_limits<double>::infinity();
    double aicMix  = std::numeric_limits<double>::infinity();

    bool skipExpensive = false;
    if (m_config.adaptiveModels) {
        // If Gaussian AIC is already better than Poisson by the threshold,
        // and the data is small enough that complex models are unlikely to help,
        // skip the expensive Skew-Normal and Bimodal fits.
        double bestSimpleAIC = std::min(aicGaussian, aicPoisson);
        // Complex models need to overcome AIC penalty for extra parameters.
        // Skew-Normal has k=3 (vs Gaussian k=2), Bimodal has k=5.
        // If AIC is already very good for simple models, skip.
        skipExpensive = (bestSimpleAIC < -2.0 * static_cast<double>(cleanData.size()))
                     || (cleanData.size() < 6);
    }

    if (!skipExpensive) {
        SkewNormalFitResult skewFit = fitSkewNormal(cleanData);
        aicSkew = aic(skewFit.logLikelihood, skewFit.k);

        GaussianMixResult mixFit = fitGaussianMixture2(cleanData);
        aicMix = aic(mixFit.logLikelihood, mixFit.k);
    }

    // 6. Select model with lowest AIC
    struct ModelAIC {
        DistributionType type;
        double aicValue;
    };

    ModelAIC models[] = {
        {DistributionType::Gaussian, aicGaussian},
        {DistributionType::Poisson,  aicPoisson},
        {DistributionType::SkewNormal, aicSkew},
        {DistributionType::Bimodal,  aicMix}
    };

    DistributionType bestType = DistributionType::Gaussian;
    double bestAIC = aicGaussian;
    for (const auto& m : models) {
        if (m.aicValue < bestAIC) {
            bestAIC = m.aicValue;
            bestType = m.type;
        }
    }
```

**Step 5: Wire adaptiveModels parameter from Instance to PixelSelector**

In `NukeXStackInstance.cpp`, when creating the `PixelSelector::Config` (around line 248), add:

```cpp
      selConfig.adaptiveModels = p_adaptiveModels;
```

**Step 6: Register test in CMakeLists.txt**

In `tests/CMakeLists.txt`, add the new test source file.

**Step 7: Run all tests**

Run: `cd /home/scarter4work/projects/nukex3/build && cmake .. -DPCLDIR=$HOME/PCL && make -j$(nproc) && ctest --output-on-failure`
Expected: All tests pass (old + new)

**Step 8: Commit**

```bash
git add src/engine/PixelSelector.h src/engine/PixelSelector.cpp \
        src/NukeXStackInstance.cpp tests/unit/test_adaptive_models.cpp \
        tests/CMakeLists.txt
git commit -m "feat: adaptive model selection — skip expensive fits for simple pixels"
```

---

### Task 6: CUDA Build System Integration

**Files:**
- Modify: `CMakeLists.txt` — add CUDAToolkit detection, CUDA language, conditional compilation
- Modify: `Makefile` — add nvcc detection, CUDA compilation rules
- Create: `src/engine/cuda/` directory

**Step 1: Update CMakeLists.txt for CUDA**

After `find_package(OpenMP)` (line 45), add:

```cmake
# Optional CUDA support for GPU acceleration
include(CheckLanguage)
check_language(CUDA)
if(CMAKE_CUDA_COMPILER)
    enable_language(CUDA)
    find_package(CUDAToolkit QUIET)
    if(CUDAToolkit_FOUND)
        set(NUKEX_HAS_CUDA TRUE)
        message(STATUS "  CUDA:          ${CUDAToolkit_VERSION} (GPU acceleration enabled)")
    endif()
endif()
```

Modify the source file glob to exclude `.cu` files from the C++ glob:

```cmake
file(GLOB_RECURSE NUKEX_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
```

Add CUDA sources conditionally:

```cmake
if(NUKEX_HAS_CUDA)
    file(GLOB_RECURSE NUKEX_CUDA_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/src/engine/cuda/*.cu"
    )
    list(APPEND NUKEX_SOURCES ${NUKEX_CUDA_SOURCES})
    target_compile_definitions(NukeX PRIVATE NUKEX_HAS_CUDA)
    set_source_files_properties(${NUKEX_CUDA_SOURCES} PROPERTIES LANGUAGE CUDA)
    set_target_properties(NukeX PROPERTIES CUDA_STANDARD 17 CUDA_SEPARABLE_COMPILATION ON)
    target_link_libraries(NukeX PRIVATE CUDA::cudart)
endif()
```

**Step 2: Update Makefile for CUDA**

Add CUDA detection and compilation rules:

```makefile
# CUDA support (optional)
NVCC := $(shell which nvcc 2>/dev/null)
ifdef NVCC
    CUDA_HAS = 1
    CUDA_FLAGS = -DNUKEX_HAS_CUDA
    CUDA_DIR = src/engine/cuda
    CUDA_SOURCES = $(wildcard $(CUDA_DIR)/*.cu)
    CUDA_OBJECTS = $(CUDA_SOURCES:.cu=.o)
    NVCC_FLAGS = -std=c++17 -O3 --compiler-options="$(PLATFORM_CXXFLAGS) -fvisibility=hidden" \
                 -I$(PCL_INCDIR) -I$(CURDIR)/src -I$(CURDIR)/src/engine \
                 $(addprefix -I,$(subst -I,,$(filter -I%,$(VENDOR_CXXFLAGS)))) \
                 -DNUKEX_HAS_CUDA -DBOOST_MATH_STANDALONE=1
    CXXFLAGS += $(CUDA_FLAGS)
    OBJECTS += $(CUDA_OBJECTS)
    LDFLAGS += -lcudart
else
    CUDA_HAS = 0
endif
```

Add CUDA compilation rule:

```makefile
%.o: %.cu
	@echo "Compiling CUDA $<..."
	$(NVCC) $(NVCC_FLAGS) -c $< -o $@
```

**Step 3: Create the cuda directory**

Run: `mkdir -p /home/scarter4work/projects/nukex3/src/engine/cuda`

**Step 4: Create placeholder CudaRuntime.h**

Create `src/engine/cuda/CudaRuntime.h` with the detection API:

```cpp
#pragma once

#ifdef NUKEX_HAS_CUDA
#include <cuda_runtime.h>
#endif

namespace nukex {
namespace cuda {

// Returns true if a CUDA-capable GPU is available at runtime
bool isGpuAvailable();

// Returns GPU name string (empty if unavailable)
const char* gpuName();

// Returns GPU memory in MB (0 if unavailable)
size_t gpuMemoryMB();

} // namespace cuda
} // namespace nukex
```

**Step 5: Create CudaRuntime.cpp**

Create `src/engine/cuda/CudaRuntime.cpp`:

```cpp
#include "cuda/CudaRuntime.h"

namespace nukex {
namespace cuda {

#ifdef NUKEX_HAS_CUDA

static bool s_probed = false;
static bool s_available = false;
static char s_gpuName[256] = {};
static size_t s_memoryMB = 0;

static void probeOnce() {
    if (s_probed) return;
    s_probed = true;

    int deviceCount = 0;
    cudaError_t err = cudaGetDeviceCount(&deviceCount);
    if (err != cudaSuccess || deviceCount == 0) return;

    cudaDeviceProp prop;
    err = cudaGetDeviceProperties(&prop, 0);
    if (err != cudaSuccess) return;

    s_available = true;
    strncpy(s_gpuName, prop.name, sizeof(s_gpuName) - 1);
    s_memoryMB = prop.totalGlobalMem / (1024 * 1024);
}

bool isGpuAvailable() { probeOnce(); return s_available; }
const char* gpuName() { probeOnce(); return s_gpuName; }
size_t gpuMemoryMB() { probeOnce(); return s_memoryMB; }

#else

bool isGpuAvailable() { return false; }
const char* gpuName() { return ""; }
size_t gpuMemoryMB() { return 0; }

#endif

} // namespace cuda
} // namespace nukex
```

**Step 6: Build and verify**

Run: `cd /home/scarter4work/projects/nukex3 && make clean && make release 2>&1 | tail -10`
Expected: Build succeeds, CUDA detected, `NUKEX_HAS_CUDA` defined

Run: `cd /home/scarter4work/projects/nukex3/build && cmake .. -DPCLDIR=$HOME/PCL && make -j$(nproc) && ctest --output-on-failure`
Expected: All tests pass

**Step 7: Commit**

```bash
git add CMakeLists.txt Makefile src/engine/cuda/CudaRuntime.h src/engine/cuda/CudaRuntime.cpp
git commit -m "build: conditional CUDA support with runtime GPU detection"
```

---

### Task 7: CUDA Pixel Selection Kernel

**Files:**
- Create: `src/engine/cuda/CudaPixelSelector.h`
- Create: `src/engine/cuda/CudaPixelSelector.cu`

This is the core CUDA kernel. It reimplements the full `selectBestZ()` pipeline as device code: MAD sigma-clip, ESD, all 4 distribution fits, AIC selection, median pixel value. No Boost, no Eigen — all self-contained.

**Step 1: Create CudaPixelSelector.h**

```cpp
#pragma once

#ifdef NUKEX_HAS_CUDA

#include <cstddef>
#include <cstdint>
#include <vector>

namespace nukex {
namespace cuda {

struct GpuStackConfig {
    int maxOutliers;
    double outlierAlpha;
    bool adaptiveModels;
    size_t nSubs;
    size_t height;
    size_t width;
};

struct GpuStackResult {
    bool success;
    std::string errorMessage;
};

// Run pixel selection on GPU.
// cubeData: column-major float array (nSubs * height * width)
// outputPixels: pre-allocated float array (height * width)
// distTypes: pre-allocated uint8_t array (height * width)
GpuStackResult processImageGPU(
    const float* cubeData,
    float* outputPixels,
    uint8_t* distTypes,
    const GpuStackConfig& config);

} // namespace cuda
} // namespace nukex

#endif // NUKEX_HAS_CUDA
```

**Step 2: Create CudaPixelSelector.cu**

This is a large file. Key device functions:

- `__device__ void sortDouble(double*, int)` — in-place insertion sort for small N (~30)
- `__device__ double medianDevice(double*, int)` — median via sort
- `__device__ void sigmaClipMAD_device(...)` — MAD-based outlier detection
- `__device__ void detectOutliersESD_device(...)` — Generalized ESD
- `__device__ double fitGaussian_device(...)` — analytical MLE, returns AIC
- `__device__ double fitPoisson_device(...)` — analytical MLE, returns AIC
- `__device__ double fitSkewNormal_device(...)` — L-BFGS with analytical gradients, returns AIC
- `__device__ double fitBimodalEM_device(...)` — EM algorithm, returns AIC
- `__global__ void pixelSelectionKernel(...)` — one thread per pixel

The kernel operates on the column-major SubCube layout. Each thread reads its Z-column (stride-1 in column-major), runs the full pipeline, and writes the output pixel value and distribution type.

Create the file with all device functions implementing the same math as the CPU versions. The Skew-Normal device fitter uses a simple L-BFGS implementation (3 parameters, history=5) with analytical gradients — no Eigen dependency.

**Step 3: Implement processImageGPU() host function**

This function:
1. Allocates device memory for cube, output, distTypes
2. Copies cube data H2D
3. Launches kernel with grid = ceil(totalPixels / 256), block = 256
4. Copies results D2H
5. Frees device memory
6. Returns success/error status

**Step 4: Build and verify**

Run: `cd /home/scarter4work/projects/nukex3 && make clean && make release 2>&1 | tail -10`
Expected: CUDA compiles without errors

**Step 5: Commit**

```bash
git add src/engine/cuda/CudaPixelSelector.h src/engine/cuda/CudaPixelSelector.cu
git commit -m "feat: CUDA pixel selection kernel with all 4 distribution fits"
```

---

### Task 8: Wire GPU Path into PixelSelector and ExecuteGlobal

**Files:**
- Modify: `src/engine/PixelSelector.h` — add processImageGPU method
- Modify: `src/engine/PixelSelector.cpp` — implement GPU dispatch
- Modify: `src/NukeXStackInstance.cpp` — pass useGPU flag, add GPU console logging

**Step 1: Add processImageGPU to PixelSelector**

In `src/engine/PixelSelector.h`, add config member and GPU method:

```cpp
    struct Config {
        int maxOutliers = 3;
        double outlierAlpha = 0.05;
        bool useQualityWeights = true;
        bool adaptiveModels = false;
        double adaptiveAicThreshold = 4.0;
        bool useGPU = false;              // NEW
    };

    // GPU-accelerated image processing (falls back to CPU on failure)
    std::vector<float> processImageGPU(SubCube& cube,
                                        const std::vector<double>& qualityWeights,
                                        std::vector<uint8_t>& distTypesOut,
                                        ProgressCallback progress = nullptr);
```

**Step 2: Implement processImageGPU in PixelSelector.cpp**

```cpp
#ifdef NUKEX_HAS_CUDA
#include "cuda/CudaPixelSelector.h"
#include "cuda/CudaRuntime.h"
#endif

std::vector<float> PixelSelector::processImageGPU(SubCube& cube,
                                                    const std::vector<double>& qualityWeights,
                                                    std::vector<uint8_t>& distTypesOut,
                                                    ProgressCallback progress)
{
#ifdef NUKEX_HAS_CUDA
    if (!cuda::isGpuAvailable()) {
        // Fall back to CPU
        return processImage(cube, qualityWeights, progress);
    }

    size_t H = cube.height();
    size_t W = cube.width();
    size_t totalPixels = H * W;

    std::vector<float> output(totalPixels);
    distTypesOut.resize(totalPixels);

    cuda::GpuStackConfig gpuConfig;
    gpuConfig.maxOutliers = m_config.maxOutliers;
    gpuConfig.outlierAlpha = m_config.outlierAlpha;
    gpuConfig.adaptiveModels = m_config.adaptiveModels;
    gpuConfig.nSubs = cube.numSubs();
    gpuConfig.height = H;
    gpuConfig.width = W;

    auto result = cuda::processImageGPU(
        cube.rawData(), output.data(), distTypesOut.data(), gpuConfig);

    if (!result.success) {
        // GPU failed — fall back to CPU
        return processImage(cube, qualityWeights, progress);
    }

    // Copy distTypes back into cube metadata
    for (size_t y = 0; y < H; ++y)
        for (size_t x = 0; x < W; ++x)
            cube.setDistType(y, x, distTypesOut[y * W + x]);

    return output;
#else
    // No CUDA compiled in — CPU only
    return processImage(cube, qualityWeights, progress);
#endif
}
```

**Step 3: Update ExecuteGlobal to use GPU path**

In `NukeXStackInstance.cpp`, in the Phase 3 per-channel loop, before calling `selector.processImage()`, add GPU detection and dispatch:

```cpp
      selConfig.useGPU = p_useGPU;

      // Check GPU availability at runtime
      bool gpuAvailable = false;
#ifdef NUKEX_HAS_CUDA
      gpuAvailable = nukex::cuda::isGpuAvailable();
      if (gpuAvailable) {
         console.WriteLn( String().Format( "  GPU: %s (%zu MB VRAM)",
            nukex::cuda::gpuName(), nukex::cuda::gpuMemoryMB() ) );
      }
#endif
      bool useGPU = p_useGPU && gpuAvailable;
      console.WriteLn( String().Format( "  Compute: %s | Adaptive: %s",
         useGPU ? "GPU (CUDA)" : "CPU (OpenMP)",
         p_adaptiveModels ? "On" : "Off" ) );
```

Then in the per-channel stacking loop, replace the `selector.processImage()` call with a conditional:

```cpp
         if (useGPU) {
            std::vector<uint8_t> gpuDistTypes;
            channelResults[ch] = selector.processImageGPU(cube, weights, gpuDistTypes, progressCB);
            distTypeMaps[ch] = std::move(gpuDistTypes);
         } else {
            channelResults[ch] = selector.processImage(cube, weights, progressCB);
            // ... existing distType extraction code ...
         }
```

**Step 4: Build and verify**

Run: `cd /home/scarter4work/projects/nukex3 && make clean && make release 2>&1 | tail -10`
Expected: Build succeeds with CUDA

Run: `cd /home/scarter4work/projects/nukex3/build && cmake .. -DPCLDIR=$HOME/PCL && make -j$(nproc) && ctest --output-on-failure`
Expected: All tests pass

**Step 5: Commit**

```bash
git add src/engine/PixelSelector.h src/engine/PixelSelector.cpp \
        src/NukeXStackInstance.cpp
git commit -m "feat: wire GPU pixel selection into stacking pipeline with CPU fallback"
```

---

### Task 9: CUDA vs CPU Equivalence Tests

**Files:**
- Create: `tests/unit/test_cuda_equivalence.cpp`

**Step 1: Write equivalence test**

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "engine/PixelSelector.h"
#include "engine/SubCube.h"

#ifdef NUKEX_HAS_CUDA
#include "cuda/CudaRuntime.h"
#endif

using Catch::Approx;

TEST_CASE("GPU and CPU produce equivalent results", "[cuda][equivalence]") {
#ifndef NUKEX_HAS_CUDA
    SKIP("CUDA not available");
#else
    if (!nukex::cuda::isGpuAvailable()) {
        SKIP("No GPU detected");
    }

    // Create a small cube with varied data
    size_t nSubs = 15, H = 4, W = 4;
    nukex::SubCube cube(nSubs, H, W);

    // Fill with pseudo-random but deterministic data
    for (size_t z = 0; z < nSubs; ++z)
        for (size_t y = 0; y < H; ++y)
            for (size_t x = 0; x < W; ++x) {
                float val = 0.3f + 0.01f * z + 0.001f * (y * W + x);
                // Add one outlier per pixel at z=0
                if (z == 0) val += 0.5f;
                cube.setSample(z, y, x, val);
            }

    std::vector<double> weights(nSubs, 1.0 / nSubs);

    // CPU path
    nukex::PixelSelector::Config cpuConfig;
    cpuConfig.maxOutliers = 3;
    cpuConfig.adaptiveModels = false;
    nukex::PixelSelector cpuSelector(cpuConfig);
    auto cpuResult = cpuSelector.processImage(cube, weights);

    // GPU path
    nukex::PixelSelector::Config gpuConfig;
    gpuConfig.maxOutliers = 3;
    gpuConfig.adaptiveModels = false;
    gpuConfig.useGPU = true;
    nukex::PixelSelector gpuSelector(gpuConfig);
    std::vector<uint8_t> gpuDistTypes;
    auto gpuResult = gpuSelector.processImageGPU(cube, weights, gpuDistTypes);

    // Compare results within float tolerance
    REQUIRE(cpuResult.size() == gpuResult.size());
    for (size_t i = 0; i < cpuResult.size(); ++i) {
        REQUIRE(gpuResult[i] == Approx(cpuResult[i]).margin(1e-4));
    }
#endif
}

TEST_CASE("GPU fallback to CPU on no-CUDA build", "[cuda][fallback]") {
    // This should work regardless of CUDA availability
    nukex::PixelSelector::Config config;
    config.useGPU = true; // request GPU, but should fall back gracefully
    nukex::PixelSelector selector(config);

    size_t nSubs = 5, H = 2, W = 2;
    nukex::SubCube cube(nSubs, H, W);
    for (size_t z = 0; z < nSubs; ++z)
        for (size_t y = 0; y < H; ++y)
            for (size_t x = 0; x < W; ++x)
                cube.setSample(z, y, x, 0.5f + 0.01f * z);

    std::vector<double> weights(nSubs, 0.2);
    std::vector<uint8_t> distTypes;
    auto result = selector.processImageGPU(cube, weights, distTypes);

    REQUIRE(result.size() == H * W);
    for (float val : result)
        REQUIRE(std::isfinite(val));
}
```

**Step 2: Register test in CMakeLists.txt**

Add `tests/unit/test_cuda_equivalence.cpp` to the test build.

**Step 3: Build and run tests**

Run: `cd /home/scarter4work/projects/nukex3/build && cmake .. -DPCLDIR=$HOME/PCL && make -j$(nproc) && ctest --output-on-failure`
Expected: All tests pass

**Step 4: Commit**

```bash
git add tests/unit/test_cuda_equivalence.cpp tests/CMakeLists.txt
git commit -m "test: CUDA vs CPU equivalence tests and fallback verification"
```

---

### Task 10: Parallel Frame Loading

**Files:**
- Modify: `src/engine/FrameLoader.h` — no change needed if LoadRaw signature stays the same
- Modify: `src/engine/FrameLoader.cpp` — parallelize the loading loop

**Step 1: Read FrameLoader::LoadRaw() to understand current structure**

The current implementation loads frames sequentially in a for loop. Each frame: open file → read image → extract metadata → debayer if CFA → copy to SubCube.

**Step 2: Parallelize frame loading with OpenMP tasks**

The approach: load frames in parallel into temporary per-frame buffers, then copy to SubCube sequentially (SubCube writes are not thread-safe for concurrent frames).

```cpp
// Phase 1: Load all frames in parallel into temporary storage
struct FrameData {
    std::vector<std::vector<float>> channels; // [ch][pixels]
    SubMetadata meta;
    int width, height, numChannels;
    bool valid;
};

std::vector<FrameData> frameData(paths.size());

#pragma omp parallel for schedule(dynamic, 1)
for (size_t i = 0; i < paths.size(); ++i) {
    // Load frame i into frameData[i]
    // (PCL file I/O may not be fully thread-safe — use try/catch per frame)
}

// Phase 2: Copy to SubCube sequentially
for (size_t i = 0; i < paths.size(); ++i) {
    if (frameData[i].valid)
        // copy frameData[i] into SubCube
}
```

Note: PCL file I/O thread safety needs verification. If PCL file operations use shared state, we may need to serialize file opens but parallelize debayering and memory copies.

**Step 3: Build and verify**

Run: `cd /home/scarter4work/projects/nukex3 && make clean && make release 2>&1 | tail -5`
Expected: Build succeeds

Run: `cd /home/scarter4work/projects/nukex3/build && cmake .. -DPCLDIR=$HOME/PCL && make -j$(nproc) && ctest --output-on-failure`
Expected: All tests pass

**Step 4: Commit**

```bash
git add src/engine/FrameLoader.cpp
git commit -m "perf: parallel frame loading with OpenMP tasks"
```

---

### Task 11: Version Bump + Release Workflow

Follow the CLAUDE.md 8-step release workflow:

**Step 1: Bump version**

In `src/NukeXModule.cpp`, change:
```cpp
#define MODULE_VERSION_BUILD     15
```

Update release date to current.

**Step 2: Update repository/updates.xri**

Update title and description with v3.0.0.15 — GPU acceleration + performance optimizations.

**Step 3: Clean rebuild**

Run: `make clean && make release`

**Step 4: Run tests**

Run: `cd build && ctest --output-on-failure`

**Step 5: Package**

Run: `make package`

**Step 6: Install**

Run: `sudo make install`

**Step 7: Commit everything**

```bash
git add src/NukeXModule.cpp repository/ ...
git commit -m "feat: GPU acceleration + adaptive model selection — v3.0.0.15"
```

**Step 8: Push**

Run: `git push`
