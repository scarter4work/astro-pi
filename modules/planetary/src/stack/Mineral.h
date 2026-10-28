#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "Debayer.h"
#include "StackTypes.h"

// The mineral moon (spec docs/superpowers/specs/2026-10-27-planetary-mineral-moon-design.md, Part B, and its
// "Amendment 2026-10-27": Scott's ratio-based algorithm). PCL-free; threads through Parallel.h, no OpenMP.
// Input: a finished linear colour stack, 3 interleaved channels R, G, B, samples normally in [0, 1].
// The steps, in the amendment's order:
//   1. SegmentLunar          the lit disc and the sky
//   2. CalibrateColour       white = the lit disc, background = the sky plane
//   3. NeutraliseBackground  per-channel sky plane removed
//   4. DisplayTransfer       linked midtones stretch, disc median to 0.35, headroom above the brightest sample
//   4b. AlignChannels        R and B onto G when dispersion moved them by more than 0.1 px
//   4c. SharpenLuminanceImage  the sharp luminance L (starlet layers, noise-limited)
//   5-7. MineralColour       out = clip(L (1 + k w d)) with the chroma from ChromaLayer
// MakeMineralMoon runs them all; PrepareMineral (1 - 4c, the expensive part) and ApplyMineralColour (5-7 only)
// split them for the interface's sliders, which re-run only the colour on the cached display RGB and L.
// Every step reports one ASCII HISTORY line; a step that cannot run says why and leaves the image as it was
// (global constraint: it never invents data).

namespace planetary::stack {

// ---- 1. Lunar disk and sky (spec step 1) ----
// disk: the lit disc, terminator side included, enclosed dark features filled; sky: below the disc threshold
// and more than 4 px from the disc. Pixels in neither are the limb's transition, the unlit side's edge, stars.
struct LunarMasks {
    int width = 0, height = 0;
    std::vector<uint8_t> disk, sky;   // 1 = member; disjoint; neither = limb transition / unsure
    size_t diskCount = 0, skyCount = 0;
};
// On the 3 x 3 mean of the channels' mean: the disc's level D is the median of Otsu's brighter class; sky
// candidates are darker than D / 4 (maria, 0.5-0.7 of the highlands, are not); the threshold is the sky level
// plus 5 sigma of its pixel noise, kept within 4-15% of the contrast (a lit terminator at 10% of the lit median
// is disc). Pieces under 1% of the largest are dropped (stars, specks). Under 0.1% of the frame in sky candidates
// there is no sky. Limit: a sky brighter than D / 4 (a twilight Moon) reads as "no sky".
// Throws std::invalid_argument unless rgb is 3 channels whose data matches its size.
LunarMasks SegmentLunar(const ser::FloatImage& rgb, int threads);

// One step's outcome: one ASCII HISTORY line (unsplit), and whether the step declined to run.
struct StepRecord { std::string history; bool skipped = false; };

// ---- 2-3. Colour calibration and background (spec steps 2, 3) ----
// White reference: the mean of the lit disc less the sky plane (NeutraliseBackground's model), over pixels with
// no sample at or above kClippedLuma (Measure.h); R and B are scaled to G's level about the background, so the
// disc's mean is neutral. Skipped (image unchanged) when the disc, or its unsaturated part, is under 1% of the
// frame. With under 1% sky it runs without a background reference and HISTORY says so.
StepRecord CalibrateColour(ser::FloatImage& rgb, const LunarMasks& m);
// Per channel, a plane a + b x + c y fitted on the sky (three rounds of 3-sigma rejection) and subtracted from
// every pixel. Samples may go below 0 (DisplayTransfer sets the black point). Skipped (unchanged) under 1% sky.
StepRecord NeutraliseBackground(ser::FloatImage& rgb, const LunarMasks& m);

// ---- 4b. Channel alignment (amendment) ----
// Measures R's and B's shift against G (Gauss-Newton on G ~ a C(x + s) + b, Catmull-Rom resampling, 8 px border
// left out) and, when either exceeds 0.1 px, resamples R and B onto G. At or under 0.1 px, or when the
// measurement does not converge within 8 px, nothing changes (skipped) and HISTORY gives the shifts or the
// reason. Measured bias on synthetic discs: up to 0.04 px (the channels' terrain differs in colour).
StepRecord AlignChannels(ser::FloatImage& rgb, int threads);

// ---- 4. Luminance sharpening (spec step 4) ----
// The B3-spline starlet (a trous) of a 1-channel image: `layers` detail layers (layer j = c_(j-1) - c_j, holes
// 2^(j-1)), then the residual c_layers; they sum to the input. Mirrored edges. Increment 2's sliders reuse it.
// Throws std::invalid_argument for a non-1-channel image or layers < 1.
std::vector<ser::FloatImage> StarletLayers(const ser::FloatImage& lum1, int layers, int threads);  // last = residual
// Robust std (1.4826 x MAD) of a layer's first channel over the region's pixels (empty region: all). 0 for
// fewer than 2 pixels.
double LayerNoise(const ser::FloatImage& layer, const std::vector<uint8_t>& region);              // robust (MAD) std
// Sharpens a 1-channel luminance in place: layers 1-4, nominal gains {1.5, 2.5, 1, 1}, each times its Wiener
// factor (the share of the layer's energy over the disc that is not noise; the noise is the lower of the sky's
// and the flattest disc tiles' layer-1 MAD), applied to the part of each coefficient beyond 2 sigma of that
// layer's noise. The boost is 0 off the disc and fades in from one to two layer reaches inside the limb's and
// terminator's half-contrast line (no halo, no rim). Above 0.9 a smooth shoulder keeps L under 0.998, so
// sharpening never clips a sample the stack did not. Skipped (unchanged) when there is no disc.
StepRecord SharpenLuminanceImage(ser::FloatImage& lum1, const LunarMasks& m, int threads);
// The same on an RGB image's luminance (the channels' mean): every channel scaled by L's own factor, so R/G and
// B/G are unchanged (chroma untouched); a pixel that would pass 0.998 in a channel is scaled less, uniformly.
StepRecord SharpenLuminance(ser::FloatImage& rgb, const LunarMasks& m, int threads);

// ---- 5-7. Chroma layer and mineral colour (amendment) ----
// Each channel Gaussian-blurred by sigma px (separable, radius 4 sigma, mirrored edges): the chroma noise
// reduction. sigma 0 returns a copy. Throws std::invalid_argument for a negative or non-finite sigma.
ser::FloatImage ChromaLayer(const ser::FloatImage& rgb, double sigma, int threads);
// strength: k, the colour gain (the module's mineralStrength, 0-15, default 8). chromaBlurSigma: px at frames up
// to 2000 px wide, scaled by width / 2000 above (the module's chromaBlur, 0.5-6, default 2.5). eps, devClamp and
// satRamp are internal (amendment). satThreshold < 0 means AutoSatThreshold, the only value the module uses; the
// tests set it to switch the fade off.
struct MineralParams {
    double strength = 8.0, chromaBlurSigma = 2.5, eps = 0.002, devClamp = 0.06, satRamp = 0.12;
    double satThreshold = -1;  // < 0: AutoSatThreshold
};
// 0.92 x the lowest, over R, G and B, of the channel's 99.99th percentile (linear interpolation between order
// statistics, numpy's default) over every pixel.
double AutoSatThreshold(const ser::FloatImage& rgb);
// In place: C = ChromaLayer(rgb, sigma); d_c = clamp(C_c / (mean(C) + eps) - 1, +-devClamp); w = clamp((threshold
// - max(C_R, C_G)) / satRamp, 0, 1); out_c = clip(L (1 + k w d_c), 0, 1), L = lumSharp. Hue follows the sign of d,
// all detail comes from L. One addition to the amendment, for its anti-goal "no new clipping": where some channel
// would pass 0.998, w is lowered for the whole pixel (hue kept, saturation given up), so the colour never clips
// a sample; with k 8 that acts only where L > 0.67 with w > 0. Note eps darkens: the channels' mean of d is
// -eps / (mean(C) + eps), so out's luminance is L (1 - k eps / (mean(C) + eps)) -- 4.5% at a display level of
// 0.35 with k 8, and the clamp's 48% in the sky and the unlit side (mean(C) < 0.031).
// Throws std::invalid_argument unless rgb is 3 channels, lumSharp 1 channel of its size, and the parameters
// finite with strength, sigma, eps, devClamp >= 0 and satRamp > 0.
StepRecord MineralColour(ser::FloatImage& rgb, const ser::FloatImage& lumSharp, const MineralParams& p, int threads = 1);

// ---- 8. Display transfer and the pipeline ----
// A linked midtones (MTF) stretch of x = v / W clipped to [0, 1]: the balance puts the disc's median luminance
// (the frame's without a disc) at 0.35, and the white point W is the smallest at or above the brightest sample
// not flagged in stackClipped (one flag per sample, as rgb.data; empty: none) that lands it at or under 0.95.
// Flagged samples may reach 1.0; no other does. Skipped (samples only clipped to [0, 1]) without signal.
StepRecord DisplayTransfer(ser::FloatImage& rgb, const LunarMasks& m, const std::vector<uint8_t>& stackClipped);

// Progress phase of PrepareMineral and MakeMineralMoon: report(kPhaseMineral, step, 7) before each step (and
// (7, 7) at the end of MakeMineralMoon); cancelled() is polled at each report and a true result throws
// StackCancelled, returning nothing.
constexpr const char* kPhaseMineral = "Mineral moon";

// What the colour step reads, kept so the colour can be re-run without the rest (B8's sliders).
struct MineralCache {
    ser::FloatImage display;  // calibrated, background-neutral, display-transferred, channel-aligned RGB
    ser::FloatImage lum;      // the sharp luminance L (1 channel)
    LunarMasks masks;
    std::vector<std::string> history;  // steps 1-6 (Lunar disk .. Sharpen)
    std::vector<std::pair<std::string, double>> stepSeconds;
};
// Steps 1 - 4c. Throws std::invalid_argument for a stack that is not 3 channels (mono is refused: the spec's
// anti-goal), StackCancelled on a cancel. Memory: the cache holds 4 floats per pixel, plus the input's copy.
MineralCache PrepareMineral(const ser::FloatImage& stack, int threads, const Progress& progress);
// Steps 5-7 only, on a copy of cache.display; record (if given) receives the colour step's HISTORY line. Cheap: a
// chroma blur and one pass (1080p, 32 threads: well under a second). Exceptions as MineralColour's.
ser::FloatImage ApplyMineralColour(const MineralCache& cache, const MineralParams& p, int threads,
                                   StepRecord* record = nullptr);

struct MineralOutcome {
    ser::FloatImage image;             // display-ready RGB, [0,1]
    std::vector<std::string> history;  // one line per step, in order (ASCII)
    std::vector<std::pair<std::string, double>> stepSeconds;
};
// PrepareMineral + ApplyMineralColour: seven HISTORY lines and step times, in the amendment's order (Lunar disk,
// Colour calibration, Background, Display transfer, Channel alignment, Sharpen, Mineral colour). Throws
// std::invalid_argument for a non-3-channel image; StackCancelled when progress cancels (no image is returned).
MineralOutcome MakeMineralMoon(const ser::FloatImage& stack, int threads, const Progress& progress,
                               const MineralParams& params = {});

}  // namespace planetary::stack
