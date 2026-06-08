# Post-Stretch Subcube Remediation Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace pre-stack trail detection (Phase 1c) and self-flat correction (Phase 1a) with a unified post-stretch remediation phase that detects artifacts in the stretched image and remediates through the subcube using GPU-accelerated re-selection and neighbor brightness correction.

**Architecture:** Detection runs on the stretched 2D image (CPU, luminance channel). Trail pixels get re-selected from the subcube Z-column with contaminated frames masked out (GPU kernel). Dust and vignetting pixels get multiplicative brightness correction from clean neighbor selected values (GPU kernel). All corrections patch the linear output and re-stretch.

**Tech Stack:** C++17, PCL (PixInsight Class Library), CUDA 12.8, xtensor (column-major 3D tensors), Catch2 v3 (testing), OpenMP (CPU fallback)

**Spec:** `docs/superpowers/specs/2026-03-13-post-stretch-subcube-remediation-design.md`

---

## File Structure

### New Files

| File | Responsibility |
|------|---------------|
| `src/engine/ArtifactDetector.h` | Detection API: trail (Hough), dust (blob), vignetting (radial polynomial) on stretched images |
| `src/engine/ArtifactDetector.cpp` | Detection implementations |
| `src/engine/cuda/CudaRemediation.h` | GPU remediation API: trail re-selection, dust/vignetting correction |
| `src/engine/cuda/CudaRemediation.cu` | CUDA kernels for remediation |
| `tests/unit/test_artifact_detector.cpp` | Unit tests for all three detectors |
| `tests/unit/test_cuda_remediation.cpp` | Unit tests for GPU remediation kernels |

### Modified Files

| File | Changes |
|------|---------|
| `src/engine/SubCube.h` | Add explicit move constructor/assignment for `std::vector<SubCube>` storage |
| `src/engine/PixelSelector.h` | Make `selectBestZ()` public (needed for CPU fallback in Phase 7b) |
| `src/NukeXStackInstance.h` | Replace `p_enableTrailDetection`/`p_enableSelfFlat` member vars with remediation params |
| `src/NukeXStackInstance.cpp` | Remove Phase 1a/1c, store per-channel subcubes, add Phase 7 orchestration |
| `tests/CMakeLists.txt` | Add test entries for `test_artifact_detector` and `test_cuda_remediation` |
| `src/NukeXStackParameters.h` | Replace old parameter classes with 13 new remediation parameter classes |
| `src/NukeXStackParameters.cpp` | Implement new parameter classes |
| `src/NukeXStackInterface.h` | Replace old checkboxes with remediation GUI section |
| `src/NukeXStackInterface.cpp` | Wire up new GUI controls and event handlers |
| `src/NukeXStackProcess.cpp` | Update parameter registration in constructor |

### Removed Files

| File | Reason |
|------|--------|
| `src/engine/TrailDetector.h` | Replaced by ArtifactDetector trail detection |
| `src/engine/TrailDetector.cpp` | Replaced by ArtifactDetector trail detection |
| `src/engine/FlatEstimator.h` | Replaced by Phase 7c neighbor-ratio correction |
| `src/engine/FlatEstimator.cpp` | Replaced by Phase 7c neighbor-ratio correction |

**Build system note:** Both `CMakeLists.txt` and `Makefile` use glob-based source discovery (`GLOB_RECURSE` / `wildcard`), so adding/removing `.cpp`/`.cu` source files requires no explicit build system changes. However, `tests/CMakeLists.txt` registers each test executable explicitly — new tests must be added there.

---

## Chunk 1: Remove Old Code and Update Parameters

### Task 1: Remove TrailDetector and FlatEstimator

**Files:**
- Delete: `src/engine/TrailDetector.h`
- Delete: `src/engine/TrailDetector.cpp`
- Delete: `src/engine/FlatEstimator.h`
- Delete: `src/engine/FlatEstimator.cpp`
- Modify: `src/NukeXStackInstance.cpp` (remove includes and Phase 1a/1c blocks)

- [ ] **Step 1: Delete the old source files**

```bash
cd /home/scarter4work/projects/nukex3
rm src/engine/TrailDetector.h src/engine/TrailDetector.cpp
rm src/engine/FlatEstimator.h src/engine/FlatEstimator.cpp
```

- [ ] **Step 2: Remove includes from NukeXStackInstance.cpp**

Remove lines 15-16:
```cpp
#include "engine/TrailDetector.h"
#include "engine/FlatEstimator.h"
```

- [ ] **Step 3: Remove Phase 1a (self-flat correction) from ExecuteGlobal()**

Remove the entire Phase 1a block at ~lines 166-197. This is the `if ( p_enableSelfFlat )` block that creates `FlatEstimatorConfig`, calls `FlatEstimator::applyCorrection()`, and logs flat ranges.

- [ ] **Step 4: Remove Phase 1c (trail detection) from ExecuteGlobal()**

Remove the entire Phase 1c block at ~lines 249-306. This is the `if ( p_enableTrailDetection )` block that creates `TrailDetectorConfig`, loops over frames calling `detectAndMask()`, allocates masks in the subcube, and logs trail counts.

- [ ] **Step 5: Remove trail mask saving/restoring in Phase 3 channel loop**

In Phase 3 (~lines 393-407), remove the code that saves channel 0's trail masks and copies them to channels 1+. The mask infrastructure stays in SubCube but won't be populated until Phase 7.

- [ ] **Step 6: Verify build compiles**

```bash
cd /home/scarter4work/projects/nukex3
make clean && make release 2>&1 | tail -20
```

Expected: Build succeeds (with warnings about unused `p_enableTrailDetection`/`p_enableSelfFlat` variables — those get removed in Task 2).

- [ ] **Step 7: Run tests**

```bash
cd /home/scarter4work/projects/nukex3/build && cmake .. -DPCLDIR=$HOME/PCL && make -j$(nproc) && ctest --output-on-failure
```

Expected: All existing tests pass (trail/flat tests may need removal — check if any exist).

- [ ] **Step 8: Commit**

```bash
git add -A && git commit -m "refactor: remove TrailDetector and FlatEstimator (Phase 1a/1c)

Replaced by post-stretch subcube remediation (Phase 7) in upcoming commits.
Removes pre-stack Hough trail detection and self-flat correction."
```

---

### Task 2: Replace Old Parameters with Remediation Parameters

**Files:**
- Modify: `src/NukeXStackParameters.h` (remove old param classes ~lines 143-161, add 13 new)
- Modify: `src/NukeXStackParameters.cpp` (remove old impls ~lines 239-275, add new impls)
- Modify: `src/NukeXStackInstance.h` (replace member variables ~lines 74-75)
- Modify: `src/NukeXStackInstance.cpp` (constructor, Assign(), LockParameter())
- Modify: `src/NukeXStackProcess.cpp` (parameter registration)

- [ ] **Step 1: Remove old parameter classes from NukeXStackParameters.h**

Remove `NXSEnableTrailDetection` and `NXSEnableSelfFlat` class declarations and externs (~lines 143-161).

- [ ] **Step 2: Add new remediation parameter classes to NukeXStackParameters.h**

Add after the remaining parameter declarations, before the closing `namespace`:

```cpp
// --- Remediation parameters (Phase 7) ---

class NXSEnableRemediation : public MetaBoolean
{
public:
   NXSEnableRemediation( MetaProcess* );
   IsoString Id() const override;
   bool DefaultValue() const override;
};
extern NXSEnableRemediation* TheNXSEnableRemediationParameter;

class NXSEnableTrailRemediation : public MetaBoolean
{
public:
   NXSEnableTrailRemediation( MetaProcess* );
   IsoString Id() const override;
   bool DefaultValue() const override;
};
extern NXSEnableTrailRemediation* TheNXSEnableTrailRemediationParameter;

class NXSEnableDustRemediation : public MetaBoolean
{
public:
   NXSEnableDustRemediation( MetaProcess* );
   IsoString Id() const override;
   bool DefaultValue() const override;
};
extern NXSEnableDustRemediation* TheNXSEnableDustRemediationParameter;

class NXSEnableVignettingRemediation : public MetaBoolean
{
public:
   NXSEnableVignettingRemediation( MetaProcess* );
   IsoString Id() const override;
   bool DefaultValue() const override;
};
extern NXSEnableVignettingRemediation* TheNXSEnableVignettingRemediationParameter;

class NXSTrailDilateRadius : public MetaFloat
{
public:
   NXSTrailDilateRadius( MetaProcess* );
   IsoString Id() const override;
   int Precision() const override;
   double DefaultValue() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
};
extern NXSTrailDilateRadius* TheNXSTrailDilateRadiusParameter;

class NXSTrailOutlierSigma : public MetaFloat
{
public:
   NXSTrailOutlierSigma( MetaProcess* );
   IsoString Id() const override;
   int Precision() const override;
   double DefaultValue() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
};
extern NXSTrailOutlierSigma* TheNXSTrailOutlierSigmaParameter;

class NXSDustMinDiameter : public MetaInt32
{
public:
   NXSDustMinDiameter( MetaProcess* );
   IsoString Id() const override;
   double DefaultValue() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
};
extern NXSDustMinDiameter* TheNXSDustMinDiameterParameter;

class NXSDustMaxDiameter : public MetaInt32
{
public:
   NXSDustMaxDiameter( MetaProcess* );
   IsoString Id() const override;
   double DefaultValue() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
};
extern NXSDustMaxDiameter* TheNXSDustMaxDiameterParameter;

class NXSDustCircularityMin : public MetaFloat
{
public:
   NXSDustCircularityMin( MetaProcess* );
   IsoString Id() const override;
   int Precision() const override;
   double DefaultValue() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
};
extern NXSDustCircularityMin* TheNXSDustCircularityMinParameter;

class NXSDustDetectionSigma : public MetaFloat
{
public:
   NXSDustDetectionSigma( MetaProcess* );
   IsoString Id() const override;
   int Precision() const override;
   double DefaultValue() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
};
extern NXSDustDetectionSigma* TheNXSDustDetectionSigmaParameter;

class NXSDustNeighborRadius : public MetaInt32
{
public:
   NXSDustNeighborRadius( MetaProcess* );
   IsoString Id() const override;
   double DefaultValue() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
};
extern NXSDustNeighborRadius* TheNXSDustNeighborRadiusParameter;

class NXSDustMaxCorrectionRatio : public MetaFloat
{
public:
   NXSDustMaxCorrectionRatio( MetaProcess* );
   IsoString Id() const override;
   int Precision() const override;
   double DefaultValue() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
};
extern NXSDustMaxCorrectionRatio* TheNXSDustMaxCorrectionRatioParameter;

class NXSVignettingPolyOrder : public MetaInt32
{
public:
   NXSVignettingPolyOrder( MetaProcess* );
   IsoString Id() const override;
   double DefaultValue() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
};
extern NXSVignettingPolyOrder* TheNXSVignettingPolyOrderParameter;
```

- [ ] **Step 3: Implement new parameter classes in NukeXStackParameters.cpp**

Remove old `NXSEnableTrailDetection` and `NXSEnableSelfFlat` implementations (~lines 239-275). Add implementations for all 13 new parameters following the existing PCL pattern. Example for the first two:

```cpp
// --- Remediation parameters ---

NXSEnableRemediation* TheNXSEnableRemediationParameter = nullptr;

NXSEnableRemediation::NXSEnableRemediation( MetaProcess* p ) : MetaBoolean( p )
{
   TheNXSEnableRemediationParameter = this;
}

IsoString NXSEnableRemediation::Id() const { return "enableRemediation"; }
bool NXSEnableRemediation::DefaultValue() const { return true; }

// ---

NXSEnableTrailRemediation* TheNXSEnableTrailRemediationParameter = nullptr;

NXSEnableTrailRemediation::NXSEnableTrailRemediation( MetaProcess* p ) : MetaBoolean( p )
{
   TheNXSEnableTrailRemediationParameter = this;
}

IsoString NXSEnableTrailRemediation::Id() const { return "enableTrailRemediation"; }
bool NXSEnableTrailRemediation::DefaultValue() const { return true; }
```

Follow the same pattern for all 13 parameters. Defaults from spec:
- `enableRemediation`: true
- `enableTrailRemediation`: true
- `enableDustRemediation`: true
- `enableVignettingRemediation`: true
- `trailDilateRadius`: 5.0 (min 1.0, max 20.0, precision 1)
- `trailOutlierSigma`: 3.0 (min 1.5, max 6.0, precision 1)
- `dustMinDiameter`: 10 (min 3, max 200)
- `dustMaxDiameter`: 100 (min 10, max 500)
- `dustCircularityMin`: 0.7 (min 0.3, max 1.0, precision 2)
- `dustDetectionSigma`: 2.0 (min 1.0, max 5.0, precision 1)
- `dustNeighborRadius`: 10 (min 3, max 50)
- `dustMaxCorrectionRatio`: 10.0 (min 2.0, max 50.0, precision 1)
- `vignettingPolyOrder`: 3 (min 1, max 6)

- [ ] **Step 4: Update NukeXStackProcess.cpp parameter registration**

In the `NukeXStackProcess` constructor, remove:
```cpp
new NXSEnableTrailDetection( this );
new NXSEnableSelfFlat( this );
```

Add:
```cpp
new NXSEnableRemediation( this );
new NXSEnableTrailRemediation( this );
new NXSEnableDustRemediation( this );
new NXSEnableVignettingRemediation( this );
new NXSTrailDilateRadius( this );
new NXSTrailOutlierSigma( this );
new NXSDustMinDiameter( this );
new NXSDustMaxDiameter( this );
new NXSDustCircularityMin( this );
new NXSDustDetectionSigma( this );
new NXSDustNeighborRadius( this );
new NXSDustMaxCorrectionRatio( this );
new NXSVignettingPolyOrder( this );
```

- [ ] **Step 5: Update NukeXStackInstance.h member variables**

Replace (lines ~74-75):
```cpp
pcl_bool p_enableTrailDetection;
pcl_bool p_enableSelfFlat;
```

With:
```cpp
pcl_bool  p_enableRemediation;
pcl_bool  p_enableTrailRemediation;
pcl_bool  p_enableDustRemediation;
pcl_bool  p_enableVignettingRemediation;
float     p_trailDilateRadius;
float     p_trailOutlierSigma;
int32     p_dustMinDiameter;
int32     p_dustMaxDiameter;
float     p_dustCircularityMin;
float     p_dustDetectionSigma;
int32     p_dustNeighborRadius;
float     p_dustMaxCorrectionRatio;
int32     p_vignettingPolyOrder;
```

- [ ] **Step 6: Update NukeXStackInstance.cpp constructor and methods**

In the constructor initializer list, replace old inits with:
```cpp
p_enableRemediation( TheNXSEnableRemediationParameter->DefaultValue() ),
p_enableTrailRemediation( TheNXSEnableTrailRemediationParameter->DefaultValue() ),
p_enableDustRemediation( TheNXSEnableDustRemediationParameter->DefaultValue() ),
p_enableVignettingRemediation( TheNXSEnableVignettingRemediationParameter->DefaultValue() ),
p_trailDilateRadius( TheNXSTrailDilateRadiusParameter->DefaultValue() ),
p_trailOutlierSigma( TheNXSTrailOutlierSigmaParameter->DefaultValue() ),
p_dustMinDiameter( TheNXSDustMinDiameterParameter->DefaultValue() ),
p_dustMaxDiameter( TheNXSDustMaxDiameterParameter->DefaultValue() ),
p_dustCircularityMin( TheNXSDustCircularityMinParameter->DefaultValue() ),
p_dustDetectionSigma( TheNXSDustDetectionSigmaParameter->DefaultValue() ),
p_dustNeighborRadius( TheNXSDustNeighborRadiusParameter->DefaultValue() ),
p_dustMaxCorrectionRatio( TheNXSDustMaxCorrectionRatioParameter->DefaultValue() ),
p_vignettingPolyOrder( TheNXSVignettingPolyOrderParameter->DefaultValue() ),
```

In `Assign()`, replace old assignments with all 13 new parameter copies.

In `LockParameter()`, replace old param checks with:
```cpp
if ( p == TheNXSEnableRemediationParameter )         return &p_enableRemediation;
if ( p == TheNXSEnableTrailRemediationParameter )     return &p_enableTrailRemediation;
if ( p == TheNXSEnableDustRemediationParameter )      return &p_enableDustRemediation;
if ( p == TheNXSEnableVignettingRemediationParameter) return &p_enableVignettingRemediation;
if ( p == TheNXSTrailDilateRadiusParameter )          return &p_trailDilateRadius;
if ( p == TheNXSTrailOutlierSigmaParameter )          return &p_trailOutlierSigma;
if ( p == TheNXSDustMinDiameterParameter )            return &p_dustMinDiameter;
if ( p == TheNXSDustMaxDiameterParameter )            return &p_dustMaxDiameter;
if ( p == TheNXSDustCircularityMinParameter )         return &p_dustCircularityMin;
if ( p == TheNXSDustDetectionSigmaParameter )         return &p_dustDetectionSigma;
if ( p == TheNXSDustNeighborRadiusParameter )         return &p_dustNeighborRadius;
if ( p == TheNXSDustMaxCorrectionRatioParameter )     return &p_dustMaxCorrectionRatio;
if ( p == TheNXSVignettingPolyOrderParameter )        return &p_vignettingPolyOrder;
```

- [ ] **Step 7: Build and test**

```bash
make clean && make release 2>&1 | tail -20
cd build && cmake .. -DPCLDIR=$HOME/PCL && make -j$(nproc) && ctest --output-on-failure
```

Expected: Build succeeds, all tests pass.

- [ ] **Step 8: Commit**

```bash
git add -A && git commit -m "refactor: replace trail/self-flat params with remediation params

13 new PCL parameters for Phase 7 post-stretch subcube remediation:
enableRemediation, enableTrailRemediation, enableDustRemediation,
enableVignettingRemediation, trailDilateRadius, trailOutlierSigma,
dustMinDiameter, dustMaxDiameter, dustCircularityMin,
dustDetectionSigma, dustNeighborRadius, dustMaxCorrectionRatio,
vignettingPolyOrder."
```

---

### Task 3: Update Subcube Lifecycle (Store Per-Channel)

**Files:**
- Modify: `src/engine/SubCube.h` (add move semantics)
- Modify: `src/engine/PixelSelector.h` (make `selectBestZ()` public)
- Modify: `src/NukeXStackInstance.cpp` (Phase 3 channel loop)

- [ ] **Step 1: Add explicit move semantics to SubCube**

In `src/engine/SubCube.h`, add after the existing constructor:

```cpp
// Move semantics (needed for std::vector<SubCube> storage)
SubCube( SubCube&& ) = default;
SubCube& operator=( SubCube&& ) = default;

// No copy (subcubes are large)
SubCube( const SubCube& ) = delete;
SubCube& operator=( const SubCube& ) = delete;
```

- [ ] **Step 2: Make selectBestZ() public in PixelSelector**

In `src/engine/PixelSelector.h`, move `selectBestZ()` from `private:` to `public:`. This is needed for the CPU fallback path in Phase 7b where we re-select individual trail pixels.

- [ ] **Step 3: Store per-channel subcubes instead of letting them go out of scope**

In `ExecuteGlobal()`, before the Phase 3 channel loop, add:
```cpp
// Keep per-channel subcubes alive for Phase 7 remediation
std::vector<nukex::SubCube> channelCubes;
channelCubes.reserve( outChannels );
```

In the channel loop, instead of creating `cube` as a local variable:
- Channel 0: `channelCubes.push_back( std::move(aligned.alignedCube) );`
- Channels 1+: Build the cube, then `channelCubes.push_back( std::move(cube) );`

Reference `channelCubes[ch]` throughout the loop body instead of local `cube`.

After the channel loop, `channelResults` and `channelCubes` both survive to Phase 7.

- [ ] **Step 2: Keep qualityWeights alive**

Ensure `qualityWeights` (from Phase 2) is declared at a scope that survives through Phase 7. It's currently a local in `ExecuteGlobal()` so it should already persist — verify.

- [ ] **Step 3: Build and test**

```bash
make clean && make release 2>&1 | tail -20
cd build && cmake .. -DPCLDIR=$HOME/PCL && make -j$(nproc) && ctest --output-on-failure
```

Expected: Build succeeds, all tests pass. Pipeline behavior is identical (Phase 7 not yet wired up).

- [ ] **Step 4: Commit**

```bash
git add -A && git commit -m "refactor: store per-channel subcubes for Phase 7 remediation

SubCubes now persist past Phase 3 in std::vector<SubCube> channelCubes,
enabling Phase 7 to read Z-columns for trail re-selection. Memory impact:
~5.4GB total for RGB (3 channels x ~1.8GB), within 64GB system RAM."
```

---

## Chunk 2: Artifact Detection (ArtifactDetector)

### Task 4: Trail Detection on Stretched Image

**Files:**
- Create: `src/engine/ArtifactDetector.h`
- Create: `src/engine/ArtifactDetector.cpp`
- Create: `tests/unit/test_artifact_detector.cpp`

- [ ] **Step 1: Write failing test for trail detection**

Create `tests/unit/test_artifact_detector.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "engine/ArtifactDetector.h"
#include <vector>
#include <cmath>

TEST_CASE( "TrailDetector detects bright diagonal line", "[artifact][trail]" )
{
   // Create a 200x200 synthetic stretched image with a bright diagonal trail
   const int W = 200, H = 200;
   std::vector<float> image( W * H, 0.1f ); // background = 0.1

   // Draw a bright diagonal line from (10,10) to (190,190), ~3px wide
   for ( int y = 0; y < H; ++y )
      for ( int x = 0; x < W; ++x )
      {
         // distance from line y=x
         double dist = std::abs( x - y ) / std::sqrt( 2.0 );
         if ( dist < 1.5 )
            image[y * W + x] = 0.8f; // bright trail
      }

   nukex::ArtifactDetectorConfig config;
   config.trailDilateRadius = 3.0;
   nukex::ArtifactDetector detector( config );

   auto result = detector.detectTrails( image.data(), W, H );

   // Should detect at least one line
   REQUIRE( result.trailPixelCount > 0 );

   // Trail mask should flag pixels along the diagonal
   REQUIRE( result.mask[100 * W + 100] == 1 ); // center of diagonal
   REQUIRE( result.mask[0 * W + 100] == 0 );   // off-diagonal
}

TEST_CASE( "TrailDetector ignores faint background", "[artifact][trail]" )
{
   const int W = 200, H = 200;
   std::vector<float> image( W * H, 0.1f ); // uniform background

   nukex::ArtifactDetectorConfig config;
   nukex::ArtifactDetector detector( config );

   auto result = detector.detectTrails( image.data(), W, H );

   REQUIRE( result.trailPixelCount == 0 );
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd build && cmake .. -DPCLDIR=$HOME/PCL && make -j$(nproc) 2>&1 | tail -5
```

Expected: FAIL — `ArtifactDetector.h` not found.

- [ ] **Step 3: Create ArtifactDetector header**

Create `src/engine/ArtifactDetector.h`:

```cpp
#pragma once

#include <vector>
#include <cstdint>
#include <cstddef>

namespace nukex
{

struct ArtifactDetectorConfig
{
   // Trail detection
   double trailDilateRadius  = 5.0;
   double trailOutlierSigma  = 3.0;
   int    backgroundBoxSize   = 64;

   // Dust mote detection
   int    dustMinDiameter     = 10;
   int    dustMaxDiameter     = 100;
   double dustCircularityMin  = 0.7;
   double dustDetectionSigma  = 2.0;
   int    dustNeighborRadius  = 10;
   double dustMaxCorrectionRatio = 10.0;

   // Vignetting detection
   int    vignettingPolyOrder = 3;
};

struct TrailDetectionResult
{
   std::vector<uint8_t> mask;   // row-major, 1 = trail pixel
   int trailPixelCount = 0;
   int trailLineCount  = 0;
};

struct DustBlobInfo
{
   int    centerX, centerY;
   double radius;
   double circularity;     // 0-1, 1 = perfect circle
   double meanAttenuation; // ratio of blob brightness to surroundings
};

struct DustDetectionResult
{
   std::vector<uint8_t> mask;   // row-major, 1 = dust pixel
   std::vector<DustBlobInfo> blobs;
   int dustPixelCount = 0;
};

struct VignettingDetectionResult
{
   std::vector<float> correctionMap; // row-major, >= 1.0, multiply to correct
   double maxCorrection = 1.0;      // worst-case correction factor (corners)
};

struct DetectionResult
{
   TrailDetectionResult trail;
   DustDetectionResult dust;
   VignettingDetectionResult vignetting;
};

class ArtifactDetector
{
public:
   explicit ArtifactDetector( const ArtifactDetectorConfig& config );

   // Individual detectors — operate on stretched image data (row-major float, [0,1])
   TrailDetectionResult detectTrails( const float* image, int width, int height ) const;
   DustDetectionResult detectDust( const float* image, int width, int height ) const;
   VignettingDetectionResult detectVignetting( const float* image, int width, int height,
                                                const uint8_t* excludeMask = nullptr ) const;

   // Run all enabled detectors
   DetectionResult detectAll( const float* image, int width, int height,
                              bool enableTrail, bool enableDust, bool enableVignetting ) const;

private:
   ArtifactDetectorConfig m_config;

   // Trail internals
   void estimateBackground( const float* image, int W, int H,
                            std::vector<float>& background ) const;
   void sobelMagnitude( const float* residual, int W, int H,
                        std::vector<float>& gradient ) const;

   struct HoughLine { double rho; double theta; int votes; };
   std::vector<HoughLine> houghLines( const std::vector<uint8_t>& edgeMask,
                                       int W, int H, int minVotes ) const;
   void generateTrailMask( const std::vector<HoughLine>& lines,
                           int W, int H, double dilateRadius,
                           std::vector<uint8_t>& mask ) const;

   // Dust internals
   void localBackgroundMap( const float* image, int W, int H, int ringInner, int ringOuter,
                            std::vector<float>& bgMap ) const;

   // Vignetting internals
   void fitRadialPolynomial( const float* image, int W, int H,
                             const uint8_t* excludeMask,
                             int polyOrder, std::vector<double>& coeffs ) const;
};

} // namespace nukex
```

- [ ] **Step 4: Implement trail detection in ArtifactDetector.cpp**

Create `src/engine/ArtifactDetector.cpp`. The trail detection follows the same Hough approach as the old TrailDetector but operates on the stretched image:

```cpp
#include "ArtifactDetector.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

namespace nukex
{

ArtifactDetector::ArtifactDetector( const ArtifactDetectorConfig& config )
   : m_config( config )
{
}

// ── Background estimation (block median, same as old TrailDetector) ────────

void ArtifactDetector::estimateBackground( const float* image, int W, int H,
                                            std::vector<float>& background ) const
{
   const int box = m_config.backgroundBoxSize;
   const int gridW = ( W + box - 1 ) / box;
   const int gridH = ( H + box - 1 ) / box;

   // Compute median per grid cell
   std::vector<float> gridMedians( gridW * gridH );
   std::vector<float> block;

   for ( int gy = 0; gy < gridH; ++gy )
      for ( int gx = 0; gx < gridW; ++gx )
      {
         block.clear();
         int y0 = gy * box, y1 = std::min( y0 + box, H );
         int x0 = gx * box, x1 = std::min( x0 + box, W );
         for ( int y = y0; y < y1; ++y )
            for ( int x = x0; x < x1; ++x )
               block.push_back( image[y * W + x] );
         size_t mid = block.size() / 2;
         std::nth_element( block.begin(), block.begin() + mid, block.end() );
         gridMedians[gy * gridW + gx] = block[mid];
      }

   // Bilinear interpolation to full resolution
   background.resize( W * H );
   for ( int y = 0; y < H; ++y )
      for ( int x = 0; x < W; ++x )
      {
         double gx = ( x + 0.5 ) / box - 0.5;
         double gy = ( y + 0.5 ) / box - 0.5;
         int gx0 = std::max( 0, (int)gx );
         int gy0 = std::max( 0, (int)gy );
         int gx1 = std::min( gx0 + 1, gridW - 1 );
         int gy1 = std::min( gy0 + 1, gridH - 1 );
         double fx = gx - gx0, fy = gy - gy0;
         float v00 = gridMedians[gy0 * gridW + gx0];
         float v10 = gridMedians[gy0 * gridW + gx1];
         float v01 = gridMedians[gy1 * gridW + gx0];
         float v11 = gridMedians[gy1 * gridW + gx1];
         background[y * W + x] = float(
            v00 * (1-fx)*(1-fy) + v10 * fx*(1-fy) +
            v01 * (1-fx)*fy     + v11 * fx*fy );
      }
}

// ── Sobel gradient magnitude ───────────────────────────────────────────────

void ArtifactDetector::sobelMagnitude( const float* residual, int W, int H,
                                        std::vector<float>& gradient ) const
{
   gradient.assign( W * H, 0.0f );
   for ( int y = 1; y < H - 1; ++y )
      for ( int x = 1; x < W - 1; ++x )
      {
         float gx = -residual[(y-1)*W+(x-1)] + residual[(y-1)*W+(x+1)]
                   - 2*residual[y*W+(x-1)]    + 2*residual[y*W+(x+1)]
                   - residual[(y+1)*W+(x-1)]  + residual[(y+1)*W+(x+1)];
         float gy = -residual[(y-1)*W+(x-1)] - 2*residual[(y-1)*W+x] - residual[(y-1)*W+(x+1)]
                   + residual[(y+1)*W+(x-1)] + 2*residual[(y+1)*W+x] + residual[(y+1)*W+(x+1)];
         gradient[y * W + x] = std::sqrt( gx*gx + gy*gy );
      }
}

// ── Hough transform ───────────────────────────────────────────────────────

std::vector<ArtifactDetector::HoughLine>
ArtifactDetector::houghLines( const std::vector<uint8_t>& edgeMask,
                               int W, int H, int minVotes ) const
{
   const int nTheta = 180;
   const double maxRho = std::sqrt( double(W*W + H*H) );
   const int nRho = int( 2 * maxRho ) + 1;

   std::vector<int> accumulator( nTheta * nRho, 0 );

   // Precompute sin/cos
   std::vector<double> cosTable( nTheta ), sinTable( nTheta );
   for ( int t = 0; t < nTheta; ++t )
   {
      double theta = t * M_PI / nTheta;
      cosTable[t] = std::cos( theta );
      sinTable[t] = std::sin( theta );
   }

   // Vote
   for ( int y = 0; y < H; ++y )
      for ( int x = 0; x < W; ++x )
         if ( edgeMask[y * W + x] )
            for ( int t = 0; t < nTheta; ++t )
            {
               int rho = int( x * cosTable[t] + y * sinTable[t] + maxRho + 0.5 );
               if ( rho >= 0 && rho < nRho )
                  ++accumulator[t * nRho + rho];
            }

   // Find peaks above threshold
   std::vector<HoughLine> lines;
   for ( int t = 0; t < nTheta; ++t )
      for ( int r = 0; r < nRho; ++r )
         if ( accumulator[t * nRho + r] >= minVotes )
            lines.push_back( { r - maxRho, t * M_PI / nTheta, accumulator[t * nRho + r] } );

   // Cluster nearby peaks (merge within 5 rho, 3 degrees)
   std::vector<HoughLine> merged;
   std::vector<bool> used( lines.size(), false );
   for ( size_t i = 0; i < lines.size(); ++i )
   {
      if ( used[i] ) continue;
      HoughLine best = lines[i];
      for ( size_t j = i + 1; j < lines.size(); ++j )
      {
         if ( used[j] ) continue;
         if ( std::abs(lines[j].rho - best.rho) < 5.0 &&
              std::abs(lines[j].theta - best.theta) < 3.0 * M_PI / 180.0 )
         {
            if ( lines[j].votes > best.votes )
               best = lines[j];
            used[j] = true;
         }
      }
      merged.push_back( best );
   }

   return merged;
}

// ── Trail mask generation ──────────────────────────────────────────────────

void ArtifactDetector::generateTrailMask( const std::vector<HoughLine>& lines,
                                           int W, int H, double dilateRadius,
                                           std::vector<uint8_t>& mask ) const
{
   mask.assign( W * H, 0 );
   for ( auto& line : lines )
   {
      double cosT = std::cos( line.theta );
      double sinT = std::sin( line.theta );
      for ( int y = 0; y < H; ++y )
         for ( int x = 0; x < W; ++x )
         {
            double dist = std::abs( x * cosT + y * sinT - line.rho );
            if ( dist <= dilateRadius )
               mask[y * W + x] = 1;
         }
   }
}

// ── Trail detection main entry ─────────────────────────────────────────────

TrailDetectionResult ArtifactDetector::detectTrails( const float* image, int W, int H ) const
{
   TrailDetectionResult result;

   // 1. Background estimation
   std::vector<float> background;
   estimateBackground( image, W, H, background );

   // 2. Residual = image - background
   std::vector<float> residual( W * H );
   for ( int i = 0; i < W * H; ++i )
      residual[i] = std::max( 0.0f, image[i] - background[i] );

   // 3. Sobel edge detection
   std::vector<float> gradient;
   sobelMagnitude( residual.data(), W, H, gradient );

   // 4. Threshold: compute gradient stats, threshold at mean + sigma * MAD
   std::vector<float> nonzero;
   for ( float g : gradient )
      if ( g > 0 ) nonzero.push_back( g );

   if ( nonzero.empty() )
   {
      result.mask.assign( W * H, 0 );
      return result;
   }

   std::sort( nonzero.begin(), nonzero.end() );
   float medGrad = nonzero[nonzero.size() / 2];
   std::vector<float> absdev( nonzero.size() );
   for ( size_t i = 0; i < nonzero.size(); ++i )
      absdev[i] = std::abs( nonzero[i] - medGrad );
   std::sort( absdev.begin(), absdev.end() );
   float madGrad = absdev[absdev.size() / 2] * 1.4826f;

   float edgeThreshold = medGrad + float(m_config.trailOutlierSigma) * madGrad;

   std::vector<uint8_t> edgeMask( W * H, 0 );
   for ( int i = 0; i < W * H; ++i )
      if ( gradient[i] > edgeThreshold && residual[i] > 0 )
         edgeMask[i] = 1;

   // 5. Hough transform
   int minVotes = std::max( 100, std::min( W, H ) / 4 );
   auto lines = houghLines( edgeMask, W, H, minVotes );

   // 6. Generate dilated mask
   generateTrailMask( lines, W, H, m_config.trailDilateRadius, result.mask );

   result.trailLineCount = int( lines.size() );
   result.trailPixelCount = 0;
   for ( uint8_t v : result.mask )
      result.trailPixelCount += v;

   return result;
}

// ── Stub implementations for dust and vignetting (implemented in Tasks 5-6) ──

DustDetectionResult ArtifactDetector::detectDust( const float* /*image*/, int W, int H ) const
{
   DustDetectionResult result;
   result.mask.assign( W * H, 0 );
   return result;
}

VignettingDetectionResult ArtifactDetector::detectVignetting( const float* /*image*/,
   int W, int H, const uint8_t* /*excludeMask*/ ) const
{
   VignettingDetectionResult result;
   result.correctionMap.assign( W * H, 1.0f );
   return result;
}

DetectionResult ArtifactDetector::detectAll( const float* image, int W, int H,
                                              bool enableTrail, bool enableDust,
                                              bool enableVignetting ) const
{
   DetectionResult result;

   if ( enableTrail )
      result.trail = detectTrails( image, W, H );
   else
   {
      result.trail.mask.assign( W * H, 0 );
   }

   if ( enableDust )
      result.dust = detectDust( image, W, H );
   else
   {
      result.dust.mask.assign( W * H, 0 );
   }

   if ( enableVignetting )
   {
      // Exclude trail + dust pixels from vignetting fit
      std::vector<uint8_t> excludeMask( W * H, 0 );
      for ( int i = 0; i < W * H; ++i )
         excludeMask[i] = result.trail.mask[i] | result.dust.mask[i];
      result.vignetting = detectVignetting( image, W, H, excludeMask.data() );
   }
   else
   {
      result.vignetting.correctionMap.assign( W * H, 1.0f );
   }

   return result;
}

// ── Placeholder stubs for internals used by dust/vignetting ────────────────

void ArtifactDetector::localBackgroundMap( const float*, int, int, int, int,
                                            std::vector<float>& ) const {}

void ArtifactDetector::fitRadialPolynomial( const float*, int, int,
                                             const uint8_t*, int,
                                             std::vector<double>& ) const {}

} // namespace nukex
```

- [ ] **Step 5: Register test in tests/CMakeLists.txt**

Add entries for the new test executable in `tests/CMakeLists.txt`, following the existing pattern (each test is explicitly registered — tests do NOT use glob discovery):

```cmake
# ArtifactDetector unit tests
add_executable(test_artifact_detector
    unit/test_artifact_detector.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/ArtifactDetector.cpp
)
target_link_libraries(test_artifact_detector PRIVATE Catch2::Catch2WithMain)
target_include_directories(test_artifact_detector PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_features(test_artifact_detector PRIVATE cxx_std_17)
add_test(NAME test_artifact_detector COMMAND test_artifact_detector)
```

- [ ] **Step 6: Run tests**

```bash
cd build && cmake .. -DPCLDIR=$HOME/PCL && make -j$(nproc) && ctest --output-on-failure
```

Expected: Trail detection tests PASS. Existing tests still pass.

- [ ] **Step 7: Commit**

```bash
git add src/engine/ArtifactDetector.h src/engine/ArtifactDetector.cpp tests/unit/test_artifact_detector.cpp tests/CMakeLists.txt
git commit -m "feat: add ArtifactDetector with trail detection (Phase 7a)

Hough transform on stretched images where faint trails are visible.
Background estimation, Sobel edges, Hough voting, peak clustering,
dilated mask generation. Dust/vignetting detectors stubbed for now."
```

---

### Task 5: Dust Mote Detection

**Files:**
- Modify: `src/engine/ArtifactDetector.cpp` (implement `detectDust()` and `localBackgroundMap()`)
- Modify: `tests/unit/test_artifact_detector.cpp` (add dust tests)

- [ ] **Step 1: Write failing test for dust detection**

Add to `tests/unit/test_artifact_detector.cpp`:

```cpp
TEST_CASE( "DustDetector detects dark circular blob", "[artifact][dust]" )
{
   const int W = 200, H = 200;
   std::vector<float> image( W * H, 0.5f ); // uniform background

   // Draw a dark circular dust mote at center, radius 20px
   int cx = 100, cy = 100, r = 20;
   for ( int y = 0; y < H; ++y )
      for ( int x = 0; x < W; ++x )
      {
         double dist = std::sqrt( double((x-cx)*(x-cx) + (y-cy)*(y-cy)) );
         if ( dist < r )
            image[y * W + x] = 0.3f; // 60% of background = dust shadow
      }

   nukex::ArtifactDetectorConfig config;
   config.dustMinDiameter = 10;
   config.dustMaxDiameter = 100;
   config.dustCircularityMin = 0.5;
   config.dustDetectionSigma = 1.5;
   nukex::ArtifactDetector detector( config );

   auto result = detector.detectDust( image.data(), W, H );

   REQUIRE( result.dustPixelCount > 0 );
   REQUIRE( result.blobs.size() == 1 );
   REQUIRE( result.mask[cy * W + cx] == 1 ); // center flagged
   REQUIRE( result.blobs[0].circularity > 0.8 ); // nearly circular
}

TEST_CASE( "DustDetector ignores non-circular dark regions", "[artifact][dust]" )
{
   const int W = 200, H = 200;
   std::vector<float> image( W * H, 0.5f );

   // Draw a long thin dark rectangle (NOT circular — should be rejected)
   for ( int y = 90; y < 110; ++y )
      for ( int x = 20; x < 180; ++x )
         image[y * W + x] = 0.3f;

   nukex::ArtifactDetectorConfig config;
   config.dustCircularityMin = 0.7;
   nukex::ArtifactDetector detector( config );

   auto result = detector.detectDust( image.data(), W, H );

   // Rectangle has low circularity — should be rejected
   REQUIRE( result.blobs.empty() );
}
```

- [ ] **Step 2: Implement dust detection**

Replace the `detectDust()` stub and `localBackgroundMap()` stub in `ArtifactDetector.cpp`:

```cpp
void ArtifactDetector::localBackgroundMap( const float* image, int W, int H,
                                            int ringInner, int ringOuter,
                                            std::vector<float>& bgMap ) const
{
   bgMap.resize( W * H );
   std::vector<float> ring;

   for ( int y = 0; y < H; ++y )
      for ( int x = 0; x < W; ++x )
      {
         ring.clear();
         for ( int dy = -ringOuter; dy <= ringOuter; ++dy )
            for ( int dx = -ringOuter; dx <= ringOuter; ++dx )
            {
               int nx = x + dx, ny = y + dy;
               if ( nx < 0 || nx >= W || ny < 0 || ny >= H ) continue;
               double dist = std::sqrt( double(dx*dx + dy*dy) );
               if ( dist >= ringInner && dist <= ringOuter )
                  ring.push_back( image[ny * W + nx] );
            }

         if ( ring.size() < 8 )
         {
            bgMap[y * W + x] = image[y * W + x];
            continue;
         }
         size_t mid = ring.size() / 2;
         std::nth_element( ring.begin(), ring.begin() + mid, ring.end() );
         bgMap[y * W + x] = ring[mid];
      }
}

DustDetectionResult ArtifactDetector::detectDust( const float* image, int W, int H ) const
{
   DustDetectionResult result;
   result.mask.assign( W * H, 0 );

   int ringInner = m_config.dustMaxDiameter / 2 + 5;
   int ringOuter = ringInner + 15;

   // 1. Compute local background map
   std::vector<float> bgMap;
   localBackgroundMap( image, W, H, ringInner, ringOuter, bgMap );

   // 2. Compute MAD of background for threshold
   std::vector<float> bgValues;
   bgValues.reserve( W * H );
   for ( int i = 0; i < W * H; ++i )
      bgValues.push_back( bgMap[i] );
   std::sort( bgValues.begin(), bgValues.end() );
   float medBg = bgValues[bgValues.size() / 2];
   std::vector<float> absdev( bgValues.size() );
   for ( size_t i = 0; i < bgValues.size(); ++i )
      absdev[i] = std::abs( bgValues[i] - medBg );
   std::sort( absdev.begin(), absdev.end() );
   float madBg = absdev[absdev.size() / 2] * 1.4826f;
   if ( madBg < 1e-10f ) madBg = 1e-10f;

   // 3. Flag pixels significantly darker than local background
   std::vector<uint8_t> darkMask( W * H, 0 );
   for ( int i = 0; i < W * H; ++i )
   {
      float deficit = bgMap[i] - image[i];
      if ( deficit > m_config.dustDetectionSigma * madBg )
         darkMask[i] = 1;
   }

   // 4. Connected component labeling (simple flood fill)
   std::vector<int> labels( W * H, 0 );
   int nextLabel = 1;
   std::vector<std::vector<std::pair<int,int>>> components;

   for ( int y = 0; y < H; ++y )
      for ( int x = 0; x < W; ++x )
      {
         if ( !darkMask[y * W + x] || labels[y * W + x] ) continue;

         // Flood fill
         std::vector<std::pair<int,int>> comp;
         std::vector<std::pair<int,int>> stack = { {x, y} };
         labels[y * W + x] = nextLabel;

         while ( !stack.empty() )
         {
            auto [cx, cy] = stack.back();
            stack.pop_back();
            comp.push_back( {cx, cy} );

            for ( int dy = -1; dy <= 1; ++dy )
               for ( int dx = -1; dx <= 1; ++dx )
               {
                  int nx = cx + dx, ny = cy + dy;
                  if ( nx < 0 || nx >= W || ny < 0 || ny >= H ) continue;
                  if ( darkMask[ny * W + nx] && !labels[ny * W + nx] )
                  {
                     labels[ny * W + nx] = nextLabel;
                     stack.push_back( {nx, ny} );
                  }
               }
         }

         components.push_back( std::move(comp) );
         ++nextLabel;
      }

   // 5. Filter by size and circularity
   for ( auto& comp : components )
   {
      int n = int( comp.size() );

      // Bounding box for diameter check
      int minX = W, maxX = 0, minY = H, maxY = 0;
      for ( auto [x, y] : comp )
      {
         minX = std::min( minX, x ); maxX = std::max( maxX, x );
         minY = std::min( minY, y ); maxY = std::max( maxY, y );
      }
      int bboxW = maxX - minX + 1;
      int bboxH = maxY - minY + 1;
      int diameter = std::max( bboxW, bboxH );

      if ( diameter < m_config.dustMinDiameter || diameter > m_config.dustMaxDiameter )
         continue;

      // Circularity = 4*pi*area / perimeter^2
      // Approximate: area = n pixels, ideal circle area = pi*(d/2)^2
      double idealArea = M_PI * (diameter / 2.0) * (diameter / 2.0);
      double circularity = std::min( 1.0, double(n) / idealArea );

      if ( circularity < m_config.dustCircularityMin )
         continue;

      // Passed filters — mark as dust
      DustBlobInfo blob;
      blob.centerX = ( minX + maxX ) / 2;
      blob.centerY = ( minY + maxY ) / 2;
      blob.radius = diameter / 2.0;
      blob.circularity = circularity;

      // Mean attenuation
      double attenSum = 0;
      for ( auto [x, y] : comp )
      {
         attenSum += bgMap[y * W + x] / std::max( 1e-10f, image[y * W + x] );
         result.mask[y * W + x] = 1;
      }
      blob.meanAttenuation = attenSum / n;

      result.blobs.push_back( blob );
   }

   result.dustPixelCount = 0;
   for ( uint8_t v : result.mask )
      result.dustPixelCount += v;

   return result;
}
```

- [ ] **Step 3: Run tests**

```bash
cd build && cmake .. -DPCLDIR=$HOME/PCL && make -j$(nproc) && ctest --output-on-failure
```

Expected: Dust tests PASS.

- [ ] **Step 4: Commit**

```bash
git add src/engine/ArtifactDetector.cpp tests/unit/test_artifact_detector.cpp
git commit -m "feat: add dust mote detection to ArtifactDetector (Phase 7a)

Local background map via annular ring median, dark pixel flagging with
sigma threshold, connected component labeling, circularity + diameter
filtering. Rejects non-circular dark features (nebulae, dust lanes)."
```

---

### Task 6: Vignetting Detection

**Files:**
- Modify: `src/engine/ArtifactDetector.cpp` (implement `detectVignetting()` and `fitRadialPolynomial()`)
- Modify: `tests/unit/test_artifact_detector.cpp` (add vignetting tests)

- [ ] **Step 1: Write failing test**

Add to `tests/unit/test_artifact_detector.cpp`:

```cpp
TEST_CASE( "VignettingDetector detects radial brightness falloff", "[artifact][vignetting]" )
{
   const int W = 200, H = 200;
   std::vector<float> image( W * H );

   // Simulate vignetting: brightness = 1.0 at center, falling off radially
   double cx = W / 2.0, cy = H / 2.0;
   double maxR = std::sqrt( cx*cx + cy*cy );
   for ( int y = 0; y < H; ++y )
      for ( int x = 0; x < W; ++x )
      {
         double r = std::sqrt( (x-cx)*(x-cx) + (y-cy)*(y-cy) ) / maxR;
         image[y * W + x] = float( 0.8 - 0.3 * r * r ); // quadratic falloff
      }

   nukex::ArtifactDetectorConfig config;
   config.vignettingPolyOrder = 3;
   nukex::ArtifactDetector detector( config );

   auto result = detector.detectVignetting( image.data(), W, H, nullptr );

   // Center should need minimal correction (~1.0)
   float centerCorr = result.correctionMap[100 * W + 100];
   REQUIRE( centerCorr < 1.05f );

   // Corner should need more correction (> 1.2)
   float cornerCorr = result.correctionMap[0 * W + 0];
   REQUIRE( cornerCorr > 1.15f );

   // Corrected image should be more uniform
   float corrCenter = image[100 * W + 100] * centerCorr;
   float corrCorner = image[0 * W + 0] * cornerCorr;
   REQUIRE( std::abs(corrCenter - corrCorner) < 0.1f ); // within 10%
}

TEST_CASE( "VignettingDetector produces identity on flat image", "[artifact][vignetting]" )
{
   const int W = 100, H = 100;
   std::vector<float> image( W * H, 0.5f );

   nukex::ArtifactDetectorConfig config;
   nukex::ArtifactDetector detector( config );

   auto result = detector.detectVignetting( image.data(), W, H, nullptr );

   // All corrections should be ~1.0 for a flat image
   for ( float c : result.correctionMap )
      REQUIRE( c == Catch::Approx(1.0f).margin(0.02f) );
}
```

- [ ] **Step 2: Implement vignetting detection**

Replace stubs in `ArtifactDetector.cpp`:

```cpp
void ArtifactDetector::fitRadialPolynomial( const float* image, int W, int H,
                                             const uint8_t* excludeMask,
                                             int polyOrder,
                                             std::vector<double>& coeffs ) const
{
   // Collect (radius, brightness) samples from unmasked pixels
   double cx = W / 2.0, cy = H / 2.0;
   double maxR = std::sqrt( cx*cx + cy*cy );

   // Subsample for efficiency (every 4th pixel)
   std::vector<double> radii, values;
   for ( int y = 0; y < H; y += 4 )
      for ( int x = 0; x < W; x += 4 )
      {
         if ( excludeMask && excludeMask[y * W + x] ) continue;
         double r = std::sqrt( (x-cx)*(x-cx) + (y-cy)*(y-cy) ) / maxR;
         radii.push_back( r );
         values.push_back( image[y * W + x] );
      }

   if ( radii.empty() )
   {
      coeffs.assign( polyOrder + 1, 0.0 );
      coeffs[0] = 1.0;
      return;
   }

   // Least-squares fit: brightness = c0 + c1*r + c2*r^2 + ... + cn*r^n
   // Normal equations: A^T A x = A^T b
   int n = polyOrder + 1;
   std::vector<double> ATA( n * n, 0.0 );
   std::vector<double> ATb( n, 0.0 );

   for ( size_t i = 0; i < radii.size(); ++i )
   {
      std::vector<double> row( n );
      row[0] = 1.0;
      for ( int j = 1; j < n; ++j )
         row[j] = row[j-1] * radii[i];

      for ( int j = 0; j < n; ++j )
      {
         for ( int k = 0; k < n; ++k )
            ATA[j * n + k] += row[j] * row[k];
         ATb[j] += row[j] * values[i];
      }
   }

   // Solve via Cholesky (ATA is symmetric positive definite for polynomial fit)
   // Simple Gaussian elimination for small n
   coeffs.resize( n );
   std::vector<double> aug( n * (n+1) );
   for ( int i = 0; i < n; ++i )
   {
      for ( int j = 0; j < n; ++j )
         aug[i * (n+1) + j] = ATA[i * n + j];
      aug[i * (n+1) + n] = ATb[i];
   }

   for ( int col = 0; col < n; ++col )
   {
      // Partial pivoting
      int maxRow = col;
      for ( int row = col + 1; row < n; ++row )
         if ( std::abs(aug[row*(n+1)+col]) > std::abs(aug[maxRow*(n+1)+col]) )
            maxRow = row;
      if ( maxRow != col )
         for ( int j = 0; j <= n; ++j )
            std::swap( aug[col*(n+1)+j], aug[maxRow*(n+1)+j] );

      double pivot = aug[col*(n+1)+col];
      if ( std::abs(pivot) < 1e-15 )
      {
         coeffs.assign( n, 0.0 );
         coeffs[0] = 1.0;
         return;
      }

      for ( int j = col; j <= n; ++j )
         aug[col*(n+1)+j] /= pivot;
      for ( int row = 0; row < n; ++row )
      {
         if ( row == col ) continue;
         double factor = aug[row*(n+1)+col];
         for ( int j = col; j <= n; ++j )
            aug[row*(n+1)+j] -= factor * aug[col*(n+1)+j];
      }
   }

   for ( int i = 0; i < n; ++i )
      coeffs[i] = aug[i * (n+1) + n];
}

VignettingDetectionResult ArtifactDetector::detectVignetting(
   const float* image, int W, int H, const uint8_t* excludeMask ) const
{
   VignettingDetectionResult result;

   double cx = W / 2.0, cy = H / 2.0;
   double maxR = std::sqrt( cx*cx + cy*cy );

   // Fit radial polynomial to background brightness
   std::vector<double> coeffs;
   fitRadialPolynomial( image, W, H, excludeMask, m_config.vignettingPolyOrder, coeffs );

   // Evaluate polynomial at center (r=0) to get reference brightness
   double centerBrightness = coeffs[0]; // c0 + c1*0 + c2*0^2 + ... = c0

   // Build correction map
   result.correctionMap.resize( W * H );
   result.maxCorrection = 1.0;

   for ( int y = 0; y < H; ++y )
      for ( int x = 0; x < W; ++x )
      {
         double r = std::sqrt( (x-cx)*(x-cx) + (y-cy)*(y-cy) ) / maxR;

         // Evaluate polynomial at this radius
         double fitted = coeffs[0];
         double rPow = r;
         for ( size_t j = 1; j < coeffs.size(); ++j )
         {
            fitted += coeffs[j] * rPow;
            rPow *= r;
         }

         // Correction = center brightness / local fitted brightness
         float correction = 1.0f;
         if ( fitted > 1e-10 )
            correction = float( centerBrightness / fitted );
         correction = std::max( 1.0f, correction ); // never darken

         result.correctionMap[y * W + x] = correction;
         result.maxCorrection = std::max( result.maxCorrection, double(correction) );
      }

   return result;
}
```

- [ ] **Step 3: Run tests**

```bash
cd build && cmake .. -DPCLDIR=$HOME/PCL && make -j$(nproc) && ctest --output-on-failure
```

Expected: All vignetting + previous tests PASS.

- [ ] **Step 4: Commit**

```bash
git add src/engine/ArtifactDetector.cpp tests/unit/test_artifact_detector.cpp
git commit -m "feat: add vignetting detection to ArtifactDetector (Phase 7a)

Radial polynomial fit to background brightness (least-squares, subsampled).
Correction map = center_brightness / fitted_brightness at each pixel.
Excludes trail/dust pixels from fit. Handles flat images (identity correction)."
```

---

## Chunk 3: GPU Remediation Kernels

### Task 7: Trail Re-Selection GPU Kernel

**Files:**
- Create: `src/engine/cuda/CudaRemediation.h`
- Create: `src/engine/cuda/CudaRemediation.cu`
- Create: `tests/unit/test_cuda_remediation.cpp`

- [ ] **Step 1: Write failing test for trail re-selection**

Create `tests/unit/test_cuda_remediation.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

// Test CPU fallback path (always available, same math as GPU)
#include "engine/PixelSelector.h"
#include "engine/SubCube.h"

TEST_CASE( "Trail re-selection excludes contaminated frame", "[remediation][trail]" )
{
   // 10 frames, 1x1 pixel — Z-column has one trail outlier
   nukex::SubCube cube( 10, 1, 1 );
   float values[] = { 0.1f, 0.1f, 0.1f, 0.1f, 0.1f,
                       0.1f, 0.1f, 0.1f, 0.1f, 0.8f }; // frame 9 is trail
   for ( int z = 0; z < 10; ++z )
      cube.setPixel( z, 0, 0, values[z] );

   // Without mask: median should be pulled up (0.1 is still median, but trail survives MAD)
   nukex::PixelSelector selector;
   std::vector<double> weights( 10, 1.0 );

   auto resultClean = selector.selectBestZ( cube.zColumnPtr(0, 0), 10, weights );
   // The MAD/ESD should reject 0.8, but let's verify the mechanism works with explicit mask too

   // With mask on frame 9:
   cube.allocateMasks();
   cube.setMask( 9, 0, 0, 1 ); // mask the trail frame

   auto resultMasked = selector.selectBestZ( cube.zColumnPtr(0, 0), 10, weights,
                                              cube.maskColumnPtr(0, 0) );

   // Masked result should be 0.1 (clean frames only)
   REQUIRE( resultMasked.selectedValue == Catch::Approx(0.1f).margin(0.01f) );
}
```

- [ ] **Step 2: Run test to verify it compiles and passes (CPU path)**

```bash
cd build && cmake .. -DPCLDIR=$HOME/PCL && make -j$(nproc) && ctest --output-on-failure
```

Expected: PASS — this tests the existing CPU mask path which already works.

- [ ] **Step 3: Create CudaRemediation header**

Create `src/engine/cuda/CudaRemediation.h`:

```cpp
#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

namespace nukex { namespace cuda {

struct RemediationConfig
{
   size_t nSubs;
   size_t height;
   size_t width;
   double trailOutlierSigma = 3.0;
   double dustMaxCorrectionRatio = 10.0;
};

struct TrailPixel
{
   int x, y;
};

// Trail re-selection: re-run selectBestZ() on trail pixels with contaminated frames masked
// cubeData: column-major float subcube (nSubs x H x W), one channel
// trailPixels: compact list of (x,y) coordinates to remediate
// qualityWeights: per-sub weights from Phase 2
// outputPixels: receives corrected values (indexed same as trailPixels)
// Returns true on success, false to fall back to CPU
bool remediateTrailsGPU(
   const float* cubeData,
   size_t nSubs, size_t height, size_t width,
   const std::vector<TrailPixel>& trailPixels,
   const std::vector<double>& qualityWeights,
   double trailOutlierSigma,
   float* outputPixels );

// Dust correction: multiply dust pixels by neighbor brightness ratio
// channelResult: row-major float array (H x W) — the Phase 3 selected values
// dustMask: row-major uint8 (H x W) — 1 = dust pixel
// neighborRadius: search radius for clean neighbors
// maxRatio: clamp to prevent blowup
// correctedPixels: output for corrected dust pixel values
bool remediateDustGPU(
   const float* channelResult,
   int width, int height,
   const uint8_t* dustMask,
   int neighborRadius,
   float maxRatio,
   float* correctedOutput );

// Vignetting correction: multiply all pixels by correction map
// channelResult: row-major float array (H x W)
// correctionMap: row-major float array (H x W), >= 1.0
// correctedOutput: receives channelResult[i] * correctionMap[i]
bool remediateVignettingGPU(
   const float* channelResult,
   const float* correctionMap,
   int width, int height,
   float* correctedOutput );

} } // namespace nukex::cuda
```

- [ ] **Step 4: Implement CudaRemediation.cu**

Create `src/engine/cuda/CudaRemediation.cu`:

```cuda
#include "CudaRemediation.h"
#include <cuda_runtime.h>
#include <cmath>
#include <algorithm>
#include <cstdio>

namespace nukex { namespace cuda {

// ── Device helper functions ────────────────────────────────────────────────
// These mirror the CPU PixelSelector math. Key functions:

__device__ void d_insertionSort( float* arr, int n )
{
   for ( int i = 1; i < n; ++i )
   {
      float key = arr[i];
      int j = i - 1;
      while ( j >= 0 && arr[j] > key )
      {
         arr[j+1] = arr[j];
         --j;
      }
      arr[j+1] = key;
   }
}

__device__ float d_median( float* arr, int n )
{
   d_insertionSort( arr, n );
   return ( n % 2 == 0 ) ? 0.5f * (arr[n/2-1] + arr[n/2]) : arr[n/2];
}

__device__ float d_mad( const float* vals, int n, float med )
{
   // Compute MAD = median(|x_i - median|) * 1.4826
   float absdev[128]; // max 128 frames
   int m = min( n, 128 );
   for ( int i = 0; i < m; ++i )
      absdev[i] = fabsf( vals[i] - med );
   d_insertionSort( absdev, m );
   float madVal = ( m % 2 == 0 ) ? 0.5f * (absdev[m/2-1] + absdev[m/2]) : absdev[m/2];
   return madVal * 1.4826f;
}

// ── Trail remediation kernel ───────────────────────────────────────────────

__global__ void trailRemediationKernel(
   const float* cubeData,       // column-major (nSubs, H, W)
   int nSubs, int height, int width,
   const int* trailX,           // x coords of trail pixels
   const int* trailY,           // y coords of trail pixels
   int numTrailPixels,
   float trailOutlierSigma,
   float* outputValues )        // one output per trail pixel
{
   int tid = blockIdx.x * blockDim.x + threadIdx.x;
   if ( tid >= numTrailPixels ) return;

   int x = trailX[tid];
   int y = trailY[tid];

   // Read Z-column (column-major: stride between Z values = 1, stride between y = nSubs, etc.)
   // Layout: cube(z, y, x) = cubeData[z + y * nSubs + x * nSubs * height]
   int colOffset = y * nSubs + x * nSubs * height;

   float zValues[128]; // max 128 frames
   int n = min( nSubs, 128 );
   for ( int z = 0; z < n; ++z )
      zValues[z] = cubeData[colOffset + z];

   // Find median and MAD of Z-column
   float sorted[128];
   for ( int i = 0; i < n; ++i ) sorted[i] = zValues[i];
   float med = d_median( sorted, n );
   float madVal = d_mad( zValues, n, med );

   if ( madVal < 1e-10f ) madVal = 1e-10f;

   // Identify and exclude trail frames (bright outliers)
   float clean[128];
   int nClean = 0;
   for ( int z = 0; z < n; ++z )
   {
      if ( zValues[z] <= med + trailOutlierSigma * madVal )
         clean[nClean++] = zValues[z];
   }

   // Return median of clean values
   if ( nClean >= 1 )
      outputValues[tid] = d_median( clean, nClean );
   else
      outputValues[tid] = med; // fallback
}

// ── Dust remediation kernel ────────────────────────────────────────────────

__global__ void dustRemediationKernel(
   const float* channelResult,  // row-major (H, W)
   int width, int height,
   const uint8_t* dustMask,     // row-major (H, W), 1 = dust
   int neighborRadius,
   float maxRatio,
   float* correctedOutput )     // row-major (H, W)
{
   int idx = blockIdx.x * blockDim.x + threadIdx.x;
   if ( idx >= width * height ) return;

   // Non-dust pixels pass through unchanged
   if ( !dustMask[idx] )
   {
      correctedOutput[idx] = channelResult[idx];
      return;
   }

   int x = idx % width;
   int y = idx / width;

   // Find clean neighbors (not dust, not zero)
   float neighborSum = 0.0f;
   int neighborCount = 0;

   for ( int dy = -neighborRadius; dy <= neighborRadius; ++dy )
      for ( int dx = -neighborRadius; dx <= neighborRadius; ++dx )
      {
         int nx = x + dx, ny = y + dy;
         if ( nx < 0 || nx >= width || ny < 0 || ny >= height ) continue;
         int nIdx = ny * width + nx;
         if ( dustMask[nIdx] ) continue;  // skip other dust pixels
         if ( channelResult[nIdx] < 1e-10f ) continue; // skip near-zero
         neighborSum += channelResult[nIdx];
         ++neighborCount;
      }

   if ( neighborCount == 0 || channelResult[idx] < 1e-10f )
   {
      correctedOutput[idx] = channelResult[idx]; // can't correct
      return;
   }

   float neighborMean = neighborSum / neighborCount;
   float ratio = neighborMean / channelResult[idx];
   ratio = fminf( ratio, maxRatio ); // clamp

   correctedOutput[idx] = channelResult[idx] * ratio;
}

// ── Vignetting correction kernel ───────────────────────────────────────────

__global__ void vignettingCorrectionKernel(
   const float* channelResult,
   const float* correctionMap,
   int numPixels,
   float* correctedOutput )
{
   int idx = blockIdx.x * blockDim.x + threadIdx.x;
   if ( idx >= numPixels ) return;
   correctedOutput[idx] = channelResult[idx] * correctionMap[idx];
}

// ── Host API implementations ───────────────────────────────────────────────

bool remediateTrailsGPU(
   const float* cubeData,
   size_t nSubs, size_t height, size_t width,
   const std::vector<TrailPixel>& trailPixels,
   const std::vector<double>& qualityWeights,
   double trailOutlierSigma,
   float* outputPixels )
{
   if ( trailPixels.empty() ) return true;

   int numTrail = int( trailPixels.size() );
   size_t cubeSize = nSubs * height * width;

   // Build coordinate arrays
   std::vector<int> hX( numTrail ), hY( numTrail );
   for ( int i = 0; i < numTrail; ++i )
   {
      hX[i] = trailPixels[i].x;
      hY[i] = trailPixels[i].y;
   }

   // Allocate device memory
   float *d_cube = nullptr, *d_output = nullptr;
   int *d_trailX = nullptr, *d_trailY = nullptr;

   cudaError_t err;
   err = cudaMalloc( &d_cube, cubeSize * sizeof(float) );
   if ( err != cudaSuccess ) return false;
   err = cudaMalloc( &d_output, numTrail * sizeof(float) );
   if ( err != cudaSuccess ) { cudaFree(d_cube); return false; }
   err = cudaMalloc( &d_trailX, numTrail * sizeof(int) );
   if ( err != cudaSuccess ) { cudaFree(d_cube); cudaFree(d_output); return false; }
   err = cudaMalloc( &d_trailY, numTrail * sizeof(int) );
   if ( err != cudaSuccess ) { cudaFree(d_cube); cudaFree(d_output); cudaFree(d_trailX); return false; }

   // Upload
   cudaMemcpy( d_cube, cubeData, cubeSize * sizeof(float), cudaMemcpyHostToDevice );
   cudaMemcpy( d_trailX, hX.data(), numTrail * sizeof(int), cudaMemcpyHostToDevice );
   cudaMemcpy( d_trailY, hY.data(), numTrail * sizeof(int), cudaMemcpyHostToDevice );

   // Launch
   int blockSize = 256;
   int gridSize = ( numTrail + blockSize - 1 ) / blockSize;
   trailRemediationKernel<<<gridSize, blockSize>>>(
      d_cube, int(nSubs), int(height), int(width),
      d_trailX, d_trailY, numTrail,
      float(trailOutlierSigma), d_output );

   err = cudaGetLastError();
   if ( err != cudaSuccess )
   {
      cudaFree(d_cube); cudaFree(d_output); cudaFree(d_trailX); cudaFree(d_trailY);
      return false;
   }
   cudaDeviceSynchronize();

   // Download results
   cudaMemcpy( outputPixels, d_output, numTrail * sizeof(float), cudaMemcpyDeviceToHost );

   cudaFree(d_cube); cudaFree(d_output); cudaFree(d_trailX); cudaFree(d_trailY);
   return true;
}

bool remediateDustGPU(
   const float* channelResult,
   int width, int height,
   const uint8_t* dustMask,
   int neighborRadius,
   float maxRatio,
   float* correctedOutput )
{
   int numPixels = width * height;

   float *d_input = nullptr, *d_output = nullptr;
   uint8_t *d_mask = nullptr;

   cudaError_t err;
   err = cudaMalloc( &d_input, numPixels * sizeof(float) );
   if ( err != cudaSuccess ) return false;
   err = cudaMalloc( &d_output, numPixels * sizeof(float) );
   if ( err != cudaSuccess ) { cudaFree(d_input); return false; }
   err = cudaMalloc( &d_mask, numPixels );
   if ( err != cudaSuccess ) { cudaFree(d_input); cudaFree(d_output); return false; }

   cudaMemcpy( d_input, channelResult, numPixels * sizeof(float), cudaMemcpyHostToDevice );
   cudaMemcpy( d_mask, dustMask, numPixels, cudaMemcpyHostToDevice );

   int blockSize = 256;
   int gridSize = ( numPixels + blockSize - 1 ) / blockSize;
   dustRemediationKernel<<<gridSize, blockSize>>>(
      d_input, width, height, d_mask, neighborRadius, maxRatio, d_output );

   err = cudaGetLastError();
   if ( err != cudaSuccess )
   {
      cudaFree(d_input); cudaFree(d_output); cudaFree(d_mask);
      return false;
   }
   cudaDeviceSynchronize();

   cudaMemcpy( correctedOutput, d_output, numPixels * sizeof(float), cudaMemcpyDeviceToHost );

   cudaFree(d_input); cudaFree(d_output); cudaFree(d_mask);
   return true;
}

bool remediateVignettingGPU(
   const float* channelResult,
   const float* correctionMap,
   int width, int height,
   float* correctedOutput )
{
   int numPixels = width * height;

   float *d_input = nullptr, *d_corr = nullptr, *d_output = nullptr;

   cudaError_t err;
   err = cudaMalloc( &d_input, numPixels * sizeof(float) );
   if ( err != cudaSuccess ) return false;
   err = cudaMalloc( &d_corr, numPixels * sizeof(float) );
   if ( err != cudaSuccess ) { cudaFree(d_input); return false; }
   err = cudaMalloc( &d_output, numPixels * sizeof(float) );
   if ( err != cudaSuccess ) { cudaFree(d_input); cudaFree(d_corr); return false; }

   cudaMemcpy( d_input, channelResult, numPixels * sizeof(float), cudaMemcpyHostToDevice );
   cudaMemcpy( d_corr, correctionMap, numPixels * sizeof(float), cudaMemcpyHostToDevice );

   int blockSize = 256;
   int gridSize = ( numPixels + blockSize - 1 ) / blockSize;
   vignettingCorrectionKernel<<<gridSize, blockSize>>>(
      d_input, d_corr, numPixels, d_output );

   err = cudaGetLastError();
   if ( err != cudaSuccess )
   {
      cudaFree(d_input); cudaFree(d_corr); cudaFree(d_output);
      return false;
   }
   cudaDeviceSynchronize();

   cudaMemcpy( correctedOutput, d_output, numPixels * sizeof(float), cudaMemcpyDeviceToHost );

   cudaFree(d_input); cudaFree(d_corr); cudaFree(d_output);
   return true;
}

} } // namespace nukex::cuda
```

- [ ] **Step 5: Register test in tests/CMakeLists.txt**

Add entry for `test_cuda_remediation` in `tests/CMakeLists.txt`. This test exercises the CPU fallback (PixelSelector + SubCube + dependencies), not CUDA directly. Follow the `test_pixel_selector` pattern since it needs the same dependencies:

```cmake
# CudaRemediation unit tests (CPU fallback path)
add_executable(test_cuda_remediation
    unit/test_cuda_remediation.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/PixelSelector.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/DistributionFitter.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/SkewNormalFitter.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/GaussianMixEM.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/OutlierDetector.cpp
)
target_link_libraries(test_cuda_remediation PRIVATE Catch2::Catch2WithMain)
target_include_directories(test_cuda_remediation PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/xtensor/include
    ${CMAKE_SOURCE_DIR}/third_party/xtl/include
    ${CMAKE_SOURCE_DIR}/third_party/xsimd/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_math/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_config/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_assert/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_throw_exception/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_core/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_type_traits/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_static_assert/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_mp11/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_integer/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_lexical_cast/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_predef/include
    ${CMAKE_SOURCE_DIR}/third_party/eigen
    ${CMAKE_SOURCE_DIR}/third_party/lbfgspp/include
)
target_compile_definitions(test_cuda_remediation PRIVATE XTENSOR_USE_XSIMD BOOST_MATH_STANDALONE=1)
target_compile_features(test_cuda_remediation PRIVATE cxx_std_17)
add_test(NAME test_cuda_remediation COMMAND test_cuda_remediation)
```

- [ ] **Step 6: Run tests**

```bash
cd build && cmake .. -DPCLDIR=$HOME/PCL && make -j$(nproc) && ctest --output-on-failure
```

Expected: Trail re-selection test PASS (tests CPU path via PixelSelector). CUDA code compiles.

- [ ] **Step 7: Commit**

```bash
git add src/engine/cuda/CudaRemediation.h src/engine/cuda/CudaRemediation.cu tests/unit/test_cuda_remediation.cpp tests/CMakeLists.txt
git commit -m "feat: add CudaRemediation GPU kernels (Phase 7b/7c)

Trail re-selection: identifies bright outlier frames per pixel, excludes
them, returns median of clean Z-values. Sparse launch (one thread per
trail pixel). Dust correction: neighbor brightness ratio with clamping.
Vignetting: simple multiplicative correction. All with CPU fallback."
```

---

## Chunk 4: Pipeline Integration

### Task 8: Wire Phase 7 into NukeXStackInstance

**Files:**
- Modify: `src/NukeXStackInstance.cpp` (add Phase 7 after Phase 6)

- [ ] **Step 1: Add include for ArtifactDetector**

At the top of `NukeXStackInstance.cpp`, add:
```cpp
#include "engine/ArtifactDetector.h"
#ifdef NUKEX_HAS_CUDA
#include "engine/cuda/CudaRemediation.h"
#endif
```

- [ ] **Step 2: Hoist per-channel stretch algorithms for Phase 7d re-use**

In Phase 6 of `ExecuteGlobal()`, the per-channel stretch is currently done via `lastChAlgo` which is a single `unique_ptr` overwritten per channel iteration. We need to save each channel's configured algorithm for the Phase 7d re-stretch.

Before the Phase 6 per-channel stretch loop, declare:
```cpp
std::vector<std::unique_ptr<IStretchAlgorithm>> stretchAlgos( outChannels );
```

Inside the per-channel loop (where `lastChAlgo` is created/configured), after the stretch is applied to the channel, save the configured clone:
```cpp
stretchAlgos[ch] = lastChAlgo->Clone();
```

This keeps the per-channel AutoConfigure'd parameters alive for Phase 7d.

- [ ] **Step 3: Implement Phase 7 orchestration**

After Phase 6 (apply stretch), before the final banner, add the Phase 7 block. This is the core integration — it ties detection, GPU remediation, and re-stretch together:

```cpp
// ════════════════════════════════════════════════════════════════════════
// Phase 7: Post-stretch subcube remediation
// ════════════════════════════════════════════════════════════════════════

if ( p_enableRemediation && p_enableAutoStretch )
{
   console.WriteLn( "\n<b>Phase 7: Post-stretch subcube remediation</b>" );

   // 7a: Detection on stretched luminance
   console.WriteLn( "  Phase 7a: Detecting artifacts in stretched image..." );

   // cropW and cropH are already in scope from Phase 1b alignment
   // Extract luminance from stretched image for detection
   std::vector<float> luminance( cropW * cropH, 0.0f );

   if ( isColor )
   {
      for ( int y = 0; y < cropH; ++y )
         for ( int x = 0; x < cropW; ++x )
         {
            float r = stretchImage.Pixel( x, y, 0 );
            float g = stretchImage.Pixel( x, y, 1 );
            float b = stretchImage.Pixel( x, y, 2 );
            luminance[y * cropW + x] = ( r + g + b ) / 3.0f;
         }
   }
   else
   {
      for ( int y = 0; y < cropH; ++y )
         for ( int x = 0; x < cropW; ++x )
            luminance[y * cropW + x] = stretchImage.Pixel( x, y, 0 );
   }

   nukex::ArtifactDetectorConfig detConfig;
   detConfig.trailDilateRadius     = p_trailDilateRadius;
   detConfig.trailOutlierSigma     = p_trailOutlierSigma;
   detConfig.dustMinDiameter       = p_dustMinDiameter;
   detConfig.dustMaxDiameter       = p_dustMaxDiameter;
   detConfig.dustCircularityMin    = p_dustCircularityMin;
   detConfig.dustDetectionSigma    = p_dustDetectionSigma;
   detConfig.dustNeighborRadius    = p_dustNeighborRadius;
   detConfig.dustMaxCorrectionRatio = p_dustMaxCorrectionRatio;
   detConfig.vignettingPolyOrder   = p_vignettingPolyOrder;

   nukex::ArtifactDetector detector( detConfig );
   auto detection = detector.detectAll( luminance.data(), cropW, cropH,
                                         p_enableTrailRemediation,
                                         p_enableDustRemediation,
                                         p_enableVignettingRemediation );

   console.WriteLn( String::Format( "    Trails: %d pixels (%d lines)",
      detection.trail.trailPixelCount, detection.trail.trailLineCount ) );
   console.WriteLn( String::Format( "    Dust: %d pixels (%d blobs)",
      detection.dust.dustPixelCount, int(detection.dust.blobs.size()) ) );
   console.WriteLn( String::Format( "    Vignetting: max correction %.2f",
      detection.vignetting.maxCorrection ) );

   bool anyRemediation = detection.trail.trailPixelCount > 0
                       || detection.dust.dustPixelCount > 0
                       || detection.vignetting.maxCorrection > 1.01;

   if ( anyRemediation )
   {
      // 7b: Trail remediation (per channel)
      if ( detection.trail.trailPixelCount > 0 )
      {
         console.WriteLn( "  Phase 7b: Trail remediation (re-selecting from subcube)..." );

         // Build compact trail pixel list
         std::vector<nukex::cuda::TrailPixel> trailPixels;
         for ( int y = 0; y < cropH; ++y )
            for ( int x = 0; x < cropW; ++x )
               if ( detection.trail.mask[y * cropW + x] )
                  trailPixels.push_back( { x, y } );

         for ( int ch = 0; ch < outChannels; ++ch )
         {
            std::vector<float> corrected( trailPixels.size() );
            bool gpuOk = false;

#ifdef NUKEX_HAS_CUDA
            if ( useGPU )
            {
               gpuOk = nukex::cuda::remediateTrailsGPU(
                  channelCubes[ch].cube().data(),
                  channelCubes[ch].numSubs(), cropH, cropW,
                  trailPixels, qualityWeights,
                  p_trailOutlierSigma, corrected.data() );
            }
#endif
            if ( !gpuOk )
            {
               // CPU fallback
               nukex::PixelSelector selector;
               for ( size_t i = 0; i < trailPixels.size(); ++i )
               {
                  int x = trailPixels[i].x, y = trailPixels[i].y;
                  // Allocate masks and mark trail frames
                  channelCubes[ch].allocateMasks();
                  const float* zCol = channelCubes[ch].zColumnPtr( y, x );
                  // Find outlier frame(s) in Z-column
                  std::vector<float> zVals( zCol, zCol + channelCubes[ch].numSubs() );
                  std::sort( zVals.begin(), zVals.end() );
                  float med = zVals[zVals.size()/2];
                  // MAD
                  std::vector<float> absdev( zVals.size() );
                  for ( size_t j = 0; j < zVals.size(); ++j )
                     absdev[j] = std::abs( zVals[j] - med );
                  std::sort( absdev.begin(), absdev.end() );
                  float madVal = absdev[absdev.size()/2] * 1.4826f;
                  if ( madVal < 1e-10f ) madVal = 1e-10f;
                  float threshold = med + float(p_trailOutlierSigma) * madVal;

                  // Mask bright outliers
                  for ( size_t z = 0; z < channelCubes[ch].numSubs(); ++z )
                     if ( zCol[z] > threshold )
                        channelCubes[ch].setMask( z, y, x, 1 );

                  auto result = selector.selectBestZ(
                     zCol, channelCubes[ch].numSubs(), qualityWeights,
                     channelCubes[ch].maskColumnPtr( y, x ) );
                  corrected[i] = result.selectedValue;
               }
            }

            // Patch channelResults
            for ( size_t i = 0; i < trailPixels.size(); ++i )
            {
               int x = trailPixels[i].x, y = trailPixels[i].y;
               channelResults[ch][y * cropW + x] = corrected[i];
            }
         }

         console.WriteLn( String::Format( "    Remediated %d trail pixels per channel",
            int(trailPixels.size()) ) );
      }

      // 7c: Dust remediation (per channel)
      if ( detection.dust.dustPixelCount > 0 )
      {
         console.WriteLn( "  Phase 7c: Dust remediation (neighbor brightness correction)..." );

         for ( int ch = 0; ch < outChannels; ++ch )
         {
            std::vector<float> corrected( cropW * cropH );
            bool gpuOk = false;

#ifdef NUKEX_HAS_CUDA
            if ( useGPU )
            {
               gpuOk = nukex::cuda::remediateDustGPU(
                  channelResults[ch].data(), cropW, cropH,
                  detection.dust.mask.data(),
                  p_dustNeighborRadius, p_dustMaxCorrectionRatio,
                  corrected.data() );
            }
#endif
            if ( !gpuOk )
            {
               // CPU fallback: same algorithm
               corrected = channelResults[ch]; // copy
               for ( int y = 0; y < cropH; ++y )
                  for ( int x = 0; x < cropW; ++x )
                  {
                     if ( !detection.dust.mask[y * cropW + x] ) continue;
                     float neighborSum = 0; int neighborCount = 0;
                     for ( int dy = -p_dustNeighborRadius; dy <= p_dustNeighborRadius; ++dy )
                        for ( int dx = -p_dustNeighborRadius; dx <= p_dustNeighborRadius; ++dx )
                        {
                           int nx = x + dx, ny = y + dy;
                           if ( nx < 0 || nx >= cropW || ny < 0 || ny >= cropH ) continue;
                           if ( detection.dust.mask[ny * cropW + nx] ) continue;
                           if ( channelResults[ch][ny * cropW + nx] < 1e-10f ) continue;
                           neighborSum += channelResults[ch][ny * cropW + nx];
                           ++neighborCount;
                        }
                     if ( neighborCount > 0 && channelResults[ch][y * cropW + x] > 1e-10f )
                     {
                        float ratio = (neighborSum / neighborCount) / channelResults[ch][y * cropW + x];
                        ratio = std::min( ratio, p_dustMaxCorrectionRatio );
                        corrected[y * cropW + x] = channelResults[ch][y * cropW + x] * ratio;
                     }
                  }
            }

            channelResults[ch] = std::move( corrected );
         }

         console.WriteLn( String::Format( "    Corrected %d dust pixels per channel",
            detection.dust.dustPixelCount ) );
      }

      // 7c (cont): Vignetting correction (per channel)
      if ( detection.vignetting.maxCorrection > 1.01 )
      {
         console.WriteLn( "  Phase 7c: Vignetting correction..." );

         for ( int ch = 0; ch < outChannels; ++ch )
         {
            std::vector<float> corrected( cropW * cropH );
            bool gpuOk = false;

#ifdef NUKEX_HAS_CUDA
            if ( useGPU )
            {
               gpuOk = nukex::cuda::remediateVignettingGPU(
                  channelResults[ch].data(),
                  detection.vignetting.correctionMap.data(),
                  cropW, cropH, corrected.data() );
            }
#endif
            if ( !gpuOk )
            {
               for ( int i = 0; i < cropW * cropH; ++i )
                  corrected[i] = channelResults[ch][i] * detection.vignetting.correctionMap[i];
            }

            channelResults[ch] = std::move( corrected );
         }

         console.WriteLn( String::Format( "    Max vignetting correction: %.2f",
            detection.vignetting.maxCorrection ) );
      }

      // 7d: Patch linear output and re-stretch
      console.WriteLn( "  Phase 7d: Patching linear output and re-stretching..." );

      // Patch linear output image with corrected channelResults
      for ( int ch = 0; ch < outChannels; ++ch )
         for ( int y = 0; y < cropH; ++y )
            for ( int x = 0; x < cropW; ++x )
               outputImage.Pixel( x, y, ch ) = channelResults[ch][y * cropW + x];

      // Re-apply stretch to the corrected linear image
      // Copy corrected linear data to the stretch output window
      for ( int ch = 0; ch < outChannels; ++ch )
         for ( int y = 0; y < cropH; ++y )
            for ( int x = 0; x < cropW; ++x )
               stretchImage.Pixel( x, y, ch ) = outputImage.Pixel( x, y, ch );

      // Re-apply per-channel stretch using the saved algorithm clones from Step 2
      for ( int ch = 0; ch < outChannels; ++ch )
      {
         for ( int y = 0; y < cropH; ++y )
            for ( int x = 0; x < cropW; ++x )
            {
               double v = stretchImage.Pixel( x, y, ch );
               stretchImage.Pixel( x, y, ch ) = float( stretchAlgos[ch]->Apply( v ) );
            }
      }

      console.WriteLn( "  Remediation complete." );
   }
   else
   {
      console.WriteLn( "  No artifacts detected — skipping remediation." );
   }

   // Free subcubes (no longer needed)
   channelCubes.clear();
}
else if ( !p_enableAutoStretch )
{
   // No stretch = no detection possible, free subcubes
   channelCubes.clear();
}
```

Note: The Phase 7d re-stretch reuse will need to reference the stretch algorithm objects from Phase 6. Ensure those are kept alive (declared at the right scope in ExecuteGlobal).

- [ ] **Step 3: Build and test**

```bash
make clean && make release 2>&1 | tail -20
cd build && cmake .. -DPCLDIR=$HOME/PCL && make -j$(nproc) && ctest --output-on-failure
```

Expected: Build succeeds, all tests pass.

- [ ] **Step 4: Commit**

```bash
git add src/NukeXStackInstance.cpp
git commit -m "feat: wire Phase 7 post-stretch subcube remediation into pipeline

Detects trails, dust motes, and vignetting in stretched image.
Trail pixels re-selected from subcube Z-column (GPU or CPU fallback).
Dust pixels corrected via neighbor brightness ratio (GPU or CPU).
Vignetting corrected via radial polynomial correction map (GPU or CPU).
Patches linear output and re-stretches."
```

---

### Task 9: Update GUI

**Files:**
- Modify: `src/NukeXStackInterface.h`
- Modify: `src/NukeXStackInterface.cpp`

- [ ] **Step 1: Replace old GUI members in NukeXStackInterface.h**

In the `GUIData` struct, remove:
```cpp
CheckBox EnableTrailDetection_CheckBox;
CheckBox EnableSelfFlat_CheckBox;
```

Add:
```cpp
// Remediation section
SectionBar    Remediation_SectionBar;
Control       Remediation_Control;
CheckBox      EnableRemediation_CheckBox;
CheckBox      EnableTrailRemediation_CheckBox;
CheckBox      EnableDustRemediation_CheckBox;
CheckBox      EnableVignettingRemediation_CheckBox;
NumericControl TrailDilateRadius_NumericControl;
NumericControl TrailOutlierSigma_NumericControl;
NumericControl DustDetectionSigma_NumericControl;
NumericControl DustNeighborRadius_NumericControl;
NumericControl DustMaxCorrectionRatio_NumericControl;
NumericControl VignettingPolyOrder_NumericControl;
```

- [ ] **Step 2: Update GUI construction and event handlers in NukeXStackInterface.cpp**

Replace old checkbox creation with new remediation section GUI setup. Replace old event handlers with new ones for all remediation controls. Update `UpdateControls()` to sync all new parameters.

Follow the existing pattern in the file for `SectionBar` + `Control` + checkboxes/numeric controls.

- [ ] **Step 3: Build and test**

```bash
make clean && make release 2>&1 | tail -20
```

Expected: Build succeeds. GUI now shows remediation section instead of old trail/self-flat checkboxes.

- [ ] **Step 4: Commit**

```bash
git add src/NukeXStackInterface.h src/NukeXStackInterface.cpp
git commit -m "feat: update GUI for Phase 7 remediation parameters

Replace trail detection / self-flat checkboxes with remediation section:
master enable, trail/dust/vignetting sub-enables, and numeric controls
for all detection thresholds and correction parameters."
```

---

### Task 10: Integration Test and Cleanup

**Files:**
- Modify: `tests/unit/test_artifact_detector.cpp` (add integration test)
- Modify: any remaining references to old TrailDetector/FlatEstimator

- [ ] **Step 1: Add integration test for full detection + remediation flow**

Add to `tests/unit/test_artifact_detector.cpp`:

```cpp
TEST_CASE( "Full detection pipeline runs without crash", "[artifact][integration]" )
{
   const int W = 100, H = 100;
   std::vector<float> image( W * H );

   // Image with trail + dust + vignetting
   double cx = W/2.0, cy = H/2.0, maxR = std::sqrt(cx*cx + cy*cy);
   for ( int y = 0; y < H; ++y )
      for ( int x = 0; x < W; ++x )
      {
         double r = std::sqrt((x-cx)*(x-cx) + (y-cy)*(y-cy)) / maxR;
         image[y * W + x] = float( 0.5 - 0.15 * r * r ); // vignetting
      }

   // Add dust mote
   for ( int y = 20; y < 40; ++y )
      for ( int x = 20; x < 40; ++x )
      {
         double dist = std::sqrt( double((x-30)*(x-30) + (y-30)*(y-30)) );
         if ( dist < 10 )
            image[y * W + x] *= 0.6f;
      }

   // Add trail
   for ( int y = 0; y < H; ++y )
   {
      int x = y; // diagonal
      if ( x >= 0 && x < W )
         image[y * W + x] = 0.9f;
   }

   nukex::ArtifactDetectorConfig config;
   nukex::ArtifactDetector detector( config );

   auto result = detector.detectAll( image.data(), W, H, true, true, true );

   // Just verify it runs without crash and produces valid masks
   REQUIRE( result.trail.mask.size() == size_t(W * H) );
   REQUIRE( result.dust.mask.size() == size_t(W * H) );
   REQUIRE( result.vignetting.correctionMap.size() == size_t(W * H) );
}
```

- [ ] **Step 2: Search for any remaining references to TrailDetector/FlatEstimator**

```bash
grep -r "TrailDetector\|FlatEstimator\|enableTrailDetection\|enableSelfFlat" src/ tests/ --include="*.cpp" --include="*.h"
```

Expected: No matches (all references removed). If any found, remove them.

- [ ] **Step 3: Full build + test**

```bash
make clean && make release 2>&1 | tail -20
cd build && cmake .. -DPCLDIR=$HOME/PCL && make -j$(nproc) && ctest --output-on-failure
```

Expected: Clean build, all tests pass.

- [ ] **Step 4: Commit**

```bash
git add -A && git commit -m "feat: complete Phase 7 post-stretch subcube remediation

Integration test for full detection pipeline. All old TrailDetector
and FlatEstimator references removed. Phase 7 replaces Phase 1a and
1c with unified post-stretch artifact detection and subcube-informed
remediation (GPU-accelerated with CPU fallback)."
```

---

### Task 11: Version Bump and Package

**Files:**
- Modify: `src/NukeXModule.cpp` (bump `MODULE_VERSION_BUILD`)
- Modify: `repository/updates.xri` (update title/description)

Follow the release workflow in CLAUDE.md:

- [ ] **Step 1: Bump version**

In `src/NukeXModule.cpp`, increment `MODULE_VERSION_BUILD` (currently 21 → 22).

- [ ] **Step 2: Update updates.xri**

Update title and description to mention Phase 7 post-stretch subcube remediation.

- [ ] **Step 3: Clean build + test**

```bash
make clean && make release
cd build && cmake .. -DPCLDIR=$HOME/PCL && make -j$(nproc) && ctest --output-on-failure
```

- [ ] **Step 4: Package**

```bash
make package
```

- [ ] **Step 5: Install**

```bash
sudo make install
```

- [ ] **Step 6: Commit and push**

```bash
git add -A && git commit -m "build: bump to v3.0.0.22 — post-stretch subcube remediation

Replaces Phase 1a (self-flat) and Phase 1c (Hough trail detection) with
unified Phase 7: detect artifacts in stretched image, remediate through
subcube (trail re-selection + dust/vignetting brightness correction).
GPU-accelerated with CPU fallback."
git push
```
