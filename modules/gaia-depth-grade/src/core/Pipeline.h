// Two-stage depth grade (port of pipeline.py): prepare_grade does the gain-independent work
// (detect, catalogue, cross-match, QA); render_from_prep does the gain-dependent render.
#pragma once

#include "Catalog.h"
#include "Config.h"
#include "Detect.h"
#include "Footprint.h"
#include "Image.h"
#include "Log.h"
#include "Sky.h"

#include <cmath>
#include <vector>

namespace gdg {

struct QA {
    int n_detected = 0, n_matched = 0;
    double match_rate = 0.0, median_offset_px = NAN;
    bool low_match_warning = false;
};

struct PrepResult {
    std::vector<StarRow> table;  // every detection; r_* NaN where unmatched
    QA qa;
    Footprint footprint;         // the cone the catalogue was queried for
};

/// detect_stars(luminance(stars)) -> field_footprint -> source.distances_for -> cross_match.
/// Logs `LOW MATCH RATE %.2f < %.2f — depth grade is unreliable` when the rate is below
/// cfg.min_match_rate.
PrepResult prepare_grade(const Image& stars, const SkyTransform& sky, const GradeConfig& cfg, CatalogSource& source,
                         const Log& log);

/// effective_strength -> compute_modulation(cfg.gains) -> build_halo_profile(stars) -> render_stars.
Image render_from_prep(const Image& stars, const PrepResult& prep, const GradeConfig& cfg, const Log& log);

}  // namespace gdg
