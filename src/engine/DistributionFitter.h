// src/engine/DistributionFitter.h
#pragma once
#include <vector>
#include <cmath>
#include <cstdint>

namespace nukex {

enum class DistributionType : uint8_t {
    Gaussian = 0,
    Poisson = 1,
    SkewNormal = 2,
    Bimodal = 3
};

struct GaussianParams { double mu, sigma; };
struct PoissonParams { double lambda; };
struct SkewNormalParams { double xi, omega, alpha; };
struct BimodalParams { double mu1, sigma1, mu2, sigma2, weight; };

struct FitResult {
    DistributionType type;
    double logLikelihood;
    int k;  // number of parameters
    // Use named struct members (not a union) for safety.
    // Only the fields matching `type` are meaningful.
    GaussianParams gaussian;
    PoissonParams poisson;
    SkewNormalParams skewNormal;
    BimodalParams bimodal;
};

FitResult fitGaussian(const std::vector<double>& data);
FitResult fitPoisson(const std::vector<double>& data);
// Skew-Normal fitter: see SkewNormalFitter.h (separate to isolate Eigen/LBFGSpp deps)
// Bimodal fitter: see GaussianMixEM.h (separate to isolate Boost.Math deps)

} // namespace nukex
