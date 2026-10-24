#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "FrameSource.h"
#include "Regions.h"

namespace planetary::stack {

// Stacking (spec "The engine", steps 3 and 5): every frame the plan uses is warped onto the
// reference's grid once, at full resolution, and added with a per-pixel weight that blends the
// regions' own frame choices; outliers (a satellite or plane trail, a bird, a cosmic ray, a hot pixel
// that moves with the drift) are rejected sample by sample against the reference.
//
// The blend. Region r keeps K_r frames; its stack is their mean S_r; the image is sum_r w_r S_r where
// w_r = m(p) x the layout's sin^2 weights (those a partition of unity; m the layout's blend mask,
// RegionLayout::mask: 1 over the target and its limb, 0 over the sky, Regions.h), plus the fallback stack --
// every planned frame, equally -- with weight 1 - sum_r w_r where regions do not cover (the sky around a
// disc, a region with no kept frame). That sum is linear in the frames, so it is accumulated directly:
// frame f adds alpha_f(p) x its sample at p, alpha_f(p) = sum_{r keeps f} w_r(p) / K_r + (1 - sum_r
// w_r(p)) / K_fallback, and the result is the weighted sum over the weight sum; the frames' alphas still
// sum to 1 everywhere. One sample per pixel per frame, one output-sized buffer -- no per-region stacks
// are held.
//
// The warp. A frame is sampled at p + s (global + local(p)), s = LumaScale: the local shift is the
// regions' shifts blended with the same weights (mask included) over the regions where this frame's shift
// is usable, local(p) = sum w_r c_r d_r / max(sum w_r c_r, kWarpFade) (c_r = 1 when usable) -- smooth, so
// neighbouring regions never tear the image apart, and the global shift over the sky. Over the sky a
// region's local shift is noise (nothing to align on): with the mask on the blend but not the warp, the
// 2023-09-29 Saturn kept faint arc-like streaks in the sky of its ring-tip region, which the global-only
// stack has not; with it on both, none (E1, stack-region-probe, by eye at a +-30% sky stretch). What the
// noisy shifts smear there (the sensor's fixed pattern, an untested guess) was not established.
// alpha and the warp are evaluated exactly every kWarpGrid full-resolution px and bilinearly in
// between (alpha stays a partition of unity over the frames -- interpolation is linear; the warp's
// error is under 0.02 px, its curvature being that of sin^2 blends over >= 32 luma px).
//
// Interpolation: cubic B-spline (each frame prefiltered once, recursive filter with mirrored edges,
// then a 4 x 4 kernel with exact weights -- a table at 1/64 px was tried and moved the stack by up to
// 6e-4, measurably). It reproduces the frame exactly at integer shifts and passes more of the fine
// detail at fractional ones than Catmull-Rom: amplitude kept at a half-pixel shift 0.97 vs
// 0.88 at 0.25 cycles/px, 0.66 vs 0.45 at 0.4 cycles/px (unit test) -- lucky imaging is about that
// detail, and a frame's softening is the worst at half a pixel. A sample whose position falls outside
// the frame counts with weight kOutsideWeight (so an output pixel only the frames' edges reach still
// gets a value; anywhere else such samples move it by at most 1e-6 relative).
//
// Photometry: every sample is divided by the frame's gain (FrameRegions::gain), so frames chosen by
// different regions agree in brightness (no patchwork under passing haze; unit test: a flat field
// whose frames vary 0.7-1.3 in gain, each region keeping its own subset, has no step at any region
// border above 1.5 noise units).
//
// Range: every warped, gain-corrected sample is held to [0, top], top = 1 (full scale) or the frame's largest
// sample where that is above it (DemosaicHQ's range), so the stack -- a weighted mean of such samples -- stays
// in the frames' range (PixInsight images are [0, 1]). Two things leave it, both where the capture clipped
// (field defect D2): the B-spline's interpolation overshoots a clipped plateau -- inherent to an interpolator
// that keeps fine detail (its negative lobes; one frame of a hard step from full scale to 0.3 reads 1.092 at a
// fractional shift, a step up from black -0.072, unit test) -- and the gain divides a saturated sample, which
// holds no measure of how much brighter the scene was, above full scale. The 2026-10-23 Moon at c59e9fd4 read
// 1.00265 in blue at 17 pixels of its brightest craters: at (105, 549) the warped samples reached 1.0066
// before the gain (939 of 2315 above 1) and the gain (0.996-1.000) lifted them further (stack-probe, a scratch
// dump of every frame's sample there). A sample in range is untouched; the stack near a dark edge is not quite:
// the lower bound cuts one lobe of the B-spline's ringing there, so the mean moves up by part of it (the bench's
// four planets moved by at most 2.2e-4 at the commit that added the bound). The bound is still needed beside
// "Clipping" (below), which takes the saturated samples out of the plain mean but not every sample above full
// scale: an unsaturated sample just under it divided by a gain under 1, and the overshoot of a clip outside the
// sample's own cell. Without the upper bound, at fix round 2 of fix/planetary-edge: the unit test below reads
// 1.019, the 2023-02-12 Moon 28 samples above 1, the 2026-10-23 Moon one (stack-probe).
//
// Clipping (Rulings R2, R3): a saturated sample says only that the scene was at least that bright, so its gain-
// corrected value is a lower bound, not a measurement (the 2023-02-12 Moon's frames 0-260 are 1.257x brighter than
// the rest and saturated over its south limb: divided by their gain they read 0.796, and in a plain mean dimmed the
// limb, up to 0.088 at 3123eea8 against this estimate; a synthetic saturated planet's spot, frames at gains 0.85-1.15,
// read 0.956 at its centre, 1.0 now).
//  - Which samples. A channel of a frame's pixel is within reach of a clip where a sensor site it came from reaches
//    kClippedLuma of full scale (Measure.h's level): in mono and RGB the sample itself; in a mosaic the sites of that
//    colour within one pixel, the ones DemosaicHQ reads the channel from (DemosaicHQ keeps each site's raw value as
//    its own channel, so the Accumulator reads them off the frame it is given). A warped sample's channel is
//    saturated when a corner of the cell it lies in is within reach and its own interpolated value, before the
//    gain, reaches kClippedLuma. A sample only next to a clip is a measurement like any other: Ruling R2 dropped
//    every sample within reach, and the stack grew full-scale plateaus (bloat) and dipped where only some frames
//    reached the clip, averaging the dimmer ones (moat; the 2023-02-12 Moon's limb, G 0.916 -> 0.729 at (1353, 889),
//    review C1; unit test "The rim of a saturated crater": RGGB rim error 0.400 at 19ed2e15, 0.050 before R2 and now).
//    Read on the samples' own value, the censoring of R3 as worded -- every sample within reach a lower bound --
//    raised the crater's red rim to the few unsaturated frames' mean (0.074, from 3 of 40 frames).
//  - The estimate, per output channel. The saturated samples are kept apart (sumC_, weightC_) and are present
//    (they count as kept in the never-a-hole share, and against out-of-frame stand-ins). With unsaturated samples
//    present (clean weight over kCleanPresence of the saturated weight), the saturated ones count at max(their own
//    mean, the unsaturated mean). With none, the stack is kClipOutput where one of them saturated inside a frame no
//    brighter than the reference (it proves the scene at full scale on the reference's scale), else their own mean.
//    Held to [0, 1]. Unit tests: two frame groups at gain 1 and 1.25 saturated on one plateau (mono, RGB with blue
//    alone saturated, RGGB) read 1, its rim the scene; a dark outlier in one frame of 20 over a plateau saturated
//    in the others reads 0.98 unrejected and 1 rejected; a plateau saturated in every frame at a frame edge one frame
//    drifted 30 px off reads 1 (review C2: R2 read 0.60 and 0.20 -- the outlier or the out-of-frame mirror alone).
//    The aggregate max(own mean, unsaturated mean) is a streaming stand-in for R3's per-sample max(own, mean); they
//    agree whenever the saturated samples all lie on one side of the unsaturated mean.
//  - Rejection judges a cell on its unsaturated samples, two-sided; a saturated sample is a lower bound at its own
//    gain-corrected value, which counts only upward, above the reference by more than the thresholds: a saturated
//    satellite trail is still an outlier and its unsaturated wings are rejected around it (unit test "A single-
//    frame streak": Bayer leak 0, mono 0.0013, as before R2), frames identical to the reference reject nothing (R2's
//    fixed floor, kClippedLuma x min(1, 1 / gain) for every flagged sample, rejected 2250 of 25 600 cells there).
//    The residual scale is unchanged: a saturated frame there can only widen the tolerance.
//  - A limit: where the scene sits at the clip in some frames only, the saturated samples count at least at the
//    unsaturated mean; the true value can be higher.
//
// Rejection, per luma cell (the s x s full-resolution pixels of one measurement-luma pixel): the
// cell's mean luma (Bayer (R + 2G + B) / 4, RGB (R + G + B) / 3, mono the value) of the warped,
// gain-corrected samples is compared with the reference's luma at that cell -- the same pixel, no
// resampling -- in units of the residual scale there (ResidualScale: the RMS frame-to-reference
// difference at that pixel over the capture, which already holds the pixel noise, the seeing jitter at
// the limb and crater rims, and the blur differences, so sharp structure is not mistaken for an
// outlier). A cell beyond kRejectSigmas is an outlier; rejected are the cells within kRejectGrow of
// one, and those within kRejectReach of one that deviate beyond kGrowSigmas (a trail's wings, the
// interpolation's ringing next to a sharp trail, the demosaic's spread of a defect). Rejection is
// against the reference, not the running stack, so it is the same whichever frame comes first and
// needs no per-pixel history. Measured (unit test, a +0.5 bright or -0.5 dark 1.5 px trail across one
// frame of 40 that every region keeps): what leaks into the stack is 0.001 (mono) / 0 (Bayer),
// against >= 0.0125 unrejected, the same on a frame's row seam (see "Thread safety") as elsewhere,
// and in a border band only a tenth of the frames reach as in the interior; pixels farther than the
// reach are unchanged.
// Limit: a hot pixel that does NOT move on the output (a target perfectly still on the sensor) is in
// the reference too and is not an outlier in time; that needs a sensor-space fix before demosaicing.
//
// Memory (asserted by BytesHeld): per output sample five float sums (the kept value, a channel's unsaturated weight
// from samples with another channel saturated, the saturated value and weight, the rejected value) and per pixel
// two (the kept and the rejected weight) and a flag byte; the reference and residual scale at luma size; the plan
// (frames x regions x 9 bytes plus ~40 bytes per frame) -- 1080p RGB: 143 MB (69 bytes x 1920 x 1080; 66 MB before
// R2). Per Add call: the frame itself (moved in and prefiltered in place), the thread's clip flags (a plane of 1 byte
// per pixel, kept across the thread's Adds, and the frame's clipped sites, 4 bytes each: up to 5 bytes per pixel) and a ring of 2 kRejectReach + 1 luma rows of scratch -- O(one frame) per
// concurrent Add, independent of the number of frames. (Not the O(regions x region size) of
// per-region stacks: the blend above needs none.)
//
// Cost: what a whole capture's stacking pass takes is in stack-probe's phase times ("Stacking"). It is
// kept down by the prefilter in two streaming passes, no libm floor in the sampling, the 3 channels'
// taps as 12 vectorisable lanes, rows through a ring that stays in cache, and float sums -- which
// change the stack only at float rounding (below).
//
// Thread safety: Add() may be called from many threads at once (rows are added under per-kLockRows
// locks, each frame starting at its own row). The sums are float: the order frames arrive in changes
// the result at float rounding (~1e-7 relative per sum, ~1e-5 after thousands of frames) -- the
// concurrency test agrees with a sequential stack to 1e-6.

// Rejection threshold in residual-scale units. The residual scale is measured on luma cells, the
// test runs on luma cells: 6 keeps Gaussian false rejections near 2e-9 per cell; heavier real tails
// (seeing) are inside the scale already.
constexpr double kRejectSigmas = 6.0;
// Hysteresis growth around rejected cells (see "Rejection").
constexpr double kGrowSigmas = 2.0;
constexpr int kRejectGrow = 2;
constexpr int kRejectReach = 4;
// Weight of a sample whose position falls outside the frame (see "Interpolation").
constexpr double kOutsideWeight = 1e-6;
// The stack where every sample a pixel's channel kept was contaminated by a clip and one of them saturated in a frame
// no brighter than the reference (see "Clipping"): full scale, the sensor's clip on the reference's scale (the
// reference is the frames' own scale, at gain 1), within [0, 1].
constexpr float kClipOutput = 1.0f;
// Clean (uncontaminated) weight under this share of a channel's contaminated weight is no clean presence: only
// out-of-frame stand-ins (kOutsideWeight of a frame's blend weight) fall under it, never an in-frame sample of a
// blend that keeps fewer than 1 / kCleanPresence frames (see "Clipping").
constexpr double kCleanPresence = 10 * kOutsideWeight;
// A pixel whose kept weight is under this share of the weight its samples brought (kept + rejected)
// blends its rejected samples back in, linearly up to all of them at 0 kept: rejection never leaves a
// hole (see Finish). A share, not an absolute weight: a border band only a tenth of the frames reach
// in-bounds holds ~0.1 of the full weight, and one rejected trail must stay out there too (unit test).
constexpr double kMinKeptWeight = 0.25;
// Smallest blend weight of usable regions for the full local shift, and the spacing (full-resolution
// px) of the exactly evaluated blend weights and warp (see "The warp").
constexpr double kWarpFade = 0.25;
constexpr int kWarpGrid = 4;

// Blended warp of one frame at luma coordinate (u, v): global + local (see "The warp").
// mask: RegionLayout::mask there (1 without one); dx, dy, usable: per region of the layout, this frame's local
// shift and whether it is usable.
void LocalWarpAt(const RegionLayout& layout, const AxisWeight& ax, const AxisWeight& ay, double mask, const float* dx,
                 const float* dy, const uint8_t* usable, double& outDx, double& outDy);

// The per-pixel residual scale for rejection: frames are warped onto the reference (luma, Catmull-Rom,
// gain-corrected, the stack's blended warp on its kWarpGrid grid) and their squared difference from
// the reference accumulated per pixel, with the largest one. Sigma(): per pixel, the RMS without that
// largest squared residual (a trail or a bird crossing in one observed frame does not widen its own
// tolerance), then the largest over 3 x 3 pixels (a sharp feature landing a pixel off), floored at one
// quantisation step (an 8-bit sky clipped to black measures 0). Never smoothed over space: thin
// structure carries its large residual on its own pixels only (a 5 x 5 median rejected the sharpest
// frames there -- review finding). Observe every frame the stack uses (the tests do; a subset would
// make the leave-one-out guard weaker against a bird spanning frames). Memory per instance: 16 bytes
// per luma pixel (double sum, float max and count) plus the reference -- 1080p Bayer (960 x 540) 10 MB,
// 1080p mono 41 MB (20 bytes x 960 x 540 and x 1920 x 1080) -- so RunStack does not give every worker one:
// its ObserverPool holds PlanMemory's `observers` instances (as many as kResidualBudget allows, at most
// the workers; Engine.h), each worker checking one out per Observe, then merges them. Not thread-safe:
// one instance per thread at a time, then Merge.
class ResidualScale {
public:
    // Throws std::invalid_argument when the reference is not a 1-channel image of the layout's size.
    ResidualScale(const RegionLayout& layout, const ser::FloatImage& reference);
    // frameLuma: MeasurementLuma of the frame; m: its RegionMeasurer::Measure result (aligned regions'
    // shifts are used). Throws std::invalid_argument on a size or region-count mismatch.
    void Observe(const ser::FloatImage& frameLuma, const FrameRegions& m);
    // Adds another instance's sums (same layout and reference size; std::invalid_argument otherwise).
    void Merge(const ResidualScale& other);
    int64_t Frames() const { return frames_; }
    // normDepth: the capture's (FileInfo::normDepth), for the quantisation floor 1 / (2^depth - 1).
    // Throws std::logic_error when nothing was observed.
    ser::FloatImage Sigma(int normDepth) const;

private:
    RegionLayout layout_;
    ser::FloatImage reference_;
    std::vector<double> sum2_;
    std::vector<float> max2_;  // per pixel: the largest squared residual observed
    std::vector<float> count_;
    int64_t frames_ = 0;
};

// Hot pixels fixed on the sensor. A hot pixel that stays (nearly) still on the output -- a well-tracked
// target, seeing jitter of a pixel or two -- is in most frames and in the reference, so the rejection
// above cannot see it; it is found and repaired on the raw frames instead, before MeasurementLuma and
// DemosaicHQ (Task 6 calls Repair on every frame it reads, measurement and stacking passes alike).
// A site counts as isolated in a frame when it stands above its 8 same-colour neighbours (2 px away in
// a Bayer mosaic, 1 px in mono and in each RGB channel) by more than kHotMin of full scale AND by more
// than kHotRatio of its excess over their median: an optical image is never that peaked (a star or moon
// of PSF sigma >= 1.6 sensor px -- planetary optics at f/15 and slower -- keeps its neighbours at >= 0.46
// of its peak: ratio <= 0.8; a hot pixel's neighbours are the background, ~1). On texture -- a hot pixel
// inside a tracked disc -- the ring is not flat: a neighbour on the slope of a belt or a spot comes within
// a tenth of the hot pixel's excess of it, and the first test fails in a varying share of the frames
// (measured at 40b255d6, Task 6's capture: a 112 x 96 disc of R 24 with MakeDisc's texture, seeing blur and local
// jitter, no drift, 50 frames; a hot pixel on each of the 1386 sites within 21 px of the centre, found
// when isolated in >= kHotShare of the frames, HotPixelMap as a whole: mono +0.35 573, +0.2 6, +0.15 0;
// Bayer +0.35 177, +0.5 411). So a site also counts when it stands above the whole ring by kHotMin AND
// its excess over the ring's smooth surface -- the quadratic through the 8 neighbours, which predicts
// the site as 2 mean(4 sides) - mean(4 corners) and follows the texture's slope and curvature -- is more
// than kHotRatio of its excess over their median. In mono that is all: found then mono 1386 / 1357 /
// 1213 (+0.35 / +0.2 / +0.15); still optical detail passes only when far sharper than planetary optics
// give (over every sub-pixel placement and angle, the largest share of a still point's rise left
// unpredicted is 0.14 at PSF sigma 1.6 px, a line's well under 0.9 above ~0.65 px; an edge, a limb,
// never stands above the whole ring). In a mosaic the ring is 2 px out, and native sampling of a 2.9 um
// sensor at f/9-f/12 is PSF sigma 1.0-1.1 px, 0.5 ring steps: still points and lines that sharp pass
// the surface test too (review round 1 at 0cf84db2, 6 seeds x ~21 features, 50 frames, amplitude 0.25: sigma 1.0
// lines on a disc 0 -> 8, on sky 0 -> 13; sigma 0.8 points on a disc 0 -> 70, lines 0 -> 59). So in a
// mosaic the site must also leave the different-colour samples 1 px away at the local level: the
// larger of the rises of the 1-px pair over the same colours 3 px out, along x and along y, is under
// kHotRiseShare of its excess over the surface. A hot pixel leaves its other-colour neighbours alone;
// an optical feature through the site raises them -- over every placement and angle of still points and
// lines that pass the surface test, that rise is at least 0.25 / 0.50 / 0.90 / 1.08 / 1.27 of the
// excess at PSF sigma 0.6 / 0.8 / 1.0 / 1.1 / 1.2 px (and none pass from 1.3), and the brighter of the
// two colour pairs counts, so a neighbour colour half as bright still clears the gate from sigma 1.0
// (0.45 against 0.25) and just reaches it at 0.8 (scan of noise-free features, amplitude 0.1-1).
// Measured with the gate at 0cf84db2 (same scans): no more false flags than the first test alone at any sigma
// 0.8-2.5, still or jittered 0.2 px, on disc or sky (sigma 0.8 points on sky: 16, as before; every
// other cell 0); found on Task 6's disc, Bayer: 1160 (+0.35; 1207 without the gate), 1228 (+0.5), 493
// (+0.15). The clean disc flags nothing. Still segments with hard (non-optical) ends do pass in mono
// (6 seeds: 1 / 1 / 2 sites at sigma 1.0 / 1.3 / 2.0): an end cut off within a pixel is a step no PSF
// makes, and the surface cannot predict it; with erf ends at the PSF's sigma, none (the unit test's
// features have erf ends). In a mosaic an adjacent
// pair is two lone sites (neighbouring sensor pixels are other colours). Hot pairs and segments along a
// line -- column, row or diagonal; in mono any adjacent pair, in a mosaic 3 px or more -- put members on
// each other's ring, so a site also counts when it stands above the 6 neighbours off one such line by
// those margins, the second measured from the lowest of the ring and the 2 same-colour samples 2 ring
// steps out across the line: a segment is one pixel wide and the background flat across it, while a
// still optical feature that looks like one on the ring alone -- a point between two sites, two sites
// flanking a peak, a thin line -- falls off across it (unit test: segments of 3 and 5 px along every
// line found and repaired whole; still moons on every site colour and between sites, still lines along
// a row, a column, a diagonal and between rows, a still sharp limb, none flagged, Bayer and mono).
// Round 1's mono rule (above all but the two largest neighbours) flagged still points between pixels
// and still lines at any PSF sigma. Not found: a cluster that bends (an L of 3), a 2 x 2 block, and the
// line rule within 2 ring steps of the frame's edge across the line. Caveat: a Bayer capture sharper
// than PSF sigma ~1.1 px puts a point's distance-2 neighbours under 0.2 of its peak, so a star held
// perfectly still on one site would pass the shape test (scan, 20 frames: false flags from sigma 1.0 on;
// still lines along a line, from 0.9; mono none down to 0.9); the smooth-surface rule, gated in a
// mosaic as above, adds none down to sigma 0.8, and in mono passes a still line at its worst angle only
// from ~0.65 px down. Only the persistence rule below then protects it. A site isolated in at
// least kHotShare of the observed frames is hot (a moon peak that passes the shape test moves over the
// sensor with drift and jitter and is not on one site that often).
// Repair replaces it with the median of its same-colour neighbours. Observing ~50-100 frames spread
// over the capture is enough (kMinHotFrames at least; fewer observed marks nothing). Memory per
// instance: 2 bytes per sample until Finish (1080p mono 4 MB, 1080p RGB 12 MB: 1920 x 1080 x 2 and x 6),
// so one instance per worker, merged, is cheap; after Finish only the sites. Not thread-safe: one instance
// per thread, then Merge.
constexpr double kHotMin = 0.02;
constexpr double kHotRatio = 0.9;
constexpr double kHotShare = 0.8;
constexpr double kHotRiseShare = 0.25;  // mosaic smooth-surface gate (above)
constexpr int kMinHotFrames = 10;
class HotPixelMap {
public:
    // info: the capture (frame size, colour, normDepth for the full scale).
    explicit HotPixelMap(const ser::FileInfo& info);
    // A raw frame of the capture (std::invalid_argument on a size or channel mismatch).
    void Observe(const ser::Frame& frame);
    void Merge(const HotPixelMap& other);
    // Decides the hot sites from the frames observed so far, then releases the per-sample counts (so a
    // finished map holds only its sites). Call after the last Observe and Merge (either afterwards is a
    // std::logic_error); a second call does nothing.
    void Finish();
    // Bytes held: the per-sample counts until Finish, the sites after.
    size_t BytesHeld() const { return isolated_.capacity() * sizeof(uint16_t) + hot_.capacity() * sizeof(size_t); }
    // Hot sites (after Finish).
    size_t Count() const { return hot_.size(); }
    // Hot sample indices ((y * width + x) * channels + channel), ascending (after Finish).
    const std::vector<size_t>& Sites() const { return hot_; }
    int64_t FramesObserved() const { return frames_; }
    // Repairs a raw frame in place (no-op before Finish or without hot sites).
    void Repair(ser::Frame& frame) const;

private:
    int width_, height_, channels_, step_;
    size_t samples_ = 0;              // per frame
    bool finished_ = false;
    double fullScale_;
    std::vector<uint16_t> isolated_;  // per sample: frames in which it stood alone (released by Finish)
    std::vector<size_t> hot_;         // sample indices
    int64_t frames_ = 0;
};

class Accumulator {
public:
    // plan: PlanRegions' result, held for the Accumulator's life -- taken by value, so a caller done with it
    // moves it in and its per-frame and per-region arrays are not held twice (RunStack does; final review,
    // engine M3), while a caller that keeps using it passes a copy; reference: BuildReference's luma image;
    // residualSigma: ResidualScale::Sigma (both the plan's luma size); info: the capture -- full-resolution
    // size header.width x header.height, channels 3 for Bayer and RGB, 1 for mono; luma scale LumaScale(info).
    // Throws std::invalid_argument on any size mismatch or a plan without frames.
    Accumulator(RegionPlan plan, const ser::FloatImage& reference, const ser::FloatImage& residualSigma,
                const ser::FileInfo& info);
    const RegionPlan& Plan() const { return plan_; }
    ~Accumulator();
    Accumulator(const Accumulator&) = delete;
    Accumulator& operator=(const Accumulator&) = delete;

    // Adds frame `frameIndex` (DemosaicHQ output: full size, the capture's channels; taken by value so
    // the caller can move it in -- it is prefiltered in place). A frame the plan does not use is
    // ignored (returns false); adding the same frame twice is an std::logic_error; a size or channel
    // mismatch std::invalid_argument. Thread-safe.
    bool Add(int64_t frameIndex, ser::FloatImage frame);

    // The stack: full size, the capture's channels, interleaved, linear (normalised like DemosaicHQ).
    // Callable at any time (frames still to come are missing from it); std::logic_error before the
    // first Add.
    ser::FloatImage Finish() const;

    int64_t FramesAdded() const { return added_.load(); }
    // Luma cells rejected (counted once per frame and cell that had samples inside the frame).
    int64_t RejectedCells() const { return rejected_.load(); }
    // Bytes held between calls (sums, reference, residual scale, plan, tables).
    size_t BytesHeld() const;

private:
    RegionPlan plan_;
    int width_, height_, channels_, scale_, lumaW_, lumaH_;
    double lumaWeights_[3];
    const int* pattern_ = nullptr;        // Bayer: the CFA's channel per site ((y & 1) * 2 + (x & 1)); else null
    std::vector<float> refLuma_, sigma_;  // luma size: reference luma, residual scale
    // Kept samples: sum_ (channels per pixel) every unsaturated value; weight_ (per pixel) the weight of the samples
    // with no saturated channel; weightX_ (channels per pixel) a channel's unsaturated weight from samples with
    // another channel saturated; sumC_, weightC_ (channels per pixel) the saturated values, lower bounds (see
    // "Clipping"). Only a sample with a saturated channel touches the per-channel weights.
    std::vector<float> sum_, weight_, weightX_, sumC_, weightC_;
    std::vector<float> sumRej_, weightRej_;  // the rejected samples (Finish's stand-in): channels per pixel; per pixel
    std::vector<uint8_t> reached_;           // per pixel: bit c + 1, channel c saturated in a frame no brighter than the reference
    std::vector<int> blendRegion_;  // per region: 1 when it keeps frames
    std::vector<float> gridMask_;   // the layout's blend mask at the kWarpGrid points (empty: 1 everywhere)
    std::vector<int> cellOfX_;      // per full-resolution column: its luma cell column
    std::vector<std::unique_ptr<std::mutex>> bandLocks_;
    std::vector<uint8_t> addedFlags_;
    std::mutex addedLock_;
    std::atomic<int64_t> added_{0}, rejected_{0};
};

}  // namespace planetary::stack
