#include "core/fingerprint/BandLimit.h"

#include "core/PyFormat.h"
#include "core/num/Interp.h"

#include <cmath>
#include <stdexcept>
#include <string>

namespace ac {

namespace {

// _max_meaningful_scale: centers_arcsec[-2], the second-largest wavelet layer (§4.4).
double max_meaningful_scale(const EnergySpectrum& spec)
{
   const size_t n = spec.centers_arcsec.size();
   if (n < 2)
      throw std::out_of_range("index -2 is out of bounds for axis 0 with size " + std::to_string(n));
   return spec.centers_arcsec[n - 2];
}

// Python's builtin max(a, b) / min(a, b): the first argument wins unless the second compares greater
// (smaller), so a NaN in either slot behaves as it does in Python.
double py_max(double a, double b) { return b > a ? b : a; }
double py_min(double a, double b) { return b < a ? b : a; }

// _l2_normalize: v / np.linalg.norm(v) if the norm > 0 else v. np.linalg.norm of a 1-D float64 vector is
// sqrt(x.dot(x)), a BLAS ddot; for fewer than 16 elements the bundled OpenBLAS kernel accumulates
// serially with fused multiply-adds (verified bitwise against numpy 2.5.3 on this CPU for n = 1..15; it
// diverges from n = 16 by an ulp or so). Comparison grids hold at most n_scales - 1 bins.
std::vector<double> l2_normalize(const std::vector<double>& v)
{
   double sq = 0.0;
   for (double x : v)
      sq = std::fma(x, x, sq);
   const double norm = std::sqrt(sq);
   if (!(norm > 0.0))
      return v;
   std::vector<double> out(v.size());
   for (size_t i = 0; i < v.size(); ++i)
      out[i] = v[i] / norm;
   return out;
}

} // namespace

std::pair<double, double> comparable_band(const EnergySpectrum& ref, const EnergySpectrum& target,
                                          double ref_psf_fwhm, double target_psf_fwhm)
{
   const double lo = py_max(ref_psf_fwhm, target_psf_fwhm);
   const double hi = py_min(max_meaningful_scale(ref), max_meaningful_scale(target));
   return {lo, hi};
}

BandLimitResult band_limit(const EnergySpectrum& ref, const EnergySpectrum& target, double ref_psf_fwhm,
                           double target_psf_fwhm, int min_bins)
{
   const auto [lo, hi] = comparable_band(ref, target, ref_psf_fwhm, target_psf_fwhm);
   BandLimitResult r;
   r.lo_arcsec = lo;
   r.hi_arcsec = hi;

   if (hi <= lo) {
      r.reason = "No band overlap: lo=" + py_format_g(lo, 3) + "\" >= hi=" + py_format_g(hi, 3) +
                 "\". Instruments share no resolvable scale; structural guidance unusable.";
      return r;
   }

   std::vector<double> grid, target_energy;
   for (size_t i = 0; i < target.centers_arcsec.size(); ++i)
      if (target.centers_arcsec[i] >= lo && target.centers_arcsec[i] <= hi) {
         grid.push_back(target.centers_arcsec[i]);
         target_energy.push_back(target.energy.at(i));
      }

   if (static_cast<long long>(grid.size()) < min_bins) {
      r.reason = "Only " + std::to_string(grid.size()) + " comparable bin(s) in [" + py_format_g(lo, 3) + ", " +
                 py_format_g(hi, 3) + "]\" (need " + std::to_string(min_bins) +
                 "); structural guidance unusable, fall back to tonal/chroma.";
      return r;
   }

   // Interpolate the reference onto the target grid in log-scale space.
   std::vector<double> log_ref(ref.centers_arcsec.size());
   for (size_t i = 0; i < log_ref.size(); ++i)
      log_ref[i] = std::log(ref.centers_arcsec[i]);
   std::vector<double> ref_energy(grid.size());
   for (size_t i = 0; i < grid.size(); ++i)
      ref_energy[i] = interp(std::log(grid[i]), log_ref, ref.energy);

   r.usable = true;
   r.reason = std::to_string(grid.size()) + " comparable bins in [" + py_format_g(lo, 3) + ", " + py_format_g(hi, 3) +
              "]\".";
   r.ref_shape = l2_normalize(ref_energy);
   r.target_shape = l2_normalize(target_energy);
   r.grid_arcsec = std::move(grid);
   return r;
}

} // namespace ac
