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
// w_r are the layout's sin^2 weights (a partition of unity), plus the fallback stack -- every planned
// frame, equally -- with weight 1 - sum_r w_r where regions do not cover (the sky around a disc, a
// region with no kept frame). That sum is linear in the frames, so it is accumulated directly: frame f
// adds alpha_f(p) x its sample at p, alpha_f(p) = sum_{r keeps f} w_r(p) / K_r + (1 - sum_r w_r(p)) /
// K_fallback, and the result is the weighted sum over the weight sum. One sample per pixel per frame,
// one output-sized buffer -- no per-region stacks are held.
//
// The warp. A frame is sampled at p + s (global + local(p)), s = LumaScale: the local shift is the
// regions' shifts blended with the same weights over the regions where this frame's shift is usable,
// local(p) = sum w_r c_r d_r / max(sum w_r c_r, kWarpFade) (c_r = 1 when usable) -- smooth, so
// neighbouring regions never tear the image apart, and fading to the global shift toward the sky.
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
// frame of 40 that every region keeps): what leaks into the stack is 0 (mono) / 0.0004 (Bayer),
// against >= 0.0125 unrejected; pixels farther than the reach are unchanged.
// Limit: a hot pixel that does NOT move on the output (a target perfectly still on the sensor) is in
// the reference too and is not an outlier in time; that needs a sensor-space fix before demosaicing.
//
// Memory (asserted by BytesHeld): per output pixel (channels + 1) float sums and as many float sums of
// rejected samples; the reference and residual scale at luma size; the plan (frames x regions x 9
// bytes plus ~40 bytes per frame) -- 1080p RGB: 66 MB. Per Add call: the frame itself (moved in and
// prefiltered in place) and a ring of 2 kRejectReach + 1 luma rows of scratch -- O(one frame) per
// concurrent Add, independent of the number of frames. (Not the O(regions x region size) of
// per-region stacks: the blend above needs none.)
//
// Cost (measured 2026-10-21, Ryzen 9 9950X, 128 synthetic 1920 x 1080 frames moved in): Bayer (RGB
// output) 45 ms per Add on one thread, 219 frames/s on 16 threads and 281 on 32 (4000 frames: 18.3 /
// 14.2 s); mono 42 ms, 333 / 397 frames/s (12.0 / 10.1 s). Before the review round: 72 ms and 96 /
// 85 frames/s (Bayer). What made it so -- the prefilter in two streaming passes, no libm floor in the
// sampling, the 3 channels' taps as 12 vectorisable lanes, rows through a ring that stays in cache,
// float sums -- changes the stack by at most 4e-7 (float rounding; A/B against the double-sum
// version on mono and Bayer captures with sharp point features).
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
// A pixel whose kept weight is under this (of the 1 a full stack gives it) blends its rejected samples
// back in, linearly up to all of them at 0 kept: rejection never leaves a hole (see Finish).
constexpr double kMinKeptWeight = 0.25;
// Smallest blend weight of usable regions for the full local shift, and the spacing (full-resolution
// px) of the exactly evaluated blend weights and warp (see "The warp").
constexpr double kWarpFade = 0.25;
constexpr int kWarpGrid = 4;

// Blended warp of one frame at luma coordinate (u, v): global + local (see "The warp").
// dx, dy, usable: per region of the layout, this frame's local shift and whether it is usable.
void LocalWarpAt(const RegionLayout& layout, const AxisWeight& ax, const AxisWeight& ay, const float* dx,
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
// 1080p mono 41 MB -- so Task 6 should use one instance per worker (16-32: 0.3-1.3 GB for mono 1080p;
// cap the workers observing to 8 there, or Merge as each worker finishes a run of frames). Not
// thread-safe: one instance per thread, then Merge. Cost: 26 ms per 1920 x 1080 mono frame (one thread).
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
// a Bayer mosaic, 1 px in mono and in each RGB channel) -- in mono and RGB above all but the two
// largest, so an adjacent hot pair or a hot column segment counts too (Bayer: above all; neighbouring
// sensor pixels are other colours, so a pair there is two lone sites) -- by more than kHotMin of full
// scale AND by more than kHotRatio of its excess over their median: an optical image is never that
// peaked (a star or moon of PSF sigma >= 1.6 sensor px -- planetary optics at f/15 and slower -- keeps
// its neighbours at >= 0.46 of its peak: ratio <= 0.8; a hot pixel's neighbours are the background,
// ~1). Caveat: a Bayer capture sharper than PSF sigma ~1.1 px puts a point's distance-2 neighbours under
// 0.2 of its peak, so a star held perfectly still on one site would pass the shape test; only the
// persistence rule below then protects it. A site isolated in at least kHotShare of the observed
// frames is hot (a moon peak that passes the shape test moves over the sensor with drift and jitter and
// is not on one site that often). Repair replaces it with the median of its same-colour neighbours.
// Observing ~50-100 frames spread over the capture is enough (kMinHotFrames at least; fewer observed
// marks nothing). Memory per instance: 2 bytes per sample (1080p mono 4 MB, 1080p RGB 12 MB), so one
// instance per worker, merged, is cheap. Not thread-safe: one instance per thread, then Merge.
constexpr double kHotMin = 0.02;
constexpr double kHotRatio = 0.9;
constexpr double kHotShare = 0.8;
constexpr int kMinHotFrames = 10;
class HotPixelMap {
public:
    // info: the capture (frame size, colour, normDepth for the full scale).
    explicit HotPixelMap(const ser::FileInfo& info);
    // A raw frame of the capture (std::invalid_argument on a size or channel mismatch).
    void Observe(const ser::Frame& frame);
    void Merge(const HotPixelMap& other);
    // Decides the hot sites from the frames observed so far (call once, after the last Observe).
    void Finish();
    // Hot sites (after Finish).
    size_t Count() const { return hot_.size(); }
    // Hot sample indices ((y * width + x) * channels + channel), ascending (after Finish).
    const std::vector<size_t>& Sites() const { return hot_; }
    int64_t FramesObserved() const { return frames_; }
    // Repairs a raw frame in place (no-op before Finish or without hot sites).
    void Repair(ser::Frame& frame) const;

private:
    int width_, height_, channels_, step_;
    double fullScale_;
    std::vector<uint16_t> isolated_;  // per sample: frames in which it stood alone
    std::vector<size_t> hot_;         // sample indices
    int64_t frames_ = 0;
};

class Accumulator {
public:
    // plan: PlanRegions' result; reference: BuildReference's luma image; residualSigma: ResidualScale::
    // Sigma (both the plan's luma size); info: the capture -- full-resolution size header.width x
    // header.height, channels 3 for Bayer and RGB, 1 for mono; luma scale LumaScale(info).
    // Throws std::invalid_argument on any size mismatch or a plan without frames.
    Accumulator(const RegionPlan& plan, const ser::FloatImage& reference, const ser::FloatImage& residualSigma,
                const ser::FileInfo& info);
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
    std::vector<float> refLuma_, sigma_;  // luma size: reference luma, residual scale
    std::vector<float> sum_, weight_;         // full size (sum_: channels per pixel)
    std::vector<float> sumRej_, weightRej_;   // full size: the rejected samples (Finish's stand-in)
    std::vector<int> blendRegion_;  // per region: 1 when it keeps frames
    std::vector<int> cellOfX_;      // per full-resolution column: its luma cell column
    std::vector<std::unique_ptr<std::mutex>> bandLocks_;
    std::vector<uint8_t> addedFlags_;
    std::mutex addedLock_;
    std::atomic<int64_t> added_{0}, rejected_{0};
};

}  // namespace planetary::stack
