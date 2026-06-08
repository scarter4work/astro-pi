# GPU Acceleration & Performance Optimization — Design

**Date**: 2026-03-11
**Version target**: v3.1.0 (major feature addition)
**Problem**: 30 frames take ~40 minutes to stack on 8-core i7-11700K

## Goals

1. Add optional CUDA GPU acceleration for pixel selection (distribution fitting)
2. Improve CPU path performance (analytical gradients, adaptive model selection, parallel I/O)
3. Keep full CPU fallback — GPU is optional, never required
4. Single .so module, conditional CUDA compilation
5. Follow existing release workflow (CLAUDE.md 8-step process)

## Hardware Context

- CPU: Intel i7-11700K, 8 cores / 16 threads
- GPU: NVIDIA RTX 5070 Ti, 16GB VRAM, Compute 12.0 (Blackwell)
- CUDA: 12.8 at /usr/local/cuda-12.8

## New Parameters

| Parameter | ID | Type | Default | Description |
|-----------|-----|------|---------|-------------|
| Use GPU | `useGPU` | Boolean | Auto-detect | Enable CUDA acceleration. Grayed out if no GPU. |
| Adaptive Models | `adaptiveModels` | Boolean | true | Skip expensive fits if Gaussian AIC is excellent. |

## Architecture

### Build System

- CMakeLists.txt: `find_package(CUDAToolkit QUIET)`, conditional CUDA language enable
- Makefile: conditional nvcc detection
- Define `NUKEX_HAS_CUDA` when CUDA available
- Add `-march=native` to C++ compiler flags
- New sources: `src/engine/cuda/CudaRuntime.h/.cpp`, `src/engine/cuda/CudaPixelSelector.h/.cu`
- All CUDA code behind `#ifdef NUKEX_HAS_CUDA` guards

### Runtime Flow

```
Phases 1-2: Frame Loading (parallel) → Alignment → SubCube → QualityWeights
                          |
                   useGPU && CUDA?
                   yes/         \no
            GPU Path             CPU Path
     H2D SubCube→VRAM     OpenMP parallel for
     Launch kernel         adaptiveModels early-exit
     1 thread/pixel        Analytical Skew-Normal grads
     MAD+ESD+Fit+AIC      selectBestZ() per pixel
     D2H results
                   \           /
              Phase 4: Per-channel Stretch
```

### CPU Path Improvements

1. **`-march=native`** — enables AVX-512 on i7-11700K, free vectorization gains
2. **Analytical Skew-Normal gradients** — replace 6 numerical central-difference evaluations per L-BFGS iteration with closed-form derivatives. ~5x speedup on Skew-Normal fitting.
3. **Adaptive model selection** — fit Gaussian first, compute AIC. If AIC indicates excellent fit (delta threshold), skip Skew-Normal and Bimodal. Most sky-background pixels are well-modeled by Gaussian.
4. **Parallel frame loading** — OpenMP task parallelism for concurrent FITS/XISF reads in FrameLoader.

### GPU Path (CUDA)

**CudaRuntime** (`src/engine/cuda/CudaRuntime.h/.cpp`):
- `bool isGpuAvailable()` — runtime CUDA device probe
- `GpuContext initGpu()` — device selection, memory pre-allocation
- `void transferSubCubeToDevice(SubCube&, GpuContext&)` — H2D
- `void transferResultsFromDevice(GpuContext&, Image&)` — D2H
- Error handling: any CUDA failure → Console warning + CPU fallback, never crash

**CudaPixelSelector** (`src/engine/cuda/CudaPixelSelector.h/.cu`):
- `__global__ void pixelSelectionKernel(...)` — one thread per pixel
- Pure CUDA device functions for:
  - `sigmaClipMAD_device()` — bitonic sort + MAD
  - `detectOutliersESD_device()` — ESD on device
  - `fitGaussian_device()` — analytical MLE
  - `fitPoisson_device()` — analytical MLE
  - `fitSkewNormal_device()` — custom L-BFGS with analytical gradients (no Eigen)
  - `fitBimodalEM_device()` — EM loop
  - `aicSelect_device()` — model comparison
- Adaptive model selection supported on device too
- Thread block size: 256 (tunable)
- Grid size: ceil(totalPixels / 256)

**No external dependencies on device**: Boost.Math, Eigen, LBFGSpp are CPU-only. All device functions are self-contained CUDA reimplementations of the same math.

### Memory Budget (GPU)

For 30 frames × 4800 × 3200 × float:
- SubCube: 30 × 4800 × 3200 × 4 bytes = ~1.7 GB
- Output image: 4800 × 3200 × 3 channels × 4 bytes = ~175 MB
- Working memory per thread: minimal (stack-local)
- Total: ~2 GB of 16 GB VRAM — plenty of headroom

### PixelSelector Changes

- `processImage()` — existing CPU path, gains adaptive model selection + analytical gradients
- `processImageGPU()` — new method, CUDA dispatch (behind `#ifdef NUKEX_HAS_CUDA`)
- Selection between paths happens in `NukeXStackInstance::ExecuteGlobal()` based on `useGPU` parameter + runtime detection

### Parameter/Interface/Process Classes

Following PCL patterns (same as existing NukeXStack parameters):
- `NukeXStackParameters`: add `p_useGPU`, `p_adaptiveModels` with PCL parameter definitions
- `NukeXStackInstance`: add corresponding member variables + serialization
- `NukeXStackInterface`: add checkboxes to GUI

## Constraints

- No convergence/iteration count changes — all distribution fits use same tolerances
- CPU path must produce identical results to current v3.0.0.14 when adaptiveModels=false
- GPU path must produce numerically equivalent results (float precision differences acceptable)
- CUDA failure at any point falls back to CPU with Console warning
- Build without CUDA produces identical module to current (no regressions)

## Testing

- Existing 14 test suites must continue passing (CPU path unchanged when adaptiveModels=false)
- New tests: adaptive model selection correctness
- New tests: CUDA kernel output vs CPU output (numerical equivalence within float tolerance)
- New tests: GPU fallback behavior (simulated CUDA failure)
- New tests: build without CUDA (NUKEX_HAS_CUDA not defined)

## Expected Performance

| Configuration | Est. Time (30 frames) |
|--------------|----------------------|
| Current (v3.0.0.14) | ~40 min |
| CPU + march=native | ~32-36 min |
| CPU + analytical grads | ~20-25 min |
| CPU + adaptive models | ~10-15 min |
| CPU all improvements | ~5-10 min |
| GPU (CUDA) | ~1-3 min |
