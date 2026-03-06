#include "engine/DistributionFitter.h"
#include "engine/NumericalUtils.h"
#include <boost/math/distributions/normal.hpp>
#include <boost/math/distributions/poisson.hpp>

namespace nukex {

FitResult fitGaussian(const std::vector<double>& data) {
    FitResult result{};
    result.type = DistributionType::Gaussian;
    result.k = 2;

    double mu = kahanMean(data.data(), data.size());
    double var = variance(data.data(), data.size());
    double sigma = std::sqrt(var);
    if (sigma < 1e-15) sigma = 1e-15;  // floor

    result.gaussian = {mu, sigma};

    boost::math::normal_distribution<double> dist(mu, sigma);
    double logL = 0.0;
    for (double x : data) {
        double p = boost::math::pdf(dist, x);
        if (p < 1e-300) p = 1e-300;
        logL += std::log(p);
    }
    result.logLikelihood = logL;
    return result;
}

FitResult fitPoisson(const std::vector<double>& data) {
    FitResult result{};
    result.type = DistributionType::Poisson;
    result.k = 1;

    double lambda = kahanMean(data.data(), data.size());
    if (lambda < 1e-10) lambda = 1e-10;  // floor

    result.poisson = {lambda};

    boost::math::poisson_distribution<double> dist(lambda);
    double logL = 0.0;
    for (double x : data) {
        unsigned k = static_cast<unsigned>(std::max(0.0, std::round(x)));
        double p = boost::math::pdf(dist, k);
        if (p < 1e-300) p = 1e-300;
        logL += std::log(p);
    }
    result.logLikelihood = logL;
    return result;
}

} // namespace nukex
