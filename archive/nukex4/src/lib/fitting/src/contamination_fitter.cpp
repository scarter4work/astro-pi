#include "nukex/fitting/contamination_fitter.hpp"

#include <ceres/ceres.h>
#include <cmath>
#include <algorithm>
#include <vector>

namespace nukex {

// ============================================================
// Contamination model Negative Log-Likelihood as a Ceres FirstOrderFunction.
//
// Model: p(x) = (1-ε)·N(μ, σ²) + ε·U(a, b)
//   where U(a,b) is uniform with fixed range derived from data extent.
//
// Parameters (unconstrained):
//   params[0] = μ             (location, free)
//   params[1] = log(σ)        (enforces σ > 0)
//   params[2] = logit(ε)      (logit(ε) = log(ε/(1-ε)), enforces ε ∈ (0,1))
//
// NLL and analytical gradients are computed in a single pass over samples.
//
// Reference: Hogg, Bovy & Lang (2010), arXiv:1008.4686.
// ============================================================

class ContaminationNLL : public ceres::FirstOrderFunction {
public:
    ContaminationNLL(const float* values, const float* weights, int n,
                     double u_lo, double u_hi,
                     double eps_min, double eps_max)
        : values_(values), weights_(weights), n_(n),
          u_lo_(u_lo), u_hi_(u_hi),
          eps_min_(eps_min), eps_max_(eps_max),
          log_uniform_density_(-std::log(u_hi - u_lo)) {}

    bool Evaluate(const double* params,
                  double* cost,
                  double* gradient) const override {
        const double mu        = params[0];
        const double log_sigma = params[1];
        const double logit_eps = params[2];

        const double sigma  = std::exp(log_sigma);

        // Recover ε from logit parametrization (numerically stable sigmoid)
        double eps;
        if (logit_eps >= 0.0) {
            eps = 1.0 / (1.0 + std::exp(-logit_eps));
        } else {
            double e = std::exp(logit_eps);
            eps = e / (1.0 + e);
        }
        eps = std::clamp(eps, eps_min_, eps_max_);

        const double one_minus_eps = 1.0 - eps;

        // Gaussian normalisation constant: log(1/(sqrt(2π)*σ))
        const double log_gauss_norm = -0.5 * std::log(2.0 * M_PI) - log_sigma;

        // Uniform density (constant over [u_lo, u_hi])
        const double uniform_density = std::exp(log_uniform_density_);

        double nll        = 0.0;
        double grad_mu    = 0.0;
        double grad_lsig  = 0.0;
        double grad_logit = 0.0;

        for (int i = 0; i < n_; ++i) {
            const double x = static_cast<double>(values_[i]);
            const double w = static_cast<double>(weights_[i]);

            const double z      = (x - mu) / sigma;
            const double z2     = z * z;
            const double gauss  = std::exp(log_gauss_norm - 0.5 * z2);

            const double p = one_minus_eps * gauss + eps * uniform_density;

            // Guard against p ≤ 0 (shouldn't happen with proper clamping but be safe)
            if (p <= 0.0) continue;

            nll += -w * std::log(p);

            if (gradient) {
                const double inv_p = 1.0 / p;

                // d(log p)/d(μ):  only Gaussian term contributes
                //   d(gauss)/d(μ) = gauss * (x - μ) / σ²
                const double d_gauss_dmu = gauss * z / sigma;
                grad_mu += -w * inv_p * one_minus_eps * d_gauss_dmu;

                // d(log p)/d(log σ):
                //   d(gauss)/d(log σ) = gauss * (-1 + z²)
                //   (because d/d(log σ) [log_gauss_norm - 0.5*z²]
                //     = -1 - 0.5*d(z²)/d(log σ) = -1 - 0.5*(-2*z²) = -1 + z²)
                const double d_gauss_dlsig = gauss * (-1.0 + z2);
                grad_lsig += -w * inv_p * one_minus_eps * d_gauss_dlsig;

                // d(log p)/d(logit ε):
                //   d ε / d(logit ε) = ε * (1 - ε)
                //   d p / d ε        = -gauss + uniform_density
                const double deps_dlogit = eps * one_minus_eps;
                const double dp_deps = -gauss + uniform_density;
                grad_logit += -w * inv_p * dp_deps * deps_dlogit;
            }
        }

        *cost = nll;

        if (gradient) {
            gradient[0] = grad_mu;
            gradient[1] = grad_lsig;
            gradient[2] = grad_logit;
        }

        // Defensive: tell Ceres to back off instead of CHECK-failing the
        // Wolfe line search when outputs go non-finite.
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
    const float* values_;
    const float* weights_;
    int          n_;
    double       u_lo_;
    double       u_hi_;
    double       eps_min_;
    double       eps_max_;
    double       log_uniform_density_;  // log(1/(b-a)), precomputed
};

// ============================================================
// ContaminationFitter::fit()
// ============================================================

FitResult ContaminationFitter::fit(const float* values, const float* weights,
                                    int n, double robust_location,
                                    double robust_scale) {
    FitResult result;
    result.converged = false;
    result.n_params  = 3;   // μ, σ, ε
    result.n_samples = n;

    // Edge cases
    if (n < 5)              return result;
    if (robust_scale < 1e-30) return result;

    // Determine fixed uniform range from data extent
    float data_min = values[0];
    float data_max = values[0];
    for (int i = 1; i < n; ++i) {
        if (values[i] < data_min) data_min = values[i];
        if (values[i] > data_max) data_max = values[i];
    }
    const double data_range = static_cast<double>(data_max - data_min);
    const double margin     = 0.1 * data_range;
    const double u_lo       = static_cast<double>(data_min) - margin;
    const double u_hi       = static_cast<double>(data_max) + margin;

    // Guard: degenerate data range
    if ((u_hi - u_lo) < 1e-30) return result;

    // Initial parameter vector: [μ, log(σ), logit(0.05)]
    // logit(0.05) = log(0.05 / 0.95)
    double params[3];
    params[0] = robust_location;
    params[1] = std::log(std::max(robust_scale, SIGMA_MIN));
    params[2] = std::log(0.05 / 0.95);   // ≈ -2.944

    // Build Ceres GradientProblem
    auto* nll_function = new ContaminationNLL(values, weights, n,
                                              u_lo, u_hi,
                                              EPS_MIN, EPS_MAX);
    ceres::GradientProblem problem(nll_function);

    // Configure solver — same settings as StudentTFitter
    ceres::GradientProblemSolver::Options options;
    options.line_search_direction_type = ceres::BFGS;
    options.max_num_iterations         = 200;
    options.function_tolerance         = 1e-10;
    options.gradient_tolerance         = 1e-8;
    options.logging_type               = ceres::SILENT;

    ceres::GradientProblemSolver::Summary summary;
    ceres::Solve(options, problem, params, &summary);

    bool converged = (summary.termination_type == ceres::CONVERGENCE
                   || summary.termination_type == ceres::USER_SUCCESS);
    if (!converged) return result;

    // Extract fitted parameters
    const double mu        = params[0];
    const double sigma     = std::max(std::exp(params[1]), SIGMA_MIN);
    // Numerically stable sigmoid to prevent NaN when logit > 709
    double eps;
    if (params[2] >= 0.0) {
        eps = 1.0 / (1.0 + std::exp(-params[2]));
    } else {
        double e = std::exp(params[2]);
        eps = e / (1.0 + e);
    }
    eps = std::clamp(eps, EPS_MIN, EPS_MAX);
    const double one_minus_eps = 1.0 - eps;

    // Populate result
    result.converged = true;

    auto& dist = result.distribution;
    dist.shape                     = DistributionShape::CONTAMINATED;
    dist.params.contamination.mu   = static_cast<float>(mu);
    dist.params.contamination.sigma = static_cast<float>(sigma);
    dist.params.contamination.contamination_frac = static_cast<float>(eps);

    // Signal estimate is the clean Gaussian location
    dist.true_signal_estimate = static_cast<float>(mu);

    // Uncertainty: SE of Gaussian mean, accounting for effective sample size
    // Only the (1-ε) fraction of samples contribute clean signal.
    // Effective N = n · (1-ε); SE = σ / sqrt(N_eff)
    const double n_eff = static_cast<double>(n) * one_minus_eps;
    if (n_eff > 1.0) {
        dist.signal_uncertainty = static_cast<float>(sigma / std::sqrt(n_eff));
    } else {
        dist.signal_uncertainty = static_cast<float>(sigma);
    }

    // Confidence: fraction of clean signal
    dist.confidence = static_cast<float>(one_minus_eps);

    result.log_likelihood = -summary.final_cost;
    dist.aicc = static_cast<float>(result.aicc());

    return result;
}

} // namespace nukex
