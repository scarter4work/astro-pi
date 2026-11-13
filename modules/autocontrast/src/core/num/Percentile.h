// AutoContrast core (PCL-free): numpy-exact percentile / median.
#pragma once

#include <vector>

namespace ac {

// np.percentile(v, q) with method="linear", bit for bit. q in [0, 100], v non-empty (throws
// std::invalid_argument otherwise). A NaN anywhere in v gives NaN, as in numpy.
double percentile(std::vector<double> v, double q);

// np.percentile(v, qs): one selection pass for all levels.
std::vector<double> percentiles(std::vector<double> v, const std::vector<double>& qs);

// np.median(v): the middle element, or the mean (a + b) / 2 of the two middle ones. v non-empty.
double median(std::vector<double> v);

} // namespace ac
