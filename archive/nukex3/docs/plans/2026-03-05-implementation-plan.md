# NukeX v3 Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Build a PixInsight PCL/C++17 process module that stacks astrophotography subs using per-pixel statistical distribution inference, plus 11 ported stretch algorithms.

**Architecture:** A 3D sub cube (xtensor) loads all calibrated subs into RAM. For each pixel, four distribution models (Gaussian, Poisson, Skew-Normal, Bimodal Mixture) are fitted via MLE and scored with AIC/BIC. The winning model selects the optimal Z value. Stretch algorithms are ported from v2 without ML/segmentation dependencies.

**Tech Stack:** C++17, PCL (PixInsight Class Library), xtensor 0.26.x, xsimd, Boost.Math, LBFGSpp, Eigen 5, Catch2

**Reference docs:**
- Design: `docs/plans/2026-03-05-nukex-v3-design.md`
- Decisions: `docs/plans/2026-03-05-design-decisions.md`
- Library research: `docs/plans/2026-03-05-library-research.md`
- Architecture: `ARCHITECTURE.md`
- v2 source (read-only reference): `~/projects/NukeX/src/`
- PCL language server: `.mcp.json` (use `pcl_class_info`, `pcl_template`, `pcl_validate`)

---

## Phase 1 — Project Scaffolding & Data Model

### Task 1.1: Initialize project structure and vendored dependencies

**Files:**
- Create: `CMakeLists.txt`
- Create: `Makefile`
- Create: `src/` (empty directory)
- Create: `tests/` (empty directory)
- Create: `third_party/` (vendored headers)
- Create: `CLAUDE.md`

**Step 1: Create project CLAUDE.md**

```markdown
# NukeX v3

## Build
- CMake: `mkdir build && cd build && cmake .. && make -j$(nproc)`
- Make: `make release`
- Tests: `cd build && ctest --output-on-failure`

## Architecture
- PCL/C++17 PixInsight Process Module
- Two processes: NukeXStack (stacking) + NukeXStretch (stretching)
- See docs/plans/2026-03-05-nukex-v3-design.md for full design

## Dependencies (all header-only, vendored in third_party/)
- xtensor 0.26.x — 3D tensor (BSD-3)
- xsimd — SIMD for xtensor (BSD-3)
- Boost.Math — distributions, special functions (BSL)
- LBFGSpp — L-BFGS optimizer (MIT)
- Eigen 5 — linear algebra for LBFGSpp (MPL-2.0)
- Catch2 v3 — testing (BSL)

## Conventions
- RAII everywhere — no raw new/delete
- float for storage, double for computation
- All statistical functions must be thread-safe / reentrant
- Use PCL language server (pcl_class_info, pcl_validate) for PCL API accuracy
- Follow v2 patterns from ~/projects/NukeX/src/ for PCL boilerplate
```

**Step 2: Vendor dependencies**

```bash
# xtensor + xtl + xsimd
cd third_party
git clone --depth 1 --branch 0.26.0 https://github.com/xtensor-stack/xtensor.git
git clone --depth 1 https://github.com/xtensor-stack/xtl.git
git clone --depth 1 https://github.com/xtensor-stack/xsimd.git

# Boost.Math (headers only — extract just the math subset)
git clone --depth 1 --branch boost-1.87.0 https://github.com/boostorg/math.git boost_math
git clone --depth 1 --branch boost-1.87.0 https://github.com/boostorg/config.git boost_config
git clone --depth 1 --branch boost-1.87.0 https://github.com/boostorg/assert.git boost_assert
git clone --depth 1 --branch boost-1.87.0 https://github.com/boostorg/throw_exception.git boost_throw_exception
git clone --depth 1 --branch boost-1.87.0 https://github.com/boostorg/core.git boost_core
git clone --depth 1 --branch boost-1.87.0 https://github.com/boostorg/type_traits.git boost_type_traits
git clone --depth 1 --branch boost-1.87.0 https://github.com/boostorg/static_assert.git boost_static_assert
git clone --depth 1 --branch boost-1.87.0 https://github.com/boostorg/mp11.git boost_mp11
git clone --depth 1 --branch boost-1.87.0 https://github.com/boostorg/integer.git boost_integer
git clone --depth 1 --branch boost-1.87.0 https://github.com/boostorg/lexical_cast.git boost_lexical_cast
git clone --depth 1 --branch boost-1.87.0 https://github.com/boostorg/predef.git boost_predef

# LBFGSpp + Eigen
git clone --depth 1 https://github.com/yixuan/LBFGSpp.git lbfgspp
git clone --depth 1 --branch 5.0.0 https://gitlab.com/libeigen/eigen.git eigen

# Catch2 v3 (for tests)
git clone --depth 1 --branch v3.7.1 https://github.com/catchorg/Catch2.git catch2
```

Note: After cloning, remove `.git` directories from all vendored deps to avoid submodule confusion:
```bash
find third_party -name ".git" -type d -exec rm -rf {} + 2>/dev/null
```

**Step 3: Create CMakeLists.txt**

Port from `~/projects/NukeX/CMakeLists.txt` with these changes:
- Remove all ONNX-related conditionals and sources
- Remove all segmentation-related sources
- Add include paths for all `third_party/` headers
- Add `XTENSOR_USE_XSIMD` define
- Version 3.0.0
- Boost.Math headers require include paths for all boost sub-repos:
  ```cmake
  foreach(BOOST_LIB math config assert throw_exception core type_traits
                     static_assert mp11 integer lexical_cast predef)
      include_directories(third_party/boost_${BOOST_LIB}/include)
  endforeach()
  ```

Reference: `~/projects/NukeX/CMakeLists.txt` for PCL flags, platform detection, link targets.

**Step 4: Create Makefile**

Port from `~/projects/NukeX/Makefile` with same changes as CMake. Remove ONNX targets. Add third_party include paths.

**Step 5: Commit**

```bash
git add -A
git commit -m "feat: project scaffolding with vendored dependencies

- CMakeLists.txt and Makefile adapted from v2
- Vendored: xtensor, xsimd, xtl, Boost.Math, LBFGSpp, Eigen 5, Catch2
- Project CLAUDE.md with build instructions and conventions"
```

---

### Task 1.2: Module entry point and process registration

**Files:**
- Create: `src/NukeXModule.h`
- Create: `src/NukeXModule.cpp`
- Create: `src/NukeXStackProcess.h`
- Create: `src/NukeXStackProcess.cpp`
- Create: `src/NukeXStretchProcess.h`
- Create: `src/NukeXStretchProcess.cpp`

**Step 1: Write NukeXModule**

Port directly from `~/projects/NukeX/src/NukeXModule.h` and `.cpp`:
- Change version to 3.0.0.1
- Change release date to current
- `InstallPixInsightModule()` registers `NukeXStackProcess` and `NukeXStretchProcess`
- Use `pcl_template` to verify MetaModule pattern

**Step 2: Write NukeXStackProcess**

Port from `~/projects/NukeX/src/NukeXStackProcess.h` and `.cpp`:
- ID: "NukeXStack"
- Category: "ImageIntegration"
- Remove all ONNX/ML parameter registrations
- Keep: input frames table, outlier threshold, quality weighting params
- Add: FWHMWeight, EccentricityWeight, SkyBackgroundWeight, HFRWeight, AltitudeWeight
- Add: GenerateProvenance, GenerateDistMetadata booleans
- Use `pcl_class_info MetaProcess` to verify virtual method signatures

**Step 3: Write NukeXStretchProcess**

Port from `~/projects/NukeX/src/NukeXProcess.h` and `.cpp`:
- ID: "NukeXStretch"
- Category: "IntensityTransformations"
- Remove all segmentation/region parameters
- Keep: algorithm enum, contrast, saturation, black/white point, gamma, strength

**Step 4: Verify build compiles (stub Instance/Interface)**

Create minimal stubs for `NukeXStackInstance`, `NukeXStretchInstance`, `NukeXStackInterface`, `NukeXStretchInterface` that compile but have empty implementations:

```cpp
// NukeXStackInstance.h — minimal stub
class NukeXStackInstance : public ProcessImplementation {
public:
    NukeXStackInstance(const MetaProcess*);
    bool CanExecuteGlobal() const override { return true; }
    bool ExecuteGlobal() override { return false; } // stub
    void* LockParameter(const MetaParameter*, size_t) override;
    bool AllocateParameter(size_t, const MetaParameter*, size_t) override;
    size_t ParameterLength(const MetaParameter*, size_t) const override;
};
```

```bash
mkdir build && cd build && cmake .. && make -j$(nproc)
```

Expected: compiles and links to `NukeX-pxm.so`

**Step 5: Commit**

```bash
git add src/NukeX*.h src/NukeX*.cpp
git commit -m "feat: PCL module entry point and process registration

- NukeXModule v3.0.0 with two processes
- NukeXStackProcess (ImageIntegration category)
- NukeXStretchProcess (IntensityTransformations category)
- Stub Instance/Interface classes for compilation"
```

---

### Task 1.3: Stack parameters

**Files:**
- Create: `src/NukeXStackParameters.h`
- Create: `src/NukeXStackParameters.cpp`

**Step 1: Define parameter classes**

Port pattern from `~/projects/NukeX/src/NukeXStackParameters.h/.cpp`:

```cpp
// Parameters to define:
// MetaTable:
//   NXSInputFrames — table of input frame paths
//     NXSInputFramePath — String column
//     NXSInputFrameEnabled — Boolean column

// MetaEnumeration:
//   NXSQualityWeightMode — None, FWHMOnly, Full (default: Full)

// MetaFloat:
//   NXSOutlierSigmaThreshold — range [1.0, 10.0], default 3.0, precision 1
//   NXSFWHMWeight — range [0.0, 10.0], default 1.0, precision 2
//   NXSEccentricityWeight — range [0.0, 10.0], default 1.0, precision 2
//   NXSSkyBackgroundWeight — range [0.0, 10.0], default 0.5, precision 2
//   NXSHFRWeight — range [0.0, 10.0], default 1.0, precision 2
//   NXSAltitudeWeight — range [0.0, 10.0], default 0.3, precision 2

// MetaBoolean:
//   NXSGenerateProvenance — default true
//   NXSGenerateDistMetadata — default false
//   NXSEnableQualityWeighting — default true
```

Use `pcl_class_info MetaFloat` and `pcl_class_info MetaTable` to verify parameter class APIs.

**Step 2: Verify build**

```bash
cd build && cmake .. && make -j$(nproc)
```

**Step 3: Commit**

```bash
git add src/NukeXStackParameters.*
git commit -m "feat: stack process parameters

- Input frames table (path + enabled)
- Quality weight mode enum (None/FWHMOnly/Full)
- Per-attribute weight floats (FWHM, eccentricity, sky, HFR, altitude)
- Outlier sigma threshold
- Provenance and distribution metadata output toggles"
```

---

### Task 1.4: SubCube class — 3D tensor with RAII

**Files:**
- Create: `src/engine/SubCube.h`
- Create: `src/engine/SubCube.cpp`
- Create: `tests/unit/test_sub_cube.cpp`
- Create: `tests/CMakeLists.txt`

**Step 1: Write failing tests**

```cpp
// tests/unit/test_sub_cube.cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "engine/SubCube.h"

TEST_CASE("SubCube allocation", "[subcube]") {
    SECTION("small cube allocates successfully") {
        nukex::SubCube cube(10, 64, 64);  // 10 subs, 64x64
        REQUIRE(cube.numSubs() == 10);
        REQUIRE(cube.height() == 64);
        REQUIRE(cube.width() == 64);
    }

    SECTION("pixel values are zero-initialized") {
        nukex::SubCube cube(5, 8, 8);
        auto col = cube.zColumn(4, 4);
        REQUIRE(col.size() == 5);
        for (size_t z = 0; z < 5; z++)
            REQUIRE(col[z] == 0.0f);
    }
}

TEST_CASE("SubCube Z-column extraction", "[subcube]") {
    nukex::SubCube cube(3, 4, 4);

    // Write known values at (2, 3) through Z
    cube.setPixel(0, 2, 3, 1.0f);
    cube.setPixel(1, 2, 3, 2.0f);
    cube.setPixel(2, 2, 3, 3.0f);

    auto col = cube.zColumn(2, 3);
    REQUIRE(col.size() == 3);
    REQUIRE(col[0] == Catch::Approx(1.0f));
    REQUIRE(col[1] == Catch::Approx(2.0f));
    REQUIRE(col[2] == Catch::Approx(3.0f));
}

TEST_CASE("SubCube Z-column is contiguous in memory", "[subcube]") {
    nukex::SubCube cube(100, 16, 16);
    cube.setPixel(0, 8, 8, 42.0f);
    cube.setPixel(1, 8, 8, 43.0f);

    const float* ptr = cube.zColumnPtr(8, 8);
    REQUIRE(ptr[0] == Catch::Approx(42.0f));
    REQUIRE(ptr[1] == Catch::Approx(43.0f));
    // Verify contiguity: stride should be 1 float
    REQUIRE((&ptr[1] - &ptr[0]) == 1);
}

TEST_CASE("SubCube metadata", "[subcube]") {
    nukex::SubCube cube(3, 8, 8);

    nukex::SubMetadata meta;
    meta.fwhm = 2.5;
    meta.eccentricity = 0.3;
    meta.skyBackground = 0.15;
    meta.hfr = 1.8;
    meta.altitude = 45.0;
    meta.exposure = 300.0;
    meta.gain = 100.0;
    meta.ccdTemp = -10.0;
    meta.filter = "Ha";

    cube.setMetadata(0, meta);
    REQUIRE(cube.metadata(0).fwhm == Catch::Approx(2.5));
    REQUIRE(cube.metadata(0).filter == "Ha");
}

TEST_CASE("SubCube provenance map", "[subcube]") {
    nukex::SubCube cube(10, 8, 8);
    cube.setProvenance(3, 5, 7);  // pixel (3,5) selected from sub 7
    REQUIRE(cube.provenance(3, 5) == 7);
}
```

**Step 2: Run tests to verify they fail**

```bash
cd build && cmake .. && make -j$(nproc) && ctest --output-on-failure
```

Expected: compilation error — `SubCube.h` doesn't exist yet.

**Step 3: Implement SubCube**

```cpp
// src/engine/SubCube.h
#pragma once
#include <xtensor/xtensor.hpp>
#include <xtensor/xview.hpp>
#include <vector>
#include <string>
#include <stdexcept>
#include <pcl/Console.h>

namespace nukex {

struct SubMetadata {
    double fwhm = 0;
    double eccentricity = 0;
    double skyBackground = 0;
    double hfr = 0;
    double altitude = 0;
    double exposure = 0;
    double gain = 0;
    double ccdTemp = 0;
    std::string object;
    std::string filter;
};

class SubCube {
public:
    SubCube(size_t nSubs, size_t height, size_t width)
    try : m_cube(xt::zeros<float>({nSubs, height, width})),
          m_provenance(xt::zeros<uint32_t>({height, width})),
          m_distType(xt::zeros<uint8_t>({height, width})),
          m_metadata(nSubs),
          m_nSubs(nSubs), m_height(height), m_width(width)
    {
    }
    catch (const std::bad_alloc&) {
        double gb = double(nSubs * height * width * sizeof(float))
                    / (1024.0 * 1024.0 * 1024.0);
        pcl::Console().CriticalLn(
            "NukeX: Failed to allocate sub cube ("
            + pcl::String(pcl::String::ToFormattedString(gb, 1)) + " GB)");
        throw;
    }

    size_t numSubs()  const { return m_nSubs; }
    size_t height()   const { return m_height; }
    size_t width()    const { return m_width; }

    // Z-column access — returns view of contiguous memory
    auto zColumn(size_t y, size_t x) const {
        return xt::view(m_cube, xt::all(), y, x);
    }

    // Raw pointer to contiguous Z-column for SIMD/manual loops
    const float* zColumnPtr(size_t y, size_t x) const {
        return &m_cube(0, y, x);
    }

    float* zColumnPtr(size_t y, size_t x) {
        return &m_cube(0, y, x);
    }

    // Pixel access
    float pixel(size_t z, size_t y, size_t x) const {
        return m_cube(z, y, x);
    }

    void setPixel(size_t z, size_t y, size_t x, float val) {
        m_cube(z, y, x) = val;
    }

    // Write an entire sub slice
    void setSub(size_t z, const float* data, size_t count) {
        auto slice = xt::view(m_cube, z, xt::all(), xt::all());
        std::copy(data, data + count, slice.begin());
    }

    // Metadata
    const SubMetadata& metadata(size_t z) const { return m_metadata.at(z); }
    void setMetadata(size_t z, const SubMetadata& meta) { m_metadata.at(z) = meta; }

    // Provenance
    uint32_t provenance(size_t y, size_t x) const { return m_provenance(y, x); }
    void setProvenance(size_t y, size_t x, uint32_t z) { m_provenance(y, x) = z; }

    // Distribution type
    uint8_t distType(size_t y, size_t x) const { return m_distType(y, x); }
    void setDistType(size_t y, size_t x, uint8_t t) { m_distType(y, x) = t; }

    // Direct tensor access for bulk operations
    xt::xtensor<float, 3>&       cube()       { return m_cube; }
    const xt::xtensor<float, 3>& cube() const { return m_cube; }
    xt::xtensor<uint32_t, 2>&       provenanceMap()       { return m_provenance; }
    xt::xtensor<uint8_t, 2>&        distTypeMap()         { return m_distType; }

private:
    xt::xtensor<float, 3>     m_cube;
    xt::xtensor<uint32_t, 2>  m_provenance;
    xt::xtensor<uint8_t, 2>   m_distType;
    std::vector<SubMetadata>  m_metadata;
    size_t m_nSubs, m_height, m_width;
};

} // namespace nukex
```

**Step 4: Create test CMakeLists.txt**

```cmake
# tests/CMakeLists.txt
add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/catch2 catch2_build)

# Unit tests
add_executable(nukex_tests
    unit/test_sub_cube.cpp
)
target_include_directories(nukex_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/xtensor/include
    ${CMAKE_SOURCE_DIR}/third_party/xtl/include
    ${CMAKE_SOURCE_DIR}/third_party/xsimd/include
)
target_link_libraries(nukex_tests PRIVATE Catch2::Catch2WithMain)
target_compile_definitions(nukex_tests PRIVATE XTENSOR_USE_XSIMD)

include(CTest)
include(Catch)
catch_discover_tests(nukex_tests)
```

Note: Unit tests for the engine layer should NOT require linking PCL. The SubCube class uses `pcl::Console` only for error reporting — either mock it or conditionally compile that path. For tests, define a `NUKEX_TESTING` macro and provide a fallback that writes to stderr.

**Step 5: Run tests, verify they pass**

```bash
cd build && cmake .. && make -j$(nproc) && ctest --output-on-failure
```

Expected: all SubCube tests PASS.

**Step 6: Commit**

```bash
git add src/engine/SubCube.h tests/
git commit -m "feat: SubCube class with xtensor 3D storage and RAII

- xt::xtensor<float, 3> with shape (N_subs, H, W) for contiguous Z-access
- Provenance map (uint32_t) and distribution type map (uint8_t)
- Per-sub metadata storage
- std::bad_alloc guard with diagnostic logging
- Unit tests for allocation, Z-column extraction, contiguity, metadata"
```

---

### Task 1.5: FITS frame loading into SubCube

**Files:**
- Create: `src/engine/FrameLoader.h`
- Create: `src/engine/FrameLoader.cpp`
- Create: `tests/unit/test_frame_loader.cpp`
- Create: `tests/mock_data/generate_mock_subs.cpp`

**Step 1: Write mock data generator**

A standalone program that generates synthetic FITS files with known pixel values and FITS headers:

```cpp
// tests/mock_data/generate_mock_subs.cpp
// Uses PCL FileFormat API to write FITS files with:
// - Known sky background (configurable)
// - Poisson noise
// - Injected Gaussian stars at known positions
// - Injected satellite trails (linear features) in specific subs
// - Injected cosmic rays (single-pixel spikes) in specific subs
// - FITS keywords: FWHM, ECCENTRICITY, SKYBACK, HFR, ALTITUDE, EXPTIME, GAIN, CCD-TEMP, OBJECT, FILTER
```

This is a test utility — it writes N FITS files to a temp directory for integration testing.

**Step 2: Write FrameLoader**

Uses PCL's `FileFormat` / `FileFormatInstance` API (reference `~/projects/NukeX/src/engine/FrameStreamer.h` for the pattern):

```cpp
// src/engine/FrameLoader.h
#pragma once
#include "SubCube.h"
#include <pcl/FileFormat.h>
#include <pcl/FileFormatInstance.h>
#include <pcl/FITSHeaderKeyword.h>
#include <vector>
#include <string>

namespace nukex {

class FrameLoader {
public:
    struct FramePath {
        pcl::String path;
        bool enabled = true;
    };

    // Load all enabled frames into a SubCube
    // Validates: all frames same dimensions, extracts FITS keywords
    // Throws on dimension mismatch or I/O error
    static SubCube Load(const std::vector<FramePath>& frames,
                        pcl::StandardStatus& status);

private:
    static SubMetadata ExtractMetadata(const pcl::FITSKeywordArray& keywords);
    static double GetKeywordValue(const pcl::FITSKeywordArray& keywords,
                                  const pcl::IsoString& name,
                                  double defaultValue);
};

} // namespace nukex
```

Key implementation details:
- First pass: open first frame to get dimensions (width, height)
- Allocate SubCube(N_enabled, height, width)
- Second pass: read each frame into the cube via `ReadImage()` then copy to `cube.setSub(z, ...)`
- Extract FITS keywords and populate SubMetadata per Z
- Progress reporting via StandardStatus

Use `pcl_class_info FileFormatInstance` and `pcl_class_info FITSHeaderKeyword` for API accuracy.

**Step 3: Write unit tests**

```cpp
// tests/unit/test_frame_loader.cpp
// Integration test using mock data generator
// - Generate 5 mock subs with known values
// - Load via FrameLoader
// - Verify cube dimensions match
// - Verify pixel values match expected
// - Verify metadata extracted correctly
// - Test dimension mismatch detection (should throw)
// - Test empty frame list (should throw)
```

**Step 4: Run tests, verify pass**

**Step 5: Commit**

```bash
git add src/engine/FrameLoader.* tests/
git commit -m "feat: FrameLoader reads FITS frames into SubCube

- Loads all enabled frames with dimension validation
- Extracts FITS keywords into SubMetadata (FWHM, eccentricity, etc.)
- Progress reporting via StandardStatus
- Mock data generator for test FITS files"
```

---

### Task 1.6: Quality weights from metadata

**Files:**
- Create: `src/engine/QualityWeights.h`
- Create: `src/engine/QualityWeights.cpp`
- Create: `tests/unit/test_quality_weights.cpp`

**Step 1: Write failing tests**

```cpp
// tests/unit/test_quality_weights.cpp
TEST_CASE("Quality weights computation", "[weights]") {
    std::vector<nukex::SubMetadata> metas(3);
    metas[0].fwhm = 2.0; metas[1].fwhm = 3.0; metas[2].fwhm = 5.0;
    metas[0].eccentricity = 0.1; metas[1].eccentricity = 0.2; metas[2].eccentricity = 0.5;

    nukex::WeightConfig cfg;
    cfg.fwhmWeight = 1.0;
    cfg.eccentricityWeight = 1.0;

    auto weights = nukex::ComputeQualityWeights(metas, cfg);

    SECTION("weights are normalized to sum to 1") {
        double sum = 0;
        for (auto w : weights) sum += w;
        REQUIRE(sum == Catch::Approx(1.0));
    }

    SECTION("better subs get higher weights") {
        REQUIRE(weights[0] > weights[1]);
        REQUIRE(weights[1] > weights[2]);
    }

    SECTION("all-equal metadata gives equal weights") {
        for (auto& m : metas) { m.fwhm = 2.0; m.eccentricity = 0.1; }
        auto eq = nukex::ComputeQualityWeights(metas, cfg);
        REQUIRE(eq[0] == Catch::Approx(eq[1]));
        REQUIRE(eq[1] == Catch::Approx(eq[2]));
    }
}
```

**Step 2: Implement**

```cpp
// src/engine/QualityWeights.h
namespace nukex {

struct WeightConfig {
    double fwhmWeight = 1.0;
    double eccentricityWeight = 1.0;
    double skyBackgroundWeight = 0.5;
    double hfrWeight = 1.0;
    double altitudeWeight = 0.3;
};

// Returns normalized weight vector (sums to 1.0)
// Higher weight = better sub. Inverts "bad" metrics (FWHM, eccentricity, sky).
std::vector<double> ComputeQualityWeights(
    const std::vector<SubMetadata>& metadata,
    const WeightConfig& config);

} // namespace nukex
```

Algorithm:
- For each inverse metric (FWHM, eccentricity, sky): score = 1.0 / (1.0 + value)
- For altitude: score = sin(altitude * pi/180) (airmass proxy)
- Weighted sum of scores per sub, then normalize to sum=1

**Step 3: Run tests, verify pass**

**Step 4: Commit**

```bash
git add src/engine/QualityWeights.* tests/unit/test_quality_weights.cpp
git commit -m "feat: quality weight computation from FITS metadata

- Inverse weighting for FWHM, eccentricity, sky background
- Airmass-based altitude scoring
- Configurable per-attribute weight multipliers
- Normalized output (sum to 1.0)"
```

---

## Phase 2 — Statistical Engine

### Task 2.1: Numerical utilities

**Files:**
- Create: `src/engine/NumericalUtils.h`
- Create: `tests/unit/test_numerical_utils.cpp`

**Step 1: Write failing tests**

```cpp
TEST_CASE("log-sum-exp", "[numerical]") {
    SECTION("basic computation") {
        std::vector<double> vals = {1.0, 2.0, 3.0};
        double result = nukex::logSumExp(vals);
        double expected = std::log(std::exp(1.0) + std::exp(2.0) + std::exp(3.0));
        REQUIRE(result == Catch::Approx(expected).epsilon(1e-12));
    }

    SECTION("large values don't overflow") {
        std::vector<double> vals = {1000.0, 1001.0, 1002.0};
        double result = nukex::logSumExp(vals);
        REQUIRE(std::isfinite(result));
    }

    SECTION("large negative values don't underflow") {
        std::vector<double> vals = {-1000.0, -1001.0, -1002.0};
        double result = nukex::logSumExp(vals);
        REQUIRE(std::isfinite(result));
    }
}

TEST_CASE("AIC/BIC scoring", "[numerical]") {
    double logL = -100.0;
    int k = 2;
    int n = 100;

    REQUIRE(nukex::aic(logL, k) == Catch::Approx(204.0));
    REQUIRE(nukex::bic(logL, k, n) == Catch::Approx(200.0 + 2*std::log(100.0)));
    REQUIRE(nukex::aicc(logL, k, n) > nukex::aic(logL, k));
}
```

**Step 2: Implement header-only utilities**

```cpp
// src/engine/NumericalUtils.h
#pragma once
#include <cmath>
#include <vector>
#include <algorithm>
#include <numeric>

namespace nukex {

inline double logSumExp(const std::vector<double>& vals) {
    double maxVal = *std::max_element(vals.begin(), vals.end());
    double sum = 0.0;
    for (double v : vals)
        sum += std::exp(v - maxVal);
    return maxVal + std::log(sum);
}

inline double aic(double logLikelihood, int k) {
    return 2.0 * k - 2.0 * logLikelihood;
}

inline double bic(double logLikelihood, int k, int n) {
    return k * std::log(static_cast<double>(n)) - 2.0 * logLikelihood;
}

inline double aicc(double logLikelihood, int k, int n) {
    double a = aic(logLikelihood, k);
    return a + (2.0 * k * (k + 1.0)) / (n - k - 1.0);
}

// Kahan compensated summation
inline double kahanSum(const double* data, size_t n) {
    double sum = 0.0, c = 0.0;
    for (size_t i = 0; i < n; i++) {
        double y = data[i] - c;
        double t = sum + y;
        c = (t - sum) - y;
        sum = t;
    }
    return sum;
}

} // namespace nukex
```

**Step 3: Run tests, verify pass**

**Step 4: Commit**

```bash
git add src/engine/NumericalUtils.h tests/unit/test_numerical_utils.cpp
git commit -m "feat: numerical utilities (log-sum-exp, AIC/BIC, Kahan sum)"
```

---

### Task 2.2: Distribution fitting — Gaussian and Poisson (closed-form)

**Files:**
- Create: `src/engine/DistributionFitter.h`
- Create: `src/engine/DistributionFitter.cpp`
- Create: `tests/unit/test_distribution_fitter.cpp`

**Step 1: Write failing tests**

```cpp
TEST_CASE("Gaussian MLE", "[distributions]") {
    // Known Gaussian: mu=5.0, sigma=1.0
    std::vector<double> data = {4.0, 4.5, 5.0, 5.5, 6.0, 4.8, 5.2, 5.1, 4.9, 5.0};

    auto result = nukex::fitGaussian(data);
    REQUIRE(result.logLikelihood < 0);  // log-likelihood is negative
    REQUIRE(result.params.mu == Catch::Approx(5.0).margin(0.1));
    REQUIRE(result.params.sigma > 0);
    REQUIRE(result.k == 2);  // 2 parameters
}

TEST_CASE("Poisson MLE", "[distributions]") {
    // Known Poisson: lambda=3.0
    std::vector<double> data = {2, 3, 4, 3, 2, 3, 4, 3, 2, 5};

    auto result = nukex::fitPoisson(data);
    REQUIRE(result.params.lambda == Catch::Approx(3.1).margin(0.2));
    REQUIRE(result.k == 1);
}

TEST_CASE("AIC selects correct model", "[distributions]") {
    // Gaussian data should prefer Gaussian model
    // Use std::normal_distribution to generate N(5.0, 1.0) samples
    std::mt19937 rng(42);
    std::normal_distribution<double> dist(5.0, 1.0);
    std::vector<double> data(200);
    for (auto& d : data) d = dist(rng);

    auto gauss = nukex::fitGaussian(data);
    auto poisson = nukex::fitPoisson(data);

    double aicGauss = nukex::aic(gauss.logLikelihood, gauss.k);
    double aicPoisson = nukex::aic(poisson.logLikelihood, poisson.k);
    REQUIRE(aicGauss < aicPoisson);  // Gaussian should win
}
```

**Step 2: Implement**

```cpp
// src/engine/DistributionFitter.h
#pragma once
#include <vector>
#include <cmath>
#include <boost/math/distributions/normal.hpp>
#include <boost/math/distributions/poisson.hpp>

namespace nukex {

enum class DistributionType : uint8_t {
    Gaussian = 0,
    Poisson = 1,
    SkewNormal = 2,
    Bimodal = 3
};

struct GaussianParams { double mu, sigma; };
struct PoissonParams { double lambda; };
struct SkewNormalParams { double xi, omega, alpha; };
struct BimodalParams { double mu1, sigma1, mu2, sigma2, weight; };

struct FitResult {
    DistributionType type;
    double logLikelihood;
    int k;  // number of parameters
    union {
        GaussianParams gaussian;
        PoissonParams poisson;
        SkewNormalParams skewNormal;
        BimodalParams bimodal;
    } params;
};

FitResult fitGaussian(const std::vector<double>& data);
FitResult fitPoisson(const std::vector<double>& data);

} // namespace nukex
```

Implementation:
- Gaussian MLE: `mu = mean(data)`, `sigma = sqrt(variance(data))`, `logL = sum(log(pdf(normal(mu, sigma), x_i)))`
- Poisson MLE: `lambda = mean(data)`, `logL = sum(log(pdf(poisson(lambda), round(x_i))))`
- Use Boost.Math `pdf()` for log-likelihood computation

**Step 3: Run tests, verify pass**

**Step 4: Commit**

```bash
git add src/engine/DistributionFitter.* tests/unit/test_distribution_fitter.cpp
git commit -m "feat: Gaussian and Poisson MLE with closed-form estimators"
```

---

### Task 2.3: Distribution fitting — Skew-Normal (LBFGSpp)

**Files:**
- Modify: `src/engine/DistributionFitter.h` (add fitSkewNormal)
- Modify: `src/engine/DistributionFitter.cpp`
- Modify: `tests/unit/test_distribution_fitter.cpp`

**Step 1: Write failing tests**

```cpp
TEST_CASE("Skew-Normal MLE", "[distributions]") {
    // Generate skewed data using rejection sampling or known transform
    std::mt19937 rng(42);
    std::vector<double> data(300);
    // Simulate skewed data: alpha=5 gives strong positive skew
    for (auto& d : data) {
        // Owen's method or simple simulation
        std::normal_distribution<double> n1(0, 1), n2(0, 1);
        double u = n1(rng), v = n2(rng);
        double alpha = 5.0;
        d = (alpha * std::abs(u) + v) / std::sqrt(1.0 + alpha*alpha);
        d = d * 2.0 + 3.0;  // scale and shift: omega=2, xi=3
    }

    auto result = nukex::fitSkewNormal(data);
    REQUIRE(result.type == nukex::DistributionType::SkewNormal);
    REQUIRE(result.k == 3);
    REQUIRE(result.params.skewNormal.alpha > 0);  // should detect positive skew
}

TEST_CASE("Skew-Normal reduces to Gaussian for symmetric data", "[distributions]") {
    std::mt19937 rng(42);
    std::normal_distribution<double> dist(5.0, 1.0);
    std::vector<double> data(300);
    for (auto& d : data) d = dist(rng);

    auto result = nukex::fitSkewNormal(data);
    REQUIRE(std::abs(result.params.skewNormal.alpha) < 2.0);
}
```

**Step 2: Implement**

Uses LBFGSpp to minimize negative log-likelihood:

```cpp
FitResult fitSkewNormal(const std::vector<double>& data) {
    // Initial estimates from sample moments
    double mu = mean(data);
    double sigma = stddev(data);
    double skew = skewness(data);
    double alpha_init = skew;  // rough starting point

    // L-BFGS minimization of -logL
    // Parameters: [xi, log(omega), alpha] (log-transform omega to keep positive)
    Eigen::VectorXd x(3);
    x << mu, std::log(sigma), alpha_init;

    LBFGSpp::LBFGSParam<double> param;
    param.max_iterations = 50;
    param.epsilon = 1e-6;
    LBFGSpp::LBFGSSolver<double> solver(param);

    auto objective = [&data](const Eigen::VectorXd& x, Eigen::VectorXd& grad) -> double {
        double xi = x[0];
        double omega = std::exp(x[1]);
        double alpha = x[2];

        boost::math::skew_normal_distribution<double> dist(xi, omega, alpha);

        double negLogL = 0;
        grad.setZero();

        for (double val : data) {
            double p = boost::math::pdf(dist, val);
            if (p < 1e-300) p = 1e-300;  // prevent log(0)
            negLogL -= std::log(p);
        }

        // Numerical gradient (analytic gradient complex for skew-normal)
        // For production, derive analytic gradient
        const double h = 1e-7;
        for (int i = 0; i < 3; i++) {
            Eigen::VectorXd xp = x, xm = x;
            xp[i] += h; xm[i] -= h;
            // ... compute grad[i] via central difference
        }

        return negLogL;
    };

    double negLogL;
    solver.minimize(objective, x, negLogL);

    FitResult result;
    result.type = DistributionType::SkewNormal;
    result.logLikelihood = -negLogL;
    result.k = 3;
    result.params.skewNormal = {x[0], std::exp(x[1]), x[2]};
    return result;
}
```

Note: Start with numerical gradient (central difference). Optimize to analytic gradient in Phase 2 if profiling shows it's the bottleneck.

**Step 3: Run tests, verify pass**

**Step 4: Commit**

```bash
git add src/engine/DistributionFitter.* tests/unit/test_distribution_fitter.cpp
git commit -m "feat: Skew-Normal MLE via LBFGSpp L-BFGS optimizer"
```

---

### Task 2.4: Distribution fitting — Bimodal mixture (EM)

**Files:**
- Create: `src/engine/GaussianMixEM.h`
- Create: `src/engine/GaussianMixEM.cpp`
- Modify: `src/engine/DistributionFitter.h/.cpp` (add fitBimodal)
- Create: `tests/unit/test_gaussian_mix_em.cpp`

**Step 1: Write failing tests**

```cpp
TEST_CASE("2-component Gaussian EM", "[em]") {
    // Generate bimodal data: N(2,0.5) and N(6,0.5), 50/50 mix
    std::mt19937 rng(42);
    std::vector<double> data;
    std::normal_distribution<double> d1(2.0, 0.5), d2(6.0, 0.5);
    for (int i = 0; i < 200; i++) {
        data.push_back(d1(rng));
        data.push_back(d2(rng));
    }
    std::shuffle(data.begin(), data.end(), rng);

    auto result = nukex::fitGaussianMixture2(data);

    // Should find two well-separated components
    double lo = std::min(result.mu1, result.mu2);
    double hi = std::max(result.mu1, result.mu2);
    REQUIRE(lo == Catch::Approx(2.0).margin(0.3));
    REQUIRE(hi == Catch::Approx(6.0).margin(0.3));
    REQUIRE(result.weight > 0.3);
    REQUIRE(result.weight < 0.7);
    REQUIRE(result.converged);
}

TEST_CASE("EM on unimodal data returns near-degenerate mixture", "[em]") {
    std::mt19937 rng(42);
    std::normal_distribution<double> d(5.0, 1.0);
    std::vector<double> data(200);
    for (auto& v : data) v = d(rng);

    auto result = nukex::fitGaussianMixture2(data);
    // Both means should be close together
    REQUIRE(std::abs(result.mu1 - result.mu2) < 2.0);
}
```

**Step 2: Implement 2-component EM**

```cpp
// src/engine/GaussianMixEM.h
namespace nukex {

struct GaussianMixResult {
    double mu1, sigma1, mu2, sigma2, weight;
    double logLikelihood;
    bool converged;
    int iterations;
};

GaussianMixResult fitGaussianMixture2(
    const std::vector<double>& data,
    int maxIterations = 100,
    double convergenceThreshold = 1e-6);

} // namespace nukex
```

Algorithm:
1. Initialize: sort data, split at median for initial means; sigma = stddev of each half; weight = 0.5
2. E-step: compute responsibilities using log-sum-exp
3. M-step: update means, variances, weight from responsibilities
4. Check convergence (log-likelihood delta < threshold)
5. Floor variance at 1e-10

**Step 3: Wire into DistributionFitter as `fitBimodal()`**

**Step 4: Run tests, verify pass**

**Step 5: Commit**

```bash
git add src/engine/GaussianMixEM.* src/engine/DistributionFitter.* tests/unit/test_gaussian_mix_em.cpp
git commit -m "feat: 2-component Gaussian EM for bimodal mixture fitting"
```

---

### Task 2.5: Outlier detection

**Files:**
- Create: `src/engine/OutlierDetector.h`
- Create: `src/engine/OutlierDetector.cpp`
- Create: `tests/unit/test_outlier_detector.cpp`

**Step 1: Write failing tests**

```cpp
TEST_CASE("Generalized ESD", "[outlier]") {
    std::vector<double> data = {1, 2, 2, 3, 3, 3, 4, 4, 5, 100};
    auto outliers = nukex::detectOutliersESD(data, 3);  // max 3 outliers
    REQUIRE(std::find(outliers.begin(), outliers.end(), 9) != outliers.end());
}

TEST_CASE("Chauvenet criterion", "[outlier]") {
    std::vector<double> data = {1, 2, 2, 3, 3, 3, 4, 4, 5, 100};
    auto outliers = nukex::detectOutliersChauvenet(data);
    REQUIRE(!outliers.empty());
    REQUIRE(std::find(outliers.begin(), outliers.end(), 9) != outliers.end());
}

TEST_CASE("No false positives on clean data", "[outlier]") {
    std::mt19937 rng(42);
    std::normal_distribution<double> dist(5.0, 1.0);
    std::vector<double> data(100);
    for (auto& d : data) d = dist(rng);
    auto outliers = nukex::detectOutliersESD(data, 5);
    REQUIRE(outliers.size() <= 2);  // maybe 1-2 at 3-sigma edges
}
```

**Step 2: Implement**

- **Grubbs' test**: G = max|x_i - mean|/s, compare against `boost::math::students_t` quantile
- **Generalized ESD**: iterative Grubbs' up to max_outliers
- **Chauvenet**: P(|x - mean| >= |x_i - mean|) * n < 0.5, using `boost::math::normal` CDF

**Step 3: Run tests, verify pass**

**Step 4: Commit**

```bash
git add src/engine/OutlierDetector.* tests/unit/test_outlier_detector.cpp
git commit -m "feat: outlier detection (Generalized ESD, Chauvenet)"
```

---

### Task 2.6: Pixel selector — the main orchestrator

**Files:**
- Create: `src/engine/PixelSelector.h`
- Create: `src/engine/PixelSelector.cpp`
- Create: `tests/unit/test_pixel_selector.cpp`

**Step 1: Write failing tests**

```cpp
TEST_CASE("PixelSelector picks best Z value", "[selector]") {
    // 5 subs, 4x4 image, pixel (2,2) has a cosmic ray in sub 3
    nukex::SubCube cube(5, 4, 4);
    for (size_t z = 0; z < 5; z++)
        cube.setPixel(z, 2, 2, 100.0f);  // all same
    cube.setPixel(3, 2, 2, 10000.0f);    // cosmic ray in sub 3

    std::vector<double> weights(5, 0.2);  // equal weights

    nukex::PixelSelector selector;
    selector.processPixel(cube, 2, 2, weights);

    REQUIRE(cube.provenance(2, 2) != 3);  // should NOT select the cosmic ray sub
}

TEST_CASE("PixelSelector processes full image", "[selector]") {
    nukex::SubCube cube(10, 8, 8);
    // Fill with Gaussian noise per pixel
    std::mt19937 rng(42);
    std::normal_distribution<double> noise(100.0, 5.0);
    for (size_t z = 0; z < 10; z++)
        for (size_t y = 0; y < 8; y++)
            for (size_t x = 0; x < 8; x++)
                cube.setPixel(z, y, x, static_cast<float>(noise(rng)));

    std::vector<double> weights(10, 0.1);
    nukex::PixelSelector selector;
    auto result = selector.processImage(cube, weights);

    REQUIRE(result.width() == 8);
    REQUIRE(result.height() == 8);
    // Every pixel should have a provenance entry
    for (size_t y = 0; y < 8; y++)
        for (size_t x = 0; x < 8; x++)
            REQUIRE(cube.provenance(y, x) < 10);
}
```

**Step 2: Implement**

```cpp
// src/engine/PixelSelector.h
namespace nukex {

class PixelSelector {
public:
    struct Config {
        double outlierSigmaThreshold = 3.0;
    };

    // Process single pixel: fit distributions, select best Z
    void processPixel(SubCube& cube, size_t y, size_t x,
                      const std::vector<double>& weights);

    // Process full image with threading
    pcl::Image processImage(SubCube& cube,
                            const std::vector<double>& weights,
                            pcl::StandardStatus* status = nullptr);

private:
    Config m_config;

    // Per-pixel pipeline:
    // 1. Extract Z-column, promote to double
    // 2. Apply quality weights
    // 3. Detect and mask outliers
    // 4. Fit all 4 distributions on non-outlier data
    // 5. Score with AIC, pick best model
    // 6. Select highest-probability Z value under best model
    // 7. Write provenance and distType
    struct PixelResult {
        uint32_t selectedZ;
        DistributionType bestModel;
        float selectedValue;
    };

    PixelResult selectBestZ(const std::vector<double>& zValues,
                            const std::vector<double>& weights,
                            size_t nSubs);
};

} // namespace nukex
```

Threading: use PCL's `Thread` or OpenMP `#pragma omp parallel for` over rows. Each thread gets its own scratch buffers (pre-allocated `std::vector<double>` for Z-column and weights).

**Step 3: Run tests, verify pass**

**Step 4: Commit**

```bash
git add src/engine/PixelSelector.* tests/unit/test_pixel_selector.cpp
git commit -m "feat: PixelSelector orchestrates per-pixel distribution inference

- Fits all 4 distributions per pixel
- AIC model selection
- Outlier masking before fitting
- Parallel execution across image rows
- Provenance and distribution type output"
```

---

## Phase 3 — PCL Process Integration

### Task 3.1: NukeXStackInstance — full ExecuteGlobal

**Files:**
- Modify: `src/NukeXStackInstance.h` (replace stub)
- Create: `src/NukeXStackInstance.cpp`
- Modify: `src/NukeXStackInterface.h` (replace stub)
- Create: `src/NukeXStackInterface.cpp`

**Step 1: Implement ExecuteGlobal**

```cpp
bool NukeXStackInstance::ExecuteGlobal() {
    try {
        Console().WriteLn("NukeX v3 — Per-Pixel Statistical Inference Stacking");

        // Phase 1: Load frames
        StandardStatus status;
        StatusMonitor monitor;
        monitor.SetCallback(&status);
        monitor.Initialize("Loading sub frames...", p_inputFrames.Length());

        auto cube = FrameLoader::Load(p_inputFrames, status);

        // Phase 2: Compute quality weights
        monitor.Initialize("Computing quality weights...", 1);
        WeightConfig wcfg;
        wcfg.fwhmWeight = p_fwhmWeight;
        wcfg.eccentricityWeight = p_eccentricityWeight;
        wcfg.skyBackgroundWeight = p_skyBackgroundWeight;
        wcfg.hfrWeight = p_hfrWeight;
        wcfg.altitudeWeight = p_altitudeWeight;
        auto weights = ComputeQualityWeights(cube.metadata(), wcfg);

        // Phase 3-4: Fit distributions and select pixels
        PixelSelector selector;
        selector.setConfig({p_outlierSigmaThreshold});
        auto result = selector.processImage(cube, weights, &status);

        // Output result image
        ImageWindow window(result.Width(), result.Height(), 1,
                          32, true, false, "NukeX_stack");
        window.MainView().Image().CopyImage(result);
        window.Show();

        // Output provenance map if requested
        if (p_generateProvenance) {
            // Write provenance as separate image window
        }

        return true;
    }
    catch (const std::bad_alloc& e) {
        Console().CriticalLn("NukeX: Out of memory — " + String(e.what()));
        return false;
    }
    catch (const ProcessAborted&) {
        throw;
    }
    catch (const std::exception& e) {
        Console().CriticalLn("NukeX: " + String(e.what()));
        return false;
    }
    catch (...) {
        Console().CriticalLn("NukeX: Unknown error");
        return false;
    }
}
```

Use `pcl_class_info ImageWindow` and `pcl_class_info StandardStatus` for API accuracy.

**Step 2: Implement LockParameter, AllocateParameter, ParameterLength**

Port pattern from `~/projects/NukeX/src/NukeXStackInstance.cpp` — these handle the parameter table for the frame list.

**Step 3: Implement NukeXStackInterface**

Port UI structure from `~/projects/NukeX/src/NukeXStackInterface.h/.cpp`:
- SectionBar: Input Files (TreeBox + Add/Remove/Clear buttons)
- SectionBar: Quality Weighting (ComboBox for mode, NumericControls for weights)
- SectionBar: Outlier Detection (NumericControl for sigma threshold)
- SectionBar: Output (CheckBoxes for provenance and dist metadata)

Use `pcl_class_info SectionBar`, `pcl_class_info TreeBox`, `pcl_class_info NumericControl` for API accuracy.

**Step 4: Build and verify module loads in PixInsight**

```bash
cd build && cmake .. && make -j$(nproc)
# Install to PixInsight
cp NukeX-pxm.so /opt/PixInsight/bin/
# Launch PixInsight and verify NukeXStack appears in Process menu
```

**Step 5: Commit**

```bash
git add src/NukeXStack*
git commit -m "feat: NukeXStack process implementation and UI

- ExecuteGlobal with 4-phase pipeline
- Exception handling with graceful cleanup
- Frame list UI with Add/Remove/Clear
- Quality weight controls
- Outlier threshold control
- Provenance output toggle"
```

---

### Task 3.2: Integration test with mock data

**Files:**
- Create: `tests/integration/test_full_pipeline.cpp`

**Step 1: Write integration test**

```cpp
TEST_CASE("Full stacking pipeline", "[integration]") {
    // Generate 20 mock subs: 32x32, sky=100, noise sigma=10
    // Inject cosmic ray at (16,16) in sub 5
    // Inject satellite trail across row 8 in sub 12

    auto paths = generateMockSubs(20, 32, 32, "/tmp/nukex_test/");

    auto cube = FrameLoader::Load(paths);
    auto weights = ComputeQualityWeights(cube.metadata(), WeightConfig{});

    PixelSelector selector;
    auto result = selector.processImage(cube, weights);

    SECTION("cosmic ray rejected") {
        REQUIRE(cube.provenance(16, 16) != 5);
    }

    SECTION("satellite trail pixels rejected") {
        for (int x = 0; x < 32; x++)
            REQUIRE(cube.provenance(8, x) != 12);
    }

    SECTION("output image is reasonable") {
        // Mean pixel value should be near sky background (100)
        double sum = 0;
        for (int y = 0; y < 32; y++)
            for (int x = 0; x < 32; x++)
                sum += result.Pixel(x, y);
        double mean = sum / (32 * 32);
        REQUIRE(mean == Catch::Approx(100.0).margin(20.0));
    }
}
```

**Step 2: Run, verify pass**

**Step 3: Commit**

```bash
git add tests/integration/
git commit -m "test: full pipeline integration test with mock data

- Synthetic subs with injected artifacts
- Verifies cosmic ray and satellite trail rejection
- Validates output image quality"
```

---

## Phase 4 — Stretch Port

### Task 4.1: Port stretch algorithm base and factory

**Files:**
- Create: `src/engine/IStretchAlgorithm.h` — copy from v2, remove `AutoConfigure(RegionStatistics)` method
- Create: `src/engine/StretchLibrary.h` — copy from v2, remove MLGuided/Auto-segmentation entries

**Step 1: Copy and adapt**

Copy from `~/projects/NukeX/src/engine/IStretchAlgorithm.h` and `StretchLibrary.h`:
- Remove `#include "RegionStatistics.h"` and `#include "Segmentation.h"`
- Remove `AutoConfigure(const RegionStatistics&)` from IStretchAlgorithm
- Keep `AutoConfigure(const pcl::ImageStatistics&)` (or add it — lightweight version)
- Remove StatAuto algorithm type (re-add later in Phase 2 with distribution-based auto)
- Keep all 11 base algorithm types

**Step 2: Commit**

```bash
git add src/engine/IStretchAlgorithm.h src/engine/StretchLibrary.h
git commit -m "feat: stretch algorithm interface and factory (ported from v2, no ML)"
```

---

### Task 4.2: Port all 11 stretch algorithms

**Files:**
- Copy and adapt all 11 from `~/projects/NukeX/src/engine/algorithms/`

**Step 1: Copy each algorithm header**

For each of the 11 algorithms:
1. Copy from `~/projects/NukeX/src/engine/algorithms/`
2. Remove any `#include` of segmentation/region/ONNX headers
3. Remove any `AutoConfigure(RegionStatistics)` overrides
4. Verify no dependency on ML infrastructure

These should be clean copies — the stretch algorithms in v2 were already self-contained.

**Step 2: Write reference tests**

```cpp
TEST_CASE("MTF stretch produces expected output", "[stretch]") {
    // Create test image with known gradient
    // Apply MTF with known parameters
    // Verify output within tolerance of reference
}
// Repeat for each algorithm
```

**Step 3: Build and verify all algorithms compile**

**Step 4: Commit**

```bash
git add src/engine/algorithms/
git commit -m "feat: port 11 stretch algorithms from v2

MTF, GHS, ArcSinh, Histogram, Log, Lumpton, RNC, Photometric, OTS, SAS, VeraLux
- All ML/segmentation dependencies removed
- Self-contained implementations"
```

---

### Task 4.3: NukeXStretchInstance and Interface

**Files:**
- Modify: `src/NukeXStretchInstance.h/.cpp` (replace stubs)
- Modify: `src/NukeXStretchInterface.h/.cpp` (replace stubs)
- Create: `src/NukeXStretchParameters.h/.cpp`

**Step 1: Implement parameters**

Port from `~/projects/NukeX/src/NukeXParameters.h/.cpp`:
- Algorithm enum (11 + Auto)
- Float params: Contrast, Saturation, BlackPoint, WhitePoint, Gamma, StretchStrength
- Remove: all region enables, all segmentation booleans

**Step 2: Implement ExecuteOn**

```cpp
bool NukeXStretchInstance::ExecuteOn(View& view) {
    auto algorithm = StretchLibrary::Create(p_algorithm);
    // Set parameters from instance members
    algorithm->SetParameter("contrast", p_contrast);
    // ... etc
    ImageVariant image = view.Image();
    algorithm->ApplyToImage(image);
    return true;
}
```

**Step 3: Implement Interface**

- Algorithm selector ComboBox
- Parameter sliders (NumericControl)
- Real-time preview via PCL preview mechanism

**Step 4: Build and test in PixInsight**

**Step 5: Commit**

```bash
git add src/NukeXStretch*
git commit -m "feat: NukeXStretch process with 11 algorithms and UI

- Algorithm selection dropdown
- Parameter controls (contrast, saturation, black/white point, gamma)
- Real-time preview support"
```

---

## Phase 5 — Release

### Task 5.1: Final test suite

**Files:**
- All test files

**Step 1: Run full test suite**

```bash
cd build && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j$(nproc)
ctest --output-on-failure
```

All tests must pass: unit, integration.

**Step 2: Run mutation testing**

Use a C++ mutation testing tool (e.g., mull-project/mull or dextool) on the distribution fitting and selection code. Verify mutation score > 80%.

**Step 3: Local PixInsight validation**

```bash
# Build release
make release
# Install
cp NukeX-pxm.so /opt/PixInsight/bin/
# Launch PixInsight, open test images, run both processes
```

**Step 4: Commit final test results**

```bash
git add -A
git commit -m "test: complete test suite — unit, integration, mutation"
```

---

### Task 5.2: Sign, package, and release

**Step 1: Sign module**

```bash
/opt/PixInsight/bin/PixInsight.sh \
    --sign-module-file=build/NukeX-pxm.so \
    --xssk-file=/home/scarter4work/projects/keys/scarter4work_keys.xssk \
    --xssk-password="***REDACTED***"
```

**Step 2: Package**

```bash
DATE=$(date +%Y%m%d)
tar czf repository/${DATE}-linux-x64-NukeX.tar.gz \
    -C build NukeX-pxm.so NukeX-pxm.xsgn
```

**Step 3: Generate updates.xri**

Update `repository/updates.xri` with:
- New package entry (filename, SHA1, release date)
- Version 3.0.0
- Platform: linux x64
- Developer signature

**Step 4: Create GitHub repository and push**

```bash
gh repo create scarter4work/nukex3 --private --source=. --remote=origin --push
```

**Step 5: Create GitHub release**

```bash
gh release create v3.0.0 \
    repository/${DATE}-linux-x64-NukeX.tar.gz \
    --title "NukeX v3.0.0" \
    --notes "Initial release: per-pixel statistical inference stacking + 11 stretch algorithms"
```

**Step 6: Final commit**

```bash
git add repository/
git commit -m "release: NukeX v3.0.0 signed and packaged"
git push
```

---

## Task Dependency Graph

```
1.1 Scaffolding
 └→ 1.2 Module entry point
     ├→ 1.3 Stack parameters
     │   └→ 1.5 Frame loader
     │       └→ 1.6 Quality weights
     │           └→ 2.1 Numerical utils
     │               ├→ 2.2 Gaussian/Poisson MLE
     │               ├→ 2.3 Skew-Normal MLE
     │               └→ 2.4 Bimodal EM
     │                   └→ 2.5 Outlier detection
     │                       └→ 2.6 Pixel selector
     │                           └→ 3.1 Stack Instance/Interface
     │                               └→ 3.2 Integration test
     └→ 4.1 Stretch base/factory
         └→ 4.2 Port 11 algorithms
             └→ 4.3 Stretch Instance/Interface
                 └→ 5.1 Final test suite
                     └→ 5.2 Sign/package/release
```

Note: Task 1.4 (SubCube) can run in parallel with 1.3 (Parameters).
Tasks 4.x (Stretch) can run in parallel with Phase 2-3 (Statistical Engine).
