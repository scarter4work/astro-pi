#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "FrameSource.h"
#include "Measure.h"

namespace planetary::stack {

// Why a capture gives no image (spec "Diagnosis"). None: frames were selected. SelectFrames makes the
// first five; Unaligned (too few frames align to the reference) and the region stage's OutOfFocus are
// made by RunStack after alignment (Engine.h).
enum class DiagnosisKind { None, NoTarget, OutOfFocus, PoorSeeing, Clipped, TargetLeavesFrame, Unaligned };

struct Diagnosis {
    DiagnosisKind kind = DiagnosisKind::None;
    std::string message;                                   // one plain-words sentence (empty for None)
    std::vector<std::pair<std::string, double>> numbers;   // the numbers behind it, named
};

struct Shift;  // Align.h

// A frame sharper than the capture's quality fence (step 5, Ruling P6): a glitch (a torn frame, a
// satellite in the box) or a genuinely lucky frame -- quality alone cannot tell them apart; alignment
// can. Held out of `frames` (and so out of BuildReference) until AdmitSuspects decides.
struct Suspect {
    int64_t index = 0;
    double quality = 0;   // its FrameStats::quality
    std::string reason;   // one ASCII phrase: why it is suspect (the fence and which rule, 5a or 5b)
};

struct Selection {
    std::vector<int64_t> frames;  // selected frame indices, ascending; empty iff diagnosis.kind != None
    double threshold = 0;         // lowest selected quality (FrameStats::quality units); 0 when diagnosed
    std::string explanation;      // one ASCII result line; the diagnosis message when diagnosed
    Diagnosis diagnosis;
    // Frames above the fence (step 5), ascending by index; never in `frames`; empty when diagnosed.
    // Not lost: the caller aligns them against the clean reference and calls AdmitSuspects.
    std::vector<Suspect> suspects;
    // The explanation's parts, so AdmitSuspects can restate the line once the suspects are decided.
    struct Line {
        int64_t measured = 0;               // frames in the capture
        std::string why;                    // "the frames judged sharpest ..., quality >= X of the best"
        std::vector<std::string> notUsed;   // "3 without a target", ... (the suspects not included)
        std::string note;                   // "; frame-filling target, ..." or empty
    } line;
    bool fullFrameTarget = false; // the target fills the frame (lunar surface), step 2
    // Whether focus and seeing were judged here (step 7). False for a full-frame target: its
    // quality is texture, not a limb, so OutOfFocus / PoorSeeing for it belong to the region stage
    // (Ruling P4: Task 5 exposes per-region detail with its uncertainty, Task 6 diagnoses when even
    // the best regions of the best frames fall below an absolute texture-scale floor).
    bool focusJudged = false;
    // Step 7's size class when focusJudged, 0 otherwise: the median FrameStats::halfRadius (luma px),
    // which a blur hardly moves (the box radius grows with it). Discs under kMinJudgedRadius are judged
    // a second time from the region stage (Ruling P4, Engine.h); larger ones only here (step 7).
    double targetRadius = 0;
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
// ... and its box's shorter side is at least this share of the frame's. This refuses a trail that
// runs along a frame axis: a line a few pixels wide plus one detection block on each side is 2-3%
// of the frame across at any ROI (1080p: 3 px + 2 x 8 px of 1080; 320 x 240: 3 px + 2 x 2 px of
// 240). A lunar limb or crescent sliver worth stacking is wider: the thinnest real framing kept is
// 9%. A TILTED trail's box is as wide as a sliver's (1.5 degrees across 640 x 480 is ~6%, 4 degrees
// ~12%), so the box shape cannot refuse it; the presence rule below does.
constexpr double kFullFrameMinSide = 0.05;
// ... and the frame-filling boxes stay put (Ruling P7): the robust sd (1.4826 MAD) of their centre,
// over the frame's width and over its height, is at most this. A surface sits in the field and
// drifts slowly; a trail crosses wherever it happens to, so its box sweeps. Measured 2026-10-21 on
// the three NAS Moons: 0.000-0.015 (total drift range up to 8% of the frame); the planets 0-0.019.
// A trail crossing at a uniformly random place in a band of the frame a wide has robust sd 0.37 a
// (MAD of a uniform is a / 4), so 0.1 refuses any set of trails not confined to a band under 27% of
// the frame, while a surface may drift through up to ~27% of the frame over the capture (6x the
// measured worst). The limit for a capture re-centred once (two positions, not a drift): two equal
// halves d apart have MAD d / 2, robust sd 0.74 d, so re-centring by more than ~0.135 of the frame
// (0.1 / 0.74) is refused when it splits the frame-filling frames about evenly; a clear majority at
// one position has MAD 0 and passes whatever the jump.
constexpr double kFullFrameMaxDrift = 0.1;
// ... and the target is found (consistently, step 1) in at least this share of ALL frames (Ruling
// P7). A low satellite or an aircraft moves ~0.5-1.5 degrees/s, so it crosses a planetary field (a
// few arcminutes; 5' is crossed in 0.06-0.17 s) in 6-34 frames at 100-200 fps, under 2% of a
// 2000-frame capture (the trail tests: 3.3%) -- a trail does not reach 10% unless dozens cross;
// a surface is in the field whenever the sky is clear: the three NAS Moons
// in every frame, a cloudy night in a third or half of them (the review's probes: 30-48%). Under
// the position rule above, 10% only has to refuse trails that also stay put. A frame-filling,
// position-stable target found in fewer frames than this is diagnosed NoTarget ("cloud or gaps?"),
// never TargetLeavesFrame; at or over it, the frames without a target are just not used ("without
// a target (cloud or gaps?)").
constexpr double kFullFramePresence = 0.1;
// The per-frame half of the box rule: a found target whose box touches an edge of the lumaWidth x
// lumaHeight luma frame, spans at least kFullFrameSpan of its width or height, and at least
// kFullFrameMinSide of both. Per frame, a tilted trail can pass it; only the capture-level rule
// (share, position and presence) tells a trail from a surface.
bool FillsFrame(const FrameStats& s, int lumaWidth, int lumaHeight);
// Upper fence on log quality (frames over it can become suspects, step 5), in robust standard
// deviations (1.4826 MAD) above the median; the robust sd is floored at kFenceMinLogSd (1%:
// identical frames still differ by that much in quality from frame to frame) and nothing is fenced
// in fewer than kMinFenceFrames frames.
constexpr double kFenceSigmas = 6;
// ... and only the frames above a gap of at least this many robust sd among them (step 5) -- unless
// the band of this many robust sd just under the fence holds fewer frames than the fence has above
// it, when every frame above the fence goes (step 5).
constexpr double kFenceGapSigmas = 2;
constexpr double kFenceBandSigmas = 2;
constexpr double kFenceMinLogSd = 0.01;
constexpr int kMinFenceFrames = 30;
// Deconvolution grid and EM budget (step 6). EM never converges on this problem (the NPMLE is a
// spiky discrete distribution, reached only in the limit), so the fixed budget is a deliberate
// regulariser: early-stopped EM from a uniform start is a smoothed G. Measured 2026-10-21 (3000
// steady frames, true limb blur 12 px; 10 seeds): the winner's curse removed from q_ref at 6% / 10%
// noise is 63% / 59% at 50 steps, 76% / 74% at 300, 87% / 86% at 3000 -- but the selection gets
// worse as G sharpens (oracle-relative score at blur sd 0.2, 5% noise: mean .846 / .841 / .829,
// minimum .823 / .815 / .741 at 50 / 300 / 3000). 300 balances the two.
constexpr int kPriorGrid = 64;
constexpr int kPriorIterations = 300;
// The frequency the selection optimises, as the contrast the best frames keep there: e^-3 = 5%.
constexpr double kDetailDepth = 3.0;
// Focus / seeing: blurred when the best frames' limb blur is at least kBlurLimit of the radius.
constexpr double kBlurLimit = 0.125;
// Discs with a half-contrast radius (FrameStats::halfRadius) under this (luma px) are judged with
// the sampling correction and floor below, and again by the region stage.
constexpr double kMinJudgedRadius = 15;
// A disc of at least kMinJudgedRadius is refused only when grossly blurred: sigma(q_ref) >= this times
// its half-contrast radius (FrameStats::halfRadius), twice the highest in-focus reading measured and
// reached on real Jupiters only past a true blur of ~0.3 R (step 7). Diagnoses refuse the image, and
// a false refusal of a good capture is worse than a missed one (Ruling P15's rule for the region stage).
constexpr double kGrossBlurLimit = 0.25;
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
// 2. Full-frame or disc (Rulings P2a and P7, constants above): full-frame when FillsFrame holds in
//    at least kFullFrameShare of the target frames, their box centres stay within kFullFrameMaxDrift
//    and the target is found in at least kFullFramePresence of all frames. Disc: frames whose box
//    touches an edge are dropped (never a cut limb, Review Focus 1); fewer than floor left ->
//    TargetLeavesFrame -- unless the target frames are frame-filling and position-stable, found in
//    too few frames to pass the presence rule: then NoTarget ("cloud or gaps?").
//    Full-frame: every frame is cut by the frame; usable parts are the region stage's job (Task 5);
//    the frames without a target are not used, worded "without a target (cloud or gaps?)".
// 3. Clipped frames (clippedFraction >= kClippedFrame) are dropped; fewer than floor -> Clipped.
// 4. Frames with quality 0 ("without detail") are dropped; fewer than floor with detail ->
//    OutOfFocus.
// 5. Upper fence -> suspects (Ruling P6). Frames with log q > median + kFenceSigmas * max(1.4826
//    MAD, median relative qualitySigma, kFenceMinLogSd) are candidates. Only with at least
//    kMinFenceFrames frames, only when the candidates number fewer than ceil(sqrt(N)) (a larger
//    group is a real mode), and never leaving fewer than floor. The suspects among them -- frames
//    that may be glitches (torn frames, a satellite in the box) or may be a small group of genuinely
//    lucky frames; quality cannot tell which -- go to Selection::suspects, not to the ranking:
//    a. All of them when the band of kFenceBandSigmas robust sd just under the fence holds fewer
//       frames than the fence has above it. A real population whose top crosses the fence (a sharp
//       mode straddling it, a lucky tail) declines through it, so it has at least as many frames
//       just under the fence as over it: an exponential tail of scale s puts e^(w/s) - 1 times as
//       many frames in the band of width w under the fence as above it, >= 1 for s <= w / ln 2 =
//       2.9 robust sd (a Laplace tail's s is ~1 robust sd, a normal tail is far lighter). Glitches
//       are a sparse population of their own: spread from the bulk's edge to far above it, they
//       shield one another from the gap rule (b) but leave the band nearly empty. Measured
//       2026-10-21 (50 seeds each): a 600-frame sharp mode straddling the fence of 3000 frames has
//       461-528 frames in the band against 16-106 above; glitch clusters (50 or 40 spread over
//       q 1.05-1.6 / 1.1-1.5 on 3000 frames, a 10-rung ladder on 300) leave 1-14 in the band
//       against 8-49 above. A Laplace tail in log q (200 seeds, N 300 / 3000) is fenced no more
//       often than without this rule (0.1-0.2 frames per capture).
//    b. Otherwise the candidates above the lowest gap of at least kFenceGapSigmas robust sd between
//       consecutive frames (sharpest first, down to the sharpest frame under the fence): a compact
//       group standing apart from the continuous top of a real mode.
//    So a real sharp group of fewer than ceil(sqrt(N)) frames above the fence becomes suspect too
//    (by count or shape it cannot be told from glitches; measured in review: 50 frames 8 sd above
//    3000, no noise, 44 of them suspect) -- and being suspect costs it nothing when it aligns:
//    AdmitSuspects puts every suspect whose global alignment confidence passes the clean frames'
//    AlignmentCut back into the stack. A torn or foreign frame fails that cut (Align.h: real torn
//    frames 0.51-0.81 of the median confidence against a cut of 0.85; another scene 0.05-0.27,
//    under the 0.5 floor); a glitch that aligns well (a sharp frame with a
//    satellite crossing it) gets in, where per-region ranking and the stack's outlier rejection
//    still apply. Suspects do not enter steps 6-8: q_ref and the kept count are the clean frames'.
//    A glitch under the fence (within kFenceSigmas of the median) is not suspect by this rule and
//    ranks with the rest.
// 6. Deconvolution (Ruling P5). Each observed q_i = true quality + noise N(0, qualitySigma_i^2).
//    The distribution G of the true quality is estimated by nonparametric maximum likelihood
//    (Kiefer-Wolfowitz NPMLE, heteroscedastic; EM on kPriorGrid points over the observed range);
//    each frame's posterior over G then gives its expected true quality
//    and, in step 8, its expected contrast. G can be bimodal, so noise is removed inside each mode
//    (a single Gaussian prior cannot: in a bimodal capture the gap between the modes swamps the
//    noise). With no noise reported (qualitySigma 0, e.g. black-clipped 8-bit sky) every quality is
//    taken as exact. "The best", q_ref, is the quality the sharpest max(min(3, N), ceil(N / 100))
//    of the N frames reach in G -- the true distribution's top 1%, not the noisiest frames' (no
//    winner's curse); without noise, the raw q at that rank.
// 7. Focus / seeing, discs only (focusJudged). The disc model: a disc of radius R whose edge is
//    blurred by a Gaussian sigma (sigma << R) has quality^2 = 1 / (sqrt(pi) R sigma) (rim gradient
//    energy over disc signal energy), so sigma(q) = 1 / (sqrt(pi) q^2 R). R = the median box's
//    sqrt(w h) / 2 less the one detection block it was grown by (at least 1). The size class is R_h,
//    the median FrameStats::halfRadius (Selection::targetRadius), which a blur hardly moves:
//      R_h >= kMinJudgedRadius: judged at R_h, not at R: blurred when sigma(q_ref; R_h) >=
//           kGrossBlurLimit R_h.
//      R_h <  kMinJudgedRadius (Ruling P4): sigma' = sqrt(max(0, sigma^2 - kSamplingBlur^2)); blurred
//           when sigma'(q_ref) >= max(R / 8, kBlurFloorPixels / LumaScale). Classed by the box, a small
//           disc blurred far enough turned "large" (its box grows 1.28 sigma) and met the large-disc
//           rule before the region stage judged it: StackSynth R 8 / 11 / 14 blurred 6 / 4 / 6 px
//           reached boxes of 16-22 and were refused here (R 11 by 4 px, mono: the engine's measure
//           only warns there, 4 px over its 3 px limit = 1.33, Engine.h).
//           Measured 2026-10-22, the small rule itself did not fire on any measured frames -- its R
//           is the box's, inflated by the blur as above: S = sigma'(q_ref) / max(R / 8, kBlurFloorPixels /
//           LumaScale) reads 0.50 / 0.50 / 0.48 / 0.00 on the 2026-01-30 Jupiters 195830 / 202657 /
//           195438 and Neptune as captured (every NAS disc as captured: 0.00-0.74, the highest the
//           2022-09-08 001402 Jupiter), at most 0.93 when they are blurred by 1-24 sensor px
//           (Neptune by 1-3: 0.19-0.54; by 4 it is not found), and 0.00-0.85 on StackSynth discs of R
//           3-14 blurred 0.6-6 px (mono, limb and convolved); in the region stage's warn band (those
//           Jupiters blurred 1-3 sensor px) 0.54-0.77 -- so on measured data small discs are refused
//           for focus by the region stage's own measure (Ruling P15), not by Select before it.
//    R / 8: the coarsest planetary structure (belts, period P ~ R/4) then keeps under 1% contrast
//    (exp(-2 pi^2 sigma^2 / P^2) < 0.01 at sigma > 0.48 P).
//    Why R_h for large discs (Task 6 found an R 24 disc blurred by 12 px stacked): the box sits at the
//    blurred limb's 10% contour, R + 1.28 sigma plus up to a block, so sigma(q) / R_box =
//    1 / (sqrt(pi) (q R_box)^2), and q R_box never falls below ~2.2 however blurred the disc (a blob
//    of width s has q ~ 1 / s and a 10% contour at ~2.15 s): measured 2026-10-22 on StackSynth discs
//    of R 15 / 24 / 60 blurred 0.6-16 px, sigma(q) / R_box peaked at 0.115 / 0.109 / 0.084, never
//    R / 8;
//    the real Jupiters below, blurred by up to 12 luma px, at 0.118-0.123. A blurred limb still
//    crosses half contrast at the limb, so R_h stays put (Measure.h) and sigma(q; R_h) / R_h keeps
//    growing with the blur. Why kGrossBlurLimit, not R / 8, there: the reading is not the true blur
//    over R -- texture raises q in focus and limb darkening lowers it once the texture is gone, so a
//    real Jupiter softened by only 0.5-1 luma px already reads 0.09-0.20 -- and a refusal must be
//    safe. Measured 2026-10-22 (q_ref as here): the four largest discs on the NAS as captured read
//    0.053 / 0.053 (2022-09-17 1080p Jupiters, R_h 21.5: the only ones in this class), 0.125
//    (2022-08-03, R_h 14.5), 0.081 (2026-01-30-195137, R_h 12) -- the limit is twice the highest (43
//    planetary files surveyed, none refused for focus or seeing). The same four, defocused
//    by a Gaussian of sigma 0.5 / 1 / 1.5 / 2 / 3 / 4 / 6 / 8 luma px (stack-probe --blur, own noise
//    put back): 2022-09-17 0.093 / 0.139 / 0.150 / 0.159 / 0.184 / 0.202 / 0.249 / 0.288 and
//    0.094 / 0.142 / 0.161 / 0.170 / 0.187 / 0.215 / 0.251 / 0.288; 2022-08-03 0.160 / 0.196 / 0.206 / 0.217 / 0.238 / 0.262 / 0.318 /
//    0.343; 195137 0.096 / 0.119 / 0.147 / 0.173 / 0.237 / 0.282 / 0.343 / 0.362 -- refused from a
//    true blur of 0.28-0.37 R on; at 0.43-0.57 R they read 0.32-0.35, 1.3-1.4x over the limit.
//    StackSynth (unit test, 60 frames, noise 0.01 unless said): R 24 blurred 0.6 / 2 / 3 px 0.027 /
//    0.079 / 0.113, R 40 by 4 px 0.084 (stacked); R 24 by 12 px (Task 6's case) 0.274 (refused);
//    convolved (true defocus) at noise 0.005: R 24 by 2 px 0.086, R 40 by 3 px 0.074 (stacked), R 24
//    by 12 px 0.325, R 40 by 20 px 0.305 (refused). Limit: at noise 0.01 the convolved R 24 / 12 px
//    reads 0.267 (refused) but R 40 / 20 px 0.181 (stacked): EstimatePixelNoise reads s^2 4.3-5.2%
//    low (its 20th-percentile tile pick; measured on white noise), so q keeps ~0.18 s^2 per pixel of
//    noise in its gradient term, which dominates once a spread-out blob's own gradients are weaker,
//    and q stops falling. A defocus that grows a disc into a faint blob (sigma ~ R and over) reads
//    sharper again for the same reason, and a centre that dims moves R_h out. Missed, such a
//    capture stacks into a visibly soft image -- the failure the rule prefers.
//    The blur spread is that of the true quality: tau^2 = max(0, var(q) - mean(qualitySigma^2)),
//    spread = (sigma(mu - 1.2816 tau) - sigma(mu + 1.2816 tau)) / sigma(mu); its bounds use tau^2 -+ 2 SE, SE = sqrt(2 / N) var(q).
//    Blurred -> OutOfFocus ("steady") when even the upper bound (tau^2 + 2 SE) gives a spread under
//    kSteadySpread; PoorSeeing when even the lower bound (tau^2 - 2 SE) gives one at or over it;
//    otherwise -- or under kMinJudgedFrames frames -- OutOfFocus worded "focus or seeing, ... to
//    tell". Measured (200 seeds per cell, N = 100 / 500 / 3000, noise 3-15%): steady blur is called
//    seeing in at most 3 of 200; 6% seeing under 3% noise is found in 185 / 200 / 200 of 200.
//    No genuinely defocused or bad-seeing capture is on the NAS: the large-disc limit rests on the
//    in-focus survey and the synthetic defocus above; the small-disc one on the reasoning above.
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
//    blur log-sd 0.2 / 0.2 / 0.3, quality noise 2 / 5 / 10%, 10 seeds): S of the chosen set is on
//    average 0.967 / 0.841 / 0.644 of the oracle's best (minimum 0.951 / 0.815 / 0.602).
//
// Explanation (one ASCII line, as FITS HISTORY needs):
//   "used 612 of 4088 frames: the frames judged sharpest after allowing for measurement noise,
//    quality >= 0.91 of the best; not used: 3 without a target, 12 at the frame edge, 3459 below
//    the cut, 2 suspect frames (alignment decides)"
// -- every frame accounted for (groups: without a target -- "(cloud or gaps?)" added for a
// full-frame target --, stray detection(s), at the frame edge, clipped, without detail, below the
// cut, suspect frame(s) (alignment decides); empty groups omitted), plus "; frame-filling target,
// each region ranks its own frames" for a full-frame target. "Judged sharpest": with noise, a frame
// left out can have a higher raw quality than one kept. The quality bound is the lowest kept raw
// quality over q_ref, rounded down. With suspects, the line is provisional: AdmitSuspects restates
// it.
Selection SelectFrames(const std::vector<FrameStats>& stats, const ser::FileInfo& info);

// The suspects' outcome (Ruling P6).
struct Admission {
    std::vector<int64_t> frames;    // selection.frames plus the admitted suspects, ascending
    std::vector<int64_t> admitted;  // suspects whose confidence passed alignCut, ascending
    std::vector<int64_t> rejected;  // the others, ascending
    std::string explanation;        // the selection's line restated (below)
};

// Decides the suspects once they are aligned: suspectShifts[i] is GlobalAligner::Align's result for
// selection.suspects[i] against the reference built from the clean frames (BuildReference never
// sees a suspect), and alignCut is AlignmentCut over the clean frames' shifts. A suspect is admitted
// when its confidence >= alignCut (a NaN confidence is rejected). An admitted suspect joins the
// stack like any kept frame -- it is sharper than every one of them -- without re-running steps 6-8:
// q_ref and the kept count stay the clean frames' (with the admitted frames on top, S(k)'s optimum
// could sit at a slightly smaller k; per-region ranking, Task 5, makes the finer choice anyway).
//   "used 3047 of 3050 frames: the frames judged sharpest ..., quality >= 0.88 of the best; not
//    used: 3 rejected by alignment; 50 suspect frames (sharper than the rest): 47 admitted after
//    alignment, 3 rejected"
// The quality bound is unchanged (admitted suspects are above it). Without suspects, the result is
// the selection's frames and explanation as they are. Throws std::invalid_argument when
// suspectShifts.size() != selection.suspects.size(), or unless alignCut >= kMinAlignConfidence
// (Align.h; AlignmentCut never returns less, so a lower, zero, negative or NaN cut is a caller bug
// that would let torn or foreign frames in) -- with or without suspects.
// The line counts SELECTION only: "rejected by alignment" there means suspects. Clean frames (in
// selection.frames) that later fail AlignmentCut are still in Admission::frames and are not
// mentioned; the caller reports them in StackSummary.alignment, never by editing this line.
Admission AdmitSuspects(const Selection& selection, const std::vector<Shift>& suspectShifts, double alignCut);

// Steps 5, 6 and 8 of SelectFrames on bare samples -- the upper fence, the deconvolution and the
// "maximise sum MTF / sqrt(k)" count -- for a caller that ranks something other than whole frames
// (Task 5 ranks each region's frames with it, so a region keeps its own best frames by the same
// rule). SelectFrames itself runs through it. Positions refer to the input vectors; ties break by
// position, so pass samples in frame order to break them by frame index.
//
// Suspects (Ruling P6) are a frame-level idea and belong to SelectFrames alone: it turns `fenced`
// into Selection::suspects, and AdmitSuspects lets alignment decide them. A region has no suspects:
// its fenced samples are simply not kept (they are neither in `ranked` nor counted in `keep`), the
// same outcome as before P6. A region sample reaches the ranking only after passing the region's
// local-alignment confidence cut (PlanRegions), so there is no further alignment test that could
// admit it; per-region stacking stays a pure ranking.
enum class FenceRule {
    None,  // nothing fenced
    Band,  // 5a: too few samples in the band just under the fence -- every candidate fenced
    Gap,   // 5b: the candidates above the lowest gap of kFenceGapSigmas robust sd
};
struct DetailRanking {
    std::vector<size_t> left;    // positions not fenced off (step 5), ascending
    std::vector<size_t> ranked;  // the same positions, highest expected contrast first (step 8's order)
    std::vector<double> mean;    // per ranked entry: posterior mean of the true quality
    std::vector<double> mtf;     // per ranked entry: expected contrast at the detail frequency
    size_t outliers = 0;         // fenced off (step 5) == fenced.size()
    double qRef = 0;             // "the best" (step 6)
    size_t keep = 0;             // keep ranked[0 .. keep): step 8's count, at least min(need, left)
    // Step 5's outcome, for SelectFrames' suspects (regions ignore it):
    std::vector<size_t> fenced;  // positions fenced off, ascending; left and fenced partition the input
    FenceRule fenceRule = FenceRule::None;  // which rule fenced them (None iff fenced is empty)
    int64_t fenceBand = 0;       // 5a: the samples in the band just under the fence; 0 otherwise
    double fence = 0;            // the fence in quality units (exp of the log fence); 0 for empty input
};
// `quality` must be finite and > 0 (std::invalid_argument otherwise, or when the sizes differ);
// a non-finite or non-positive sigma counts as 0 (exact). need, floorCount: as in SelectFrames.
// Empty input gives an empty ranking.
// applyFence false skips step 5 (nothing is fenced; `fence` is still reported): for samples that have
// already passed an alignment check, where a quality far above the rest is a lucky moment, not a
// glitch (Ruling P8, PlanRegions).
DetailRanking RankByDetail(const std::vector<double>& quality, const std::vector<double>& sigma, int64_t need,
                           int64_t floorCount, bool applyFence = true);

}  // namespace planetary::stack
