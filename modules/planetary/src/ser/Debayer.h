#pragma once
#include <vector>

#include "SerFile.h"

namespace planetary::ser {

// Interleaved float image, samples normally in [0,1].
struct FloatImage {
    int width = 0, height = 0, channels = 0;
    std::vector<float> data;
};

// Divides every sample by 2^depth - 1.
FloatImage Normalize(const Frame& frame, int depth);

// Bilinear demosaic of a 1-channel CFA frame into normalised 3-channel RGB.
// Edges are mirrored (reflection that preserves Bayer parity). Throws SerError
// for a non-Bayer pattern, a non-1-channel frame, or a frame under 2x2.
FloatImage DebayerBilinear(const Frame& cfa, ColorId pattern, int depth);

}  // namespace planetary::ser
