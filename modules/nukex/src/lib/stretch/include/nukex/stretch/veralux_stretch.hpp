#pragma once
#include "nukex/stretch/stretch_op.hpp"

namespace nukex {

/// VeraLux HyperMetric Stretch (HMS) — Paterniti 2025.
///
/// An arcsinh-based stretch with color vector preservation and
/// convergence-to-white for saturated bright sources. The core innovation
/// is treating each RGB pixel as a 3D vector: decompose into magnitude
/// (luminance) and direction (chromaticity), stretch the magnitude, then
/// reconstruct with a convergence term that gracefully transitions bright
/// pixels toward white — mimicking physical sensor/film saturation.
///
/// Reference: Riccardo Paterniti, VeraLux HMS, GPL v3 (2025).
///   Source: gitlab.com/free-astro/siril-scripts/-/tree/main/VeraLux
class VeraLuxStretch : public StretchOp {
public:
    float SP       = 0.0f;   // Shadow point: input level mapped to black [0, 1)
    float log_D    = 2.0f;   // Stretch intensity: D = 10^log_D (range 0-7)
    float protect_b = 6.0f;  // Hyperbolic knee protection (0.1-15)
    float convergence_power = 3.5f;  // Star core white transition speed (1-10)

    // Sensor QE weights for luminance computation.
    // Default: Rec.709. Override for specific sensors.
    float w_R = 0.2126f;
    float w_G = 0.7152f;
    float w_B = 0.0722f;

    VeraLuxStretch() { name = "VeraLux"; category = StretchCategory::PRIMARY; }

    void  apply(Image& img) const override;

    /// Solve BOTH curve parameters against this image: the shadow point `SP`
    /// and the intensity `log_D`. Returns log_D; read `SP` for the other.
    ///
    /// A fixed intensity cannot serve different data. Measured on the three
    /// regression corpora, the linear background spans 0.0166 (M16 HaO3) to
    /// 0.1479 (NGC7635 mono) -- nearly an order of magnitude -- so one log_D
    /// puts one of them at 0.08 and another at 0.43. Worse, raising log_D
    /// brightens but FLATTENS: on NGC7635 the spread between the median and
    /// the 99.9th percentile falls from 0.191 to 0.073 as log_D goes 2 -> 5.
    /// So the intensity is solved per image instead of chosen once.
    ///
    /// Positioning and contrast are separate problems, and the intensity only
    /// solves the first: it MOVES the histogram, while only a shadow point
    /// WIDENS it. Measured on a 74-frame stack whose 99.9th percentile sits 44
    /// sigma above the background, SP = 0 delivered that as 4% of the output
    /// range, because a ~1% wide signal band rides on a pedestal that eats 98%
    /// of the curve. Solving both took the luminance spread from 0.040 to
    /// 0.299 -- 7.5x -- with the background still exactly on target.
    ///
    /// 0.25 is the target PixInsight's own ScreenTransferFunction autostretch
    /// uses, which is the convention users of this ecosystem already read
    /// their images against, and SP = median - 2.8 sigma is that same
    /// autostretch's shadow-clipping convention.
    /// Solve log_D so the stretched background lands on `target_background`.
    ///
    /// 0.12, not the 0.25 screen-autostretch convention: 0.25 spends a quarter
    /// of the output range on sky and leaves the subject on a bright grey
    /// pedestal, which is what "washed out" looks like numerically. Measured
    /// across four real corpora, 0.12 raises signal saturation 1.34x-1.72x
    /// and clips nothing. Kept as an argument because the module exposes it
    /// as a user control.
    float auto_tune(const Image& img, float target_background = 0.12f);

    /// Scalar version: stretches a single luminance value.
    float apply_scalar(float x) const override;

    std::map<std::string, std::pair<float, float>> param_bounds() const override;
    bool                                           set_param(const std::string&, float) override;
    std::optional<float>                           get_param(const std::string&) const override;
};

} // namespace nukex
