#pragma once
#include "nukex/fitting/curve_fitter.hpp"
namespace nukex {
class GaussianMixtureFitter : public CurveFitter {
public:
    static constexpr int    MAX_ITERATIONS = 100;
    static constexpr double CONVERGENCE_TOL = 1e-8;
    static constexpr double SIGMA_FLOOR = 1e-10;
    static constexpr float  SPIKE_THRESHOLD = 0.95f;
    static constexpr float  BIMODAL_MIN_MIX = 0.05f;
    FitResult fit(const float* values, const float* weights,
                  int n, double robust_location, double robust_scale) override;
};
} // namespace nukex
