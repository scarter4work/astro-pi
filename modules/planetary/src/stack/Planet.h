#pragma once
#include <array>
#include <optional>
#include <string>
#include <vector>

#include "CaptureName.h"
#include "Mineral.h"

// Planet processing (spec docs/superpowers/specs/2026-10-29-planetary-planet-processing-design.md): the steps
// where a planet differs from the mineral moon. PCL-free; threads through Parallel.h, no OpenMP. Input as the
// mineral moon's: a linear colour stack, 3 interleaved channels R, G, B, and SegmentLunar's masks of it.
// Planet discs are small (Ruling P1: 12-50 px across in the acceptance captures; 2026-10-23 Saturn, rings
// included, is 36 x 17 px on a 320 x 240 ROI), so the Moon's 1%-of-frame calibration floor and 16-window channel
// alignment would skip on every one of them; these two steps replace them for planets. The Moon path is untouched.

namespace planetary::stack {

// ---- Sharpening for small discs (Ruling P1) ----
// SharpenLuminanceImage's profile for a planet (Mineral.h SharpenProfile): gains {1.2, 1, 1, 1}, extra {0.6, 0, 0, 0},
// limb fade fraction 0.15 -- layer 1 only (gain 1.6 at the default 1.5 before the Wiener factor); kept by Task 5
// on real data (see the defaults below). On a 20-40 px disc layer 2's scale is the disc's own shape (limb darkening,
// the inner half of the blurred limb): the Moon's {1.5, 2.5} / {3, 1} lifted the interior into a dome, and the disc's
// RMS error against the truth rose 675% / 257% at r 10 (blur 1 / 1.5 px) and 342% / 33% at r 20 with this fade, 236%
// at r 20 blur 1 with the Moon's; the edge rim reached 2.5-11% of the disc mean. Measured with these values
// (PlanetSynth with fine detail -- texture to 4 px periods and spots -- noise 0.005, amount 1.5; test_planet): the disc's RMS error against
// the sharp truth falls 16.7% / 4.7% at r 10 (blur 1 / 1.5 px), 15.4% / 4.5% at r 20, 5.2% / 0.8% at r 100; around
// a ringed globe of r 8 and 16 (blur 1.2) the sky side gains 0, the undershoot below the sky is at most 0.20% and
// the rim inside the limb and ring tips at most 1.25% of the disc mean (bar 2%); the sky's noise is unchanged (the
// boost is 0 off the disc). The fade fraction's own share is small but consistent: 8 seeds at blur 1, f 0.15 against
// 0 (the Moon's fade), 20.17% against 20.05% lower at r 8, 19.19% against 18.92% at r 10; no change at blur 1.5 or at
// r 6 or 12 (where 0.15 R >= 2 px, layer 1's reach, or the fade lies outside the measured interior), none in the halo.
extern const SharpenProfile kPlanetSharpen;

// ---- The planet's disc (Task 5; rulings Q12, Q14) ----
// SegmentLunar on a copy of the frame less its sky surface, a plane or a quadratic (masks only: the image the pipeline
// processes is not touched; the background step later removes a plane from it, so a curved sky's curvature stays in the
// output's background). SegmentLunar has no sky model: its sky is what lies under a quarter of the disc's level, so a
// sky over that (a faint planet, a bright sky, a gradient) was disc -- a level-0.2 planet covering 25% of a 320 x 240
// frame on a sky of 0.058 rising 0.06 read 62410 px against 19200, at level 0.1 and 42% of the frame or more the whole
// frame; and 2026-10-23 Neptune (1911 x 1071, sky 0.058 with correlated noise and a 0.008 rise down the frame, a disc of
// about 3 px radius) read 100% disc. Steps:
// 1. The sky surface. The channels' mean in 16 px blocks, each block's noise from the difference of its two 4 px
//    checkerboard halves (smooth structure cancels there, correlated noise does not). A surface fitted to the blocks,
//    the blocks more than 3 noise sigma above it leaving round by round until none does (a body only adds light, so the
//    surface settles on the sky under it however much of the frame the body covers), then the blocks within 3 sigma of
//    it either way -- once for a plane, once for the quadratic a + b x + c y + d x^2 + e x y + f y^2 (ruling Q14), each
//    from all the blocks. The quadratic is the sky when its blocks pin one down (no pivot of its normal equations under
//    1e-10 of their largest diagonal) and it keeps at least as many blocks as the plane; else the plane. Each channel's
//    surface is then fitted over those blocks' pixels (SkyPlane's rejection). Under 8 blocks, or under 8 or a tenth of
//    them on the surface, the frame has no sky to model: SegmentLunar's masks of the frame stand (a body filling it).
//    Measured, blocks on the sky: the six real captures 44-91% -- the quadratic on 2026-09-07 Saturn (189 of 266
//    against the plane's 171), 2023-02-11 Mars (214 / 209) and 2026-10-23 Neptune (7185 / 6890 of 7854), the plane on
//    2026-10-23 Saturn (116 / 91), 2023-08-13 Jupiter (977 / 974) and 2023-11-02 Jupiter (123 / 90); a planet of r 400
//    in 480 x 360 with bands 0.6% (the quadratic does not solve), of r 250 3.6% (the quadratic keeps 8-9 blocks). On a
//    flat sky the two keep within a few blocks of each other and either may stand. The second phase settled within 19
//    rounds on every synthetic frame of the tests but a featureless planet larger than the frame (below), and is held
//    to 50.
// 2. The candidate: the brightest pixel of the 8-connected group of the copy's 3 x 3 levels over the sky by 5 of its
//    scatter (MadStd over the surface's blocks: correlated noise included) and a tenth of the brightest height, holding
//    the most light (a field star brighter per pixel than a small Neptune holds far less light than its disc).
// 3. The copy through SegmentLunar: the whole frame, then windows of 64, 128, ... px (while under a quarter of the frame)
//    centred on the candidate; the first whose split is the disc's -- SegmentLunar's disc level D, the median over its
//    Otsu threshold, 5 scatter over the sky (SegmentLunar's own test reads correlated noise low and splits a large sky's
//    noise: the real Neptune's frame D 0.0021, scatter 0.0035; its 64 px window 0.285) -- and whose planet passes. The
//    whole frame first: a window the disc fills most of puts the split inside the disc and raises its edge (2023-11-02
//    Jupiter: 2314 px in the 64 px window, 2405 in the frame). The planet: SegmentLunar's piece holding the candidate,
//    over the sky by 3 scatter, holes filled; 16 px or more, its mean 5 scatter over the sky, clear of the window's edges
//    (but where they are the frame's).
// 4. The masks: the disc; the sky, over the whole frame, more than 4 px from it and within 5 scatter of the sky's level
//    (a star, a moon and the planet's seeing halo stay out). None: no disc, and the reason says so (and names the sky
//    model: "fit its sky quadratic" or "plane").
// Measured (WithStackNoise: correlated noise 0.006): a planet of level 0.1-0.5 covering 25-55% of 320 x 240 on a sky
// rising 0.02 or 0.06 reads 20128-44088 px against the seeing-blurred disc's pi (r + 2 blur)^2 = 20397-44007 (0.987-
// 1.003 of it; the equivalent radius 1.9-2.5 px over r); one of level 0.1-0.2, r 3, 8, 14 on SmallInLargeSky's sky
// rising 0.02-0.12 is within 0.5-2x of the blurred disc with the centroid within 2 px, also at the frame's top, bottom
// and corner (where the rest of the frame is sky); one of level 0.1-0.2, r 5 and 8, is chosen over a 1 px or 2 x 2 px
// star of 0.95; the same skies with no planet: no disc. Vignetted (ruling Q14): that 320 x 240 sky rising 0.06, falling
// 0.02 or 0.04 to the farthest corner as the distance squared from the frame's centre or from (0.32 w, 0.62 h) -- with
// the plane a planet there read 14511-76800 px against a blurred disc of 330-8700 -- now gives, for a planet of level
// 0.1-0.5, r 3, 8, 14 at the centre, the top edge and two corners, or of 15% and 30% of the frame centred and off
// centre, the disc within 0.5-2x of the blurred disc and the centroid within 2 px; with no planet, no disc (also at
// 1911 x 1071). The real captures' discs are as before -- 2026-10-23 Neptune 88 px, 2023-02-11 Mars 161, 2026-09-07 and
// 2026-10-23 Saturn 577 and 598, 2023-11-02 Jupiter 2405, 2023-08-13 Jupiter 1582 for 1583 -- and their sky leaves
// out the planet's seeing halo, 5 scatter over the sky (1-13% of the sky SegmentLunar gave); the quadratic's sky is a
// little larger (2026-09-07 Saturn 91.50% of the frame against the plane's 90.87%, 2023-02-11 Mars 98.42 / 98.39%).
// Limit: a sky a quadratic does not model, and a sky the planet hides. Measured on that sky (and on one of 0.25, so a
// steeper fall stays above 0), planets of level 0.1-0.5 and r 8, 20, 50, and none, the sky falling from the centre to
// the corners as t, the squared distance over the corner's: as t, every case right to 0.16 (the largest tried); as a
// cos^4 law, 1 - (1 + 0.36 t)^-2 scaled to the fall, to 0.08; as (t + t^2) / 2, to 0.08; as t^2 (flat in the middle,
// steep at the corners), to 0.04. At 1911 x 1071 every shape holds to 0.05, the largest tried on a sky of 0.058. Beyond:
// - loud, a disc of 75% of the frame or more (Q13's notice, below): t^2 at 0.12-0.16 with no planet or one of r 8-20
//   (one of r 50 reads right but at level 0.1), at 0.08 with no planet or one of r 20 and level 0.1-0.2, at 0.05 with
//   one of r 8 and level 0.5 on the sky of 0.25; (t + t^2) / 2 at 0.16 with no planet or one of r 20 and level 0.1-0.2;
//   or a faint planet lost, no disc (the no-disc notice: t^2 at 0.05, r 8 and level 0.1, on either sky);
// - silent, a wrong disc under 75% of the frame: cos^4 at 0.12-0.16, 448-4312 px for a blurred disc of 340-1576;
//   (t + t^2) / 2 at 0.12-0.16, 10849-19254 px for 340-1576, and 15733 px on a frame with no planet; t^2 at 0.08
//   (r 8, level 0.1-0.2), 19084-19254 px for 340, and at 0.05 on the sky of 0.058 (r 8, level 0.5), 16849 px for 340.
// - A planet of 55% of the frame centred on a vignetted sky (0.02-0.04 as t) leaves sky only in the corners, which do
//   not pin a quadratic down, and neither surface keeps a tenth of the blocks: SegmentLunar's masks of the frame stand,
//   and the disc reads 1.15-1.71x the blurred disc (level 0.1-0.3: 59586-75115 px, with Q13's notice; level 0.5:
//   50671-53099 px, without).
// - A featureless planet larger than the frame (no limb in it, no bands: r 400 in 480 x 360) is a dome a quadratic
//   fits: it is taken for sky, and there is no disc (the no-disc notice; the stack is not processed). With bands the
//   quadratic does not fit it, and with its limb in the frame (r 250) it keeps fewer blocks than the plane: the frame
//   keeps SegmentLunar's masks (Q13's notice).
// A disc of 75% of the frame or more (a planet's 100% disc, a planet of r 250 or 400 in 480 x 360 with bands, whose
// frame has under a tenth of its blocks clear of it and keeps SegmentLunar's masks of the raw frame, or a planet lost
// in a sky past the quadratic above) is processed and says so (ruling Q13; PreparePlanet).
LunarMasks SegmentPlanet(const ser::FloatImage& rgb, int threads);

// ---- Colour calibration (spec call 3) ----
// A body's colour relative to sunlight: the ratios of its geometric albedos in Johnson B, Johnson V and Sloan r'.
struct BodyColour { double bOverG, rOverG; };
// Mallama, Krobusek & Pavlov 2017, "Comprehensive wide-band magnitudes and albedos for the planets", Icarus 282
// (arXiv:1609.05048), Table 7: B/G = A_B / A_V and R/G = A_r' / A_V, as spec call 3 tabulates them (checked against
// the paper on 2026-10-29). Red is the Sloan r' albedo, not Cousins Rc (ruling Q3c): the camera's R channel stops at
// its UV/IR-cut filter (~690 nm), while Rc extends to ~800+ nm, into methane absorption the camera never sees.
// Saturn's are at ring inclination 0 (the paper's Eq. A-6.1, C0): the globe's.
// nullopt for the Moon and Other.
std::optional<BodyColour> PlanetReflectance(ser::Target t);

// Scales R and B about the sky plane (CalibrateColour's background model: three rounds of 3-sigma rejection) so
// that the disc's mean less that plane has B/G and R/G equal to PlanetReflectance(t); G is unchanged. The disc
// mean leaves out every pixel with a sample at or above kClippedLuma (Measure.h). The camera's B, G and R are taken
// as Johnson B, V and Sloan r' (an approximation; HISTORY says so, and names the source, the target, and the
// disc's B/G and R/G before and after). Skipped (unchanged; HISTORY gives the reason): for a target with no
// catalogue colour, with no disc, or with under 30 unsaturated disc pixels or a non-positive channel level
// (Ruling P1's absolute floor in place of the Moon's 1% of the frame). With under 1% of the frame in sky it runs
// without a background reference, and HISTORY says so (as CalibrateColour). The rings are in the disc mask
// (SegmentLunar kept every ring pixel above 4% of the globe's contrast, PlanetSynth at openings of 4 and 25 deg, 16
// and 32 px globes; spec P6), so at wide openings Saturn's mean is the globe's and the rings' together (spec call 3:
// that error is accepted). Measured (PlanetSynth: cast 1.25/1/0.70, pedestal and gradient per channel, noise 0.004;
// r 6-100 px; Mars, Jupiter, Saturn): the disc's B/G and R/G within 0.07% of the target.
// Throws std::invalid_argument unless rgb is 3 channels and the masks are of its size.
constexpr size_t kPlanetMinDiscPx = 30;
StepRecord CalibratePlanetColour(ser::FloatImage& rgb, const LunarMasks& m, ser::Target t);

// ---- Channel alignment for small discs (Ruling P1) ----
struct DispersionShift {
    double dx = 0, dy = 0;  // where the channel's content sits against G (px; +y down)
    double ux = 0, uy = 0;  // 1-sigma uncertainty of dx, dy
    bool measured = false;  // converged with enough of the disc to judge it
    bool applied = false;   // the channel was resampled by (dx, dy) onto G
    // What the uncertainty along any direction is taken from (DispersionSigmaAlong): the formal covariance of (dx, dy)
    // (xx, xy, yy) and the jackknife's solutions, one per group of sectors.
    std::array<double, 3> cov{0, 0, 0};
    std::vector<std::array<double, 2>> groups;
    bool alongAxis = false;  // applied as its component along the other channel's dispersion axis (AlignPlanetChannels)
};
// The 1-sigma of a channel's shift along the unit direction (ax, ay): the larger of the formal one and the jackknife's
// over the groups' solutions projected there (MeasurePlanetDispersion's ux, uy per axis, for any direction, less the
// shift's dependence on e: AlignPlanetChannels uses it only along an axis another channel's own correction has set).
double DispersionSigmaAlong(const DispersionShift& s, double ax, double ay);
struct PlanetDispersion {
    DispersionShift r, b;
    size_t pixels = 0;      // pixels the fit used (the limb band, inside the frame)
    int sectors = 0;        // sectors of the limb with data
    bool frameCut = false;  // the disc touches the frame's edge
    std::string reason;     // empty when both channels were measured; else why not (ASCII)
};
// R's and B's shift against G from the disc's limb: the band from 4 px inside the disc mask's edge to 4 px outside
// it (the interior's colour structure -- bands, albedo features -- reads as a shift: fitted over the whole disc, 3%
// band colour put 40 and 200 px discs 0.16 and 0.47 px off), less the pixels whose interpolation taps at the current
// shift would leave the frame, on a 1 px Gaussian-smoothed copy (AlignChannels' reason: Catmull-Rom's noise cut at
// half pixels). Gauss-Newton on G ~ a_k (C(x + s) + e dC/dn) + b_k: one shift s per channel; a gain and offset per
// sector k, the sectors binning the limb's outward normal n by direction (about 4 px of limb each, 8 to 64), so the
// limb's colour may vary along it; and e, one offset along the normal for the whole limb, which takes up what is
// the same all round -- chromatic seeing, a limb darkening that differs by channel. The normal is the direction down
// the gradient of G smoothed by 2 px (it holds for a disc the frame cuts, a phase, the rings' edges), and position
// enters only through the derivative along it: structure along the limb (bands crossing it, the rings' arms) carries
// colour as well as position and is not used. 1-sigma per axis: the largest of the formal one (residual variance
// inflated by the smoothing's correlation area), the delete-one-group jackknife over 8 runs of neighbouring sectors
// (a colour that differs around the limb moves the groups apart; a true dispersion moves every group alike), and the
// shift's dependence on e (where the limb is not alike all round -- the frame cuts it, or it is brighter on one side --
// s and e trade off, and a channel whose limb differs from G's reads a shift that is not there: B blurred 2.0 px against
// 1.2, the light tilted 0.4 per radius, read +0.29 px at r 10 with none; Task 5 review). On real 2023-11-02 Jupiter that
// term puts B's +0.64 px at +-0.47 (+-0.13 without it); B is corrected along R's axis (AlignPlanetChannels).
// Measured (PlanetSynth: cast, pedestal and gradient per channel, blur 1.2 px, noise 0.004 on a 0.5 disc, 3% band
// colour, limb darkening 0.45/0.50/0.55; 8 seeds, R and B): with no true shift, nothing applied on any disc, whole,
// cut by the frame or ringed (largest |measured| 0.02-0.07 px on whole 12-200 px discs, up to 0.20 on cut ones); a
// 0.5 px shift on whole 12, 20, 40 and 200 px discs leaves at most 0.045, 0.047, 0.051, 0.023 px (Jupiter-like band
// contrast 0.7/1/1.4: 0.078, 0.053, 0.054, 0.049; Saturn with rings 8% bluer, 16 px globe, at 25 and 4 deg: 0.087,
// 0.059; the same at a 32 px globe and 4 deg: 0.113; noise 0.012 on a 12 px disc: 0.146, sigma 0.07).
// Not measured (reason set): no disc, under kPlanetMinDiscPx disc pixels, no limb inside the frame, under 4 sectors
// with 8 px of data each, or no convergence within 8 px.
// The result does not depend on the thread count (sums run in a fixed order, on one thread).
PlanetDispersion MeasurePlanetDispersion(const ser::FloatImage& rgb, const LunarMasks& m, int threads);
// MeasurePlanetDispersion, then each channel resampled onto G (Catmull-Rom; at the frame's edge, AlignChannels'
// ratio-to-G rule) when its shift exceeds 0.1 px AND 3 times its uncertainty (AlignChannels' gate, spec P1). On a
// disc the frame cuts, also only when that uncertainty is within 0.05 px: there colour and shift trade off along the
// visible arc, and without the rule the corrections applied there left 0.12-0.16 px at sigma 0.10-0.17 (centre 3 px
// from the edge); with it, applied corrections there left at most 0.095 px and the rest declined.
// The dispersion axis (Task 5): atmospheric dispersion puts R and B on opposite sides of G along one line. When one
// channel passes and the other does not (on a whole disc), the other is tested along the passing channel's line: its
// component there, opposite the passing channel, must exceed 0.1 px and 3 x that component's own uncertainty
// (DispersionSigmaAlong: without the shift's dependence on e, as the other channel's opposite shift, corrected on its
// own, is the evidence that the dispersion is real), and its component across the line be within 0.1 px or 3 sigma;
// then it is resampled by that component alone (alongAxis). 2026-10-23 Saturn: R +0.39, -0.40 px at +-0.22 (the rings' colour in the jackknife) was
// declined beside B -0.31, +0.32 at +-0.08 and left a 0.5 px red fringe; along B's axis it is +0.563 +-0.166 and is
// corrected (independent check: 0.115 +-0.11 px left, the fringe 0.997 -> 0.057). Synthetic (14 px disc, R/G 0.15,
// noise 0.016, R +0.55 / B -0.45 px on one line): of 16 seeds 5 pass alone, 5 more along the axis (residual 0.04-0.18
// px), 6 stay declined; an unshifted R is never moved. 2023-11-02 Jupiter's B (+0.64 px, +-0.47 on its own, the shift's
// dependence on e) is corrected this way beside R -0.85 px. HISTORY gives the shifts, their sigma, the axis test and what was
// resampled (and why a channel was not); skipped (unchanged, HISTORY says why) when nothing was. result (if given)
// receives the measurement and what was applied.
// Throws std::invalid_argument unless rgb is 3 channels and the masks are of its size.
StepRecord AlignPlanetChannels(ser::FloatImage& rgb, const LunarMasks& m, int threads, PlanetDispersion* result = nullptr);

// ---- The planet pipeline (spec calls 2, 4, 6; plan Task 4) ----
// The sliders (spec call 7): Saturation, a multiple of the natural colour, 1 by default (0 grey, 3 the top); Chroma
// blur (px at any frame width: not the Moon's width / 2000 scale, as a planet's disc does not grow with the frame);
// Sharpen (SharpenLuminanceImage's amount, with kPlanetSharpen). Defaults measured on the real captures (Task 5,
// ruling Q2; Scott's pick from the series is Q3): Chroma blur 1.0 px. The brief's rule -- the smallest blur that halves the sky's chroma noise -- holds for no blur up
// to 2.5 on any capture (the sky's output chroma is 0.006-0.64 of one 8-bit level, already held down by the colour
// step's ratio; blur 2.5 leaves 0.72-0.97 of it), nor does halving the disc's split-half chroma noise (0.53-0.91 at
// 2.5). The disc's chroma noise is under one 8-bit level at blur 0 on every capture but 2026-10-23 Neptune (1.35 levels;
// 1.03 at 1, 0.88 at 1.5); the others 0.16-0.35. The bound is acceptance 7 (Saturation 1 keeps the disc's colour within
// 2%): the blur mixes a small disc's limb with the sky, where the ratio is eps's, and 2023-02-11 Mars (r 7 px) reads
// 1.6% at 1.0 and 2.4% at 1.5. So 1.0, the largest blur of the series that keeps acceptance 7 on every capture; there
// the disc's split-half chroma noise is 0.68-0.99 of blur 0's and its colour rms 0.995-1.004 of blur 0's.
// Sharpen 1.5 (kSharpenDefault, unchanged), top 4 (kSharpenMax), kPlanetSharpen unchanged. On the synthetic truth
// (PlanetSynth with fine detail, noise 0.005, 4 seeds; test_planet "[.series]") the disc's RMS error falls at every
// amount 1-4 at every radius 10-100 px and blur 1-1.5 px (at 1.5: 0.8-20.2% lower; at 4: 3.3-38.8%), and the disc's noise
// gain is 1.05-1.38 at 1.5 and 1.49-1.65 at 2 (r 10-20, blur 1). Real (split-half noise gain on the disc interior at
// 1.5): Jupiter 1.06, Mars 1.15, Saturn 1.31 / 1.64 (its globe's edge and rings inside the mask), Neptune 1.36; the
// sky's noise x1.000 at every amount (the boost is 0 off the disc, spec P2). Saturn shows a bright rim inside the
// globe's edge: faint at 1.5, plain from 2, strong at 3-4 (layer 1's boost on the limb's own curvature, 2 px in). Not
// retuned: a layer-2 extra of 0.3 lowered the synthetic error further at 1.5-2 (r 20 blur 1: 39% at 2 against 24%) but
// raised it above the input's from 3 up on sharp discs (r 10 blur 1: -25% at 3, -100% at 4), so the slider's top would make artefacts;
// raising the top past 4 adds only layer-1 boost (Jupiter's detail is at layers 2-3: x1.14 fine detail at 4).
constexpr double kPlanetSaturationDefault = 1.0, kPlanetSaturationMax = 3.0;
constexpr double kPlanetChromaBlurDefault = 1.0, kPlanetChromaBlurMax = 6.0;
constexpr double kPlanetSharpenDefault = kSharpenDefault, kPlanetSharpenMax = kSharpenMax;
struct PlanetParams {
    double saturation = kPlanetSaturationDefault, chromaBlurSigma = kPlanetChromaBlurDefault, sharpen = kPlanetSharpenDefault;
};

// Progress phase of PreparePlanet and FinishPlanet, as kPhaseMineral's: report(kPhasePlanet, step, 7) before each of
// PreparePlanet's steps 0-4 and at its end (5), then FinishPlanet's (6, 7) after the sharpening and (7, 7) at its end;
// cancelled() is polled at each report and a true result throws StackCancelled, returning nothing.
constexpr const char* kPhasePlanet = "Planet";

// PrepareMineral's pipeline for a planet (spec call 2: the same code, parameterised by the target kind), its five
// steps, each with one HISTORY line:
//   Planet disc          SegmentPlanet (its px and equivalent radius named)
//   Colour calibration   CalibratePlanetColour to t's catalogue reflectance (call 3), not grey
//   Background           NeutraliseBackground
//   Display transfer     linear (call 4): DisplayTransfer with medianTarget 0, the brightest sample the stack did not
//                        clip at 0.95 (calibration may have lifted it above 1.0: it lands there all the same)
//   Channel alignment    AlignPlanetChannels (Ruling P1); after a resampling every sample is clamped to [0, 1]
//                        (Catmull-Rom overshoots below 0 beside the limb; HISTORY counts the samples clamped)
// and the sharpening's noise exactly as PrepareMineral measures it (Mineral.h: the stack's noise model or one level,
// through the transfer's slope, now 1 / W). No highlight threshold is measured (cache.satThreshold -1: the planet
// colour has no fade). With no disc found (a faint Neptune, a frame of sky) every later step skips with that reason
// ("no planet disc distinguished from the sky", also MineralOutcome::notice) and the image is the stack clipped to
// [0, 1]: an accepted outcome (Ruling P1), not a failure. A disc of 75% of the frame or more (ruling Q13: a planet
// filling the frame, or a planet lost in a sky curved more than a quadratic models -- SegmentPlanet's limit) is processed,
// every step running, and the "Planet disc" line ends with the notice (MineralOutcome::notice, clipped false): "the
// planet disc covers N% of the frame (75% or more): the disc fills the frame, or the sky is curved more than a quadratic
// models; colour calibration, the background and the sharpening use that disc and what little sky is left" (N floored).
// (A disc wholly
// inside a 4:3 frame covers at most 59% of it; the planets in a sky measured above reach 57%, the real captures 3%.)
// Memory as PrepareMineral's.
// Throws std::invalid_argument for a target with no catalogue colour (Moon, Other: the colour reference needs a known
// body), and as PrepareMineral for a stack that is not 3 channels or holds NaN or infinite samples; StackCancelled on
// a cancel.
MineralCache PreparePlanet(const ser::FloatImage& stack, ser::Target t, int threads, const Progress& progress,
                           const StackNoise* noise = nullptr);
// The finish on the cache (FinishMineral's shape; the module's sliders re-run only this): SharpenedLuminance at
// p.sharpen with kPlanetSharpen, then MineralColour's transfer on a copy of cache.display with k = p.saturation,
// chroma blur p.chromaBlurSigma px at any width (scaleBlurWithWidth off), devClamp 1e3 (no clamp: Mars's B/G is about
// half its R/G) and satThreshold 1e3 (no highlight fade: Jupiter's zones and Saturn's rings keep their colour), the
// gamut guard as it is (no new clipping: where a channel would pass 0.998 the pixel's saturation gives, its hue holds). At saturation 1 that is the natural
// colour, out_c = L (C_c + eps) / (mean(C) + eps), eps the sky's divide guard: 0.002 x min(1, the disc's display median
// / 0.35) (Task 5: a fixed 0.002 pulled a dim linear disc toward grey by about eps / C -- 2026-10-23 Neptune, median
// 0.035, 2.4% against acceptance 7's 2%; scaled, 1.5%); at 0, R = G = B = L.
// HISTORY: the cache's five lines, then "Sharpen ..." and "Saturation ...", each step timed. The notice is the
// cache's. Throws std::invalid_argument for a parameter negative or not finite; StackCancelled when progress cancels.
MineralOutcome FinishPlanet(const MineralCache& cache, const PlanetParams& params, int threads, const Progress& progress = {});

}  // namespace planetary::stack
