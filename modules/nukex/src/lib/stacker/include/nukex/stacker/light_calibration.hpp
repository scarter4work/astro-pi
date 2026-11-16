#pragma once

#include "nukex/stacker/calibration_masters.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nukex {

/// How closely a calibration master's header has to agree with a light's.
struct MatchTolerance {
    /// |t_light - t_dark| <= exposure_rel * t_light is "the same exposure":
    /// the dark is subtracted as it is, without scaling.
    double exposure_rel = 0.001;

    /// A dark more than this many degrees from the light is a mismatch...
    double temperature_c = 2.0;

    /// ...unless dark-current temperature scaling is switched on: > 0 is the
    /// temperature step (degrees C) over which dark current doubles, and the
    /// dark current is then scaled by 2^((T_light - T_dark) / doubling) at
    /// every temperature difference up to `temperature_scaling_limit_c`.
    ///
    /// OFF (0) by default, on evidence: below about 0 C a cooled Sony CMOS
    /// sensor's dark signal is dominated by amplifier glow, which is only
    /// weakly temperature-dependent (Sony IMX455, arXiv 2510.14484), and the
    /// doubling step reported for CMOS sensors ranges from about 5 to 8 C. A
    /// fixed exponential would mis-scale exactly the structure a dark is for.
    double dark_doubling_temperature_c = 0.0;
    double temperature_scaling_limit_c = 10.0;

    /// Scale a dark of a different exposure by t_light / t_dark (needs a
    /// master bias to separate the pedestal from the dark signal).
    bool allow_exposure_scaling = true;
};

/// The masters a batch may be calibrated with. Several darks (different
/// exposures or temperatures) and several biases may be offered; each light
/// is matched to the ones that fit it by header.
struct CalibrationSet {
    std::vector<MasterFrame> biases;
    std::vector<MasterFrame> darks;
    std::vector<MasterFrame> dark_flats;
    MatchTolerance           tolerance;

    /// Settings for the master flat the engine builds from the batch's flats
    /// when a calibration set is active (cache_dir is taken from the engine).
    MasterBuilder::Config    flat_builder;

    bool empty() const { return biases.empty() && darks.empty() && dark_flats.empty(); }
};

/// The dark current's fine structure -- hot pixels, fixed pattern, glow
/// edges -- that a dark scale is fitted against: each pixel's excess over
/// the median of its eight same-colour neighbours in (D - B), kept where it
/// stands more than `threshold_sigma` above the noise of that excess.
struct DarkStructure {
    std::vector<std::uint32_t> index;   ///< pixel offsets into the frame's data
    std::vector<float>         excess;  ///< (D - B) excess there
    float sigma  = 0.0f;                ///< robust scale of the excess, all pixels
    int   stride = 2;                   ///< same-colour neighbour distance (2 for CFA, 1 mono)
};

/// The fitted dark scale for one light.
///
/// The scale is solved, not assumed: a Huber M-estimate of the regression
///     excess_light(p) = k * excess_dark(p)
/// over the structure pixels, where excess_light is the same neighbour
/// excess measured on (L - B). Sky, nebula and stars are smooth on the
/// scale of a pixel's eight same-colour neighbours or, where they are not
/// (star cores), are the regression's outliers and get Huber's delta/|r|
/// weight. The header ratio t_light/t_dark (times the temperature factor)
/// only seeds it. Same estimator, tuning and MAD scale as the stacker's
/// per-pixel fit.
///
/// Why: dark signal is not exactly linear in exposure. Measured on an
/// ASI2400MC Pro, a 180 s dark scaled to 300 s by the header ratio (1.667)
/// left the hot pixels ~5 ADU over-subtracted.
struct DarkScaleFit {
    double initial = 1.0;          ///< header ratio: the seed
    double scale = 1.0;            ///< the scale actually applied
    double fitted = 0.0;           ///< the fit's value (even when refused)
    double standard_error = 0.0;   ///< of the fitted value
    int    n_pixels = 0;           ///< structure pixels the fit used
    int    iterations = 0;
    bool   well_conditioned = false;
    std::string warning;           ///< why the header ratio had to be used instead
};

/// What will be subtracted from one light.
///
///   dark, unscaled (same exposure and temperature):  L - D
///   dark, scaled by k:                               L - B - k * (D - B)
///   bias only:                                       L - B
///
/// k is fitted per light (DarkScaleFit), seeded at `dark_scale`. An
/// unscaled dark already contains the bias, so B is not subtracted again
/// (L - B - (D - B) is L - D).
struct CalibrationPlan {
    const MasterFrame* bias = nullptr;   ///< used only when !dark or dark_scaled
    const MasterFrame* dark = nullptr;
    bool   dark_scaled = false;
    double exposure_ratio = 1.0;         ///< t_light / t_dark
    double temperature_factor = 1.0;     ///< 2^(dT / doubling), or 1
    double dark_scale = 1.0;             ///< exposure_ratio * temperature_factor: the fit's seed
    std::string description;             ///< one line, for the log
    /// The (bias, dark) pair's structure for the scale fit, computed once and
    /// shared by every light of the plan (LightCalibrator::prepare). Null:
    /// apply() computes it itself.
    std::shared_ptr<const DarkStructure> structure;

    bool active() const { return bias != nullptr || dark != nullptr; }
};

struct PlanResult {
    bool            ok = false;
    std::string     error;     ///< every candidate and why it was refused
    CalibrationPlan plan;
    std::vector<std::string> warnings;   ///< accepted, but a keyword could not be checked
};

/// Header matching between a light (or a flat set) and the offered masters.
class CalibrationMatcher {
public:
    /// Empty if `master` can calibrate a frame with header `frame`, else why
    /// not: camera, geometry, CFA pattern, binning, gain, offset and data
    /// scale must agree. A keyword present on one side and missing on the
    /// other is a mismatch (it cannot be checked); missing on both is
    /// accepted with a warning appended to `warnings`.
    static std::string compatibility(const CalibrationHeader& frame,
                                     const CalibrationHeader& master,
                                     std::vector<std::string>* warnings = nullptr);

    /// The plan for a frame with header `frame` (a light, or the first of a
    /// flat set when matching dark-flats). Picks, among compatible darks, one
    /// of the same exposure first, then the smallest exposure ratio away from
    /// 1, then the smallest temperature difference. A dark that would need
    /// scaling is usable only with a compatible bias.
    ///
    /// `require_dark` / `require_bias`: refuse (with every candidate's
    /// reason) rather than fall back when nothing matches. With darks offered
    /// a light that no dark fits is an error -- it is never stacked
    /// uncalibrated beside lights that were.
    static PlanResult plan(const CalibrationHeader& frame,
                           const std::vector<MasterFrame>& biases,
                           const std::vector<MasterFrame>& darks,
                           const MatchTolerance& tol,
                           bool require_dark, bool require_bias);

    /// The pedestal plan for a flat set (header `flat`, its first frame) when
    /// NEITHER a master dark-flat NOR a master bias was given -- lights, darks
    /// and flats only, the common CMOS night. A flat divided in with its
    /// pedestal still on under-corrects vignetting by about
    /// pedestal / flat level, so the pedestal comes off before the flat is
    /// normalised, from the darks, in this order:
    ///
    ///   1. A master dark at the flats' exposure (and temperature, within
    ///      `tol`) IS a dark-flat: subtracted as is.
    ///   2. Otherwise a compatible dark (camera, geometry, CFA, binning, gain,
    ///      offset, data scale) is scaled to the flats' exposure around an
    ///      ESTIMATED bias -- the dark's floor, LightCalibrator::estimate_bias:
    ///          pedestal = B^ + k (D - B^),   k = t_flat / t_dark
    ///      Among several, the shortest dark (least dark signal in its floor),
    ///      then the nearest in temperature. `estimated_bias` receives B^ and
    ///      the returned plan points at it, so it must outlive the plan.
    ///
    /// Limits of 2, stated in the plan's description, which the engine logs:
    /// B^ is the dark's per-CFA-channel median, so it overstates the bias by
    /// the dark's median dark signal (a few ADU on a cooled camera) and
    /// carries none of the bias's fixed pattern; both land in the flat
    /// divided by its level (~1e-4 at a 20000 ADU flat), against the
    /// pedestal / level (~3-5%) they replace. The temperature of the dark is
    /// not matched -- at k ~ 1e-3 its dark signal is negligible in a flat --
    /// but the difference is reported. Lights do NOT get this: an estimate's
    /// error would land undivided in their background, so a light whose dark
    /// needs scaling still needs a real master bias (plan()).
    ///
    /// Not used: the camera's OFFSET keyword. It is a driver setting whose
    /// ADU pedestal depends on the camera model and bit depth, so it cannot
    /// be turned into a level without a per-camera table; the dark's floor
    /// is a measurement of that pedestal.
    ///
    /// Refused, with every dark's reason, when no dark fits.
    static PlanResult flat_plan_from_darks(const CalibrationHeader& flat,
                                           const std::vector<MasterFrame>& darks,
                                           const MatchTolerance& tol,
                                           MasterFrame& estimated_bias);
};

/// Applies a plan to one frame, before debayer, on the fixed [0, 1] scale.
class LightCalibrator {
public:
    /// Subtract per `plan`, then divide by `master_flat` (may be null) with
    /// the same floor FlatCalibration::apply uses. Values stay linear: no
    /// clipping, no pedestal -- a pixel whose calibrated value is negative is
    /// a measurement of zero signal plus noise, and stays one.
    ///
    /// Returns an empty string on success, else why the frame cannot be
    /// calibrated (geometry or data scale differs from the masters').
    ///
    /// A scaled dark's scale is fitted to this frame (fit_dark_scale); the
    /// result goes to `fit_out` when given. A poorly conditioned fit falls
    /// back to the header ratio and says so in `fit_out->warning` -- callers
    /// must report it.
    static std::string apply(Image& frame, double frame_full_scale,
                             const CalibrationPlan& plan,
                             const MasterFrame* master_flat,
                             float min_flat_value = 0.01f,
                             DarkScaleFit* fit_out = nullptr);

    /// The structure a (bias, dark) pair's scale is fitted against.
    static std::shared_ptr<const DarkStructure> dark_structure(const MasterFrame& bias,
                                                               const MasterFrame& dark,
                                                               float threshold_sigma = 5.0f);

    /// Attach the structure to a scaled plan (no-op otherwise).
    static void prepare(CalibrationPlan& plan);

    /// Fit the dark scale for `frame` (raw, not yet calibrated, same scale as
    /// the masters). Well conditioned needs at least `min_pixels` structure
    /// pixels, a positive scale within a factor 4 of the seed, and a relative
    /// standard error below `max_relative_error`; otherwise the seed is used.
    static DarkScaleFit fit_dark_scale(const Image& frame, const MasterFrame& bias,
                                       const MasterFrame& dark, const DarkStructure& structure,
                                       double initial, int min_pixels = 200,
                                       double max_relative_error = 0.02);

    /// The pedestal image a plan would subtract from a frame
    /// (D, B + s * (D - B), or B). Used to calibrate flats with a dark-flat
    /// or bias before the master flat is fitted. A scaled dark-flat uses the
    /// header ratio here: a flat's signal swamps the dark structure a scale
    /// would be fitted against.
    static Image pedestal(const CalibrationPlan& plan);

    /// A master bias estimated from a master dark: every pixel of each CFA
    /// channel (each 2x2 phase of a mosaic; the whole plane for mono) set to
    /// the median of that channel of the dark. The median is robust to hot
    /// pixels and glow, and sits above the true bias by the median dark
    /// signal. `levels_adu` receives the per-channel levels in ADU (on the
    /// dark's full scale) for the log. Kind BIAS, the dark's header with no
    /// exposure, n_frames 0, signature derived from the dark's.
    static MasterFrame estimate_bias(const MasterFrame& dark,
                                     std::vector<double>* levels_adu = nullptr);
};

} // namespace nukex
