// Star detection as the Python core's detect.py does it (PCL-free, reentrant).
#pragma once

#include "Image.h"

#include <cmath>
#include <vector>

namespace gdg {

/// One detected star, later annotated by the cross-match (Task 11) with Bailer-Jones distances
/// and colour. NaN marks "no counterpart"; `matched` is the match flag.
struct StarRow {
    double x, y, flux;
    double r_med = NAN, r_lo = NAN, r_hi = NAN, bp_rp = NAN;
    bool matched = false;
};

/// img.mean(axis=2) for colour (numpy: 0.0 + the channels' pairwise sum, divided by C), the
/// image itself for mono.
Image luminance(const Image& img);

/// detect.detect_stars(lum, fwhm, threshold_sigma): astropy sigma_clipped_stats(lum, sigma=3),
/// then DAOStarFinder(fwhm=fwhm, threshold=threshold_sigma*std)(lum - median). Rows in
/// photutils' order with x/y/flux set. The `fwhm` column of the Python table is never read and
/// is not ported. Throws std::invalid_argument for a multi-channel image.
std::vector<StarRow> detect_stars(const Image& lum, double fwhm, double threshold_sigma);

}  // namespace gdg
