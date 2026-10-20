#pragma once
#include <cstddef>
#include <string>
#include <vector>

#include "SerFile.h"

namespace planetary::ser {

// Interleaved float image, samples normally in [0,1].
struct FloatImage {
    int width = 0, height = 0, channels = 0;
    std::vector<float> data;
};

// Divides every sample by 2^depth - 1. Throws SerError for a depth outside 1..16.
FloatImage Normalize(const Frame& frame, int depth);

// Bilinear demosaic of a 1-channel CFA frame into normalised 3-channel RGB.
// Edges are mirrored (reflection that preserves Bayer parity). Throws SerError
// for a non-Bayer pattern, a depth outside 1..16, a non-1-channel frame, or a
// frame under 2x2.
FloatImage DebayerBilinear(const Frame& cfa, ColorId pattern, int depth);

// The same results written into caller-owned storage: pixel i of channel c goes
// to planes[c][i * stride]. Separate plane buffers with stride 1 are planar (a
// PixInsight image); {data, data + 1, data + 2} with stride 3 is interleaved.
// NormalizeInto needs frame.channels planes, DebayerBilinearInto three.
void NormalizeInto(const Frame& frame, int depth, float* const* planes, size_t stride);
void DebayerBilinearInto(const Frame& cfa, ColorId pattern, int depth, float* const* planes, size_t stride);

// "" when every sample of frame `index` fits in normDepth bits, else
// "frame <index> samples exceed the <normDepth>-bit range; values above 1.0".
std::string FrameRangeWarning(const Frame& frame, int64_t index, int normDepth);

}  // namespace planetary::ser
