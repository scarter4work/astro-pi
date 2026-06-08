#pragma once
#include "nukex/stretch/stretch_op.hpp"
#include <vector>

namespace nukex {

/// Target distribution for Optimal Transport Stretch.
enum class OTSTarget {
    MUNSELL,    // CIE L* perceptual lightness (default)
    SQRT,       // Square root — gentle lift
    UNIFORM,    // Pure histogram equalization
    GAUSSIAN    // Gaussian — soft midtone emphasis
};

/// Optimal Transport Stretch — quantile function matching.
///
/// Maps the source image's distribution to a target distribution using
/// 1D optimal transport: T(x) = F_target_inv(F_source(x)).
/// This is the unique monotone map that transforms the source histogram
/// into the target histogram while minimizing L2 transport cost.
///
/// Reference: Villani (2003) "Topics in Optimal Transportation"
class OTSStretch : public StretchOp {
public:
    OTSTarget target = OTSTarget::MUNSELL;
    int n_bins = 2048;   // Histogram resolution
    float gauss_mu    = 0.25f;   // Gaussian target: mean
    float gauss_sigma = 0.15f;   // Gaussian target: std dev

    bool luminance_only = true;

    OTSStretch() { name = "OTS"; category = StretchCategory::PRIMARY; }

    void  apply(Image& img) const override;

    /// apply_scalar not meaningful for OTS (requires full-image CDF).
    /// Returns identity.
    float apply_scalar(float x) const override { return x; }

private:
    /// Build the transport LUT from source histogram to target distribution.
    std::vector<float> build_lut(const float* data, int n) const;

    /// Compute target CDF inverse (quantile function) at probability p.
    float target_quantile(float p) const;
};

} // namespace nukex
