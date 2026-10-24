#pragma once
#include <vector>

#include "Debayer.h"
#include "FrameSource.h"

namespace planetary::stack {

// Fast luminance for per-frame measurement, normalised by 2^normDepth - 1:
//  - Bayer (1-channel CFA): half resolution, each output pixel is the mean of one 2x2
//    CFA cell (R+G+G+B)/4, so W/2 x H/2 (a trailing odd row/column is dropped);
//  - 1-channel non-Bayer: the frame as is;
//  - 3-channel (RGB, or BGR already reordered by the reader): mean of the channels.
// Throws ser::SerError for a depth outside 1..16, a sample count that does not match the
// frame size, a Bayer pattern on a non-1-channel frame, a Bayer frame under 2x2, or a
// channel count other than 1 or 3.
ser::FloatImage LumaForMeasure(const ser::Frame& frame, ser::ColorId color, int normDepth);

// Final-quality colour image, normalised by 2^normDepth - 1.
//  - Bayer: Hamilton-Adams edge-directed demosaic, interleaved RGB output. Green is
//    interpolated along the direction of the smaller gradient, with a Laplacian correction
//    from the site's own channel, held inside the range of the four green neighbours. Red
//    and blue are rebuilt from colour differences (R-G, B-G), along the smoother diagonal
//    at red/blue sites. Exact on any flat colour field; edges are mirrored with Bayer
//    parity kept, so borders included.
//    Range: every output sample is in [0, M], M = the frame's largest sample / (2^normDepth - 1).
//    M <= 1 for in-range data; samples above 2^normDepth - 1 keep their value above 1.
//  - Mono / RGB: Normalize, unchanged layout (no clamping).
// Throws ser::SerError for the cases LumaForMeasure lists (Bayer frames under 2x2 included).
ser::FloatImage DemosaicHQ(const ser::Frame& frame, ser::ColorId color, int normDepth);

// DemosaicHQ's working planes (Bayer: the padded CFA and green planes, 8 bytes per pixel), kept by a caller that
// demosaics frame after frame (RunStack's stacking workers, one each).
struct DemosaicScratch {
    std::vector<float> cfa, green;
};
// DemosaicHQ into `out`, the same samples. `out` and `scratch` are resized, never cleared (every sample is written), so
// buffers reused for frames of one size are neither allocated nor zero-filled again (re-review 3, M4). One thread at a
// time per `out` and `scratch`.
void DemosaicHQ(const ser::Frame& frame, ser::ColorId color, int normDepth, ser::FloatImage& out,
                DemosaicScratch& scratch);

}  // namespace planetary::stack
