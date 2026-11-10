// Port of gaia_depth_grade/pipeline.py (prepare_grade, render_from_prep).
#include "Pipeline.h"

#include "Format.h"
#include "HaloProfile.h"
#include "Match.h"
#include "Modulate.h"
#include "RenderStars.h"
#include "Transform.h"

namespace gdg {

PrepResult prepare_grade(const Image& stars, const SkyTransform& sky, const GradeConfig& cfg, CatalogSource& source,
                         const Log& log) {
    const Image lum = luminance(stars);
    std::vector<StarRow> detected = detect_stars(lum, cfg.detect_fwhm, cfg.detect_threshold_sigma);
    PrepResult prep;
    prep.footprint = field_footprint(sky, lum.height, lum.width);
    const Catalog catalog = source.distances_for(prep.footprint);
    MatchStats stats;
    prep.table = cross_match(detected, catalog, sky, cfg.match_tolerance_px, stats);

    const bool low = stats.match_rate < cfg.min_match_rate;
    if (low)
        log_warning(log, strformat("LOW MATCH RATE %.2f < %.2f \xE2\x80\x94 depth grade is unreliable", stats.match_rate,
                                   cfg.min_match_rate));
    prep.qa = QA{stats.n_detected, stats.n_matched, stats.match_rate, stats.median_offset_px, low};
    return prep;
}

Image render_from_prep(const Image& stars, const PrepResult& prep, const GradeConfig& cfg, const Log& log) {
    const size_t n = prep.table.size();
    std::vector<double> rm(n), rl(n), rh(n);
    for (size_t i = 0; i < n; ++i) {
        rm[i] = prep.table[i].r_med;
        rl[i] = prep.table[i].r_lo;
        rh[i] = prep.table[i].r_hi;
    }
    const std::vector<double> strength = effective_strength(rm, rl, rh, cfg.p_low, cfg.p_high, cfg.neutral_strength);
    const Modulation modulation = compute_modulation(strength, cfg.gains);
    // The star-shine halo profile comes from the very layer being graded.
    const HaloProfile halo = build_halo_profile(stars, prep.table, cfg.base_sigma_px, cfg, log);
    return render_stars(stars, prep.table, modulation, cfg.base_sigma_px, &halo, cfg.gains.halo_gain);
}

}  // namespace gdg
