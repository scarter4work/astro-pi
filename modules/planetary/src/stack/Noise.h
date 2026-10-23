#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "Debayer.h"
#include "FrameSource.h"
#include "StackTypes.h"

namespace planetary::stack {

struct NoiseEstimate;  // Measure.h

// The capture's pixel noise as a function of the signal level (Ruling P26): the variance of a luma pixel
// at each level, measured from the frames themselves (FitNoiseModel) once per capture -- the camera, its
// gain and its exposure do not change during it -- and used wherever a detail measure subtracts or
// propagates the noise: MeasureFrame's quality and qualitySigma (MeasureAll) and the regions' quality and
// qualitySigma (RegionMeasurer). Before P26 one level was measured -- on the sky for the frame, over all
// the regions' windows for the regions -- and applied to every pixel, so a bright target's own shot noise,
// a black-clipped sky (2 noise sd above black, the sky clips 2.3% and its variance reads 0.960 of the
// target's) and the regions of a dark lunar limb (a level measured mostly on the bright surface zeroed
// their quality) all got the wrong noise.
//
// Not used for: target detection (the detection floor is about the sky's own fluctuation, which the sky's
// measured noise is: Measure.h, "Background and noise"), the regions' placement (on the reference's own
// noise reading, Regions.h), the stack's outlier rejection (its scale is the per-pixel RMS frame-to-
// reference difference over the capture, which already holds the noise at that pixel's level:
// Accumulate.h), hot pixels (HotPixelMap's ring tests are relative to the neighbours) and the engine's
// focus measure (a half-stack cross-spectrum, Engine.cpp).
//
// The curve. Physically a photon-transfer line, var(t) = a + b t (read noise plus shot noise, luma
// units^2), bent where the data clip: a sky at or near black, a colour site that saturates first (the
// 2026-01-30 Moon 193142 turns down from luma 0.85 and reads 4.5e-5 at 0.94, where the line gives 2.9e-4),
// the dim sites of a dark level clipping before the luma does (the same Moon's curve is concave under
// luma ~0.1: 2.2e-6 at 0.02, where the line gives 6.1e-6; test/calibration/nreal.cpp). So the model is
// the measured curve itself, smoothed (NoiseModel::Knot); the line marks where the curve clips below it.
//
// Its level is the one the measures look it up by: a pixel's 3 x 3 mean in the frame (PixelVariance,
// Measure.h), noise and all. Measured against the same observed level, the curve is the mean noise of the
// capture's pixels at each observed level, so summed over a population of pixels like the capture's it is
// unbiased whatever the level's own noise: a sky's levels spread with its noise and the curve stays flat
// across them. (A [1 4 1] x [1 4 1] / 36 level, the response's own weights, has 1.5x the 3 x 3 mean's noise
// (sd: sqrt(sum w^2) = 18 / 36 against 1 / 3), so a curve read against it is not the one the lookups at the
// 3 x 3 mean need.)
//
// Measurement (a noise curve per intensity bin, as Colom and Buades, JOSA A 31(4) 863, 2014, from one
// image; here from frames in time), in two passes over the same runs of frames:
//  - responses of [1 -2 1] over three consecutive frames times [1 -2 1] x [1 -2 1] in space, at every
//    pixel: noise of variance v gives responses of variance 216 v, whatever its distribution; anything
//    static (texture, a colour mosaic's pattern, fixed-pattern noise) cancels in time, anything smooth
//    cancels in space, and texture the seeing moves leaks in only through its curvature times the jitter's
//    second difference. Measured 2026-10-22 per level (32 triples, plain mean squares;
//    test/calibration/temporal.cpp): a spatial-only response reads 1.1-1.7x this on the 2026-01-30 Moon
//    193142 and 1.6-3.1x on the 2023-02-02 Moon (texture), a temporal-only one 1.2-2.4x / 1.7-6.6x (the seeing moves the texture); on sky (Saturn
//    203627, Neptune 204515) the three agree within 0.2% (temporal) and 2-3.5% (spatial: the sensor's fixed
//    pattern, which every frame shares, so it neither scatters the quality nor is a frame's noise).
//    Triples slide by one frame within runs of consecutive frames (NoiseRuns), up to kNoiseResponses
//    responses in all: the error of a capture's model is shared by all its frames, and on a faint blob,
//    whose gradient energy is a few percent of the noise's, +0.5% in v lowers the frames' mean quality by
//    17% (R 24 convolved by 24 px over a sky of 0.06, seed 7: 0.0278 -> 0.0229), and every pixel of every
//    triple puts the line's own sd at 0.20% (test/calibration/modelerror.cpp). A response's level is taken
//    on the frame after the triple, whose noise is independent of the response's;
//  - pass 1: kFineBins bins of equal width over [0, 1], merged from the dark end until each holds
//    kMinBinResponses; per bin the plain mean square over 216 and its sampling variance from the bin's
//    own fourth moment, times the overlapping responses' correlation (1.944 per axis and in time for
//    [1 -2 1] at a step of 1) -- no trimming: a trimmed moment is biased by the noise's own tails (the real
//    sky of the 2026-01-30 Saturn AVI 203627, all 18 triples: response kurtosis 3.32, its Gaussian-corrected
//    trimmed moment 0.966 of its variance at an 80% trim, 0.960 at 50%; test/calibration/skynoise.cpp). Its
//    mean holds the texture the seeing moved across a limb or a crater rim (the Moon 193142's curve read
//    1.43x the dark limb's own noise at luma 0.13, from the limb's edges at that level);
//  - pass 2, that texture out: kNoiseTile x kNoiseTile tiles of one level (NoiseSamples::Tile, AddTriple),
//    each its responses' summed square over pass 1's variance summed at their levels; per kBinTiles tiles
//    by level (never across a gap in level: a bin straddling levels no tile takes reads a mix as one
//    level's), the median ratio c, unbiased to the mean through the tiles' chi^2 law (TilePoints). The
//    texture pass 1 holds there, m (1 - c) (m pass 1's curve; c smoothed over kKnotTiles tiles within
//    kTileSpread of the level), is subtracted from pass 1's bins: linearly in level between the bins,
//    held to the outermost bins' own tile levels, none beyond (Corrected). Subtracted, not scaled:
//    texture does not grow with the noise, so the model rises by exactly the noise added to a capture
//    (fix round 1: a ratio interpolated across the 2023-02-02 Moon's gap between its sky tiles and its
//    surface's, from a bin straddling it, rose by more than the noise added there). A small disc has no
//    tile of one level: the 2026-01-30 Jupiter 195830 disc, ~20 luma px across, keeps pass 1's 0.88-0.97 of
//    its own noise. Measured against each window's own spatio-temporal noise, 300 triples
//    (test/calibration/winnoise.cpp; the added noise below, inject.cpp): the Moon 193142's dark limb at
//    0.13 / 0.047 / 0.019, 1.05 / 1.02 / 1.01 (pass 1 alone: 1.43 / 1.46 / 1.01), its bright surface at
//    0.91 / 0.94, 1.01 / 1.04; Neptune 204515's disc 1.01; the 2023-02-11 / 2026-01-30 Jupiters' skies 0.99 / 1.16. With Gaussian noise
//    of 1.5 / 3 DN added to every raw sample of the 2023-02-02 Moon (8-bit, a raw mosaic: v0 = (DN^2 +
//    1/12) / 255^2 / 4), the model rises by 1.01-1.15 / 0.98-1.09 v0 over luma 0.03-0.6 (pass 1 alone:
//    0.95-1.15 / 0.98-1.02) and less where the data clip: 0.71-0.77 / 0.80-0.92 from 0.7 (saturation),
//    0.88 at the 3-DN sky (black);
//  - the line: Theil-Sen start (the median of the slopes between bins), then weighted least squares
//    iterated with Tukey's biweight (kTukey) on each bin's relative error (sampling error plus kFitSlack);
//    a, b >= 0; bins within kClipSigmas of a sample's noise of black (a sample of a K-sample luma pixel has
//    K times its variance: K = LumaSites) are left out (a sky 2 noise sd above black reads 0.960 of the
//    line, inside the fit's slack); and a slope only where the bins' levels spread over more than
//    kSlopeSpread level-noise sd -- within a narrow range, a sky alone, the observed level is mostly noise
//    (the 2026-01-30 Saturn AVI 203627: its 4 sky bins clear of black span 0.003 of level; fitted with a
//    slope they give a = 0, b 6.6e-4 -- all of the sky's noise called shot noise, half of it at half the
//    level -- and without one a = 1.64e-5, 1.04 of their own variance; test/calibration/skynoise.cpp).
// Measured on synthetic still frames (test "the capture's noise model ..."): white noise 0.01 over a
// textured disc, a 0.994e-4; read 0.005 plus shot noise at gains 500 / 3000, within 5% of the truth at
// levels 0.05-0.55; a sky 2 noise sd above black, its own 0.966e-4 against the true 0.960e-4; a static
// colour mosaic, the noise alone; 8-bit noise of 1 / 3 DN, 0.984 / 1.002 of the frames' own variance.
struct NoiseModel {
    double a = 0, b = 0;  // the line; both 0: no measurable noise
    // The line's own (co)variance from the fit.
    double varA = 0, varB = 0, covAB = 0;
    // True when measured on the very frame it is applied to (FromSkyNoise): its error then differs from
    // frame to frame, and qualitySigma carries it. A capture's model is shared by all its frames: its
    // error moves every frame's quality alike, does not scatter them, and is left out of qualitySigma.
    bool ownFrame = false;
    int64_t responses = 0;  // responses read (per pass)
    int frames = 0;         // frame triples pooled
    // The curve, per level bin (ascending level): its level and the variance there (smoothed over
    // neighbouring bins up to kKnotResponses responses, then times pass 2's ratio), and whether it lies
    // under the line by more than kClipSigmas of its sd, or the bin is near black (a clip).
    struct Knot {
        double level = 0, variance = 0;
        bool clipped = false;
    };
    std::vector<Knot> knots;

    // Not measured at all (FitNoiseModel of a capture with no frame triple to read: under 4 frames, or
    // frames under 5 x 5) -- unlike a measured zero, which says the capture shows no noise. Measures given
    // an unmeasured model take each frame's own sky noise instead (MeasureFrame, MeasureAll; RegionMeasurer
    // the frame's texture-blind reading, Regions.h): with the zero model in its place, a 1-3 frame capture's
    // quality read 1.164x its noise-free value and qualitySigma 0.
    bool unmeasured = false;
    bool Measured() const { return !unmeasured; }
    static NoiseModel Unmeasured() {
        NoiseModel m;
        m.unmeasured = true;
        return m;
    }

    bool Zero() const { return !(a > 0 || b > 0); }
    double Line(double t) const { return std::max(0.0, a + b * t); }
    // Variance of a pixel whose observed 3 x 3 mean level is m: the knots' curve, linear between them, the
    // outermost knot's value held beyond it; the line when there are no knots.
    double VarianceAtMean(double m) const;
    // Variance of the line's value at level t (the estimate's own error): varA + t^2 varB + 2 t covAB.
    double LineVariance(double t) const { return varA + t * t * varB + 2 * t * covAB; }
    bool AnyClipped() const;
    // VarianceAtMean through a table of kNoiseTable points over [0, 1] (linear between them; clamped to
    // the ends): the per-pixel form. Built by Tabulate(), which FitNoise and FromSkyNoise call; call it
    // again after changing the model.
    double Lookup(double m) const {
        if (table_.empty()) return VarianceAtMean(m);
        const double u = std::clamp(m, 0.0, 1.0) * (table_.size() - 1);
        const size_t i = std::min(static_cast<size_t>(u), table_.size() - 2);
        const double f = u - static_cast<double>(i);
        return table_[i] + f * (table_[i + 1] - table_[i]);
    }
    void Tabulate();

    // The one-image model: one level, s^2 from the sky (EstimatePixelNoise, Ruling P19), b = 0, no knots,
    // ownFrame (its variance V = varSigma2). One image carries no time, and its sky is the only place its
    // noise is free of texture.
    static NoiseModel FromSkyNoise(const NoiseEstimate& sky);

private:
    std::vector<double> table_;
};

constexpr int kNoiseTable = 4097;
// Pass 1.
constexpr int kFineBins = 1024;
constexpr int kMinBinResponses = 400;
constexpr double kKnotResponses = 20000;
// Pass 2.
constexpr int kNoiseTile = 16;           // a tile: kNoiseTile x kNoiseTile responses
constexpr int kBinTiles = 64;            // tiles per level bin
constexpr double kNoiseQuantile = 0.5;  // each bin read at this quantile of its tiles' ratios
constexpr double kMaxKurtosis = 12;      // beyond: no chi^2, the bin's mean ratio
constexpr double kTileSpread = 0.25;     // a tile of one level: its levels' sd within this share of their mean
constexpr double kKnotTiles = 640;       // the knots' smoothing, in tiles ...
// Both.
constexpr double kFitSlack = 0.05;
constexpr double kTukey = 4.685;
constexpr double kClipSigmas = 3;
constexpr double kSlopeSpread = 4;
// The work: responses per frame triple at most (every NoiseStep-th pixel), responses in all, triples per
// run (consecutive, so each frame of a run is read and prepared once), and passes over the runs.
constexpr int kNoiseResponsesPerTriple = 1 << 20;
constexpr int64_t kNoiseResponses = int64_t{1} << 25;
constexpr int kNoiseRun = 32;
constexpr int kNoisePasses = 2;
// At most this many triples per pass whatever the frame size: a 24 x 24-luma capture of 50 000 frames
// would otherwise be read twice in full (2^25 responses of 400 a triple) for a curve 2048 triples fix.
constexpr int64_t kNoiseMaxTriples = 2048;

// A pass's samples. Add any number of triples, Merge in a fixed order (sums, appended tiles), so a fit is
// the same for any thread count.
struct NoiseSamples {
    // Pass 1, per fine level bin: responses, the sum of their levels, of r^2 / 216, of its square.
    std::vector<double> n, level, r2, r4;
    // Pass 2, per tile: its responses' mean level, mean standardised square (r^2 / 216 over pass 1's
    // variance at the response's level), that square's kurtosis (mean of its square over its mean squared;
    // kMaxKurtosis for a tile of zeros), and the responses.
    struct Tile {
        float level = 0, ratio = 0, kurtosis = 0, n = 0;
    };
    std::vector<Tile> tiles;
    int frames = 0;
    // Overlapping responses are correlated: a bin's sampling variance is its independent value times
    // `overlap` (space and time), a tile's times `tileOverlap` (space). AddTriple sets them.
    double overlap = 1, tileOverlap = 1;
    NoiseSamples();
    void Merge(const NoiseSamples& other);
};

// The sampling step (both axes) that keeps a frame's responses within kNoiseResponsesPerTriple.
int NoiseStep(int width, int height);
// One frame prepared, at the sampled centres only (x = 2 + i step, y = 2 + j step, i < nx, j < ny; step =
// NoiseStep): its spatial responses ([1 -2 1] x [1 -2 1]) and levels (the 3 x 3 mean), nx x ny row-major
// (8 bytes per sampled centre, at most kNoiseResponsesPerTriple of them). Empty under 5 x 5.
// std::invalid_argument for an empty or non-1-channel image.
struct NoisePlane {
    int w = 0, h = 0, step = 1, nx = 0, ny = 0;
    std::vector<float> response, level;
    explicit NoisePlane(const ser::FloatImage& luma);
};
// Adds the responses of three consecutive frames (the same size; std::invalid_argument otherwise).
// `levels`: a frame outside the triple (the next one), whose level at a pixel is independent of the
// triple's noise there. `first` null: pass 1 (the fine bins); else pass 2 (the tiles, standardised by it).
void AddTriple(const NoisePlane& previous, const NoisePlane& middle, const NoisePlane& next, const NoisePlane& levels,
               NoiseSamples& out, const NoiseModel* first = nullptr);
// sites: samples averaged per luma pixel (LumaSites), each clipping on its own. Pass 1's model, then the
// final one from both passes' samples (pass 1's model when pass 2 holds no tile).
NoiseModel FitNoise(const NoiseSamples& samples, int sites);
NoiseModel FitNoise(const NoiseSamples& first, const NoiseSamples& second, int sites);
// Runs of consecutive triples a capture's model reads: as many triples as kNoiseResponses allows, at most
// kNoiseMaxTriples (all of them in a short or small capture), in runs of up to kNoiseRun spread evenly over
// the capture -- back to back, each triple once, when all are read. None (the model unmeasured) under 4
// frames or for frames under 5 x 5 luma, which hold no response.
struct NoiseRun {
    int64_t first = 0, triples = 0;  // frames first .. first + triples + 2
};
std::vector<NoiseRun> NoiseRuns(const ser::FileInfo& info);
// The model of consecutive frames (tests, tools): every triple (0, 1, 2), (1, 2, 3), ..., each with the
// levels of the frame after it. Unmeasured under 4 frames or 5 x 5.
NoiseModel FitNoiseModel(const std::vector<ser::FloatImage>& frames, int sites = 1);
// Samples per luma pixel of a capture: 4 (Bayer, an undeclared mosaic), 3 (RGB), 1 (mono).
int LumaSites(const ser::FileInfo& info);
// The model of a capture, over NoiseRuns(info), read through `factory` as MeasurementLuma (Measure.h) on up
// to `threads` workers (one run at a time each; RunPool, Parallel.h). Each run's samples are merged in plan
// order as soon as the runs before it are in, and a worker claims a run only within 2 x workers of the next
// to merge, so at most that many runs' samples are held: deterministic for any thread count. No run:
// NoiseModel::Unmeasured(). Progress phase "Measuring noise" (done, kNoisePasses x runs: each run is read
// once per pass); cancel -> StackCancelled; a worker's error is rethrown after every thread is joined.
NoiseModel FitNoiseModel(const std::function<std::unique_ptr<ser::FrameSource>()>& factory,
                         const ser::FileInfo& info, int threads, const Progress& progress);

}  // namespace planetary::stack
