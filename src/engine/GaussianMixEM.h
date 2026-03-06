// src/engine/GaussianMixEM.h
#pragma once
#include <vector>
#include <cmath>

namespace nukex {

struct GaussianMixResult {
    double mu1, sigma1;     // component 1
    double mu2, sigma2;     // component 2
    double weight;          // mixing weight for component 1 (component 2 weight = 1-weight)
    double logLikelihood;
    bool converged;
    int iterations;
};

GaussianMixResult fitGaussianMixture2(
    const std::vector<double>& data,
    int maxIterations = 100,
    double convergenceThreshold = 1e-6);

} // namespace nukex
