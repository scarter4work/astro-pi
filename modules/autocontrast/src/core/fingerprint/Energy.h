// AutoContrast core (PCL-free): the angular energy spectrum, the fingerprint's "depth signature" —
// fingerprint/energy.py.
#pragma once

#include "core/Image.h"

#include <vector>

namespace ac {

// Raw (un-normalised) L2 energy per wavelet plane, labelled with its angular centre in arcsec.
struct EnergySpectrum {
   std::vector<double> centers_arcsec;
   std::vector<double> energy;
};

// energy_spectrum_from_planes(planes, pixel_scale_arcsec): energy[i] = np.sum(planes[i]**2) (numpy's
// pairwise order over the whole H*W plane, reproduced to the bit), centers[i] = 2.0**i * pixel_scale.
// The residual is not passed (it is gradient/DC, not structure). Planes must share one shape
// (std::invalid_argument otherwise — numpy cannot hold a ragged (n, H, W) array).
EnergySpectrum energy_spectrum_from_planes(const std::vector<Plane>& planes, double pixel_scale_arcsec,
                                           unsigned threads = 0);

// energy_spectrum(image, pixel_scale_arcsec, n_scales): starlet_transform then the above.
EnergySpectrum energy_spectrum(const Plane& image, double pixel_scale_arcsec, int n_scales, unsigned threads = 0);

} // namespace ac
