// src/engine/cuda/CudaRemediation.h
// GPU remediation kernels for post-stack artifact correction.
// Three kernels: trail re-selection, dust neighbor-ratio, vignetting multiply.
// All functions return false on CUDA error to trigger CPU fallback.
#pragma once

#ifdef NUKEX_HAS_CUDA

#include <cstdint>
#include <cstddef>
#include <vector>

namespace nukex {
namespace cuda {

struct TrailPixel { int x, y; };

// Trail re-selection: for each trail pixel, read the Z-column from the subcube,
// compute median and MAD, exclude frames where value > median + sigma * MAD,
// and return the median of the remaining clean values.
// cubeData: column-major float subcube (nSubs x H x W), Z-columns contiguous
// trailPixels: compact list of (x,y) coordinates of trail-contaminated pixels
// trailOutlierSigma: rejection threshold in MAD units
// outputPixels: receives corrected values (one per trail pixel)
// Returns true on GPU success, false to fall back to CPU.
bool remediateTrailsGPU(
   const float* cubeData,
   size_t nSubs, size_t height, size_t width,
   const std::vector<TrailPixel>& trailPixels,
   double trailOutlierSigma,
   float* outputPixels );

// Dust correction: for each dust pixel, compute mean of clean (non-dust,
// non-zero) neighbors within neighborRadius, then multiply the pixel by
// clamp(neighborMean / pixelValue, 1.0, maxRatio).
// channelResult: row-major float (H x W) selected values from Phase 3
// dustMask: row-major uint8 (H x W), 1 = dust pixel
// correctedOutput: receives the corrected image
// Returns true on GPU success, false to fall back to CPU.
bool remediateDustGPU(
   const float* channelResult,
   int width, int height,
   const uint8_t* dustMask,
   int neighborRadius,
   float maxRatio,
   float* correctedOutput );

// Vignetting correction: multiply each pixel by its correction factor.
// channelResult: row-major float (H x W) input image
// correctionMap: row-major float (H x W) multiplicative factors
// correctedOutput: receives channelResult[i] * correctionMap[i]
// Returns true on GPU success, false to fall back to CPU.
bool remediateVignettingGPU(
   const float* channelResult,
   const float* correctionMap,
   int width, int height,
   float* correctedOutput );

} // namespace cuda
} // namespace nukex

#endif // NUKEX_HAS_CUDA
