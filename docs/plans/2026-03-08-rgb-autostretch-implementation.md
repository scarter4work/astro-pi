# RGB Stacking + Distribution-Aware Auto-Stretch Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Extend NukeXStack to stack RGB subs per-channel and automatically select + apply the best stretch algorithm based on distribution type maps.

**Architecture:** LoadRaw reads all channels into `LoadedFrames.pixelData[frame][channel][pixels]`. Alignment runs on channel 0; offsets are applied to all channels via a new `applyAlignment()` helper. Per-channel PixelSelector produces distType maps that feed `AutoStretchSelector` — a pure decision tree selecting from 11 concrete algorithms. Two output windows: `NukeX_stack` (linear) + `NukeX_stretched` (auto-stretched).

**Tech Stack:** C++17, PCL SDK, xtensor, Boost.Math, OpenMP, Catch2 v3

---

### Task 1: RGB-Aware LoadedFrames + LoadRaw

**Files:**
- Modify: `src/engine/FrameLoader.h:37-42`
- Modify: `src/engine/FrameLoader.cpp:143-259`

**Step 1: Update LoadedFrames struct**

In `src/engine/FrameLoader.h`, change `LoadedFrames`:

```cpp
struct LoadedFrames {
    std::vector<std::vector<std::vector<float>>> pixelData;  // [frame][channel][pixels]
    std::vector<SubMetadata> metadata;
    int width, height, numChannels;
};
```

**Step 2: Update LoadRaw to read all channels**

In `src/engine/FrameLoader.cpp`, modify `LoadRaw()`:

1. After opening first frame, capture `refChannels = images0[0].info.numberOfChannels` and store in `result.numChannels`.
2. Log channel count: `"  Reference: %d x %d, %d channel(s)"`.
3. In the per-frame loop, resize `result.pixelData[i]` to `numChannels` vectors.
4. Replace the single `img.PixelData(0)` copy with a loop over `c = 0..numChannels-1`:
   ```cpp
   for (int c = 0; c < result.numChannels; ++c) {
       const pcl::Image::sample* src = img.PixelData(c);
       result.pixelData[i][c].assign(src, src + numPx);
   }
   ```
5. Log per-frame: filename, channels, dimensions.

**Step 3: Fix all callers of LoadedFrames.pixelData**

In `src/NukeXStackInstance.cpp`, the existing code does:
```cpp
for (const auto& f : raw.pixelData)
    framePtrs.push_back(f.data());
```
This now needs to index channel 0:
```cpp
for (const auto& f : raw.pixelData)
    framePtrs.push_back(f[0].data());  // align on channel 0
```

**Step 4: Build to verify compilation**

Run: `cd /home/scarter4work/projects/nukex3/build && cmake .. -DPCLDIR=$HOME/PCL && make -j$(nproc)`
Expected: compiles cleanly

**Step 5: Run existing tests**

Run: `cd /home/scarter4work/projects/nukex3/build && ctest --output-on-failure`
Expected: All 12 tests pass (no behavioral change for mono)

**Step 6: Commit**

```bash
git add src/engine/FrameLoader.h src/engine/FrameLoader.cpp src/NukeXStackInstance.cpp
git commit -m "feat: RGB-aware LoadRaw reads all channels into LoadedFrames"
```

---

### Task 2: applyAlignment Helper in FrameAligner

**Files:**
- Modify: `src/engine/FrameAligner.h`
- Modify: `src/engine/FrameAligner.cpp`
- Test: `tests/unit/test_frame_aligner.cpp`

**Step 1: Write the failing test**

Add to `tests/unit/test_frame_aligner.cpp`:

```cpp
TEST_CASE("applyAlignment produces aligned SubCube for single channel", "[aligner]") {
    int W = 100, H = 80;
    int nFrames = 3;

    // Create per-frame pixel data with known patterns
    std::vector<std::vector<float>> channelFrameData(nFrames);
    for (int i = 0; i < nFrames; ++i) {
        channelFrameData[i].resize(W * H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                channelFrameData[i][y * W + x] = float(i * 1000 + y * W + x);
    }

    std::vector<nukex::AlignmentResult> offsets = {
        {0, 0, 10, 0.5, true},
        {3, -2, 8, 0.6, true},
        {-1, 4, 9, 0.4, true}
    };

    auto crop = nukex::computeCropRegion(offsets, W, H);
    auto cube = nukex::applyAlignment(channelFrameData, offsets, crop, W, H);

    REQUIRE(cube.numSubs() == 3);
    REQUIRE(cube.width() == static_cast<size_t>(crop.width()));
    REQUIRE(cube.height() == static_cast<size_t>(crop.height()));

    // Verify reference frame (offset 0,0) pixel at (0,0) in crop space
    float expected0 = float(0 * 1000 + crop.y0 * W + crop.x0);
    REQUIRE(cube.pixel(0, 0, 0) == Catch::Approx(expected0));

    // Verify frame 1 (offset 3,-2): source pixel at (crop.x0+0+3, crop.y0+0-2)
    int srcX1 = crop.x0 + 0 + 3;
    int srcY1 = crop.y0 + 0 + (-2);
    float expected1 = float(1 * 1000 + srcY1 * W + srcX1);
    REQUIRE(cube.pixel(1, 0, 0) == Catch::Approx(expected1));
}
```

**Step 2: Run test to verify it fails**

Run: `cd /home/scarter4work/projects/nukex3/build && make -j$(nproc) test_frame_aligner && ./tests/test_frame_aligner`
Expected: FAIL — `applyAlignment` not declared

**Step 3: Declare applyAlignment in header**

Add to `src/engine/FrameAligner.h` before the closing `}` of namespace:

```cpp
// Apply pre-computed alignment offsets to one channel's frame data.
// Returns a SubCube with aligned, cropped pixel data.
SubCube applyAlignment(const std::vector<std::vector<float>>& channelFrameData,
                       const std::vector<AlignmentResult>& offsets,
                       const CropRegion& crop, int width, int height);
```

**Step 4: Implement applyAlignment**

Add to `src/engine/FrameAligner.cpp`:

```cpp
SubCube applyAlignment(const std::vector<std::vector<float>>& channelFrameData,
                       const std::vector<AlignmentResult>& offsets,
                       const CropRegion& crop, int width, int height) {
    size_t nFrames = channelFrameData.size();
    SubCube cube(nFrames, crop.height(), crop.width());

    for (size_t i = 0; i < nFrames; ++i) {
        int dx = offsets[i].dx;
        int dy = offsets[i].dy;
        const float* src = channelFrameData[i].data();

        for (int cy = 0; cy < crop.height(); ++cy) {
            for (int cx = 0; cx < crop.width(); ++cx) {
                int srcX = crop.x0 + cx + dx;
                int srcY = crop.y0 + cy + dy;
                if (srcX >= 0 && srcX < width && srcY >= 0 && srcY < height)
                    cube.setPixel(i, cy, cx, src[srcY * width + srcX]);
            }
        }
    }

    return cube;
}
```

**Step 5: Run test to verify it passes**

Run: `cd /home/scarter4work/projects/nukex3/build && make -j$(nproc) test_frame_aligner && ./tests/test_frame_aligner`
Expected: All frame aligner tests PASS

**Step 6: Commit**

```bash
git add src/engine/FrameAligner.h src/engine/FrameAligner.cpp tests/unit/test_frame_aligner.cpp
git commit -m "feat: add applyAlignment() helper for per-channel aligned SubCube"
```

---

### Task 3: AutoStretchSelector Engine Class

**Files:**
- Create: `src/engine/AutoStretchSelector.h`
- Create: `src/engine/AutoStretchSelector.cpp`
- Create: `tests/unit/test_auto_stretch_selector.cpp`
- Modify: `tests/CMakeLists.txt`

**Step 3.1: Write the failing test**

Create `tests/unit/test_auto_stretch_selector.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include "engine/AutoStretchSelector.h"

using namespace nukex;

// Helper: fill a distType map with a single distribution type
static std::vector<uint8_t> uniformDistMap(size_t size, DistributionType type) {
    return std::vector<uint8_t>(size, static_cast<uint8_t>(type));
}

TEST_CASE("AutoStretchSelector: mostly Gaussian -> MTF", "[autostretch]") {
    size_t sz = 1000;
    auto map = uniformDistMap(sz, DistributionType::Gaussian);
    std::vector<std::vector<uint8_t>> maps = { map, map, map };

    ChannelStats stats;
    stats.median = 0.1;
    stats.mad = 0.01;
    stats.mean = 0.12;
    std::vector<ChannelStats> perChannel = { stats, stats, stats };

    auto result = AutoStretchSelector::Select(maps, perChannel);
    REQUIRE(result.algorithm == StretchAlgorithm::MTF);
}

TEST_CASE("AutoStretchSelector: high Bimodal fraction -> ArcSinh", "[autostretch]") {
    size_t sz = 1000;
    // 20% bimodal, 80% gaussian
    std::vector<uint8_t> map(sz, static_cast<uint8_t>(DistributionType::Gaussian));
    for (size_t i = 0; i < 200; ++i)
        map[i] = static_cast<uint8_t>(DistributionType::Bimodal);

    std::vector<std::vector<uint8_t>> maps = { map, map, map };

    ChannelStats stats;
    stats.median = 0.1;
    stats.mad = 0.01;
    stats.mean = 0.12;
    std::vector<ChannelStats> perChannel = { stats, stats, stats };

    auto result = AutoStretchSelector::Select(maps, perChannel);
    REQUIRE(result.algorithm == StretchAlgorithm::ArcSinh);
}

TEST_CASE("AutoStretchSelector: high Skew-Normal fraction -> GHS", "[autostretch]") {
    size_t sz = 1000;
    // 25% skew-normal, 75% gaussian
    std::vector<uint8_t> map(sz, static_cast<uint8_t>(DistributionType::Gaussian));
    for (size_t i = 0; i < 250; ++i)
        map[i] = static_cast<uint8_t>(DistributionType::SkewNormal);

    std::vector<std::vector<uint8_t>> maps = { map, map, map };

    ChannelStats stats;
    stats.median = 0.1;
    stats.mad = 0.01;
    stats.mean = 0.12;
    std::vector<ChannelStats> perChannel = { stats, stats, stats };

    auto result = AutoStretchSelector::Select(maps, perChannel);
    REQUIRE(result.algorithm == StretchAlgorithm::GHS);
}

TEST_CASE("AutoStretchSelector: high channel divergence -> Lumpton", "[autostretch]") {
    size_t sz = 1000;
    auto mapR = uniformDistMap(sz, DistributionType::Gaussian);
    auto mapG = uniformDistMap(sz, DistributionType::Poisson);  // very different
    auto mapB = uniformDistMap(sz, DistributionType::Gaussian);

    std::vector<std::vector<uint8_t>> maps = { mapR, mapG, mapB };

    ChannelStats stats;
    stats.median = 0.1;
    stats.mad = 0.01;
    stats.mean = 0.12;
    std::vector<ChannelStats> perChannel = { stats, stats, stats };

    auto result = AutoStretchSelector::Select(maps, perChannel);
    REQUIRE(result.algorithm == StretchAlgorithm::Lumpton);
}

TEST_CASE("AutoStretchSelector: mono (1 channel) works", "[autostretch]") {
    size_t sz = 1000;
    auto map = uniformDistMap(sz, DistributionType::Gaussian);
    std::vector<std::vector<uint8_t>> maps = { map };

    ChannelStats stats;
    stats.median = 0.1;
    stats.mad = 0.01;
    stats.mean = 0.12;
    std::vector<ChannelStats> perChannel = { stats };

    auto result = AutoStretchSelector::Select(maps, perChannel);
    // Mono can't have channel divergence, should pick based on distribution
    REQUIRE(result.algorithm == StretchAlgorithm::MTF);
}

TEST_CASE("AutoStretchSelector: high Poisson -> GHS aggressive", "[autostretch]") {
    size_t sz = 1000;
    // 50% Poisson, 50% Gaussian
    std::vector<uint8_t> map(sz, static_cast<uint8_t>(DistributionType::Gaussian));
    for (size_t i = 0; i < 500; ++i)
        map[i] = static_cast<uint8_t>(DistributionType::Poisson);

    std::vector<std::vector<uint8_t>> maps = { map, map, map };

    ChannelStats stats;
    stats.median = 0.05;
    stats.mad = 0.005;
    stats.mean = 0.06;
    std::vector<ChannelStats> perChannel = { stats, stats, stats };

    auto result = AutoStretchSelector::Select(maps, perChannel);
    // High Poisson → GHS (aggressive) or Veralux if outliers present
    REQUIRE((result.algorithm == StretchAlgorithm::GHS ||
             result.algorithm == StretchAlgorithm::Veralux));
}
```

**Step 3.2: Run test to verify it fails**

Run: `cd /home/scarter4work/projects/nukex3/build && cmake .. -DPCLDIR=$HOME/PCL && make -j$(nproc) test_auto_stretch_selector 2>&1 | head -20`
Expected: FAIL — file not found or target not found

**Step 3.3: Create AutoStretchSelector header**

Create `src/engine/AutoStretchSelector.h`:

```cpp
#pragma once

#include "engine/DistributionFitter.h"
#include <vector>
#include <string>
#include <cstdint>

namespace nukex {

// Maps to pcl::AlgorithmType values for interop with StretchLibrary
enum class StretchAlgorithm {
    MTF = 0, Histogram, GHS, ArcSinh, Log,
    Lumpton, RNC, Photometric, OTS, SAS, Veralux
};

struct ChannelStats {
    double median = 0;
    double mad = 0;
    double mean = 0;
};

struct StretchSelection {
    StretchAlgorithm algorithm;
    std::string reason;
    // Per-channel distribution fractions (for logging)
    struct ChannelFractions {
        double gaussian, poisson, skewNormal, bimodal;
    };
    std::vector<ChannelFractions> fractions;
    double channelDivergence = 0;
};

class AutoStretchSelector {
public:
    // Select the best stretch algorithm from distribution type maps.
    // distTypeMaps: one vector<uint8_t> per channel (H*W elements each)
    // perChannelStats: median/MAD/mean per channel from the stacked result
    static StretchSelection Select(
        const std::vector<std::vector<uint8_t>>& distTypeMaps,
        const std::vector<ChannelStats>& perChannelStats);

private:
    struct DistFractions {
        double gaussian = 0, poisson = 0, skewNormal = 0, bimodal = 0;
    };

    static DistFractions computeFractions(const std::vector<uint8_t>& map);
    static double computeDivergence(const std::vector<DistFractions>& perChannel);
    static bool hasBrightOutliers(const std::vector<ChannelStats>& stats);
};

} // namespace nukex
```

**Step 3.4: Implement AutoStretchSelector**

Create `src/engine/AutoStretchSelector.cpp`:

```cpp
#include "engine/AutoStretchSelector.h"
#include <algorithm>
#include <cmath>

namespace nukex {

AutoStretchSelector::DistFractions
AutoStretchSelector::computeFractions(const std::vector<uint8_t>& map) {
    DistFractions f{};
    if (map.empty()) return f;

    size_t counts[4] = {0, 0, 0, 0};
    for (uint8_t t : map) {
        if (t < 4) counts[t]++;
    }

    double total = static_cast<double>(map.size());
    f.gaussian   = counts[0] / total;
    f.poisson    = counts[1] / total;
    f.skewNormal = counts[2] / total;
    f.bimodal    = counts[3] / total;
    return f;
}

double AutoStretchSelector::computeDivergence(
    const std::vector<DistFractions>& perChannel) {
    if (perChannel.size() < 2) return 0.0;

    // Max cross-channel difference in any distribution fraction
    double maxDiff = 0.0;
    for (size_t i = 0; i < perChannel.size(); ++i) {
        for (size_t j = i + 1; j < perChannel.size(); ++j) {
            maxDiff = std::max(maxDiff, std::abs(perChannel[i].gaussian   - perChannel[j].gaussian));
            maxDiff = std::max(maxDiff, std::abs(perChannel[i].poisson    - perChannel[j].poisson));
            maxDiff = std::max(maxDiff, std::abs(perChannel[i].skewNormal - perChannel[j].skewNormal));
            maxDiff = std::max(maxDiff, std::abs(perChannel[i].bimodal    - perChannel[j].bimodal));
        }
    }
    return maxDiff;
}

bool AutoStretchSelector::hasBrightOutliers(const std::vector<ChannelStats>& stats) {
    for (const auto& s : stats) {
        if (s.mad > 0 && (s.mean - s.median) / s.mad > 3.0)
            return true;
    }
    return false;
}

StretchSelection AutoStretchSelector::Select(
    const std::vector<std::vector<uint8_t>>& distTypeMaps,
    const std::vector<ChannelStats>& perChannelStats)
{
    StretchSelection result;
    size_t nCh = distTypeMaps.size();

    // 1. Compute per-channel distribution fractions
    std::vector<DistFractions> fracs(nCh);
    for (size_t c = 0; c < nCh; ++c)
        fracs[c] = computeFractions(distTypeMaps[c]);

    // Store fractions for logging
    result.fractions.resize(nCh);
    for (size_t c = 0; c < nCh; ++c) {
        result.fractions[c].gaussian   = fracs[c].gaussian;
        result.fractions[c].poisson    = fracs[c].poisson;
        result.fractions[c].skewNormal = fracs[c].skewNormal;
        result.fractions[c].bimodal    = fracs[c].bimodal;
    }

    // 2. Compute channel divergence
    result.channelDivergence = computeDivergence(fracs);

    // 3. Find max fractions across channels
    double maxBimodal = 0, maxSkewNormal = 0, maxPoisson = 0, maxGaussian = 0;
    for (const auto& f : fracs) {
        maxBimodal    = std::max(maxBimodal,    f.bimodal);
        maxSkewNormal = std::max(maxSkewNormal, f.skewNormal);
        maxPoisson    = std::max(maxPoisson,    f.poisson);
        maxGaussian   = std::max(maxGaussian,   f.gaussian);
    }

    // 4. Decision tree
    if (result.channelDivergence > 0.15) {
        result.algorithm = StretchAlgorithm::Lumpton;
        result.reason = "High channel divergence — Lumpton preserves color ratios";
    }
    else if (maxBimodal > 0.15) {
        result.algorithm = StretchAlgorithm::ArcSinh;
        result.reason = "High bimodal fraction indicates HDR/two-population pixels";
    }
    else if (maxSkewNormal > 0.20) {
        result.algorithm = StretchAlgorithm::GHS;
        result.reason = "High skew-normal fraction indicates nebulosity";
    }
    else if (maxPoisson > 0.40 && hasBrightOutliers(perChannelStats)) {
        result.algorithm = StretchAlgorithm::Veralux;
        result.reason = "High Poisson with bright outliers — Veralux for faint + highlights";
    }
    else if (maxPoisson > 0.40) {
        result.algorithm = StretchAlgorithm::GHS;
        result.reason = "High Poisson fraction — GHS with aggressive parameters";
    }
    else if (nCh >= 3 && result.channelDivergence < 0.05) {
        result.algorithm = StretchAlgorithm::RNC;
        result.reason = "Channels similar, broadband RGB — RNC for natural color";
    }
    else if (maxGaussian > 0.70) {
        result.algorithm = StretchAlgorithm::MTF;
        result.reason = "Mostly Gaussian — clean data, MTF is simple and effective";
    }
    else {
        result.algorithm = StretchAlgorithm::GHS;
        result.reason = "Fallback — GHS is the most versatile algorithm";
    }

    return result;
}

} // namespace nukex
```

**Step 3.5: Add test target to CMakeLists.txt**

Add to `tests/CMakeLists.txt`:

```cmake
# AutoStretchSelector unit tests
add_executable(test_auto_stretch_selector
    unit/test_auto_stretch_selector.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/AutoStretchSelector.cpp
)
target_link_libraries(test_auto_stretch_selector PRIVATE Catch2::Catch2WithMain)
target_include_directories(test_auto_stretch_selector PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_features(test_auto_stretch_selector PRIVATE cxx_std_17)
add_test(NAME test_auto_stretch_selector COMMAND test_auto_stretch_selector)
```

**Step 3.6: Build and run tests**

Run: `cd /home/scarter4work/projects/nukex3/build && cmake .. -DPCLDIR=$HOME/PCL && make -j$(nproc) test_auto_stretch_selector && ./tests/test_auto_stretch_selector -v`
Expected: All 6 AutoStretchSelector tests PASS

**Step 3.7: Run full test suite**

Run: `cd /home/scarter4work/projects/nukex3/build && ctest --output-on-failure`
Expected: 13 tests pass (12 existing + 1 new)

**Step 3.8: Commit**

```bash
git add src/engine/AutoStretchSelector.h src/engine/AutoStretchSelector.cpp \
        tests/unit/test_auto_stretch_selector.cpp tests/CMakeLists.txt
git commit -m "feat: add AutoStretchSelector — distribution-aware algorithm selection"
```

---

### Task 4: Auto-Stretch Parameter + GUI Checkbox

**Files:**
- Modify: `src/NukeXStackParameters.h`
- Modify: `src/NukeXStackParameters.cpp`
- Modify: `src/NukeXStackInstance.h`
- Modify: `src/NukeXStackInstance.cpp` (constructor, Assign, LockParameter only — NOT ExecuteGlobal yet)
- Modify: `src/NukeXStackInterface.h`
- Modify: `src/NukeXStackInterface.cpp`

**Step 1: Add parameter class in NukeXStackParameters.h**

After the `NXSEnableQualityWeighting` class, add:

```cpp
class NXSEnableAutoStretch : public MetaBoolean
{
public:
   NXSEnableAutoStretch( MetaProcess* );
   IsoString Id() const override;
   bool DefaultValue() const override;
};

extern NXSEnableAutoStretch* TheNXSEnableAutoStretchParameter;
```

**Step 2: Implement parameter in NukeXStackParameters.cpp**

After `NXSEnableQualityWeighting` implementation, add:

```cpp
NXSEnableAutoStretch* TheNXSEnableAutoStretchParameter = nullptr;

NXSEnableAutoStretch::NXSEnableAutoStretch( MetaProcess* P )
   : MetaBoolean( P )
{
   TheNXSEnableAutoStretchParameter = this;
}

IsoString NXSEnableAutoStretch::Id() const
{
   return "enableAutoStretch";
}

bool NXSEnableAutoStretch::DefaultValue() const
{
   return true;
}
```

**Step 3: Register parameter in NukeXStackProcess.cpp**

Find where existing parameters are constructed (in `NukeXStackProcess` constructor) and add:
```cpp
new NXSEnableAutoStretch( this );
```

**Step 4: Add instance member in NukeXStackInstance.h**

Add to private section:
```cpp
pcl_bool p_enableAutoStretch;
```

**Step 5: Wire up Instance constructor, Assign, LockParameter**

In constructor init list:
```cpp
, p_enableAutoStretch( TheNXSEnableAutoStretchParameter->DefaultValue() )
```

In `Assign()`:
```cpp
p_enableAutoStretch = x->p_enableAutoStretch;
```

In `LockParameter()`:
```cpp
if ( p == TheNXSEnableAutoStretchParameter )
    return &p_enableAutoStretch;
```

**Step 6: Add GUI checkbox in NukeXStackInterface.h**

In the `GUIData` struct, inside the Output section, add:
```cpp
CheckBox EnableAutoStretch_CheckBox;
```

**Step 7: Wire up GUI in NukeXStackInterface.cpp**

In GUIData constructor (Output section), after `GenerateDistMetadata_CheckBox` setup:

```cpp
EnableAutoStretch_CheckBox.SetText( "Auto-Stretch Output" );
EnableAutoStretch_CheckBox.SetToolTip( "<p>Automatically select and apply the best stretch "
                                       "algorithm based on per-pixel distribution statistics. "
                                       "Creates a second output window (NukeX_stretched).</p>" );
EnableAutoStretch_CheckBox.OnClick( (Button::click_event_handler)&NukeXStackInterface::e_CheckBoxClick, w );
```

Add to `Output_Sizer`:
```cpp
Output_Sizer.Add( EnableAutoStretch_CheckBox );
```

In `UpdateControls()`:
```cpp
GUI->EnableAutoStretch_CheckBox.SetChecked( m_instance.p_enableAutoStretch );
```

In `e_CheckBoxClick()`:
```cpp
else if ( sender == GUI->EnableAutoStretch_CheckBox )
{
    m_instance.p_enableAutoStretch = checked;
}
```

**Step 8: Build and verify**

Run: `cd /home/scarter4work/projects/nukex3/build && cmake .. -DPCLDIR=$HOME/PCL && make -j$(nproc)`
Expected: compiles cleanly

**Step 9: Commit**

```bash
git add src/NukeXStackParameters.h src/NukeXStackParameters.cpp \
        src/NukeXStackProcess.cpp src/NukeXStackInstance.h src/NukeXStackInstance.cpp \
        src/NukeXStackInterface.h src/NukeXStackInterface.cpp
git commit -m "feat: add enableAutoStretch parameter and GUI checkbox"
```

---

### Task 5: RGB ExecuteGlobal Pipeline + Verbose Logging

This is the big one — rewrites `ExecuteGlobal()` in `NukeXStackInstance.cpp` to handle RGB per-channel stacking with the narrative logging from the design doc.

**Files:**
- Modify: `src/NukeXStackInstance.cpp`
- Modify: `src/NukeXStackInstance.h` (add `#include` for AutoStretchSelector and StretchLibrary)

**Step 1: Add includes**

In `src/NukeXStackInstance.h`, add:
```cpp
#include "engine/AutoStretchSelector.h"
```

In `src/NukeXStackInstance.cpp`, add:
```cpp
#include "engine/AutoStretchSelector.h"
#include "engine/StretchLibrary.h"
#include <chrono>
```

**Step 2: Rewrite ExecuteGlobal**

Replace the `ExecuteGlobal()` body with the full RGB pipeline. Key structure:

```cpp
bool NukeXStackInstance::ExecuteGlobal()
{
   try
   {
      auto t0 = std::chrono::steady_clock::now();

      Console console;
      console.WriteLn( "<end><cbr>"
         "\n\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
         "\n  NukeX v3 \xe2\x80\x94 Per-Pixel Statistical Inference Stacking"
         "\n\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90" );

      // -- Phase 1: Load frames --
      auto tPhase1 = std::chrono::steady_clock::now();
      // ... filter enabled frames ...
      console.WriteLn( String().Format( "\nPhase 1: Loading %d frames...", int(framePaths.size()) ) );
      console.Flush();
      Module->ProcessEvents();

      nukex::LoadedFrames raw = nukex::FrameLoader::LoadRaw( framePaths );
      int numChannels = raw.numChannels;
      const char* chNames[] = { "R", "G", "B" };
      if (numChannels == 1) chNames[0] = "L";  // mono

      auto elapsed1 = std::chrono::steady_clock::now() - tPhase1;
      console.WriteLn( String().Format( "  Loaded %d frames in %.1fs",
          int(framePaths.size()),
          std::chrono::duration<double>(elapsed1).count() ) );
      Module->ProcessEvents();

      // -- Phase 1b: Align (channel 0 only) --
      auto tPhase1b = std::chrono::steady_clock::now();
      console.WriteLn( "\nPhase 1b: Aligning frames..." );
      console.Flush();
      Module->ProcessEvents();

      std::vector<const float*> framePtrs;
      for ( const auto& f : raw.pixelData )
          framePtrs.push_back( f[0].data() );  // channel 0

      nukex::AlignmentOutput aligned = nukex::alignFrames(
          framePtrs, raw.width, raw.height );

      // Log alignment details
      for (size_t i = 0; i < aligned.offsets.size(); ++i) {
          const auto& o = aligned.offsets[i];
          console.WriteLn( String().Format( "  [%d/%d] dx=%+d, dy=%+d (%d stars, RMS=%.2f)",
              int(i+1), int(aligned.offsets.size()),
              o.dx, o.dy, o.matchedStars, o.rms ) );
      }
      console.WriteLn( String().Format( "  Crop: %d x %d (from %d x %d)",
          aligned.crop.width(), aligned.crop.height(), raw.width, raw.height ) );

      auto elapsed1b = std::chrono::steady_clock::now() - tPhase1b;
      console.WriteLn( String().Format( "  Alignment complete in %.1fs",
          std::chrono::duration<double>(elapsed1b).count() ) );
      Module->ProcessEvents();

      // Copy metadata into first cube for quality weights
      for ( size_t i = 0; i < raw.metadata.size(); ++i )
          aligned.alignedCube.setMetadata( i, raw.metadata[i] );

      // -- Phase 2: Quality weights --
      console.WriteLn( "\nPhase 2: Computing quality weights..." );
      console.Flush();
      Module->ProcessEvents();

      std::vector<double> weights;
      // ... existing quality weight code ...

      console.WriteLn( String().Format( "  Weight range: %.2f \xe2\x80\x94 %.2f",
          *std::min_element(weights.begin(), weights.end()),
          *std::max_element(weights.begin(), weights.end()) ) );
      Module->ProcessEvents();

      // -- Phase 3: Per-channel stacking --
      auto tPhase3 = std::chrono::steady_clock::now();
      int cropW = aligned.crop.width();
      int cropH = aligned.crop.height();
      size_t nSubs = aligned.offsets.size();

      console.WriteLn( "\nPhase 3: Per-channel stacking..." );
      console.WriteLn( String().Format( "  Image: %d x %d, %d subs, %d channel(s)",
          cropW, cropH, int(nSubs), numChannels ) );
      console.Flush();
      Module->ProcessEvents();

      // Results: one flat pixel array + one distType map per channel
      std::vector<std::vector<float>> channelResults(numChannels);
      std::vector<std::vector<uint8_t>> distTypeMaps(numChannels);

      nukex::PixelSelector::Config selConfig;
      selConfig.maxOutliers = static_cast<int>( p_outlierSigmaThreshold );
      nukex::PixelSelector selector( selConfig );

      for (int ch = 0; ch < numChannels; ++ch) {
          console.WriteLn( String().Format( "  Channel %s (%d/%d):",
              chNames[ch], ch+1, numChannels ) );
          console.Flush();
          Module->ProcessEvents();

          // Build per-channel frame data for alignment
          std::vector<std::vector<float>> chFrameData(nSubs);
          for (size_t f = 0; f < nSubs; ++f)
              chFrameData[f] = raw.pixelData[f][ch];

          // For channel 0, reuse the already-aligned cube
          nukex::SubCube cube = (ch == 0)
              ? std::move(aligned.alignedCube)
              : nukex::applyAlignment(chFrameData, aligned.offsets,
                                       aligned.crop, raw.width, raw.height);

          // Copy metadata (needed for quality weights in processImage)
          if (ch > 0)
              for (size_t i = 0; i < raw.metadata.size(); ++i)
                  cube.setMetadata(i, raw.metadata[i]);

          // Stack
          channelResults[ch] = selector.processImage(cube, weights);

          // Extract distType map
          size_t mapSize = size_t(cropH) * size_t(cropW);
          distTypeMaps[ch].resize(mapSize);
          for (size_t y = 0; y < size_t(cropH); ++y)
              for (size_t x = 0; x < size_t(cropW); ++x)
                  distTypeMaps[ch][y * cropW + x] = cube.distType(y, x);

          // Log distribution summary
          size_t counts[4] = {};
          for (uint8_t t : distTypeMaps[ch])
              if (t < 4) counts[t]++;
          console.WriteLn( String().Format(
              "    Distribution: %.0f%% Gaussian, %.0f%% Poisson, %.0f%% Skew-Normal, %.0f%% Bimodal",
              100.0*counts[0]/mapSize, 100.0*counts[1]/mapSize,
              100.0*counts[2]/mapSize, 100.0*counts[3]/mapSize ) );
          console.Flush();
          Module->ProcessEvents();

          // cube goes out of scope here, freeing memory
      }

      // Free raw frame data
      raw.pixelData.clear();
      raw.pixelData.shrink_to_fit();

      auto elapsed3 = std::chrono::steady_clock::now() - tPhase3;
      console.WriteLn( String().Format( "  Stacking complete in %.1fs",
          std::chrono::duration<double>(elapsed3).count() ) );
      Module->ProcessEvents();

      // -- Phase 4: Create linear output --
      console.WriteLn( "\nPhase 4: Creating linear output..." );
      console.Flush();
      Module->ProcessEvents();

      bool isColor = (numChannels >= 3);
      ImageWindow window( cropW, cropH,
          isColor ? 3 : 1,  // channels
          32, true, isColor, "NukeX_stack" );
      if ( window.IsNull() )
          throw Error( "Failed to create output image window." );

      View mainView = window.MainView();
      ImageVariant v = mainView.Image();
      Image& outputImage = static_cast<Image&>( *v );

      for (int ch = 0; ch < (isColor ? 3 : 1); ++ch) {
          int srcCh = (ch < numChannels) ? ch : 0;
          for (int y = 0; y < cropH; ++y)
              for (int x = 0; x < cropW; ++x)
                  outputImage.Pixel(x, y, ch) = channelResults[srcCh][y * cropW + x];
      }

      window.Show();
      window.ZoomToFit();
      console.WriteLn( String().Format( "  Window: NukeX_stack (%d x %d, %s)",
          cropW, cropH, isColor ? "RGB" : "Mono" ) );
      Module->ProcessEvents();

      // -- Phase 5 & 6: Auto-stretch (if enabled) --
      if ( p_enableAutoStretch )
      {
          console.WriteLn( "\nPhase 5: Auto-stretch selection..." );
          console.Flush();
          Module->ProcessEvents();

          // Compute per-channel stats from stacked result
          std::vector<nukex::ChannelStats> chStats(isColor ? 3 : 1);
          for (int ch = 0; ch < (isColor ? 3 : 1); ++ch) {
              int srcCh = (ch < numChannels) ? ch : 0;
              const auto& px = channelResults[srcCh];
              size_t n = px.size();

              // Mean
              double sum = 0;
              for (float v : px) sum += v;
              chStats[ch].mean = sum / n;

              // Median (copy + sort)
              std::vector<float> sorted = px;
              std::sort(sorted.begin(), sorted.end());
              chStats[ch].median = sorted[n / 2];

              // MAD
              std::vector<float> deviations(n);
              for (size_t i = 0; i < n; ++i)
                  deviations[i] = std::abs(sorted[i] - float(chStats[ch].median));
              std::sort(deviations.begin(), deviations.end());
              chStats[ch].mad = deviations[n / 2];
          }

          auto selection = nukex::AutoStretchSelector::Select(distTypeMaps, chStats);

          // Log selection
          const char* chLabels[] = { "R", "G", "B" };
          if (!isColor) chLabels[0] = "L";
          for (size_t c = 0; c < selection.fractions.size(); ++c) {
              const auto& f = selection.fractions[c];
              console.WriteLn( String().Format(
                  "  %s: %.0f%% Gaussian, %.0f%% Poisson, %.0f%% Skew-Normal, %.0f%% Bimodal",
                  chLabels[c], f.gaussian*100, f.poisson*100, f.skewNormal*100, f.bimodal*100 ) );
          }
          console.WriteLn( String().Format( "  Channel divergence: %.2f (%s)",
              selection.channelDivergence,
              selection.channelDivergence < 0.05 ? "similar" :
              selection.channelDivergence < 0.15 ? "moderate" : "divergent" ) );
          console.WriteLn( String( "  Selected: " ) + String( selection.reason.c_str() ) );
          Module->ProcessEvents();

          // Phase 6: Apply stretch
          console.WriteLn( "\nPhase 6: Applying stretch..." );
          console.Flush();
          Module->ProcessEvents();

          // Map nukex::StretchAlgorithm to pcl::AlgorithmType
          AlgorithmType algoType = static_cast<AlgorithmType>( static_cast<int>(selection.algorithm) );

          auto algo = StretchLibrary::Instance().Create( algoType );
          if ( algo == nullptr ) {
              console.WarningLn( "  Warning: algorithm not available, falling back to GHS" );
              algo = StretchLibrary::Instance().Create( AlgorithmType::GHS );
          }

          console.WriteLn( String( "  Algorithm: " ) + algo->Name() );

          // Clone linear image into stretched window
          ImageWindow stretchWindow( cropW, cropH,
              isColor ? 3 : 1, 32, true, isColor, "NukeX_stretched" );
          if ( stretchWindow.IsNull() )
              throw Error( "Failed to create stretched output window." );

          View stretchView = stretchWindow.MainView();
          ImageVariant sv = stretchView.Image();
          Image& stretchImage = static_cast<Image&>( *sv );

          // Copy linear data
          for (int ch = 0; ch < (isColor ? 3 : 1); ++ch)
              for (int y = 0; y < cropH; ++y)
                  for (int x = 0; x < cropW; ++x)
                      stretchImage.Pixel(x, y, ch) = outputImage.Pixel(x, y, ch);

          // Auto-configure and apply
          double med = stretchImage.Median();
          double mad = stretchImage.MAD( med );
          console.WriteLn( String().Format( "  Image median: %.6f, MAD: %.6f", med, mad ) );

          algo->AutoConfigure( med, mad );

          // Log parameters
          auto params = algo->GetParameters();
          for (const auto& p : params)
              console.WriteLn( String().Format( "  %s = %.4f",
                  IsoString(p.name).c_str(), p.value ) );

          algo->ApplyToImage( stretchImage );

          stretchWindow.Show();
          stretchWindow.ZoomToFit();
          console.WriteLn( String().Format( "  Window: NukeX_stretched (%d x %d, %s)",
              cropW, cropH, isColor ? "RGB" : "Mono" ) );
          Module->ProcessEvents();
      }

      // Final banner
      auto totalElapsed = std::chrono::steady_clock::now() - t0;
      double totalSec = std::chrono::duration<double>(totalElapsed).count();
      int minutes = int(totalSec) / 60;
      double seconds = totalSec - minutes * 60;

      console.WriteLn( String().Format(
          "\n\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
          "\n  NukeX stacking complete \xe2\x80\x94 %dm %.0fs total"
          "\n\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90",
          minutes, seconds ) );

      return true;
   }
   catch ( ... ) { /* keep existing catch blocks */ }
}
```

**Important implementation notes:**
- The UTF-8 `\xe2\x95\x90` sequences are the `═` character for the banner lines
- The UTF-8 `\xe2\x80\x94` is the em-dash `—`
- For channel 0 we reuse `aligned.alignedCube` (moved), for channels 1+ we call `applyAlignment()`
- Memory is freed after each channel's SubCube goes out of scope
- All `Console` and `ProcessEvents()` calls are on the main thread (never inside OpenMP regions)

**Step 3: Build to verify compilation**

Run: `cd /home/scarter4work/projects/nukex3/build && cmake .. -DPCLDIR=$HOME/PCL && make -j$(nproc)`
Expected: compiles cleanly

**Step 4: Commit**

```bash
git add src/NukeXStackInstance.h src/NukeXStackInstance.cpp
git commit -m "feat: RGB per-channel stacking pipeline with auto-stretch and verbose logging"
```

---

### Task 6: Build, Sign, Package, Update Repository

**Files:**
- Modify: `src/NukeXModule.cpp` (bump version)
- Modify: `repository/updates.xri` (new SHA1, title, date)
- Replace: `repository/20260308-linux-x64-NukeX.tar.gz`

**Step 1: Bump module version**

In `src/NukeXModule.cpp`, change:
```cpp
#define MODULE_VERSION_BUILD     8
```

**Step 2: Full build**

```bash
cd /home/scarter4work/projects/nukex3/build && cmake .. -DPCLDIR=$HOME/PCL && make -j$(nproc)
```

**Step 3: Run all tests**

```bash
ctest --output-on-failure
```
Expected: 13 tests pass

**Step 4: Sign module**

```bash
/opt/PixInsight/bin/PixInsight.sh --sign-module-file=/home/scarter4work/projects/nukex3/build/lib/NukeX-pxm.so \
    --xssk-file=/home/scarter4work/projects/keys/scarter4work_keys.xssk \
    --xssk-password="***REDACTED***"
```

**Step 5: Package tarball**

```bash
rm -rf /tmp/nukex-pkg && mkdir -p /tmp/nukex-pkg/bin
cp /home/scarter4work/projects/nukex3/build/lib/NukeX-pxm.so /tmp/nukex-pkg/bin/
cp /home/scarter4work/projects/nukex3/build/lib/NukeX-pxm.xsgn /tmp/nukex-pkg/bin/
cd /tmp/nukex-pkg && tar czf /home/scarter4work/projects/nukex3/repository/20260308-linux-x64-NukeX.tar.gz bin/
```

**Step 6: Get SHA1 of new tarball**

```bash
sha1sum /home/scarter4work/projects/nukex3/repository/20260308-linux-x64-NukeX.tar.gz
```

**Step 7: Update updates.xri**

Update `sha1`, title to "NukeX 3.0.0.8", version description to mention RGB stacking + auto-stretch. Strip old `<Signature>` block before re-signing.

**Step 8: Re-sign updates.xri**

```bash
/opt/PixInsight/bin/PixInsight.sh --sign-xml-file=/home/scarter4work/projects/nukex3/repository/updates.xri \
    --xssk-file=/home/scarter4work/projects/keys/scarter4work_keys.xssk \
    --xssk-password="***REDACTED***"
```

**Step 9: Install locally for testing**

```bash
sudo cp /home/scarter4work/projects/nukex3/build/lib/NukeX-pxm.so /opt/PixInsight/bin/
sudo cp /home/scarter4work/projects/nukex3/build/lib/NukeX-pxm.xsgn /opt/PixInsight/bin/
```

**Step 10: Commit**

```bash
git add -A
git commit -m "release: NukeX v3.0.0.8 — RGB stacking + distribution-aware auto-stretch"
```

**Step 11: Push**

```bash
git push origin main
```

---

## Summary of All Files Changed

| File | Action | Description |
|------|--------|-------------|
| `src/engine/FrameLoader.h` | Modify | `LoadedFrames` gets 3D pixelData + numChannels |
| `src/engine/FrameLoader.cpp` | Modify | `LoadRaw` reads all channels |
| `src/engine/FrameAligner.h` | Modify | Add `applyAlignment()` declaration |
| `src/engine/FrameAligner.cpp` | Modify | Add `applyAlignment()` implementation |
| `src/engine/AutoStretchSelector.h` | Create | Decision tree header |
| `src/engine/AutoStretchSelector.cpp` | Create | Decision tree implementation |
| `src/NukeXStackParameters.h` | Modify | Add `NXSEnableAutoStretch` parameter |
| `src/NukeXStackParameters.cpp` | Modify | Implement `NXSEnableAutoStretch` |
| `src/NukeXStackProcess.cpp` | Modify | Register new parameter |
| `src/NukeXStackInstance.h` | Modify | Add `p_enableAutoStretch` member + new includes |
| `src/NukeXStackInstance.cpp` | Modify | Full RGB pipeline + auto-stretch + logging |
| `src/NukeXStackInterface.h` | Modify | Add auto-stretch checkbox |
| `src/NukeXStackInterface.cpp` | Modify | Wire up auto-stretch checkbox |
| `src/NukeXModule.cpp` | Modify | Bump to v3.0.0.8 |
| `tests/unit/test_frame_aligner.cpp` | Modify | Add applyAlignment test |
| `tests/unit/test_auto_stretch_selector.cpp` | Create | 6 decision tree tests |
| `tests/CMakeLists.txt` | Modify | Add AutoStretchSelector test target |
| `repository/updates.xri` | Modify | New SHA1, version, description |
| `repository/20260308-linux-x64-NukeX.tar.gz` | Replace | New package |
