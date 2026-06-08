# Progress Observer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add hierarchical progress reporting and cancellation support so PI users see a progress bar, per-frame/per-batch detail in the Console, and can cancel mid-execution.

**Architecture:** A `ProgressObserver` abstract class in the core library (no PCL dependency) with a `NukeXProgress` PI adapter in the module layer. The observer threads through `StackingEngine::execute()` and `GPUExecutor::execute_phase_b()`. A `NullProgressObserver` default ensures all existing tests and headless callers work unchanged. A `RecordingObserver` test helper validates phase lifecycle and cancellation.

**Tech Stack:** C++17, PCL (Console, StandardStatus, StatusMonitor), Catch2 v3

**Spec:** `docs/superpowers/specs/2026-04-07-progress-observer-design.md`

---

## File Map

| File | Action | Responsibility |
|------|--------|---------------|
| `src/lib/core/include/nukex/core/progress_observer.hpp` | CREATE | Abstract interface + NullProgressObserver |
| `src/lib/stacker/include/nukex/stacker/stacking_engine.hpp` | MODIFY | Add ProgressObserver* parameter |
| `src/lib/stacker/src/stacking_engine.cpp` | MODIFY | Instrument all phases with observer calls |
| `src/lib/gpu/include/nukex/gpu/gpu_executor.hpp` | MODIFY | Add ProgressObserver* parameter |
| `src/lib/gpu/src/gpu_executor.cpp` | MODIFY | Instrument batch loop with observer calls |
| `src/module/NukeXProgress.h` | CREATE | PI adapter header |
| `src/module/NukeXProgress.cpp` | CREATE | PI adapter: Console + StatusMonitor bridge |
| `src/module/NukeXInstance.cpp` | MODIFY | Wire NukeXProgress into ExecuteGlobal() |
| `src/module/CMakeLists.txt` | MODIFY | Add NukeXProgress.cpp |
| `test/unit/stacker/test_stacking_engine.cpp` | MODIFY | Add RecordingObserver + cancellation tests |

---

### Task 1: Create ProgressObserver abstract interface

**Files:**
- Create: `src/lib/core/include/nukex/core/progress_observer.hpp`

- [ ] **Step 1: Create the header**

```cpp
// src/lib/core/include/nukex/core/progress_observer.hpp
#pragma once

#include <string>

namespace nukex {

/// Abstract progress observer for pipeline instrumentation.
/// Phases nest: begin_phase/end_phase pairs can be called inside
/// an outer phase. The adapter decides how to render nesting.
class ProgressObserver {
public:
    virtual ~ProgressObserver() = default;

    /// Open a new phase scope with a human-readable name and total step count.
    virtual void begin_phase(const std::string& name, int total_steps) = 0;

    /// Advance the current phase by `steps` (0 = detail-only, no bar movement).
    /// Optional detail string for substep labels (e.g., "debayering").
    virtual void advance(int steps = 1, const std::string& detail = {}) = 0;

    /// Close the current phase scope. Must match a prior begin_phase().
    virtual void end_phase() = 0;

    /// Freeform log message (GPU info, warnings, timing).
    virtual void message(const std::string& text) = 0;

    /// Check if the user has requested cancellation.
    virtual bool is_cancelled() const = 0;
};

/// No-op observer for unit tests and headless use.
class NullProgressObserver : public ProgressObserver {
public:
    void begin_phase(const std::string&, int) override {}
    void advance(int, const std::string&) override {}
    void end_phase() override {}
    void message(const std::string&) override {}
    bool is_cancelled() const override { return false; }
};

/// Static instance for use as default when no observer is provided.
inline NullProgressObserver& null_progress_observer() {
    static NullProgressObserver instance;
    return instance;
}

} // namespace nukex
```

- [ ] **Step 2: Verify build**

Run: `cd /home/scarter4work/projects/nukex4/build && cmake .. && make -j$(nproc) 2>&1 | tail -5`
Expected: Build succeeds (header-only, no new source files to compile)

- [ ] **Step 3: Commit**

```bash
git add src/lib/core/include/nukex/core/progress_observer.hpp
git commit -m "feat: add ProgressObserver abstract interface and NullProgressObserver"
```

---

### Task 2: Add ProgressObserver to StackingEngine signature

**Files:**
- Modify: `src/lib/stacker/include/nukex/stacker/stacking_engine.hpp`

- [ ] **Step 1: Update the header**

Add `#include "nukex/core/progress_observer.hpp"` and change the `execute()` signature:

```cpp
// Before:
    Result execute(const std::vector<std::string>& light_paths,
                   const std::vector<std::string>& flat_paths);

// After:
    Result execute(const std::vector<std::string>& light_paths,
                   const std::vector<std::string>& flat_paths,
                   ProgressObserver* progress = nullptr);
```

- [ ] **Step 2: Update the implementation signature**

In `src/lib/stacker/src/stacking_engine.cpp`, change the function signature at line 79:

```cpp
// Before:
StackingEngine::Result StackingEngine::execute(
    const std::vector<std::string>& light_paths,
    const std::vector<std::string>& flat_paths)
{

// After:
StackingEngine::Result StackingEngine::execute(
    const std::vector<std::string>& light_paths,
    const std::vector<std::string>& flat_paths,
    ProgressObserver* progress)
{
    ProgressObserver& obs = progress ? *progress : null_progress_observer();
```

Add include at top of file:
```cpp
#include "nukex/core/progress_observer.hpp"
```

- [ ] **Step 3: Verify build + existing tests pass**

Run: `cd /home/scarter4work/projects/nukex4/build && cmake .. && make -j$(nproc) && ctest --output-on-failure -R test_stacking_engine`
Expected: Build succeeds, both stacking engine tests pass unchanged (nullptr default)

- [ ] **Step 4: Commit**

```bash
git add src/lib/stacker/include/nukex/stacker/stacking_engine.hpp \
        src/lib/stacker/src/stacking_engine.cpp
git commit -m "feat: add ProgressObserver parameter to StackingEngine::execute()"
```

---

### Task 3: Add ProgressObserver to GPUExecutor signatures

**Files:**
- Modify: `src/lib/gpu/include/nukex/gpu/gpu_executor.hpp`
- Modify: `src/lib/gpu/src/gpu_executor.cpp`

- [ ] **Step 1: Update gpu_executor.hpp**

Add `#include "nukex/core/progress_observer.hpp"` and update the two public method signatures:

```cpp
// execute_phase_b: add ProgressObserver* at the end
    void execute_phase_b(
        Cube& cube,
        FrameCache& cache,
        const std::vector<FrameStats>& frame_stats,
        const WeightConfig& weight_config,
        FittingFn fitting_fn,
        Image& stacked_output,
        Image& noise_output,
        ProgressObserver* progress = nullptr);

// execute_spatial_context: add ProgressObserver* at the end
    void execute_spatial_context(
        const Image& stacked,
        Cube& cube,
        ProgressObserver* progress = nullptr);
```

- [ ] **Step 2: Update gpu_executor.cpp signatures**

Update `execute_phase_b()` at line 374:

```cpp
// Before:
void GPUExecutor::execute_phase_b(
    Cube& cube,
    FrameCache& cache,
    const std::vector<FrameStats>& frame_stats,
    const WeightConfig& weight_config,
    FittingFn fitting_fn,
    Image& stacked_output,
    Image& noise_output) {

// After:
void GPUExecutor::execute_phase_b(
    Cube& cube,
    FrameCache& cache,
    const std::vector<FrameStats>& frame_stats,
    const WeightConfig& weight_config,
    FittingFn fitting_fn,
    Image& stacked_output,
    Image& noise_output,
    ProgressObserver* progress) {

    ProgressObserver& obs = progress ? *progress : null_progress_observer();
```

Update `execute_spatial_context()` at line 460:

```cpp
// Before:
void GPUExecutor::execute_spatial_context(
    const Image& stacked,
    Cube& cube) {

// After:
void GPUExecutor::execute_spatial_context(
    const Image& stacked,
    Cube& cube,
    ProgressObserver* progress) {

    ProgressObserver& obs = progress ? *progress : null_progress_observer();
```

Add include at top of file:
```cpp
#include "nukex/core/progress_observer.hpp"
```

- [ ] **Step 3: Verify build + all GPU tests pass**

Run: `cd /home/scarter4work/projects/nukex4/build && cmake .. && make -j$(nproc) && ctest --output-on-failure -R test_gpu`
Expected: Build succeeds, all 4 GPU tests pass unchanged

- [ ] **Step 4: Commit**

```bash
git add src/lib/gpu/include/nukex/gpu/gpu_executor.hpp \
        src/lib/gpu/src/gpu_executor.cpp
git commit -m "feat: add ProgressObserver parameter to GPUExecutor methods"
```

---

### Task 4: Instrument StackingEngine with observer calls

**Files:**
- Modify: `src/lib/stacker/src/stacking_engine.cpp`

- [ ] **Step 1: Instrument Phase A (frame loading loop)**

Replace the Phase A loop (lines 135-196) with instrumented version. The `obs` reference was created in Task 2.

After the cube/cache allocation and before the loop (after line 131), add:
```cpp
    obs.message("Frames: " + std::to_string(n_frames) + " light"
                + (flat_paths.empty() ? "" : ", " + std::to_string(flat_paths.size()) + " flat"));
    obs.begin_phase("Phase A: Loading frames", n_frames);
```

Inside the frame loop, instrument each substep. Replace the loop body (lines 135-196):

```cpp
    for (int f = 0; f < n_frames; f++) {
        // Extract filename for display
        std::string filename = light_paths[f];
        auto slash = filename.rfind('/');
        if (slash != std::string::npos) filename = filename.substr(slash + 1);
        obs.advance(0, "Frame " + std::to_string(f + 1) + "/" + std::to_string(n_frames)
                       + ": " + filename);

        // 1. Load
        auto read_result = (f == 0) ? std::move(first) : FITSReader::read(light_paths[f]);
        if (!read_result.success) {
            obs.advance(1, "  skipped (read failed)");
            continue;
        }

        Image image = std::move(read_result.image);
        auto& meta = read_result.metadata;

        // 2. Debayer
        if (bayer != BayerPattern::NONE) {
            obs.advance(0, "  debayering (" + first.metadata.bayer_pattern + ")");
            image = DebayerEngine::debayer(image, bayer);
        }

        // 3. Flat correct
        if (!master_flat.empty()) {
            obs.advance(0, "  flat correcting");
            FlatCalibration::apply(image, master_flat);
        }

        // 4. Align
        obs.advance(0, "  aligning");
        auto aligned = aligner.align(image, f);

        obs.advance(0, "  aligning (" + std::to_string(aligned.stars.stars.size()) + " stars"
                       + (aligned.alignment.is_meridian_flipped ? ", meridian flipped" : "")
                       + ")");

        // 5. Cache aligned frame
        obs.advance(0, "  caching");
        cache.write_frame(f, aligned.image);

        // 6. Frame-level stats
        float frame_median = compute_frame_median(aligned.image);
        float frame_fwhm = compute_median_fwhm(aligned.stars);

        frame_stats[f].read_noise = meta.read_noise;
        frame_stats[f].gain = meta.gain;
        frame_stats[f].exposure = meta.exposure;
        frame_stats[f].has_noise_keywords = meta.has_noise_keywords;
        frame_stats[f].is_meridian_flipped = aligned.alignment.is_meridian_flipped;
        frame_stats[f].frame_weight = aligned.alignment.weight_penalty;
        frame_stats[f].median_luminance = frame_median;
        frame_stats[f].fwhm = frame_fwhm;
        frame_fwhms[f] = frame_fwhm;

        if (aligned.alignment.alignment_failed) {
            result.n_frames_failed_alignment++;
        }

        // 7. Accumulate into cube
        obs.advance(0, "  accumulating");
        for (int y = 0; y < out_height; y++) {
            for (int x = 0; x < out_width; x++) {
                auto& voxel = cube.at(x, y);
                for (int ch = 0; ch < n_ch; ch++) {
                    float value = aligned.image.at(x, y, ch);
                    voxel.welford[ch].update(value);
                    if (voxel.welford[ch].count() == 1) {
                        voxel.histogram[ch].initialize_range(
                            value - 0.1f, value + 0.1f);
                    }
                    voxel.histogram[ch].update(value);
                }
                voxel.n_frames++;
            }
        }

        cube.n_frames_loaded++;
        result.n_frames_processed++;
        obs.advance(1);  // advance the progress bar

        // Cancellation check
        if (obs.is_cancelled()) {
            obs.message("Cancelled during frame loading.");
            obs.end_phase();
            return result;
        }
    }

    obs.end_phase();
```

- [ ] **Step 2: Instrument Between-Phases and Phase B call**

After the between-phases global statistics block (after line 244), add Phase B instrumentation. Replace the Phase B section (lines 246-270):

```cpp
    // ═══ PHASE B — Analysis (GPU-accelerated) ════════════════════════

    ModelSelector fitter(config_.fitting_config);

    // Output images
    Image stacked(out_width, out_height, n_ch);
    Image noise_map(out_width, out_height, n_ch);

    GPUExecutor gpu(config_.gpu_config);

    // Report GPU backend
    if (gpu.active_backend() == GPUBackend::OPENCL) {
        const auto& di = gpu.device_info();
        obs.message("GPU: " + di.name + " (OpenCL, "
                    + std::to_string(di.global_mem_bytes / (1024*1024)) + " MB)");
    } else {
        obs.message("GPU: CPU fallback");
    }

    auto fitting_fn = [&fitter](SubcubeVoxel& voxel,
                                 const float* values, const float* weights,
                                 int nf, int nc,
                                 const FrameStats* /*fs*/) {
        for (int ch = 0; ch < nc; ch++) {
            fitter.select(values + ch * nf, weights + ch * nf, nf, voxel, ch);
        }
    };

    gpu.execute_phase_b(cube, cache, frame_stats, config_.weight_config,
                         fitting_fn, stacked, noise_map, &obs);

    if (obs.is_cancelled()) {
        obs.message("Cancelled during distribution fitting.");
        return result;
    }
```

- [ ] **Step 3: Instrument Phase C (post-processing)**

Replace the post-processing section (lines 272-298):

```cpp
    // Post-processing: dominant shape + quality scores
    obs.begin_phase("Phase C: Post-processing", 3);

    obs.advance(1, "dominant shape computation");
    for (int y = 0; y < out_height; y++) {
        for (int x = 0; x < out_width; x++) {
            auto& voxel = cube.at(x, y);
            compute_dominant_shape(voxel, n_ch);
        }
    }

    obs.advance(1, "quality scores");
    for (int y = 0; y < out_height; y++) {
        for (int x = 0; x < out_width; x++) {
            auto& voxel = cube.at(x, y);
            float avg_snr = 0.0f;
            for (int ch = 0; ch < n_ch; ch++) avg_snr += voxel.snr[ch];
            avg_snr /= n_ch;
            float cloud_fraction = (voxel.n_frames > 0) ?
                static_cast<float>(voxel.cloud_frame_count) / voxel.n_frames : 0.0f;
            voxel.quality_score = voxel.distribution[0].confidence
                * (1.0f - cloud_fraction)
                * std::min(1.0f, avg_snr / 50.0f);
            voxel.confidence = voxel.distribution[0].confidence;
        }
    }

    // Spatial context
    obs.advance(0, "spatial context");
    gpu.execute_spatial_context(stacked, cube, &obs);
    obs.advance(1);

    obs.end_phase();

    // Quality map
    Image quality_map = OutputAssembler::assemble_quality_map(cube);

    result.stacked = std::move(stacked);
    result.noise_map = std::move(noise_map);
    result.quality_map = std::move(quality_map);

    return result;
```

- [ ] **Step 4: Verify build + existing tests pass**

Run: `cd /home/scarter4work/projects/nukex4/build && cmake .. && make -j$(nproc) && ctest --output-on-failure -R test_stacking_engine`
Expected: Build succeeds, both stacking engine tests pass (nullptr observer = NullProgressObserver)

- [ ] **Step 5: Commit**

```bash
git add src/lib/stacker/src/stacking_engine.cpp
git commit -m "feat: instrument StackingEngine with progress observer calls"
```

---

### Task 5: Instrument GPUExecutor with observer calls

**Files:**
- Modify: `src/lib/gpu/src/gpu_executor.cpp`

- [ ] **Step 1: Instrument execute_phase_b() batch loop**

The `obs` reference was created in Task 3. Replace the batch loop (lines 396-453) with:

```cpp
    int total_batches = (total_voxels + batch_size - 1) / batch_size;
    obs.begin_phase("Phase B: Distribution fitting", total_batches);
    obs.advance(0, std::to_string(total_voxels) + " voxels, "
                   + std::to_string(total_batches) + " batches");

    std::string backend_tag = use_gpu ? " [GPU]" : " [CPU]";

    int processed = 0;
    int batch_idx = 0;
    while (processed < total_voxels) {
        int count = std::min(batch_size, total_voxels - processed);
        batch_idx++;

        obs.advance(0, "Batch " + std::to_string(batch_idx) + "/"
                       + std::to_string(total_batches) + " ("
                       + std::to_string(count) + " voxels)");

        // Step 1: Extract voxel data into SoA buffers
        buf.extract_from_cube(cube, cache, processed, count, n_channels);

        // Steps 2-3: Weight computation + robust stats (GPU or CPU)
        obs.advance(0, "  kernel 1: weight classification" + backend_tag);
        obs.advance(0, "  kernel 2: robust statistics" + backend_tag);
        if (use_gpu) {
            execute_batch_gpu(buf, frame_stats.data(), weight_config,
                              count, n_channels, N);
        } else {
            execute_batch_cpu(buf, frame_stats.data(), weight_config,
                              count, n_channels, N);
        }

        // Step 4: Writeback classification + robust stats
        buf.writeback_classification(cube, processed, count, n_channels);

        // Step 5: CPU fitting (Ceres — cannot run on GPU)
        obs.advance(0, "  fitting distributions (Ceres)");
        int w = cube.width;
        for (int vi = 0; vi < count; vi++) {
            int voxel_idx = processed + vi;
            int px = voxel_idx % w;
            int py = voxel_idx / w;
            auto& voxel = cube.at(px, py);

            std::vector<float> vals(n_channels * N);
            std::vector<float> wts(n_channels * N);
            for (int ch = 0; ch < n_channels; ch++) {
                for (int fi = 0; fi < N; fi++) {
                    vals[ch * N + fi] = buf.pixel_values[ch * N * count + fi * count + vi];
                    wts[ch * N + fi] = buf.pixel_weights[ch * N * count + fi * count + vi];
                }
            }

            fitting_fn(voxel, vals.data(), wts.data(), N,
                        n_channels, frame_stats.data());
        }

        // Step 6: Extract fitted distributions for select_pixels
        buf.extract_distributions(cube, processed, count, n_channels);

        // Step 7: Pixel selection (GPU or CPU)
        obs.advance(0, "  kernel 3: pixel selection" + backend_tag);
        if (use_gpu) {
            execute_select_gpu(buf, frame_stats.data(), count, n_channels, N);
        } else {
            GPUCPUFallback::select_pixels(buf, frame_stats.data(),
                                           count, n_channels, N);
        }

        // Step 8: Writeback output values
        buf.writeback_selection(cube, processed, count, n_channels,
                                 stacked_output.data(), noise_output.data());

        processed += count;
        obs.advance(1);  // batch complete, advance progress bar

        // Cancellation check
        if (obs.is_cancelled()) {
            obs.message("Cancelled during batch " + std::to_string(batch_idx)
                        + "/" + std::to_string(total_batches));
            break;
        }
    }

    obs.end_phase();
```

- [ ] **Step 2: Verify build + all tests pass**

Run: `cd /home/scarter4work/projects/nukex4/build && cmake .. && make -j$(nproc) && ctest --output-on-failure`
Expected: Build succeeds, all tests pass

- [ ] **Step 3: Commit**

```bash
git add src/lib/gpu/src/gpu_executor.cpp
git commit -m "feat: instrument GPUExecutor batch loop with progress observer"
```

---

### Task 6: Create PI adapter (NukeXProgress)

**Files:**
- Create: `src/module/NukeXProgress.h`
- Create: `src/module/NukeXProgress.cpp`
- Modify: `src/module/CMakeLists.txt`

- [ ] **Step 1: Create NukeXProgress.h**

```cpp
// src/module/NukeXProgress.h
#ifndef __NukeXProgress_h
#define __NukeXProgress_h

#include <pcl/Console.h>
#include <pcl/StandardStatus.h>
#include <pcl/StatusMonitor.h>

#include "nukex/core/progress_observer.hpp"
#include <vector>
#include <string>

namespace pcl
{

/// Bridges nukex::ProgressObserver to PI's Console + StatusMonitor.
/// The outermost begin_phase() drives the progress bar.
/// Inner phases and advance() detail strings write indented text to Console.
class NukeXProgress : public nukex::ProgressObserver
{
public:
   NukeXProgress();

   void begin_phase( const std::string& name, int total_steps ) override;
   void advance( int steps, const std::string& detail ) override;
   void end_phase() override;
   void message( const std::string& text ) override;
   bool is_cancelled() const override;

private:
   struct Phase {
      std::string name;
      int total;
      int current;
   };

   Console           console_;
   StandardStatus    status_;
   StatusMonitor     monitor_;
   std::vector<Phase> stack_;
   int               depth_ = 0;  // nesting depth for indentation
};

} // namespace pcl

#endif // __NukeXProgress_h
```

- [ ] **Step 2: Create NukeXProgress.cpp**

```cpp
// src/module/NukeXProgress.cpp
#include "NukeXProgress.h"
#include <pcl/String.h>

namespace pcl
{

NukeXProgress::NukeXProgress()
   : status_()
   , monitor_()
{
}

void NukeXProgress::begin_phase( const std::string& name, int total_steps )
{
   depth_++;
   stack_.push_back( { name, total_steps, 0 } );

   if ( depth_ == 1 )
   {
      // Outermost phase drives the progress bar
      console_.WriteLn( String( "<end><cbr>" ) );
      console_.WriteLn( String().Format( "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90 %s (%d steps) \xe2\x95\x90\xe2\x95\x90\xe2\x95\x90",
                                          name.c_str(), total_steps ) );
      monitor_.SetCallback( &status_ );
      monitor_.Initialize( String( name.c_str() ), total_steps );
   }
   else
   {
      // Nested phase — console only
      String indent;
      for ( int i = 1; i < depth_; i++ ) indent += "  ";
      console_.WriteLn( indent + String( name.c_str() ) );
   }
   console_.Flush();
}

void NukeXProgress::advance( int steps, const std::string& detail )
{
   if ( stack_.empty() ) return;

   auto& phase = stack_.back();

   if ( !detail.empty() )
   {
      String indent;
      for ( int i = 0; i < depth_; i++ ) indent += "  ";
      console_.WriteLn( indent + String( detail.c_str() ) );
      console_.Flush();
   }

   if ( steps > 0 )
   {
      phase.current += steps;

      // Advance the outermost progress bar
      if ( depth_ == 1 )
      {
         for ( int i = 0; i < steps; i++ )
            ++monitor_;
      }
   }
}

void NukeXProgress::end_phase()
{
   if ( stack_.empty() ) return;

   if ( depth_ == 1 )
   {
      // Close the progress bar
      monitor_.Complete();
   }

   stack_.pop_back();
   depth_--;
}

void NukeXProgress::message( const std::string& text )
{
   console_.WriteLn( String( text.c_str() ) );
   console_.Flush();
}

bool NukeXProgress::is_cancelled() const
{
   return monitor_.IsAborted();
}

} // namespace pcl
```

- [ ] **Step 3: Add NukeXProgress.cpp to CMakeLists.txt**

In `src/module/CMakeLists.txt`, add `NukeXProgress.cpp` to MODULE_SOURCES:

```cmake
set(MODULE_SOURCES
    NukeXModule.cpp
    NukeXProcess.cpp
    NukeXInstance.cpp
    NukeXInterface.cpp
    NukeXParameters.cpp
    NukeXProgress.cpp
)
```

- [ ] **Step 4: Verify module build**

Run: `cd /home/scarter4work/projects/nukex4/build && cmake .. -DNUKEX_BUILD_MODULE=ON && make -j$(nproc) NukeX-pxm 2>&1 | tail -10`
Expected: NukeX-pxm.so builds successfully

- [ ] **Step 5: Commit**

```bash
git add src/module/NukeXProgress.h \
        src/module/NukeXProgress.cpp \
        src/module/CMakeLists.txt
git commit -m "feat: create NukeXProgress PI adapter for Console + StatusMonitor"
```

---

### Task 7: Wire NukeXProgress into ExecuteGlobal()

**Files:**
- Modify: `src/module/NukeXInstance.cpp`

- [ ] **Step 1: Replace ExecuteGlobal() implementation**

Add include at top:
```cpp
#include "NukeXProgress.h"
```

Remove includes that are now unused (StandardStatus is used by NukeXProgress):
```cpp
// Remove: #include <pcl/StandardStatus.h>
```

Replace the ExecuteGlobal() method (lines 89-201):

```cpp
bool NukeXInstance::ExecuteGlobal()
{
   NukeXProgress progress;
   progress.message( "NukeX v4 \xe2\x80\x94 Distribution-Fitted Stacking" );
   progress.message( String().Format( "Processing %zu light frame(s)", lightFrames.Length() ).ToUTF8().c_str() );

   // Collect enabled file paths
   std::vector<std::string> light_paths;
   for ( const auto& f : lightFrames )
   {
      if ( f.enabled )
         light_paths.push_back( f.path.ToUTF8().c_str() );
   }

   std::vector<std::string> flat_paths;
   for ( const auto& f : flatFrames )
   {
      if ( f.enabled )
         flat_paths.push_back( f.path.ToUTF8().c_str() );
   }

   if ( light_paths.empty() )
   {
      progress.message( "** No enabled light frames. Aborting." );
      return false;
   }

   // Configure stacking engine
   nukex::StackingEngine::Config config;
   config.cache_dir = cacheDirectory.ToUTF8().c_str();
   config.gpu_config.force_cpu_fallback = !enableGPU;

   // Execute pipeline with progress reporting
   nukex::StackingEngine engine( config );
   auto result = engine.execute( light_paths, flat_paths, &progress );

   // Check cancellation
   if ( progress.is_cancelled() )
   {
      progress.message( "** Cancelled by user." );
      return false;
   }

   if ( result.n_frames_processed == 0 )
   {
      progress.message( "** No frames were processed. Check input files." );
      return false;
   }

   progress.message( String().Format(
      "Stacking complete: %d frame(s) processed, %d failed alignment",
      result.n_frames_processed, result.n_frames_failed_alignment ).ToUTF8().c_str() );

   // Create output ImageWindow with the stacked result
   if ( !result.stacked.empty() )
   {
      int w = result.stacked.width();
      int h = result.stacked.height();
      int nc = result.stacked.n_channels();

      ImageWindow window( w, h, nc,
                          32,    // bits per sample (float32)
                          true,  // float sample
                          nc >= 3, // color if 3+ channels
                          true,  // initialProcessing
                          "NukeX_stacked" );

      View view = window.MainView();
      ImageVariant v = view.Image();

      if ( v.IsFloatSample() && v.BitsPerSample() == 32 )
      {
         pcl::Image& img = static_cast<pcl::Image&>( *v );
         for ( int ch = 0; ch < nc; ch++ )
         {
            const float* src = result.stacked.channel_data( ch );
            float* dst = img.PixelData( ch );
            ::memcpy( dst, src, w * h * sizeof( float ) );
         }
      }

      window.Show();
      progress.message( "Stacked image opened." );
   }

   // Create noise map window
   if ( !result.noise_map.empty() )
   {
      int w = result.noise_map.width();
      int h = result.noise_map.height();
      int nc = result.noise_map.n_channels();

      ImageWindow nw( w, h, nc, 32, true, nc >= 3, true, "NukeX_noise" );
      View nv = nw.MainView();
      ImageVariant nvi = nv.Image();

      if ( nvi.IsFloatSample() && nvi.BitsPerSample() == 32 )
      {
         pcl::Image& ni = static_cast<pcl::Image&>( *nvi );
         for ( int ch = 0; ch < nc; ch++ )
         {
            const float* src = result.noise_map.channel_data( ch );
            float* dst = ni.PixelData( ch );
            ::memcpy( dst, src, w * h * sizeof( float ) );
         }
      }

      nw.Show();
      progress.message( "Noise map opened." );
   }

   progress.message( "NukeX v4 done." );
   return true;
}
```

- [ ] **Step 2: Verify module build**

Run: `cd /home/scarter4work/projects/nukex4/build && cmake .. -DNUKEX_BUILD_MODULE=ON && make -j$(nproc) NukeX-pxm 2>&1 | tail -10`
Expected: NukeX-pxm.so builds successfully

- [ ] **Step 3: Commit**

```bash
git add src/module/NukeXInstance.cpp
git commit -m "feat: wire NukeXProgress into ExecuteGlobal() for live progress reporting"
```

---

### Task 8: Add RecordingObserver tests

**Files:**
- Modify: `test/unit/stacker/test_stacking_engine.cpp`

- [ ] **Step 1: Add RecordingObserver and phase lifecycle test**

Append to `test/unit/stacker/test_stacking_engine.cpp`:

```cpp
#include "nukex/core/progress_observer.hpp"

// ── Test helper: records all observer calls ──────────────────────

class RecordingObserver : public nukex::ProgressObserver {
public:
    struct Event {
        enum Type { BEGIN, ADVANCE, END, MESSAGE } type;
        std::string text;
        int value = 0;
    };

    std::vector<Event> events;
    bool cancelled = false;
    int cancel_after_advances = -1;  // cancel after N advance(steps>0) calls
    int advance_count = 0;

    void begin_phase(const std::string& name, int total) override {
        events.push_back({Event::BEGIN, name, total});
    }
    void advance(int steps, const std::string& detail) override {
        events.push_back({Event::ADVANCE, detail, steps});
        if (steps > 0) {
            advance_count++;
            if (cancel_after_advances > 0 && advance_count >= cancel_after_advances) {
                cancelled = true;
            }
        }
    }
    void end_phase() override {
        events.push_back({Event::END, {}, 0});
    }
    void message(const std::string& text) override {
        events.push_back({Event::MESSAGE, text, 0});
    }
    bool is_cancelled() const override { return cancelled; }

    // Count how many BEGIN events have a matching END
    bool all_phases_closed() const {
        int depth = 0;
        for (const auto& e : events) {
            if (e.type == Event::BEGIN) depth++;
            if (e.type == Event::END) depth--;
            if (depth < 0) return false;  // extra END
        }
        return depth == 0;
    }

    int count_type(Event::Type t) const {
        int n = 0;
        for (const auto& e : events) if (e.type == t) n++;
        return n;
    }

    int count_bar_advances() const {
        int n = 0;
        for (const auto& e : events)
            if (e.type == Event::ADVANCE && e.value > 0) n++;
        return n;
    }
};

TEST_CASE("StackingEngine: observer receives no calls for empty input", "[engine][progress]") {
    RecordingObserver obs;
    StackingEngine::Config cfg;
    cfg.cache_dir = "/tmp";
    StackingEngine engine(cfg);

    auto result = engine.execute({}, {}, &obs);
    REQUIRE(result.n_frames_processed == 0);
    // Empty input returns before any phases start
    REQUIRE(obs.events.empty());
}

TEST_CASE("StackingEngine: observer phases are balanced", "[engine][progress][integration][!mayfail]") {
    std::string data_dir = "/home/scarter4work/projects/processing/M16/";
    if (!std::filesystem::exists(data_dir)) {
        SKIP("M16 test data not available at " + data_dir);
    }

    std::vector<std::string> lights;
    for (const auto& entry : std::filesystem::directory_iterator(data_dir)) {
        auto ext = entry.path().extension().string();
        if (ext == ".fits" || ext == ".fit") {
            lights.push_back(entry.path().string());
            if (lights.size() >= 3) break;
        }
    }
    if (lights.size() < 3) SKIP("Not enough FITS files");

    RecordingObserver obs;
    StackingEngine::Config cfg;
    cfg.cache_dir = "/tmp";
    StackingEngine engine(cfg);

    auto result = engine.execute(lights, {}, &obs);
    REQUIRE(result.n_frames_processed >= 3);

    // All begin_phase/end_phase pairs are balanced
    REQUIRE(obs.all_phases_closed());

    // At least 3 phases: A, B, C
    REQUIRE(obs.count_type(RecordingObserver::Event::BEGIN) >= 3);

    // Progress bar advances at least once per frame in Phase A
    REQUIRE(obs.count_bar_advances() >= static_cast<int>(lights.size()));
}

TEST_CASE("StackingEngine: cancellation mid-Phase-A returns partial result", "[engine][progress][integration][!mayfail]") {
    std::string data_dir = "/home/scarter4work/projects/processing/M16/";
    if (!std::filesystem::exists(data_dir)) {
        SKIP("M16 test data not available at " + data_dir);
    }

    std::vector<std::string> lights;
    for (const auto& entry : std::filesystem::directory_iterator(data_dir)) {
        auto ext = entry.path().extension().string();
        if (ext == ".fits" || ext == ".fit") {
            lights.push_back(entry.path().string());
            if (lights.size() >= 5) break;
        }
    }
    if (lights.size() < 5) SKIP("Need at least 5 FITS files");

    RecordingObserver obs;
    obs.cancel_after_advances = 2;  // Cancel after 2 frames

    StackingEngine::Config cfg;
    cfg.cache_dir = "/tmp";
    StackingEngine engine(cfg);

    auto result = engine.execute(lights, {}, &obs);

    // Should have processed some but not all frames
    REQUIRE(result.n_frames_processed >= 1);
    REQUIRE(result.n_frames_processed < static_cast<int>(lights.size()));

    // Phases should still be balanced (early return closes phases)
    REQUIRE(obs.all_phases_closed());
}
```

- [ ] **Step 2: Run the tests**

Run: `cd /home/scarter4work/projects/nukex4/build && cmake .. && make -j$(nproc) && ctest --output-on-failure -R test_stacking_engine`
Expected: All stacking engine tests pass (empty-input test always runs, integration tests run if M16 data is available)

- [ ] **Step 3: Commit**

```bash
git add test/unit/stacker/test_stacking_engine.cpp
git commit -m "test: add RecordingObserver tests for progress lifecycle and cancellation"
```

---

### Task 9: Full build + test verification

- [ ] **Step 1: Clean rebuild**

Run: `cd /home/scarter4work/projects/nukex4/build && cmake .. && make -j$(nproc) 2>&1 | tail -5`
Expected: Build succeeds with zero errors

- [ ] **Step 2: Run all tests**

Run: `cd /home/scarter4work/projects/nukex4/build && ctest --output-on-failure 2>&1 | tail -20`
Expected: All tests pass (existing + new progress tests)

- [ ] **Step 3: Module build**

Run: `cd /home/scarter4work/projects/nukex4/build && cmake .. -DNUKEX_BUILD_MODULE=ON && make -j$(nproc) NukeX-pxm 2>&1 | tail -5`
Expected: NukeX-pxm.so builds successfully

- [ ] **Step 4: Verify no regressions — run full test suite one more time**

Run: `cd /home/scarter4work/projects/nukex4/build && ctest --output-on-failure`
Expected: 100% pass rate
