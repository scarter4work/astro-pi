#include "engine/SkewNormalFitter.h"
#include "engine/NumericalUtils.h"

#include <Eigen/Core>
#include <LBFGS.h>
#include <boost/math/distributions/skew_normal.hpp>

#include <cmath>
#include <stdexcept>

namespace nukex {

// Objective functor for LBFGSpp
// Minimizes negative log-likelihood of the skew-normal distribution.
// Parameters: x[0] = xi (location), x[1] = log(omega) (log-scale), x[2] = alpha (shape).
class SkewNormalObjective {
public:
    explicit SkewNormalObjective(const std::vector<double>& data) : m_data(data) {}

    // Evaluate negative log-likelihood at given parameters
    double evalNLL(const Eigen::VectorXd& x) const {
        double xi    = x[0];
        double omega = std::exp(x[1]);  // log-transform keeps omega > 0
        double alpha = x[2];

        if (omega < 1e-15) omega = 1e-15;
        boost::math::skew_normal_distribution<double> dist(xi, omega, alpha);

        double negLogL = 0.0;
        for (double val : m_data) {
            double p = boost::math::pdf(dist, val);
            if (p < 1e-300) p = 1e-300;
            negLogL -= std::log(p);
        }
        return negLogL;
    }

    // LBFGSpp requires: double operator()(const VectorXd& x, VectorXd& grad)
    double operator()(const Eigen::VectorXd& x, Eigen::VectorXd& grad) {
        double negLogL = evalNLL(x);

        // Numerical gradient via central differences
        const double h = 1e-7;
        for (int i = 0; i < 3; i++) {
            Eigen::VectorXd xp = x, xm = x;
            xp[i] += h;
            xm[i] -= h;
            grad[i] = (evalNLL(xp) - evalNLL(xm)) / (2.0 * h);
        }

        return negLogL;
    }

private:
    const std::vector<double>& m_data;
};

SkewNormalFitResult fitSkewNormal(const std::vector<double>& data) {
    SkewNormalFitResult result{};
    result.k = 3;

    // Moment-based initial estimates
    double mu   = kahanMean(data.data(), data.size());
    double sigma = stddev(data.data(), data.size());
    double skew  = skewness(data.data(), data.size());
    if (sigma < 1e-10) sigma = 1e-10;

    // Initial parameter vector: [xi, log(omega), alpha]
    Eigen::VectorXd x(3);
    x << mu, std::log(sigma), skew;

    // Configure L-BFGS solver
    LBFGSpp::LBFGSParam<double> param;
    param.max_iterations = 50;
    param.epsilon        = 1e-6;
    LBFGSpp::LBFGSSolver<double> solver(param);

    SkewNormalObjective objective(data);
    double negLogL = 0.0;

    try {
        int niter = solver.minimize(objective, x, negLogL);
        result.xi            = x[0];
        result.omega         = std::exp(x[1]);
        result.alpha         = x[2];
        result.logLikelihood = -negLogL;
        result.iterations    = niter;
        result.converged     = true;
    } catch (...) {
        // Fallback: return moment-based estimates with analytical log-likelihood.
        // Must not throw — this is called from OpenMP parallel regions.
        result.xi    = mu;
        result.omega = sigma;
        result.alpha = skew;
        result.iterations = 0;
        result.converged  = false;

        // Approximate log-likelihood treating as Gaussian (safe, no Boost needed)
        if (sigma < 1e-15) sigma = 1e-15;
        double logSigma = std::log(sigma);
        constexpr double LOG_2PI = 1.8378770664093453;
        double logL = 0.0;
        for (double val : data) {
            double z = (val - mu) / sigma;
            logL += -0.5 * (LOG_2PI + z * z) - logSigma;
        }
        result.logLikelihood = logL;
    }

    return result;
}

} // namespace nukex
