// astropy 8.0 sigma_clipped_stats (PCL-free, reentrant).
#pragma once

#include <cstddef>
#include <vector>

namespace gdg {

struct ClipStats {
    double mean, median, std;
    int iterations = 0;        // clipping passes run (astropy's SigmaClip._niterations)
    size_t nonfinite = 0;      // NaN/inf inputs dropped before clipping (astropy warns about these)
};

/// astropy.stats.sigma_clipped_stats(v, sigma=sigma, maxiters=maxiters) with its defaults:
/// cenfunc='median', stdfunc='std' (ddof 0), symmetric bounds, grow=False, axis=None.
///
/// This is astropy's _sigmaclip_noaxis path: non-finite values are dropped first; then, while
/// the last pass removed something and fewer than maxiters passes ran, bounds
/// median -/+ sigma*std are computed and only lo <= x <= hi is kept (values on a bound stay).
/// mean/median/std are numpy's nanmean/nanmedian/nanstd of what is left (pairwise sums, two-pass
/// variance). maxiters == 0 means no limit (astropy's `maxiters or np.inf`); a negative maxiters
/// runs no pass. No finite input gives NaN for all three, as numpy's empty nan-reductions do.
ClipStats sigma_clipped_stats(const std::vector<double>& v, double sigma = 3.0, int maxiters = 5);

}  // namespace gdg
