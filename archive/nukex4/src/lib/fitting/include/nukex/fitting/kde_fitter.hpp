#pragma once
#include "nukex/fitting/curve_fitter.hpp"
namespace nukex {
class KDEFitter : public CurveFitter {
public:
    static constexpr int GRID_SIZE = 512;
    FitResult fit(const float* values, const float* weights,
                  int n, double robust_location, double robust_scale) override;
    static double evaluate_kde(double x, const float* values, int n, double h);
    static double find_mode(const float* values, int n, double h,
                            double grid_min, double grid_max);
    static double isj_bandwidth(const float* values, int n);
};
} // namespace nukex
