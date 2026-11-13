// AutoContrast core (PCL-free): scipy.ndimage 1-D correlation / convolution along one axis.
#pragma once

#include "core/Image.h"

#include <vector>

namespace ac {

enum class EdgeMode {
   Mirror,    // scipy mode='mirror':  d c b | a b c d | c b a
   Reflect    // scipy mode='reflect': d c b a | a b c d | d c b a (ndimage default)
};

// Source index for position i (any distance outside [0, n)) on a line of length n >= 1 under `mode`, bouncing
// periodically as scipy's line extension does for kernels wider than the line.
long extend_index(long i, long n, EdgeMode mode);

// scipy.ndimage.correlate1d(plane, weights, axis, mode=mode), odd weights.size() (throws otherwise).
// axis 0 runs down the rows (y), axis 1 along x. Tap summation order and the symmetric / anti-symmetric
// shortcuts (|w[i] -/+ w[-i]| <= DBL_EPSILON) follow ni_filters.c::NI_Correlate1D, so results match scipy
// to the bit. Lines are independent: `threads` (0 = default) never changes a result.
Plane correlate1d(const Plane& in, const std::vector<double>& weights, int axis, EdgeMode mode, unsigned threads = 0);

// scipy.ndimage.convolve1d(plane, kernel, axis, mode='mirror'): the kernel is reversed and correlated.
// Kernels wider than the axis are fine (the edge extension bounces repeatedly, as scipy's does).
Plane convolve1d_mirror(const Plane& in, const std::vector<double>& kernel, int axis, unsigned threads = 0);

} // namespace ac
