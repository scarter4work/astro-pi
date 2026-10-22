#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "FrameSource.h"
#include "Measure.h"

namespace planetary::stack {

// Why a capture gives no image (spec "Diagnosis"). None: frames were selected.
enum class DiagnosisKind { None, NoTarget, OutOfFocus, PoorSeeing, Clipped, TargetLeavesFrame };

struct Diagnosis {
    DiagnosisKind kind = DiagnosisKind::None;
    std::string message;                                   // one plain-words sentence (empty for None)
    std::vector<std::pair<std::string, double>> numbers;   // the numbers behind it, named
};

struct Selection {
    std::vector<int64_t> frames;  // selected frame indices, ascending; empty iff diagnosis.kind != None
    double threshold = 0;         // lowest selected quality (FrameStats::quality units); 0 when diagnosed
    std::string explanation;      // one result line; the diagnosis message when diagnosed
    Diagnosis diagnosis;
    bool fullFrameTarget = false; // the target fills the frame (lunar surface); see below
};

// Fewest frames worth stacking: outlier rejection (satellite, hot pixel) needs a handful of samples
// per pixel to tell an outlier from the rest. A capture shorter than this needs all its frames.
constexpr int kMinStackFrames = 5;
// A frame is clipped when this fraction of its target pixels is at full scale: a quarter of a disc
// is its bright centre, where the detail is. Measured 2026-10-21: Neptune 0.08-0.10 and the 1080p
// Jupiter 0.12 (both usable), Venus 2023-02-11 0.37 in every frame (saturated).
constexpr double kClippedFrame = 0.25;
// Full-frame (frame-filling) target: in at least this share of the target frames, the box touches
// an edge AND spans at least kFullFrameSpan of the frame's width or height.
constexpr double kFullFrameShare = 0.75;
constexpr double kFullFrameSpan = 0.5;
// The frequency the selection optimises, as the contrast the best frames keep there: e^-3 = 5%.
constexpr double kDetailDepth = 3.0;
// Focus / seeing judgement of a disc: only when its radius is at least this many luma pixels (a
// sharp disc's limb is ~1 luma pixel wide from sampling alone, so a smaller disc always reads
// "blurred" relative to its size), and it fails when the best frames' limb blur is at least
// kBlurLimit of the radius.
constexpr double kMinJudgedRadius = 15;
constexpr double kBlurLimit = 0.125;
// A blurred capture whose limb blur varies less than this (p10..p90 over the median) is steady:
// the blur is in the optics (focus), not the air (seeing).
constexpr double kSteadySpread = 0.05;

// Automatic frame selection and diagnosis from the per-frame statistics (spec "The engine" step 2
// and "Diagnosis"). `stats` holds one entry per frame (MeasureAll's output; any order); `info` is the
// capture's (for the luma frame size: header.width / LumaScale(info), and full-resolution numbers).
// Deterministic, O(n log n): 20 000 frames take milliseconds.
//
// 1. Target frames: targetFound. Fewer than need = min(kMinStackFrames, frames) -> NoTarget.
// 2. Full-frame or disc. The target is full-frame (a lunar surface larger than the frame) when, in
//    >= 75% of the target frames, its box touches an edge and spans >= half the frame's width or
//    height. A disc smaller than the frame that drifts out shows a box touching the edge but small;
//    a Moon bigger than the frame touches an edge in every frame and is large. Ruling P2 named
//    ">= 3 edges"; measured 2026-10-21 it holds for 1 of 3 lunar captures (2023-02-12) -- the
//    2026-01-30 Moon touches 1 edge (box 1550 x 944 of 1920 x 1080), the 2023-02-02 Moon 2 (box
//    310 x 321 of 640 x 480) -- so the rule tests size, which all three meet and no planet does.
//    Disc: frames whose box touches any edge are dropped (never a cut limb, Review Focus 1); fewer
//    than need left -> TargetLeavesFrame. Full-frame: every frame is cut by the frame, so no edge
//    rule; which parts of a frame are usable is the per-region pass's job (Task 5).
// 3. Clipped frames (clippedFraction >= kClippedFrame) are dropped; fewer than need left -> Clipped.
// 4. Ranking. Each frame's limb blur follows from its quality through the disc model: for a disc of
//    radius R whose edge is blurred by a Gaussian sigma (sigma << R), the statistic of Measure.h
//    gives quality^2 = 1 / (sqrt(pi) R sigma) -- gradient energy along the rim over the disc's
//    signal energy -- so sigma_i / sigma_ref = (q_ref / q_i)^2 within one capture. q_ref is "the
//    best": the quality of the frame at rank m = max(min(3, n), ceil(n / 100)) (n = frames left),
//    so one glitch frame cannot set it. (Texture and limb darkening add quality that does not
//    follow the model; they make the blur spread look smaller than it is -- the rule errs on
//    keeping frames, and the per-region pass ranks again.)
// 5. Focus / seeing (discs with R >= kMinJudgedRadius luma pixels; R = the median box's
//    sqrt(w h) / 2 less one detection block, which the box was grown by): the best frames' limb
//    blur sigma_ref = 1 / (sqrt(pi) q_ref^2 R). At sigma_ref >= R / 8 the coarsest planetary
//    structure (belts, period ~R/4) keeps under 1% contrast (exp(-2 pi^2 sigma^2 / P^2) < 0.01 at
//    sigma > 0.48 P) -- nothing to stack. Then: OutOfFocus when the blur is steady across frames
//    (spread < kSteadySpread: optics), PoorSeeing when it varies (air). Any capture whose best
//    quality is 0 (no detail at all) is OutOfFocus. Measured 2026-10-21 on in-focus captures:
//    sigma_ref / R = 0.007-0.033 where judged (4x margin); smaller discs (Neptune, Mars, Venus,
//    the 480 x 360 Bayer Jupiter at R = 9.5 luma px) are not judged. No defocused or bad-seeing
//    capture was available to measure: those two limits rest on the reasoning above.
//    Full-frame targets are not judged here: their quality is texture, not a limb, and a sharp
//    Moon's texture scale (1/q ~ 6 luma px) is no blur.
// 6. Selection. Frame i's contrast at the frequency w where the best frames keep e^-kDetailDepth
//    is MTF_i = exp(-kDetailDepth max(1, (sigma_i / sigma_ref)^2)) (Gaussian blur; frames sharper
//    than the best count as the best). Stacking the k sharpest frames gives that detail with
//    signal-to-noise S(k) = sum_{i<=k} MTF_i / sqrt(k): each frame adds its contrast, noise adds in
//    quadrature. The kept set is the k that maximises S -- a frame joins exactly when its MTF is at
//    least about half the mean MTF of those already in -- at least min(need, frames left). So
//    the kept fraction follows the shape of the distribution, with no percentage anywhere: a steady
//    capture (all frames alike) keeps nearly all, a bimodal one keeps its sharp mode, a poor night
//    with a few lucky frames keeps those. kDetailDepth = 3: the frequency where the best frames
//    keep 5% contrast is the finest detail a stack of hundreds of frames lifts back out of noise;
//    a larger value chases finer detail and keeps fewer frames. On a full-frame target the
//    whole-frame quality varies little (lunar spread 0.8-4%: seeing averages out over the frame),
//    so this keeps nearly every frame -- the lenient global cut of Ruling P2; the regions are
//    ranked separately in Task 5.
//
// Explanation (one line): "used 612 of 4088 frames: the sharpest group, quality >= 0.91 of the best"
// plus what was left out and why ("; not used: 12 at the frame edge, 3 clipped") and, for a
// full-frame target, "; frame-filling target, each region ranks its own frames". "The best" is q_ref.
Selection SelectFrames(const std::vector<FrameStats>& stats, const ser::FileInfo& info);

}  // namespace planetary::stack
