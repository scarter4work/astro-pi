# Progress Observer — Design Spec

**Date:** 2026-04-07
**Status:** Approved
**Problem:** NukeX v4 provides no progress feedback during execution. The PI Process Console shows a spinner with no progress bar, no phase information, and no cancel button. Console output is buffered and invisible until `ExecuteGlobal()` returns. On real data this looks indistinguishable from a hang.

## Architecture

### Abstract Interface

`src/lib/core/include/nukex/core/progress_observer.hpp` — pure C++, no PCL dependency.

```cpp
namespace nukex {

class ProgressObserver {
public:
    virtual ~ProgressObserver() = default;

    virtual void begin_phase(const std::string& name, int total_steps) = 0;
    virtual void advance(int steps = 1, const std::string& detail = {}) = 0;
    virtual void end_phase() = 0;
    virtual void message(const std::string& text) = 0;
    virtual bool is_cancelled() const = 0;
};

class NullProgressObserver : public ProgressObserver {
public:
    void begin_phase(const std::string&, int) override {}
    void advance(int, const std::string&) override {}
    void end_phase() override {}
    void message(const std::string&) override {}
    bool is_cancelled() const override { return false; }
};

} // namespace nukex
```

**Design decisions:**
- Pure abstract base — no state. Each adapter manages its own nesting stack.
- `NullProgressObserver` ships alongside so callsites can default to a static instance. No null checks needed anywhere in the pipeline.
- `advance()` takes an optional detail string for substep labels (e.g., "debayering") without opening a new phase.
- Phases nest: Phase A > Frame 3/20 > "debayering". The adapter decides how to render nesting.

### PI Adapter (Module Layer)

`src/module/NukeXProgress.h` + `src/module/NukeXProgress.cpp` — the only code that touches PCL.

Bridges the abstract observer to PI's `Console` + `StatusMonitor` + `StandardStatus`.

**Nesting strategy:**
- The **outermost** `begin_phase()` drives `StatusMonitor` (progress bar + abort button).
- Inner phases and `advance()` detail strings write indented text to `Console`.
- Every `message()` and `advance()` calls `Console::Flush()` — output is always visible immediately.
- `is_cancelled()` delegates to `StatusMonitor::IsAborted()`.

**Internal state:** A `std::vector<Phase>` stack where each Phase tracks `{name, total, current}`.

### Example Console Output

```
NukeX v4 — Distribution-Fitted Stacking
GPU: NVIDIA RTX 5070 Ti (OpenCL 3.0, 16384 MB)
═══ Phase A: Loading frames (20 frames) ═══
  Frame 1/20: M31_light_001.fits
    debayering (RGGB)...
    flat correcting...
    aligning (47 stars, FWHM 2.3px)...
    caching...
    accumulating into cube...
  Frame 2/20: M31_light_002.fits
    ...
═══ Phase B: Distribution fitting (65536 voxels, 8 batches) ═══
  Batch 1/8 (8192 voxels)
    kernel 1: weight classification [GPU]...
    kernel 2: robust statistics [GPU]...
    fitting distributions (Ceres)...
    kernel 3: pixel selection [GPU]...
  Batch 2/8 (8192 voxels)
    ...
═══ Phase C: Post-processing ═══
  dominant shape computation...
  quality scores...
  kernel 4: spatial context [GPU]...
═══ Complete: 20 frames, 0 failed ═══
NukeX v4 done. (42.3s)
```

## Integration Points

### Signature Changes

| Method | Current | New |
|--------|---------|-----|
| `StackingEngine::execute()` | `(light_paths, flat_paths)` | `(light_paths, flat_paths, ProgressObserver*)` |
| `GPUExecutor::execute_phase_b()` | `(cube, cache, frame_stats, weight_config, fitting_fn, stacked, noise)` | same + `ProgressObserver*` |
| `GPUExecutor::execute_spatial_context()` | `(stacked, cube)` | same + `ProgressObserver*` |

Observer parameter defaults to `nullptr`. When null, a static `NullProgressObserver` is used.

### StackingEngine::execute() Instrumentation

```
begin_phase("Phase A: Loading frames", n_frames)
  for each frame:
    advance(0, "Frame 3/20: filename.fits")
    // after debayer:  advance(0, "  debayering (RGGB)")
    // after flat:     advance(0, "  flat correcting")
    // after align:    advance(0, "  aligning (47 stars)")
    // after cache:    advance(0, "  caching")
    // after accum:    advance(1, "  accumulating")     // advance bar
    if (is_cancelled()) return partial result
end_phase()

begin_phase("Phase B: Distribution fitting", total_batches)
  // GPUExecutor calls advance() per batch + detail per kernel
end_phase()

begin_phase("Phase C: Post-processing", 3)
  advance(1, "dominant shape computation")
  advance(1, "quality scores")
  advance(1, "spatial context")
end_phase()
```

### GPUExecutor::execute_phase_b() Instrumentation

```
for each batch:
    advance(0, "kernel 1: weight classification [GPU/CPU]")
    advance(0, "kernel 2: robust statistics [GPU/CPU]")
    advance(0, "fitting distributions (Ceres)")
    advance(0, "kernel 3: pixel selection [GPU/CPU]")
    advance(1)  // batch complete, advance bar
    if (is_cancelled()) break
```

### ExecuteGlobal() Wiring

```cpp
bool NukeXInstance::ExecuteGlobal() {
    NukeXProgress progress;
    progress.message("NukeX v4 — Distribution-Fitted Stacking");
    progress.message(gpu_device_info);

    nukex::StackingEngine engine(config);
    auto result = engine.execute(light_paths, flat_paths, &progress);

    if (progress.is_cancelled()) {
        progress.message("** Cancelled by user.");
        return false;
    }
    // ... create ImageWindows as before ...
}
```

## Cancellation Semantics

- **Phase A:** Stop loading new frames. Return partial `Result` with `n_frames_processed` reflecting completed frames only. Skip ImageWindow creation.
- **Phase B:** Stop after current batch completes. Processed voxels have valid output; unprocessed voxels remain zero. Skip ImageWindow creation.
- **Phase C:** Let it finish — single pass, fast. Not worth checking.
- **Check frequency:** Every substep boundary (after each frame in Phase A, after each batch in Phase B). Worst-case cancel latency is one frame load or one GPU batch.
- **Cleanup:** `ExecuteGlobal()` checks `is_cancelled()` after `execute()` returns. If cancelled, writes "Cancelled by user" to console, returns `false`. No partial images shown.

## Testing

- **Existing tests:** All 25+ tests unchanged — observer defaults to `NullProgressObserver`.
- **RecordingObserver:** Test helper that captures all calls into a `vector<Event>`. Used in `test_stacking_engine.cpp` to verify:
  - All phases open and close (no leaked `begin_phase` without `end_phase`)
  - Frame-level advance count matches input frame count
  - Cancellation mid-Phase-A produces partial result
  - Cancellation mid-Phase-B returns computed-so-far
- **No PI-layer tests:** `NukeXProgress` is a thin (~50 line) adapter. Testing it would require mocking PI internals. The recording observer proves correctness at the library layer.

## Files Created/Modified

| File | Action |
|------|--------|
| `src/lib/core/include/nukex/core/progress_observer.hpp` | **CREATE** — abstract interface + NullProgressObserver |
| `src/module/NukeXProgress.h` | **CREATE** — PI adapter header |
| `src/module/NukeXProgress.cpp` | **CREATE** — PI adapter implementation |
| `src/lib/stacker/include/nukex/stacker/stacking_engine.hpp` | **MODIFY** — add ProgressObserver* param |
| `src/lib/stacker/src/stacking_engine.cpp` | **MODIFY** — instrument all phases |
| `src/lib/gpu/include/nukex/gpu/gpu_executor.hpp` | **MODIFY** — add ProgressObserver* param |
| `src/lib/gpu/src/gpu_executor.cpp` | **MODIFY** — instrument batches + kernels |
| `src/module/NukeXInstance.cpp` | **MODIFY** — wire NukeXProgress into ExecuteGlobal() |
| `test/unit/stacker/test_stacking_engine.cpp` | **MODIFY** — add RecordingObserver tests |
| `CMakeLists.txt` | **MODIFY** — add new source files |
