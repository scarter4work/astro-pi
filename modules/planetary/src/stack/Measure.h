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
    // Half the larger extent, along x or y, of the box's pixels above half the target's contrast in
    // the opened luma ("Background and noise": a noise spike on a shallow, blurred limb would otherwise
    // push the extent out -- an R 24 disc blurred 12 px read 25.5 on the raw luma, 24.5 opened)
    // (luma px; 0 when no target): a disc's radius at its limb's half point. A blur hardly
    // moves it -- a blurred edge still crosses half contrast at the edge; only once the blur is a large
    // part of R does the dimming centre move it (out) -- while the box, regrown from the 10% contour,
    // moves out by 1.28 sigma plus up to a block (measured 2026-10-22, the 2022-09-17 1080p Jupiter
    // blurred by 0 / 3 / 6 luma px: this 21.5 / 21.0 / 20.5, the box 26 / 27 / 30). Dark texture
    // inside the disc does not shrink it (only the outermost pixels count), nor does the phase of a
    // gibbous or crescent disc (its cusps still span the diameter along one axis; a diagonal one, at
    // least 0.85 of it) -- reasoned, untested: no Mars or Venus capture on the NAS is a large disc.
    // Saturn's open rings widen it: they span ~2.3 globe radii and are bright enough to clear half
    // contrast (the 2022-08-03 Saturn: 12.5 against a box radius of 13.6 -- the rings fill the box), so
    // its focus reading, ~1 / R^2, is up to ~5x lower than the globe's own: Select's large-disc rule is
    // effectively off for a ringed Saturn (it fails safe: stacked, never refused). Edge-on rings are
    // opened away. Select judges large discs' focus by it (Select.h, step 7).
    double halfRadius = 0;
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

// A raw colour mosaic read as mono (Ruling P9): an AVI whose Bayer pattern is unknown -- Auto found no
// usable sidecar, FileInfo::avi->bayerSource "none" -- so frames come as 1-channel CFA data. Its
// colour pattern sits at the Nyquist frequency in every frame and would dominate any detail measure;
// measurement (frame statistics, reference, global and local alignment, region quality) therefore
// works on 2 x 2-binned luma, as for Bayer input, while the stack itself stays raw mono at full
// resolution. That stack is not a CFA any more -- every frame was resampled at sub-pixel shifts before
// it was added, which mixes the colour sites -- so it cannot be debayered afterwards; colour needs the
// pattern chosen when the capture is opened (BayerChoice), and the stack run again. A pattern chosen
// as "none" by the user (bayerSource "chosen") is a mono camera and is measured as is.
bool UndeclaredMosaic(const ser::FileInfo& info);

// Frame (sensor) pixels per measurement-luma pixel, along each axis. 2 for Bayer input and for an
// undeclared mosaic (the luma is each 2x2 cell's mean, so width / 2 x height / 2, integer division),
// 1 for mono and RGB. FrameStats coordinates times this are frame (sensor) pixels; the luma image's
// size is header.width / LumaScale (integer part).
// The one place this factor is defined (Ruling P3): consumers never scale by 2 on their own.
double LumaScale(const ser::FileInfo& info);

// The measurement luma of a frame of this capture: LumaForMeasure, with an undeclared mosaic binned
// 2 x 2 like a Bayer frame. Every measuring stage reads frames through this.
ser::FloatImage MeasurementLuma(const ser::Frame& frame, const ser::FileInfo& info);

// Side d of the detection grid's square blocks for a luma image of width x height:
// d = ceil(longest side / 256), at least 1 (see "Background and noise" below).
int DetectionBlock(int width, int height);

// Luma at or above this is saturated: 254/255 is not, a 12- or 10-bit ADC scaled into 16 bits
// (65520/65535, 65472/65535) is. For Bayer input the luma is a 2x2 cell mean, so the test is
// "cell mean >= 0.999": with in-range samples (each <= 1) the four sites' shortfalls from full
// scale have to sum to at most 0.004. Every site is therefore at >= 0.996 of full scale, and a site
// can sit as low as 0.996 only when the other three are exactly full scale; a cell with only its red
// site clipped does not count, and neither does one whose four sites are all at 254/255.
constexpr double kClippedLuma = 0.999;

// Measures one luma image (1 channel, normalised to [0, 1] as LumaForMeasure makes it).
// `lumaScale`: frame (sensor) pixels per luma pixel along each axis -- LumaScale(info) for a frame
// of a capture (MeasurementLuma's output), 1 for an image at sensor resolution. Only the target
// size floor uses it ("Target rule"); every FrameStats field stays in luma pixels.
// Throws std::invalid_argument for an empty image, a channel count other than 1, a sample
// count that does not match width * height, or a lumaScale that is not finite and >= 1.
// Deterministic: the same image gives the same stats.
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
// and vignetting), floored at s / d (s as EstimatePixelNoise reads it: the box is not known yet).
// On sky the floor is what holds -- s / d exceeds the MAD estimate in every frame of the 2026-01-30
// Saturn, Neptune and Jupiter 195830 and the 2023-02-11 Jupiter (measured 2026-10-22) -- so s_b
// moved with Ruling P19's s (+2-3% there).
// The pixel noise s is read from the smoothest tiles -- sky where there is any -- so planet texture
// does not inflate it and quantised 8-bit data still gets its true (quantisation included) noise:
// over tiles of up to 32 x 32 pixels, each tile's mean squared horizontal neighbour difference
// (2 s^2 for white noise); q = the 20th percentile of them; s^2 = the noise level at which q is where
// that order statistic is expected (Ruling P19). Taken as is, q / 2 read s^2 4.3-5.2% low (0.948-0.957
// of the truth on white noise, 100^2 to 1080^2): the lowest fifth of K tile estimates sits ~0.84 of
// their spread under their mean. The model of a noise tile: m pairs in rows of L estimate 2 s^2
// near-normally with relative sd tau = sqrt((kappa - (kappa - 1) / (2 L)) / m) (a row's variance is
// (4 kappa L - 2 kappa + 2) s^4; kappa = mu4 / s^4 of the noise, 3 for Gaussian noise, 4.8 for 8-bit
// noise of 0.4 DN), and the k-th smallest of N of them is expected at their Phi^-1((k + 0.625) /
// (N + 0.25)) (Blom). The noise tiles are those whose estimate lies within 3 Gaussian tau of the
// fitted mean (counted / 0.99865) -- the target's own tiles sit far above the sky's -- and kappa is
// measured on them (2 E[d^4] / E[d^2]^2 - 3, at most 12); 8 rounds to a fixed point. Where every tile
// is a full tile of Gaussian noise this is the order-statistic correction 1 / (1 - 0.8416 sqrt(3 K /
// M)); where not it is more exact: that one, counting a disc's tiles as noise, read s^2 0.2-2.3% high
// with a disc of radius 0.15 of the short side and 1.0-14% high at 0.3, and with few tiles its rank is
// off (the 4th of 16 is the 0.22 quantile, not 0.2: 100^2 read 1.005-1.007). Measured 2026-10-22
// (test "EstimatePixelNoise is unbiased ...", white noise 0.01): mean s^2 over the truth 0.997-1.001
// from 100^2 to 1080^2, on sky and with such a disc; 0.997-1.000 on 8-bit sky of 0.4-3 DN. The
// variance of the estimate (qualitySigma's V) is the sample quantile's, pr (1 - pr) / (N f(q)^2) (pr
// the Blom share, f the noise tiles' density at q), scaled to s^2.
// No correction (s^2 = q / 2, variance 6.12 s^4 / M, M the pairs used: the same order statistic's
// for K Gaussian tiles at p = 0.2) when q cannot be shown to be noise: the sky -- the tiles clear of
// the target's box in MeasureFrame, the whole image for EstimatePixelNoise -- holds at most k noise
// tiles, or there are at most 2k + 1 in all (q is not in their lower half). That is a target that
// fills the frame: its smoothest tiles are texture, which no noise model describes (corrected anyway,
// s varied 0-1.9% between the frames of a synthetic surface, as the share of its tiles in the band
// did, and the frames' ranking moved with it). Caveat there: s comes out too high and the quality's
// noise term over-corrects (all frames of the capture alike, so their ranking holds, but the absolute
// quality reads low and can reach 0 on a smooth, blurred frame).
//
// Target rule. Blocks brighter than b + 4 s_b form 8-connected components. A component is a
// target candidate when its brightest block stands at least max(10 s_b, 0.01) above b and at
// least 16 sensor pixels' worth of the opened luma inside it (luma pixels x lumaScale^2) stand
// above half that contrast; the candidate with the most flux (sum of block excess) is the target.
// 10 s_b: white noise peaks near 4-5 s_b over a 1080p grid, and the 2026-01-30 Saturn AVI (a
// 5-6 DN pedestal and nothing else) peaks at 4.5-6.0 s_b, 0.0045-0.0060 of full scale on the
// opened grid (measured 2026-10-22 over its 21 frames, the brightest block over b; 4.7-6.2 s_b with
// the s that read low before Ruling P19), so a real target has to stand well clear of both. 0.01 of full scale (2.5 DN at
// 8 bits): a smooth amp glow or gradient can reach many s_b yet stays that faint, while a planet
// capture is exposed to tens of percent (the same night's Neptune, the faintest target on the NAS,
// at gain high enough for 22 DN of noise: its brightest opened block 0.34-0.60 of full scale,
// 9.7-17.2 s_b -- the one frame of 94 under 10 s_b is the one found without a target, before P19 as
// after (9.9-17.6 s_b then)). 16 sensor pixels, counted on the luma rather than as whole blocks (a 1080p block alone is
// 64 px): the opening already removes anything thinner than 3 luma px, so at sensor resolution this
// rejects a compact 3x3 hot cluster (9 px) or a small cosmic-ray blob, while a planet
// 5 px across is found every time (measured 2026-10-22 at lumaScale 1: seeing sigma 1 px on the Saturn
// AVI's background, 96 placements per diameter: 5 px and up 96/96, 4 px 85-87/96, 3 px 28-44/96,
// 2 px 32-41/96, 1 px 0/96; 2 px reads as often as 3 px, a non-monotone result whose cause was not
// investigated; test "the size floor finds a blurred planet from 5 px across"), so below
// ~5 px across detection is unreliable. On half-resolution luma (Bayer, or an undeclared mosaic) the floor is 4 luma px,
// below the 9 that any structure surviving the opening already has (a 3x3 luma square = 6x6 sensor
// pixels; the hot structures above, painted at sensor resolution and binned, are all rejected), so
// the opening is the floor there. Counting the floor in
// luma pixels instead lost 29 of 94 frames of the 2026-01-30 Neptune AVI (measured 2026-10-22:
// binned, its disc has 9-22 luma px above half contrast) and then the whole capture, whose aligned
// reference fell under it too. The target's extent is then
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
//  - s^2 is itself estimated (from the 20th percentile of the tile estimates), variance V as
//    "Background and noise" gives it (~6.7 s^4 / M over the M pixel pairs used, on 1080p sky); it enters
//    SN as -4 n s^2 and SD as -n s^2, adding 16 n^2 V, n^2 V and covariance 4 n^2 V.
// sum lap(t)^2 is the measured 5-point Laplacian energy minus its noise part (20 s^2 per pixel).
// Then (dQ/Q)^2 = (Var SN / SN^2 + Var SD / SD^2 - 2 Cov / (SN SD)) / 4. The background level b
// and the region are taken as fixed (their noise moves SD a little and SN not at all). Checked
// against 200 noise realisations of a textured planet and a plain disc at two noise levels: the
// empirical scatter is 0.86-1.05 times the reported sigma (0.88-1.07 before Ruling P19); on a
// defocused blob at the noise floor (StackSynth R 40 convolved by 20 px, noise 0.005 / 0.01) 1.06 /
// 0.95. There the quality is near the noise floor in another way: its gradient energy is ~4.5% of
// the noise's 4 s^2, so an error of 1% in s^2 moves it ~10% -- the estimate's own scatter is in
// qualitySigma, but what is left of its bias is not (+0.4-0.9% in s^2 on such frames, whose wing tiles
// carry a little gradient: quality 3-9% low).
FrameStats MeasureFrame(const ser::FloatImage& luma, int64_t index, double lumaScale);

// The pieces of MeasureFrame that other stages reuse on parts of a frame (Task 5's regions).
// Pixel noise s of a luma image, as MeasureFrame estimates it ("Background and noise") with the whole
// image taken as sky (no target box), and the variance of the estimate s^2 itself. Throws
// std::invalid_argument for an empty or non-1-channel image or a sample count that does not match.
struct NoiseEstimate {
    double sigma = 0;      // s
    double varSigma2 = 0;  // variance of the estimate s^2
};
NoiseEstimate EstimatePixelNoise(const ser::FloatImage& luma);
// MeasureFrame's quality and qualitySigma ("Quality", "qualitySigma") over the rectangle
// [rx0, rx1) x [ry0, ry1) of a row-major 1-channel image of row length w (the rectangle must lie
// inside it), with background level b and the image's pixel noise. Both 0 when the noise-corrected
// gradient or signal energy is not positive (or the rectangle is empty).
struct QualityEstimate {
    double quality = 0, sigma = 0;
};
QualityEstimate RegionQuality(const float* p, int w, int rx0, int ry0, int rx1, int ry1, double b,
                              const NoiseEstimate& noise);

// Opens one reader. Called once per worker thread, from that thread -- so concurrently.
using SourceFactory = std::function<std::unique_ptr<ser::FrameSource>()>;

// Measures frames 0 .. info.frameCount - 1: each frame is read, turned into luma with
// LumaForMeasure(frame, info.colorId, info.normDepth) and measured. `threads` workers (fewer when
// there are fewer frames; < 1 counts as 1) each open their own reader through `factory` and take
// runs of consecutive frames, so every reader reads forward. Result: one FrameStats per frame,
// in frame order, identical for any thread count.
// Memory: besides its reader and the frame in flight, each worker thread keeps MeasureFrame's
// thread_local scratch alive for the life of the thread (two opened-luma buffers, ~16 MB at 1080p
// mono, 4 bytes per luma pixel each), so budget ~16 MB per worker at 1080p on top of the frames.
// Progress phase "Measuring frames" (done, frameCount); report() and cancelled() are only called
// from the calling thread. Cancel: workers stop, every thread is joined, StackCancelled is thrown.
// A worker's exception (a read error, a factory that throws or returns null, a reader whose size,
// frame count, colour id or normalisation depth differs from `info`) is rethrown here after every
// thread is joined; so is an exception from report() or cancelled(). No partial result is returned.
std::vector<FrameStats> MeasureAll(const SourceFactory& factory, const ser::FileInfo& info, int threads,
                                   const Progress& progress);

}  // namespace planetary::stack
