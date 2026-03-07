#include "engine/StarDetector.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace nukex {

std::pair<double, double> computeBackground(const float* data, size_t count) {
    if (count == 0) return {0.0, 0.0};

    // Copy and sort for median
    std::vector<double> sorted(data, data + count);
    std::sort(sorted.begin(), sorted.end());

    double median;
    if (count % 2 == 0) {
        median = (sorted[count / 2 - 1] + sorted[count / 2]) / 2.0;
    } else {
        median = sorted[count / 2];
    }

    // Compute MAD = median(|x_i - median|)
    std::vector<double> absdev(count);
    for (size_t i = 0; i < count; ++i) {
        absdev[i] = std::abs(sorted[i] - median);
    }
    std::sort(absdev.begin(), absdev.end());

    double mad;
    if (count % 2 == 0) {
        mad = (absdev[count / 2 - 1] + absdev[count / 2]) / 2.0;
    } else {
        mad = absdev[count / 2];
    }

    return {median, mad};
}

} // namespace nukex
