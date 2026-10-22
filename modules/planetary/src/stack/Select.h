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
    std::string explanation;      // one ASCII result line; the diagnosis message when diagnosed
    Diagnosis diagnosis;
    bool fullFrameTarget = false; // the target fills the frame (lunar surface), step 2
    // Whether focus and seeing were judged here (step 7). False for a full-frame target: its
    // quality is texture, not a limb, so OutOfFocus / PoorSeeing for it belong to the region stage
    // (Ruling P4: Task 5 exposes per-region detail with its uncertainty, Task 6 diagnoses when even
    // the best regions of the best frames fall below an absolute texture-scale floor).
    bool focusJudged = false;
};

// Fewest frames worth stacking: outlier rejection (satellite, hot pixel) needs a handful of samples
// per pixel to tell an outlier from the rest.
constexpr int kMinStackFrames = 5;
// Stray detections: a target frame counts only when its box width and height are each within this
// factor of the capture's median box width and height (a satellite streak's box is a frame-wide
// line; a disc half cut by the edge still passes).
constexpr double kBoxTolerance = 2.0;
// A frame is clipped when this fraction of its target pixels is at full scale: a quarter of a disc
// is its bright centre, where the detail is. Measured 2026-10-21: Neptune 0.08-0.10 and the 1080p
// Jupiter 0.12 (both usable), Venus 2023-02-11 0.37 in every frame (saturated).
constexpr double kClippedFrame = 0.25;
// Full-frame target (Ruling P2a): in at least this share of the target frames, the box touches an
// edge AND spans at least kFullFrameSpan of the frame's width or height.
constexpr double kFullFrameShare = 0.75;
constexpr double kFullFrameSpan = 0.5;
// Upper fence on log quality, in robust standard deviations (1.4826 MAD) above the median.
constexpr double kFenceSigmas = 6;
// The frequency the selection optimises, as the contrast the best frames keep there: e^-3 = 5%.
constexpr double kDetailDepth = 3.0;
// Focus / seeing: blurred when the best frames' limb blur is at least kBlurLimit of the radius.
constexpr double kBlurLimit = 0.125;
// Discs with a radius under this (luma px) are judged with the sampling correction and floor below.
constexpr double kMinJudgedRadius = 15;
// Limb width the luma sampling itself gives a perfectly sharp edge, removed in quadrature (luma px).
constexpr double kSamplingBlur = 0.5;
// A small disc is called blurred only when its limb blur is also at least this many sensor pixels:
// the diffraction blur of a planetary setup is sigma ~ 0.42 lambda N (550 nm: 1.6 px at f/20 and
// 2.8 px at f/35 on 2.9 um pixels), so a limb under 3 px can be a perfectly focused tiny disc.
constexpr double kBlurFloorPixels = 3;
// A blurred capture whose de-noised limb blur varies less than this (p10..p90 over the median) is
// steady -- the optics (focus), not the air (seeing) -- provided at least kMinJudgedFrames frames
// were judged; with fewer, the message says which of the two cannot be told.
constexpr double kSteadySpread = 0.05;
constexpr int kMinJudgedFrames = 30;

// Automatic frame selection and diagnosis from the per-frame statistics (spec "The engine" step 2
// and "Diagnosis"). `stats` holds one entry per frame (MeasureAll's output; any order); `info` is the
// capture's (luma frame size header.width / LumaScale(info), and sensor-pixel numbers).
// Deterministic and independent of the input order; O(n log n): 20 000 frames take milliseconds.
//
// need  = max(1, min(kMinStackFrames, ceil(n / 2)))   -- fewest frames to stack; a short capture
//                                                        may lose up to half its frames
// floor = max(need, ceil(sqrt(n)))                     -- fewest frames that speak for a capture
//
// 1. Target frames: targetFound, and the box within kBoxTolerance of the median box (else a "stray
//    detection"). Fewer than floor -> NoTarget.
// 2. Full-frame or disc (Ruling P2a, constants above). Disc: frames whose box touches an edge are
//    dropped (never a cut limb, Review Focus 1); fewer than floor left -> TargetLeavesFrame.
//    Full-frame: every frame is cut by the frame; usable parts are the region stage's job (Task 5).
// 3. Clipped frames (clippedFraction >= kClippedFrame) are dropped; fewer than floor -> Clipped.
// 4. Frames with quality 0 ("without detail") are dropped; fewer than need with detail -> OutOfFocus.
// 5. Upper fence: frames with log q > median + kFenceSigmas * max(1.4826 MAD, median relative
//    qualitySigma) are outliers (a torn frame, a satellite in the box) -- but only when they number
//    fewer than ceil(sqrt(frames left)): a larger group that far above the bulk is a real sharp mode
//    (steady spells of a poor night), and a genuine lucky tail never reaches 6 sigma.
// 6. De-noising (Ruling P5): with mu and v the mean and variance of q over the frames left and
//    tau^2 = max(0, v - mean(qualitySigma^2)) the spread the noise does not explain,
//        q^_i = mu + lambda_i (q_i - mu),   lambda_i = tau^2 / (tau^2 + qualitySigma_i^2)
//    (empirical Bayes; lambda = 1 when both are 0). Every difference between two frames -- so each
//    frame's deficit to the reference -- shrinks by lambda: scatter within the noise (tau ~ 0) reads
//    as equal quality, real seeing (tau >> sigma) is kept. Measured 2026-10-21: Neptune lambda ~ 0
//    (spread 0.059 of the mean, noise 0.070), Jupiter captures 0.89-1, Moons 0.67-1.
//    Everything below uses q^. Ranked sharpest first (ties by frame index); "the best" q_ref is the
//    q^ at rank max(min(3, N), ceil(N / 100)) of the N frames left, so a lone glitch cannot set it.
// 7. Focus / seeing, discs only (focusJudged). The disc model: a disc of radius R whose edge is
//    blurred by a Gaussian sigma (sigma << R) has quality^2 = 1 / (sqrt(pi) R sigma) (rim gradient
//    energy over disc signal energy), so sigma_i = 1 / (sqrt(pi) q^_i^2 R). R = the median box's
//    sqrt(w h) / 2 less the one detection block it was grown by (at least 1).
//      R >= kMinJudgedRadius: blurred when sigma_ref >= R / 8.
//      R <  kMinJudgedRadius (Ruling P4): sigma' = sqrt(max(0, sigma^2 - kSamplingBlur^2)); blurred
//           when sigma'_ref >= max(R / 8, kBlurFloorPixels / LumaScale).
//    R / 8: the coarsest planetary structure (belts, period P ~ R/4) then keeps under 1% contrast
//    (exp(-2 pi^2 sigma^2 / P^2) < 0.01 at sigma > 0.48 P). Blurred -> OutOfFocus when the
//    de-noised blur spread is under kSteadySpread, PoorSeeing when it is not; under kMinJudgedFrames
//    frames, OutOfFocus worded "focus or seeing, too few frames to tell". Measured 2026-10-21 on
//    in-focus captures: sigma_ref / R = 0.007-0.033 where R >= 15. No defocused or bad-seeing
//    capture was available, so these limits rest on the reasoning above.
// 8. Selection. Each frame's deficit to the reference is first reduced by the 1-sigma noise left in
//    it (Ruling P5, literally): q_eff_i = min(q_ref, q^_i + sqrt(n_ref^2 + n_i^2)), n = lambda *
//    qualitySigma -- a frame within its own noise of the best is the best. (Step 6 alone is not
//    enough: in a bimodal capture the gap between the modes makes tau large and lambda ~ 1, so the
//    noise inside the sharp mode stays, and the reference -- its top 1% -- sits ~2.3 sigma above it.)
//    Frame i's contrast at the frequency where the best frames keep e^-kDetailDepth is
//    MTF_i = exp(-kDetailDepth max(1, (sigma_i / sigma_ref)^2)), sigma_i / sigma_ref = (q_ref / q_eff_i)^2
//    (Gaussian blur; frames sharper than the best count as the best); frames ranked by q_eff. The k sharpest frames give
//    that detail with signal-to-noise S(k) = sum_{i<=k} MTF_i / sqrt(k) (contrast adds, noise adds
//    in quadrature); the kept set is the k maximising S, at least min(need, N). A frame joins
//    exactly when its MTF is at least about half the mean MTF of those in, so the kept fraction
//    follows the distribution's shape -- no percentage anywhere. kDetailDepth = 3: 5% contrast in
//    the best frames is about the finest detail a stack of hundreds of frames lifts out of noise;
//    larger chases finer detail and keeps fewer frames. On a full-frame target the whole-frame
//    quality varies little (seeing averages out over the frame), so this keeps nearly every frame
//    -- the lenient global cut of Ruling P2; the regions are ranked separately (Task 5).
//
// Explanation (one ASCII line, as FITS HISTORY needs):
//   "used 612 of 4088 frames: the sharpest group, quality >= 0.91 of the best; not used: 3 without
//    a target, 12 at the frame edge, 2 outliers, 3459 below the cut"
// -- every frame accounted for (groups: without a target, stray detection(s), at the frame edge,
// clipped, without detail, outlier(s), below the cut; empty groups omitted), plus "; frame-filling
// target, each region ranks its own frames" for a full-frame target. The quality bound is the lowest
// kept raw quality over q_ref's frame's raw quality, rounded down.
Selection SelectFrames(const std::vector<FrameStats>& stats, const ser::FileInfo& info);

}  // namespace planetary::stack
