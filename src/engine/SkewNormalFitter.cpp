#include "engine/SkewNormalFitter.h"
#include "engine/NumericalUtils.h"

#include <Eigen/Core>
#include <LBFGS.h>

#include <cmath>
#include <stdexcept>

namespace nukex {

namespace {
constexpr double LOG_2     = 0.6931471805599453;
constexpr double LOG_2PI   = 1.8378770664093453;
constexpr double SQRT_2    = 1.4142135623730951;
constexpr double INV_SQRT2 = 0.7071067811865476;

// Numerically stable log(Phi(t)) where Phi is the standard normal CDF.
// Uses log(erfc(-t/sqrt(2))/2) which handles large negative t without underflow.
inline double logNormalCDF(double t) {
    return std::log(std::erfc(-t * INV_SQRT2) * 0.5);
}
} // anonymous namespace

// Objective functor for LBFGSpp
// Minimizes negative log-likelihood of the skew-normal distribution.
// Parameters: x[0] = xi (location), x[1] = log(omega) (log-scale), x[2] = alpha (shape).
//
// Analytical log-PDF: log f(x) = log(2) - log(omega) - 0.5*log(2*pi) - 0.5*z^2 + log(Phi(alpha*z))
// where z = (x - xi) / omega, Phi = standard normal CDF.
class SkewNormalObjective {
public:
    explicit SkewNormalObjective(const std::vector<double>& data) : m_data(data) {}

    // Evaluate negative log-likelihood at given parameters
    double evalNLL(const Eigen::VectorXd& x) const {
        double xi    = x[0];
        double omega = std::exp(x[1]);  // log-transform keeps omega > 0
        double alpha = x[2];

        if (omega < 1e-15) omega = 1e-15;
        double logOmega = std::log(omega);

        // Per-sample constant: log(2) - log(omega) - 0.5*log(2*pi)
        double perSampleConst = LOG_2 - logOmega - 0.5 * LOG_2PI;

        double negLogL = 0.0;
        for (double val : m_data) {
            double z = (val - xi) / omega;
            // log f(x) = perSampleConst - 0.5*z^2 + log(Phi(alpha*z))
            negLogL -= perSampleConst - 0.5 * z * z + logNormalCDF(alpha * z);
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
    } catch (const std::bad_alloc&) {
        throw;  // Memory errors must propagate — never silently degrade
    } catch (const std::exception&) {
        // Optimization convergence failure — fall back to moment-based estimates.
        // Must not throw further — called from OpenMP parallel regions.
        result.xi    = mu;
        result.omega = sigma;
        result.alpha = skew;
        result.iterations = 0;
        result.converged  = false;

        // Gaussian log-likelihood as fallback (not skew-normal — avoids Boost in error path)
        if (sigma < 1e-15) sigma = 1e-15;
        double logSigma = std::log(sigma);
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
