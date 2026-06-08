#pragma once
#include "nukex/fitting/curve_fitter.hpp"

namespace nukex {

/// Student-t distribution fitter using Maximum Likelihood Estimation.
///
/// Fits Student-t(mu, sigma, nu) to sample data via Ceres Solver's
/// GradientProblem API (BFGS line search). Analytical gradients for mu
/// and log(sigma); central finite differences for nu (digamma not
/// needed). Classifies as GAUSSIAN when nu > 30, HEAVY_TAILED otherwise.
///
/// Reference: Lange, Little & Taylor (1989), JASA 84(408), 881-896.
class StudentTFitter : public CurveFitter {
public:
    static constexpr float NU_GAUSSIAN_THRESHOLD = 30.0f;
    static constexpr double SIGMA_MIN = 1e-10;
    static constexpr double NU_MIN    = 2.0;
    static constexpr double NU_MAX    = 100.0;
    FitResult fit(const float* values, const float* weights,
                  int n, double robust_location, double robust_scale) override;
};

} // namespace nukex
