#pragma once
#include "nukex/fitting/curve_fitter.hpp"
namespace nukex {
class KDEFitter : public CurveFitter {
public:
    static constexpr int GRID_SIZE = 512;
    /// n < 3: no KDE is possible; returns converged=false with
    /// true_signal_estimate = robust_location and signal_uncertainty =
    /// robust_scale (shape UNKNOWN, used_nonparametric). Callers keep the
    /// FIT_FAILED flag as the loud signal; the value is never zeroed.
    FitResult fit(const float* values, const float* weights,
                  int n, double robust_location, double robust_scale) override;
    static double evaluate_kde(double x, const float* values, int n, double h);
    static double find_mode(const float* values, int n, double h,
                            double grid_min, double grid_max);
    static double isj_bandwidth(const float* values, int n);
};
} // namespace nukex
