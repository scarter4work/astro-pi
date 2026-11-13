// AutoContrast core (PCL-free): the band-limiting rule (design §4.4) — fingerprint/bandlimit.py.
#pragma once

#include "core/fingerprint/Energy.h"

#include <string>
#include <utility>
#include <vector>

namespace ac {

// On success (`usable`), ref_shape and target_shape are unit-L2 energy vectors on grid_arcsec. On failure
// the three vectors are empty (Python: None) and `reason` says why.
struct BandLimitResult {
   double lo_arcsec = 0.0, hi_arcsec = 0.0;
   bool usable = false;
   std::string reason;
   std::vector<double> grid_arcsec, ref_shape, target_shape;
};

// comparable_band: lo = max(ref_psf, target_psf); hi = min of each spectrum's second-largest centre.
// Throws std::out_of_range when a spectrum has fewer than two centres (Python: IndexError on centers[-2]).
std::pair<double, double> comparable_band(const EnergySpectrum& ref, const EnergySpectrum& target,
                                          double ref_psf_fwhm, double target_psf_fwhm);

// band_limit: restrict to [lo, hi] on the target's own centres, interpolate the reference in log-arcsec
// (np.interp), L2-normalise both. Reason strings are Python's f-strings to the byte (`.3g` floats).
BandLimitResult band_limit(const EnergySpectrum& ref, const EnergySpectrum& target, double ref_psf_fwhm,
                           double target_psf_fwhm, int min_bins = 3);

} // namespace ac
