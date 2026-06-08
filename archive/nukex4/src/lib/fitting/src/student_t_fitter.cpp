#include "nukex/fitting/student_t_fitter.hpp"

#include <ceres/ceres.h>
#include <cmath>
#include <algorithm>
#include <vector>

namespace nukex {

// ============================================================
// Student-t Negative Log-Likelihood as a Ceres FirstOrderFunction.
//
// Parameters: [0] = mu, [1] = log(sigma), [2] = nu
//
// NLL for one weighted sample (x_i, w_i):
//   w_i * [ -lgamma((nu+1)/2) + lgamma(nu/2) + 0.5*log(nu*pi*sigma^2)
//           + ((nu+1)/2)*log(1 + (x_i - mu)^2 / (nu*sigma^2)) ]
//
// Analytical gradients for mu and log(sigma).
// Central finite differences for nu (avoids digamma implementation).
//
// Reference: Lange, Little & Taylor (1989), JASA 84(408), 881-896.
// ============================================================

class StudentTNLL : public ceres::FirstOrderFunction {
public:
    StudentTNLL(const float* values, const float* weights, int n,
                double nu_min, double nu_max)
        : values_(values), weights_(weights), n_(n),
          nu_min_(nu_min), nu_max_(nu_max) {}

    bool Evaluate(const double* params,
                  double* cost,
                  double* gradient) const override {
        const double mu       = params[0];
        const double log_sigma = params[1];
        const double nu       = params[2];

        const double sigma = std::exp(log_sigma);
        const double sigma2 = sigma * sigma;

        // Precompute nu-dependent terms
        const double half_nu_plus_1 = 0.5 * (nu + 1.0);
        const double half_nu        = 0.5 * nu;
        const double lgamma_half_nup1 = std::lgamma(half_nu_plus_1);
        const double lgamma_half_nu   = std::lgamma(half_nu);
        const double log_norm = -lgamma_half_nup1 + lgamma_half_nu
                              + 0.5 * std::log(nu * M_PI * sigma2);

        double nll = 0.0;
        double grad_mu = 0.0;
        double grad_log_sigma = 0.0;

        for (int i = 0; i < n_; ++i) {
            const double x = static_cast<double>(values_[i]);
            const double w = static_cast<double>(weights_[i]);
            const double diff = x - mu;
            const double diff2 = diff * diff;
            const double z2 = diff2 / (nu * sigma2);
            const double log_term = std::log(1.0 + z2);

            // NLL contribution
            nll += w * (log_norm + half_nu_plus_1 * log_term);

            if (gradient) {
                // Derivative of log(1 + z2) w.r.t. mu:
                //   d/dmu log(1 + (x-mu)^2/(nu*sigma^2))
                //     = -2*(x-mu) / (nu*sigma^2 * (1 + z2))
                const double inv_1_plus_z2 = 1.0 / (1.0 + z2);
                const double d_log_dmu = -2.0 * diff / (nu * sigma2) * inv_1_plus_z2;

                // d(NLL_i)/d(mu) = w * (nu+1)/2 * d_log_dmu
                grad_mu += w * half_nu_plus_1 * d_log_dmu;

                // Derivative w.r.t. log(sigma):
                //   d(log_norm)/d(log_sigma) = 1.0
                //     (because log_norm has 0.5*log(sigma^2) = log(sigma),
                //      and d(log(sigma))/d(log(sigma)) = 1)
                //   d(log_term)/d(log_sigma) = d/d(log_sigma) log(1 + diff^2/(nu*sigma^2))
                //     = -2 * diff^2/(nu*sigma^2) / (1 + z2)
                //     = -2 * z2 / (1 + z2)
                const double d_log_dlogsig = -2.0 * z2 * inv_1_plus_z2;

                grad_log_sigma += w * (1.0 + half_nu_plus_1 * d_log_dlogsig);
            }
        }

        *cost = nll;

        if (gradient) {
            gradient[0] = grad_mu;
            gradient[1] = grad_log_sigma;

            // Central finite difference for nu
            constexpr double eps = 1e-5;
            double nu_plus  = std::min(nu + eps, nu_max_);
            double nu_minus = std::max(nu - eps, nu_min_);
            double actual_eps = nu_plus - nu_minus;

            if (actual_eps < 1e-15) {
                gradient[2] = 0.0;
            } else {
                double nll_plus  = compute_nll(mu, log_sigma, nu_plus);
                double nll_minus = compute_nll(mu, log_sigma, nu_minus);
                gradient[2] = (nll_plus - nll_minus) / actual_eps;
            }
        }

        // Defensive: if any output is non-finite (e.g. sigma underflowed to 0
        // or an outlier produced an overflow), tell Ceres to back off this
        // line-search step. Returning false here is much cheaper than the
        // LOG(FATAL) CHECK in ceres/line_search.cc:705 when a NaN gradient
        // reaches the Wolfe Zoom algorithm.
        if (!std::isfinite(*cost)) return false;
        if (gradient) {
            if (!std::isfinite(gradient[0])
             || !std::isfinite(gradient[1])
             || !std::isfinite(gradient[2])) return false;
        }

        return true;
    }

    int NumParameters() const override { return 3; }

private:
    /// Compute NLL at given (mu, log_sigma, nu) without gradients.
    double compute_nll(double mu, double log_sigma, double nu) const {
        const double sigma = std::exp(log_sigma);
        const double sigma2 = sigma * sigma;
        const double half_nu_plus_1 = 0.5 * (nu + 1.0);
        const double half_nu        = 0.5 * nu;
        const double lgamma_half_nup1 = std::lgamma(half_nu_plus_1);
        const double lgamma_half_nu   = std::lgamma(half_nu);
        const double log_norm = -lgamma_half_nup1 + lgamma_half_nu
                              + 0.5 * std::log(nu * M_PI * sigma2);

        double nll = 0.0;
        for (int i = 0; i < n_; ++i) {
            const double x = static_cast<double>(values_[i]);
            const double w = static_cast<double>(weights_[i]);
            const double diff = x - mu;
            const double z2 = (diff * diff) / (nu * sigma2);
            nll += w * (log_norm + half_nu_plus_1 * std::log(1.0 + z2));
        }
        return nll;
    }

    const float* values_;
    const float* weights_;
    int          n_;
    double       nu_min_;
    double       nu_max_;
};

// ============================================================
// StudentTFitter::fit()
// ============================================================

FitResult StudentTFitter::fit(const float* values, const float* weights,
                              int n, double robust_location,
                              double robust_scale) {
    FitResult result;
    result.converged  = false;
    result.n_params   = 3;   // mu, sigma, nu
    result.n_samples  = n;

    // Edge case: too few samples for 3-parameter fit
    if (n < 5) return result;

    // Edge case: zero or near-zero scale → data is constant, nothing to fit
    if (robust_scale < 1e-30) return result;

    // Initial parameter vector: [mu, log(sigma), nu]
    double params[3];
    params[0] = robust_location;
    params[1] = std::log(std::max(robust_scale, SIGMA_MIN));
    params[2] = 5.0;

    // Build Ceres GradientProblem
    auto* nll_function = new StudentTNLL(values, weights, n, NU_MIN, NU_MAX);
    ceres::GradientProblem problem(nll_function);

    // Configure solver
    ceres::GradientProblemSolver::Options options;
    options.line_search_direction_type = ceres::BFGS;
    options.max_num_iterations         = 100;
    options.function_tolerance         = 1e-10;
    options.gradient_tolerance         = 1e-8;
    options.logging_type               = ceres::SILENT;

    // Solve
    ceres::GradientProblemSolver::Summary summary;
    ceres::Solve(options, problem, params, &summary);

    // Check convergence
    bool converged = (summary.termination_type == ceres::CONVERGENCE
                   || summary.termination_type == ceres::USER_SUCCESS);

    if (!converged) return result;

    // Extract fitted parameters
    double mu    = params[0];
    double sigma = std::exp(params[1]);
    double nu    = params[2];

    // Clamp nu to valid range
    nu = std::clamp(nu, NU_MIN, NU_MAX);

    // Clamp sigma to minimum
    sigma = std::max(sigma, SIGMA_MIN);

    // Populate result
    result.converged = true;

    // Distribution parameters
    result.distribution.params.student_t.mu    = static_cast<float>(mu);
    result.distribution.params.student_t.sigma = static_cast<float>(sigma);
    result.distribution.params.student_t.nu    = static_cast<float>(nu);

    // Shape classification
    if (nu > static_cast<double>(NU_GAUSSIAN_THRESHOLD)) {
        result.distribution.shape = DistributionShape::GAUSSIAN;
    } else {
        result.distribution.shape = DistributionShape::HEAVY_TAILED;
    }

    // True signal estimate is the location parameter
    result.distribution.true_signal_estimate = static_cast<float>(mu);

    // Signal uncertainty: sigma * sqrt(nu / (nu - 2)) / sqrt(N)
    // This is the standard error of the mean under Student-t.
    // Only valid for nu > 2 (finite variance).
    if (nu > 2.0) {
        double variance_factor = nu / (nu - 2.0);
        double se = sigma * std::sqrt(variance_factor) / std::sqrt(static_cast<double>(n));
        result.distribution.signal_uncertainty = static_cast<float>(se);
    } else {
        // nu <= 2: infinite variance, uncertainty is meaningless
        result.distribution.signal_uncertainty = static_cast<float>(sigma);
    }

    // Confidence: higher nu → more confident, capped at 0.95
    result.distribution.confidence = static_cast<float>(
        std::min(0.95, 1.0 - 1.0 / nu));

    // Log-likelihood is -NLL (Ceres minimizes the NLL)
    result.log_likelihood = -summary.final_cost;

    // AICc
    result.distribution.aicc = static_cast<float>(result.aicc());

    return result;
}

} // namespace nukex
