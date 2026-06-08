#pragma once
#include "nukex/stretch/stretch_op.hpp"

namespace nukex {

/// Generalized Hyperbolic Stretch (Payne & Cranfield 2021).
///
/// Unifies five curve families (logarithmic, integral, exponential,
/// hyperbolic, harmonic) plus arcsinh via a shape parameter b.
/// Piecewise construction places peak contrast at the symmetry point SP,
/// with linear shadow/highlight protection regions below LP and above HP.
///
/// Reference: ghsastro.co.uk/doc/tools/GeneralizedHyperbolicStretch
class GHSStretch : public StretchOp {
public:
    float D  = 5.0f;    // Stretch intensity (max gradient of base curve). D=0 → identity.
    float b  = 0.0f;    // Shape: -1=log, <0=integral, 0=exponential, >0=hyperbolic, 1=harmonic
    float SP = 0.0f;    // Symmetry point: input value with maximum contrast [0, 1]
    float LP = 0.0f;    // Shadow protection point [0, SP]
    float HP = 1.0f;    // Highlight protection point [SP, 1]

    bool luminance_only = true;

    GHSStretch() { name = "GHS"; category = StretchCategory::PRIMARY; }

    void  apply(Image& img) const override;
    float apply_scalar(float x) const override;

    std::map<std::string, std::pair<float, float>> param_bounds() const override;
    bool                                           set_param(const std::string&, float) override;
    std::optional<float>                           get_param(const std::string&) const override;

private:
    /// Base transformation T(x) for x >= 0, parameterized by D and b.
    /// T(0) = 0, T'(0) = D (peak gradient at origin).
    float base_transform(float x) const;

    /// Derivative of base transformation T'(x).
    float base_transform_deriv(float x) const;
};

} // namespace nukex
