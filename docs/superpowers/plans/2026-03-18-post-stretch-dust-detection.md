# Post-Stretch Dust Detection with Subcube Verification

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move dust spatial detection from Phase 3b (linear domain, where per-pixel SNR is too low) to Phase 7a (post-stretch, where the nonlinear MTF stretch amplifies faint deficits and the dust mote was proven detectable at 8.6σ), while keeping subcube verification using the original per-pixel per-frame consistency check from the spec.

**Architecture:** Phase 3b is removed. Phase 7a runs `detectDustSubcube()` on the stretched luminance image, passing the still-in-memory subcube for per-pixel verification. The subcube data is linear, but the spec's verification criterion (`madDeficit < medianDeficit * 0.5`) should work because the dust deficit is a fixed optical attenuation (~9%) that appears at the same magnitude in every frame — the stretched image just gives better spatial candidate detection. Subcubes are already kept in memory through Phase 7 for trail remediation. No memory lifetime changes needed.

**Tech Stack:** C++17, PCL, xtensor (SubCube), Catch2 v3

**Key insight:** The v3.1.0.16 aggregate verification is reverted. The original spec's per-pixel verification is restored — the problem was never the verification algorithm, it was running spatial detection on linear data where the dust deficit is indistinguishable from noise. On stretched data, the DoS deficit map will show the mote clearly (8.6σ proven), producing candidates at the right location, and the per-pixel subcube check will then confirm them because the subcube deficit is a real ~9% optical effect in every frame.

---

### Task 1: Revert v3.1.0.16 aggregate verification, restore spec per-pixel logic

**Files:**
- Modify: `src/engine/ArtifactDetector.cpp` (the `detectDustSubcube` method, ~lines 797-900)

The v3.1.0.16 change replaced the spec's per-pixel per-frame verification with an aggregate approach. Revert to the spec's algorithm: sample up to 20 pixels, check each pixel's per-frame deficit individually, blob passes if >50% of pixels pass. Keep the `LogCallback` diagnostics from v3.1.0.15 (those are valuable).

- [ ] **Step 1: Restore per-pixel verification in detectDustSubcube**

Replace the aggregate block (starting at "Sample up to 40 pixels") with the original per-pixel logic from the spec:

```cpp
// Sample up to 20 pixels from the blob
const int maxSamples = 20;
std::vector<int> samplePixels;
if ( c.area <= maxSamples )
{
   samplePixels = c.memberPixels;
}
else
{
   int step = c.area / maxSamples;
   for ( int s = 0; s < maxSamples; ++s )
      samplePixels.push_back( c.memberPixels[s * step] );
}

// Use first channel cube for verification
const SubCube* cube = channelCubes[0];
size_t nSubs = cube->numSubs();
int neighborRadius = std::max( 5, static_cast<int>( blob.radius / 2 ) );

int passCount = 0;
for ( int pixIdx : samplePixels )
{
   int px = pixIdx % width;
   int py = pixIdx / width;

   int nx0 = std::max( 0, px - neighborRadius );
   int nx1 = std::min( width - 1, px + neighborRadius );
   int ny0 = std::max( 0, py - neighborRadius );
   int ny1 = std::min( height - 1, py + neighborRadius );

   std::vector<double> frameDeficits( nSubs );
   for ( size_t z = 0; z < nSubs; ++z )
   {
      double neighborMean = ( cube->pixel( z, ny0, nx0 )
                            + cube->pixel( z, ny0, nx1 )
                            + cube->pixel( z, ny1, nx0 )
                            + cube->pixel( z, ny1, nx1 ) ) / 4.0;
      double pixelVal = cube->pixel( z, py, px );
      frameDeficits[z] = neighborMean - pixelVal;
   }

   std::vector<double> sortedDeficits = frameDeficits;
   size_t defMid = sortedDeficits.size() / 2;
   std::nth_element( sortedDeficits.begin(), sortedDeficits.begin() + defMid, sortedDeficits.end() );
   double medianDeficit = sortedDeficits[defMid];

   std::vector<double> defAbsDevs( nSubs );
   for ( size_t z = 0; z < nSubs; ++z )
      defAbsDevs[z] = std::abs( frameDeficits[z] - medianDeficit );
   size_t defMadMid = defAbsDevs.size() / 2;
   std::nth_element( defAbsDevs.begin(), defAbsDevs.begin() + defMadMid, defAbsDevs.end() );
   double madDeficit = 1.4826 * defAbsDevs[defMadMid];

   bool pixelPassed = ( medianDeficit > 0 && madDeficit < medianDeficit * 0.5 );
   if ( pixelPassed )
      ++passCount;

   if ( samplePixels.size() <= 20 || pixIdx == samplePixels[0] )
   {
      std::ostringstream oss;
      oss << "[Phase3b]   Sample (" << px << "," << py << "): medDeficit="
          << medianDeficit << ", madDeficit=" << madDeficit
          << ", ratio=" << (medianDeficit > 0 ? madDeficit/medianDeficit : 999.0)
          << (pixelPassed ? " PASS" : " FAIL");
      emit( oss.str() );
   }
}

{
   std::ostringstream oss;
   oss << "[Phase3b] Blob verification: " << passCount << "/" << samplePixels.size()
       << " passed (" << (100.0*passCount/samplePixels.size()) << "%)";
   emit( oss.str() );
}
if ( passCount > static_cast<int>( samplePixels.size() ) / 2 )
```

- [ ] **Step 2: Build and run tests**

Run: `cd build && cmake .. -DPCLDIR=$HOME/PCL && make -j$(nproc) && ctest --output-on-failure`
Expected: 17/17 PASS (existing subcube tests use synthetic data with strong signal — they should still pass with per-pixel logic)

- [ ] **Step 3: Commit**

```bash
git add src/engine/ArtifactDetector.cpp
git commit -m "revert: restore spec per-pixel subcube verification (undo v3.1.0.16 aggregate)"
```

---

### Task 2: Move dust detection from Phase 3b to Phase 7a

**Files:**
- Modify: `src/NukeXStackInstance.cpp` (~lines 410-456 and ~lines 833-875)

Remove the Phase 3b block entirely. In Phase 7a, after computing the stretched luminance (already done for trail detection), call `detectDustSubcube()` on the stretched luminance instead of `detectDust()` via `detectAll()`.

The subcubes (`channelCubes`) are already in memory at Phase 7 — they're used by Phase 7b trail remediation and freed at line 1181.

- [ ] **Step 1: Remove Phase 3b block**

Delete the entire Phase 3b section (lines 410-456 in NukeXStackInstance.cpp):
```cpp
// Phase 3b: Subcube-based dust detection (linear domain)
// ... everything through the closing brace and Module->ProcessEvents()
```

Keep the `dustDetection` variable declaration but move it to the class/function scope where Phase 7 can see it (it's already declared before Phase 3b, at line 411, so just leave it there with its default-constructed empty state).

- [ ] **Step 2: In Phase 7a, run detectDustSubcube on stretched luminance**

After the existing trail detection in Phase 7a (line ~867: `auto detection = detector.detectAll(...)`), add dust detection using the stretched luminance and subcube:

```cpp
// Dust detection on stretched image, verified against subcube
if ( p_enableDustRemediation )
{
   std::vector<nukex::SubCube*> cubePtrs;
   for ( int ch = 0; ch < numChannels; ++ch )
      cubePtrs.push_back( &channelCubes[ch] );

   dustDetection = detector.detectDustSubcube(
      luminance.data(), cubePtrs, cropW, cropH,
      [&console]( const std::string& msg ) {
         console.WriteLn( String( msg.c_str() ) );
      } );
}
```

Update the dust console log to say "from Phase 7a" instead of "from Phase 3b":
```cpp
console.WriteLn( String().Format( "    Dust: %d pixels (%d verified blobs)",
   dustDetection.dustPixelCount, int( dustDetection.blobs.size() ) ) );
```

- [ ] **Step 3: Build and run tests**

Run: `cd build && cmake .. -DPCLDIR=$HOME/PCL && make -j$(nproc) && ctest --output-on-failure`
Expected: 17/17 PASS

- [ ] **Step 4: Commit**

```bash
git add src/NukeXStackInstance.cpp
git commit -m "feat: move dust detection from Phase 3b to Phase 7a (post-stretch)"
```

---

### Task 3: Update diagnostic log prefix

**Files:**
- Modify: `src/engine/ArtifactDetector.cpp`

The `[Phase3b]` prefix in diagnostic messages is now misleading since detection runs in Phase 7a. Update all `[Phase3b]` strings to `[DustDetect]` — this is accurate regardless of where in the pipeline it runs.

- [ ] **Step 1: Replace all [Phase3b] with [DustDetect]**

Global find-replace in `ArtifactDetector.cpp`: `[Phase3b]` → `[DustDetect]`

- [ ] **Step 2: Build and run tests**

Run: `cd build && make -j$(nproc) && ctest --output-on-failure`
Expected: 17/17 PASS

- [ ] **Step 3: Commit**

```bash
git add src/engine/ArtifactDetector.cpp
git commit -m "refactor: rename diagnostic prefix [Phase3b] to [DustDetect]"
```

---

### Task 4: Update tests for stretched-domain detection

**Files:**
- Modify: `tests/unit/test_artifact_detector.cpp`

The existing subcube tests use synthetic data at 0.5 brightness with 10% depression (0.45). This simulates a stretched image more than a linear one (linear background is ~0.07). The tests should still pass as-is, but add a comment clarifying this represents stretched-domain data. No behavioral changes needed — the `detectDustSubcube` API is unchanged.

- [ ] **Step 1: Add clarifying comments to existing tests**

Update the test descriptions and comments to note these operate on stretched-domain-scale data:

```cpp
TEST_CASE( "detectDustSubcube verifies against subcube consistency", "[artifact][dust][subcube]" )
{
   // Synthetic stretched-domain image (background 0.5) with dust mote at (32, 32)
   // Subcube frames contain the same depression plus small per-frame noise
   // Verification should pass: consistent deficit across all frames
```

```cpp
TEST_CASE( "detectDustSubcube rejects inconsistent blobs", "[artifact][dust][subcube]" )
{
   // Synthetic image with dark blob, but subcube shows it only in half the frames
   // Verification should reject: high inter-frame variance
```

- [ ] **Step 2: Build and run tests**

Run: `cd build && make -j$(nproc) && ctest --output-on-failure`
Expected: 17/17 PASS

- [ ] **Step 3: Commit**

```bash
git add tests/unit/test_artifact_detector.cpp
git commit -m "test: clarify subcube dust tests operate on stretched-domain data"
```

---

### Task 5: Version bump, package, and deploy

**Files:**
- Modify: `src/NukeXModule.cpp` (version bump to 17)
- Modify: `repository/updates.xri` (title + description)
- Create: `repository/20260318-linux-x64-NukeX.tar.gz` (via `make package`)

Follow the release workflow from CLAUDE.md exactly.

- [ ] **Step 1: Bump MODULE_VERSION_BUILD to 17**

In `src/NukeXModule.cpp`:
```cpp
#define MODULE_VERSION_BUILD     17
```

- [ ] **Step 2: Update updates.xri title and description**

Title: `NukeX 3.1.0.17`
Description version line: `Version 3.1.0.17 - Post-stretch dust detection`
Description body: Dust spatial detection moved from Phase 3b (linear domain) to Phase 7a (post-stretch) where nonlinear MTF stretch amplifies the dust deficit signal. Subcube per-pixel per-frame verification retained per original spec. The 8.6σ SNR proven on stretched data should now produce verified detections.

- [ ] **Step 3: Clean rebuild**

Run: `make clean && make release`
Expected: `Build complete: NukeX-pxm.so`

- [ ] **Step 4: Run tests**

Run: `cd build && cmake .. -DPCLDIR=$HOME/PCL && make -j$(nproc) && ctest --output-on-failure`
Expected: 17/17 PASS

- [ ] **Step 5: Package**

Run: `make package`
Expected: Module signed, tarball created, SHA1 updated, XRI signed

- [ ] **Step 6: Commit all together**

```bash
git add src/NukeXModule.cpp repository/updates.xri repository/20260318-linux-x64-NukeX.tar.gz
git commit -m "build: bump to v3.1.0.17 — post-stretch dust detection"
```

- [ ] **Step 7: Push**

```bash
git push
```

---

## Notes

- The `detectDustSubcube` function signature is unchanged — it takes a `const float* stackedImage` which now receives stretched luminance instead of linear luminance. The subcube data remains linear. This is correct because Step 1 (spatial detection) operates on whatever image is passed, while Step 2 (verification) operates on the subcube directly.
- The stretched luminance is already computed in Phase 7a for trail detection. No duplicate computation.
- Memory: subcubes are freed at line 1181, after Phase 7. No lifetime changes needed.
- If verification still fails (unlikely given 8.6σ spatial signal), the next step from the memory notes is two-pass adaptive detection.
