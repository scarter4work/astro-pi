#pragma once

#include "SubCube.h"
#include <vector>

namespace nukex {

struct WeightConfig {
    double fwhmWeight = 1.0;
    double eccentricityWeight = 1.0;
    double skyBackgroundWeight = 0.5;
    double hfrWeight = 1.0;
    double altitudeWeight = 0.3;
};

// Returns normalized weight vector (sums to 1.0)
// Higher weight = better sub
std::vector<double> ComputeQualityWeights(
    const std::vector<SubMetadata>& metadata,
    const WeightConfig& config);

} // namespace nukex
