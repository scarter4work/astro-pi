#pragma once

#include "nukex/core/distribution.hpp"
#include <cmath>

namespace nukex {

/// Result of fitting a parametric or non-parametric model to sample data.
struct FitResult {
    ZDistribution distribution;
    double        log_likelihood = 0.0;
    int           n_params       = 0;
    int           n_samples      = 0;
    bool          converged      = false;

    /// Corrected Akaike Information Criterion (Hurvich & Tsai 1989).
    /// AICc = 2k - 2·ln(L) + 2k(k+1)/(N-k-1)
    /// Reference: Burnham & Anderson (2002), Model Selection and Multimodel
    /// Inference, 2nd ed., Springer.
    double aicc() const {
        if (n_samples <= n_params + 1) return 1e30;
        double aic = 2.0 * n_params - 2.0 * log_likelihood;
        double correction = (2.0 * n_params * (n_params + 1.0))
                          / (n_samples - n_params - 1.0);
        return aic + correction;
    }
};

/// Abstract interface for distribution fitting backends.
class CurveFitter {
public:
    virtual ~CurveFitter() = default;
    virtual FitResult fit(const float* values, const float* weights,
                          int n, double robust_location,
                          double robust_scale) = 0;
};

} // namespace nukex
