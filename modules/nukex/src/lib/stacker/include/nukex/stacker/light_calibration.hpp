#pragma once

#include "nukex/stacker/calibration_masters.hpp"

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

/// What will be subtracted from one light.
///
///   dark, unscaled (same exposure and temperature):  L - D
///   dark, scaled by s:                               L - B - s * (D - B)
///   bias only:                                       L - B
///
/// An unscaled dark already contains the bias, so B is not subtracted again
/// (L - B - (D - B) is L - D).
struct CalibrationPlan {
    const MasterFrame* bias = nullptr;   ///< used only when !dark or dark_scaled
    const MasterFrame* dark = nullptr;
    bool   dark_scaled = false;
    double exposure_ratio = 1.0;         ///< t_light / t_dark
    double temperature_factor = 1.0;     ///< 2^(dT / doubling), or 1
    double dark_scale = 1.0;             ///< exposure_ratio * temperature_factor
    std::string description;             ///< one line, for the log

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
    static std::string apply(Image& frame, double frame_full_scale,
                             const CalibrationPlan& plan,
                             const MasterFrame* master_flat,
                             float min_flat_value = 0.01f);

    /// The pedestal image a plan would subtract from a frame
    /// (D, B + s * (D - B), or B). Used to calibrate flats with a dark-flat
    /// or bias before the master flat is fitted.
    static Image pedestal(const CalibrationPlan& plan);
};

} // namespace nukex
