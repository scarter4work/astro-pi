# Remove GPU MAX_SUBS Limit — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove the hardcoded `MAX_SUBS = 64` limit in the CUDA pixel selection kernel so GPU stacking works with any number of subs (tested up to 750+), and surface GPU failures visibly in the PI console instead of silently falling back.

**Architecture:** Replace per-thread stack arrays (`double arr[MAX_SUBS]`) with a pre-allocated global memory workspace pool. Each thread gets a fixed workspace slot indexed by its position in a grid-stride loop. Add row-band chunking to `processImageGPU` so the cube data transfer fits in VRAM regardless of sub count. Move quality scores from `__constant__` memory (fixed 64-element array) to global memory with `__ldg()` read-only cache hints.

**Tech Stack:** CUDA 12.8, C++17, nvcc with `-arch=native` (sm_120 / RTX 5070 Ti), Catch2 v3 for tests, xtensor column-major SubCube layout.

---

## File Map

| Action | File | Responsibility |
|--------|------|---------------|
| Create | `src/engine/cuda/CudaWorkspace.h` | Workspace layout computation — host+device compatible struct |
| Modify | `src/engine/cuda/CudaPixelSelector.cu` | Major: workspace pointers, grid-stride loop, row-band chunking |
| Modify | `src/engine/cuda/CudaPixelSelector.h` | Update `GpuStackConfig`, `GpuStackResult`, function signature |
| Modify | `src/engine/PixelSelector.h` | Add `lastGpuFallback()` accessor |
| Modify | `src/engine/PixelSelector.cpp` | Track GPU fallback, pass error message back |
| Modify | `src/NukeXStackInstance.cpp` | Log GPU fallback/error to PI console |
| Modify | `tests/unit/test_cuda_equivalence.cpp` | Add high-sub-count GPU vs CPU equivalence tests |

**Not in scope (dead code):** `CudaRemediation.cu` also has `MAX_SUBS = 128` for the trail remediation kernel, but Phase 7 trail remediation is dead at runtime. Fix separately if trail detection is revived.

---

## Background: Memory Layout

The SubCube is `xtensor<float, 3>` shape `(nSubs, H, W)` in **column-major** order. Element `(z, y, x)` lives at:

```
cubeData[z + y * nSubs + x * nSubs * H]
```

A Z-column at pixel `(y, x)` is `nSubs` contiguous floats starting at `y * nSubs + x * nSubs * H`. For a fixed `x`, consecutive `y` rows are adjacent in memory (stride = `nSubs`).

**Row-band gather:** To process rows `[startY, startY + bandH)`, we extract a band-local cube of shape `(nSubs, bandH, W)` with the same column-major layout but `bandH` instead of `H`:

```cpp
for (size_t x = 0; x < W; ++x) {
    memcpy(bandBuf + x * nSubs * bandH,
           cubeData + startY * nSubs + x * nSubs * H,
           nSubs * bandH * sizeof(float));
}
```

If `bandH == H` (entire image fits), skip the gather and upload directly.

---

## Workspace Design

### Per-Thread Workspace Layout

Each thread gets a workspace slot of `bytesPerSlot` bytes. Arrays within a slot are packed contiguously (doubles first, then ints, then bools) with natural alignment.

**Persistent arrays** (live across multiple kernel phases):

| Array | Type | Phase range | Purpose |
|-------|------|------------|---------|
| `zValues` | `double[n]` | 1–6 | Promoted Z-column values |
| `preFiltered` | `double[n]` | 3–4 | MAD-clean subset for ESD |
| `cleanData` | `double[n]` | 6–10 | Outlier-free values for fitting |
| `sortedClean` | `double[n]` | 7–9 | Sorted clean for shortest-half + tiebreaker |
| `preFilteredIdx` | `int[n]` | 3–5 | Maps preFiltered → original index |
| `cleanIdx` | `int[n]` | 6–9 | Maps cleanData → original index |
| `sortedCleanIdx` | `int[n]` | 7–9 | Parallel sort index for sortedClean |
| `madOutlier` | `bool[n]` | 2–5 | MAD sigma-clip mask |
| `esdOutlier` | `bool[n]` | 4–5 | ESD outlier mask (preFiltered-relative) |
| `allOutlier` | `bool[n]` | 5–6 | Combined outlier mask |

**Scratch arrays** (reused across non-overlapping phases):

| Slot | Type | Phase 2 (sigma) | Phase 4 (ESD) | Phase 8 (shortest-half) | Phase 10 (bimodal) |
|------|------|-----------------|---------------|------------------------|-------------------|
| `scratch_d1` | `double[n]` | sorted | working | shValues | sorted |
| `scratch_d2` | `double[n]` | deviations | testStats | shDeviations | r1 |
| `scratch_d3` | `double[n]` | — | critVals | — | — |
| `scratch_i1` | `int[n]` | — | indexMap | — | — |
| `scratch_i2` | `int[n]` | — | removedOrigIdx | — | — |

**Total per slot:** `7 * 8n + 5 * 4n + 3 * 1n = 79n` bytes. For 750 subs: **~58 KB per slot.**

### Grid Sizing

Target: 10,240–20,480 concurrent threads (40–80 blocks × 256 threads). With 750 subs at 58 KB/slot:
- 10,240 slots × 58 KB = **580 MB** workspace
- Plus band cube (~1.2 GB for 100 rows × 4000 cols × 750 subs)
- Plus output/distTypes/quality scores (~small)
- **Total ~2 GB** — comfortably fits in 16 GB VRAM

The host function will query free VRAM and auto-size the grid and band height.

---

### Task 1: Fix Silent GPU Fallback (Visibility)

**Files:**
- Modify: `src/engine/PixelSelector.h:62-64`
- Modify: `src/engine/PixelSelector.cpp:402-452`
- Modify: `src/NukeXStackInstance.cpp:471-484`

- [ ] **Step 1: Add fallback tracking to PixelSelector**

In `src/engine/PixelSelector.h`, add a member and accessor after `m_lastErrorCount`:

```cpp
private:
    Config m_config;
    size_t m_lastErrorCount = 0;
    bool m_lastGpuFallback = false;
    std::string m_lastGpuError;

public:
    bool lastGpuFallback() const { return m_lastGpuFallback; }
    const std::string& lastGpuError() const { return m_lastGpuError; }
```

- [ ] **Step 2: Set fallback flag in processImageGPU**

In `src/engine/PixelSelector.cpp`, at the start of `processImageGPU`, clear the flag:

```cpp
std::vector<float> PixelSelector::processImageGPU(SubCube& cube, ...) {
    m_lastGpuFallback = false;
    m_lastGpuError.clear();
```

At the GPU-unavailable fallback (line ~408-411):

```cpp
    if (!cuda::isGpuAvailable()) {
        m_lastGpuFallback = true;
        m_lastGpuError = "No CUDA-capable GPU detected at runtime";
        return processImage(cube, qualityScores, progress);
    }
```

At the kernel-failure fallback (line ~434-438):

```cpp
    if (!result.success) {
        m_lastGpuFallback = true;
        m_lastGpuError = result.errorMessage;
        std::fprintf(stderr, "NukeX: GPU stacking failed: %s -- falling back to CPU\n",
            result.errorMessage.c_str());
        return processImage(cube, qualityScores, progress);
    }
```

- [ ] **Step 3: Log fallback in NukeXStackInstance**

In `src/NukeXStackInstance.cpp`, after the GPU call at line ~473:

```cpp
         if ( useGPU )
         {
            channelResults[ch] = selector.processImageGPU( channelCubes[ch], qualityScoresPtr, distTypeMaps[ch], progressCB );
            if ( selector.lastGpuFallback() )
            {
               if ( ch == 0 ) // Log once, not per-channel
                  console.WarningLn( String().Format(
                     "  GPU stacking failed: %s \xe2\x80\x94 fell back to CPU",
                     selector.lastGpuError().c_str() ) );
            }
         }
```

- [ ] **Step 4: Build and run existing tests**

```bash
cd /home/scarter4work/projects/nukex3/build && cmake .. && make -j$(nproc)
ctest --output-on-failure
```

Expected: All 19 existing test suites pass. No behavior change for sub counts ≤ 64.

- [ ] **Step 5: Commit**

```bash
git add src/engine/PixelSelector.h src/engine/PixelSelector.cpp src/NukeXStackInstance.cpp
git commit -m "fix: surface GPU stacking failures in PI console instead of silent fallback"
```

---

### Task 2: Create Workspace Infrastructure

**Files:**
- Create: `src/engine/cuda/CudaWorkspace.h`

- [ ] **Step 1: Write the workspace header**

Create `src/engine/cuda/CudaWorkspace.h`:

```cpp
// src/engine/cuda/CudaWorkspace.h
// Per-thread workspace layout for GPU pixel selection.
// Replaces fixed-size stack arrays (old MAX_SUBS=64) with dynamically-sized
// global memory workspaces, removing any limit on sub count.
#pragma once

#include <cstddef>
#include <cstdint>

namespace nukex {
namespace cuda {

// Workspace layout computed on the host, passed to the kernel.
// All offsets are byte offsets from the slot base pointer.
struct WorkspaceLayout {
    size_t bytesPerSlot;

    // Persistent double arrays
    size_t off_zValues;         // double[nSubs]
    size_t off_preFiltered;     // double[nSubs]
    size_t off_cleanData;       // double[nSubs]
    size_t off_sortedClean;     // double[nSubs]

    // Scratch double arrays (reused across non-concurrent phases)
    size_t off_scratch_d1;      // double[nSubs]
    size_t off_scratch_d2;      // double[nSubs]
    size_t off_scratch_d3;      // double[nSubs]

    // Persistent int arrays
    size_t off_preFilteredIdx;  // int[nSubs]
    size_t off_cleanIdx;        // int[nSubs]
    size_t off_sortedCleanIdx;  // int[nSubs]

    // Scratch int arrays
    size_t off_scratch_i1;      // int[nSubs]
    size_t off_scratch_i2;      // int[nSubs]

    // Bool arrays
    size_t off_madOutlier;      // bool[nSubs]
    size_t off_esdOutlier;      // bool[nSubs]
    size_t off_allOutlier;      // bool[nSubs]
};

// Compute workspace layout for a given sub count.
inline WorkspaceLayout computeWorkspaceLayout(int nSubs)
{
    WorkspaceLayout w{};
    size_t n = static_cast<size_t>(nSubs);
    size_t off = 0;

    // Double zone (7 arrays)
    w.off_zValues      = off; off += n * sizeof(double);
    w.off_preFiltered  = off; off += n * sizeof(double);
    w.off_cleanData    = off; off += n * sizeof(double);
    w.off_sortedClean  = off; off += n * sizeof(double);
    w.off_scratch_d1   = off; off += n * sizeof(double);
    w.off_scratch_d2   = off; off += n * sizeof(double);
    w.off_scratch_d3   = off; off += n * sizeof(double);

    // Int zone (5 arrays) — align to 4 bytes (already aligned after doubles)
    w.off_preFilteredIdx  = off; off += n * sizeof(int);
    w.off_cleanIdx        = off; off += n * sizeof(int);
    w.off_sortedCleanIdx  = off; off += n * sizeof(int);
    w.off_scratch_i1      = off; off += n * sizeof(int);
    w.off_scratch_i2      = off; off += n * sizeof(int);

    // Bool zone (3 arrays)
    w.off_madOutlier   = off; off += n * sizeof(bool);
    w.off_esdOutlier   = off; off += n * sizeof(bool);
    w.off_allOutlier   = off; off += n * sizeof(bool);

    // Align total to 8 bytes for clean slot boundaries
    w.bytesPerSlot = (off + 7) & ~size_t(7);
    return w;
}

// Device-side helper: get a typed pointer from workspace base + offset.
#ifdef __CUDACC__
template<typename T>
__device__ __forceinline__ T* wsPtr(char* base, size_t offset)
{
    return reinterpret_cast<T*>(base + offset);
}
#endif

} // namespace cuda
} // namespace nukex
```

- [ ] **Step 2: Verify header compiles**

```bash
cd /home/scarter4work/projects/nukex3/build && cmake .. && make -j$(nproc)
```

Expected: Clean build (header is not yet included anywhere — no behavior change).

- [ ] **Step 3: Commit**

```bash
git add src/engine/cuda/CudaWorkspace.h
git commit -m "feat: add CudaWorkspace.h — workspace layout for dynamic sub counts"
```

---

### Task 3: Write Failing Test for >64 Subs

**Files:**
- Modify: `tests/unit/test_cuda_equivalence.cpp`

- [ ] **Step 1: Add test case for 100 subs**

Append to `tests/unit/test_cuda_equivalence.cpp`. Also add `#include <random>` to the includes block at the top of the file if not already present.

```cpp
TEST_CASE("GPU stacking with 100 subs matches CPU", "[cuda][equivalence][high-subs]") {
#ifndef NUKEX_HAS_CUDA
    SKIP("CUDA support not compiled in");
#else
    if (!nukex::cuda::isGpuAvailable()) {
        SKIP("No CUDA-capable GPU available");
    }

    constexpr size_t nSubs = 100;
    constexpr size_t H = 4, W = 4;

    // Create subcube with reproducible synthetic data
    nukex::SubCube cube(nSubs, H, W);
    std::mt19937 rng(12345);
    std::normal_distribution<double> noise(500.0, 15.0);

    for (size_t z = 0; z < nSubs; ++z)
        for (size_t y = 0; y < H; ++y)
            for (size_t x = 0; x < W; ++x)
                cube.setPixel(z, y, x, static_cast<float>(noise(rng)));

    // Inject outlier in frame 50 at pixel (2,2)
    cube.setPixel(50, 2, 2, 9999.0f);

    // CPU reference
    nukex::PixelSelector::Config cfg;
    cfg.maxOutliers = 3;
    cfg.outlierAlpha = 0.05;
    cfg.adaptiveModels = false;
    nukex::PixelSelector cpuSel(cfg);
    auto cpuResult = cpuSel.processImage(cube, nullptr, nullptr);

    // GPU path
    std::vector<float> gpuOutput(H * W);
    std::vector<uint8_t> gpuDistTypes(H * W);

    nukex::cuda::GpuStackConfig gpuConfig;
    gpuConfig.maxOutliers = 3;
    gpuConfig.outlierAlpha = 0.05;
    gpuConfig.adaptiveModels = false;
    gpuConfig.nSubs = nSubs;
    gpuConfig.height = H;
    gpuConfig.width = W;

    auto result = nukex::cuda::processImageGPU(
        cube.cube().data(), gpuOutput.data(), gpuDistTypes.data(), gpuConfig);

    REQUIRE(result.success);

    for (size_t i = 0; i < cpuResult.size(); ++i) {
        REQUIRE(gpuOutput[i] == Catch::Approx(cpuResult[i]).margin(1e-3f));
    }
#endif
}

TEST_CASE("GPU stacking with 256 subs matches CPU", "[cuda][equivalence][high-subs]") {
#ifndef NUKEX_HAS_CUDA
    SKIP("CUDA support not compiled in");
#else
    if (!nukex::cuda::isGpuAvailable()) {
        SKIP("No CUDA-capable GPU available");
    }

    constexpr size_t nSubs = 256;
    constexpr size_t H = 4, W = 4;

    nukex::SubCube cube(nSubs, H, W);
    std::mt19937 rng(67890);
    std::normal_distribution<double> noise(300.0, 10.0);

    for (size_t z = 0; z < nSubs; ++z)
        for (size_t y = 0; y < H; ++y)
            for (size_t x = 0; x < W; ++x)
                cube.setPixel(z, y, x, static_cast<float>(noise(rng)));

    nukex::PixelSelector::Config cfg;
    cfg.maxOutliers = 5;
    cfg.outlierAlpha = 0.05;
    nukex::PixelSelector cpuSel(cfg);
    auto cpuResult = cpuSel.processImage(cube, nullptr, nullptr);

    std::vector<float> gpuOutput(H * W);
    std::vector<uint8_t> gpuDistTypes(H * W);

    nukex::cuda::GpuStackConfig gpuConfig;
    gpuConfig.maxOutliers = 5;
    gpuConfig.outlierAlpha = 0.05;
    gpuConfig.nSubs = nSubs;
    gpuConfig.height = H;
    gpuConfig.width = W;

    auto result = nukex::cuda::processImageGPU(
        cube.cube().data(), gpuOutput.data(), gpuDistTypes.data(), gpuConfig);

    REQUIRE(result.success);

    for (size_t i = 0; i < cpuResult.size(); ++i) {
        REQUIRE(gpuOutput[i] == Catch::Approx(cpuResult[i]).margin(1e-3f));
    }
#endif
}
```

- [ ] **Step 2: Build and verify tests fail**

```bash
cd /home/scarter4work/projects/nukex3/build && cmake .. && make -j$(nproc)
./test_cuda_equivalence "[high-subs]" -v
```

Expected: Both tests FAIL with `result.success == false` and error message `"nSubs exceeds MAX_SUBS (64)"`.

- [ ] **Step 3: Commit failing tests**

```bash
git add tests/unit/test_cuda_equivalence.cpp
git commit -m "test: add failing GPU tests for >64 subs (documents MAX_SUBS bug)"
```

---

### Tasks 4–6: CUDA Kernel Refactor (Atomic Commit)

> **Build note:** Tasks 4, 5, and 6 are a single atomic change — the sub-function signatures, kernel body, and host launch function must all be updated together for the build to succeed. Implement all three, then commit once.

#### Task 4: Refactor Device Sub-Functions to Accept Workspace Pointers

**Files:**
- Modify: `src/engine/cuda/CudaPixelSelector.cu:263-400` (sigmaClipMAD, detectOutliersESD)
- Modify: `src/engine/cuda/CudaPixelSelector.cu:678-784` (fitBimodalEM)

This task changes the three device functions that use `MAX_SUBS`-sized stack arrays to accept external workspace pointers instead.

- [ ] **Step 1: Add CudaWorkspace.h include**

At the top of `CudaPixelSelector.cu`, add after the existing includes:

```cpp
#include "cuda/CudaWorkspace.h"
```

- [ ] **Step 2: Remove MAX_SUBS constant and constant-memory quality scores**

Delete these lines (near top of file):

```cpp
static constexpr int MAX_SUBS = 64;

// Quality scores in constant memory — broadcast efficiently to all threads
__constant__ double d_qualityScores[64];
```

- [ ] **Step 3: Refactor sigmaClipMAD_device**

Change signature from:
```cpp
__device__ void sigmaClipMAD_device(
    const double* zValues, int nSubs, double kappa, bool* isOutlier)
```

To:
```cpp
__device__ void sigmaClipMAD_device(
    const double* zValues, int nSubs, double kappa, bool* isOutlier,
    double* sorted, double* deviations)
```

Remove the two local array declarations (`double sorted[MAX_SUBS]` and `double deviations[MAX_SUBS]`). The rest of the function body is unchanged — it already uses `sorted` and `deviations` by name.

- [ ] **Step 4: Refactor detectOutliersESD_device**

Change signature from:
```cpp
__device__ void detectOutliersESD_device(
    const double* data, int n, int maxOutliers, double alpha, bool* isOutlier)
```

To:
```cpp
__device__ void detectOutliersESD_device(
    const double* data, int n, int maxOutliers, double alpha, bool* isOutlier,
    double* working, int* indexMap, double* testStats, double* critVals, int* removedOrigIdx)
```

Remove the five local array declarations. The rest of the function body is unchanged.

- [ ] **Step 5: Refactor fitBimodalEM_device**

Change signature from:
```cpp
__device__ FitResultDevice fitBimodalEM_device(const double* data, int n)
```

To:
```cpp
__device__ FitResultDevice fitBimodalEM_device(const double* data, int n,
    double* sorted, double* r1)
```

Remove the two local array declarations (`double sorted[MAX_SUBS]` and `double r1[MAX_SUBS]`). The rest of the function body is unchanged.

Proceed directly to Task 5 (kernel body) — do NOT attempt to build or commit yet.

---

#### Task 5: Refactor Kernel to Use Workspace + Grid-Stride Loop

**Files:**
- Modify: `src/engine/cuda/CudaPixelSelector.cu:804-1040` (pixelSelectionKernel)

This is the core change. The kernel switches from stack arrays to workspace pointers and uses a grid-stride loop.

- [ ] **Step 1: Update kernel signature**

Replace the current signature:
```cpp
__global__ void pixelSelectionKernel(
    const float* __restrict__ cubeData,
    float* __restrict__ outputPixels,
    uint8_t* __restrict__ distTypes,
    uint32_t* __restrict__ provenanceOut,
    int nSubs, int height, int width,
    int maxOutliers, double outlierAlpha,
    bool adaptiveModels, bool enableMetadataTiebreaker)
```

With:
```cpp
__global__ void pixelSelectionKernel(
    const float* __restrict__ bandCube,
    float* __restrict__ outputPixels,
    uint8_t* __restrict__ distTypes,
    uint32_t* __restrict__ provenanceOut,
    char* __restrict__ workspace,
    const WorkspaceLayout layout,
    const double* __restrict__ qualityScores,
    int nSubs,
    int bandStartRow,
    int bandHeight,
    int fullWidth,
    int bandPixels,
    int maxOutliers, double outlierAlpha,
    bool adaptiveModels, bool enableMetadataTiebreaker)
```

- [ ] **Step 2: Replace kernel body with workspace + grid-stride pattern**

Replace the kernel body. The new structure:

```cpp
{
    int slotIdx = blockIdx.x * blockDim.x + threadIdx.x;
    char* myWS = workspace + slotIdx * layout.bytesPerSlot;

    // Workspace pointers (computed once per thread, reused across grid-stride iterations)
    double* zValues      = wsPtr<double>(myWS, layout.off_zValues);
    double* preFiltered  = wsPtr<double>(myWS, layout.off_preFiltered);
    double* cleanData    = wsPtr<double>(myWS, layout.off_cleanData);
    double* sortedClean  = wsPtr<double>(myWS, layout.off_sortedClean);
    double* scratch_d1   = wsPtr<double>(myWS, layout.off_scratch_d1);
    double* scratch_d2   = wsPtr<double>(myWS, layout.off_scratch_d2);
    double* scratch_d3   = wsPtr<double>(myWS, layout.off_scratch_d3);
    int*    preFilteredIdx  = wsPtr<int>(myWS, layout.off_preFilteredIdx);
    int*    cleanIdx        = wsPtr<int>(myWS, layout.off_cleanIdx);
    int*    sortedCleanIdx  = wsPtr<int>(myWS, layout.off_sortedCleanIdx);
    int*    scratch_i1      = wsPtr<int>(myWS, layout.off_scratch_i1);
    int*    scratch_i2      = wsPtr<int>(myWS, layout.off_scratch_i2);
    bool*   madOutlier   = wsPtr<bool>(myWS, layout.off_madOutlier);
    bool*   esdOutlier   = wsPtr<bool>(myWS, layout.off_esdOutlier);
    bool*   allOutlier   = wsPtr<bool>(myWS, layout.off_allOutlier);

    // Grid-stride loop over band pixels
    for (int bp = slotIdx; bp < bandPixels; bp += gridDim.x * blockDim.x) {
        int localY = bp / fullWidth;
        int x      = bp % fullWidth;
        int globalY = bandStartRow + localY;
        int outputIdx = globalY * fullWidth + x;

        // Z-column in band cube (column-major: z + localY*nSubs + x*nSubs*bandHeight)
        const float* zCol = bandCube + localY * nSubs + x * nSubs * bandHeight;

        // === PIXEL PROCESSING (same logic as before, using workspace pointers) ===

        // 1. Promote to double
        for (int i = 0; i < nSubs; ++i)
            zValues[i] = static_cast<double>(zCol[i]);

        // 2a. MAD sigma-clip (scratch_d1 = sorted, scratch_d2 = deviations)
        sigmaClipMAD_device(zValues, nSubs, 3.0, madOutlier, scratch_d1, scratch_d2);

        // 2b. Build pre-filtered data
        int nPreFiltered = 0;
        for (int i = 0; i < nSubs; ++i) {
            if (!madOutlier[i]) {
                preFiltered[nPreFiltered] = zValues[i];
                preFilteredIdx[nPreFiltered] = i;
                ++nPreFiltered;
            }
        }

        // 2c. ESD on pre-filtered (scratch_d1=working, scratch_i1=indexMap,
        //     scratch_d2=testStats, scratch_d3=critVals, scratch_i2=removedOrigIdx)
        for (int i = 0; i < nSubs; ++i)
            allOutlier[i] = madOutlier[i];
        if (nPreFiltered >= 3) {
            detectOutliersESD_device(preFiltered, nPreFiltered, maxOutliers,
                outlierAlpha, esdOutlier,
                scratch_d1, scratch_i1, scratch_d2, scratch_d3, scratch_i2);
            for (int i = 0; i < nPreFiltered; ++i)
                if (esdOutlier[i]) allOutlier[preFilteredIdx[i]] = true;
        }

        // 3. Build clean data
        int nClean = 0;
        for (int i = 0; i < nSubs; ++i) {
            if (!allOutlier[i]) {
                cleanData[nClean] = zValues[i];
                cleanIdx[nClean] = i;
                ++nClean;
            }
        }

        // 4. Relaxation
        if (nClean < 3) {
            if (nPreFiltered >= 3) {
                nClean = nPreFiltered;
                for (int i = 0; i < nPreFiltered; ++i) {
                    cleanData[i] = preFiltered[i];
                    cleanIdx[i] = preFilteredIdx[i];
                }
            } else {
                nClean = nSubs;
                for (int i = 0; i < nSubs; ++i) {
                    cleanData[i] = zValues[i];
                    cleanIdx[i] = i;
                }
            }
        }

        // 5. Sort clean data (parallel sort of value + index)
        for (int i = 0; i < nClean; ++i) {
            sortedClean[i] = cleanData[i];
            sortedCleanIdx[i] = cleanIdx[i];
        }
        for (int i = 1; i < nClean; ++i) {
            double keyVal = sortedClean[i];
            int keyIdx = sortedCleanIdx[i];
            int j = i - 1;
            while (j >= 0 && sortedClean[j] > keyVal) {
                sortedClean[j + 1] = sortedClean[j];
                sortedCleanIdx[j + 1] = sortedCleanIdx[j];
                --j;
            }
            sortedClean[j + 1] = keyVal;
            sortedCleanIdx[j + 1] = keyIdx;
        }
        double cleanMedian = medianDevice(sortedClean, nClean);

        uint8_t bestType = DIST_GAUSSIAN;
        int bestZ = 0;

        if (nClean >= 3) {
            // 6. Shortest-half mode (scratch_d1 = shValues, scratch_d2 = shDeviations)
            double selectedValue = cleanMedian;
            int halfN = nClean / 2;
            if (halfN < 1) halfN = 1;
            int shBestStart = 0;
            {
                double minRange = sortedClean[halfN - 1] - sortedClean[0];
                for (int i = 1; i + halfN - 1 < nClean; ++i) {
                    double range = sortedClean[i + halfN - 1] - sortedClean[i];
                    if (range < minRange) { minRange = range; shBestStart = i; }
                }
                double sum = 0.0;
                for (int i = shBestStart; i < shBestStart + halfN; ++i)
                    sum += sortedClean[i];
                selectedValue = sum / halfN;
            }

            // Closest frame
            double bestDist = 1e300;
            for (int i = 0; i < nClean; ++i) {
                double dist = fabs(cleanData[i] - selectedValue);
                if (dist < bestDist) { bestDist = dist; bestZ = cleanIdx[i]; }
            }

            // Metadata tiebreaker (qualityScores via __ldg from global memory)
            if (enableMetadataTiebreaker && halfN > 1 && qualityScores != nullptr) {
                double shLo = sortedClean[shBestStart];
                double shHi = sortedClean[shBestStart + halfN - 1];

                double* shValues = scratch_d1;
                double* shDeviations = scratch_d2;
                for (int i = 0; i < halfN; ++i)
                    shValues[i] = sortedClean[shBestStart + i];
                double shMedian = medianDevice(shValues, halfN);
                for (int i = 0; i < halfN; ++i)
                    shDeviations[i] = fabs(shValues[i] - shMedian);
                insertionSort(shDeviations, halfN);
                double shMAD = 1.4826 * medianDevice(shDeviations, halfN);

                if (shMAD > 0.0) {
                    double bestScore = __ldg(&qualityScores[bestZ]);
                    for (int i = 0; i < nClean; ++i) {
                        double val = cleanData[i];
                        int origIdx = cleanIdx[i];
                        if (val >= shLo && val <= shHi &&
                            fabs(val - selectedValue) <= shMAD) {
                            double score = __ldg(&qualityScores[origIdx]);
                            if (score > bestScore) {
                                bestScore = score;
                                bestZ = origIdx;
                            }
                        }
                    }
                }
            }

            cleanMedian = selectedValue;

            // 7. Distribution fitting (same as before, bimodal uses scratch_d1/d2)
            FitResultDevice gaussFit = fitGaussian_device(cleanData, nClean);
            double aicGauss = aiccDevice(gaussFit.logLikelihood, gaussFit.k, nClean);
            FitResultDevice poisFit = fitPoisson_device(cleanData, nClean);
            double aicPois = aiccDevice(poisFit.logLikelihood, poisFit.k, nClean);

            double aicSkew = 1e300, aicMix = 1e300;
            bool skipExpensive = false;
            if (adaptiveModels) {
                double best = fmin(aicGauss, aicPois);
                skipExpensive = (nClean < 6) || (best / static_cast<double>(nClean) < 2.0);
            }
            if (!skipExpensive) {
                FitResultDevice skewFit = fitSkewNormal_device(cleanData, nClean);
                aicSkew = aiccDevice(skewFit.logLikelihood, skewFit.k, nClean);
                FitResultDevice mixFit = fitBimodalEM_device(cleanData, nClean,
                    scratch_d1, scratch_d2);
                if (mixFit.weight > 0.05 && mixFit.weight < 0.95)
                    aicMix = aiccDevice(mixFit.logLikelihood, mixFit.k, nClean);
            }

            // Model selection
            double bestAIC = aicGauss; bestType = DIST_GAUSSIAN;
            if (aicPois < bestAIC)  { bestAIC = aicPois;  bestType = DIST_POISSON; }
            if (aicSkew < bestAIC)  { bestAIC = aicSkew;  bestType = DIST_SKEW_NORMAL; }
            if (aicMix < bestAIC)   { bestType = DIST_BIMODAL; }
        }

        // 8. Output
        outputPixels[outputIdx] = static_cast<float>(cleanMedian);
        distTypes[outputIdx] = bestType;
        if (provenanceOut != nullptr)
            provenanceOut[outputIdx] = static_cast<uint32_t>(bestZ);
    }
}
```

**Key differences from old kernel:**
- `workspace` + `WorkspaceLayout` replace stack arrays
- `qualityScores` pointer with `__ldg()` replaces `d_qualityScores` constant memory
- Grid-stride loop (`for (bp = slotIdx; bp < bandPixels; bp += stride)`)
- Band-local addressing: `bandCube` with `bandHeight` + `bandStartRow` offset for output

- [ ] **Step 2: Build**

```bash
cd /home/scarter4work/projects/nukex3/build && cmake .. && make -j$(nproc)
```

Expected: Clean compile (host function still uses old launch pattern — will fail at link only if signatures mismatch, but the host function is updated in Task 6).

Proceed directly to Task 6 (host function) — do NOT attempt to build or commit yet.

---

#### Task 6: Refactor processImageGPU Host Function — Row-Band Chunking + RAII Cleanup

**Files:**
- Modify: `src/engine/cuda/CudaPixelSelector.cu:1060-1161` (processImageGPU)
- Modify: `src/engine/cuda/CudaPixelSelector.h:14-39` (GpuStackConfig, function comment)

- [ ] **Step 1: Update CudaPixelSelector.h**

Update the `GpuStackConfig` comment (quality scores are now uploaded to global memory, not constant memory):

```cpp
struct GpuStackConfig {
    int maxOutliers = 3;
    double outlierAlpha = 0.05;
    bool adaptiveModels = false;
    bool enableMetadataTiebreaker = false;
    size_t nSubs = 0;
    size_t height = 0;
    size_t width = 0;
    const double* qualityScores = nullptr;  // host pointer, uploaded to device global memory
    uint32_t* provenanceOut = nullptr;      // host output, optional (nullptr OK)
};
```

- [ ] **Step 2: Rewrite processImageGPU with row-band chunking and RAII cleanup**

Replace the entire `processImageGPU` function (lines ~1046–1161) with:

```cpp
// RAII guard for CUDA device memory
struct CudaMemGuard {
    void** ptrs;
    int count;
    ~CudaMemGuard() { for (int i = 0; i < count; ++i) if (ptrs[i]) cudaFree(ptrs[i]); }
};

GpuStackResult processImageGPU(
    const float* cubeData,
    float* outputPixels,
    uint8_t* distTypes,
    const GpuStackConfig& config)
{
    GpuStackResult result;
    result.success = false;

    const size_t nSubs = config.nSubs;
    const size_t H     = config.height;
    const size_t W     = config.width;
    const size_t totalPixels = H * W;

    if (nSubs == 0 || H == 0 || W == 0) {
        result.errorMessage = "Empty dimensions";
        return result;
    }

    // Workspace layout
    WorkspaceLayout layout = computeWorkspaceLayout(static_cast<int>(nSubs));

    // Query available VRAM
    size_t freeMem = 0, totalMem = 0;
    cudaMemGetInfo(&freeMem, &totalMem);

    // Budget: reserve 256 MB headroom for driver + PI
    const size_t HEADROOM = 256ULL * 1024 * 1024;
    size_t budget = (freeMem > HEADROOM) ? freeMem - HEADROOM : freeMem / 2;

    // Fixed allocations (output + distTypes + provenance + qualityScores)
    size_t fixedBytes = totalPixels * sizeof(float)          // d_output
                      + totalPixels * sizeof(uint8_t)        // d_distTypes
                      + nSubs * sizeof(double);              // d_qualityScores
    if (config.provenanceOut)
        fixedBytes += totalPixels * sizeof(uint32_t);

    if (budget < fixedBytes + layout.bytesPerSlot * 256) {
        result.errorMessage = "Insufficient VRAM for GPU stacking";
        return result;
    }
    size_t remaining = budget - fixedBytes;

    // Determine grid size and band height
    // Strategy: pick grid size first (caps workspace), then maximize band height
    constexpr int BLOCK_SIZE = 256;
    int maxBlocks = 80;  // ~1 block per SM for RTX 5070 Ti; conservative
    size_t workspaceBytes = static_cast<size_t>(maxBlocks) * BLOCK_SIZE * layout.bytesPerSlot;

    // If workspace doesn't fit, reduce blocks
    while (workspaceBytes > remaining / 2 && maxBlocks > 4) {
        maxBlocks /= 2;
        workspaceBytes = static_cast<size_t>(maxBlocks) * BLOCK_SIZE * layout.bytesPerSlot;
    }

    int numSlots = maxBlocks * BLOCK_SIZE;
    size_t bandBudget = remaining - workspaceBytes;

    // Band height: how many rows fit in bandBudget?
    size_t cubeRowBytes = nSubs * W * sizeof(float);
    size_t bandH = bandBudget / cubeRowBytes;
    if (bandH > H) bandH = H;
    if (bandH < 1) bandH = 1;

    // Allocate device memory
    float*    d_output     = nullptr;
    uint8_t*  d_distTypes  = nullptr;
    uint32_t* d_provenance = nullptr;
    double*   d_quality    = nullptr;
    char*     d_workspace  = nullptr;
    float*    d_bandCube   = nullptr;

    void* allPtrs[6] = {};
    CudaMemGuard guard{allPtrs, 6};

    auto cuCheck = [&](cudaError_t err, const char* ctx) -> bool {
        if (err != cudaSuccess) {
            char buf[512];
            snprintf(buf, sizeof(buf), "%s: %s", ctx, cudaGetErrorString(err));
            result.errorMessage = buf;
            return false;
        }
        return true;
    };

    if (!cuCheck(cudaMalloc(&d_output,    totalPixels * sizeof(float)),   "alloc output"))    return result;
    allPtrs[0] = d_output;
    if (!cuCheck(cudaMalloc(&d_distTypes, totalPixels * sizeof(uint8_t)), "alloc distTypes")) return result;
    allPtrs[1] = d_distTypes;
    if (!cuCheck(cudaMalloc(&d_quality,   nSubs * sizeof(double)),        "alloc quality"))   return result;
    allPtrs[2] = d_quality;
    if (!cuCheck(cudaMalloc(&d_workspace, numSlots * layout.bytesPerSlot),"alloc workspace")) return result;
    allPtrs[3] = d_workspace;
    if (!cuCheck(cudaMalloc(&d_bandCube,  bandH * W * nSubs * sizeof(float)), "alloc bandCube")) return result;
    allPtrs[4] = d_bandCube;

    if (config.provenanceOut) {
        if (!cuCheck(cudaMalloc(&d_provenance, totalPixels * sizeof(uint32_t)), "alloc provenance")) return result;
        allPtrs[5] = d_provenance;
    }

    // Upload quality scores (once)
    if (config.qualityScores) {
        if (!cuCheck(cudaMemcpy(d_quality, config.qualityScores,
                                nSubs * sizeof(double), cudaMemcpyHostToDevice),
                     "upload quality scores")) return result;
    }

    // Host-side band gather buffer (only needed if bandH < H)
    std::vector<float> bandBuf;
    if (bandH < H)
        bandBuf.resize(nSubs * bandH * W);

    // Process row-bands
    for (size_t startRow = 0; startRow < H; startRow += bandH) {
        size_t curBandH = std::min(bandH, H - startRow);
        size_t bandPixels = curBandH * W;

        // Gather band cube data
        const float* uploadSrc;
        size_t uploadBytes = nSubs * curBandH * W * sizeof(float);

        if (curBandH == H) {
            // Entire image fits — upload directly, no gather needed
            uploadSrc = cubeData;
        } else {
            // Gather rows into contiguous band buffer
            for (size_t x = 0; x < W; ++x) {
                std::memcpy(
                    bandBuf.data() + x * nSubs * curBandH,
                    cubeData + startRow * nSubs + x * nSubs * H,
                    nSubs * curBandH * sizeof(float));
            }
            uploadSrc = bandBuf.data();
        }

        // Upload band cube
        if (!cuCheck(cudaMemcpy(d_bandCube, uploadSrc, uploadBytes,
                                cudaMemcpyHostToDevice), "upload band cube")) return result;

        // Launch kernel
        int gridBlocks = std::min(maxBlocks,
            static_cast<int>((bandPixels + BLOCK_SIZE - 1) / BLOCK_SIZE));

        pixelSelectionKernel<<<gridBlocks, BLOCK_SIZE>>>(
            d_bandCube, d_output, d_distTypes, d_provenance,
            d_workspace, layout,
            config.qualityScores ? d_quality : nullptr,
            static_cast<int>(nSubs),
            static_cast<int>(startRow),
            static_cast<int>(curBandH),
            static_cast<int>(W),
            static_cast<int>(bandPixels),
            config.maxOutliers, config.outlierAlpha,
            config.adaptiveModels, config.enableMetadataTiebreaker);

        if (!cuCheck(cudaGetLastError(), "kernel launch")) return result;
        if (!cuCheck(cudaDeviceSynchronize(), "kernel sync")) return result;
    }

    // Download results
    if (!cuCheck(cudaMemcpy(outputPixels, d_output,
                            totalPixels * sizeof(float), cudaMemcpyDeviceToHost),
                 "download output")) return result;
    if (!cuCheck(cudaMemcpy(distTypes, d_distTypes,
                            totalPixels * sizeof(uint8_t), cudaMemcpyDeviceToHost),
                 "download distTypes")) return result;
    if (config.provenanceOut && d_provenance) {
        if (!cuCheck(cudaMemcpy(config.provenanceOut, d_provenance,
                                totalPixels * sizeof(uint32_t), cudaMemcpyDeviceToHost),
                     "download provenance")) return result;
    }

    result.success = true;
    return result;
}
```

**Key changes from old function:**
- No `MAX_SUBS` check — any `nSubs` is supported
- `CudaMemGuard` RAII — all device memory freed on any error path
- Row-band chunking with VRAM-aware sizing
- Quality scores uploaded to global memory (not constant memory)
- Workspace pool allocated based on grid size
- No `cudaDeviceSetLimit(cudaLimitStackSize, ...)` — kernel no longer uses large stack

- [ ] **Step 3: Build**

```bash
cd /home/scarter4work/projects/nukex3/build && cmake .. && make -j$(nproc)
```

Expected: Clean compile.

- [ ] **Step 4: Run tests**

```bash
cd /home/scarter4work/projects/nukex3/build && ctest --output-on-failure
```

Expected: All existing tests pass. The two new high-subs tests from Task 3 **now pass**.

- [ ] **Step 5: Commit Tasks 4+5+6 as a single atomic change**

```bash
git add src/engine/cuda/CudaPixelSelector.cu src/engine/cuda/CudaPixelSelector.h src/engine/cuda/CudaWorkspace.h
git commit -m "feat: remove GPU MAX_SUBS limit — workspace pool + row-band chunking

Replace fixed-size stack arrays (MAX_SUBS=64) with dynamically-sized global
memory workspace pool. Add row-band chunking for cube data transfer so any
sub count fits in VRAM. RAII cleanup for all device allocations."
```

---

### Task 7: Verify All Tests Pass + Clean Up

**Files:**
- Review: all modified files

- [ ] **Step 1: Full rebuild and test**

```bash
cd /home/scarter4work/projects/nukex3/build && cmake .. && make -j$(nproc) && ctest --output-on-failure
```

Expected: All tests pass, including the two new high-subs tests.

- [ ] **Step 2: Quick manual smoke test with small data**

Run the equivalence test binary directly to see verbose output:

```bash
cd /home/scarter4work/projects/nukex3/build && ./test_cuda_equivalence -v
```

Expected: All CUDA equivalence tests pass with results matching CPU within tolerance.

- [ ] **Step 3: Verify no warnings in build**

```bash
cd /home/scarter4work/projects/nukex3/build && make -j$(nproc) 2>&1 | grep -i warning | head -20
```

Expected: No new warnings from the CUDA code.

- [ ] **Step 4: Commit any cleanup**

If any issues were found and fixed, commit:

```bash
git add -u
git commit -m "fix: address review issues from GPU workspace refactor"
```

---

## Summary of Deliverables

| What | Before | After |
|------|--------|-------|
| Max GPU subs | 64 (hard limit) | Unlimited (VRAM-bounded) |
| GPU failure visibility | stderr only (invisible in PI) | PI console warning |
| CUDA memory cleanup | Leaks on error | RAII guard |
| Quality scores | `__constant__` 64-element array | Global memory with `__ldg()` |
| Large image support | Uploads entire cube (may OOM) | Row-band chunking |
| Kernel scheduling | One pixel per thread, fixed grid | Grid-stride loop, VRAM-aware grid |

## Future Optimizations (not in scope)

- **CUDA streams + async transfers**: overlap band upload with previous band's kernel execution
- **Workspace overlap analysis**: reduce per-slot bytes by reusing non-concurrent arrays more aggressively (current plan already overlaps scratch arrays)
- **CudaRemediation.cu MAX_SUBS=128**: fix when trail remediation is revived (currently dead code)
