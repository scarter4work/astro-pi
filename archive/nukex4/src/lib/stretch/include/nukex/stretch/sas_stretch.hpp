#pragma once
#include "nukex/stretch/stretch_op.hpp"

namespace nukex {

/// Signal-Adaptive Stretch — spatially-varying GHS.
///
/// Divides the image into overlapping tiles, fits GHS stretch intensity (D)
/// to each tile's local median, then blends the per-tile stretch results
/// using Gaussian-weighted interpolation. This creates a spatially-varying
/// stretch where faint regions get more aggressive stretching and bright
/// regions get gentler treatment.
///
/// Related to CLAHE (Zuiderveld 1994) but uses GHS instead of histogram
/// equalization, and adapts GHS parameters rather than applying a fixed
/// clip limit.
class SASStretch : public StretchOp {
public:
    int   tile_size    = 256;    // Tile dimensions (square)
    float tile_overlap = 0.5f;  // Overlap fraction (0.5 = 50% overlap)
    float target_median = 0.25f; // Target median in output space
    float min_D = 1.0f;         // Minimum GHS stretch intensity
    float max_D = 20.0f;        // Maximum GHS stretch intensity
    float ghs_b = 0.0f;         // GHS curve family (0 = exponential)

    bool luminance_only = true;

    SASStretch() { name = "SAS"; category = StretchCategory::PRIMARY; }

    void  apply(Image& img) const override;
    float apply_scalar(float x) const override { return x; } // Not meaningful for spatial op

private:
    /// Find GHS D parameter that maps local_median to target_median.
    float fit_D(float local_median) const;
};

} // namespace nukex
