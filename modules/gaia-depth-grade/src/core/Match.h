// Catalogue <-> detection cross-match (port of match.cross_match).
#pragma once

#include "Catalog.h"
#include "Detect.h"
#include "Sky.h"

#include <cmath>
#include <vector>

namespace gdg {

struct MatchStats {
    int n_detected = 0, n_matched = 0;
    double match_rate = 0.0, median_offset_px = NAN;
};

/// Each catalogue row (in order) is projected to the image and paired with its nearest detection;
/// pairs farther than tol_px are rejected, and each detection keeps the catalogue row with the
/// strictly smallest distance (the earlier row on a tie), as the Python dict loop does. Returns a
/// copy of `detected` with matched / r_med / r_lo / r_hi / bp_rp set on the matched rows (and
/// reset to false / NaN on the others). A row the transform cannot project never matches.
std::vector<StarRow> cross_match(const std::vector<StarRow>& detected, const Catalog& catalog,
                                 const SkyTransform& sky, double tol_px, MatchStats& stats);

}  // namespace gdg
