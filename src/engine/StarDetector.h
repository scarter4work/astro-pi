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

// A blob is a list of (x, y) pixel coordinates belonging to one connected component
typedef std::vector<std::pair<int, int>> Blob;

// Compute background statistics: returns (median, MAD)
std::pair<double, double> computeBackground(const float* data, size_t count);

// Extract connected components of pixels above threshold (4-connectivity flood fill)
std::vector<Blob> extractBlobs(const float* image, int width, int height, double threshold);

} // namespace nukex
