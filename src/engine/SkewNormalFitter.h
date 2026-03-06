#pragma once
#include <vector>
#include <cmath>

namespace nukex {

struct SkewNormalFitResult {
    double xi;      // location
    double omega;   // scale
    double alpha;   // shape (skewness)
    double logLikelihood;
    int k = 3;      // number of parameters
    int iterations;
    bool converged;
};

SkewNormalFitResult fitSkewNormal(const std::vector<double>& data);

} // namespace nukex
