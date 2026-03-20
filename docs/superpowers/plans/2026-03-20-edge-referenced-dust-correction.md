# Edge-Referenced Dust Mote Correction — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the self-flat correction map with edge-referenced correction that measures the deficit from the same image being corrected, eliminating coordinate-space mismatches and visible mask boundaries.

**Architecture:** Keep sensor-space detection (Phase 7a) to find blob centers and radii. Replace the correction map + mask approach with a new `remediateDustBlob()` function that, for each blob: samples an annular ring of edge pixels just outside the mote boundary, builds a per-angle edge brightness profile, computes a per-radius correction factor by comparing interior radial averages to the edge profile, and applies the correction per-pixel. The correction naturally tapers to 1.0 at the boundary because the deficit itself tapers to zero there.

**Tech Stack:** C++17, xtensor (subcube), PCL Image API (stretched image pixels), Catch2 v3 (tests)

---

## Principles

1. **Measure from the image you're correcting.** The deficit in the stretched image IS the ground truth. No need to compute it from linear sensor-space data and hope it matches.
2. **Edge pixels define the reference.** Pixels just outside the mote are the best estimate of what the background looks like at the mote boundary. Per-angle sampling handles background gradients (galaxy, vignetting).
3. **Radial correction preserves structure.** The correction factor at each radius is derived from the azimuthal average, so individual pixel noise/stars are preserved — only the smooth radial deficit is removed.
4. **No mask boundary.** Correction at the edge is near 1.0 because the mote deficit is near zero there. No step, no taper needed.

---

## File Structure

| File | Action | Responsibility |
|------|--------|----------------|
| `src/engine/ArtifactDetector.h` | Modify | Remove `correctionMap` from `DustDetectionResult`. Keep `mask` and `blobs`. |
| `src/engine/ArtifactDetector.cpp` | Modify | Remove correction map construction, cosine taper, extent tracing. Detection returns blobs with center + radius only. |
| `src/engine/DustCorrector.h` | Create | New class `DustCorrector` — takes blobs + image → corrects in-place. |
| `src/engine/DustCorrector.cpp` | Create | Edge-referenced correction implementation with per-angle radial profiles. |
| `src/NukeXStackInstance.cpp` | Modify | Phase 7c calls `DustCorrector` on the stretched image instead of applying correction map. |
| `src/engine/cuda/CudaRemediation.h` | Modify | Remove or deprecate `remediateDustGPU` (no longer called from Phase 7c). |
| `tests/unit/test_dust_corrector.cpp` | Create | Tests for the new corrector: uniform bg, gradient bg, off-center gradient, multiple motes. |
| `tests/unit/test_cuda_remediation.cpp` | Modify | Remove `correctionMap` references from dust correction tests. |
| `tests/unit/test_artifact_detector.cpp` | Modify | Remove `correctionMap` assertions from subcube dust tests. |
| `tests/CMakeLists.txt` | Modify | Add `test_dust_corrector` target. |

---

## Task 1: Create DustCorrector with edge-brightness sampling

**Files:**
- Create: `src/engine/DustCorrector.h`
- Create: `src/engine/DustCorrector.cpp`
- Test: `tests/unit/test_dust_corrector.cpp`
- Modify: `tests/CMakeLists.txt`

### Algorithm (core of the plan)

For each blob with center `(cx, cy)` and radius `R`:

```
Step A — Sample edge ring brightness per angular bin:
  - Divide 360° into 72 bins (5° each)
  - For each bin, average the brightness of pixels in annulus [R+2, R+5]
  - Result: edgeBrightness[72] — background brightness just outside the mote

Step B — Build per-angle radial correction profile:
  - For each radius r = 0, 1, ..., R:
    - For each angular bin, average brightness of pixels at distance [r-1, r+1] in that bin
    - Correction at (r, bin) = edgeBrightness[bin] / brightness(r, bin)
  - Result: correctionProfile[R+1][72] — one correction factor per radius per angle
  - Collapse to radial-only: correctionProfile[r] = azimuthal mean of correctionProfile[r][*]
  - Smooth the profile (3-point running average) to remove noise
  - Note: per-angle edge brightness handles background gradients (galaxy, vignetting)
    because the correction at each angle is relative to the LOCAL edge brightness.

Step C — Apply per-pixel correction:
  - For each pixel (x,y) inside the circle (distance d < R from center):
    - Interpolate correction factor from correctionProfile at distance d
    - corrected = pixel × correction(d)
  - Pixels at d ≈ R get correction ≈ 1.0 (deficit → 0 at boundary)
  - Pixels at d ≈ 0 get maximum correction (~1.05 for 5% deficit)
```

- [ ] **Step 1: Write the failing test for a uniform-background mote**

```cpp
// tests/unit/test_dust_corrector.cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "engine/DustCorrector.h"
#include "engine/ArtifactDetector.h"   // for DustBlobInfo
#include <vector>
#include <cmath>

TEST_CASE( "DustCorrector removes circular deficit on uniform background", "[dust][corrector]" )
{
   const int W = 300, H = 300;
   const float bg = 0.3f;        // stretched background level
   const int cx = 150, cy = 150;
   const int R = 20;
   const float peakAtten = 0.92f; // 8% deficit at center

   // Build image with Gaussian-profile mote
   std::vector<float> image( W * H );
   for ( int y = 0; y < H; ++y )
      for ( int x = 0; x < W; ++x )
      {
         double d = std::sqrt( double((x-cx)*(x-cx) + (y-cy)*(y-cy)) );
         float atten = 1.0f;
         if ( d < R )
            atten = 1.0f - (1.0f - peakAtten) * float( std::exp( -0.5 * (d*d) / (R*R/4.0) ) );
         image[y * W + x] = bg * atten;
      }

   nukex::DustBlobInfo blob;
   blob.centerX = cx;
   blob.centerY = cy;
   blob.radius = R;

   nukex::DustCorrector corrector;
   corrector.correct( image.data(), W, H, { blob } );

   // After correction, the center pixel should be close to bg
   float centerVal = image[cy * W + cx];
   REQUIRE( centerVal == Catch::Approx( bg ).margin( bg * 0.02f ) );

   // Edge pixel (just inside R) should be barely changed
   float edgeVal = image[cy * W + (cx + R - 2)];
   REQUIRE( edgeVal == Catch::Approx( bg ).margin( bg * 0.02f ) );
}
```

- [ ] **Step 2: Run test to verify it fails** (`function not defined`)

Run: `cd build && make -j$(nproc) && ctest -R test_dust_corrector --output-on-failure`

- [ ] **Step 3: Write DustCorrector header**

```cpp
// src/engine/DustCorrector.h
#pragma once
#include "engine/ArtifactDetector.h"  // for DustBlobInfo
#include <vector>
#include <functional>
#include <string>

namespace nukex {

using LogCallback = std::function<void( const std::string& )>;

class DustCorrector
{
public:
   // Correct dust motes in-place on a single-channel image.
   // For each blob, samples edge ring, builds radial correction profile,
   // and applies per-pixel correction.
   void correct( float* image, int width, int height,
                 const std::vector<DustBlobInfo>& blobs,
                 LogCallback log = nullptr ) const;

private:
   static constexpr int ANGULAR_BINS = 72;       // 5° per bin
   static constexpr int EDGE_INNER   = 2;        // edge ring starts R+2
   static constexpr int EDGE_OUTER   = 5;        // edge ring ends R+5
   static constexpr float MAX_CORRECTION = 1.5f; // safety clamp
};

} // namespace nukex
```

- [ ] **Step 4: Write DustCorrector implementation**

```cpp
// src/engine/DustCorrector.cpp
#include "engine/DustCorrector.h"
#include <cmath>
#include <algorithm>
#include <numeric>
#include <sstream>

namespace nukex {

void DustCorrector::correct( float* image, int width, int height,
                              const std::vector<DustBlobInfo>& blobs,
                              LogCallback log ) const
{
   auto emit = [&log]( const std::string& msg ) { if (log) log(msg); };

   for ( const auto& blob : blobs )
   {
      int cx = static_cast<int>( blob.centerX );
      int cy = static_cast<int>( blob.centerY );
      int R  = static_cast<int>( blob.radius );
      if ( R < 3 ) continue;

      // --- Step A: Sample edge ring brightness per angular bin ---
      std::vector<double> edgeSum( ANGULAR_BINS, 0 );
      std::vector<int>    edgeCount( ANGULAR_BINS, 0 );

      for ( int dy = -(R + EDGE_OUTER); dy <= R + EDGE_OUTER; ++dy )
         for ( int dx = -(R + EDGE_OUTER); dx <= R + EDGE_OUTER; ++dx )
         {
            int distSq = dx * dx + dy * dy;
            int rInner = R + EDGE_INNER;
            int rOuter = R + EDGE_OUTER;
            if ( distSq < rInner * rInner || distSq > rOuter * rOuter )
               continue;
            int px = cx + dx, py = cy + dy;
            if ( px < 0 || px >= width || py < 0 || py >= height )
               continue;

            double angle = std::atan2( dy, dx ) + M_PI;  // [0, 2π]
            int bin = static_cast<int>( angle / (2.0 * M_PI) * ANGULAR_BINS ) % ANGULAR_BINS;
            edgeSum[bin] += image[py * width + px];
            ++edgeCount[bin];
         }

      // Fill empty bins from neighbors
      for ( int i = 0; i < ANGULAR_BINS; ++i )
         if ( edgeCount[i] == 0 )
         {
            int prev = (i - 1 + ANGULAR_BINS) % ANGULAR_BINS;
            int next = (i + 1) % ANGULAR_BINS;
            if ( edgeCount[prev] > 0 && edgeCount[next] > 0 )
            {
               edgeSum[i] = (edgeSum[prev] / edgeCount[prev] + edgeSum[next] / edgeCount[next]) * 0.5;
               edgeCount[i] = 1;
            }
         }

      std::vector<double> edgeBrightness( ANGULAR_BINS );
      for ( int i = 0; i < ANGULAR_BINS; ++i )
         edgeBrightness[i] = ( edgeCount[i] > 0 ) ? edgeSum[i] / edgeCount[i] : 0;

      double edgeMean = 0;
      int edgeValidBins = 0;
      for ( int i = 0; i < ANGULAR_BINS; ++i )
         if ( edgeCount[i] > 0 ) { edgeMean += edgeBrightness[i]; ++edgeValidBins; }
      if ( edgeValidBins == 0 ) continue;
      edgeMean /= edgeValidBins;

      // --- Step B: Build per-angle radial correction profile ---
      // For each (radius, angular bin), compute the ratio edge/interior.
      // This handles background gradients because each angle uses its own
      // edge reference brightness.
      // correctionByAngle[r][bin] = edgeBrightness[bin] / avg_brightness(r, bin)
      std::vector<std::vector<double>> radialSum( R + 1, std::vector<double>( ANGULAR_BINS, 0 ) );
      std::vector<std::vector<int>>    radialCount( R + 1, std::vector<int>( ANGULAR_BINS, 0 ) );

      for ( int dy = -R; dy <= R; ++dy )
         for ( int dx = -R; dx <= R; ++dx )
         {
            int distSq = dx * dx + dy * dy;
            if ( distSq > R * R ) continue;
            int px = cx + dx, py = cy + dy;
            if ( px < 0 || px >= width || py < 0 || py >= height ) continue;

            int r = static_cast<int>( std::sqrt( static_cast<double>( distSq ) ) );
            if ( r > R ) r = R;
            double angle = std::atan2( dy, dx ) + M_PI;
            int bin = static_cast<int>( angle / (2.0 * M_PI) * ANGULAR_BINS ) % ANGULAR_BINS;
            radialSum[r][bin] += image[py * width + px];
            ++radialCount[r][bin];
         }

      // Collapse per-angle corrections to radial profile (azimuthal mean).
      // Each angle contributes edge[bin]/interior[bin] — gradient-aware.
      std::vector<float> correctionProfile( R + 1, 1.0f );
      for ( int r = 0; r <= R; ++r )
      {
         double corrSum = 0;
         int corrN = 0;
         for ( int bin = 0; bin < ANGULAR_BINS; ++bin )
         {
            if ( radialCount[r][bin] > 0 && edgeCount[bin] > 0 )
            {
               double interior = radialSum[r][bin] / radialCount[r][bin];
               if ( interior > 1e-10 )
               {
                  corrSum += edgeBrightness[bin] / interior;
                  ++corrN;
               }
            }
         }
         if ( corrN > 0 )
            correctionProfile[r] = std::min( static_cast<float>( corrSum / corrN ), MAX_CORRECTION );
      }

      // Smooth the profile (3-point running average) to remove noise
      std::vector<float> smoothed = correctionProfile;
      for ( int r = 1; r < R; ++r )
         smoothed[r] = ( correctionProfile[r-1] + correctionProfile[r] + correctionProfile[r+1] ) / 3.0f;
      // Force boundary to 1.0 — no correction at the edge
      smoothed[R] = 1.0f;
      correctionProfile = smoothed;

      // --- Step C: Apply per-pixel correction ---
      int correctedCount = 0;
      for ( int dy = -R; dy <= R; ++dy )
         for ( int dx = -R; dx <= R; ++dx )
         {
            int distSq = dx * dx + dy * dy;
            if ( distSq > R * R ) continue;
            int px = cx + dx, py = cy + dy;
            if ( px < 0 || px >= width || py < 0 || py >= height ) continue;

            double dist = std::sqrt( static_cast<double>( distSq ) );
            // Interpolate correction from profile
            int rLow = static_cast<int>( dist );
            int rHigh = std::min( rLow + 1, R );
            float frac = static_cast<float>( dist - rLow );
            float corr = correctionProfile[rLow] * (1.0f - frac) + correctionProfile[rHigh] * frac;

            image[py * width + px] *= corr;
            ++correctedCount;
         }

      {
         std::ostringstream oss;
         oss << "[DustCorrect] Blob (" << cx << "," << cy << "): R=" << R
             << ", edgeMean=" << edgeMean
             << ", centerCorr=" << correctionProfile[0]
             << ", corrected=" << correctedCount << "px";
         emit( oss.str() );
      }
   }
}

} // namespace nukex
```

- [ ] **Step 5: Add to CMakeLists.txt and run test**

Run: `cd build && cmake .. -DPCLDIR=$HOME/PCL && make -j$(nproc) && ctest -R test_dust_corrector --output-on-failure`
Expected: PASS — center pixel ≈ bg, edge pixel ≈ bg

- [ ] **Step 6: Commit**

```
git add src/engine/DustCorrector.{h,cpp} tests/unit/test_dust_corrector.cpp tests/CMakeLists.txt
git commit -m "feat: add DustCorrector with edge-referenced radial correction"
```

---

## Task 2: Test with background gradient (galaxy proximity)

**Files:**
- Test: `tests/unit/test_dust_corrector.cpp`

- [ ] **Step 1: Write test with linear gradient across the mote**

```cpp
TEST_CASE( "DustCorrector handles background gradient", "[dust][corrector]" )
{
   const int W = 300, H = 300;
   const int cx = 150, cy = 150, R = 20;
   const float peakAtten = 0.92f;

   // Background with linear gradient (simulating galaxy proximity)
   std::vector<float> image( W * H );
   for ( int y = 0; y < H; ++y )
      for ( int x = 0; x < W; ++x )
      {
         float bg = 0.2f + 0.002f * x;  // gradient: 0.2 at left, 0.8 at right
         double d = std::sqrt( double((x-cx)*(x-cx) + (y-cy)*(y-cy)) );
         float atten = 1.0f;
         if ( d < R )
            atten = 1.0f - (1.0f - peakAtten) * float( std::exp( -0.5 * (d*d) / (R*R/4.0) ) );
         image[y * W + x] = bg * atten;
      }

   // Save a copy of the uncorrected image for reference
   float bgAtCenter = 0.2f + 0.002f * cx;  // expected background at mote center

   nukex::DustBlobInfo blob;
   blob.centerX = cx;  blob.centerY = cy;  blob.radius = R;

   nukex::DustCorrector corrector;
   corrector.correct( image.data(), W, H, { blob } );

   // Center should be close to background value at that position
   float centerVal = image[cy * W + cx];
   REQUIRE( centerVal == Catch::Approx( bgAtCenter ).margin( bgAtCenter * 0.03f ) );
}
```

- [ ] **Step 2: Run test — should pass with existing implementation**

The per-angle edge sampling captures the gradient, so edgeMean varies with angle and the radial profile compensates. (Note: current implementation uses a single edgeMean for the radial profile. If this test fails, upgrade to per-angle radial correction in Task 3.)

- [ ] **Step 3: Commit**

---

## Task 3: Wire DustCorrector into Phase 7c

**Files:**
- Modify: `src/NukeXStackInstance.cpp` (Phase 7c, ~lines 947-970)
- Modify: `src/engine/ArtifactDetector.h` (remove `correctionMap` from `DustDetectionResult`)
- Modify: `src/engine/ArtifactDetector.cpp` (remove correction map construction + taper code)

- [ ] **Step 1: Remove `correctionMap` from `DustDetectionResult`**

In `ArtifactDetector.h`:
```cpp
struct DustDetectionResult
{
   std::vector<uint8_t> mask;    // 1 = dust pixel (kept for pixel counting only)
   std::vector<DustBlobInfo> blobs;
   int dustPixelCount = 0;
};
```

- [ ] **Step 2: Remove correction map construction from `detectDustSubcube()`**

In `ArtifactDetector.cpp`:
- Remove `result.correctionMap.assign(N, 1.0f)` at initialization
- Remove the entire cosine-taper correction map block in the mask-painting loop
- Simplify mask painting back to just setting `result.mask[idx] = 1`

- [ ] **Step 3: Replace Phase 7c in `NukeXStackInstance.cpp`**

Replace the `useSelfFlat` / neighbor-brightness dual path with:

```cpp
// 7c: Dust remediation (per channel) — edge-referenced correction
// Note: blobs are populated by detectDustSubcube(). The old detectDust() path
// does not populate blobs, so DustCorrector will not run for that path.
// This is intentional — the old path is deprecated.
if ( p_enableDustRemediation && !dustDetection.blobs.empty() )
{
   console.WriteLn( "  Phase 7c: Dust remediation (edge-referenced correction)..." );
   console.Flush();
   Module->ProcessEvents();

   nukex::DustCorrector corrector;
   for ( int ch = 0; ch < outChannels; ++ch )
   {
      corrector.correct(
         channelResults[ch].data(), cropW, cropH,
         dustDetection.blobs,
         [&console]( const std::string& msg ) {
            console.WriteLn( String( msg.c_str() ) );
         } );
   }

   int totalPixels = 0;
   for ( const auto& b : dustDetection.blobs )
      totalPixels += static_cast<int>( M_PI * b.radius * b.radius );
   console.WriteLn( String().Format( "    Corrected ~%d dust pixels per channel", totalPixels ) );
   Module->ProcessEvents();
}
```

- [ ] **Step 4: Build and run all tests**

Run: `cd build && cmake .. -DPCLDIR=$HOME/PCL && make -j$(nproc) && ctest --output-on-failure`
Expected: All tests pass. Some existing `test_artifact_detector` tests may need adjustment since `correctionMap` is removed.

- [ ] **Step 5: Update existing artifact detector tests**

Remove any assertions on `correctionMap` from `test_artifact_detector.cpp`. The dust blob detection tests should still verify center/radius/circularity — only the correction map checks change.

- [ ] **Step 6: Commit**

```
git add src/engine/ArtifactDetector.{h,cpp} src/NukeXStackInstance.cpp tests/unit/test_artifact_detector.cpp
git commit -m "refactor: replace self-flat correction with edge-referenced DustCorrector"
```

---

## Task 4: Version bump, build, package, push

**Files:**
- Modify: `src/NukeXModule.cpp` (bump build number)

- [ ] **Step 1: Bump version** (increment from current — currently v3.1.0.39, so next is .40. If intermediate commits were made in Tasks 1-3, use whatever the next number is.)
- [ ] **Step 2: Full build + test**

Run: `cd build && cmake .. -DPCLDIR=$HOME/PCL && make -j$(nproc) && ctest --output-on-failure`

- [ ] **Step 3: Package**

Run: `make package`

- [ ] **Step 4: Commit and push**

```
git add src/NukeXModule.cpp repository/
git commit -m "build: bump to v3.1.0.40 — edge-referenced dust correction"
git push
```

---

## Why This Works

| Previous approach (self-flat) | New approach (edge-referenced) |
|---|---|
| Measures deficit from sensor-space linear data | Measures deficit from the stretched image being corrected |
| Correction map in sensor coordinates | Correction applied directly to image pixels |
| Sharp mask boundary needs cosine taper | No boundary — correction naturally reaches 1.0 at edge |
| `1/normalized` approximation for nonlinear stretch | Exact ratio from actual pixel values |
| Correction factor pre-computed, applied later | Correction computed and applied in one pass |

## What We Keep

- Sensor-space self-flat **detection** (Phase 7a) — still the best way to find mote centers and radii
- The `mask` in `DustDetectionResult` — still used for pixel counting and diagnostics
- All the review fixes from v3.1.0.36 (MAD floor, NaN guard, GPU logging, etc.)

## What We Remove

- `correctionMap` field from `DustDetectionResult`
- Correction map construction in `detectDustSubcube()` (cosine taper, extent tracing for correction)
- The dual-path (self-flat / neighbor-brightness) in Phase 7c
- The `p_dustNeighborRadius` and `p_dustMaxCorrectionRatio` parameters become unused by the primary path (keep for backward compat)
