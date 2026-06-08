#pragma once
#include "nukex/stretch/stretch_op.hpp"

namespace nukex {

/// Lupton RGB — color-preserving asinh stretch for multi-band images.
///
/// Computes shared intensity I from RGB, applies arcsinh stretch to I,
/// then scales all channels by the same ratio f(I)/I. This preserves
/// color ratios across the full dynamic range — faint red galaxies stay
/// red, bright stars saturate gracefully instead of burning to white.
///
/// Reference: Lupton et al. (2004) "Preparing Red-Green-Blue Images from
///   CCD Data", PASP 116:133-137.
///
/// NOTE: This is inherently a multi-channel algorithm. On single-channel
/// images it reduces to a standard arcsinh stretch.
class LuptonStretch : public StretchOp {
public:
    float Q       = 8.0f;    // Softening: controls linear/log transition
    float stretch = 5.0f;    // Linear stretch range

    // Per-band minimum to subtract (background levels).
    // If all zero, no background subtraction is applied.
    float minimum_r = 0.0f;
    float minimum_g = 0.0f;
    float minimum_b = 0.0f;

    LuptonStretch() { name = "Lupton"; category = StretchCategory::PRIMARY; }

    void  apply(Image& img) const override;

    /// Scalar version applies the arcsinh curve to a single intensity value.
    float apply_scalar(float x) const override;

    std::map<std::string, std::pair<float, float>> param_bounds() const override;
    bool                                           set_param(const std::string&, float) override;
    std::optional<float>                           get_param(const std::string&) const override;
};

} // namespace nukex
