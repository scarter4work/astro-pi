#pragma once
#include <cstdint>
#include <memory>
#include <vector>

#include "Align.h"
#include "FrameSource.h"
#include "Measure.h"
#include "Noise.h"
#include "Select.h"

namespace planetary::stack {

// Lucky imaging per region (spec "The engine", steps 3-4): the image is split into regions placed
// automatically on structure; every region measures, in every frame, its own sharpness and its own
// sub-pixel shift (seeing moves and blurs each part of the image differently), ranks its frames and
// keeps its own best ones (RankByDetail, SelectFrames' rule -- no percentage); Accumulate.h stacks
// them and blends the regions seamlessly.
//
// Scale. Everything here is in measurement-luma pixels (MeasurementLuma: half resolution for Bayer
// input and an undeclared mosaic), like FrameStats and Shift; the Accumulator works at full
// resolution and scales by LumaScale(info) (Ruling P3). Shift convention as in Align.h:
// frame(x + dx, y + dy) ~ reference(x, y).
//
// The pipeline (Task 6 drives it; every piece here is I/O-free, Ruling P1):
//  1. layout = PlaceRegions(reference, MeasureFrame(reference, 0, LumaScale(info)), selection.fullFrameTarget,
//                             noise);   // noise: FitNoiseModel of the capture
//  2. for each selected, globally aligned frame (its luma and GlobalAligner shift):
//         FrameRegions m = measurer.Measure(luma, index, shift);   // RegionMeasurer(reference, layout)
//         residual.Observe(luma, m);                               // ResidualScale, Accumulate.h
//  3. plan = PlanRegions(layout, all FrameRegions);
//  4. Accumulator(plan, reference, residual.Sigma(...), info): Add(index, DemosaicHQ(frame)) for every
//     frame plan.Uses(index); Finish().

// ---- Regions --------------------------------------------------------------------------------------

// Region side, in luma px: a disc's regions are about a quarter of its smaller box side across (the
// belts and spots of a planet; seeing distorts patches of that order), a frame-filling surface's an
// eighth of the frame's shorter side; always a multiple of 8 within [kMinRegionSize, kMaxRegionSize].
// 32 px is the smallest window that still measures a sub-0.1 px shift and a quality with a usable
// uncertainty from one noisy frame; 128 px keeps a 1080p mono surface at ~15 x 8 regions.
constexpr int kRegionsAcrossDisc = 4;
constexpr int kRegionsAcrossFrame = 8;
constexpr int kMinRegionSize = 32;
constexpr int kMaxRegionSize = 128;
// A region holds structure when one frame can be aligned on it to within kMaxShiftNoise luma px:
// the predicted 1-sigma error of a local shift, shiftNoise = 2 s_f / sqrt(sum over the window of
// |grad reference|^2 less the reference's own noise share) (least squares with white noise s_f in the
// frame and the reference's gradient as the regressor, both axes together). s_f is one frame's pixel
// noise: the reference's over the target, measured texture-blind (below), times sqrt(kReferenceFrames)
// (the reference averages that many frames), floored at kMinFrameNoise. Regions without structure
// (sky, a featureless disc interior) are not placed; the global stack covers them (Accumulate.h,
// "fallback"). The criterion is on the window's total gradient energy, not its per-pixel density, so a
// low-contrast belt over a 32-px window still qualifies (planet interiors carry the lucky detail).
//
// The reference's noise for placement is its own, read texture-blind (RegionNoise below, over the
// target's box) -- the reading kMaxShiftNoise was set with. The capture's noise model (Noise.h) is not
// used here: a reference is an average of interpolated frames, whose noise the model does not describe,
// and with the model's frame noise in its place the synthetic Moon of test "a dark lunar limb ..." placed
// far fewer regions than this reading does (the reference's texture-blind reading, times
// sqrt(kReferenceFrames), sits well under a frame's true noise: interpolation smooths the reference's
// pixel-scale noise). Placing by level would move every layout; the dark-limb windows of the 2026-01-30
// Moon 193142 are placed either way (Ruling P26 concerns their quality, below).
//
// The regions' quality (RegionMeasurer) takes the capture's noise model at each pixel's level
// (RegionLayout::noise, Ruling P26). Before, it took one level per frame, read texture-blind over all the
// regions' windows (Ruling P25): on the 2026-01-30 Moon 193142 that level was the bright surface's, and
// its dark limb regions, whose own noise is a fraction of it, measured quality 0 in nearly every frame and
// kept none (at 851ba41b, stack-region-probe: 3 of its 324 regions kept no frame, 3 read as noise only at a
// median q / qualitySigma of 0.02-1.8).
constexpr double kMaxShiftNoise = 0.1;
constexpr double kMinFrameNoise = 1e-3;

// One region: its window on the luma image (half-open, clipped to the image) and its grid node.
struct Region {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    // Predicted 1-sigma error of one frame's local shift here (luma px, <= kMaxShiftNoise): smaller =
    // more structure to align and rank on.
    double shiftNoise = 0;
    int gx = 0, gy = 0;  // node on the layout grid
};

// The regions' blend mask (E1, Task 9 Investigations 1-2). A region's own frame choice buys detail only
// where there is detail; over sky it buys only noise -- sqrt(fallback frames / region frames) of it -- and,
// where the sky's glow changes from frame to frame, a level of its own. On the 2023-09-29 Saturn the region
// over the ring tip kept 160 of 3670 frames and put a square of speckle in the sky of its window (high-pass
// RMS 1.445x the global-only stack's on that 32 px tile, 51abe702); on the 2023-02-12 Moon sky regions that
// kept few frames left dark patches in the glow around the limb. So for a disc every region weight is
// multiplied by a mask m from the reference -- in the stack's blend and in its warp (Accumulate.h) -- and
// the fallback takes 1 - sum of the masked weights: the frames' weights still sum to 1.
//  - Seeds: pixels whose 3 x 3 reference mean stands above the sky (the median of the reference outside
//    the target's box) by max(kMaskSigmas x a frame's noise at the sky's level -- the capture's model
//    (Noise.h); the reference's own reading, EstimatePixelNoise, when unmeasured, and never below it -- ,
//    kMaskContrast x the target's mean level over the sky (FrameStats::brightness)).
//  - m = 1 within kMaskGrow luma px of a seed, a cosine taper to 0 over the next kMaskTaper px.
// Measured (stack-region-probe on local copies, BGGR; per 32 px sky tile against the global-only stack of
// the same frames; E1 report): the Saturn's worst sky tile 1.020 (all 1199 sky tiles: median 1.008), its
// faint arcs gone with the warp masked too; the Moon's darkest sky tile against its 5 x 5 neighbours 0.958,
// the global-only stack's own darkest (0.937 with the sigma term alone: the glow there stands above 2 frame
// sd, so kMaskContrast is what keeps it out), 11 tiles under 0.97 (global 15). Where m is 1 the stack is
// unchanged (|difference| <= 2.1e-6, float rounding, on both). The choices:
//  - kMaskSigmas 2: k 1 and 2 gave the same Saturn sky (worst tile 1.020) and one connected mask (no seed
//    in the sky's noise: the reference averages up to kReferenceFrames frames and is smoothed 3 x 3 here,
//    so 2 frame sd is many of its own); 2 is kept for references of few frames;
//  - kMaskContrast 0.02: 0.02, 0.05 and 0.1 gave the Moon the same darkest tile (0.958) and left the
//    Saturn's mask as the sigma term makes it (8.6% / 8.4% of the grid's extent at 0.02 / 0.05);
//  - kMaskGrow 4 and kMaskTaper 6 (luma px): grow 2 / 4 / 8 with taper 4 / 6 / 8 gave the same Saturn sky
//    (1.020) and the same dip just outside the Moon's mask (5.7-6.4e-4 below the sky 40 px out, 3% of the
//    sky: the region stacks' darker glow, which the unmasked blend had over the whole window); the red
//    test's synthetic disc reads 1.057 (bar 1.25), and 0.98-1.17 over k 1-3, grow 2-8, taper 4-8 with the
//    mask on the blend alone. On the 2024-02-01 Moon (also a disc) the sky tiles the mask leaves at 0 read
//    at most 1.002 (1.897 and 15 tiles over 1.15 before).
//    Not tied to a measured limb blur: over that range nothing measured moved.
// Not for a frame-filling target (flatEdges): there is no sky outside its box to define the level by, and
// dark surface (maria, shadow) must keep its regions.
constexpr double kMaskSigmas = 2.0;
constexpr double kMaskContrast = 0.02;
constexpr double kMaskGrow = 4.0;
constexpr double kMaskTaper = 6.0;
struct BlendMask {
    int x0 = 0, y0 = 0, width = 0, height = 0;  // the luma rectangle it covers
    // width x height, row-major; null: 1 everywhere. Shared, so the copies of a layout share one mask.
    std::shared_ptr<const std::vector<float>> m;
    bool Empty() const { return !m; }
    // The mask at luma coordinate (u, v) (pixel centres at integers): bilinear; 1 when empty; 0 outside.
    double At(double u, double v) const;
};

// Regions sit on a regular grid of square windows of side `size` with half overlap: node (i, j)'s
// window starts at (originX + i * step, originY + j * step), step = size / 2. Blending weight of node i
// along x at luma coordinate u (pixel centres at integers): w_i(u) = sin^2(pi (u + 0.5 - o_i) / size)
// over its window [o_i - 0.5, o_i + size - 0.5], 0 outside; two neighbouring nodes' weights sum to
// exactly 1 (sin^2 + cos^2), so the 2-D weights w_i(u) w_j(v) of the four nodes around a point sum
// to 1 inside the grid (a partition of unity: a flat field stays flat, no seams). With flatEdges (a
// frame-filling target) the first and last node on each axis keep weight 1 from their centre out to
// the image edge and beyond, so the whole frame is covered; without (a disc) the grid spans the
// target's box plus a margin, every node's weight tapers to 0 outward, and the fallback stack takes
// over smoothly in the sky.
struct RegionLayout {
    int width = 0, height = 0;    // the luma image
    int size = 0, step = 0;
    int originX = 0, originY = 0;
    int nx = 0, ny = 0;
    bool flatEdges = false;
    std::vector<Region> regions;  // the nodes that hold structure, in row-major node order
    std::vector<int> node;        // nx * ny: index into regions, -1 where no region was placed
    // The capture's noise model (Noise.h), as PlaceRegions was given it: every region quality's noise.
    NoiseModel noise;
    // The reference's texture-blind reading is taken on 2 x 2 bins, not the pixels (RegionNoiseOnBins over
    // the target's box, decided once by PlaceRegions; Ruling P25): placement's, and a frame's own reading
    // when `noise` is unmeasured (RegionMeasurer).
    bool noiseOnBins = false;
    // Every region weight is multiplied by it, in the stack's blend and its warp (Accumulate.h); PlaceRegions
    // sets it for a disc (RegionBlendMask), empty for a frame-filling target.
    BlendMask mask;
};
// The mask PlaceRegions gives a disc layout (lay: its geometry and noise model). Empty for a frame-filling
// layout, one without regions, or a reference without sky outside refStats' box.
BlendMask RegionBlendMask(const ser::FloatImage& reference, const FrameStats& refStats, const RegionLayout& lay);

// The reference's pixel noise for placement, and a frame's own for its region quality when the capture's
// model is unmeasured (Ruling P25; every frame's until Ruling P26), blind to texture: MaskNoise -- the
// trimmed second moment of the [1 -2 1] x [1 -2 1] responses, Regions.cpp -- on the pixels of [x0, x1) x
// [y0, y1) (clipped to the image), or with onBins twice that on their 2 x 2 bins (white noise s gives s / 2
// in a bin). Each reading misreads one texture as noise: a raw colour mosaic read as mono carries its
// pattern at Nyquist in every frame, which the pixels see and a bin (one cell of the pattern) cancels;
// texture at 4-8 px, coarse for the pixels, is fine for the bins. The capture's own data says which applies
// (RegionNoiseOnBins, on the reference) -- not the smaller of the two, which reads s^2 0.921 / 0.958 / 0.978
// / 0.990 / 0.996 of the truth on white noise over 32^2 / 64^2 / 128^2 / 256^2 / 512^2, taking whichever
// fluctuated low. Each reading alone (white noise, 32^2-512^2; test "RegionNoise is unbiased ..."): pixels
// 1.000-1.006 of the truth, bins 0.997-1.012 from 48^2 and 1.024 at 32^2 (16 x 16 bins, 196 responses: the
// trimmed moment's small-sample excess) (test/calibration/regionnoise.cpp).
// varSigma2 = kRegionNoiseVariance s^4 / (pixels measured: the rectangle's, a quarter of it for
// bins): measured Var(s^2) x pixels / s^4 = 8.6-11.6 on pixels and 8.8-12.3 on bins (bins counted),
// 32^2 to 1024^2: 0.71-1.23 of the formula, 1.23 the 32^2 bins (the 21.8 s^4 / area it replaces was ~2x
// high on pixels and ~2x low on bins). 0 for a rectangle under 3 x 3 (6 x 6 for bins).
constexpr double kRegionNoiseVariance = 10.0;
NoiseEstimate RegionNoise(const ser::FloatImage& im, int x0, int y0, int x1, int y1, bool onBins);
// Bins when the reference's pixel reading exceeds kRegionNoiseMosaicRatio times its bin reading: a
// mosaic. Measured 2026-10-22 over the regions' extent of the references of the 2026-01-30 Jupiter
// 195830 (AVI and SER), Neptune 204515 and Moon 193142, the 2023-02-02 Moon, the 2023-02-11 Jupiter and
// the 2022-09-17 Jupiter 225418 (all measured on 2 x 2 luma, so no mosaic reaches the region stage):
// pixels / bins 0.21-0.65 -- texture raises the bins' reading -- while a StackSynth disc as a
// mono-labelled mosaic (sites R 1 / G 0.5 / B 0.35) reads 2.19 in one frame at noise 0.01 (0.93
// without the mosaic).
constexpr double kRegionNoiseMosaicRatio = 1.5;
bool RegionNoiseOnBins(const ser::FloatImage& reference, int x0, int y0, int x1, int y1);

// The (at most two) nodes covering coordinate u along one axis, and their weights (0 for a node
// outside the grid; i0 may then be -1 or nx - 1 with w1 = 0 and so on). Shared by the measurement,
// residual and stacking stages so they blend identically.
struct AxisWeight {
    int i0 = -1;           // first node; the second is i0 + 1
    double w0 = 0, w1 = 0;
};
AxisWeight AxisWeightAt(const RegionLayout& layout, bool vertical, double u);

// Places the regions on the reference (BuildReference's luma image; refStats = MeasureFrame of it);
// noise: the capture's model (FitNoiseModel), kept in the layout for the regions' quality.
// fullFrame (Selection::fullFrameTarget): the grid covers the whole frame with flat edges; else it
// covers the target's box grown by a margin of step / 2 (seeing moves the limb; the box is already
// grown by a detection block), so the whole disc lies where the weights sum to 1. A node gets a
// region when its window, clipped to the image, keeps at least a quarter of its area, touches the
// target's box (disc), and holds structure (kMaxShiftNoise). No region at all is a valid layout
// (a featureless target): everything is then stacked by the fallback.
// Deterministic. Throws std::invalid_argument for an empty or non-1-channel image or a sample count
// that does not match; for a disc, also when refStats has no target.
RegionLayout PlaceRegions(const ser::FloatImage& reference, const FrameStats& refStats, bool fullFrame,
                          const NoiseModel& noise);
// The plan's two-argument form: fullFrame decided from the reference itself (FillsFrame, or true
// when it has no target), as GlobalShift does. Prefer the three-argument form with the selection's
// capture-level verdict.
RegionLayout PlaceRegions(const ser::FloatImage& reference, const FrameStats& refStats, const NoiseModel& noise);

// ---- Per-frame measurement (Ruling P1) ------------------------------------------------------------

// Largest local shift (luma px, each axis) a region may report on top of the global shift. Global
// alignment keeps half-frame tears of 4-6 px (Align.h, kAlignRelative); a region inside the displaced
// part must absorb them, so the search reaches 8 px with margin.
constexpr double kMaxLocalShift = 12.0;
// Reach of the exhaustive local search around the global shift (luma px, each axis, at least).
constexpr double kLocalSearch = 8.0;
// Pyramid levels of the local search ([1 3 3 1] / 8 low pass, then every other pixel).
constexpr int kLocalLevels = 3;

struct RegionMeasure {
    // Local shift on top of the global one: frame(x + global + d) ~ reference(x) over the window.
    float dx = 0, dy = 0;
    // Normalised cross-correlation of the aligned window with the reference's (mean-free), [-1, 1]:
    // whether this part of the frame shows the reference's scene at the found shift.
    float confidence = 0;
    // RegionQuality over the frame's pixels at the window's position rounded to the found shift
    // (unresampled: interpolation would blur by an amount that depends on the fractional shift), with
    // the window mean as background and the capture's noise model (RegionLayout::noise). 0 = no measurable
    // detail. Over a window partly outside the frame, the part inside.
    float quality = 0, qualitySigma = 0;
    float quality2Sigma = 0;  // sd of quality^2 (FrameStats::quality2Sigma)
    // The search and refinement succeeded (see Method) with the window at least half inside the frame.
    bool aligned = false;
};

struct FrameRegions {
    int64_t frameIndex = 0;
    Shift global;
    // Brightness of the frame relative to the reference over all region windows (ratio of sums,
    // aligned): haze or a thin cloud dims a frame, and frames kept by different regions must be
    // brought to one brightness or the regions would differ (Accumulator divides by it). 1 when
    // there are no regions. A sky pedestal p shared by all frames is not removed: a frame dimmed to a
    // of the reference is restored to within (1 - a) p of it.
    double gain = 1;
    std::vector<RegionMeasure> regions;  // one per layout.regions
};

// Prepared measurement for one reference and layout: the reference windows' pyramids and gradients
// are computed once. Immutable after construction; Measure() may run on many threads at once.
//
// Method per region:
//  1. Search: on the coarsest pyramid level where the window is still >= 12 px across (level 1 for
//     32-px regions, 2 for 64 and up), the normalised cross-correlation with the reference window at
//     every integer offset within kLocalSearch luma px of the global shift (over the part of the
//     window inside the frame); the best is taken (it must reach 0.2). Exhaustive, so a large shift
//     or a repetitive texture cannot trap it the way a gradient descent from 0 can.
//  2. Refinement (inverse-compositional Lucas-Kanade for a translation) from that level down to full
//     resolution: the frame is sampled over the window at the current shift (global + local,
//     Catmull-Rom), its mean removed and a gain fitted to the reference window's (so brightness and
//     blur-induced contrast loss do not bias the shift); the update solves
//     (H + lambda I) delta = sum grad T (F - a T) / a, H the reference window's gradient matrix and
//     lambda 2% of its mean eigenvalue (a straight limb segment fixes only the shift across it; along
//     it the update stays near the search's answer instead of chasing noise). Up to 3 / 3 / 2
//     iterations on levels 2 / 1 / 0, stopping below 0.01 luma px. Only window pixels whose
//     interpolation support lies inside the frame count (a window at the frame edge moved by the drift
//     would otherwise be pulled toward the clamped edge); full-resolution windows of 96 px and more
//     use every other pixel in each axis (still >= 48 x 48 points). Fails (aligned = false) when the
//     shift leaves kMaxLocalShift, the window has mostly left the frame, or it loses all correlation
//     (fitted gain under 0.05).
//  3. Confidence, quality and the gain sums from the final full-resolution window.
// Accuracy (unit tests, 32-px windows, frame noise 0.01): shift error rms < 0.05 luma px (noise-
// limited: ~s sqrt(2 / sum |grad T|^2)); an 8 px error of the global shift recovered to 0.08; inside
// the displaced part of a torn frame (4-6 px) every window reports the tear (0.1).
// Cost (measured 2026-10-21, per frame on each of 16 busy threads): 0.4 ms per 240 x 180 Jupiter luma
// frame (9 regions); 47 ms per 1920 x 1080 mono lunar frame (324 regions of 128 px, the refinement
// about half of it); a 1080p Bayer capture's 960 x 540 luma is about a quarter of that.
class RegionMeasurer {
public:
    // Throws std::invalid_argument when the reference is not a non-empty 1-channel image of the
    // layout's size.
    RegionMeasurer(const ser::FloatImage& reference, const RegionLayout& layout);
    // frameLuma: MeasurementLuma of the frame (the reference's size, std::invalid_argument otherwise);
    // global: its GlobalAligner shift. Deterministic: the same inputs give bit-identical results.
    FrameRegions Measure(const ser::FloatImage& frameLuma, int64_t frameIndex, const Shift& global) const;
    const RegionLayout& Layout() const { return layout_; }

private:
    struct Window {
        int x0, y0, w, h;          // window on this level (apron of 1 px around it in `t`)
        std::vector<float> t;      // (w + 2) x (h + 2) reference samples, apron included
        std::vector<float> tm;     // w x h, the window less its mean
        double mean = 0, energy = 0;          // mean and sum (T - mean)^2 over the window
        double hxx = 0, hxy = 0, hyy = 0;     // gradient matrix
    };
    RegionLayout layout_;
    std::vector<std::vector<Window>> windows_;  // [region][level]
};

// The plan's one-call form (Ruling P1): RegionMeasurer(reference, layout).Measure(frameLuma, 0, global).
// For many frames prepare a RegionMeasurer once.
FrameRegions MeasureRegions(const ser::FloatImage& reference, const RegionLayout& layout,
                            const ser::FloatImage& frameLuma, const Shift& global);

// ---- Per-region selection --------------------------------------------------------------------------

// Per-region outcome (Ruling P4: Task 6 judges focus and seeing for frame-filling targets and small
// discs from it).
struct RegionDetail {
    int64_t candidates = 0;   // frames aligned at this region (confidence >= cut) with detail
    int64_t kept = 0;         // frames stacked here
    double confidenceCut = 0; // kAlignRelative x the median confidence of the aligned frames
    // "The best" quality of this region (RankByDetail's qRef: what the region's sharpest 1% truly
    // reach, noise deconvolved), in 1/luma px.
    double bestQuality = 0;
    // One frame's measurement uncertainty at that level: the median qualitySigma of the kept frames.
    // Not the uncertainty of bestQuality itself (a deconvolved quantile has none in closed form); a
    // floor judged against bestQuality should allow a few of these.
    double keptQualitySigma = 0;
    // Absolute texture scale: 1 / bestQuality, luma px (times LumaScale for sensor px). The region's
    // RMS spatial frequency inverted: for a Gaussian-blurred white texture it is about sqrt(2) x the
    // blur sigma, so it grows with defocus or seeing whatever the target's brightness.
    double detailScale = 0;
    // Ruling P27 (PlanRegions): no detail to rank above the measurement noise (kept 0: the fallback's
    // plain mean).
    bool noiseOnly = false;
};

// A region's detail is real when its median candidate's quality is at least this many of its own
// qualitySigma (Ruling P27; 3 until Ruling P26). Measured, median q / qualitySigma over a region's
// candidates (q > 0):
//  - pure noise (white 0.01 on a flat level, 32 / 128 px windows, 200 repeats, the true noise as the
//    model; test/calibration/purer.cpp): a median of 2.9-3.2 whatever the count, p99 7.9-8.6 / 5.1-5.5 /
//    4.2-4.3 / 3.7-3.8 / 3.3 at 10 / 30 / 64 / 150 / 400 frames (0.42 of them candidates), the 400-frame
//    maximum 3.4-3.5. P26's qualitySigma is the scatter's own (Measure.h); the one before overstated it at
//    low SNR, so 3 sat well above noise then and at its median now;
//  - real regions (Ruling P26's model; stack-region-probe): the least detailed of the five test captures
//    with regions reads 10.31 (the 2026-01-30 Moon 193142's region at 160, 288: saturated, luma 0.945, its
//    alignment match a low 0.85, the model 1.30x its own noise (calibration/winnoise.cpp); it keeps all
//    2044 frames), then 17.6 (Neptune 204515, 93 frames) and 60.8 (the 2023-02-02 Moon) -- the Moon
//    193142's dark limb among them, whose regions the one sky level put at 0.02-1.8 (851ba41b, above);
//  - synthetic textured regions under seeing that changes from frame to frame (test "A low-SNR region
//    with real seeing variation ...": half contrast, noise 0.01, 150 frames; calibration/lowsnr.cpp):
//    3.71-11.97, 4 of 165 under 4 (they take the plain mean).
// 4 is over pure noise's p99 from ~150 frames and its 400-frame maximum, and under every real region by
// 2.5x. A fixed gate, so pure noise passes it more often with fewer candidates (2000 regions each, 32 / 64
// px windows, exactly C candidates; calibration/purer2.cpp): 14.9 / 17.2% at C = 10, 1.4 / 2.6% at 30, none
// at 100 or 300. A noise-only region that passes is ranked, and the deconvolution keeps nearly every frame
// of a spread that is noise (Neptune 204515: 92-93 of 93); under kMinStackFrames candidates a region takes
// the plain mean anyway. A count-dependent gate would trade that for real low-SNR regions of short
// captures: no single threshold separates the two there.
constexpr double kRegionDetailSigmas = 4;

struct RegionPlan {
    RegionLayout layout;
    std::vector<int64_t> frames;  // frame indices measured, ascending
    // Per frame: stacked at all -- kept by some region, or (when no region keeps any frame) every
    // measured frame. These are the fallback stack's frames (where regions do not cover); a frame no
    // region keeps would add nothing anywhere else, so it is not read, demosaiced or added at all.
    std::vector<uint8_t> stacked;
    int64_t stackedCount = 0;
    std::vector<Shift> global;    // per frame
    std::vector<float> gain;      // per frame (FrameRegions::gain)
    // Per frame and region, at [frame * regions + region]:
    std::vector<float> dx, dy;    // local shift
    std::vector<uint8_t> flags;   // kShiftUsable | kKept
    std::vector<RegionDetail> detail;  // per region
    // The region with the highest bestQuality (-1 without regions), for a one-line verdict.
    int bestRegion = -1;
    static constexpr uint8_t kShiftUsable = 1;  // aligned and confidence >= the region's cut
    static constexpr uint8_t kKept = 2;         // stacked in this region

    size_t RegionCount() const { return layout.regions.size(); }
    // Position of a frame index in `frames`, or -1.
    int64_t Find(int64_t frameIndex) const;
    // Whether the stack uses this frame at all (see `stacked`); Task 6 reads only these in pass B.
    bool Uses(int64_t frameIndex) const {
        const int64_t p = Find(frameIndex);
        return p >= 0 && stacked[static_cast<size_t>(p)] != 0;
    }
};

// Builds the per-region choice from every measured frame (any order; duplicates of a frame index are
// an error -> std::invalid_argument, as are measurements whose region count differs from the layout).
// Per region: frames aligned there with confidence >= kAlignRelative x the region's median confidence
// (relative, like AlignmentCut: a tear or a foreign object costs a frame its match locally) and
// quality > 0 are the candidates; RankByDetail(quality, qualitySigma, quality2Sigma, need, floor) over
// them, in frame order, with SelectFrames' need = max(1, min(kMinStackFrames, ceil(n / 2))) and floor =
// max(need, ceil(sqrt(n))), keeps the region's own sharpest frames: the count follows the region's
// quality distribution -- few where the region's sharpness comes and goes with the seeing, most where
// it is steady (measured 2026-10-21: the steady 2026-01-30 Jupiter keeps 260-300 of 300 per region,
// spread +-3%; the same night's Moon a median 199 of 300). Deterministic and order-independent.
// Ruling P27, before the ranking: a region with fewer than kMinStackFrames candidates, or whose median
// candidate's quality is under kRegionDetailSigmas of its qualitySigma, keeps no frame (noiseOnly: the
// fallback's plain mean of the stacked frames -- ranked, its noise-floor "detail" kept a handful, once
// a single frame of 2102 on the Moon 193142, sqrt(2102) times the noise; and one or two candidates out
// of many aligned frames, or after alignment collapsed there, are the noise's tail, whatever their
// ratio). That includes every region of a whole capture of 1-4 frames, which SelectFrames accepts:
// ranking 2-4 frames per region gains nothing, so such a capture is the plain mean. A region whose
// detail is real but whose spread is noise is left to the ranking: step 6's deconvolution keeps nearly
// everything there (Neptune 204515: 92-93 of 93 per region). An override that kept every aligned frame
// when the spread was within qualitySigma discarded real lucky frames at low SNR, where qualitySigma
// then overstated q's scatter at low SNR (before Ruling P26; Measure.h "qualitySigma").
// threads: regions are ranked on that many threads (< 1 counts as 1); the result is the same for any count.
RegionPlan PlanRegions(const RegionLayout& layout, const std::vector<FrameRegions>& measured, int threads = 1);

}  // namespace planetary::stack
