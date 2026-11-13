// AutoContrast core (PCL-free): scipy.ndimage.gaussian_filter for 2-D planes.
#pragma once

#include "core/Image.h"

#include <vector>

namespace ac {

// scipy.ndimage._filters._gaussian_kernel1d(sigma, order=0, radius): normalised Gaussian, 2*radius+1 taps.
std::vector<double> gaussian_kernel1d(double sigma, int radius);

// scipy.ndimage.gaussian_filter(plane, sigma) with mode='reflect', truncate=4.0: radius int(4*sigma + 0.5);
// axis 0 then axis 1, each a correlate1d with the (reversed) kernel. sigma <= 1e-15 returns a copy.
Plane gaussian_filter(const Plane& in, double sigma, unsigned threads = 0);

} // namespace ac
