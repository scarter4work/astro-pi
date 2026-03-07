#pragma once
#include <vector>
#include <cstddef>
#include <utility>

namespace nukex {

struct StarPosition {
    double x;       // centroid x (sub-pixel precision from intensity weighting)
    double y;       // centroid y
    double flux;    // total intensity of the star blob
};

// Compute background statistics: returns (median, MAD)
std::pair<double, double> computeBackground(const float* data, size_t count);

} // namespace nukex
