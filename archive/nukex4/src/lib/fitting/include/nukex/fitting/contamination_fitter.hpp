#pragma once
#include "nukex/fitting/curve_fitter.hpp"
namespace nukex {
class ContaminationFitter : public CurveFitter {
public:
    static constexpr double SIGMA_MIN = 1e-10;
    static constexpr double EPS_MIN   = 0.001;
    static constexpr double EPS_MAX   = 0.5;
    FitResult fit(const float* values, const float* weights,
                  int n, double robust_location, double robust_scale) override;
};
} // namespace nukex
