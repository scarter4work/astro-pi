# Z-Plane Metadata Tiebreaker Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace dead frame-level quality weights with per-Z-plane metadata tiebreaker in pixel selection.

**Architecture:** Remove `QualityWeights.h/.cpp` and all frame weight infrastructure. Add `qualityScore` field to `SubMetadata`. After shortest-half mode selection in `selectBestZ`, scan for alternative frames within 1× MAD of the selected value and prefer the one with the best metadata score (FWHM + eccentricity composite). Fix `ComputeFrameMetrics` thresholds so scores are non-zero on uncalibrated subs.

**Tech Stack:** C++17, PCL, xtensor, CUDA, Catch2 v3

**Spec:** `docs/superpowers/specs/2026-03-17-z-plane-metadata-tiebreaker-design.md`

---

### Task 1: Add `qualityScore` to SubMetadata + write tiebreaker test

**Files:**
- Modify: `src/engine/SubCube.h:19-30`
- Modify: `tests/unit/test_pixel_selector.cpp`

- [ ] **Step 1: Add `qualityScore` field to SubMetadata**

In `src/engine/SubCube.h`, add `qualityScore` after `ccdTemp`:

```cpp
struct SubMetadata {
    double fwhm = 0;
    double eccentricity = 0;
    double skyBackground = 0;
    double hfr = 0;
    double altitude = 0;
    double exposure = 0;
    double gain = 0;
    double ccdTemp = 0;
    double qualityScore = 0.0;
    std::string object;
    std::string filter;
};
```

- [ ] **Step 2: Write tiebreaker test — picks better-scored frame**

Append to `tests/unit/test_pixel_selector.cpp`:

```cpp
TEST_CASE("PixelSelector tiebreaker picks better-scored frame", "[selector][tiebreaker]") {
    // 10 subs, all values near 100.0 (within tight cluster)
    // Sub 3 has slightly better value (closest to mode) but worse score
    // Sub 7 has slightly worse value but better score
    // Both are within MAD tolerance → tiebreaker should pick sub 7
    nukex::SubCube cube(10, 2, 2);
    for (size_t z = 0; z < 10; z++)
        cube.setPixel(z, 0, 0, 100.0f + static_cast<float>(z) * 0.1f);

    // Set quality scores: sub 7 has the best score
    for (size_t z = 0; z < 10; z++) {
        nukex::SubMetadata meta;
        meta.qualityScore = (z == 7) ? 0.9 : 0.1;
        cube.setMetadata(z, meta);
    }

    // Build scores array from metadata
    std::vector<double> scores(10);
    for (size_t z = 0; z < 10; z++)
        scores[z] = cube.metadata(z).qualityScore;

    nukex::PixelSelector::Config cfg;
    cfg.enableMetadataTiebreaker = true;
    nukex::PixelSelector selector(cfg);
    auto result = selector.selectBestZ(cube.zColumnPtr(0, 0), 10,
                                        scores.data(), nullptr);

    // Tiebreaker should select sub 7 (best score within MAD tolerance)
    REQUIRE(result.selectedZ == 7);
}

TEST_CASE("PixelSelector tiebreaker no-op with equal scores", "[selector][tiebreaker]") {
    nukex::SubCube cube(10, 2, 2);
    for (size_t z = 0; z < 10; z++)
        cube.setPixel(z, 0, 0, 100.0f + static_cast<float>(z) * 0.1f);

    // All scores equal
    std::vector<double> scores(10, 0.5);

    nukex::PixelSelector::Config cfg;
    cfg.enableMetadataTiebreaker = true;
    nukex::PixelSelector selector(cfg);
    auto resultWith = selector.selectBestZ(cube.zColumnPtr(0, 0), 10,
                                            scores.data(), nullptr);

    // Without scores — should get same result
    auto resultWithout = selector.selectBestZ(cube.zColumnPtr(0, 0), 10,
                                              nullptr, nullptr);

    REQUIRE(resultWith.selectedZ == resultWithout.selectedZ);
    REQUIRE(resultWith.selectedValue == resultWithout.selectedValue);
}

TEST_CASE("PixelSelector tiebreaker no-op with null scores", "[selector][tiebreaker]") {
    nukex::SubCube cube(10, 2, 2);
    for (size_t z = 0; z < 10; z++)
        cube.setPixel(z, 0, 0, 100.0f + static_cast<float>(z) * 0.1f);

    nukex::PixelSelector selector;
    auto result = selector.selectBestZ(cube.zColumnPtr(0, 0), 10,
                                        nullptr, nullptr);

    // Should still produce a valid result
    REQUIRE(result.selectedZ < 10);
    REQUIRE(result.selectedValue > 99.0f);
    REQUIRE(result.selectedValue < 102.0f);
}

TEST_CASE("PixelSelector tiebreaker zero scores graceful degradation", "[selector][tiebreaker]") {
    nukex::SubCube cube(10, 2, 2);
    for (size_t z = 0; z < 10; z++)
        cube.setPixel(z, 0, 0, 100.0f + static_cast<float>(z) * 0.1f);

    std::vector<double> scores(10, 0.0);

    nukex::PixelSelector::Config cfg;
    cfg.enableMetadataTiebreaker = true;
    nukex::PixelSelector selector(cfg);
    auto resultZero = selector.selectBestZ(cube.zColumnPtr(0, 0), 10,
                                            scores.data(), nullptr);
    auto resultNull = selector.selectBestZ(cube.zColumnPtr(0, 0), 10,
                                            nullptr, nullptr);

    REQUIRE(resultZero.selectedZ == resultNull.selectedZ);
}

TEST_CASE("PixelSelector tiebreaker single candidate no change", "[selector][tiebreaker]") {
    // Only 3 subs, halfN=1, MAD=0 → tiebreaker disabled
    nukex::SubCube cube(3, 2, 2);
    cube.setPixel(0, 0, 0, 100.0f);
    cube.setPixel(1, 0, 0, 200.0f);
    cube.setPixel(2, 0, 0, 300.0f);

    std::vector<double> scores = {0.1, 0.9, 0.1};

    nukex::PixelSelector::Config cfg;
    cfg.enableMetadataTiebreaker = true;
    nukex::PixelSelector selector(cfg);
    auto result = selector.selectBestZ(cube.zColumnPtr(0, 0), 3,
                                        scores.data(), nullptr);

    // Should still work — just no tiebreaker effect
    REQUIRE(result.selectedZ < 3);
}
```

- [ ] **Step 3: Run tests to verify they fail**

Run: `cd build && cmake .. -DPCLDIR=$HOME/PCL && make -j$(nproc) test_pixel_selector 2>&1 | tail -5`
Expected: compilation error — `enableMetadataTiebreaker` not a member of Config, `selectBestZ` signature mismatch

- [ ] **Step 4: Commit data model + tests**

```bash
git add src/engine/SubCube.h tests/unit/test_pixel_selector.cpp
git commit -m "test: add tiebreaker tests + qualityScore field to SubMetadata"
```

---

### Task 2: Implement tiebreaker in PixelSelector (CPU path)

**Files:**
- Modify: `src/engine/PixelSelector.h:19-25` (Config struct)
- Modify: `src/engine/PixelSelector.h:32-33,37-38,42-43,58-60` (method signatures)
- Modify: `src/engine/PixelSelector.cpp:37-40,262-278,281-284,291-293,317,350-352,358,376-377,381,393`

- [ ] **Step 1: Update Config struct in PixelSelector.h**

Replace the Config struct (lines 19-25):

```cpp
    struct Config {
        int maxOutliers = 3;
        double outlierAlpha = 0.05;
        bool adaptiveModels = false;
        bool useGPU = false;
        bool enableMetadataTiebreaker = true;
    };
```

- [ ] **Step 2: Update method signatures in PixelSelector.h**

Replace `qualityWeights` with `qualityScores` in all four methods:

```cpp
    float processPixel(SubCube& cube, size_t y, size_t x,
                       const double* qualityScores = nullptr);

    std::vector<float> processImage(SubCube& cube,
                                    const double* qualityScores = nullptr,
                                    ProgressCallback progress = nullptr);

    std::vector<float> processImageGPU(SubCube& cube,
                                        const double* qualityScores,
                                        std::vector<uint8_t>& distTypesOut,
                                        ProgressCallback progress = nullptr);

    PixelResult selectBestZ(const float* zColumnPtr, size_t nSubs,
                            const double* qualityScores = nullptr,
                            const uint8_t* maskColumn = nullptr);
```

- [ ] **Step 3: Update selectBestZ signature in PixelSelector.cpp**

Change line 38-40 from:
```cpp
PixelSelector::selectBestZ(const float* zColumnPtr, size_t nSubs,
                           const std::vector<double>& qualityWeights,
                           const uint8_t* maskColumn)
```
To:
```cpp
PixelSelector::selectBestZ(const float* zColumnPtr, size_t nSubs,
                           const double* qualityScores,
                           const uint8_t* maskColumn)
```

- [ ] **Step 4: Add tiebreaker logic after step 8 in selectBestZ**

After the "find closest frame" loop (line 272) and before the "return result" block (line 274), insert:

```cpp
    // 8b. Metadata tiebreaker: if multiple frames are within MAD tolerance
    //     of the selected value, prefer the one with the best quality score.
    if (qualityScores != nullptr && m_config.enableMetadataTiebreaker) {
        // Recompute the shortest-half bounds for tiebreaker scan
        std::vector<double> shSorted;
        shSorted.reserve(cleanIndices.size());
        for (size_t idx : cleanIndices)
            shSorted.push_back(zValues[idx]);
        std::sort(shSorted.begin(), shSorted.end());

        int shN = static_cast<int>(shSorted.size());
        int shHalf = shN / 2;
        if (shHalf < 1) shHalf = 1;

        // Compute MAD of the shortest-half cluster
        if (shHalf > 1) {
            // Find best start (same as above)
            double shMinRange = shSorted[shHalf - 1] - shSorted[0];
            int shBestStart = 0;
            for (int i = 1; i + shHalf - 1 < shN; ++i) {
                double range = shSorted[i + shHalf - 1] - shSorted[i];
                if (range < shMinRange) {
                    shMinRange = range;
                    shBestStart = i;
                }
            }

            // Compute MAD of the shortest-half values
            double shLo = shSorted[shBestStart];
            double shHi = shSorted[shBestStart + shHalf - 1];
            std::vector<double> shValues(shSorted.begin() + shBestStart,
                                          shSorted.begin() + shBestStart + shHalf);
            double shMedian = medianOfSorted(shValues.data(), shValues.size());
            std::vector<double> shDeviations(shValues.size());
            for (size_t i = 0; i < shValues.size(); ++i)
                shDeviations[i] = std::abs(shValues[i] - shMedian);
            std::sort(shDeviations.begin(), shDeviations.end());
            double shMAD = 1.4826 * medianOfSorted(shDeviations.data(), shDeviations.size());

            if (shMAD > 0.0) {
                double bestScore = qualityScores[bestZ];
                for (size_t idx : cleanIndices) {
                    double val = zValues[idx];
                    if (val >= shLo && val <= shHi &&
                        std::abs(val - selectedValue) <= shMAD) {
                        double score = qualityScores[originalIndices[idx]];
                        if (score > bestScore) {
                            bestScore = score;
                            bestZ = static_cast<uint32_t>(originalIndices[idx]);
                        }
                    }
                }
            }
        }
    }
```

- [ ] **Step 5: Update processPixel signature and call**

Change lines 281-284:
```cpp
float PixelSelector::processPixel(SubCube& cube, size_t y, size_t x,
                                   const double* qualityScores)
{
    auto result = selectBestZ(cube.zColumnPtr(y, x), cube.numSubs(), qualityScores,
                              cube.maskColumnPtr(y, x));
```

- [ ] **Step 6: Update processImage signature and calls**

Change line 291-293 signature and line 317 inner call:
```cpp
std::vector<float> PixelSelector::processImage(SubCube& cube,
                                                const double* qualityScores,
                                                ProgressCallback progress)
```

And line 317:
```cpp
                    auto result = selectBestZ(cube.zColumnPtr(y, x), N, qualityScores,
```

- [ ] **Step 7: Update processImageGPU signature and fallback calls**

Change line 350-352:
```cpp
std::vector<float> PixelSelector::processImageGPU(SubCube& cube,
                                                    const double* qualityScores,
                                                    std::vector<uint8_t>& distTypesOut,
                                                    ProgressCallback progress)
```

Update fallback calls at lines 358, 381, 393:
```cpp
        return processImage(cube, qualityScores, progress);
```

- [ ] **Step 8: Update existing test signatures**

In `tests/unit/test_pixel_selector.cpp`, change all 5 existing tests:
- Replace `std::vector<double> weights(N, X);` with removal of the variable
- Replace `selector.processPixel(cube, y, x, weights)` with `selector.processPixel(cube, y, x)`
- Replace `selector.processImage(cube, weights)` with `selector.processImage(cube, nullptr)`

- [ ] **Step 9: Build and run tests**

Run: `cd build && cmake .. -DPCLDIR=$HOME/PCL && make -j$(nproc) test_pixel_selector && ./test_pixel_selector`
Expected: all 9 tests pass (5 existing + 4 new tiebreaker tests + 1 single-candidate test)

- [ ] **Step 10: Commit**

```bash
git add src/engine/PixelSelector.h src/engine/PixelSelector.cpp tests/unit/test_pixel_selector.cpp
git commit -m "feat: implement metadata tiebreaker in selectBestZ CPU path"
```

---

### Task 3: Remove QualityWeights system

**Files:**
- Delete: `src/engine/QualityWeights.h`
- Delete: `src/engine/QualityWeights.cpp`
- Delete: `tests/unit/test_quality_weights.cpp`
- Modify: `src/NukeXStackInstance.h:20` (remove include)
- Modify: `src/NukeXStackInstance.h:65,87-91` (remove weight params)
- Modify: `src/NukeXStackInstance.cpp:42,60-64,88,105-109` (remove weight init/assign)
- Modify: `src/NukeXStackInstance.cpp:1197-1198,1231-1240` (remove LockParameter cases)
- Modify: `src/NukeXStackParameters.h:59-77,330-393` (remove parameter classes)
- Modify: `src/NukeXStackParameters.cpp:76-113,684-855` (remove implementations)
- Modify: `src/NukeXStackProcess.cpp:34,56-60` (remove parameter registration)
- Modify: `tests/CMakeLists.txt:20-34,267` (remove test and source)
- Modify: `tests/integration/test_full_pipeline.cpp:9` (remove include)

- [ ] **Step 1: Delete QualityWeights files and test**

```bash
rm src/engine/QualityWeights.h src/engine/QualityWeights.cpp tests/unit/test_quality_weights.cpp
```

- [ ] **Step 2: Remove `#include "engine/QualityWeights.h"` from NukeXStackInstance.h**

Remove line 20: `#include "engine/QualityWeights.h"`

- [ ] **Step 3: Remove weight instance parameters from NukeXStackInstance.h**

Remove from the private section (lines 65, 87-91):
```
   pcl_enum p_qualityWeightMode;
   float    p_fwhmWeight;
   float    p_eccentricityWeight;
   float    p_skyBackgroundWeight;
   float    p_hfrWeight;
   float    p_altitudeWeight;
```

Rename `p_enableQualityWeighting` → `p_enableMetadataTiebreaker` (line 70).

- [ ] **Step 4: Remove weight init from NukeXStackInstance.cpp constructor**

Remove from constructor init list (lines 42, 60-64):
```
   , p_qualityWeightMode( NXSQualityWeightMode::Default )
   , p_fwhmWeight( ... )
   , p_eccentricityWeight( ... )
   , p_skyBackgroundWeight( ... )
   , p_hfrWeight( ... )
   , p_altitudeWeight( ... )
```

Rename `p_enableQualityWeighting` → `p_enableMetadataTiebreaker` in init list (line 45).

- [ ] **Step 5: Remove weight Assign in NukeXStackInstance.cpp**

Remove from Assign() (lines 88, 105-109):
```
      p_qualityWeightMode       = x->p_qualityWeightMode;
      p_fwhmWeight              = x->p_fwhmWeight;
      p_eccentricityWeight      = x->p_eccentricityWeight;
      p_skyBackgroundWeight     = x->p_skyBackgroundWeight;
      p_hfrWeight               = x->p_hfrWeight;
      p_altitudeWeight          = x->p_altitudeWeight;
```

Rename `p_enableQualityWeighting` → `p_enableMetadataTiebreaker` (line 91).

- [ ] **Step 6: Remove weight LockParameter cases in NukeXStackInstance.cpp**

Remove (lines 1197-1198, 1231-1240):
```
   if ( p == TheNXSQualityWeightModeParameter )
      return &p_qualityWeightMode;
   if ( p == TheNXSFWHMWeightParameter )
      return &p_fwhmWeight;
   if ( p == TheNXSEccentricityWeightParameter )
      return &p_eccentricityWeight;
   if ( p == TheNXSSkyBackgroundWeightParameter )
      return &p_skyBackgroundWeight;
   if ( p == TheNXSHFRWeightParameter )
      return &p_hfrWeight;
   if ( p == TheNXSAltitudeWeightParameter )
      return &p_altitudeWeight;
```

Rename `TheNXSEnableQualityWeightingParameter` → `TheNXSEnableMetadataTiebreakerParameter` (line 1203-1204).

- [ ] **Step 7: Remove parameter classes from NukeXStackParameters.h**

Remove `NXSQualityWeightMode` class + extern (lines 59-77).
Remove `NXSFWHMWeight`, `NXSEccentricityWeight`, `NXSSkyBackgroundWeight`, `NXSHFRWeight`, `NXSAltitudeWeight` classes + externs (lines 330-393).
Rename `NXSEnableQualityWeighting` → `NXSEnableMetadataTiebreaker` (lines 103-111).

- [ ] **Step 8: Remove parameter implementations from NukeXStackParameters.cpp**

Remove `NXSQualityWeightMode` implementation (lines 76-113).
Remove `NXSFWHMWeight` through `NXSAltitudeWeight` implementations (lines 684-855).
Rename `NXSEnableQualityWeighting` → `NXSEnableMetadataTiebreaker` (lines 159-175). Update Id() to return `"enableMetadataTiebreaker"`.

- [ ] **Step 9: Remove parameter registration from NukeXStackProcess.cpp**

Remove (lines 34, 56-60):
```
   new NXSQualityWeightMode( this );
   new NXSFWHMWeight( this );
   new NXSEccentricityWeight( this );
   new NXSSkyBackgroundWeight( this );
   new NXSHFRWeight( this );
   new NXSAltitudeWeight( this );
```

Rename: `new NXSEnableQualityWeighting( this )` → `new NXSEnableMetadataTiebreaker( this )` (line 39).

Update the process Description() HTML (line 108-109): replace "Quality weighting" bullet with "Metadata tiebreaker" description.

- [ ] **Step 10: Remove QualityWeights from tests/CMakeLists.txt**

Remove the entire `test_quality_weights` target block (lines 20-34).
Remove `${CMAKE_SOURCE_DIR}/src/engine/QualityWeights.cpp` from `test_full_pipeline` sources (line 267).

- [ ] **Step 11: Remove QualityWeights include from test_full_pipeline.cpp**

Remove line 9: `#include "engine/QualityWeights.h"`

Update any `qualityWeights` usage in the test to use `nullptr` instead.

- [ ] **Step 12: Build and run all tests**

Run: `cd build && cmake .. -DPCLDIR=$HOME/PCL && make -j$(nproc) && ctest --output-on-failure`
Expected: all tests pass (one fewer test binary — `test_quality_weights` is gone)

- [ ] **Step 13: Commit**

```bash
git add -A
git commit -m "refactor: remove dead QualityWeights system, rename to MetadataTiebreaker"
```

---

### Task 4: Refactor Phase 2 + fix ComputeFrameMetrics

**Files:**
- Modify: `src/engine/FrameLoader.cpp:588-589` (fix thresholds)
- Modify: `src/NukeXStackInstance.cpp:242-269` (Phase 2 refactor)

- [ ] **Step 1: Fix ComputeFrameMetrics thresholds in FrameLoader.cpp**

Change lines 588-589:
```cpp
        detector.SetSensitivity( 0.1 );
        detector.SetMinSNR( 5.0 );
```

- [ ] **Step 2: Refactor Phase 2 in NukeXStackInstance.cpp**

Replace the Phase 2 block (lines 242-269) with:

```cpp
      // Phase 2: Compute frame metadata scores
      console.WriteLn( "\nPhase 2: Computing frame metadata scores..." );
      console.Flush();
      Module->ProcessEvents();

      std::vector<double> qualityScores( nSubs, 0.0 );
      if ( p_enableMetadataTiebreaker )
      {
         for ( size_t z = 0; z < nSubs; ++z )
         {
            nukex::SubMetadata meta = aligned.alignedCube.metadata( z );
            if ( meta.fwhm > 0 || meta.eccentricity > 0 )
            {
               meta.qualityScore = 0.6 * (1.0 / (1.0 + meta.fwhm))
                                 + 0.4 * (1.0 / (1.0 + meta.eccentricity));
            }
            aligned.alignedCube.setMetadata( z, meta );
            qualityScores[z] = meta.qualityScore;

            console.WriteLn( String().Format(
               "  Sub %d: FWHM=%.2f, Ecc=%.3f, Score=%.4f",
               int( z ), meta.fwhm, meta.eccentricity, meta.qualityScore ) );
         }

         double minScore = *std::min_element( qualityScores.begin(), qualityScores.end() );
         double maxScore = *std::max_element( qualityScores.begin(), qualityScores.end() );
         console.WriteLn( String().Format(
            "  Score range: %.4f \xe2\x80\x94 %.4f", minScore, maxScore ) );
      }
      else
      {
         console.WriteLn( "  Metadata tiebreaker disabled" );
      }
      Module->ProcessEvents();
```

- [ ] **Step 3: Update Phase 3 stacking calls to pass qualityScores**

Find the Phase 3 calls to `selector.processImage()` and `selector.processImageGPU()` in the per-channel stacking loop. Pass `qualityScores.data()` instead of `weights`:

```cpp
// CPU path:
channelResults[ch] = selector.processImage(channelCubes[ch], qualityScores.data(), progressCb);

// GPU path:
channelResults[ch] = selector.processImageGPU(channelCubes[ch], qualityScores.data(), distTypeMaps[ch], progressCb);
```

- [ ] **Step 4: Update Phase 7 remediation calls**

Find the CudaRemediation calls that pass `weights` and replace with `qualityScores`:
The `remediateTrailsGPU` function still takes `qualityWeights` — this will be fixed in Task 5.
For the CPU fallback `selectBestZ` calls in Phase 7, pass `qualityScores.data()` instead of `weights`.

- [ ] **Step 5: Build and run tests**

Run: `cd build && cmake .. -DPCLDIR=$HOME/PCL && make -j$(nproc) && ctest --output-on-failure`
Expected: all tests pass

- [ ] **Step 6: Commit**

```bash
git add src/engine/FrameLoader.cpp src/NukeXStackInstance.cpp
git commit -m "feat: refactor Phase 2 to compute metadata scores, fix star detection thresholds"
```

---

### Task 5: Update UI + CudaRemediation cleanup

**Files:**
- Modify: `src/NukeXStackInterface.h:88-96` (remove weight GUI members)
- Modify: `src/NukeXStackInterface.cpp:150-165,398-416,488-497,623-711` (simplify Quality section)
- Modify: `src/engine/cuda/CudaRemediation.h:31` (remove qualityWeights param)
- Modify: `src/engine/cuda/CudaRemediation.cu:237,241` (remove qualityWeights param)

- [ ] **Step 1: Simplify Quality section in NukeXStackInterface.h**

Replace the Quality Weighting Section GUI members (lines 88-96) with:

```cpp
      // Quality / Metadata Tiebreaker Section
      SectionBar        Quality_SectionBar;
      Control           Quality_Control;
      VerticalSizer     Quality_Sizer;
         CheckBox          EnableMetadataTiebreaker_CheckBox;
```

- [ ] **Step 2: Update UpdateControls() in NukeXStackInterface.cpp**

Replace the quality weighting block (lines 150-165) with:

```cpp
   // Metadata tiebreaker
   GUI->EnableMetadataTiebreaker_CheckBox.SetChecked( m_instance.p_enableMetadataTiebreaker );
```

- [ ] **Step 3: Update e_ComboBoxItemSelected**

Remove the `QualityMode_ComboBox` handler (lines 398-401).

- [ ] **Step 4: Update e_CheckBoxClick**

Replace the `EnableQualityWeighting_CheckBox` handler (lines 408-417) with:

```cpp
   if ( sender == GUI->EnableMetadataTiebreaker_CheckBox )
   {
      m_instance.p_enableMetadataTiebreaker = checked;
   }
```

- [ ] **Step 5: Update e_NumericValueUpdated**

Remove the weight slider handlers (lines 488-497):
```
   else if ( sender == GUI->FWHMWeight_NumericControl )
      ...
   else if ( sender == GUI->AltitudeWeight_NumericControl )
      ...
```

- [ ] **Step 6: Simplify Quality section GUI construction**

Replace the Quality Weighting Section construction (lines 623-712) with:

```cpp
   // =========================================================================
   // Metadata Tiebreaker Section
   // =========================================================================

   Quality_SectionBar.SetTitle( "Metadata Tiebreaker" );
   Quality_SectionBar.SetSection( Quality_Control );

   EnableMetadataTiebreaker_CheckBox.SetText( "Enable Metadata Tiebreaker" );
   EnableMetadataTiebreaker_CheckBox.SetToolTip(
      "<p>When multiple frames produce statistically indistinguishable pixel values, "
      "prefer the frame with better seeing (FWHM) and tracking (eccentricity). "
      "The tiebreaker only affects provenance — the statistical estimate is unchanged.</p>" );
   EnableMetadataTiebreaker_CheckBox.OnClick(
      (Button::click_event_handler)&NukeXStackInterface::e_CheckBoxClick, w );

   Quality_Sizer.SetSpacing( 4 );
   Quality_Sizer.Add( EnableMetadataTiebreaker_CheckBox );

   Quality_Control.SetSizer( Quality_Sizer );
```

- [ ] **Step 7: Remove qualityWeights from CudaRemediation**

In `src/engine/cuda/CudaRemediation.h`, remove `const std::vector<double>& qualityWeights` from `remediateTrailsGPU`:

```cpp
bool remediateTrailsGPU(
   const float* cubeData,
   size_t nSubs, size_t height, size_t width,
   const std::vector<TrailPixel>& trailPixels,
   double trailOutlierSigma,
   float* outputPixels );
```

In `src/engine/cuda/CudaRemediation.cu`, update the function signature and remove the `(void)qualityWeights;` line.

Update any call sites in `NukeXStackInstance.cpp` that pass `weights` / `qualityWeights` to `remediateTrailsGPU`.

- [ ] **Step 8: Build and run tests**

Run: `cd build && cmake .. -DPCLDIR=$HOME/PCL && make -j$(nproc) && ctest --output-on-failure`
Expected: all tests pass

- [ ] **Step 9: Commit**

```bash
git add -A
git commit -m "refactor: simplify Quality UI to single tiebreaker checkbox, remove CudaRemediation qualityWeights"
```

---

### Task 6: GPU tiebreaker in CudaPixelSelector

**Files:**
- Modify: `src/engine/cuda/CudaPixelSelector.h:14-21` (add fields to GpuStackConfig)
- Modify: `src/engine/cuda/CudaPixelSelector.cu` (add tiebreaker to kernel, copy scores)
- Modify: `src/engine/PixelSelector.cpp:368-377` (pass scores to GPU config)

- [ ] **Step 1: Add fields to GpuStackConfig**

```cpp
struct GpuStackConfig {
    int maxOutliers;
    double outlierAlpha;
    bool adaptiveModels;
    bool enableMetadataTiebreaker;
    size_t nSubs;
    size_t height;
    size_t width;
    const double* qualityScores;   // host pointer, will be copied to device
    uint32_t* provenanceOut;       // device output buffer, optional (can be nullptr)
};
```

- [ ] **Step 2: Add tiebreaker to CUDA kernel**

In the CUDA kernel (device function), after shortest-half mode + closest frame selection, add the same tiebreaker logic:
- Compute MAD of shortest-half cluster
- If MAD > 0 and scores pointer is non-null, scan for better-scored alternatives within MAD tolerance
- Use the `qualityScores` array from constant or global memory (copied by host before launch)
- Write `bestZ` to `provenanceOut[pixelIdx]` if non-null

The scores array is small (max 64 doubles = 512 bytes) — copy to `__constant__` memory for fast broadcast access.

- [ ] **Step 3: Update host-side processImageGPU to copy scores**

In `PixelSelector::processImageGPU()` (lines 368-377), add:

```cpp
    gpuConfig.enableMetadataTiebreaker = m_config.enableMetadataTiebreaker;
    gpuConfig.qualityScores = qualityScores;  // host pointer — cuda::processImageGPU will copy to device
    gpuConfig.provenanceOut = nullptr;  // provenance not consumed downstream yet
```

- [ ] **Step 4: Build and run tests**

Run: `cd build && cmake .. -DPCLDIR=$HOME/PCL && make -j$(nproc) && ctest --output-on-failure`
Expected: all tests pass

- [ ] **Step 5: Commit**

```bash
git add src/engine/cuda/CudaPixelSelector.h src/engine/cuda/CudaPixelSelector.cu src/engine/PixelSelector.cpp
git commit -m "feat: add metadata tiebreaker to CUDA pixel selection kernel"
```

---

### Task 7: Integration test update + full build verification

**Files:**
- Modify: `tests/integration/test_full_pipeline.cpp`

- [ ] **Step 1: Update integration test**

In `tests/integration/test_full_pipeline.cpp`:
- Remove `#include "engine/QualityWeights.h"` (line 9)
- Update any `qualityWeights` vector creation → pass `nullptr` to processImage
- Verify the test still exercises the full pipeline correctly

- [ ] **Step 2: Full clean build**

Run: `cd build && cmake .. -DPCLDIR=$HOME/PCL && make clean && make -j$(nproc)`
Expected: clean build, no warnings about removed symbols

- [ ] **Step 3: Run all tests**

Run: `cd build && ctest --output-on-failure`
Expected: all tests pass (count should be one fewer than before — no `test_quality_weights`)

- [ ] **Step 4: Commit**

```bash
git add tests/integration/test_full_pipeline.cpp
git commit -m "test: update integration test for metadata tiebreaker API"
```

---

### Task 8: Version bump + package

Per CLAUDE.md release workflow — MUST follow in order.

- [ ] **Step 1: Bump MODULE_VERSION_BUILD in NukeXModule.cpp**

Increment build number (currently 10 → 11).

- [ ] **Step 2: Update repository/updates.xri**

Update title + description with new version: "v3.1.0.11 — metadata tiebreaker replaces frame weights"

- [ ] **Step 3: Clean rebuild**

Run: `make clean && make release`

- [ ] **Step 4: Run all tests**

Run: `cd build && ctest --output-on-failure`

- [ ] **Step 5: Package**

Run: `make package`
This signs the module, creates tarball, updates SHA1, signs XRI.

- [ ] **Step 6: Commit version bump + package**

```bash
git add -A
git commit -m "build: bump to v3.1.0.11 — metadata tiebreaker replaces frame weights"
```

- [ ] **Step 7: Push**

```bash
git push
```
