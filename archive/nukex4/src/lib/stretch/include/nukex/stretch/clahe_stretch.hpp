#pragma once
#include "nukex/stretch/stretch_op.hpp"

namespace nukex {

/// CLAHE — Contrast Limited Adaptive Histogram Equalization.
///
/// Tile-based adaptive histogram equalization with clip limit to prevent
/// noise amplification. Uses bilinear interpolation between tile transfer
/// functions for smooth transitions.
///
/// Designed as a FINISHER: apply after a primary stretch to enhance local
/// contrast in nebula structure without blowing out stars or amplifying
/// sky noise.
///
/// Reference: Pizer et al. (1987) "Adaptive Histogram Equalization and
///   Its Variations", CVGIP 39:355-368.
class CLAHEStretch : public StretchOp {
public:
    float clip_limit = 2.0f;   // Max contrast amplification (1.5-4.0 for astro)
    int   tile_cols  = 8;      // Number of tile columns
    int   tile_rows  = 8;      // Number of tile rows
    int   n_bins     = 256;    // Histogram resolution

    bool luminance_only = true;

    CLAHEStretch() { name = "CLAHE"; category = StretchCategory::FINISHER; }

    void  apply(Image& img) const override;
    float apply_scalar(float x) const override { return x; } // Spatial op

    std::map<std::string, std::pair<float, float>> param_bounds() const override;
    bool                                           set_param(const std::string&, float) override;
    std::optional<float>                           get_param(const std::string&) const override;

private:
    /// Build clipped CDF for a tile region.
    std::vector<float> build_tile_cdf(const float* data, int w, int h,
                                       int x0, int y0, int x1, int y1) const;
};

} // namespace nukex
