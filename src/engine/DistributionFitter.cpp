#include "engine/DistributionFitter.h"
#include "engine/NumericalUtils.h"
#include <cmath>

namespace nukex {

namespace {
constexpr double LOG_2PI = 1.8378770664093453;
} // anonymous namespace

FitResult fitGaussian(const std::vector<double>& data) {
    FitResult result{};
    result.type = DistributionType::Gaussian;
    result.k = 2;

    double mu = kahanMean(data.data(), data.size());
    double var = variance(data.data(), data.size());
    double sigma = std::sqrt(var);
    if (sigma < 1e-15) sigma = 1e-15;

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

    result.poisson = {lambda};

    // Analytical log-PMF: log P(k|lambda) = k*log(lambda) - lambda - lgamma(k+1)
    double logLambda = std::log(lambda);
    double logL = 0.0;
    for (double x : data) {
        unsigned k = static_cast<unsigned>(std::max(0.0, std::round(x)));
        logL += k * logLambda - lambda - std::lgamma(k + 1.0);
    }
    result.logLikelihood = logL;
    return result;
}

} // namespace nukex
