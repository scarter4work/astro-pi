// src/engine/GaussianMixEM.cpp
#include "engine/GaussianMixEM.h"
#include "engine/NumericalUtils.h"

#include <boost/math/distributions/normal.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace nukex {

namespace {

constexpr double SIGMA_FLOOR = 1e-10;
constexpr double WEIGHT_MIN = 0.01;
constexpr double WEIGHT_MAX = 0.99;

double clampWeight(double w) {
    return std::max(WEIGHT_MIN, std::min(WEIGHT_MAX, w));
}

double floorSigma(double s) {
    return std::max(SIGMA_FLOOR, s);
}

// Log of normal PDF at x given mean mu and standard deviation sigma
double logNormalPdf(double x, double mu, double sigma) {
    boost::math::normal_distribution<double> dist(mu, sigma);
    double p = boost::math::pdf(dist, x);
    if (p <= 0.0) {
        return -std::numeric_limits<double>::infinity();
    }
    return std::log(p);
}

} // anonymous namespace

GaussianMixResult fitGaussianMixture2(
    const std::vector<double>& data,
    int maxIterations,
    double convergenceThreshold)
{
    const size_t n = data.size();
    GaussianMixResult result{};
    result.converged = false;
    result.iterations = 0;
    result.logLikelihood = -std::numeric_limits<double>::infinity();

    if (n < 2) {
        // Degenerate case: not enough data
        if (n == 1) {
            result.mu1 = data[0];
            result.mu2 = data[0];
            result.sigma1 = SIGMA_FLOOR;
            result.sigma2 = SIGMA_FLOOR;
            result.weight = 0.5;
            result.logLikelihood = 0.0;
            result.converged = true;
        }
        return result;
    }

    // Step 1: Initialize by sorting and splitting at median
    std::vector<double> sorted(data);
    std::sort(sorted.begin(), sorted.end());

    size_t mid = n / 2;

    // Lower half: [0, mid), Upper half: [mid, n)
    double mu1 = kahanMean(sorted.data(), mid);
    double mu2 = kahanMean(sorted.data() + mid, n - mid);

    double sigma1 = floorSigma(stddev(sorted.data(), mid));
    double sigma2 = floorSigma(stddev(sorted.data() + mid, n - mid));

    double weight = 0.5;

    double oldLogL = -std::numeric_limits<double>::infinity();
    std::vector<double> r1(n);  // responsibilities for component 1

    for (int iter = 0; iter < maxIterations; ++iter) {
        // E-Step: compute responsibilities
        for (size_t i = 0; i < n; ++i) {
            double log_r1 = std::log(weight) + logNormalPdf(data[i], mu1, sigma1);
            double log_r2 = std::log(1.0 - weight) + logNormalPdf(data[i], mu2, sigma2);

            double lse[2] = {log_r1, log_r2};
            double log_total = logSumExp(lse, 2);

            r1[i] = std::exp(log_r1 - log_total);
            // Clamp to [0, 1] for safety
            r1[i] = std::max(0.0, std::min(1.0, r1[i]));
        }

        // M-Step: update parameters using responsibilities
        double N1 = 0.0, N2 = 0.0;
        double sum_r1_x = 0.0, sum_r2_x = 0.0;

        for (size_t i = 0; i < n; ++i) {
            double r2_i = 1.0 - r1[i];
            N1 += r1[i];
            N2 += r2_i;
            sum_r1_x += r1[i] * data[i];
            sum_r2_x += r2_i * data[i];
        }

        // Avoid division by zero
        if (N1 < SIGMA_FLOOR) N1 = SIGMA_FLOOR;
        if (N2 < SIGMA_FLOOR) N2 = SIGMA_FLOOR;

        mu1 = sum_r1_x / N1;
        mu2 = sum_r2_x / N2;

        double var1 = 0.0, var2 = 0.0;
        for (size_t i = 0; i < n; ++i) {
            double diff1 = data[i] - mu1;
            double diff2 = data[i] - mu2;
            double r2_i = 1.0 - r1[i];
            var1 += r1[i] * diff1 * diff1;
            var2 += r2_i * diff2 * diff2;
        }

        sigma1 = floorSigma(std::sqrt(var1 / N1));
        sigma2 = floorSigma(std::sqrt(var2 / N2));

        weight = clampWeight(N1 / static_cast<double>(n));

        // Compute log-likelihood
        double logL = 0.0;
        for (size_t i = 0; i < n; ++i) {
            double log_p1 = std::log(weight) + logNormalPdf(data[i], mu1, sigma1);
            double log_p2 = std::log(1.0 - weight) + logNormalPdf(data[i], mu2, sigma2);
            double lse[2] = {log_p1, log_p2};
            logL += logSumExp(lse, 2);
        }

        result.iterations = iter + 1;

        // Check convergence using relative change for numerical robustness
        double absDiff = std::abs(logL - oldLogL);
        double relDiff = (std::abs(logL) > 1.0) ? absDiff / std::abs(logL) : absDiff;
        if (relDiff < convergenceThreshold) {
            result.converged = true;
            result.logLikelihood = logL;
            result.mu1 = mu1;
            result.sigma1 = sigma1;
            result.mu2 = mu2;
            result.sigma2 = sigma2;
            result.weight = weight;
            return result;
        }

        oldLogL = logL;
    }

    // Did not converge within maxIterations
    result.logLikelihood = oldLogL;
    result.mu1 = mu1;
    result.sigma1 = sigma1;
    result.mu2 = mu2;
    result.sigma2 = sigma2;
    result.weight = weight;
    return result;
}

} // namespace nukex
