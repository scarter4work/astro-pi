#pragma once
#include <vector>
#include <cstddef>
#include <utility>
#include <optional>
#include <cmath>

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

struct DetectorConfig {
    double sigmaThreshold = 6.0;   // detection threshold in MAD units above median
    int    minBlobSize    = 3;     // minimum pixels in a star blob
    int    maxBlobSize    = 200;   // maximum pixels (reject nebula cores)
    double maxEccentricity = 0.7;  // reject elongated blobs (trails/satellites)
};

// Convert a blob to a StarPosition (returns nullopt if blob fails filtering)
std::optional<StarPosition> blobToStar(const Blob& blob, const float* image,
                                        int width, int height,
                                        int minSize = 3, int maxSize = 200,
                                        double maxEccentricity = 0.7);

// Full star detection pipeline: background → threshold → blobs → filter → centroids
// Returns stars sorted by flux (brightest first)
std::vector<StarPosition> detectStars(const float* image, int width, int height,
                                       const DetectorConfig& config = DetectorConfig{});

} // namespace nukex
