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
// ... and its box's shorter side is at least this share of the frame's (a satellite streak's
// frame-wide box is ~2% tall; the real lunar boxes measured 2026-10-21 are >= 48% on both sides).
constexpr double kFullFrameMinSide = 0.25;
// The per-frame half of that rule: a found target whose box touches an edge of the lumaWidth x
// lumaHeight luma frame, spans at least kFullFrameSpan of its width or height, and at least
// kFullFrameMinSide of both.
bool FillsFrame(const FrameStats& s, int lumaWidth, int lumaHeight);
// Upper fence on log quality, in robust standard deviations (1.4826 MAD) above the median; the
// robust sd is floored at kFenceMinLogSd (1%: identical frames still differ by that much in
// quality from frame to frame) and nothing is fenced in fewer than kMinFenceFrames frames.
constexpr double kFenceSigmas = 6;
constexpr double kFenceMinLogSd = 0.01;
constexpr int kMinFenceFrames = 30;
// Deconvolution grid and EM budget (step 6).
constexpr int kPriorGrid = 64;
constexpr int kPriorIterations = 300;
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
// A blurred capture whose true limb blur varies less than this (p10..p90 over the median, from the
// noise-corrected spread tau of step 7) is steady -- the optics (focus), not the air (seeing). Fewer
// than kMinJudgedFrames frames, or a spread the noise leaves undecided, is worded "can't tell".
constexpr double kSteadySpread = 0.05;
constexpr int kMinJudgedFrames = 30;

// Automatic frame selection and diagnosis from the per-frame statistics (spec "The engine" step 2
// and "Diagnosis"). `stats` holds one entry per frame (MeasureAll's output; any order); `info` is the
// capture's (luma frame size header.width / LumaScale(info), and sensor-pixel numbers).
// Deterministic and independent of the input order; 20 000 frames with noise take well under a
// second (the deconvolution is O(N kPriorGrid) per EM step).
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
// 4. Frames with quality 0 ("without detail") are dropped; fewer than floor with detail ->
//    OutOfFocus.
// 5. Upper fence: frames with log q > median + kFenceSigmas * max(1.4826 MAD, median relative
//    qualitySigma, kFenceMinLogSd) are outliers (a torn frame, a satellite in the box) -- only with
//    at least kMinFenceFrames frames, only when the frames above number fewer than ceil(sqrt(N)) (a
//    larger group that far above the bulk is a real sharp mode, and a genuine lucky tail never
//    reaches 6 sigma), and never leaving fewer than floor.
// 6. Deconvolution (Ruling P5). Each observed q_i = true quality + noise N(0, qualitySigma_i^2).
//    The distribution G of the true quality is estimated by nonparametric maximum likelihood
//    (Kiefer-Wolfowitz NPMLE, heteroscedastic; EM on kPriorGrid points over the observed range, up
//    to kPriorIterations steps); each frame's posterior over G then gives its expected true quality
//    and, in step 8, its expected contrast. G can be bimodal, so noise is removed inside each mode
//    (a single Gaussian prior cannot: in a bimodal capture the gap between the modes swamps the
//    noise). With no noise reported (qualitySigma 0, e.g. black-clipped 8-bit sky) every quality is
//    taken as exact. "The best", q_ref, is the quality the sharpest max(min(3, N), ceil(N / 100))
//    of the N frames reach in G -- the true distribution's top 1%, not the noisiest frames' (no
//    winner's curse); without noise, the raw q at that rank.
// 7. Focus / seeing, discs only (focusJudged). The disc model: a disc of radius R whose edge is
//    blurred by a Gaussian sigma (sigma << R) has quality^2 = 1 / (sqrt(pi) R sigma) (rim gradient
//    energy over disc signal energy), so sigma(q) = 1 / (sqrt(pi) q^2 R). R = the median box's
//    sqrt(w h) / 2 less the one detection block it was grown by (at least 1).
//      R >= kMinJudgedRadius: blurred when sigma(q_ref) >= R / 8.
//      R <  kMinJudgedRadius (Ruling P4): sigma' = sqrt(max(0, sigma^2 - kSamplingBlur^2)); blurred
//           when sigma'(q_ref) >= max(R / 8, kBlurFloorPixels / LumaScale).
//    R / 8: the coarsest planetary structure (belts, period P ~ R/4) then keeps under 1% contrast
//    (exp(-2 pi^2 sigma^2 / P^2) < 0.01 at sigma > 0.48 P). The blur spread is that of the true
//    quality: tau^2 = max(0, var(q) - mean(qualitySigma^2)), spread = (sigma(mu - 1.2816 tau) -
//    sigma(mu + 1.2816 tau)) / sigma(mu); its upper bound uses tau^2 + 2 SE, SE = sqrt(2 / N) var(q).
//    Blurred -> PoorSeeing when spread >= kSteadySpread; OutOfFocus when even the upper bound is
//    under it; otherwise (or under kMinJudgedFrames frames) OutOfFocus worded "focus or seeing,
//    can't tell". Measured 2026-10-21 on in-focus captures: sigma(q_ref) / R = 0.007-0.033 where
//    R >= 15. No defocused or bad-seeing capture was available, so these limits rest on the
//    reasoning above.
// 8. Selection. A frame of true quality q has, at the frequency where the best frames keep
//    e^-kDetailDepth, the contrast MTF(q) = exp(-kDetailDepth max(1, (q_ref / q)^4)) ((q_ref / q)^2
//    is sigma / sigma_ref; Gaussian blur; frames sharper than the best count as the best). Each frame
//    gets its posterior expectation of MTF (its exact MTF without noise). Stacking the k frames with
//    the highest expected MTF gives that detail with signal-to-noise S(k) = sum_{i<=k} MTF_i /
//    sqrt(k) (contrast adds, noise adds in quadrature); the kept set is the k maximising S, at least
//    min(need, N). A frame joins exactly when its MTF is at least about half the mean MTF of those
//    in, so the kept fraction follows the distribution's shape -- no percentage anywhere.
//    kDetailDepth = 3: 5% contrast in the best frames is about the finest detail a stack of
//    hundreds of frames lifts out of noise; larger chases finer detail and keeps fewer frames. On a
//    full-frame target the whole-frame quality varies little (seeing averages out over the frame),
//    so this keeps nearly every frame -- the lenient global cut of Ruling P2; the regions are ranked
//    separately (Task 5). Checked against an oracle that knows each frame's true blur (3000 frames,
//    blur log-sd 0.2 / 0.2 / 0.3, quality noise 2 / 5 / 10%): S of the chosen set is 0.975 / 0.872
//    / 0.724 of the oracle's best.
//
// Explanation (one ASCII line, as FITS HISTORY needs):
//   "used 612 of 4088 frames: the frames judged sharpest after allowing for measurement noise,
//    quality >= 0.91 of the best; not used: 3 without a target, 12 at the frame edge, 2 outliers,
//    3459 below the cut"
// -- every frame accounted for (groups: without a target, stray detection(s), at the frame edge,
// clipped, without detail, outlier(s), below the cut; empty groups omitted), plus "; frame-filling
// target, each region ranks its own frames" for a full-frame target. "Judged sharpest": with noise,
// a frame left out can have a higher raw quality than one kept. The quality bound is the lowest
// kept raw quality over q_ref, rounded down.
Selection SelectFrames(const std::vector<FrameStats>& stats, const ser::FileInfo& info);

}  // namespace planetary::stack
