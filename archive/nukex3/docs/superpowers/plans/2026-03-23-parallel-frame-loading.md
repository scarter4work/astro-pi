# Parallel Frame Loading Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Parallelize `FrameLoader::LoadRaw()` with 4 OpenMP threads so file reads overlap, reducing Phase 1 wall-clock time for 100+ frame sessions.

**Architecture:** Add `#pragma omp parallel for` around the per-frame loading loop. Serialize `FileFormat` construction, `Open()`, keyword reads, and `Close()` with a `critical(cfitsio)` section. Console output via `critical(console)`. Refactor `ComputeFrameMetrics()` to return warnings via output parameter instead of writing to Console directly.

**Tech Stack:** C++17, OpenMP (already linked), PCL FileFormatInstance, Catch2 v3

---

## File Map

| Action | File | Responsibility |
|--------|------|---------------|
| Modify | `src/engine/FrameLoader.cpp:144-333` | Parallelize `LoadRaw()` loop, refactor `ComputeFrameMetrics()` |
| Modify | `src/engine/FrameLoader.h:80` | Update `ComputeFrameMetrics()` signature (add warning output param) |

---

### Task 1: Refactor ComputeFrameMetrics to Not Use Console

**Files:**
- Modify: `src/engine/FrameLoader.h:80`
- Modify: `src/engine/FrameLoader.cpp:582-641`

The current `ComputeFrameMetrics()` writes warnings directly to `pcl::Console` in its catch blocks. This is not thread-safe. Refactor it to return warnings via an output parameter so the caller can write them under a critical section.

- [ ] **Step 1: Update signature in FrameLoader.h**

Change line 80 from:
```cpp
    static void ComputeFrameMetrics( const pcl::Image& img, SubMetadata& meta );
```
To:
```cpp
    static void ComputeFrameMetrics( const pcl::Image& img, SubMetadata& meta,
                                      std::string* warningOut = nullptr );
```

Add `#include <string>` to the includes if not already present.

- [ ] **Step 2: Update implementation in FrameLoader.cpp**

In `ComputeFrameMetrics()` (lines 582-641), replace the two catch blocks that write to Console:

Replace lines 631-640:
```cpp
    catch ( const pcl::Error& e )
    {
        pcl::Console().WarningLn( "FrameLoader: PSF metrics failed: " + e.Message()
            + " -- frame will use default quality score" );
    }
    catch ( const std::exception& e )
    {
        pcl::Console().WarningLn( pcl::String( "FrameLoader: PSF metrics failed: " )
            + e.what() + " -- frame will use default quality score" );
    }
```

With:
```cpp
    catch ( const pcl::Error& e )
    {
        if ( warningOut )
            *warningOut = "FrameLoader: PSF metrics failed: "
                + pcl::IsoString( e.Message() ).c_str()
                + std::string( " -- frame will use default quality score" );
    }
    catch ( const std::exception& e )
    {
        if ( warningOut )
            *warningOut = std::string( "FrameLoader: PSF metrics failed: " )
                + e.what() + " -- frame will use default quality score";
    }
```

- [ ] **Step 3: Build and test**

```bash
cd /home/scarter4work/projects/nukex3/build && cmake .. && make -j$(nproc) && ctest --output-on-failure
```

Expected: All 19 tests pass. Behavioral change: warnings go to the caller instead of directly to Console. The two call sites in `LoadRaw()` (lines 309 and 323) still pass `nullptr` for now, so warnings are silently dropped — acceptable temporarily since they're rare (only when FITS headers lack FWHM and star detection also fails).

- [ ] **Step 4: Commit**

```bash
git add src/engine/FrameLoader.h src/engine/FrameLoader.cpp
git commit -m "refactor: ComputeFrameMetrics returns warnings via output param (thread-safety prep)"
```

---

### Task 2: Parallelize LoadRaw() Loop

**Files:**
- Modify: `src/engine/FrameLoader.cpp:228-325` (the loading loop)

This is the core change. Replace the serial `for` loop with an OpenMP parallel for.

- [ ] **Step 1: Add includes**

At the top of `FrameLoader.cpp`, add after the existing includes:
```cpp
#include <omp.h>
#include <string>
```

- [ ] **Step 2: Add error and warning vectors before the loop**

After line 226 (`result.metadata.resize( enabled.size() );`), add:

```cpp
    // Parallel loading: pre-allocate per-frame error/warning slots
    const int LOAD_THREADS = 4;
    size_t N = enabled.size();
    std::vector<std::string> errors( N );
    std::vector<std::string> warnings( N );
```

- [ ] **Step 3: Replace the loading loop**

Replace lines 228-325 (the entire `for ( size_t i = 0; ...)` loop). Insert the replacement BEFORE the existing console summary at line 327.

The critical design: `Open` + `ReadKeywords` + `Close` are serialized (CFITSIO global handle table). `ReadImage` runs **outside** the critical section — each thread's `FileFormatInstance` has its own file handle, so concurrent reads on different handles are safe.

```cpp
    // 5. Load each enabled frame (parallel — file reads overlap)
    #pragma omp parallel for num_threads(LOAD_THREADS) schedule(dynamic)
    for ( size_t i = 0; i < N; ++i )
    {
        try
        {
            const pcl::String& path = enabled[i]->path;

            #pragma omp critical(console)
            {
                console.WriteLn( pcl::String().Format(
                    "  [%d/%d] %s",
                    int( i + 1 ), int( N ),
                    pcl::IsoString( pcl::File::ExtractNameAndExtension( path ) ).c_str() ) );
            }

            // Create format + instance as thread-local variables (persist for ReadImage)
            pcl::String ext = pcl::File::ExtractExtension( path ).Lowercase();
            pcl::FileFormat format( ext, true/*read*/, false/*write*/ );
            pcl::FileFormatInstance file( format );
            pcl::ImageDescriptionArray images;
            pcl::FITSKeywordArray keywords;
            int w = 0, h = 0;
            bool canStoreKW = format.CanStoreKeywords();

            // Serialize: Open + keyword read (CFITSIO global handle table safety)
            #pragma omp critical(cfitsio)
            {
                if ( !file.Open( images, path ) )
                    throw pcl::Error( "FrameLoader: failed to open: " + path );

                if ( images.IsEmpty() )
                {
                    file.Close();
                    throw pcl::Error( "FrameLoader: no image data in: " + path );
                }

                w = images[0].info.width;
                h = images[0].info.height;

                if ( canStoreKW )
                    file.ReadFITSKeywords( keywords );
            }

            // Validate dimensions match reference
            if ( w != refWidth || h != refHeight )
            {
                #pragma omp critical(cfitsio)
                { file.Close(); }
                throw pcl::Error( pcl::String().Format(
                    "FrameLoader: dimension mismatch in frame %d — expected %dx%d, got %dx%d: ",
                    int( i + 1 ), refWidth, refHeight, w, h ) + path );
            }

            // PARALLEL: Read the image (each thread has its own file handle)
            pcl::Image img;
            if ( !file.ReadImage( img ) )
            {
                #pragma omp critical(cfitsio)
                { file.Close(); }
                throw pcl::Error( "FrameLoader: failed to read image data: " + path );
            }

            // Serialize: Close (CFITSIO global handle table safety)
            #pragma omp critical(cfitsio)
            { file.Close(); }

            // === Everything below runs in PARALLEL (no shared state) ===

            img.Normalize();

            size_t numPx = size_t( refWidth ) * size_t( refHeight );

            if ( needsDebayer )
            {
                const pcl::Image::sample* cfa = img.PixelData( 0 );
                result.pixelData[i].resize( 3 );
                DebayerBilinear( cfa, refWidth, refHeight, bayerPattern,
                                 result.pixelData[i][0],
                                 result.pixelData[i][1],
                                 result.pixelData[i][2] );

                pcl::Image rgbImg;
                rgbImg.AllocateData( refWidth, refHeight, 3, pcl::ColorSpace::RGB );
                std::copy( result.pixelData[i][0].begin(), result.pixelData[i][0].end(),
                           rgbImg.PixelData( 0 ) );
                std::copy( result.pixelData[i][1].begin(), result.pixelData[i][1].end(),
                           rgbImg.PixelData( 1 ) );
                std::copy( result.pixelData[i][2].begin(), result.pixelData[i][2].end(),
                           rgbImg.PixelData( 2 ) );

                result.metadata[i] = ExtractMetadata( keywords );
                if ( result.metadata[i].fwhm == 0.0 && result.metadata[i].eccentricity == 0.0 )
                    ComputeFrameMetrics( rgbImg, result.metadata[i], &warnings[i] );
            }
            else
            {
                result.pixelData[i].resize( outChannels );
                for ( int c = 0; c < outChannels; ++c )
                {
                    const pcl::Image::sample* src = img.PixelData( c );
                    result.pixelData[i][c].assign( src, src + numPx );
                }

                result.metadata[i] = ExtractMetadata( keywords );
                if ( result.metadata[i].fwhm == 0.0 && result.metadata[i].eccentricity == 0.0 )
                    ComputeFrameMetrics( img, result.metadata[i], &warnings[i] );
            }
        }
        catch ( const pcl::Error& e )
        {
            try { errors[i] = pcl::IsoString( e.Message() ).c_str(); }
            catch ( ... ) { errors[i] = "FrameLoader: unknown error in frame " + std::to_string( i + 1 ); }
        }
        catch ( const std::exception& e )
        {
            errors[i] = e.what();
        }
        catch ( ... )
        {
            errors[i] = "FrameLoader: unknown error in frame " + std::to_string( i + 1 );
        }
    }

    // 6. Emit any warnings from parallel region
    for ( size_t i = 0; i < N; ++i )
    {
        if ( !warnings[i].empty() )
            console.WarningLn( pcl::String( warnings[i].c_str() ) );
    }

    // 7. Check for errors — throw the first one
    for ( size_t i = 0; i < N; ++i )
    {
        if ( !errors[i].empty() )
            throw pcl::Error( pcl::String( errors[i].c_str() ) );
    }
```

**Key design — parallel file reads:**
- `FileFormat` + `FileFormatInstance` are thread-local variables (created per-iteration, outside critical)
- `critical(cfitsio)` wraps only `Open` + `ReadKeywords` and `Close` — protects the CFITSIO global handle table
- `ReadImage()` runs **in parallel** — each thread reads from its own file handle. This is the core speedup: multiple file reads overlap on SSD/NAS.
- Post-read CPU work (Normalize, Debayer, PSF fitting, pixel copy) also runs in parallel
- Exception catch blocks use nested try/catch for the `pcl::IsoString` conversion to avoid UB in OpenMP

- [ ] **Step 3: Build and test**

```bash
cd /home/scarter4work/projects/nukex3/build && cmake .. && make -j$(nproc) && ctest --output-on-failure
```

Expected: All 19 tests pass.

- [ ] **Step 4: Commit**

```bash
git add src/engine/FrameLoader.cpp
git commit -m "perf: parallelize LoadRaw() with 4 OpenMP threads

Serialize FileFormat/Open/Read/Close (CFITSIO safety) while running
normalize, debayer, and PSF metrics in parallel across frames."
```

---

### Task 3: Verify and Clean Up

- [ ] **Step 1: Full rebuild and test**

```bash
cd /home/scarter4work/projects/nukex3/build && cmake .. && make -j$(nproc) && ctest --output-on-failure
```

Expected: All 19 tests pass.

- [ ] **Step 2: Check for warnings**

```bash
cd /home/scarter4work/projects/nukex3/build && make -j$(nproc) 2>&1 | grep -i warning | head -10
```

Expected: No new warnings.

- [ ] **Step 3: Commit any cleanup**

If any issues found:
```bash
git add -u && git commit -m "fix: cleanup from parallel loading implementation"
```
