#pragma once
#include <cstddef>
#include <string>

#include "FrameSource.h"
#include "SerFile.h"

// Small helpers shared by the bilinear debayer (Debayer.cpp) and the stack engine's
// demosaic (src/stack): CFA site lookup, depth scale, frame-size check, edge mirroring.
namespace planetary::ser {

// Channel (0=R, 1=G, 2=B) at site (x,y), indexed by (y&1)*2 + (x&1).
// Throws SerError for a non-Bayer pattern.
inline const int* PatternTable(ColorId p) {
    static const int rggb[4] = {0, 1, 1, 2}, grbg[4] = {1, 0, 2, 1},
                     gbrg[4] = {1, 2, 0, 1}, bggr[4] = {2, 1, 1, 0};
    switch (p) {
        case ColorId::BayerRGGB: return rggb;
        case ColorId::BayerGRBG: return grbg;
        case ColorId::BayerGBRG: return gbrg;
        case ColorId::BayerBGGR: return bggr;
        default: throw SerError(std::string(ColorIdName(p)) + " is not a Bayer pattern");
    }
}

// 2^depth - 1: samples are LSB-aligned in 1..16 bits; anything else has no such scale.
// Throws SerError for a depth outside 1..16.
inline float MaxSampleValue(int depth) {
    if (depth < 1 || depth > 16)
        throw SerError("bit depth " + std::to_string(depth) + " is outside 1..16");
    return static_cast<float>((1u << depth) - 1u);
}

// Throws SerError unless the frame has positive dimensions and exactly w*h*channels samples.
inline void CheckFrameSize(const Frame& f) {
    if (f.width <= 0 || f.height <= 0 || f.channels <= 0 ||
        f.samples.size() != static_cast<size_t>(f.width) * f.height * f.channels)
        throw SerError("frame holds " + std::to_string(f.samples.size()) + " samples, expected " +
                       std::to_string(f.width) + "x" + std::to_string(f.height) + "x" +
                       std::to_string(f.channels));
}

// Reflection about the edge sample (-1 -> 1, n -> n-2), repeated until in range: keeps the
// Bayer site parity. A 1-sample axis maps everything to 0 (there is no parity to keep).
inline int MirrorIndex(int i, int n) {
    if (n <= 1) return 0;
    while (i < 0 || i >= n) {
        if (i < 0) i = -i;
        if (i >= n) i = 2 * (n - 1) - i;
    }
    return i;
}

}  // namespace planetary::ser
