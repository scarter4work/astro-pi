#pragma once
#include <vector>
#include <cstdint>

namespace nukex {

// Returns indices of detected outliers using Generalized ESD test
// maxOutliers: maximum number of outliers to detect
// alpha: significance level (default 0.05)
std::vector<size_t> detectOutliersESD(
    const std::vector<double>& data,
    int maxOutliers,
    double alpha = 0.05);

// MAD-based sigma clipping: returns indices of values beyond
// kappa * 1.4826 * MAD from the median.
// MAD (Median Absolute Deviation) is robust to outliers — unlike stddev,
// it doesn't inflate when outliers are present, so transients can't mask
// their own detection.
// The 1.4826 factor normalizes MAD to Gaussian sigma equivalence.
std::vector<size_t> sigmaClipMAD(
    const std::vector<double>& data,
    double kappa = 3.0);

// Returns indices of detected outliers using Chauvenet's criterion
std::vector<size_t> detectOutliersChauvenet(
    const std::vector<double>& data);

// Single Grubbs' test -- returns index of outlier or SIZE_MAX if none
// This is a building block for ESD
size_t grubbsTest(
    const std::vector<double>& data,
    double alpha = 0.05);

} // namespace nukex
