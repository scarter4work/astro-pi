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
    // targetFound -- never quality -- says whether there is a target: quality is 0 both for
    // "no target" and for a found target with no measurable detail (gradient energy at or below
    // the noise floor), so a consumer must test targetFound first.
    double quality = 0;
    // 1-sigma uncertainty of `quality` from the pixel noise s, propagated per frame (derivation at
    // MeasureFrame). 0 when s measures 0 -- e.g. a sky clipped to a constant black level in 8-bit
    // data, where the target's own noise is real but not measurable this way -- and 0 whenever
    // quality is 0.
    double qualitySigma = 0;
    // Mean luma of the target pixels (in the box, above 10% of the contrast), in [0, 1] units.
    double brightness = 0;
    // Fraction of the target pixels with luma >= kClippedLuma.
    double clippedFraction = 0;
};

// Measurement-luma pixels per frame pixel, inverted: a frame pixel is LumaScale(info) times smaller
// than a luma pixel. 2 for Bayer input (LumaForMeasure sums each 2x2 CFA cell, so the luma image is
// width / 2 x height / 2, integer division), 1 for mono and RGB. FrameStats coordinates times this
// are frame (sensor) pixels; the luma image's size is header.width / LumaScale (integer part).
// The one place this factor is defined (Ruling P3): consumers never scale by 2 on their own.
double LumaScale(const ser::FileInfo& info);

// Side d of the detection grid's square blocks for a luma image of width x height:
// d = ceil(longest side / 256), at least 1 (see "Background and noise" below).
int DetectionBlock(int width, int height);

// Luma at or above this is saturated: 254/255 is not, a 12- or 10-bit ADC scaled into 16 bits
// (65520/65535, 65472/65535) is. For Bayer input the luma is a 2x2 cell mean, so the test is
// "cell mean >= 0.999": with in-range samples (each <= 1) that needs every one of the four sites
// at >= 0.996 of full scale, i.e. all four saturated -- a cell with only its red site clipped
// does not count.
constexpr double kClippedLuma = 0.999;

// Measures one luma image (1 channel, normalised to [0, 1] as LumaForMeasure makes it).
// Throws std::invalid_argument for an empty image, a channel count other than 1, or a sample
// count that does not match width * height. Deterministic: the same image gives the same stats.
//
// Background and noise. Detection works on a grey-scale opening of the luma by a 3x3 square
// (erosion, then dilation): every structure a 3x3 square does not fit inside -- a hot pixel, a hot
// pair, a hot column or row segment, a 1-2 px wide cosmic-ray streak or staircase -- is flattened
// to its surroundings, while a target keeps its shape (only texture peaks are trimmed, and only
// for detection: centroid, box contents, brightness, clipping and quality use the raw luma).
// The opened image is block-averaged on a detection grid of d x d pixels,
// d = ceil(longest side / 256) (1080p mono: d = 8). The background b is the 20th percentile of
// the block means (the sky, as long as the target covers less than ~80% of the frame). The block
// noise s_b is the MAD of neighbouring block differences / sqrt(2) (blind to smooth gradients
// and vignetting), floored at s / d. The pixel noise s is from the 20th percentile, over tiles
// of up to 32 x 32 pixels, of the mean squared neighbour difference / 2 -- the smoothest tiles,
// i.e. sky where there is any -- so planet texture does not inflate it and quantised 8-bit data
// still gets its true (quantisation included) noise. Caveat: on a target that fills the frame
// (lunar surface, no sky) the smoothest tiles are still textured, s comes out too high, and the
// quality's noise term over-corrects (all frames of the capture alike, so their ranking holds,
// but the absolute quality reads low and can reach 0 on a smooth, blurred frame).
//
// Target rule. Blocks brighter than b + 4 s_b form 8-connected components. A component is a
// target candidate when its brightest block stands at least max(10 s_b, 0.01) above b and at
// least 16 real pixels of the opened luma inside it stand above half that contrast; the candidate
// with the most flux (sum of block excess) is the target.
// 10 s_b: white noise peaks near 4-5 s_b over a 1080p grid, and the 2026-01-30 Saturn AVI (a
// 5-6 DN pedestal and nothing else) peaks at 4.9-5.7 s_b, 0.0044-0.0051 of full scale on the
// opened grid (measured 2026-10-21), so a real target has to stand well clear of both. 0.01 of full scale (2.5 DN at
// 8 bits): a smooth amp glow or gradient can reach many s_b yet stays that faint, while a planet
// capture is exposed to tens of percent (the same night's Neptune, the faintest target on the NAS,
// at gain high enough for 22 DN of noise: 0.80 of full scale, over 18 s_b). 16 pixels, counted on the luma rather than as whole blocks (a 1080p block alone is
// 64 px): the opening already removes anything thinner than 3 px, so this rejects a compact 3x3
// hot cluster (9 px) or a small cosmic-ray blob, while a planet down to ~5 px across (a 12-px
// Neptune has ~100 px above half its contrast) is found. The target's extent is then
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
//
// qualitySigma (noise propagation, first order). Over the n region pixels let SN = n * numerator
// (gradient energy, sum |grad t|^2 of the noise-free t) and SD = n * denominator (signal energy,
// sum (t - b)^2). With white noise e of variance s^2 added to t:
//  - SN = sum |grad(t + e)|^2 - 4 n s^2 fluctuates by 2 sum grad t . grad e = -2 sum e lap(t)
//    (summation by parts), variance 4 s^2 sum lap(t)^2; plus the quadratic sum |grad e|^2, variance
//    40 n s^4 (8 s^4 per squared difference, 2 s^4 with each of the 2 same-direction neighbours,
//    2 s^4 with each of the 4 crossing differences that share a pixel);
//  - SD fluctuates by 2 sum (t - b) e, variance 4 s^2 SD; plus sum e^2, variance 2 n s^4;
//  - their linear parts covary by -4 s^2 sum lap(t) (t - b) = +4 s^2 SN, the quadratic parts by
//    8 n s^4 (4 s^4 per difference, both directions);
//  - s^2 is itself estimated (the 20th percentile of the tile estimates), variance V = 6.12 s^4 / M
//    over the M pixel pairs used; it enters SN as -4 n s^2 and SD as -n s^2, adding 16 n^2 V,
//    n^2 V and covariance 4 n^2 V.
// sum lap(t)^2 is the measured 5-point Laplacian energy minus its noise part (20 s^2 per pixel).
// Then (dQ/Q)^2 = (Var SN / SN^2 + Var SD / SD^2 - 2 Cov / (SN SD)) / 4. The background level b
// and the region are taken as fixed (their noise moves SD a little and SN not at all). Checked
// against 200 noise realisations of a textured planet and a plain disc at two noise levels: the
// empirical scatter is 0.88-1.07 times the reported sigma.
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
// A worker's exception (a read error, a factory that throws or returns null, a reader whose size,
// frame count, colour id or normalisation depth differs from `info`) is rethrown here after every
// thread is joined; so is an exception from report() or cancelled(). No partial result is returned.
std::vector<FrameStats> MeasureAll(const SourceFactory& factory, const ser::FileInfo& info, int threads,
                                   const Progress& progress);

}  // namespace planetary::stack
