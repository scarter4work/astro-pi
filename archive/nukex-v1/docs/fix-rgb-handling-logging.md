# Fix RGB Handling & Add Stacking Logging (v1.1.0.31)

## Problem

NukeXStack loaded 75 RGB frames as mono (3840x2160x1 instead of x3), producing garbage output. Three issues identified:

1. **CFA/Bayer FITS files** (BAYERPAT=RGGB, NAXIS=2, 1 channel) from color cameras are technically mono — the Bayer mosaic pattern encodes color spatially, not as separate channels. The stacker gives no indication that debayering is needed.

2. **Multi-extension FITS** (compressed FITS with multiple HDUs) are not handled — the code always uses `images[0]` which may be an empty primary HDU. PCL's `SelectImage()` API exists but is never called.

3. **Missing ColorSpace** on output `AllocateData` calls in RunIntegration and RunIntegrationStreaming.

4. **Logging is too noisy** (75x "FITS keywords extracted" spam) and missing critical diagnostic info (channel count, CFA detection, file format).

## Changes

### 1. FrameStreamer (`.h` and `.cpp`)

- Add `m_isMultiExtension` flag and `hduIndex` to FrameInfo struct
- In `Initialize()`: detect multi-extension FITS (multiple HDUs with valid image data), set `m_channels` accordingly, check for BAYERPAT keyword
- In `ReadRow()`: call `SelectImage(channel)` for multi-extension files before `ReadSamples()`
- Log first-frame format summary instead of per-frame keyword counts

### 2. NukeXStackInstance (`.cpp`)

- In `LoadFrame()`: handle multi-extension FITS by finding valid HDUs, calling `SelectImage()`, reading each extension as a separate channel
- In `ExecuteGlobal()`: add format summary logging, consolidate frame loading progress (every 10% instead of per-frame), warn about CFA/Bayer data
- Fix `output.AllocateData` to include proper ColorSpace (lines ~878 and ~1058)

### 3. Logging improvements

**Remove/consolidate:**
- Per-frame "N FITS keywords extracted" during FrameStreamer init
- Per-frame 3-line load messages during in-memory loading

**Add:**
- One-time format detection summary: dimensions, channels, color type, bit depth
- CFA/Bayer detection warning with recommended action
- Multi-extension FITS detection with HDU count
- Consolidated frame loading progress
- Post-load memory usage summary

## Files Modified

- `src/engine/FrameStreamer.h`
- `src/engine/FrameStreamer.cpp`
- `src/NukeXStackInstance.cpp`
- `src/NukeXModule.cpp` (version bump)

## Verification

1. Build and sign
2. Test with CFA frames → should see clear BAYERPAT warning
3. Test with debayered RGB frames → should detect 3 channels
4. Check console output: no keyword spam, clear progress, format summary
