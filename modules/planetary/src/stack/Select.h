#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "FrameSource.h"
#include "Measure.h"

namespace planetary::stack {

// Why a capture gives no image (spec "Diagnosis"). None: frames were selected. SelectFrames makes the
// first five; only Unaligned (too few frames align to the reference) is made by RunStack, after
// alignment (Engine.h). OutOfFocus and PoorSeeing come only from SelectFrames (steps 4 and 7): the
// region-stage focus check warns and never refuses (Ruling P20).
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
    // quality is texture, not a limb, so its focus is left to the region stage (Ruling P4), which
    // only warns (Ruling P20) -- a frame-filling target is never refused for focus.
    bool focusJudged = false;
    // Step 7's size class when focusJudged, 0 otherwise: the median FrameStats::halfRadius (luma px),
    // which a blur hardly moves (the box radius grows with it). The region stage judges every disc
    // too, but only to warn (Ruling P20); step 7's large-disc rule (halfRadius >= kMinJudgedRadius) is
    // the only refusal of a disc for gross defocus. Small discs' own rule did not fire on any measured
    // capture (step 7), so in practice a small disc is never refused for focus.
    double targetRadius = 0;
    // Step 7's reading when focusJudged (0 otherwise): the sharpest frames' limb blur over the radius it
    // is judged at, and the limit it is judged against, both as a fraction of that radius -- refused
    // when focusReading >= focusLimit. focusQuality: the quality the blur was read at (step 7).
    double focusReading = 0, focusLimit = 0, focusQuality = 0;
    // Step 7's warn level (the same units; 0 unless focusJudged) and, when the reading is at or over it and
    // under the limit, the focus warning (Ruling P15b; RunStack adds it to the summary's warnings unless
    // the region stage warns too); empty otherwise.
    double focusWarn = 0;
    std::string focusWarning;
};

// Fewest frames worth stacking: outlier rejection (satellite, hot pixel) needs a handful of samples
// per pixel to tell an outlier from the rest.
constexpr int kMinStackFrames = 5;
// Stray detections: a target frame counts only when its box width and height are each within this
// factor of the capture's median box width and height (a satellite streak's box is a frame-wide
// line; a disc half cut by the edge still passes).
constexpr double kBoxTolerance = 2.0;
// A frame is clipped when this fraction of its target pixels' sensor samples is at full scale
// (FrameStats::clippedFraction): a quarter of a disc is its bright centre, where the detail is. Set
// 2026-10-21 (9b2b92da) on per-pixel luma, when every capture here was read as mono: Neptune 0.08-0.10
// and the 1080p Jupiter 0.12 (both usable), the 2023-02-11 Venus 0.37 in every frame (saturated). Re-measured
// 2026-10-23 on the samples (E2, focus-survey --dump-stats, every frame), the quantity the 2 x 2 cell means of
// Ruling P9 had lost (they read Venus 0.109-0.357, 1558 of 6485 frames clipped, and both others 0): the
// 2026-01-30 Neptune 204515 0.048-0.087 (median 0.068) and Jupiter 195137 (1080p) 0.113-0.123, both
// usable; the Venus 0.239-0.452 (median 0.352), 6483 of 6485 frames clipped, diagnosed Clipped.
// The edge: one colour saturated over the whole target reads 0.25 for red or blue (a quarter of the sites;
// RGB weighs its channels 1 : 2 : 1 the same way, MeasurementImage) and is dropped (>=), so a disc whose
// red clips everywhere -- a Mars exposed for green -- is refused, as per raw pixel it always was; red
// clipped over less than all of the target stays under the limit. The one Mars on the NAS (2023-02-11 192403,
// run-focus-survey.sh's list) reads at most 0.016 over its 13250 frames (2026-10-23, focus-survey --dump-stats).
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
// Focus / seeing, a small disc: blurred when the best frames' limb blur is at least kBlurLimit of the radius
// (and kBlurFloorPixels). Twice the highest in-focus small-disc reading of the survey (step 7): 0.119 R,
// the 2022-08-03 015840 Jupiter's first 30 frames (Ruling P26 fix round 2; R / 8 before, set on readings
// the noise had lowered).
constexpr double kBlurLimit = 0.24;
// Discs with a half-contrast radius (FrameStats::halfRadius) under this (luma px) are judged with
// the sampling correction and floor below; the region stage judges them too, warn-only (Ruling P20).
constexpr double kMinJudgedRadius = 15;
// A disc of at least kMinJudgedRadius is refused only when grossly blurred: sigma(q_ref) >= this times
// its half-contrast radius (FrameStats::halfRadius). Twice the highest in-focus large-disc reading of the
// survey (step 7), rounded up to 2 decimals: 2 x 0.1388 (the 2022-09-17 230420 Jupiter, all 761 frames) =
// 0.2776 -> 0.28 (Ruling P15a: P15 binds as written; 0.25 before, 1.80x). A defocus that reads under it is
// stacked, not refused, and the region stage warns where its limb reading is over kDiscWarnAbove (Engine.h):
// a false refusal of a good capture leaves the user nothing, a missed moderate defocus still gives an image
// (Rulings P15, P15a and P20: refuse only on gross failure, and only here -- the region stage never refuses).
constexpr double kGrossBlurLimit = 0.28;
// Step 7's warn band (Ruling P15b): a disc whose reading is at or over this and under its refuse limit is
// stacked with a focus warning ("may be out of focus ... check the focus"). 1.5x the surveyed in-focus
// maximum, rounded up to 2 decimals: large 1.5 x 0.1388 = 0.208 -> 0.21; small 1.5 x 0.1189 (the
// 2022-08-03 015840 Jupiter's first 30 frames) = 0.178 -> 0.18, with kBlurFloorPixels as in the limit (a
// limb under it can be a focused tiny disc, so neither warned nor refused). The region stage's own disc
// warning (Engine.h kDiscWarnAbove) cannot do this for a large disc: its measure reads in-focus Neptune at
// 0.382 and the defocused large Jupiters at 0.385-0.399 (Ruling P17: no warn band there); the gross
// reading puts the in-focus maximum at 0.139 and every known defocus at 0.247 and over (step 7).
constexpr double kGrossBlurWarn = 0.21;
constexpr double kBlurWarn = 0.18;
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
// Step 7's gross reading narrows q_ref to the spread the data support only at low per-frame SNR (median
// quality / qualitySigma): fully at kShrinkSnrFull and under, not at all from kShrinkSnrNone, log-linear
// between (step 7). Measured: the faint blob it is for reads 3.4 (calibration/mdq.cpp); every NAS disc 26.9 and
// over, short or long (focus-survey's r50: the 2022-08-03 021039 Saturn's first 60 frames the lowest).
constexpr double kShrinkSnrFull = 5;
constexpr double kShrinkSnrNone = 15;

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
//    AlignmentCut and is in one piece (PassesAlignment) back into the stack. A torn or foreign frame
//    fails (Align.h: real torn frames 0.51-0.81 of the median confidence against a cut of 0.85, and
//    their halves 3.46-4.35 px apart against kMaxTear's 3; another scene 0.05-0.27, under the 0.5
//    floor); a glitch that aligns well (a sharp frame with a
//    satellite crossing it) gets in, where per-region ranking and the stack's outlier rejection
//    still apply. Suspects do not enter steps 6-8: q_ref and the kept count are the clean frames'.
//    A glitch under the fence (within kFenceSigmas of the median) is not suspect by this rule and
//    ranks with the rest.
// 6. Deconvolution (Ruling P5). Each observed q_i^2 = true quality^2 + noise N(0, quality2Sigma_i^2),
//    seen only where it came out positive (a frame of quality 0 is not ranked): the likelihood at a true
//    quality u is N(q_i^2; u^2, rho_i^2) / Phi(u^2 / rho_i) (Ruling P26). quality^2 is the measure's own
//    ratio, linear in the noise, so its sd does not move with the frame's own noise draw; a Gaussian in q
//    with sd qualitySigma_i (which is ~1 / q_i) trusted the frames that drew high noise most, and lifted
//    the top of G: on a faint blob (R 24 convolved by 24 px over an unclipped sky, noise 0.01, ~3.5
//    qualitySigma per frame) q_ref reads 0.0485 / 0.0425 / 0.0450 for seeds 7 / 107 / 207 with the
//    quality-domain likelihood against 0.0477 / 0.0384 / 0.0367 with this one (noise-free 0.0371;
//    test/calibration/seedsel.cpp, mdq.cpp).
//    Samples without quality2Sigma (synthetic ones) keep the Gaussian in q with qualitySigma.
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
//           kGrossBlurLimit R_h; warned (stacked, Selection::focusWarning) from kGrossBlurWarn R_h.
//      R_h <  kMinJudgedRadius (Ruling P4): sigma' = sqrt(max(0, sigma^2 - kSamplingBlur^2)); blurred
//           when sigma'(q_ref) >= max(kBlurLimit R, kBlurFloorPixels / LumaScale); warned from
//           max(kBlurWarn R, kBlurFloorPixels / LumaScale). Classed by the box, a small
//           disc blurred far enough turned "large" (its box grows 1.28 sigma) and met the large-disc
//           rule before the region stage judged it: StackSynth R 8 / 11 / 14 blurred 6 / 4 / 6 px
//           reached boxes of 16-22 and were refused here (R 11 by 4 px as PoorSeeing): the size
//           class moved with the blur being judged.
//           Measured (test/run-focus-survey.sh survey / defocus): S = sigma'(q_ref) / max(kBlurLimit R,
//           kBlurFloorPixels / LumaScale) reads at most 0.495 on every NAS disc as captured, at full length
//           and over its first 8 / 30 / 60 / 120 frames (the 2022-08-03 015840 Jupiter's first 30 frames;
//           0.42 at full length; 0.47 the 2022-09-08 001402 Jupiter, which read the highest at 851ba41b --
//           0.74 at R / 8, step 7's reading off its FrameStats there -- and 0.84 at R / 8 now), and at most
//           0.51 blurred by 1-16 sensor px (the 2026-01-30 195137 Jupiter at 16 px); StackSynth's small
//           disc smeared over 5 px is refused on the floor. So on measured data a small disc is never
//           refused for focus -- 1.95x under the limit at the most -- the region stage only warns
//           (Ruling P20), and a disc defocused far enough to grow its half-contrast radius past
//           kMinJudgedRadius meets the large-disc rule.
//    kBlurLimit was R / 8 until Ruling P26 fix round 2 -- the coarsest planetary structure (belts, period
//    P ~ R/4) then keeps under 1% contrast (exp(-2 pi^2 sigma^2 / P^2) < 0.01 at sigma > 0.48 P) -- but in
//    focus a measured disc read up to 0.95 of R / 8 with the noise corrected, so P15's 2x sets it instead.
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
//    safe. Surveyed again with Ruling P26 (fix round 2; 851ba41b -> head, reading as here): the same 46
//    planetary files, none refused for focus or seeing either way. ("Before P26" below: at 851ba41b, step
//    7's reading taken off that build's MeasureAll FrameStats by a scratch port of focus-survey, which needs
//    Selection::focusReading; limb-probe recomputes its 0.053 at head, from each frame's own-sky quality.)
//    The large class (Ruling P15a widened the search: every AVI / SER on the NAS, 2026-10-22 -- the rest
//    are small, frame-filling, or hold no disc; the 2026-01-30 195830 Jupiter is R_h 8, the 2023-02-11
//    191831 Jupiter R_h 5, the largest small disc the 2022-08-03 015840 Jupiter at R_h 14.2-14.5, reading
//    0.098-0.119) is the two 2022-09-17 1080p Jupiters (R_h 21.5), as captured and over their first 8 /
//    30 / 60 / 120 frames (test/run-focus-survey.sh):
//      225418  606 frames 0.1182 (0.053 before P26);  8 / 30 / 60 / 120: 0.1307 / 0.1235 / 0.1314 / 0.1311
//      230420  761 frames 0.1388 (0.053 before P26);  8 / 30 / 60 / 120: 0.1115 / 0.1340 / 0.1301 / 0.1318
//    kGrossBlurLimit = 2 x 0.1388 rounded up = 0.28: the in-focus readings 2.02x under it at the least,
//    where they read 4.7x under the old limit before P26. The old readings were low: on 225418 each
//    frame's quality with its sky's noise alone read a mean 0.147 against 0.093 for a stack of its
//    sharpest 303 frames (near noise-free) and 0.098 with the capture's model -- the disc's own shot
//    noise kept as detail -- and the limb fitted on that stack (test/limb-probe.cpp) is 3.9-4.3 luma px
//    wide, 0.18-0.20 R_h (the reading at q_ref: 0.109 now, 0.053 before).
//    Defocused (stack-probe --blur, the capture's own noise put back; 2 / 4 / 6 / 8 / 12 / 16 sensor px, 1-8
//    luma px on these Bayer captures) the readings did not move: 225418 0.143 / 0.162 / 0.188 / 0.203 /
//    0.252 / 0.294 (0.139 / 0.159 / 0.184 / 0.202 / 0.250 / 0.289 before), 230420 0.151 / 0.172 / 0.199 /
//    0.216 / 0.247 / 0.279 (0.251 / 0.290 before at 12 / 16), the 2022-08-03 Jupiter at 16 px (R_h 15, large)
//    0.350 (0.349) -- a blurred limb's noise is small against it. StackSynth: R 24 by 10 px 0.267-0.290
//    over seeds 7, 107, ..., 907, every other refused case 0.300 and over. So the margin closed from above,
//    not below, and at 0.28 a moderate defocus is no longer refused: 225418 at 6 luma px (0.252), 230420 at
//    6 and 8 (0.247, 0.279) and R 24 by 10 px on seeds 7 / 507 / 707 / 807 / 907 (0.267-0.278) stack, where
//    0.25 refused all but 230420 at 6. Each is warned by the warn band (Ruling P15b): at or over kGrossBlurWarn
//    0.21 (kBlurWarn 0.18 R for a small disc, with the floor) and under the limit, the selection carries a
//    "may be out of focus ... check the focus" warning, which RunStack reports unless the region stage warns
//    too (one warning, the region stage's). The region stage alone could not do it (Engine.h; RunStack, all
//    frames): 230420 at 6 / 8 luma px reads 0.409 / 0.473 there, 225418 at 6 0.385, R 24 by 10 px 0.418-0.423
//    on seeds 7 / 507 / 707 and 0.399 / 0.394 on 807 / 907, against its warn level 0.40 and in-focus Neptune's
//    0.382. In focus nothing reaches the band: the largest in-focus reading is 0.66 of it (230420, 0.1388),
//    and a small disc's 0.66 too (0.1189 of 0.18 R). Mildly soft, 230420 at 4 luma px (0.216) is warned,
//    225418 there (0.203) not. Refused still: 225418 at 8 luma px (0.294), the 2022-08-03 Jupiter at 8 (0.350),
//    R 24 by 10 px on seeds 107-407 and 607 (0.280-0.290), every other StackSynth case above.
//    StackSynth (the unit test's MeasuredDisc, 60 frames, noise 0.01 unless said, seed 7; at head,
//    test/calibration/mdq.cpp's "reading"): R 24 blurred 0.6 / 2 / 3 px 0.027 / 0.080 / 0.116, R 40 by 4 px
//    0.089 (stacked); R 24 by 10 px 0.267 (stacked and warned; refused at 0.25), by 12 px (Task 6's case)
//    0.337 (refused); convolved (true defocus) at noise 0.005: R 24 by 2 px 0.086, R 40 by 3 px 0.075
//    (stacked), R 24 by 12 px 0.350, R 40 by 20 px 0.341 (refused); at noise 0.01 R 24 by 2 px 0.088, R 40
//    by 3 px 0.074 (stacked), R 24 by 12 px 0.364 (refused). Noise-free, the convolved R 24 / 12 px reads
//    0.334, R 40 / 20 px 0.342 and R 24 / 24 px 0.400. Before Ruling P19's unbiased pixel noise, s^2 read
//    low, q kept part of the noise in its gradient term -- which dominates once a spread-out blob's own
//    gradients are weaker (a few percent of the noise's 4 s^2) -- and q stopped falling: the faint blobs were
//    stacked. Over a sky of 0.06 at noise 0.01 (seeds 7 / 107 / 207): R 40 / 20 px 0.323 / 0.339 / 0.335,
//    R 24 / 24 px 0.520 / 0.419 / 0.434 (refused), and R 24 / 12 px over seeds 7, 107, ..., 907 0.328-0.354,
//    1.17-1.26x the limit. With s measured on the sky and taken as the target's, the reading was low again
//    where they differ: MakeDisc's own sky of 0.02 is 2 noise sd above black at noise 0.01, the 16-bit
//    capture clips 2.3% of it at 0, and the clipped sky's noise variance is 0.960 of the target's (as was a
//    target's own shot noise above the sky's) -- there the convolved R 40 / 20 px was stacked (at 851ba41b,
//    whose MeasureAll took the sky's level as every pixel's: step 7's reading off its FrameStats, 0.240). The
//    capture's noise model (Ruling P26, Noise.h) gives every pixel the noise of its level, and qualitySigma
//    is the scatter's own -- which exposed q_ref's own noise: a deconvolution of ~55 frames at a median 3.4
//    qualitySigma reads a spread of the true quality its 2-SE bound does not support, and its top lifts q_ref
//    up to 29% (R 24 / 24 px over the 0.06 sky, seed 7: 0.0477 against 0.0371 noise-free; read there, 0.221
//    -- stacked at 941ab4e7, which did; test/calibration/seedsel.cpp's X). So the limb blur is read at the
//    narrowest spread the data cannot rule out: q_ref's excursion above the single quality that best
//    explains every frame (DetailRanking::pointQuality) is scaled by tauLo / G's sd when G is wider than
//    tauLo (Ruling P26 fix round 1) -- only where the frames' own SNR leaves G's top to EM's early-stopped
//    smoothing: fully at a median quality / qualitySigma of kShrinkSnrFull and under, not at all from
//    kShrinkSnrNone (fix round 2). The faint blob reads a median 3.4 (calib-mdq's "median r"); every NAS disc
//    26.9 and over, full length or its first 8-120 frames (focus-survey's r50), where tauLo reaches 0 on a short
//    capture (at e6d0f4ca, which read every capture at the point quality, the 2022-09-17 225418 Jupiter's
//    first 60 frames read 0.140 against 0.131 at q_ref: test N1's fixture); StackSynth's other refused cases
//    12.9 and over. Over seeds 7, 107, ..., 907 (calib-seedsel): R 24 / 24 px 0.336-0.801 (0.293-0.343 at
//    851ba41b and 0.222-0.511 at 941ab4e7, step 7's reading off their FrameStats), refused in all 10; the
//    clipped-sky R 40 / 20 px 0.313-0.372 (calib-mdq), refused in all 10; the in-focus cases above as at
//    q_ref. A centre that dims (blur ~ R and over) also moves R_h out. Missed by a little, such a capture
//    stacks into a visibly soft image with a focus warning (the warn band under the limit, kGrossBlurWarn) --
//    the failure the rule prefers.
//    The blur spread is that of the true quality, read in quality^2 when every frame carries
//    quality2Sigma (measured frames: its noise is linear and its sd rho first-order exact), else in
//    quality: tau^2 = max(0, var(x) - mean(sd^2)), x = q^2 (sd rho) or q (sd qualitySigma); spread =
//    (sigma(mu - 1.2816 tau) - sigma(mu + 1.2816 tau)) / sigma(mu), sigma the limb blur at x; its bounds
//    use tau^2 -+ 2 SE, SE = sqrt(2 / N) var(x). Blurred -> OutOfFocus ("steady") when even the upper
//    bound (tau^2 + 2 SE) gives a spread under kSteadySpread; PoorSeeing when even the lower bound
//    (tau^2 - 2 SE) gives one at or over it; otherwise -- or under kMinJudgedFrames frames -- OutOfFocus worded "focus or seeing, ... to
//    tell". Measured (200 seeds per cell, N = 100 / 500 / 3000, noise 3-15%), synthetic frame statistics
//    (quality Gaussian, qualitySigma exact; R 60, limb 24 px; test/calibration/synthrate.cpp): steady blur
//    is called seeing in at most 3 of 200; 6% seeing under 3% noise is found in 185 / 200 / 200 of 200. (The
//    protocol's limb was 20 px, 0.33 R, until Ruling P15a: at kGrossBlurLimit 0.28 the sharpest frames of 6%
//    seeing read under the limit and 13 / 32 / 11 of 200 were stacked, not judged.) Measured through
//    MeasureFrame (test/calibration/classrate.cpp with PLAIN: a texture-free disc of R 24 by 12 px, white
//    noise for 3.2 / 7 / 15% quality noise, the true noise model): steady blur called seeing in 0 of 200 in
//    every cell (Ruling P26 fix round 1, with the spread read in quality as 941ab4e7 read it: 5 / 10 / 28 at
//    15% noise); 6% seeing under 3.2% noise found in 145 / 199 / 200 (1 of 200 stacked at N = 500) -- 176 /
//    191 / 199 at the blur spread that gives quality the protocol's 3% spread, where 4 / 9 / 1 of 200 now
//    read under kGrossBlurLimit and are stacked (the rendered disc's quality moves 0.9x the disc model's
//    with its blur: classrate's QSPREAD).
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
//   "selected 612 of 4088 frames: the frames judged sharpest after allowing for measurement noise,
//    quality >= 0.91 of the best; not selected: 3 without a target, 12 at the frame edge, 3459 below
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
// when it passes alignment (PassesAlignment: confidence >= alignCut and not torn, Shift::tear <=
// kMaxTear; a NaN is rejected). An admitted suspect joins the
// stack like any kept frame -- it is sharper than every one of them -- without re-running steps 6-8:
// q_ref and the kept count stay the clean frames' (with the admitted frames on top, S(k)'s optimum
// could sit at a slightly smaller k; per-region ranking, Task 5, makes the finer choice anyway).
//   "selected 3047 of 3050 frames: the frames judged sharpest ..., quality >= 0.88 of the best; not
//    selected: 3 rejected by alignment; 50 suspect frames (sharper than the rest): 47 admitted after
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
    // The deconvolved distribution G of the true quality (step 6): its mean and sd (0 without one).
    double priorMean = 0, priorSd = 0;
    // The single true quality that best explains every sample (step 6's likelihood, G a point mass).
    double pointQuality = 0;
    size_t keep = 0;             // keep ranked[0 .. keep): step 8's count, at least min(need, left)
    // Step 5's outcome, for SelectFrames' suspects (regions ignore it):
    std::vector<size_t> fenced;  // positions fenced off, ascending; left and fenced partition the input
    FenceRule fenceRule = FenceRule::None;  // which rule fenced them (None iff fenced is empty)
    int64_t fenceBand = 0;       // 5a: the samples in the band just under the fence; 0 otherwise
    double fence = 0;            // the fence in quality units (exp of the log fence); 0 for empty input
    // Grid evaluations done by steps 6 and 8 (the likelihood table, two per sample and grid point per
    // EM step, one per sample and point for the posteriors): the cost, counted -- deterministic, so a
    // test can check it is linear in the samples without a clock (the fence and sorts add N log N).
    uint64_t work = 0;
};
// `quality` must be finite and > 0 (std::invalid_argument otherwise, or when the sizes differ);
// a non-finite or non-positive sigma counts as 0 (exact). need, floorCount: as in SelectFrames.
// Empty input gives an empty ranking.
// applyFence false skips step 5 (nothing is fenced; `fence` is still reported): for samples that have
// already passed an alignment check, where a quality far above the rest is a lucky moment, not a
// glitch (Ruling P8, PlanRegions).
DetailRanking RankByDetail(const std::vector<double>& quality, const std::vector<double>& sigma, int64_t need,
                           int64_t floorCount, bool applyFence = true);
// The measured form (Ruling P26): sigma2[i] is the sd of quality[i]^2 (FrameStats::quality2Sigma,
// RegionMeasure::quality2Sigma), and step 6's likelihood is that of quality^2 (Select.h, step 6); sigma
// still sets the fence's noise floor (step 5). Empty sigma2: the form above.
DetailRanking RankByDetail(const std::vector<double>& quality, const std::vector<double>& sigma,
                           const std::vector<double>& sigma2, int64_t need, int64_t floorCount, bool applyFence = true);

}  // namespace planetary::stack
