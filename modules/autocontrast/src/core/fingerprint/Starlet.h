// AutoContrast core (PCL-free): the à trous (starlet) B3-spline transform — fingerprint/starlet.py.
#pragma once

#include "core/Image.h"

#include <vector>

namespace ac {

struct Starlet {
   std::vector<Plane> planes;   // detail at each scale, finest first
   Plane residual;              // the coarse approximation left over
};

// _dilate(_B3_KERNEL, scale): [1,4,6,4,1]/16 with 2**scale - 1 zeros between taps ((4 << scale) + 1 taps).
std::vector<double> b3_dilated_kernel(int scale);

// scipy.ndimage.convolve1d(plane, b3_dilated_kernel(scale), axis, mode="mirror"), to the bit, touching only
// the five non-zero taps. Identical bits to convolve1d_mirror() with the explicit kernel for every input
// (signed zeros, NaN, Inf and huge values included — see Starlet.cpp for the argument). Rows/columns are
// independent: `threads` (0 = default) never changes a result.
Plane b3_smooth_axis(const Plane& in, int scale, int axis, unsigned threads = 0);

// starlet_transform(image, n_scales): for scale in 0..n-1: smoothed = smooth axis 0 then axis 1 with the
// dilated B3 kernel (mirror borders); planes[scale] = current - smoothed; current = smoothed. The residual
// is the final `current`, so sum(planes) + residual reconstructs the image. Throws std::invalid_argument
// "n_scales must be >= 1, got <n>" as Python's ValueError. Bit-identical at any thread count.
Starlet starlet_transform(const Plane& image, int n_scales, unsigned threads = 0);

} // namespace ac
