# Minimal Flat Calibration — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add flat field calibration to NukeX Phase 1. User provides raw flat frames, NukeX median-stacks them into a master flat, normalizes it, and divides each light frame after debayer. Removes dust motes, vignetting, and optical path illumination in one operation.

**Architecture:** New `FlatCalibrator` engine class handles flat stacking and calibration. FrameLoader calls it after debayering each light. The NukeXStack interface/parameters add a flat file list. Filter matching is automatic via FITS FILTER keyword.

**Tech Stack:** C++17, PCL FileFormat API (FITS loading), Catch2 v3 (tests)

---

## Design Decisions

1. **Median stack the flats** — robust to outliers (dust hitting sensor during flat acquisition)
2. **Normalize by dividing by the median pixel value** — master flat values ≈ 1.0 everywhere, <1.0 at dust/vignetting
3. **Divide BEFORE debayer** — calibrate at the raw CFA level, matching what PI's ImageCalibration does. This means the flat and lights must have the same Bayer pattern.
4. **Filter matching** — read FILTER keyword from flat and light FITS headers. If they match, use that flat. If no FILTER keyword, use the flat as-is (broadband assumption).
5. **One master flat per filter** — if user provides flats for multiple filters, stack each set separately and match to lights by filter.

**Wait — debayer timing matters.** NukeX currently debayers in Phase 1 during loading. Flat calibration should happen BEFORE debayer (on raw CFA data) for correctness. But the current FrameLoader does load → debayer in one pass. We have two options:

- **Option A**: Calibrate before debayer (correct but requires refactoring FrameLoader to separate load/calibrate/debayer)
- **Option B**: Calibrate after debayer (simpler — debayer the flat too, then divide channel-by-channel)

**Going with Option B** — debayer the master flat the same way as the lights, then divide per-channel. This is mathematically equivalent for bilinear debayer and avoids refactoring FrameLoader. PI's WBPP also supports post-debayer flat calibration.

---

## File Structure

| File | Action | Responsibility |
|------|--------|----------------|
| `src/engine/FlatCalibrator.h` | Create | `FlatCalibrator` class — loads flats, stacks, normalizes, applies |
| `src/engine/FlatCalibrator.cpp` | Create | Implementation: median stack, normalization, per-pixel division |
| `src/NukeXStackInstance.cpp` | Modify | Phase 1: call FlatCalibrator after debayer if flats provided |
| `src/NukeXStackParameters.h` | Modify | Add flat file list parameter |
| `src/NukeXStackParameters.cpp` | Modify | Register flat file list parameter |
| `src/NukeXStackInstance.h` | Modify | Add flat file list member |
| `src/NukeXStackInterface.cpp` | Modify | Add flat file picker to UI (TreeBox) |
| `src/NukeXStackInterface.h` | Modify | Add flat UI controls |
| `src/NukeXStackProcess.cpp` | Modify | Register flat parameter |
| `tests/unit/test_flat_calibrator.cpp` | Create | Tests for stacking, normalization, calibration |
| `tests/CMakeLists.txt` | Modify | Add test_flat_calibrator target |

---

## Task 1: Create FlatCalibrator engine class

**Files:**
- Create: `src/engine/FlatCalibrator.h`
- Create: `src/engine/FlatCalibrator.cpp`
- Test: `tests/unit/test_flat_calibrator.cpp`
- Modify: `tests/CMakeLists.txt`

### FlatCalibrator API

```cpp
// src/engine/FlatCalibrator.h
#pragma once
#include <vector>
#include <string>
#include <functional>

namespace nukex {

using LogCallback = std::function<void( const std::string& )>;

class FlatCalibrator
{
public:
   // Load raw flat frames, debayer, median-stack per channel, normalize.
   // Returns false if loading fails.
   bool loadFlats( const std::vector<std::string>& flatPaths,
                   int expectedWidth, int expectedHeight,
                   LogCallback log = nullptr );

   // Apply flat calibration to a debayered frame (3-channel, row-major).
   // Divides each pixel by the corresponding master flat pixel.
   // Frame dimensions must match the loaded flat.
   void calibrate( float* frameR, float* frameG, float* frameB,
                   int width, int height ) const;

   bool isLoaded() const { return !m_masterR.empty(); }
   int width() const { return m_width; }
   int height() const { return m_height; }

private:
   std::vector<float> m_masterR, m_masterG, m_masterB;
   int m_width = 0, m_height = 0;

   // Minimum flat value to avoid division by near-zero
   static constexpr float MIN_FLAT_VALUE = 0.01f;
};

} // namespace nukex
```

### Implementation

```cpp
// src/engine/FlatCalibrator.cpp

bool FlatCalibrator::loadFlats( const std::vector<std::string>& flatPaths,
                                 int expectedWidth, int expectedHeight,
                                 LogCallback log )
{
   // For each flat file:
   //   1. Load raw FITS (16-bit integers, single channel CFA)
   //   2. Debayer using bilinear interpolation (same as FrameLoader)
   //   3. Store debayered R/G/B channels
   //
   // After all flats loaded:
   //   4. Median-stack each channel independently
   //   5. Normalize: divide by the median pixel value per channel
   //   6. Clamp minimum to MIN_FLAT_VALUE to prevent division-by-zero
   //
   // NOTE: Since FlatCalibrator is engine code, it cannot use PCL's
   // FileFormat API directly. Instead, the caller (NukeXStackInstance)
   // loads the flat frames using FrameLoader and passes the debayered
   // pixel data. The FlatCalibrator stacks and normalizes.
}

void FlatCalibrator::calibrate( float* frameR, float* frameG, float* frameB,
                                 int width, int height ) const
{
   // For each pixel: frame[i] /= masterFlat[i]
   // The master flat is normalized so values ≈ 1.0 for clean pixels.
   // Dust motes have values < 1.0, so division brightens the affected pixels.
}
```

**IMPORTANT**: The engine class cannot use PCL APIs (FileFormat). Two options:
- **A**: FlatCalibrator takes pre-loaded debayered pixel arrays (caller loads via PCL)
- **B**: FlatCalibrator uses raw FITS I/O (cfitsio or manual parsing)

**Going with A** — keep the engine PCL-free. NukeXStackInstance uses FrameLoader to load+debayer the flats, then passes the pixel arrays to FlatCalibrator for stacking+normalization.

### Revised API

```cpp
class FlatCalibrator
{
public:
   // Add a debayered flat frame (3 channels, W*H floats each).
   // Call once per flat file. Stores all frames for median stacking.
   void addFrame( const float* r, const float* g, const float* b,
                  int width, int height );

   // Median-stack all added frames and normalize.
   // Call after all frames are added.
   void buildMasterFlat( LogCallback log = nullptr );

   // Apply calibration: divide each pixel by master flat.
   void calibrate( float* r, float* g, float* b,
                   int width, int height ) const;

   bool isReady() const { return m_ready; }
   int frameCount() const { return m_frameCount; }

private:
   std::vector<std::vector<float>> m_framesR, m_framesG, m_framesB;
   std::vector<float> m_masterR, m_masterG, m_masterB;
   int m_width = 0, m_height = 0;
   int m_frameCount = 0;
   bool m_ready = false;

   static constexpr float MIN_FLAT_VALUE = 0.01f;
};
```

- [ ] **Step 1: Write failing test — median stack 3 synthetic flats with a dust mote**

Test creates 3 flat frames with a known circular deficit, adds them to FlatCalibrator, calls buildMasterFlat(), verifies the master has the deficit. Then creates a "light" with the same deficit, calls calibrate(), verifies the deficit is removed.

- [ ] **Step 2: Implement FlatCalibrator**

Key implementation details:
- `addFrame()`: copy R/G/B into `m_frames*` vectors. Validate dimensions match.
- `buildMasterFlat()`: for each pixel position, take median across all frames. Then normalize each channel: divide by channel median. Clamp to MIN_FLAT_VALUE.
- `calibrate()`: for each pixel, `frame[i] /= master[i]`

- [ ] **Step 3: Add to CMakeLists.txt, build, test**
- [ ] **Step 4: Commit**

---

## Task 2: Wire FlatCalibrator into NukeXStack Phase 1

**Files:**
- Modify: `src/NukeXStackParameters.h` — add flat file list parameter class
- Modify: `src/NukeXStackParameters.cpp` — register parameter
- Modify: `src/NukeXStackInstance.h` — add flat file list + FlatCalibrator members
- Modify: `src/NukeXStackInstance.cpp` — Phase 1: load flats, calibrate lights
- Modify: `src/NukeXStackProcess.cpp` — register parameter

### Phase 1 integration

In `ExecuteGlobal()`, AFTER loading all light frames (Phase 1) but BEFORE alignment (Phase 1b):

```
Phase 1a: Flat calibration (new)
  1. If flat file list is non-empty:
     a. Load each flat file using FrameLoader::LoadSingleFrame()
     b. Debayer each flat (same as lights)
     c. Add debayered channels to FlatCalibrator
     d. Call buildMasterFlat()
     e. For each loaded light frame, call calibrate() on its R/G/B channels
  2. Log: "Flat calibration: N flats stacked, applied to 30 frames"
```

### Parameter

The flat file list follows the same PCL pattern as the light file list. It's a `MetaTable` with a `MetaString` column for file paths. The interface adds a TreeBox + Add/Remove buttons (same as the light frame list).

- [ ] **Step 1: Add NXSFlatFramePath parameter** (follow NXSFramePath pattern exactly)
- [ ] **Step 2: Add p_flatFrames to NukeXStackInstance** (StringList)
- [ ] **Step 3: Register in NukeXStackProcess**
- [ ] **Step 4: Add Phase 1a flat loading/calibration to ExecuteGlobal**
- [ ] **Step 5: Build and test manually** — run on M63 data with Lqef flats
- [ ] **Step 6: Commit**

---

## Task 3: Add flat file picker to UI

**Files:**
- Modify: `src/NukeXStackInterface.h` — add flat TreeBox + buttons
- Modify: `src/NukeXStackInterface.cpp` — flat file picker UI + event handlers

This follows the same pattern as the existing light frame file list TreeBox. Add/Remove/Clear buttons, drag-and-drop support.

- [ ] **Step 1: Add flat UI controls** (TreeBox, Add/Remove buttons, GroupBox)
- [ ] **Step 2: Wire event handlers** (file dialog, add to list, remove from list)
- [ ] **Step 3: Sync UI ↔ instance** (UpdateControls reads p_flatFrames, apply writes back)
- [ ] **Step 4: Build and test UI in PI**
- [ ] **Step 5: Commit**

---

## Task 4: Version bump, build, package, push

- [ ] **Step 1: Bump version**
- [ ] **Step 2: Full build + test**
- [ ] **Step 3: Test on real M63 data with Lqef flats**
- [ ] **Step 4: Package and push**

---

## Test Data

Flats are at: `/home/scarter4work/projects/nukex3/test_data/flats/`
- `Lqef/` — 10 frames, 33.3ms, matches M63 lights
- `Lpro/` — 10 frames, 33.3ms
- `HaO3/` — 10 frames, 508.3ms
- `S2O3/` — 10 frames, 591.7ms

M63 lights are at: `/mnt/qnap/astro_data/2_28_2026/M 63/`

## What This Replaces

With flat calibration in Phase 1, the algorithmic dust detection (sensor-space self-flat) and DustCorrector become unnecessary for users who provide flats. The detection code stays as a fallback for users without flats, but the Phase 7c DustCorrector will only fire if no flat was applied AND dust blobs are detected.
