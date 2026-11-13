#include "core/fingerprint/Energy.h"

#include "core/Parallel.h"
#include "core/fingerprint/Starlet.h"
#include "core/num/Sum.h"

#include <cmath>
#include <stdexcept>

namespace ac {

EnergySpectrum energy_spectrum_from_planes(const std::vector<Plane>& planes, double pixel_scale_arcsec,
                                           unsigned threads)
{
   EnergySpectrum spec;
   const size_t n = planes.size();
   if (n == 0)
      return spec;
   for (const auto& p : planes)
      if (p.width != planes[0].width || p.height != planes[0].height ||
          p.data.size() != static_cast<size_t>(planes[0].width) * static_cast<size_t>(planes[0].height))
         throw std::invalid_argument("energy_spectrum_from_planes: planes must share one (H, W) shape");

   // energy = np.sum(planes**2, axis=(1, 2)): one pairwise reduction per plane over its H*W samples
   spec.energy.assign(n, 0.0);
   parallel_for(n, [&](size_t i) {
      const double* d = planes[i].data.data();
      spec.energy[i] = pairwise_sum(0, planes[i].data.size(), [d](size_t k) { return d[k] * d[k]; });
   }, threads);

   // centers = 2.0 ** np.arange(n) * pixel_scale_arcsec  (2.0**i is exact)
   spec.centers_arcsec.resize(n);
   for (size_t i = 0; i < n; ++i)
      spec.centers_arcsec[i] = std::ldexp(1.0, static_cast<int>(i)) * pixel_scale_arcsec;
   return spec;
}

EnergySpectrum energy_spectrum(const Plane& image, double pixel_scale_arcsec, int n_scales, unsigned threads)
{
   const Starlet st = starlet_transform(image, n_scales, threads);
   return energy_spectrum_from_planes(st.planes, pixel_scale_arcsec, threads);
}

} // namespace ac
