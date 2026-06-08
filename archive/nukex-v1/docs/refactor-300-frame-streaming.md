# NukeX Stacker: 300+ Frame Streaming Refactor Plan

## Problem

The current stacker loads ALL frames into RAM simultaneously. With 300 frames of
4144x2822 pixels (32-bit float), that's **13+ GB** just for frame data. This exceeds
what most systems can handle alongside PixInsight, segmentation, and OS overhead.

Additionally, the `uint64_t outlierMask` bitmask caps outlier tracking at 64 frames.
Frames 65+ silently lose outlier information, degrading integration quality.

## Goals

- Stack 300+ frames without exceeding ~1 GB of frame memory
- Full outlier tracking for all frames (not just 64)
- No quality regression vs. current all-in-memory approach
- Preserve OpenMP parallelization
- Minimal API surface change

## Current Architecture

```
ExecuteGlobal()
  |
  +-- Load ALL frames into vector<Image> frames    [13 GB for 300 frames]
  |
  +-- RunIntegration(frames, keywords, output, summary)
        |
        +-- CreateMedianReference(frames)            [reads all frames per-pixel]
        |     -> Segmentation on reference image
        |
        +-- For each channel:
        |     ProcessStackWithMetadata(framePtrs, c, metadata)
        |       -> For each pixel (y, x):
        |            Read frames[0..N-1] at (x, y)
        |            SelectPixel(values, x, y)
        |              -> AnalyzePixelWithClass / AnalyzePixel
        |              -> SelectBestValue
        |
        +-- TransitionChecker::CheckAndSmooth()
```

**Peak memory**: All N frames + reference + segmentation + output + metadata

## Proposed Architecture

```
ExecuteGlobal()
  |
  +-- Validate all frames (dimensions, channels) without loading pixel data
  +-- Extract FITS keywords from all frames
  |
  +-- Phase 1: Build median reference (streaming)
  |     -> FrameStreamer reads one ROW from each frame at a time
  |     -> Compute per-pixel median row-by-row
  |     -> Write reference image (~45 MB)
  |
  +-- Run segmentation on reference image (unchanged)
  |
  +-- Phase 2: Pixel selection (streaming)
  |     -> FrameStreamer reads one ROW from each frame
  |     -> For each pixel in row: collect N values -> SelectPixel
  |     -> Write output row
  |     -> Discard row buffers
  |
  +-- TransitionChecker (unchanged, operates on output image)
```

**Peak memory**: N row buffers (~N * width * 4 bytes) + reference + output + segmentation

For 300 frames of width 4144: 300 * 4144 * 4 = **4.7 MB per channel** of row buffers.

## Detailed Design

### 1. FrameStreamer Class

New file: `src/engine/FrameStreamer.h` / `.cpp`

```cpp
class FrameStreamer
{
public:
   // Open all frame files, validate dimensions, extract keywords
   bool Initialize( const std::vector<String>& paths,
                    int& width, int& height, int& channels,
                    std::vector<FITSKeywordArray>& allKeywords );

   int NumFrames() const;
   int Width() const;
   int Height() const;
   int Channels() const;

   // Read a single row from all frames for a given channel
   // rowData[f][x] = pixel value from frame f at column x
   // Internally opens/seeks/reads/closes files as needed
   bool ReadRow( int y, int channel, std::vector<std::vector<float>>& rowData );

   // Read a single pixel position from all frames
   // values[f] = pixel value from frame f
   bool ReadPixel( int x, int y, int channel, std::vector<float>& values );

   // Get FITS keywords for a specific frame
   const FITSKeywordArray& GetKeywords( int frameIndex ) const;

   // Get frame file path
   const String& GetPath( int frameIndex ) const;

private:
   struct FrameInfo {
      String path;
      FITSKeywordArray keywords;
      // File handle kept open for sequential row reads
      std::unique_ptr<FileFormatInstance> file;
      bool isOpen = false;
   };

   std::vector<FrameInfo> m_frames;
   int m_width = 0, m_height = 0, m_channels = 0;

   // Row cache: keeps the last-read row per frame to avoid re-reads
   // when processing multiple channels of the same row
   int m_cachedRow = -1;
   int m_cachedChannel = -1;
   std::vector<std::vector<float>> m_rowCache;  // [numFrames][width]
};
```

**Key design decisions**:
- Row-based access (not pixel-based) for sequential disk I/O
- Row cache avoids re-reading when OpenMP threads access the same row
- Files opened lazily on first read, kept open for sequential access
- Memory: ~N * width * 4 bytes for row buffers = **4.7 MB** for 300 frames

**Alternative: Memory-mapped I/O**
For even better performance, use `mmap()` on each FITS file. The OS manages paging
automatically. However, this complicates FITS format parsing (headers, byte order).
Row-based file reads are simpler and sufficient.

### 2. Streaming Median Reference

Replace `CreateMedianReference(vector<Image>& frames)` with:

```cpp
Image CreateMedianReferenceStreaming( FrameStreamer& streamer )
{
   int width = streamer.Width();
   int height = streamer.Height();
   int channels = streamer.Channels();
   int numFrames = streamer.NumFrames();

   Image reference;
   reference.AllocateData( width, height, channels );

   std::vector<float> values( numFrames );

   for ( int c = 0; c < channels; ++c )
   {
      // Process row-by-row for sequential disk I/O
      for ( int y = 0; y < height; ++y )
      {
         std::vector<std::vector<float>> rowData;
         streamer.ReadRow( y, c, rowData );  // reads from all frames

         for ( int x = 0; x < width; ++x )
         {
            for ( int f = 0; f < numFrames; ++f )
               values[f] = rowData[f][x];

            std::nth_element( values.begin(),
                              values.begin() + numFrames / 2,
                              values.end() );
            reference.Pixel( x, y, c ) = values[numFrames / 2];
         }
      }
   }

   return reference;
}
```

**Memory**: One row from each frame = N * width * 4 bytes.
For 300 frames, 4144 width: **4.7 MB** (vs 13 GB current).

**Optimization**: For very large frame counts (>100), use a sampled median from
20-30 randomly selected frames. The segmentation quality is similar but 10x faster.
Make this configurable:
```cpp
int medianSampleSize = std::min( numFrames, 30 );  // sample for speed
```

### 3. Streaming Pixel Selection

Replace the current `ProcessStackWithMetadata` signature:

**Current**:
```cpp
Image ProcessStackWithMetadata(
   const std::vector<const Image*>& frames,
   int channel,
   std::vector<PixelSelectionResult>& metadata ) const;
```

**New** (add streaming overload):
```cpp
Image ProcessStackWithMetadata(
   FrameStreamer& streamer,
   int channel,
   std::vector<PixelSelectionResult>& metadata ) const;
```

Implementation:
```cpp
Image PixelSelector::ProcessStackWithMetadata(
   FrameStreamer& streamer,
   int channel,
   std::vector<PixelSelectionResult>& metadata ) const
{
   int width = streamer.Width();
   int height = streamer.Height();
   int numFrames = streamer.NumFrames();

   Image result( width, height, ColorSpace::Gray );
   result.Zero();

   size_t numPixels = static_cast<size_t>( width ) * height;
   metadata.resize( numPixels );

   Image::sample* outData = result.PixelData( 0 );

   // Process row-by-row (sequential I/O)
   for ( int y = 0; y < height; ++y )
   {
      // Read this row from all frames (sequential disk reads)
      std::vector<std::vector<float>> rowData;
      streamer.ReadRow( y, channel, rowData );

      // Now process pixels within this row in parallel
      #pragma omp parallel for schedule(static)
      for ( int x = 0; x < width; ++x )
      {
         // Collect values from the pre-read row data
         std::vector<float> pixelValues( numFrames );
         for ( int f = 0; f < numFrames; ++f )
            pixelValues[f] = rowData[f][x];

         PixelSelectionResult result_pixel = SelectPixel( pixelValues, x, y );

         size_t outIdx = static_cast<size_t>( y ) * width + x;
         outData[outIdx] = result_pixel.value;
         metadata[outIdx] = result_pixel;
      }
   }

   return result;
}
```

**Key change**: Outer loop is sequential (row-by-row I/O), inner loop is parallel
(pixel processing within a row). This preserves OpenMP parallelism while enabling
streaming. `schedule(static)` is better than `dynamic` here since all pixels in a
row have similar work.

**Memory per row**: N * width * 4 = 4.7 MB for 300 frames.

### 4. Dynamic Outlier Tracking

Replace `uint64_t outlierMask` with a dynamic container.

**Option A: `std::vector<bool>`** (1 bit per frame, arbitrary size)
- Pros: Exact same behavior, unlimited frames
- Cons: 300 bits = 38 bytes per pixel. For 4144x2822 image = 444 MB of outlier data
- This is too much per-pixel overhead

**Option B: Outlier count only** (don't track which frames, just how many)
- Pros: 2 bytes per pixel (uint16_t), minimal memory
- Cons: Lose per-frame detail
- For quality metrics, the count is usually sufficient

**Option C: Inline bitset with overflow** (recommended)
- Use `uint64_t` for first 64 frames (fast path, common case)
- Add `uint16_t outlierCount` for total count across all frames
- For frames 64+, only track the count, not per-frame identity

```cpp
struct PixelStackMetadata
{
   StackDistributionParams distribution;
   float selectedValue = 0.0f;
   uint16_t sourceFrame = 0;
   float confidence = 0.0f;
   uint64_t outlierMask = 0;       // Per-frame detail for first 64
   uint16_t outlierCount = 0;      // Total outlier count (all frames)
   uint16_t totalFrames = 0;       // How many frames were analyzed

   bool IsOutlier( int frameIndex ) const
   {
      return (frameIndex < 64) && ((outlierMask & (uint64_t( 1 ) << frameIndex)) != 0);
   }

   void SetOutlier( int frameIndex )
   {
      if ( frameIndex < 64 )
         outlierMask |= (uint64_t( 1 ) << frameIndex);
      outlierCount++;
   }
};
```

**Fix all `i < 64` caps**: Remove the `&& i < 64` from ALL 11 loop locations.
The loops should iterate `values.size()`:

```cpp
// BEFORE:
for ( size_t i = 0; i < values.size() && i < 64; ++i )

// AFTER:
for ( size_t i = 0; i < values.size(); ++i )
```

The `SetOutlier` method handles the overflow gracefully — it always increments
`outlierCount` even if `frameIndex >= 64`.

Similarly update `PixelSelectionResult`:
```cpp
struct PixelSelectionResult
{
   // ...existing fields...
   uint64_t outlierMask = 0;       // Per-frame detail for first 64
   uint16_t outlierCount = 0;      // Total outlier count
   uint16_t totalFrames = 0;       // Total frames analyzed
};
```

### 5. EXPTIME-Based Frame Weights (Streaming)

Frame weights from EXPTIME need all keywords available at selection time.
With streaming, keywords are extracted during `FrameStreamer::Initialize()` and
stored in `FrameInfo`. The weights are computed once before processing starts:

```cpp
// In RunIntegration, after FrameStreamer init:
std::vector<float> frameWeights = ComputeFrameWeights( streamer );
selector.SetFrameWeights( frameWeights );
```

No change needed — weights are already computed from keywords, not pixel data.

### 6. ExecuteGlobal Refactor

```cpp
bool NukeXStackInstance::ExecuteGlobal()
{
   Console console;
   console.EnableAbort();

   // Phase 0: Initialize streaming
   FrameStreamer streamer;
   std::vector<String> paths;
   for ( size_t i = 0; i < p_inputFrames.size(); ++i )
      if ( p_inputFrames[i].enabled )
         paths.push_back( p_inputFrames[i].path );

   int width, height, channels;
   std::vector<FITSKeywordArray> allKeywords;
   if ( !streamer.Initialize( paths, width, height, channels, allKeywords ) )
      return false;

   // Phase 1: Build median reference (streaming, ~5 MB memory)
   Image reference = CreateMedianReferenceStreaming( streamer );

   // Phase 2: Segmentation on reference (unchanged, ~90 MB)
   // ... existing segmentation code ...

   // Phase 3: Pixel selection (streaming, ~5 MB per row)
   PixelSelector selector( BuildSelectorConfig() );
   // ... setup segmentation, context, weights ...

   Image output;
   output.AllocateData( width, height, channels );

   for ( int c = 0; c < channels; ++c )
   {
      std::vector<PixelSelectionResult> metadata;
      Image channelResult = selector.ProcessStackWithMetadata( streamer, c, metadata );

      for ( int y = 0; y < height; ++y )
         for ( int x = 0; x < width; ++x )
            output.Pixel( x, y, c ) = channelResult.Pixel( x, y, 0 );
   }

   // Phase 4: Transition smoothing (unchanged)
   // ...
}
```

## File Change Summary

| File | Changes | Effort |
|------|---------|--------|
| **NEW** `src/engine/FrameStreamer.h` | FrameStreamer class declaration | Medium |
| **NEW** `src/engine/FrameStreamer.cpp` | Initialize, ReadRow, ReadPixel, row cache | High |
| `src/NukeXStackInstance.cpp` | Replace frame loading with FrameStreamer; streaming median; streaming selection | High |
| `src/engine/PixelSelector.h` | Add streaming ProcessStackWithMetadata overload; update outlierMask fields | Low |
| `src/engine/PixelSelector.cpp` | Implement streaming overload | Medium |
| `src/engine/PixelStackAnalyzer.h` | Add outlierCount/totalFrames; remove 64-cap from IsOutlier/SetOutlier | Low |
| `src/engine/PixelStackAnalyzer.cpp` | Remove all `i < 64` caps (11 locations); update IdentifyOutliers | Medium |
| `Makefile` | Add FrameStreamer.cpp | Trivial |

## Execution Phases

### Phase A: Outlier mask fix (independent, quick win)
- Remove all `i < 64` caps
- Add `outlierCount` / `totalFrames` to both metadata structs
- Update `SetOutlier()` to always increment count
- **No architectural change, just removes the 64-frame limitation**

### Phase B: FrameStreamer implementation
- New class with row-based reading
- Row cache for performance
- FITS keyword extraction at init time
- Unit test with mock frames

### Phase C: Streaming median reference
- Replace `CreateMedianReference` with `CreateMedianReferenceStreaming`
- Optional: sampled median for >100 frames

### Phase D: Streaming pixel selection
- Add streaming overload to `ProcessStackWithMetadata`
- Refactor `ExecuteGlobal` to use FrameStreamer
- Keep old in-memory overloads for backward compatibility

### Phase E: Testing and optimization
- Test with 10, 70, 150, 300 frames
- Profile I/O bottleneck (add progress reporting per row)
- Optional: async I/O with double-buffering (read next row while processing current)

## Memory Budget (300 frames, 4144x2822)

| Component | Memory |
|-----------|--------|
| FrameStreamer row buffers | 4.7 MB |
| Reference image | 44.6 MB |
| Segmentation map + confidence | 89.2 MB |
| Output image | 44.6 MB |
| Metadata (flat, per channel) | 30.6 MB |
| Working buffers (OMP threads) | ~10 MB |
| **Total peak** | **~224 MB** |

This is a **59x reduction** from the current 13.2 GB peak, fitting comfortably
on any system with 4+ GB RAM.

## Performance Considerations

**Disk I/O is the new bottleneck.** For 300 frames of 4144x2822:
- Total data: 13 GB
- NVME SSD sequential read: ~3 GB/s → ~4.3 seconds for one full pass
- Two passes needed (median + selection) → ~8.6 seconds of I/O
- Current in-memory processing: ~2-5 seconds for pixel selection

**Mitigation**:
- Row-based reads are sequential → maximizes SSD throughput
- Process one channel at a time → reduces seeks
- Double-buffer: read next row while processing current row
- For HDD users: warn about performance, suggest SSD

**Optimization**: If system has enough RAM (checked at runtime), load all frames
in memory as before. Only stream when frame count * frame size exceeds a threshold
(e.g., 8 GB):

```cpp
size_t totalFrameBytes = numFrames * width * height * sizeof(float) * channels;
bool useStreaming = (totalFrameBytes > 8ULL * 1024 * 1024 * 1024);  // 8 GB threshold
```

This gives the best of both worlds: fast in-memory processing for small stacks,
streaming for large stacks.

## Backward Compatibility

- Keep the old `ProcessStackWithMetadata(vector<const Image*>&, ...)` overload
- FrameStreamer is additive (new class, no existing API removed)
- `outlierCount`/`totalFrames` fields default to 0 — old code unaffected
- Old saved process instances work unchanged (no parameter changes)
