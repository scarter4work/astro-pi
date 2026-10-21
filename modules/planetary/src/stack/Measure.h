#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "Debayer.h"
#include "FrameSource.h"
#include "StackTypes.h"

namespace planetary::stack {

// Per-frame measurement (spec "The engine", step 1). Every coordinate is in the pixels of the
// luma image that was measured: for Bayer input that is LumaForMeasure's half-resolution image
// (one luma pixel per 2x2 CFA cell), for mono/RGB it is the frame itself.
struct FrameStats {
    int64_t index = 0;
    // A target stands clearly above the background (rule below). When false: cx = cy = 0, the
    // box is empty (all 0), quality = 0, brightness = the frame's mean luma and clippedFraction
    // is over the whole frame (so a white-out frame still reports how much of it is clipped).
    bool targetFound = false;
    // Centroid of the target: luma above half its contrast, weighted by the excess.
    double cx = 0, cy = 0;
    // The target's extent, half-open [bx0, bx1) x [by0, by1), conservative: the blocks of the
    // detection grid (see below) where the target exceeds 10% of its contrast, grown by one block,
    // clipped to the image. A target cut by -- or within one block of -- the image edge has its box
    // touching that edge (bx0 == 0, by0 == 0, bx1 == width or by1 == height). A target that fills
    // the frame (a lunar surface) touches all four edges.
    int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    // Sharpness, in 1/pixel; only frames of the same capture are meant to be compared.
    double quality = 0;
    // Mean luma of the target pixels (in the box, above 10% of the contrast), in [0, 1] units.
    double brightness = 0;
    // Fraction of the target pixels with luma >= kClippedLuma.
    double clippedFraction = 0;
};

// Luma at or above this is saturated: 254/255 is not, a 12- or 10-bit ADC scaled into 16 bits
// (65520/65535, 65472/65535) is, and a Bayer cell counts only when all four sites are.
constexpr double kClippedLuma = 0.999;

// Measures one luma image (1 channel, normalised to [0, 1] as LumaForMeasure makes it).
// Throws std::invalid_argument for an empty image, a channel count other than 1, or a sample
// count that does not match width * height. Deterministic: the same image gives the same stats.
//
// Background and noise. The image is despiked (each pixel held to the largest of its 4 neighbours,
// so a hot pixel cannot make a target) and block-averaged on a detection grid of d x d pixels,
// d = ceil(longest side / 256) (1080p mono: d = 8). The background b is the 20th percentile of
// the block means (the sky, as long as the target covers less than ~80% of the frame). The block
// noise s_b is the MAD of neighbouring block differences / sqrt(2) (blind to smooth gradients
// and vignetting), floored at s / d. The pixel noise s is from the 20th percentile, over tiles
// of up to 32 x 32 pixels, of the mean squared neighbour difference / 2 -- the smoothest tiles,
// i.e. sky where there is any, the flattest maria on a full-frame Moon -- so texture does not
// inflate it and quantised 8-bit data still gets its true (quantisation included) noise.
//
// Target rule. Blocks brighter than b + 4 s_b form 8-connected components. A component is a
// target candidate when its brightest block stands at least max(10 s_b, 0.01) above b and it
// covers at least 9 pixels; the candidate with the most flux (sum of block excess) is the target.
// 10 s_b: white noise peaks near 4-5 s_b over a 1080p grid, and the 2026-01-30 Saturn AVI (a
// 5-6 DN pedestal and nothing else) peaks at 5.2-6.7 s_b, 0.0050-0.0064 of full scale (measured
// 2026-10-21), so a real target has to stand well clear of both. 0.01 of full scale (2.5 DN at
// 8 bits): a smooth amp glow or gradient can reach many s_b yet stays that faint, while a planet
// capture is exposed to tens of percent (the same night's Neptune, the faintest target on the NAS:
// 0.74, 24 s_b). 9 pixels: a lone noise spike is never a target. The target's extent is then
// regrown from its brightest block over blocks above b + max(4 s_b, 10% of the contrast), which
// keeps a faint halo or a nearby moon out of the box.
//
// Quality. Over the box plus a 10% margin, with forward differences:
//     quality^2 = (mean(dx^2) + mean(dy^2) - 4 s^2) / (mean((luma - b)^2) - s^2)
// i.e. the target's RMS spatial frequency: gradient energy over signal energy. Why this one:
//  - brightness: numerator and denominator both scale with brightness^2, so a frame dimmed by
//    haze measures the same detail (no brightness term survives);
//  - blur: for any blur that multiplies the spectrum by exp(-sigma^2 w^2), this power-weighted mean
//    of 4 sin^2(w/2) (increasing in |w|) strictly falls as sigma grows -- so sharper is always
//    higher, with no tuning. No smoothing before the differences: a smoothing kernel would turn
//    the frequency weight non-monotone near Nyquist;
//  - noise: white noise adds 4 s^2 to the gradient term and s^2 to the signal term, removed with
//    the measured s, so a noisy (dim) frame does not look "detailed". Photon noise on the target
//    itself is not removed; it is near-equal across the frames of one capture. Where the sky is
//    clipped to a constant black level (common in 8-bit captures), s measures 0 and nothing is
//    removed: ranking within the capture still holds, but haze-dimmed frames read slightly sharp.
//  - box size: background pixels add ~0 to both sums after the noise terms, so how tight the box
//    is does not matter.
FrameStats MeasureFrame(const ser::FloatImage& luma, int64_t index);

// Opens one reader. Called once per worker thread, from that thread -- so concurrently.
using SourceFactory = std::function<std::unique_ptr<ser::FrameSource>()>;

// Measures frames 0 .. info.frameCount - 1: each frame is read, turned into luma with
// LumaForMeasure(frame, info.colorId, info.normDepth) and measured. `threads` workers (fewer when
// there are fewer frames; < 1 counts as 1) each open their own reader through `factory` and take
// runs of consecutive frames, so every reader reads forward. Result: one FrameStats per frame,
// in frame order, identical for any thread count.
// Progress phase "Measuring frames" (done, frameCount); report() and cancelled() are only called
// from the calling thread. Cancel: workers stop, every thread is joined, StackCancelled is thrown.
// A worker's exception (a read error, a factory that throws or returns null, a reader whose size
// or frame count differs from `info`) is rethrown here after every thread is joined.
std::vector<FrameStats> MeasureAll(const SourceFactory& factory, const ser::FileInfo& info, int threads,
                                   const Progress& progress);

}  // namespace planetary::stack
