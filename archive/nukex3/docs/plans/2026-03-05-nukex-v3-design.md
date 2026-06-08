# NukeX v3 — Complete Design Document

*March 5, 2026*

---

## 1. Overview

NukeX v3 is a complete architectural rethink. It abandons the ML-based segmentation approach
of v1/v2 in favor of a mathematically rigorous per-pixel statistical inference model operating
on unstretched linear data. The module ships two processes:

1. **NukeXStack** — Per-pixel distribution inference stacking engine (the core innovation)
2. **NukeXStretch** — 11 stretch algorithms ported from v2, no ML/segmentation dependencies

The goal: a real PixInsight Process Module (PCL/C++17) that produces demonstrably better
stacks than WBPP by making per-pixel rejection decisions informed by per-sub quality metadata
and fitted statistical distributions.

---

## 2. Dependency Stack

All dependencies are header-only with permissive licenses. No GPL, no LGPL, no dynamic
linking requirements.

| Layer | Library | Version | Purpose | License |
|---|---|---|---|---|
| 3D Tensor | xtensor | 0.26.x | Cube storage, contiguous Z-column extraction | BSD-3 |
| SIMD | xsimd | latest | Vectorized operations for xtensor | BSD-3 |
| Distributions | Boost.Math | 1.89+ | PDF/CDF/quantile, KS test, special functions | BSL |
| Optimizer | LBFGSpp | latest | L-BFGS for Skew-Normal MLE | MIT |
| Small vectors | Eigen | 5.x | Internal to LBFGSpp | MPL-2.0 |
| Robust stats | PCL native | — | Median, MAD, Sn, Qn, biweight midvariance | PCLL |
| Framework | PCL | — | PixInsight module framework | PCLL |

All vendored in `third_party/` — no package manager required to build.

---

## 3. Data Model

### 3.1 The 3D Sub Cube

```cpp
xt::xtensor<float, 3> cube({N_subs, height, width});  // row-major
```

- **Shape**: `(N_subs, height, width)` — Z-axis first
- **Layout**: Row-major (xtensor default) — Z-values at each (X,Y) are contiguous in memory
- **Type**: `float` for storage (~32 GB at 4096×4096×500), `double` for all computation
- **Loading**: Sequential read of all calibrated, registered subs via PCL FileFormat API

Memory access pattern for the hot loop:
```
cube[0][y][x], cube[1][y][x], ..., cube[N-1][y][x]  ← contiguous floats
```

### 3.2 Per-Sub Metadata

```cpp
struct SubMetadata {
    double fwhm;           // Focus quality (lower = better)
    double eccentricity;   // Tracking error (lower = better)
    double skyBackground;  // Light pollution level
    double hfr;            // Half-flux radius (overall quality)
    double altitude;       // Atmospheric refraction proxy
    double exposure;       // Exposure time (seconds)
    double gain;           // Camera gain
    double ccdTemp;        // CCD temperature
    String object;         // Target name
    String filter;         // Filter (Ha, OIII, L, R, G, B, etc.)
};

std::vector<SubMetadata> metadata(N_subs);  // Indexed by Z
```

Parsed from FITS headers at load time using PCL's `FITSHeaderKeyword` / `FITSKeywordArray`.

### 3.3 Output Products

1. **Stacked image** — `pcl::Image` (W×H, the final result)
2. **Provenance map** — `xt::xtensor<uint32_t, 2>({height, width})` — selected Z index per pixel
3. **Distribution type map** — `xt::xtensor<uint8_t, 2>({height, width})` — fitted model per pixel
   (0=Gaussian, 1=Poisson, 2=SkewNormal, 3=Bimodal)
4. **Distribution parameters** — per-pixel fitted parameters (for Phase 2 stretch integration)

### 3.4 Memory Safety

All large allocations use RAII and are guarded against `std::bad_alloc`:

```cpp
class SubCube {
    xt::xtensor<float, 3> m_cube;
    std::vector<SubMetadata> m_metadata;
    xt::xtensor<uint32_t, 2> m_provenance;
    xt::xtensor<uint8_t, 2> m_distType;

public:
    SubCube(size_t nSubs, size_t height, size_t width)
    try : m_cube({nSubs, height, width}),
          m_provenance({height, width}),
          m_distType({height, width})
    {
        m_metadata.reserve(nSubs);
    }
    catch (const std::bad_alloc&) {
        double gb = (nSubs * height * width * sizeof(float)) / (1024.0*1024.0*1024.0);
        Console().CriticalLn(
            "NukeX: Failed to allocate sub cube ("
            + String::ToFormattedString(gb, 1) + " GB). "
            + "Reduce stack size or free RAM.");
        throw;
    }
};
```

Additional safeguards:
- Top-level `catch(...)` in `ExecuteGlobal()` to log diagnostics before PCL's exception handler
- All xtensor containers are RAII — destructors free on any exception propagation
- No raw `new`/`delete` anywhere in the codebase

---

## 4. Statistical Engine

### 4.1 Per-Pixel Distribution Fitting

For each (X,Y) position:

1. **Extract Z-column** — contiguous read via `xt::view(cube, xt::all(), y, x)`
2. **Promote to double** — copy 40-500 floats into a thread-local `std::vector<double>`
3. **Apply quality weights** — multiply by per-Z weight derived from SubMetadata
4. **Fit all four distributions**:

| Distribution | Parameters | MLE Method | Cost |
|---|---|---|---|
| Gaussian | mu, sigma (k=2) | Closed-form: mu=mean, sigma²=var | ~1 µs |
| Poisson | lambda (k=1) | Closed-form: lambda=mean | ~0.5 µs |
| Skew-Normal | xi, omega, alpha (k=3) | L-BFGS via LBFGSpp, analytic gradient | ~20-50 µs |
| Bimodal Mixture | mu1, sigma1, mu2, sigma2, w (k=5) | 2-component EM, log-sum-exp | ~50-100 µs |

5. **Score with AIC/BIC** — select best-fit model:
   ```
   AIC  = 2k - 2·logL
   BIC  = k·ln(n) - 2·logL
   AICc = AIC + 2k(k+1)/(n-k-1)
   ```

6. **Select highest-probability Z value** under fitted model, weighted by sub quality
7. **Record provenance** — store selected Z index and distribution type

### 4.2 Quality Weighting

Per-sub weights derived from FITS metadata:

| Attribute | Weight Effect |
|---|---|
| FWHM | Higher FWHM = blurrier = downweight |
| Eccentricity | Tracking error = downweight |
| Sky background | Light pollution = downweight |
| HFR | Overall quality inverse |
| Altitude | Low altitude = more atmosphere = downweight |

Weights are normalized so they sum to 1 across all subs. Applied as multiplicative
factors during distribution fitting.

### 4.3 Outlier Detection

Before distribution fitting, flag obvious outliers:
- Generalized ESD test (iterative Grubbs') for automatic outlier count detection
- Chauvenet's criterion as a fast pre-filter
- Both use Boost.Math distribution quantiles (Student's t, Normal CDF)

Flagged pixels are excluded from fitting but recorded in provenance metadata.

### 4.4 Threading Model

- Embarrassingly parallel across pixels — each (X,Y) is independent
- PCL `Thread` pool or `AbstractImage::RunThreads`
- Per-thread pre-allocated state:
  - One `LBFGSpp::LBFGSSolver` instance
  - One `std::vector<double>` scratch buffer for Z-column (max N_subs elements)
  - One `std::vector<double>` scratch buffer for weights
- Zero locks, zero shared mutable state
- Output written to non-overlapping regions of provenance/distType tensors

### 4.5 Numerical Stability

- All statistics computed in `double` (pixel data promoted from `float`)
- Log-likelihood throughout (never raw likelihood)
- Log-sum-exp trick for mixture model E-step
- `lgamma` via Boost.Math (not `tgamma`)
- Variance floored at 1e-10 in EM to prevent singularity
- Bounded parameter optimization: log(sigma) instead of sigma, softmax for mixture weights
- Kahan compensated summation via PCL's `StableSum` where applicable

---

## 5. Stretch Process

### 5.1 Ported Algorithms (11)

1. MTF (Midtones Transfer Function)
2. GHS (Generalized Hyperbolic Stretch)
3. ArcSinh (HDR compression)
4. Histogram (classical equalization)
5. Log (shadow detail)
6. Lumpton (SDSS-style HDR)
7. RNC (Roger Clark Color Stretch)
8. Photometric (preserves photometric accuracy)
9. OTS (Optimal Transfer Stretch)
10. SAS (Statistical Adaptive Stretch)
11. VeraLux

All ported directly from v2 `src/engine/algorithms/`. No ONNX, no segmentation,
no region analysis dependencies.

### 5.2 Auto Mode (Phase 1)

Lightweight heuristics from `pcl::ImageStatistics`:
- Histogram shape analysis (peak location, skewness)
- Dynamic range measurement
- Noise floor estimation
- Simple decision tree to recommend algorithm

### 5.3 Auto Mode (Phase 2 — Deferred)

Feed distribution metadata from stacking engine into stretch selection:
- Per-pixel distribution shapes reveal object types (stars, nebulae, background)
- Statistically-derived segmentation map replaces ML-based regions
- Per-region or per-pixel stretch informed by stacking engine output

---

## 6. PCL Module Architecture

### 6.1 Module Structure

```
NukeXModule (MetaModule)
├── NukeXStackProcess (MetaProcess)
│   ├── Category: "ImageIntegration"
│   ├── NukeXStackInstance (ProcessImplementation)
│   │   └── ExecuteGlobal()
│   ├── NukeXStackInterface (ProcessInterface)
│   └── NukeXStackParameters
│
└── NukeXStretchProcess (MetaProcess)
    ├── Category: "IntensityTransformations"
    ├── NukeXStretchInstance (ProcessImplementation)
    │   └── ExecuteOn()
    ├── NukeXStretchInterface (ProcessInterface)
    └── NukeXStretchParameters
```

### 6.2 Stack Parameters

| Parameter | Type | Description |
|---|---|---|
| InputFrames | MetaTable | Path + enabled flag per sub |
| OutlierSigmaThreshold | MetaFloat | Sigma threshold for outlier pre-filter |
| QualityWeightMode | MetaEnumeration | None, FWHM-only, Full metadata |
| FWHMWeight | MetaFloat | Weight multiplier for FWHM |
| EccentricityWeight | MetaFloat | Weight multiplier for eccentricity |
| SkyBackgroundWeight | MetaFloat | Weight multiplier for sky background |
| HFRWeight | MetaFloat | Weight multiplier for HFR |
| AltitudeWeight | MetaFloat | Weight multiplier for altitude |
| GenerateProvenance | MetaBoolean | Output provenance map |
| GenerateDistMetadata | MetaBoolean | Output distribution metadata |

### 6.3 Stretch Parameters

| Parameter | Type | Description |
|---|---|---|
| Algorithm | MetaEnumeration | 11 algorithms + Auto |
| Contrast | MetaFloat | Global contrast adjustment |
| Saturation | MetaFloat | Saturation boost |
| BlackPoint | MetaFloat | Black point clipping |
| WhitePoint | MetaFloat | White point clipping |
| Gamma | MetaFloat | Gamma correction |
| StretchStrength | MetaFloat | Overall stretch intensity |

### 6.4 Progress Reporting

Four-phase status via `pcl::StandardStatus`:
1. "Loading sub frames..." (% of files read)
2. "Computing quality weights..." (single pass, fast)
3. "Fitting distributions..." (% of pixels — the main progress bar)
4. "Selecting optimal pixels..." (% of pixels)

### 6.5 Exception Handling

```cpp
bool NukeXStackInstance::ExecuteGlobal()
{
    try {
        // Phase 1-4
        return true;
    }
    catch (const std::bad_alloc& e) {
        Console().CriticalLn("NukeX: Out of memory — " + String(e.what()));
        // RAII cleans up all xtensor/vector allocations
        return false;
    }
    catch (const pcl::ProcessAborted&) {
        // User cancelled — RAII cleans up
        throw;  // Let PCL handle abort
    }
    catch (const std::exception& e) {
        Console().CriticalLn("NukeX: " + String(e.what()));
        return false;
    }
    catch (...) {
        Console().CriticalLn("NukeX: Unknown error during execution");
        return false;
    }
}
```

---

## 7. Project Structure

```
nukex3/
├── src/
│   ├── NukeXModule.cpp/.h              # Module entry point
│   ├── NukeXStackProcess.cpp/.h        # Stack MetaProcess
│   ├── NukeXStackInstance.cpp/.h       # Stack ProcessImplementation
│   ├── NukeXStackInterface.cpp/.h      # Stack UI
│   ├── NukeXStackParameters.cpp/.h     # Stack parameters
│   ├── NukeXStretchProcess.cpp/.h      # Stretch MetaProcess
│   ├── NukeXStretchInstance.cpp/.h     # Stretch ProcessImplementation
│   ├── NukeXStretchInterface.cpp/.h    # Stretch UI
│   ├── NukeXStretchParameters.cpp/.h   # Stretch parameters
│   └── engine/
│       ├── SubCube.h/.cpp              # 3D tensor wrapper, RAII, loading
│       ├── QualityWeights.h/.cpp       # FITS metadata → per-sub weights
│       ├── DistributionFitter.h/.cpp   # All 4 distribution MLE + AIC/BIC
│       ├── PixelSelector.h/.cpp        # Per-pixel Z selection engine
│       ├── OutlierDetector.h/.cpp      # Grubbs, ESD, Chauvenet
│       ├── GaussianMixEM.h/.cpp        # 2-component EM for bimodal
│       ├── NumericalUtils.h            # log-sum-exp, Kahan sum, etc.
│       └── algorithms/                 # 11 stretch algorithms (ported)
│           ├── MTFStretch.h
│           ├── GHStretch.h
│           ├── ArcSinhStretch.h
│           ├── HistogramStretch.h
│           ├── LogStretch.h
│           ├── LumptonStretch.h
│           ├── RNCStretch.h
│           ├── PhotometricStretch.h
│           ├── OTSStretch.h
│           ├── SASStretch.h
│           └── VeraluxStretch.h
├── third_party/
│   ├── xtensor/                        # Vendored headers
│   ├── xsimd/                          # Vendored headers
│   ├── boost_math/                     # Boost.Math headers only
│   ├── lbfgspp/                        # Vendored headers
│   └── eigen/                          # Vendored headers
├── tests/
│   ├── unit/
│   │   ├── test_distribution_fitter.cpp
│   │   ├── test_outlier_detector.cpp
│   │   ├── test_gaussian_mix_em.cpp
│   │   ├── test_quality_weights.cpp
│   │   ├── test_pixel_selector.cpp
│   │   ├── test_sub_cube.cpp
│   │   ├── test_numerical_utils.cpp
│   │   └── test_stretch_algorithms.cpp
│   ├── integration/
│   │   ├── test_full_pipeline.cpp
│   │   └── test_mock_data_generator.cpp
│   ├── mutation/
│   │   └── mutation_config.yml          # Mutation testing configuration
│   └── mock_data/
│       └── generate_mock_subs.cpp       # Synthetic FITS generator
├── repository/
│   └── updates.xri                      # PixInsight update metadata
├── docs/
│   └── plans/
│       ├── 2026-03-05-nukex-v3-design.md
│       ├── 2026-03-05-design-decisions.md
│       └── 2026-03-05-library-research.md
├── CMakeLists.txt
├── Makefile
├── ARCHITECTURE.md
├── .mcp.json                            # PCL language server config
└── CLAUDE.md                            # Project-specific AI instructions
```

---

## 8. Build System

### CMake

```cmake
cmake_minimum_required(VERSION 3.15)
project(NukeX VERSION 3.0.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# PCL
set(PCLDIR $ENV{PCLDIR})
include_directories(${PCLDIR}/include)
link_directories(${PCLDIR}/lib)

# Header-only deps
include_directories(third_party/xtensor/include)
include_directories(third_party/xsimd/include)
include_directories(third_party/boost_math/include)
include_directories(third_party/lbfgspp/include)
include_directories(third_party/eigen)

# Compiler flags
add_compile_options(-O3 -fPIC -fvisibility=hidden -fvisibility-inlines-hidden
                    -fnon-call-exceptions -D__PCL_BUILDING_MODULE)

# Platform
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_compile_definitions(__PCL_LINUX)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    add_compile_definitions(__PCL_MACOSX)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    add_compile_definitions(__PCL_WINDOWS)
endif()

# SIMD
add_compile_definitions(XTENSOR_USE_XSIMD)

# Sources
file(GLOB_RECURSE SOURCES "src/*.cpp")
add_library(NukeX-pxm SHARED ${SOURCES})

# Link PCL
target_link_libraries(NukeX-pxm
    PCL-pxi lz4-pxi zstd-pxi zlib-pxi RFC6234-pxi lcms-pxi cminpack-pxi
    pthread)

# OpenMP for parallel pixel processing
find_package(OpenMP)
if(OpenMP_CXX_FOUND)
    target_link_libraries(NukeX-pxm OpenMP::OpenMP_CXX)
endif()

# Tests
enable_testing()
add_subdirectory(tests)
```

### Makefile (alternative)

Adapted from v2 with updated include paths for third_party deps.

---

## 9. Testing Strategy

### 9.1 Unit Tests (Catch2)

- **DistributionFitter**: Synthetic data from known distributions, verify MLE params ±tolerance
- **OutlierDetector**: Inject known outliers at known positions, verify detection rate
- **GaussianMixEM**: Synthetic bimodal data, verify component separation
- **QualityWeights**: Mock FITS headers, verify weight computation
- **PixelSelector**: Known-distribution pixel stacks, verify correct Z selection
- **SubCube**: Allocation, loading, exception handling, memory cleanup
- **NumericalUtils**: log-sum-exp precision, Kahan sum accuracy
- **Stretch algorithms**: Reference image comparison (ported from v2 tests)

### 9.2 Integration Tests

- Full pipeline with mock FITS subs (synthetic, injected artifacts)
- End-to-end: load → cube → fit → select → output image
- Regression baseline against WBPP output on same dataset
- Memory usage profiling under large stack sizes

### 9.3 Mutation Testing

- Mutate distribution fitting (wrong MLE formula) → tests must catch
- Mutate AIC/BIC scoring (swap AIC for BIC) → model selection tests must catch
- Mutate quality weighting (invert weights) → output quality tests must catch
- Mutate outlier detection thresholds → detection rate tests must catch

### 9.4 Mock Data Generator

Generates synthetic calibrated subs with:
- Known sky background + Poisson noise
- Injected stars (Gaussian PSF)
- Injected satellite trails (linear features in specific Z slices)
- Injected cosmic rays (single-pixel spikes in specific Z slices)
- Variable seeing (per-sub FWHM variation)
- Known FITS headers matching the SubMetadata fields

---

## 10. Release Pipeline

1. **Build**: `cmake --build build --config Release`
2. **Test**: `cd build && ctest --output-on-failure`
3. **Sign module**:
   ```bash
   /opt/PixInsight/bin/PixInsight.sh \
     --sign-module-file=NukeX-pxm.so \
     --xssk-file=/home/scarter4work/projects/keys/scarter4work_keys.xssk \
     --xssk-password="<password>"
   ```
4. **Package**: `tar czf YYYYMMDD-linux-x64-NukeX.tar.gz NukeX-pxm.so NukeX-pxm.xsgn`
5. **Generate updates.xri**: Update repository metadata with SHA1 hash
6. **Install locally**: Copy to `/opt/PixInsight/bin/` and test in PixInsight
7. **Push to GitHub**: Commit, tag release, push
8. **GitHub Release**: Upload tar.gz artifact

---

## 11. Development Phases

### Phase 1 — Data Model + Infrastructure
- Project scaffolding (module registration, build system, third_party deps)
- SubCube class with xtensor, FITS loading, metadata parsing
- Memory safety infrastructure (RAII, exception guards)
- Mock data generator
- Unit tests for SubCube

### Phase 2 — Statistical Engine
- Closed-form Gaussian and Poisson MLE
- Skew-Normal MLE with LBFGSpp
- 2-component Gaussian EM for bimodal
- AIC/BIC model selection
- Outlier detection (Grubbs, ESD, Chauvenet)
- Quality weighting from metadata
- PixelSelector orchestration
- Comprehensive unit tests

### Phase 3 — PCL Process Integration
- NukeXStackProcess, Instance, Interface, Parameters
- Progress reporting (4-phase StandardStatus)
- Provenance map output
- Integration tests with mock data

### Phase 4 — Stretch Port
- Port 11 algorithms from v2 (no segmentation deps)
- NukeXStretchProcess, Instance, Interface, Parameters
- Auto mode with lightweight heuristics
- Unit tests (reference image comparison)

### Phase 5 — Release
- Full test suite (unit + integration + mutation)
- Local installation and validation in PixInsight
- Module signing
- Package, upload, GitHub release

---

## 12. Key Architectural Principles

- **Per-pixel, not per-sub**: Every decision (model selection, rejection, weighting) happens at the individual pixel level
- **Math, not ML**: Statistical distributions fitted to real data, not a neural network's approximation
- **RAII everywhere**: No manual memory management, graceful cleanup on any exception
- **Embarrassingly parallel**: Each pixel is independent, thread pool saturates all cores
- **Contiguous Z-access**: Memory layout optimized for the hot loop's access pattern
- **Double precision compute**: Promote from float storage to double for all statistics
- **Provenance tracking**: Every output pixel has a paper trail back to its source sub

---

*Design approved — March 5, 2026*
