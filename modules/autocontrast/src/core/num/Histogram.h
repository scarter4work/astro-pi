// AutoContrast core (PCL-free): numpy-exact histograms.
#pragma once

#include "core/Image.h"

#include <cstddef>
#include <vector>

namespace ac {

// np.histogram(x, bins=<int>, range=(lo, hi), weights=w) (numpy/lib/_histograms_impl.py, uniform fast path
// 812-873): values outside [lo, hi] and NaN are dropped; the last bin includes hi; bin edges are
// np.linspace(lo, hi, bins + 1); indices are corrected against those edges; weighted sums are
// accumulated per 65536-element block (np.bincount order) and the blocks added in order. `w` may be null
// (counts). lo < hi required (equal edges are widened by +-0.5 as numpy does).
std::vector<double> histogram_uniform(const double* x, const double* w, size_t n, int bins, double lo, double hi);

// np.histogram2d(a, b, bins=[e, e], weights=w) with e = np.linspace(-extent, extent, bins + 1)
// (histogramdd, _histograms_impl.py 1042-1061): searchsorted(side='right'), value == last edge goes into
// the last bin, everything else outside dropped, sequential bincount accumulation. Row-major bins x bins,
// index [ia * bins + ib]. a, b, w have the same size; an empty `w` plane means unweighted counts.
std::vector<double> histogram2d(const Plane& a, const Plane& b, const Plane& w, int bins, double extent);

// np.linspace(start, stop, num) exactly (numpy/_core/function_base.py 141-176).
std::vector<double> linspace(double start, double stop, int num);

} // namespace ac
