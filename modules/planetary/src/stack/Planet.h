#pragma once
#include <optional>
#include <string>

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
// limb fade fraction 0.15 -- layer 1 only (gain 1.6 at the default 1.5 before the Wiener factor); Task 5 retunes them
// on real data. On a 20-40 px disc layer 2's scale is the disc's own shape (limb darkening, the inner half of the
// blurred limb): the Moon's {1.5, 2.5} / {3, 1} lifted the interior into a dome, and the disc's RMS error against
// the truth rose 675% / 257% at r 10 (blur 1 / 1.5 px) and 342% / 33% at r 20 with this fade, 236% at r 20 blur 1
// with the Moon's; the edge rim reached 2.5-11% of the disc mean. Measured with these values (PlanetSynth with fine
// detail -- texture to 4 px periods and spots -- noise 0.005, amount 1.5; test_planet): the disc's RMS error against
// the sharp truth falls 16.7% / 4.7% at r 10 (blur 1 / 1.5 px), 15.4% / 4.5% at r 20, 5.2% / 0.8% at r 100; around
// a ringed globe of r 8 and 16 (blur 1.2) the sky side gains 0, the undershoot below the sky is at most 0.20% and
// the rim inside the limb and ring tips at most 1.25% of the disc mean (bar 2%); the sky's noise is unchanged (the
// boost is 0 off the disc). The fade fraction's own share is small but consistent: 8 seeds at blur 1, f 0.15 against
// 0 (the Moon's fade), 20.17% against 20.05% lower at r 8, 19.19% against 18.92% at r 10; no change at blur 1.5 or at
// r 6 or 12 (where 0.15 R >= 2 px, layer 1's reach, or the fade lies outside the measured interior), none in the halo.
extern const SharpenProfile kPlanetSharpen;

// ---- Colour calibration (spec call 3) ----
// A body's colour relative to sunlight: the ratios of its geometric albedos in Johnson B, Johnson V and Cousins Rc.
struct BodyColour { double bOverG, rOverG; };
// Mallama, Krobusek & Pavlov 2017, "Comprehensive wide-band magnitudes and albedos for the planets", Icarus 282
// (arXiv:1609.05048), Table 7: B/G = A_B / A_V and R/G = A_Rc / A_V, as spec call 3 tabulates them (checked against
// the paper on 2026-10-29). Saturn's are at ring inclination 0 (the paper's Eq. A-6.1, C0): the globe's.
// nullopt for the Moon and Other.
std::optional<BodyColour> PlanetReflectance(ser::Target t);

// Scales R and B about the sky plane (CalibrateColour's background model: three rounds of 3-sigma rejection) so
// that the disc's mean less that plane has B/G and R/G equal to PlanetReflectance(t); G is unchanged. The disc
// mean leaves out every pixel with a sample at or above kClippedLuma (Measure.h). The camera's B, G and R are taken
// as Johnson B, V and Cousins Rc (an approximation; HISTORY says so, and names the source, the target, and the
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
};
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
// shift's dependence on e (nil on a whole disc; on a cut one, s and e trade off).
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
// from the edge); with it, applied corrections there left at most 0.095 px and the rest declined. HISTORY gives the
// shifts, their sigma, and what was resampled; skipped (unchanged, HISTORY says why) when nothing was. result (if
// given) receives the measurement and what was applied.
// Throws std::invalid_argument unless rgb is 3 channels and the masks are of its size.
StepRecord AlignPlanetChannels(ser::FloatImage& rgb, const LunarMasks& m, int threads, PlanetDispersion* result = nullptr);

}  // namespace planetary::stack
