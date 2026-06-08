#pragma once

#include <cstdint>
#include <cfloat>
#include <cmath>

namespace nukex {

/// Welford's online algorithm for numerically stable single-pass mean and variance.
///
/// Reference: Welford B.P. (1962), "Note on a method for calculating corrected
/// sums of squares and products." Technometrics 4(3):419-420.
///
/// Unlike naive variance (Σx²/n - (Σx/n)²), this algorithm avoids catastrophic
/// cancellation when the mean is large relative to the standard deviation.
struct WelfordAccumulator {
    float    mean    = 0.0f;
    float    M2      = 0.0f;    // sum of squared deviations from running mean
    float    min_val = FLT_MAX;
    float    max_val = -FLT_MAX;
    uint32_t n       = 0;

    void update(float x) {
        n++;
        float delta  = x - mean;
        mean        += delta / static_cast<float>(n);
        float delta2 = x - mean;   // note: uses UPDATED mean
        M2          += delta * delta2;
        min_val      = (x < min_val) ? x : min_val;
        max_val      = (x > max_val) ? x : max_val;
    }

    float variance() const {
        return (n > 1) ? std::max(0.0f, M2) / static_cast<float>(n - 1) : 0.0f;
    }

    float std_dev() const {
        return std::sqrt(variance());
    }

    uint32_t count() const {
        return n;
    }

    void reset() {
        mean    = 0.0f;
        M2      = 0.0f;
        min_val = FLT_MAX;
        max_val = -FLT_MAX;
        n       = 0;
    }
};

} // namespace nukex
