#include "nukex/fitting/kde_fitter.hpp"
#include <algorithm>
#include <cmath>
#include <vector>
#include <numeric>

namespace nukex {

double KDEFitter::evaluate_kde(double x, const float* values, int n, double h) {
    if (n <= 0 || h <= 0.0) return 0.0;
    double sum = 0.0;
    double inv_h = 1.0 / h;
    double norm = inv_h / (std::sqrt(2.0 * M_PI) * n);
    for (int i = 0; i < n; i++) {
        double z = (x - values[i]) * inv_h;
        sum += std::exp(-0.5 * z * z);
    }
    return sum * norm;
}

double KDEFitter::find_mode(const float* values, int n, double h,
                             double grid_min, double grid_max) {
    double best_x = grid_min;
    double best_density = -1.0;
    double step = (grid_max - grid_min) / GRID_SIZE;

    for (int i = 0; i <= GRID_SIZE; i++) {
        double x = grid_min + i * step;
        double d = evaluate_kde(x, values, n, h);
        if (d > best_density) {
            best_density = d;
            best_x = x;
        }
    }

    // Newton refinement (3 iterations)
    double eps = h * 0.001;
    for (int iter = 0; iter < 3; iter++) {
        double f_plus = evaluate_kde(best_x + eps, values, n, h);
        double f_minus = evaluate_kde(best_x - eps, values, n, h);
        double f_center = evaluate_kde(best_x, values, n, h);
        double first_deriv = (f_plus - f_minus) / (2.0 * eps);
        double second_deriv = (f_plus - 2.0 * f_center + f_minus) / (eps * eps);
        if (std::fabs(second_deriv) < 1e-30) break;
        double delta = -first_deriv / second_deriv;
        delta = std::clamp(delta, -step, step);
        best_x += delta;
        best_x = std::clamp(best_x, grid_min, grid_max);
    }

    return best_x;
}

double KDEFitter::isj_bandwidth(const float* values, int n) {
    if (n <= 1) return 1.0;

    // Sheather-Jones plug-in bandwidth selection
    // Uses roughness estimation via kernel second derivatives
    std::vector<float> sorted(values, values + n);
    std::sort(sorted.begin(), sorted.end());
    double data_min = sorted[0];
    double data_max = sorted[n - 1];
    double range = data_max - data_min;
    if (range < 1e-30) return 1.0;

    // IQR-based initial estimate (Silverman's rule as starting point)
    int q1 = n / 4, q3 = (3 * n) / 4;
    double iqr_val = sorted[q3] - sorted[q1];

    // Compute sample standard deviation
    double mean = 0.0;
    for (int i = 0; i < n; i++) mean += values[i];
    mean /= n;
    double var = 0.0;
    for (int i = 0; i < n; i++) var += (values[i] - mean) * (values[i] - mean);
    double stddev = std::sqrt(var / (n - 1));

    double sigma_hat = std::min(stddev, iqr_val / 1.34);
    if (sigma_hat < 1e-30) sigma_hat = range;

    double h_silverman = 0.9 * sigma_hat * std::pow(n, -0.2);

    // For large N, the O(N²) roughness estimation is too slow.
    // Subsample to at most 100 points with a deterministic stride.
    int sub_n = n;
    std::vector<float> subsample;
    if (n > 100) {
        int step = n / 100;
        subsample.reserve(100);
        for (int i = 0; i < n; i += step) {
            subsample.push_back(sorted[i]);
        }
        sub_n = static_cast<int>(subsample.size());
    }
    const float* sub_data = (n > 100) ? subsample.data() : sorted.data();

    // Roughness estimation via pilot bandwidth
    double h_pilot = 1.5 * h_silverman;

    // Estimate integral(f''²)dx using kernel second derivative
    // K''(z) = (z² - 1) · K(z) for Gaussian kernel
    double roughness = 0.0;
    for (int i = 0; i < sub_n; i++) {
        for (int j = 0; j < sub_n; j++) {
            double z = (sub_data[i] - sub_data[j]) / h_pilot;
            double z2 = z * z;
            double k = std::exp(-0.5 * z2) / std::sqrt(2.0 * M_PI);
            roughness += (z2 - 1.0) * k / (h_pilot * h_pilot);
        }
    }
    roughness /= (static_cast<double>(sub_n) * sub_n * h_pilot);

    if (std::fabs(roughness) < 1e-30) return h_silverman;

    // h_opt from AMISE: h = (1 / (2·sqrt(pi) · N · |integral(f''²)|))^(1/5)
    double h_opt = std::pow(1.0 / (2.0 * std::sqrt(M_PI) * n * std::fabs(roughness)), 0.2);
    h_opt = std::clamp(h_opt, range * 0.001, range * 0.5);

    return h_opt;
}

FitResult KDEFitter::fit(const float* values, const float* weights,
                          int n, double robust_location, double robust_scale) {
    FitResult result;
    result.n_params = 1;
    result.n_samples = n;

    if (n < 3) {
        result.converged = false;
        return result;
    }

    double h = isj_bandwidth(values, n);

    float data_min = *std::min_element(values, values + n);
    float data_max = *std::max_element(values, values + n);
    double pad = 3.0 * h;
    double mode = find_mode(values, n, h, data_min - pad, data_max + pad);

    // Log-likelihood for AICc comparison
    double ll = 0.0;
    for (int i = 0; i < n; i++) {
        double d = evaluate_kde(values[i], values, n, h);
        if (d > 0.0) {
            ll += static_cast<double>(weights[i]) * std::log(d);
        }
    }

    result.converged = true;
    result.log_likelihood = ll;

    auto& dist = result.distribution;
    dist.shape = DistributionShape::UNIFORM;
    dist.true_signal_estimate = static_cast<float>(mode);
    dist.signal_uncertainty = static_cast<float>(h / std::sqrt(n));
    dist.confidence = 0.5f;
    dist.kde_mode = static_cast<float>(mode);
    dist.kde_bandwidth = static_cast<float>(h);
    dist.used_nonparametric = true;
    dist.aicc = static_cast<float>(result.aicc());

    return result;
}

} // namespace nukex
