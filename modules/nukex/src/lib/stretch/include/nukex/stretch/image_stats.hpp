#pragma once

#include "nukex/io/image.hpp"
#include <array>
#include <cstdint>
#include <string>

namespace nukex {

struct ImageStats {
    // Per-channel, in RGB order. NaN where not applicable (e.g. mono: G=B=NaN).
    std::array<double, 3> median{};
    std::array<double, 3> mad{};
    std::array<double, 3> p50{};
    std::array<double, 3> p95{};
    std::array<double, 3> p99{};
    std::array<double, 3> p999{};
    std::array<double, 3> skew{};
    std::array<double, 3> sat_frac{};

    // Global stats
    double bright_concentration = 0.0;
    double color_rg             = 1.0;
    double color_bg             = 1.0;
    double fwhm_median          = 0.0;
    int    star_count           = 0;

    // Flatten into a contiguous feature row. RGB takes all 29 columns; mono
    // emits NaN for unused channels. Used to build Eigen matrices for ridge.
    std::array<double, 29> to_feature_row() const;
};

// Compute stats from a stacked linear image. n_channels:
//   1 -> mono (writes index 0 only; indices 1,2 stay NaN)
//   3 -> RGB
// star_fwhm_median and star_count are computed via the existing StarDetector
// when available; pass 0 / 0 when the caller has no star catalogue.
//
// Non-finite samples (NaN, +/-Inf) are excluded: every per-channel field is
// computed over that channel's finite samples only, and sat_frac is a fraction
// of the finite samples. A channel with NO finite sample has every per-channel
// field NaN (as an unused mono channel does), so color_rg / color_bg are NaN
// when R, G or B is such a channel, and bright_concentration skips any pixel
// whose luminance is not finite. A NaN feature row is refused by
// ParamModel::predict_and_apply, so the caller falls back to defaults.
//
// Median, MAD and the percentiles are exact order statistics (nth_element
// selection, identical to a full sort); channels run concurrently via
// nukex::parallel_for_dynamic. Reentrant: no shared state.
ImageStats compute_image_stats(const Image& img,
                               double       star_fwhm_median = 0.0,
                               int          star_count       = 0,
                               float        saturation_level = 0.95f);

} // namespace nukex
