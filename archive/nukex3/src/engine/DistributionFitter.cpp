#include "engine/DistributionFitter.h"
#include "engine/NumericalUtils.h"
#include <cmath>
#include <limits>

namespace nukex {

namespace {
constexpr double LOG_2PI = 1.8378770664093453;
constexpr double NEG_INF = -std::numeric_limits<double>::infinity();
} // anonymous namespace

FitResult fitGaussian(const std::vector<double>& data) {
    FitResult result{};
    result.type = DistributionType::Gaussian;
    result.k = 2;

    double mu = kahanMean(data.data(), data.size());
    double var = variance(data.data(), data.size());
    double sigma = std::sqrt(var);
    if (sigma < 1e-15) sigma = 1e-15;

    // NaN guard: corrupted input propagates NaN through mean/variance
    if (!std::isfinite(mu) || !std::isfinite(sigma)) {
        result.gaussian = {0.0, 1.0};
        result.logLikelihood = NEG_INF;  // loses AIC selection — intentional
        return result;
    }

    result.gaussian = {mu, sigma};

    // Analytical log-PDF: log N(x|mu,sigma) = -0.5*log(2*pi) - log(sigma) - 0.5*z^2
    double logSigma = std::log(sigma);
    double logL = 0.0;
    for (double x : data) {
        double z = (x - mu) / sigma;
        logL += -0.5 * (LOG_2PI + z * z) - logSigma;
    }
    result.logLikelihood = logL;
    return result;
}

FitResult fitPoisson(const std::vector<double>& data) {
    FitResult result{};
    result.type = DistributionType::Poisson;
    result.k = 1;

    double lambda = kahanMean(data.data(), data.size());
    if (lambda < 1e-10) lambda = 1e-10;

    // NaN guard: corrupted input propagates NaN through mean
    if (!std::isfinite(lambda)) {
        result.poisson = {1.0};
        result.logLikelihood = NEG_INF;  // loses AIC selection — intentional
        return result;
    }

    result.poisson = {lambda};

    // Analytical log-PMF: log P(k|lambda) = k*log(lambda) - lambda - lgamma(k+1)
    double logLambda = std::log(lambda);
    double logL = 0.0;
    for (double x : data) {
        // Clamp to uint64_t range to prevent overflow on extreme values
        double rounded = std::max(0.0, std::round(x));
        if (rounded > 1e15) rounded = 1e15;
        auto k = static_cast<uint64_t>(rounded);
        logL += k * logLambda - lambda - std::lgamma(k + 1.0);
    }
    result.logLikelihood = logL;
    return result;
}

} // namespace nukex
