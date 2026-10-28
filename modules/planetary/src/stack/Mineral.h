#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "Debayer.h"
#include "Noise.h"
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
//   4c. SharpenLuminanceImage  the sharp luminance L (starlet layers, noise-limited, by the Sharpen amount)
//   5-7. MineralColour       out = clip(L (1 + k w d)) with the chroma from ChromaLayer
// MakeMineralMoon runs them all; PrepareMineral (1 - 4b and the sharpening's noise, the expensive part) and
// FinishMineral / ApplyMineralColour (4c and 5-7) split them for the interface's sliders (Strength, Chroma blur,
// Sharpen), which re-run only the sharpening and the colour on the cached display RGB, its luminance and noise.
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
    std::string reason;               // with no disc (diskCount 0): why none was distinguished (ASCII); else empty
};
// On the 3 x 3 mean of the channels' mean: the disc's level D is the median of Otsu's brighter class (histogram
// over the full finite range, so a disc of a few hundred px in a 1080p frame is its own class); D must clear the
// darker class's median by 5 sigma of the noise, or there is no disc (empty masks, reason set: a frame of sky). Sky
// candidates are darker than D / 4 (maria, 0.5-0.7 of the highlands, are not); the threshold is the sky level
// plus 5 sigma of its pixel noise, kept within 4-15% of the contrast (a lit terminator at 10% of the lit median
// is disc). Pieces under 1% of the largest, or under 16 px, are dropped (stars, specks). Under 0.1% of the frame
// in sky candidates there is no sky. Limits: a sky brighter than D / 4 (a twilight Moon) reads as "no sky"; a
// frame of sky whose gradient lifts its brighter half 5 noise sigma over its darker half is not told from a disc
// filling the frame (HISTORY then reads "100% of the frame"). NaN samples are left out of the levels.
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
// Measures R's and B's shift against G on a 1 px Gaussian-smoothed copy: one shift s per channel, Gauss-Newton on
// G ~ a_k C(x + s) + b_k with a gain and offset per 16 px window k (lunar colour varies across the disc), over
// the windows where C and G share structure (8 px border left out), with its 1-sigma uncertainty. A channel is
// resampled onto G (Catmull-Rom) when its shift exceeds 0.1 px AND 3 times that uncertainty. Nothing changes
// (skipped; HISTORY gives the shifts or the reason) otherwise, when the measurement does not converge within
// 8 px, or under 16 informative windows (a disc under about 60 px across: its colour cannot be told from a
// shift). Measured on MoonSynth with no true shift (tints +-4%, tint structure 0.8%): a no-sky quarter Moon
// 0.003-0.016 px (6 seeds; 0.047 px with one gain per frame); a 24 x 24 frame with an 8 px disc is skipped
// (one gain per frame applied false shifts of 0.10-0.24 px there). A 0.5 px shift leaves a residual of 0.010 px.
// The result does not depend on the thread count (per-window sums added in window order).
StepRecord AlignChannels(ser::FloatImage& rgb, int threads);

// ---- 4. Luminance sharpening (spec step 4) ----
// The B3-spline starlet (a trous) of a 1-channel image: `layers` detail layers (layer j = c_(j-1) - c_j, holes
// 2^(j-1)), then the residual c_layers; they sum to the input. Mirrored edges. Increment 2's sliders reuse it.
// Throws std::invalid_argument for a non-1-channel image or layers < 1.
std::vector<ser::FloatImage> StarletLayers(const ser::FloatImage& lum1, int layers, int threads);  // last = residual
// Robust std (1.4826 x MAD) of a layer's first channel over the region's pixels (empty region: all). 0 for
// fewer than 2 pixels.
double LayerNoise(const ser::FloatImage& layer, const std::vector<uint8_t>& region);              // robust (MAD) std
// The white-noise std of a 1-channel luminance: the lower of the sky's and the flattest disc tiles' layer-1 MAD,
// / 0.889. Fair only where the sky's noise is the disc's, i.e. on linear data (after a display transfer the sky is
// clipped at 0 and the curve is steeper on the disc: it read ~7x low there). 0 with no disc. Throws
// std::invalid_argument unless lum1 is 1 channel and the masks are of its size.
double LuminanceNoise(const ser::FloatImage& lum1, const LunarMasks& m, int threads);
// Sharpens a 1-channel luminance in place: layers 1-4, nominal gains {1.5, 2.5, 1, 1} at amount 1, each times its Wiener
// factor (the share of the layer's energy over the disc that is not noise), applied to the part of each
// coefficient beyond 2 sigma of that layer's noise at the pixel. The noise: `noise`, the white-noise std per
// pixel when given (PrepareMineral: LuminanceNoise on the linear data times the display transfer's slope there),
// else LuminanceNoise(lum1) everywhere (right for linear input). The boost is 0 off the disc and fades in from one
// to two layer reaches inside the limb's and terminator's half-contrast line (no halo, no rim). Above 0.9 a
// smooth shoulder keeps L under 0.998, and below a quarter of a pixel's unsharpened value a smooth toe keeps it above
// 0 (shadows beside sunlit rims), so sharpening never clips a sample the stack did not, at either end. Skipped (unchanged)
// when there is no disc. Throws std::invalid_argument as LuminanceNoise, or for a noise map not of lum1's size.
// layerShape (optional; Ruling R33): per layer, its noise over the white-noise prediction (LayerNoiseShape); null:
// white noise (1 for every layer).
// amount (the Sharpen slider, Task SHARP): 0 leaves lum1 exactly as it is (the step is skipped, HISTORY says so);
// 1 is the nominal gains above; above 1 each layer's boost grows by (amount - 1) x {3, 1, 0, 0} more, the finest
// layer most, as a deconvolution would (Mineral.cpp, kLayerExtra: measured on real 64.6F against Scott's
// reference). The noise limit (Wiener factor, soft threshold at the per-pixel noise) holds at every amount: a
// noise-only (flat) disc's noise gain is 1.006 at 1, 1.04 at 2, 1.13 at 4. Noise riding texture above the threshold
// takes the full gain, so on a textured disc it grows with the amount (MoonSynth, two realisations: 1.39 at 1, 1.86
// at 1.25, 3.31 at 2), and on MoonSynth (truth known) 2 sharpens past the truth; the synthetic textured and
// truth bars are judged at 1 (Ruling R40). Default 1.5, Scott's pick from the 64.6F series (Ruling R42): there
// 64.6F's crater rims measure 1.07x his reference's crispness (2: 1.38x, at ~3.6x the disc noise; Task SHARP
// report). Range 0 - 4. Throws std::invalid_argument for a negative or non-finite amount. HISTORY's Sharpen line
// names the amount.
constexpr double kSharpenDefault = 1.5, kSharpenMax = 4.0;
StepRecord SharpenLuminanceImage(ser::FloatImage& lum1, const LunarMasks& m, int threads,
                                 const std::vector<float>* noise = nullptr,
                                 const std::array<double, 4>* layerShape = nullptr, double amount = kSharpenDefault);
// The noise's shape over the starlet layers 1-4 (Ruling R33): per layer j, the robust std of layer j over the sky
// (eroded by the layer's reach, so the disc's edge stays out) over kStarletNoise[j] times the white-noise std that
// layer 1 implies there; layer 1 is 1 by construction. A stack's noise is correlated (the demosaic, the warp's
// interpolation): on real Moons layers 2-4 read 1.6-1.7x, 1.9-2.2x, 2.4-3.1x the white prediction, and the white
// thresholds let that noise through the boost. Each factor is at least 1 (a sky smoother than white noise does not
// lower a threshold). A layer whose eroded sky has under 1000 px (a disc filling the frame; final review B, I1) is
// measured the same way over the disc's flattest tenth of 16 x 16 tiles where its boost is at full weight, picked by
// layer 1's std over `norm` (the predicted noise per pixel, when given; the coefficients are divided by it too):
// terrain only raises the factor there, so it errs toward less boost. With under 10 such tiles, kAssumedShape (the
// real-data range's top, 1.7 / 2.2 / 3.1). source (if given) receives where the factors came from ("the sky's",
// "the disc's flattest tiles'", "assumed, ...", or per layer when they differ) for HISTORY. 1s with no disc. lum1
// linear (the sky is clipped after a display transfer). Throws std::invalid_argument as LuminanceNoise, or for a
// norm not of lum1's size.
std::array<double, 4> LayerNoiseShape(const ser::FloatImage& lum1, const LunarMasks& m, int threads,
                                      const std::vector<float>* norm = nullptr, std::string* source = nullptr);

// ---- 5-7. Chroma layer and mineral colour (amendment) ----
// Each channel Gaussian-blurred by sigma px (separable, radius 4 sigma, mirrored edges): the chroma noise
// reduction. sigma 0 returns a copy. Throws std::invalid_argument for a negative or non-finite sigma.
ser::FloatImage ChromaLayer(const ser::FloatImage& rgb, double sigma, int threads);
// strength: k, the colour gain (the module's mineralStrength, 0-15, default 8). chromaBlurSigma: px at frames up
// to 2000 px wide, scaled by width / 2000 above (the module's chromaBlur, 0.5-6, default 2.5). eps, devClamp and
// satRamp are internal (amendment). satThreshold < 0 means automatic: AutoSatThreshold in MineralColour itself, the
// cache's HighlightThreshold in ApplyMineralColour (the module's only path); the tests set it to switch the fade off.
struct MineralParams {
    double strength = 8.0, chromaBlurSigma = 2.5, sharpen = kSharpenDefault, eps = 0.002, devClamp = 0.06, satRamp = 0.12;
    double satThreshold = -1;  // < 0: automatic (see above)
};
// 0.92 x the lowest, over R, G and B, of the channel's 99.99th percentile (linear interpolation between order
// statistics, numpy's default) over every pixel.
double AutoSatThreshold(const ser::FloatImage& rgb);
// The fade threshold the pipeline uses (Task B9; spec step 7, no cyan cast in the brightest 1% of the disc):
// AutoSatThreshold, capped at the disc's 99th percentile of the fade's own variable, max(C_R, C_G) of the chroma
// layer at chromaBlurSigma (scaled as MineralColour scales it), so the disc's brightest 1% always fade to neutral.
// AutoSatThreshold alone follows the frame's channel extremes: where R's sits low (64.6F, 700F) it is under the
// disc's 99th percentile already (0.59 against 0.66); where the three sit close (0528a/b: 0.73-0.78) it landed above
// it (0.67 against 0.60), the brightest highlands kept 0.41-0.46 of their colour boost and their bluish tint came
// out cyan 0.048 (0.024 before the colour step; capped: 0.008). The disc's own value only ever lowers it.
// PrepareMineral measures it once, at the default chroma blur (MineralParams{}); a slider's other blur moves the
// fade variable's 99th percentile only slightly.
double HighlightThreshold(const ser::FloatImage& rgb, const LunarMasks& m, double chromaBlurSigma, int threads);
// In place: C = ChromaLayer(rgb, sigma); d_c = clamp((C_c - mean(C)) / (mean(C) + eps), +-devClamp); w =
// clamp((threshold - max(C_R, C_G)) / satRamp, 0, 1); out_c = clip(L (1 + k w d_c), 0, 1), L = lumSharp. Hue
// follows the sign of d, all detail comes from L. eps only keeps a black sky from dividing by 0 (Ruling R26): a
// neutral pixel gets d = 0, and the channels' mean of d is 0 wherever the clamp does not engage, so out's
// luminance is L's there. One addition to the amendment, for its anti-goal "no new clipping": where some channel
// would pass 0.998, w is lowered for the whole pixel (hue kept, saturation given up), so the colour never clips
// a sample; with k 8 that acts only where L > 0.67 with w > 0.
// Throws std::invalid_argument unless rgb is 3 channels, lumSharp 1 channel of its size, and the parameters
// finite with strength, sigma, eps, devClamp >= 0 and satRamp > 0.
StepRecord MineralColour(ser::FloatImage& rgb, const ser::FloatImage& lumSharp, const MineralParams& p, int threads = 1);

// ---- 8. Display transfer and the pipeline ----
// A linked midtones (MTF) stretch of x = v / W clipped to [0, 1]: the balance puts the disc's median luminance
// (the frame's without a disc) at 0.35, and the white point W is the smallest at or above the brightest sample
// not flagged in stackClipped (one flag per sample, as rgb.data; empty: none) that lands it at or under 0.95.
// Flagged samples may reach 1.0; no other does. Skipped (samples only clipped to [0, 1]) without signal. curve
// (if given) receives the transfer applied, the default (identity on [0, 1]) when it skipped.
struct DisplayCurve {
    double white = 1, balance = 0.5;  // y = MTF(clamp(v / white, 0, 1), balance)
};
StepRecord DisplayTransfer(ser::FloatImage& rgb, const LunarMasks& m, const std::vector<uint8_t>& stackClipped,
                           DisplayCurve* curve = nullptr);

// Progress phase of PrepareMineral, FinishMineral and MakeMineralMoon: report(kPhaseMineral, step, 7) before each
// of PrepareMineral's steps 0-4 and at its end (5), then FinishMineral's (6, 7) after the sharpening and (7, 7) at
// its end; cancelled() is polled at each report and a true result throws StackCancelled, returning nothing.
constexpr const char* kPhaseMineral = "Mineral moon";

// What the sharpening and the colour step read, kept so they can be re-run without the rest (the sliders).
struct MineralCache {
    ser::FloatImage display;  // calibrated, background-neutral, display-transferred, channel-aligned RGB
    ser::FloatImage lumBase;  // its luminance, the channels' mean (1 channel), unsharpened
    std::vector<float> noise;  // the sharpening's white-noise std per pixel at display scale (empty with no disc)
    std::array<double, 4> layerShape{1, 1, 1, 1};  // LayerNoiseShape on the linear luminance
    std::string sharpenNote;   // the noise's source and shape, appended to the Sharpen line
    LunarMasks masks;
    std::vector<std::string> history;  // steps 1-5 (Lunar disk .. Channel alignment)
    std::vector<std::pair<std::string, double>> stepSeconds;
    std::string notice;                // as MineralOutcome::notice
    double satThreshold = -1;          // HighlightThreshold(display, masks): the fade threshold, measured once
};
// Steps 1 - 4b and the sharpening's noise. Throws std::invalid_argument for a stack that is not 3 channels (mono is
// refused: the spec's anti-goal) or that holds NaN or infinite samples (the message gives their count; none is
// guessed at), StackCancelled on a cancel. With no disc found, every later step skips (see
// MineralOutcome::notice). Memory: the cache holds 22 bytes per pixel (display 12, lumBase 4, noise 4, masks 2:
// 46 MB at 1920 x 1080); the working copy of the stack becomes cache.display (moved, not copied).
// The sharpening's noise (Ruling R35). With `noise` (the stack's: RunStack's StackOutcome::noise, for this stack;
// Usable() and of its size) and a sky of at least 1000 px: per pixel, sqrt(model(t) alpha2) (Noise.h, StackNoise)
// scaled once so that over the sky it equals the sky's measured white-noise std on the calibrated linear
// luminance -- the level (shot noise: real discs 7-10x their sky's noise) and the frame count come from the stack,
// the units from the sky -- then through the display transfer's slope at each pixel. With under 1000 px of sky (a
// disc filling the frame; final review B, I1) it is scaled on the disc's flattest tiles instead (see LayerNoiseShape):
// terrain only raises that, so on real Moons, textured everywhere, it errs high (64.6F cut to its disc: x8.5 against
// the sky's x1.7) and sharpens less, never more. Without it (null or unusable), the sky's noise everywhere (or the
// flattest disc tiles', with no sky), a lower bound on a bright disc's. HISTORY's Sharpen line says which, and where
// the layer shape (LayerNoiseShape's on the linear data, normalised by the map when there is one) came from.
MineralCache PrepareMineral(const ser::FloatImage& stack, int threads, const Progress& progress,
                            const StackNoise* noise = nullptr);
// Step 4c on the cache: cache.lumBase sharpened by `amount` (SharpenLuminanceImage with the cache's noise and layer
// shape); record (if given) receives the Sharpen HISTORY line (the noise's source appended). With no disc in the
// cache it skips (record says why) and returns lumBase as it is. Throws std::invalid_argument for a negative or
// non-finite amount.
ser::FloatImage SharpenedLuminance(const MineralCache& cache, double amount, int threads, StepRecord* record = nullptr);
// Steps 4c-7 on the cache: SharpenedLuminance at p.sharpen, then the colour on a copy of cache.display; record (if
// given) receives the colour step's HISTORY line, sharpenRecord the Sharpen line. Cheap: a starlet transform, a
// chroma blur and a few passes (1910 x 1071, 32 threads: about 0.07 s); it allocates about 50 bytes per pixel while
// it runs. p.satThreshold < 0 takes cache.satThreshold (the same value MineralColour would measure on
// cache.display). Exceptions as MineralColour's. With no disc in the cache, both steps skip (the records say why) and
// the copy is returned as it is.
ser::FloatImage ApplyMineralColour(const MineralCache& cache, const MineralParams& p, int threads,
                                   StepRecord* record = nullptr, StepRecord* sharpenRecord = nullptr);

struct MineralOutcome {
    ser::FloatImage image;             // display-ready RGB, [0,1]
    std::vector<std::string> history;  // one line per step, in order (ASCII)
    std::vector<std::pair<std::string, double>> stepSeconds;
    // Empty, or why the image is not a mineral moon, for the result line (spec anti-goal: a step that cannot run
    // says why in HISTORY and in the result line): "no lunar disk distinguished from the sky", when step 1 finds
    // none. Every step then skips with that reason and the image is the stack clipped to [0, 1].
    std::string notice;
};
// ApplyMineralColour as an outcome: the cache's five HISTORY lines and step times plus the Sharpen and Mineral colour
// lines, each step timed; the notice is the cache's. The window's sliders and MakeMineralMoon both take this path, so
// the two are the same bit for bit. Exceptions as ApplyMineralColour's; StackCancelled when progress cancels.
MineralOutcome FinishMineral(const MineralCache& cache, const MineralParams& params, int threads, const Progress& progress = {});
// PrepareMineral + FinishMineral: seven HISTORY lines and step times, in the amendment's order (Lunar disk,
// Colour calibration, Background, Display transfer, Channel alignment, Sharpen, Mineral colour). Throws
// std::invalid_argument for a non-3-channel image, NaN or infinite samples or parameters out of range;
// StackCancelled when progress cancels (no image is returned).
MineralOutcome MakeMineralMoon(const ser::FloatImage& stack, int threads, const Progress& progress,
                               const MineralParams& params = {}, const StackNoise* noise = nullptr);

}  // namespace planetary::stack
