# Parallel Frame Loading Design

## Problem

Phase 1 frame loading is serial — each FITS/XISF file is opened, read, debayered, normalized, and metadata-extracted one at a time. For 100+ frame sessions this is the dominant wall-clock bottleneck before stacking begins.

## Solution

Parallelize the per-frame loading loop in `FrameLoader::LoadRaw()` using OpenMP with 4 threads. Serialize `FileFormat` construction, `FileFormatInstance::Open()`, keyword reads, and `Close()` (CFITSIO global handle table + PCL format registry safety), allowing the actual file reads, debayering, normalization, and metadata extraction to run concurrently.

## Scope

**Files to modify:**
- `src/engine/FrameLoader.cpp` — parallelize `LoadRaw()` loop, protect `ComputeFrameMetrics()` console output

**Not parallelized:** `Load()` writes directly to a column-major SubCube where adjacent frame indices share cache lines. Parallelizing it would cause severe false sharing (~10-50x write slowdown), negating any I/O gains. `LoadRaw()` is the primary pipeline entry point (called from NukeXStackInstance) and writes to separate per-frame heap vectors — no false sharing. `Load()` stays serial.

**No changes to:** SubCube, NukeXStackInstance, FrameAligner, or any other file.

## Design

### Thread Safety Analysis

Each iteration of the loading loop creates local objects that are not shared:
- `pcl::Image` — local buffer, no sharing
- `pcl::FITSKeywordArray` — local, no sharing

**Shared state requiring protection:**
| Resource | Risk | Mitigation |
|----------|------|-----------|
| CFITSIO global file handle table | Corrupted if two threads call `fits_open_file` or `fits_close_file` simultaneously | `#pragma omp critical(cfitsio)` around FileFormat ctor + Open + ReadKeywords + Close |
| PCL format registry | `pcl::FileFormat` constructor queries a global singleton registry | Wrap in same `critical(cfitsio)` section (microsecond cost, eliminates risk) |
| `pcl::Console` | Not thread-safe for concurrent writes | `#pragma omp critical(console)` around all WriteLn calls, including inside `ComputeFrameMetrics()` |
| `result.pixelData[i]` | None — each thread writes to its own slot | Pre-sized with `resize()` before parallel region |
| `result.metadata[i]` | None — each thread writes to its own slot | Pre-sized with `resize()` before parallel region |

### ComputeFrameMetrics() Console Safety

`ComputeFrameMetrics()` has internal `catch` blocks that call `pcl::Console().WarningLn()`. These must be protected. Strategy: wrap the `ComputeFrameMetrics()` call site in a try/catch within the parallel loop. If it throws or emits warnings, catch them and write via the `critical(console)` section. Alternatively, refactor `ComputeFrameMetrics()` to return warnings as strings instead of writing directly to Console — cleaner but slightly more invasive.

Recommended: refactor to return warnings via an output parameter, keeping Console writes centralized in the loading loop.

### Error Handling

OpenMP does not propagate exceptions across threads. Strategy:
- Each thread catches exceptions and stores the error message (including frame path) in a `std::vector<std::string> errors(N)` array (one slot per frame, pre-allocated)
- After the parallel region, check if any errors occurred
- If so, throw the first error (matching the current serial behavior)

### Structure of LoadRaw() After Change

```
1. Filter enabled frames, open first frame for reference dims (serial, unchanged)
2. Detect Bayer pattern from first frame (serial, unchanged)
3. Pre-allocate result.pixelData and result.metadata (serial, unchanged)
4. Pre-allocate errors vector

5. #pragma omp parallel for num_threads(4) schedule(dynamic)
   for each frame i:
     a. Console output            → #pragma omp critical(console)
     b. FileFormat + Open()       → #pragma omp critical(cfitsio)
        + ReadFITSKeywords()        (inside critical — same handle, fast)
     c. ReadImage(img)            → PARALLEL (own handle, the slow part)
     d. Close()                   → #pragma omp critical(cfitsio)
     e. Normalize()               → PARALLEL (local image)
     f. Debayer (if needed)       → PARALLEL (local data)
     g. ComputeFrameMetrics()     → PARALLEL (refactored: no Console writes)
     h. Console warnings (if any) → #pragma omp critical(console)
     i. Write to pixelData[i]     → PARALLEL (own slot)
     j. Write to metadata[i]      → PARALLEL (own slot)
     k. On exception: store in errors[i], continue

6. Check errors — throw first if any (serial)
7. Final console summary (serial)
```

### Console Output Ordering

With parallel loading, frame log lines may appear out of order (e.g., `[3/100]` before `[2/100]`). This is acceptable — WBPP and FastIntegration have the same behavior. The summary line at the end confirms the total count.

### Thread Count

Default: 4 threads. Rationale:
- Matches SSD queue depth sweet spot (4-8 concurrent reads)
- NAS benefits from overlapping network latency (even 2-4 helps)
- Low enough to avoid memory pressure (4 full-frame images in flight)
- Does not need to be configurable in the UI — hardcoded constant

### Keyword Read Placement

`ReadFITSKeywords()` is fast (reads header, not pixel data) and operates on the same `FileFormatInstance` handle. Keep it inside the critical section with Open to avoid a second critical section boundary. The overhead is negligible since headers are small.

## Testing

- Existing tests continue to pass (no behavioral change, just speed)
- Manual PI test: load a 100+ frame session and verify the console output shows all frames loaded correctly, stacking produces identical results
- No new unit tests needed — this is a performance optimization with no logic change

## Not In Scope

- Parallelizing `Load()` — false sharing on column-major SubCube makes it counterproductive
- Configurable thread count UI parameter (YAGNI)
- Parallel alignment (Phase 1b) — separate concern, different bottleneck
- Parallel flat loading — same pattern, can apply later if needed
