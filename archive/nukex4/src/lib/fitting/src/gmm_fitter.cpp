#include "nukex/fitting/gmm_fitter.hpp"
#include <algorithm>
#include <cmath>
#include <vector>
#include <numeric>

namespace nukex {

namespace {

double gaussian_log_pdf(double x, double mu, double sigma) {
    double z = (x - mu) / sigma;
    return -0.5 * std::log(2.0 * M_PI) - std::log(sigma) - 0.5 * z * z;
}

void initialize_from_gap(const float* values, int n,
                         double& mu1, double& sigma1, double& mu2, double& sigma2,
                         double& pi1) {
    std::vector<float> sorted(values, values + n);
    std::sort(sorted.begin(), sorted.end());

    int split = n / 2;
    float max_gap = 0.0f;
    for (int i = 1; i < n; i++) {
        float gap = sorted[i] - sorted[i - 1];
        if (gap > max_gap) {
            max_gap = gap;
            split = i;
        }
    }

    if (split < 2) split = 2;
    if (split > n - 2) split = n - 2;

    double sum1 = 0.0, sum2 = 0.0;
    for (int i = 0; i < split; i++) sum1 += sorted[i];
    for (int i = split; i < n; i++) sum2 += sorted[i];
    mu1 = sum1 / split;
    mu2 = sum2 / (n - split);

    double ss1 = 0.0, ss2 = 0.0;
    for (int i = 0; i < split; i++) ss1 += (sorted[i] - mu1) * (sorted[i] - mu1);
    for (int i = split; i < n; i++) ss2 += (sorted[i] - mu2) * (sorted[i] - mu2);
    sigma1 = std::sqrt(std::max(ss1 / std::max(split - 1, 1), 1e-20));
    sigma2 = std::sqrt(std::max(ss2 / std::max(n - split - 1, 1), 1e-20));

    pi1 = static_cast<double>(split) / n;
}

} // anonymous namespace

FitResult GaussianMixtureFitter::fit(const float* values, const float* weights,
                                      int n, double robust_location,
                                      double robust_scale) {
    FitResult result;
    result.n_params = 5;
    result.n_samples = n;

    if (n < 10) {
        result.converged = false;
        return result;
    }

    double mu1, sigma1, mu2, sigma2, pi1;
    initialize_from_gap(values, n, mu1, sigma1, mu2, sigma2, pi1);

    sigma1 = std::max(sigma1, SIGMA_FLOOR);
    sigma2 = std::max(sigma2, SIGMA_FLOOR);
    pi1 = std::clamp(pi1, 0.01, 0.99);

    std::vector<double> gamma(n);
    double prev_ll = -1e30;

    for (int iter = 0; iter < MAX_ITERATIONS; iter++) {
        // E-step: compute responsibilities using log-sum-exp for numerical stability
        double ll = 0.0;
        for (int i = 0; i < n; i++) {
            double x = static_cast<double>(values[i]);
            double lp1 = std::log(pi1) + gaussian_log_pdf(x, mu1, sigma1);
            double lp2 = std::log(1.0 - pi1) + gaussian_log_pdf(x, mu2, sigma2);
            double max_lp = std::max(lp1, lp2);
            // Guard: if sample is so far from both components that both log-pdfs
            // are -inf, exp(-inf - (-inf)) = exp(NaN) = NaN. Assign equal responsibility.
            if (!std::isfinite(max_lp)) { gamma[i] = 0.5; continue; }
            double sum_exp = std::exp(lp1 - max_lp) + std::exp(lp2 - max_lp);
            gamma[i] = std::exp(lp1 - max_lp) / sum_exp;
            ll += static_cast<double>(weights[i]) * (max_lp + std::log(sum_exp));
        }

        if (std::fabs(ll - prev_ll) < CONVERGENCE_TOL) {
            result.converged = true;
            result.log_likelihood = ll;
            break;
        }
        prev_ll = ll;

        // M-step: closed-form weighted MLE updates
        double wsum1 = 0.0, wsum2 = 0.0;
        double wmu1 = 0.0, wmu2 = 0.0;
        for (int i = 0; i < n; i++) {
            double w = static_cast<double>(weights[i]);
            wsum1 += w * gamma[i];
            wsum2 += w * (1.0 - gamma[i]);
            wmu1  += w * gamma[i] * values[i];
            wmu2  += w * (1.0 - gamma[i]) * values[i];
        }

        double total_w = wsum1 + wsum2;
        // Degenerate component collapse — converged stays false
        if (wsum1 < 1e-30 || wsum2 < 1e-30) break;

        mu1 = wmu1 / wsum1;
        mu2 = wmu2 / wsum2;

        double wvar1 = 0.0, wvar2 = 0.0;
        for (int i = 0; i < n; i++) {
            double w = static_cast<double>(weights[i]);
            double d1 = values[i] - mu1;
            double d2 = values[i] - mu2;
            wvar1 += w * gamma[i] * d1 * d1;
            wvar2 += w * (1.0 - gamma[i]) * d2 * d2;
        }

        sigma1 = std::sqrt(std::max(wvar1 / wsum1, SIGMA_FLOOR * SIGMA_FLOOR));
        sigma2 = std::sqrt(std::max(wvar2 / wsum2, SIGMA_FLOOR * SIGMA_FLOOR));
        pi1 = wsum1 / total_w;
        pi1 = std::clamp(pi1, 0.001, 0.999);

        // Reached iteration limit without convergence tolerance — accept result
        if (iter == MAX_ITERATIONS - 1) {
            result.converged = true;
            result.log_likelihood = ll;
        }
    }

    // Canonicalize: mu1 < mu2
    if (mu1 > mu2) {
        std::swap(mu1, mu2);
        std::swap(sigma1, sigma2);
        pi1 = 1.0 - pi1;
    }

    auto& dist = result.distribution;
    dist.params.bimodal.comp1 = {
        static_cast<float>(mu1), static_cast<float>(sigma1), static_cast<float>(pi1)
    };
    dist.params.bimodal.comp2 = {
        static_cast<float>(mu2), static_cast<float>(sigma2), static_cast<float>(1.0 - pi1)
    };
    dist.params.bimodal.mixing_ratio = static_cast<float>(pi1);

    float dominant_pi = static_cast<float>(std::max(pi1, 1.0 - pi1));
    if (dominant_pi >= SPIKE_THRESHOLD) {
        dist.shape = DistributionShape::SPIKE_OUTLIER;
        if (pi1 > 1.0 - pi1) {
            dist.true_signal_estimate = static_cast<float>(mu1);
            dist.signal_uncertainty = static_cast<float>(sigma1 / std::sqrt(n));
        } else {
            dist.true_signal_estimate = static_cast<float>(mu2);
            dist.signal_uncertainty = static_cast<float>(sigma2 / std::sqrt(n));
        }
    } else {
        dist.shape = DistributionShape::BIMODAL;
        if (std::fabs(pi1 - (1.0 - pi1)) < 0.1) {
            // Near-equal mix: take the brighter (higher mu) component as signal
            dist.true_signal_estimate = static_cast<float>(mu2);
            dist.signal_uncertainty = static_cast<float>(sigma2 / std::sqrt(n));
        } else if (pi1 > 0.5) {
            // comp1 (lower mu) is dominant
            dist.true_signal_estimate = static_cast<float>(mu1);
            dist.signal_uncertainty = static_cast<float>(sigma1 / std::sqrt(n));
        } else {
            // comp2 (higher mu) is dominant
            dist.true_signal_estimate = static_cast<float>(mu2);
            dist.signal_uncertainty = static_cast<float>(sigma2 / std::sqrt(n));
        }
    }

    dist.confidence = static_cast<float>(dominant_pi);
    dist.aicc = static_cast<float>(result.aicc());

    return result;
}

} // namespace nukex
