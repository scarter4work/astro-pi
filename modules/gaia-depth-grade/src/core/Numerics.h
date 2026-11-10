// numpy / scipy-exact numeric primitives for the Gaia Depth Grade core (PCL-free).
//
// Every function here reproduces the arithmetic order of the library call it replaces, so the
// results match the Python core bit for bit (or to the tolerance its test states). gdg_core is
// built with -ffp-contract=off so no compiler fuses those operations into FMAs.
//
// All functions are reentrant: no static or shared mutable state.
#pragma once

#include "Image.h"

#include <cstddef>
#include <vector>

namespace gdg {

// --- summation and boundaries --------------------------------------------------------------

/// np.sum of a contiguous 1-D float64 array: 0.0 + numpy's pairwise sum (8-way unrolled blocks
/// of <= 128, halving split on multiples of 8 above that).
double pairwise_sum(const double* a, size_t n);
inline double pairwise_sum(const std::vector<double>& v) { return pairwise_sum(v.data(), v.size()); }

/// scipy.ndimage mode='reflect' (half-sample symmetric, `d c b a | a b c d | d c b a`) index
/// for any integer i, including reflections longer than the axis: the extension is periodic
/// with period 2n, exactly as scipy's NI_ExtendLine builds it.
inline long reflect_index(long i, long n) {
    const long period = 2 * n;
    long m = i % period;
    if (m < 0) m += period;
    return m < n ? m : period - 1 - m;
}

// --- Gaussian filter (scipy.ndimage.gaussian_filter, truncate=4.0, mode='reflect') ----------

/// scipy's _gaussian_kernel1d(sigma, 0, radius): exp(-0.5/sigma^2 * x^2), normalised by its
/// numpy sum. Length 2*radius + 1.
std::vector<double> gaussian_kernel1d(double sigma, int radius);

/// scipy's kernel radius int(4.0*sigma + 0.5).
int gaussian_radius(double sigma);

/// scipy.ndimage.gaussian_filter(in, sigma) over the two image axes: correlate1d along axis 0
/// (y), then axis 1 (x), each channel independently -- `axes=(0, 1)` for colour, the default for
/// mono. sigma <= 1e-15 is the identity (scipy skips the axis). Throws std::invalid_argument on a
/// negative or non-finite sigma.
Image gaussian_filter(const Image& in, double sigma);

}  // namespace gdg
