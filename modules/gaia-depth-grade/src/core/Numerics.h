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

// --- order statistics (numpy 2.5) -----------------------------------------------------------
//
// NaN policy: percentile / percentiles / median / mad require finite input and throw
// std::invalid_argument otherwise (and on empty input); callers filter first, exactly where the
// Python core does. nanmedian_rows is the one NaN-aware function.

/// np.percentile(v, q), method "linear": virtual index (n-1)*(q/100), neighbours by partial
/// selection, numpy's _lerp (a + (b-a)*g, or b - (b-a)*(1-g) when g >= 0.5). q in [0, 100].
double percentile(std::vector<double> v, double q);

/// np.percentile(v, qs) for several q at once (one copy of v).
std::vector<double> percentiles(std::vector<double> v, const std::vector<double>& qs);

/// np.median: the middle value, or (a + b) / 2 of the two middle values when n is even.
double median(std::vector<double> v);

/// median(|v - median(v)|), unscaled; callers multiply by 1.4826 where Python does.
double mad(const std::vector<double>& v);

/// np.interp(xq, xp, fp, left, right). xp must be non-decreasing (numpy does not check either).
/// A NaN query returns NaN. Throws std::invalid_argument on empty xp or a size mismatch.
std::vector<double> interp(const std::vector<double>& xq, const std::vector<double>& xp,
                           const std::vector<double>& fp, double left, double right);

/// np.interp's defaults: left = fp[0], right = fp[-1].
std::vector<double> interp(const std::vector<double>& xq, const std::vector<double>& xp,
                           const std::vector<double>& fp);

/// np.nanmedian(np.vstack(stack), axis=0): per column, the median of the non-NaN entries; NaN
/// for an all-NaN column. Every row must have the same length (std::invalid_argument otherwise).
std::vector<double> nanmedian_rows(const std::vector<std::vector<double>>& stack);

}  // namespace gdg
