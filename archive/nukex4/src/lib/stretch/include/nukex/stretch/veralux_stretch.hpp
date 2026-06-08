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

    /// Scalar version: stretches a single luminance value.
    float apply_scalar(float x) const override;

    std::map<std::string, std::pair<float, float>> param_bounds() const override;
    bool                                           set_param(const std::string&, float) override;
    std::optional<float>                           get_param(const std::string&) const override;
};

} // namespace nukex
