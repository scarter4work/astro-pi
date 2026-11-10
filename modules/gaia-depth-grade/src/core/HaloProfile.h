// Star-shine halo profiles, as the Python core's psf.py (PCL-free, reentrant).
//
// A HaloProfile is a normalised 1-D radial lookup: sample(r) is the halo amplitude at r px,
// 1.0 at the centre, falling to 0 at the edge. build_halo_profile stacks it from the bright,
// unsaturated, isolated stars of the layer itself; with too few of them it falls back, loudly,
// to a Moffat.
#pragma once

#include "Config.h"
#include "Detect.h"
#include "Image.h"
#include "Log.h"

#include <vector>

namespace gdg {

struct HaloProfile {
    std::vector<double> r_grid;  // ascending
    std::vector<double> values;  // values[0] == 1
    bool is_empirical = false;

    /// np.interp(r, r_grid, values, left=values[0], right=0.0), and 0 beyond r_grid.back() so
    /// the halo never adds energy past where the profile was measured.
    std::vector<double> sample(const std::vector<double>& r) const;
};

/// psf.moffat_profile: (1 + (r/alpha)^2)^(-beta) normalised to 1 at r = 0, alpha chosen so the
/// FWHM matches a Gaussian of base_sigma_px; grid np.linspace(0, radius_px, max(int(radius)+1, 2)).
HaloProfile moffat_profile(double base_sigma_px, double beta, double radius_px);

/// psf.build_halo_profile: brightest first (flux descending; ties in reverse table order, NaN
/// flux first, as a stable argsort reversed), skip stamps the frame cuts, dark or saturated
/// (peak >= 0.9) stamps and stamps with a neighbour above half the peak beyond 4 base sigma;
/// radial medians / peak, stacked until max(3*min_refs, min_refs); nanmedian, NaN -> 0,
/// normalised, monotone non-increasing. Fewer than cfg.halo_min_refs stamps: the warning
/// "star-shine: only N clean reference star(s) (need M) — using a Moffat halo this run, not the
/// image's own PSF" through log.warning and the Moffat(base_sigma_px, halo_moffat_beta, radius).
HaloProfile build_halo_profile(const Image& stars, const std::vector<StarRow>& table, double base_sigma_px,
                               const GradeConfig& cfg, const Log& log);

}  // namespace gdg
