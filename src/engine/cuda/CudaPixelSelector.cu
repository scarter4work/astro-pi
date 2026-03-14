// src/engine/cuda/CudaPixelSelector.cu
// CUDA pixel selection kernel — self-contained GPU reimplementation of the
// full selectBestZ() pipeline: MAD sigma-clip, Generalized ESD,
// Gaussian/Poisson/Skew-Normal/Bimodal fitting, AIC model selection,
// median pixel value output.
//
// All device functions are pure CUDA — no Boost, Eigen, or LBFGSpp.
// Copyright (c) 2026 Scott Carter

#include "cuda/CudaPixelSelector.h"

#include <cuda_runtime.h>
#include <cstdio>
#include <cmath>

namespace nukex {
namespace cuda {

// ============================================================================
// Constants
// ============================================================================

static constexpr int MAX_SUBS = 64;

__device__ constexpr double LOG_2PI       = 1.8378770664093453;
__device__ constexpr double LOG_2         = 0.6931471805599453;
__device__ constexpr double INV_SQRT2     = 0.7071067811865476;
__device__ constexpr double INV_SQRT_2PI  = 0.3989422804014327;
__device__ constexpr double MAD_TO_SIGMA  = 1.4826;
__device__ constexpr double SIGMA_FLOOR   = 1e-10;
__device__ constexpr double WEIGHT_MIN    = 0.01;
__device__ constexpr double WEIGHT_MAX    = 0.99;

// Distribution type enum values (matches CPU DistributionType)
__device__ constexpr uint8_t DIST_GAUSSIAN    = 0;
__device__ constexpr uint8_t DIST_POISSON     = 1;
__device__ constexpr uint8_t DIST_SKEW_NORMAL = 2;
__device__ constexpr uint8_t DIST_BIMODAL     = 3;

// ============================================================================
// Basic math helpers
// ============================================================================

__device__ void insertionSort(double* arr, int n) {
    for (int i = 1; i < n; ++i) {
        double key = arr[i];
        int j = i - 1;
        while (j >= 0 && arr[j] > key) {
            arr[j + 1] = arr[j];
            --j;
        }
        arr[j + 1] = key;
    }
}

__device__ double medianDevice(const double* sorted, int n) {
    if (n == 0) return 0.0;
    if (n % 2 == 0)
        return (sorted[n / 2 - 1] + sorted[n / 2]) * 0.5;
    return sorted[n / 2];
}

__device__ double kahanSumDevice(const double* data, int n) {
    double sum = 0.0;
    double c = 0.0;
    for (int i = 0; i < n; ++i) {
        double y = data[i] - c;
        double t = sum + y;
        c = (t - sum) - y;
        sum = t;
    }
    return sum;
}

__device__ double kahanMeanDevice(const double* data, int n) {
    if (n == 0) return 0.0;
    return kahanSumDevice(data, n) / static_cast<double>(n);
}

__device__ double varianceDevice(const double* data, int n) {
    if (n < 2) return 0.0;
    double mean = kahanMeanDevice(data, n);
    double sum = 0.0;
    double c = 0.0;
    for (int i = 0; i < n; ++i) {
        double diff = data[i] - mean;
        double val = diff * diff;
        double y = val - c;
        double t = sum + y;
        c = (t - sum) - y;
        sum = t;
    }
    return sum / static_cast<double>(n);
}

__device__ double stddevDevice(const double* data, int n) {
    return sqrt(varianceDevice(data, n));
}

// Sample stddev (Bessel-corrected, divide by n-1) — needed for Grubbs' test
__device__ double sampleStddevDevice(const double* data, int n) {
    if (n < 2) return 0.0;
    double mean = kahanMeanDevice(data, n);
    double sum = 0.0;
    double c = 0.0;
    for (int i = 0; i < n; ++i) {
        double diff = data[i] - mean;
        double val = diff * diff;
        double y = val - c;
        double t = sum + y;
        c = (t - sum) - y;
        sum = t;
    }
    return sqrt(sum / static_cast<double>(n - 1));
}

__device__ double skewnessDevice(const double* data, int n) {
    if (n < 3) return 0.0;
    double mean = kahanMeanDevice(data, n);
    double sd = stddevDevice(data, n);
    if (sd < 1e-15) return 0.0;

    double m3 = 0.0;
    double c = 0.0;
    for (int i = 0; i < n; ++i) {
        double diff = data[i] - mean;
        double val = diff * diff * diff;
        double y = val - c;
        double t = m3 + y;
        c = (t - m3) - y;
        m3 = t;
    }
    m3 /= static_cast<double>(n);
    double sd3 = sd * sd * sd;
    return m3 / sd3;
}

__device__ double logSumExpDevice(double a, double b) {
    double mx = fmax(a, b);
    return mx + log(exp(a - mx) + exp(b - mx));
}

// Clamp helpers
__device__ double clampWeightDevice(double w) {
    return fmax(WEIGHT_MIN, fmin(WEIGHT_MAX, w));
}

__device__ double floorSigmaDevice(double s) {
    return fmax(SIGMA_FLOOR, s);
}

// ============================================================================
// Numerically stable log(Phi(t)) — standard normal CDF in log-space
// ============================================================================

__device__ double logNormalCDFDevice(double t) {
    return log(erfc(-t * INV_SQRT2) * 0.5);
}

// ============================================================================
// Inverse normal CDF (probit) — Beasley-Springer-Moro approximation
// Input: p in (0, 1), returns z such that Phi(z) = p
// ============================================================================

__device__ double inverseCDFNormalDevice(double p) {
    // Peter Acklam's algorithm — accurate to ~1e-9
    // Rational approximation for central region
    constexpr double a1 = -3.969683028665376e+01;
    constexpr double a2 =  2.209460984245205e+02;
    constexpr double a3 = -2.759285104469687e+02;
    constexpr double a4 =  1.383577518672690e+02;
    constexpr double a5 = -3.066479806614716e+01;
    constexpr double a6 =  2.506628277459239e+00;

    constexpr double b1 = -5.447609879822406e+01;
    constexpr double b2 =  1.615858368580409e+02;
    constexpr double b3 = -1.556989798598866e+02;
    constexpr double b4 =  6.680131188771972e+01;
    constexpr double b5 = -1.328068155288572e+01;

    constexpr double c1 = -7.784894002430293e-03;
    constexpr double c2 = -3.223964580411365e-01;
    constexpr double c3 = -2.400758277161838e+00;
    constexpr double c4 = -2.549732539343734e+00;
    constexpr double c5 =  4.374664141464968e+00;
    constexpr double c6 =  2.938163982698783e+00;

    constexpr double d1 =  7.784695709041462e-03;
    constexpr double d2 =  3.224671290700398e-01;
    constexpr double d3 =  2.445134137142996e+00;
    constexpr double d4 =  3.754408661907416e+00;

    constexpr double p_low  = 0.02425;
    constexpr double p_high = 1.0 - p_low;

    double q, r;

    if (p < p_low) {
        // Rational approximation for lower region
        q = sqrt(-2.0 * log(p));
        return (((((c1*q + c2)*q + c3)*q + c4)*q + c5)*q + c6) /
               ((((d1*q + d2)*q + d3)*q + d4)*q + 1.0);
    } else if (p <= p_high) {
        // Rational approximation for central region
        q = p - 0.5;
        r = q * q;
        return (((((a1*r + a2)*r + a3)*r + a4)*r + a5)*r + a6) * q /
               (((((b1*r + b2)*r + b3)*r + b4)*r + b5)*r + 1.0);
    } else {
        // Rational approximation for upper region
        q = sqrt(-2.0 * log(1.0 - p));
        return -(((((c1*q + c2)*q + c3)*q + c4)*q + c5)*q + c6) /
                ((((d1*q + d2)*q + d3)*q + d4)*q + 1.0);
    }
}

// ============================================================================
// Approximate inverse t-distribution quantile
// For df >= 1, using Cornish-Fisher expansion from the normal quantile
// ============================================================================

__device__ double inverseStudentTDevice(double p, double df) {
    // Get normal quantile
    double z = inverseCDFNormalDevice(p);

    // For large df, normal is sufficient
    if (df > 1000.0) return z;

    // Cornish-Fisher expansion (Hill, 1970)
    // t approx z + g1(z)/df + g2(z)/df^2 + g3(z)/df^3 + ...
    double z2 = z * z;
    double z3 = z2 * z;
    double z5 = z3 * z2;
    double z7 = z5 * z2;
    double z9 = z7 * z2;

    double invDf = 1.0 / df;

    // First correction term
    double g1 = (z3 + z) * 0.25;

    // Second correction term
    double g2 = (5.0 * z5 + 16.0 * z3 + 3.0 * z) / 96.0;

    // Third correction term
    double g3 = (3.0 * z7 + 19.0 * z5 + 17.0 * z3 - 15.0 * z) / 384.0;

    // Fourth correction term
    double g4 = (79.0 * z9 + 776.0 * z7 + 1482.0 * z5 - 1920.0 * z3 - 945.0 * z) / 92160.0;

    return z + g1 * invDf + g2 * invDf * invDf
             + g3 * invDf * invDf * invDf
             + g4 * invDf * invDf * invDf * invDf;
}

// ============================================================================
// MAD-based sigma clipping
// ============================================================================

__device__ void sigmaClipMAD_device(
    const double* zValues, int nSubs, double kappa, bool* isOutlier)
{
    if (nSubs < 3) {
        for (int i = 0; i < nSubs; ++i) isOutlier[i] = false;
        return;
    }

    // Sort a copy to find median
    double sorted[MAX_SUBS];
    for (int i = 0; i < nSubs; ++i) sorted[i] = zValues[i];
    insertionSort(sorted, nSubs);
    double median = medianDevice(sorted, nSubs);

    // Compute absolute deviations and sort for MAD
    double deviations[MAX_SUBS];
    for (int i = 0; i < nSubs; ++i)
        deviations[i] = fabs(zValues[i] - median);
    insertionSort(deviations, nSubs);
    double mad = medianDevice(deviations, nSubs);

    double threshold = kappa * MAD_TO_SIGMA * mad;

    if (threshold < 1e-15) {
        // MAD is zero — bulk data at single value. Use range-based fallback.
        double range = sorted[nSubs - 1] - sorted[0];
        if (range < 1e-15) {
            for (int i = 0; i < nSubs; ++i) isOutlier[i] = false;
            return;
        }
        threshold = range * 0.1;
    }

    for (int i = 0; i < nSubs; ++i)
        isOutlier[i] = (fabs(zValues[i] - median) > threshold);
}

// ============================================================================
// Generalized ESD (iterative Grubbs' test)
// ============================================================================

__device__ double grubbsCriticalDevice(int n, double alpha) {
    double nd = static_cast<double>(n);
    double df = nd - 2.0;
    if (df < 1.0) return 0.0;

    double p = 1.0 - alpha / (2.0 * nd);
    double t = inverseStudentTDevice(p, df);

    double t2 = t * t;
    double g_crit = ((nd - 1.0) / sqrt(nd)) * sqrt(t2 / (nd - 2.0 + t2));
    return g_crit;
}

__device__ void detectOutliersESD_device(
    const double* data, int n, int maxOutliers, double alpha, bool* isOutlier)
{
    // Initialize all as non-outlier
    for (int i = 0; i < n; ++i) isOutlier[i] = false;

    if (n < 3 || maxOutliers <= 0) return;

    int maxK = maxOutliers;
    if (maxK > n - 2) maxK = n - 2;

    // Working copy with index mapping
    double working[MAX_SUBS];
    int indexMap[MAX_SUBS];
    int curN = n;
    for (int i = 0; i < n; ++i) {
        working[i] = data[i];
        indexMap[i] = i;
    }

    double testStats[MAX_SUBS];
    double critVals[MAX_SUBS];
    int removedOrigIdx[MAX_SUBS];
    int nTested = 0;

    for (int iter = 0; iter < maxK; ++iter) {
        if (curN < 3) break;

        double mean = kahanMeanDevice(working, curN);
        double sd = sampleStddevDevice(working, curN);

        if (sd <= 0.0) {
            testStats[iter] = 0.0;
            critVals[iter] = 1.0;
            nTested = iter + 1;
            break;
        }

        // Find most extreme value
        double maxDev = 0.0;
        int extremeIdx = 0;
        for (int i = 0; i < curN; ++i) {
            double dev = fabs(working[i] - mean);
            if (dev > maxDev) {
                maxDev = dev;
                extremeIdx = i;
            }
        }

        testStats[iter] = maxDev / sd;
        critVals[iter] = grubbsCriticalDevice(curN, alpha);
        removedOrigIdx[iter] = indexMap[extremeIdx];

        // Remove from working set (shift down)
        for (int i = extremeIdx; i < curN - 1; ++i) {
            working[i] = working[i + 1];
            indexMap[i] = indexMap[i + 1];
        }
        --curN;
        nTested = iter + 1;
    }

    // Find largest i such that testStats[i] > critVals[i]
    int lastSignificant = -1;
    for (int i = 0; i < nTested; ++i) {
        if (testStats[i] > critVals[i]) {
            lastSignificant = i;
        } else {
            break; // ESD stops at first non-significant
        }
    }

    // Mark outliers
    for (int i = 0; i <= lastSignificant; ++i) {
        isOutlier[removedOrigIdx[i]] = true;
    }
}

// ============================================================================
// Distribution fitting — Gaussian MLE
// ============================================================================

struct FitResultDevice {
    double logLikelihood;
    int k;
    // Fitted parameters (only meaningful for the corresponding fit function)
    double mu;         // Gaussian mean / Poisson lambda
    double sigma;      // Gaussian sigma
    // Bimodal EM
    double mu1, sigma1, mu2, sigma2, weight;
};

__device__ FitResultDevice fitGaussian_device(const double* data, int n) {
    FitResultDevice result;
    result.k = 2;

    double mu = kahanMeanDevice(data, n);
    double var = varianceDevice(data, n);
    double sigma = sqrt(var);
    if (sigma < 1e-15) sigma = 1e-15;

    if (!isfinite(mu) || !isfinite(sigma)) {
        result.logLikelihood = -1e300;
        return result;
    }

    result.mu = mu;
    result.sigma = sigma;

    double logSigma = log(sigma);
    double logL = 0.0;
    for (int i = 0; i < n; ++i) {
        double z = (data[i] - mu) / sigma;
        logL += -0.5 * (LOG_2PI + z * z) - logSigma;
    }
    result.logLikelihood = logL;
    return result;
}

// ============================================================================
// Distribution fitting — Poisson MLE
// ============================================================================

__device__ FitResultDevice fitPoisson_device(const double* data, int n) {
    FitResultDevice result;
    result.k = 1;

    double lambda = kahanMeanDevice(data, n);
    if (lambda < 1e-10) lambda = 1e-10;

    if (!isfinite(lambda)) {
        result.logLikelihood = -1e300;
        return result;
    }

    result.mu = lambda;  // Poisson mean = lambda

    double logLambda = log(lambda);
    double logL = 0.0;
    for (int i = 0; i < n; ++i) {
        double rounded = fmax(0.0, round(data[i]));
        if (rounded > 1e15) rounded = 1e15;
        double k = rounded;
        logL += k * logLambda - lambda - lgamma(k + 1.0);
    }
    result.logLikelihood = logL;
    return result;
}

// ============================================================================
// Distribution fitting — Skew-Normal MLE (self-contained L-BFGS)
// ============================================================================

// Minimal L-BFGS for 3 parameters with history m=5
// Minimizes negative log-likelihood of skew-normal distribution

__device__ FitResultDevice fitSkewNormal_device(const double* data, int n) {
    FitResultDevice result;
    result.k = 3;

    // Initial estimates from moments
    double mu = kahanMeanDevice(data, n);
    double sigma = stddevDevice(data, n);
    double skew = skewnessDevice(data, n);
    if (sigma < 1e-10) sigma = 1e-10;

    // Parameters: x[0]=xi, x[1]=log(omega), x[2]=alpha
    double x[3] = { mu, log(sigma), skew };

    // L-BFGS storage: history m=5, dimension d=3
    constexpr int M = 5;   // history size
    constexpr int D = 3;   // number of parameters
    constexpr int MAX_ITER = 50;
    constexpr double EPSILON = 1e-6;

    double s_hist[M][D];   // s_k = x_{k+1} - x_k
    double y_hist[M][D];   // y_k = g_{k+1} - g_k
    double rho[M];         // 1 / (y_k . s_k)
    double alphaLBFGS[M];  // workspace for two-loop recursion
    int histLen = 0;
    int histStart = 0;

    // Evaluate objective and gradient
    auto evalFG = [&](const double* params, double* grad) -> double {
        double xi    = params[0];
        double omega = exp(params[1]);
        if (omega < 1e-15) omega = 1e-15;
        double al    = params[2];

        double logOmega = log(omega);
        double invOmega = 1.0 / omega;
        double perSampleConst = LOG_2 - logOmega - 0.5 * LOG_2PI;

        double negLogL = 0.0;
        double g0 = 0.0, g1 = 0.0, g2 = 0.0;

        for (int i = 0; i < n; ++i) {
            double z = (data[i] - xi) * invOmega;
            double az = al * z;

            double logPhi = logNormalCDFDevice(az);
            double phiAz = INV_SQRT_2PI * exp(-0.5 * az * az);
            double PhiAz = exp(logPhi);
            double ratio = (PhiAz > 1e-300) ? phiAz / PhiAz : 0.0;

            negLogL -= perSampleConst - 0.5 * z * z + logPhi;

            // Analytical gradients of negative log-likelihood
            g0 += -z * invOmega + al * invOmega * ratio;
            g1 += 1.0 - z * z + al * z * ratio;
            g2 += -z * ratio;
        }

        grad[0] = g0;
        grad[1] = g1;
        grad[2] = g2;
        return negLogL;
    };

    double grad[D];
    double prevGrad[D];
    double negLogL = evalFG(x, grad);

    for (int iter = 0; iter < MAX_ITER; ++iter) {
        // Check convergence: gradient norm
        double gnorm = 0.0;
        for (int i = 0; i < D; ++i) gnorm += grad[i] * grad[i];
        gnorm = sqrt(gnorm);
        if (gnorm < EPSILON) break;

        // L-BFGS two-loop recursion to compute search direction q
        double q[D];
        for (int i = 0; i < D; ++i) q[i] = grad[i];

        // First loop: newest to oldest
        for (int j = histLen - 1; j >= 0; --j) {
            int idx = (histStart + j) % M;
            double dot = 0.0;
            for (int i = 0; i < D; ++i) dot += s_hist[idx][i] * q[i];
            alphaLBFGS[idx] = rho[idx] * dot;
            for (int i = 0; i < D; ++i) q[i] -= alphaLBFGS[idx] * y_hist[idx][i];
        }

        // Initial Hessian approximation: gamma * I
        double gamma = 1.0;
        if (histLen > 0) {
            int last = (histStart + histLen - 1) % M;
            double yy = 0.0, sy = 0.0;
            for (int i = 0; i < D; ++i) {
                yy += y_hist[last][i] * y_hist[last][i];
                sy += s_hist[last][i] * y_hist[last][i];
            }
            if (yy > 1e-30) gamma = sy / yy;
        }
        for (int i = 0; i < D; ++i) q[i] *= gamma;

        // Second loop: oldest to newest
        for (int j = 0; j < histLen; ++j) {
            int idx = (histStart + j) % M;
            double dot = 0.0;
            for (int i = 0; i < D; ++i) dot += y_hist[idx][i] * q[i];
            double beta = rho[idx] * dot;
            for (int i = 0; i < D; ++i) q[i] += (alphaLBFGS[idx] - beta) * s_hist[idx][i];
        }

        // Search direction = -q (descent)
        double dir[D];
        for (int i = 0; i < D; ++i) dir[i] = -q[i];

        // Check descent direction
        double dirDotGrad = 0.0;
        for (int i = 0; i < D; ++i) dirDotGrad += dir[i] * grad[i];
        if (dirDotGrad >= 0.0) {
            // Not a descent direction — reset to steepest descent
            for (int i = 0; i < D; ++i) dir[i] = -grad[i];
            dirDotGrad = -gnorm * gnorm;
            histLen = 0;
            histStart = 0;
        }

        // Backtracking line search (Armijo condition)
        double step = 1.0;
        constexpr double C1 = 1e-4;
        constexpr double STEP_SHRINK = 0.5;
        constexpr int MAX_LS = 20;
        double xNew[D];
        double gradNew[D];
        double newNegLogL;

        bool lsFound = false;
        for (int ls = 0; ls < MAX_LS; ++ls) {
            for (int i = 0; i < D; ++i)
                xNew[i] = x[i] + step * dir[i];

            newNegLogL = evalFG(xNew, gradNew);

            if (isfinite(newNegLogL) && newNegLogL <= negLogL + C1 * step * dirDotGrad) {
                lsFound = true;
                break;
            }
            step *= STEP_SHRINK;
        }

        if (!lsFound) break; // Line search failed

        // Save previous gradient
        for (int i = 0; i < D; ++i) prevGrad[i] = grad[i];

        // Update L-BFGS history
        int newIdx;
        if (histLen < M) {
            newIdx = (histStart + histLen) % M;
            ++histLen;
        } else {
            newIdx = histStart;
            histStart = (histStart + 1) % M;
        }

        double sy = 0.0;
        for (int i = 0; i < D; ++i) {
            s_hist[newIdx][i] = xNew[i] - x[i];
            y_hist[newIdx][i] = gradNew[i] - prevGrad[i];
            sy += s_hist[newIdx][i] * y_hist[newIdx][i];
        }
        rho[newIdx] = (fabs(sy) > 1e-30) ? 1.0 / sy : 0.0;

        // Accept step
        for (int i = 0; i < D; ++i) {
            x[i] = xNew[i];
            grad[i] = gradNew[i];
        }
        negLogL = newNegLogL;
    }

    if (isfinite(negLogL)) {
        result.logLikelihood = -negLogL;
    } else {
        // Fallback: Gaussian log-likelihood
        if (sigma < 1e-15) sigma = 1e-15;
        double logSigma = log(sigma);
        double logL = 0.0;
        for (int i = 0; i < n; ++i) {
            double z = (data[i] - mu) / sigma;
            logL += -0.5 * (LOG_2PI + z * z) - logSigma;
        }
        result.logLikelihood = logL;
    }
    return result;
}

// ============================================================================
// Distribution fitting — Bimodal Gaussian mixture (EM, 2 components)
// ============================================================================

__device__ FitResultDevice fitBimodalEM_device(const double* data, int n) {
    FitResultDevice result;
    result.k = 5;

    if (n < 2) {
        result.logLikelihood = -1e300;
        return result;
    }

    // Sort for initialization
    double sorted[MAX_SUBS];
    for (int i = 0; i < n; ++i) sorted[i] = data[i];
    insertionSort(sorted, n);

    int mid = n / 2;

    // Initialize from halves
    double mu1 = kahanMeanDevice(sorted, mid);
    double mu2 = kahanMeanDevice(sorted + mid, n - mid);
    double sigma1 = floorSigmaDevice(stddevDevice(sorted, mid));
    double sigma2 = floorSigmaDevice(stddevDevice(sorted + mid, n - mid));
    double weight = 0.5;

    double oldLogL = -1e300;
    double r1[MAX_SUBS]; // responsibilities for component 1

    constexpr int MAX_EM_ITER = 100;
    constexpr double CONV_THRESH = 1e-6;

    for (int iter = 0; iter < MAX_EM_ITER; ++iter) {
        // E-Step
        double logL = 0.0;
        double logW1 = log(weight);
        double logW2 = log(1.0 - weight);

        for (int i = 0; i < n; ++i) {
            double z1 = (data[i] - mu1) / sigma1;
            double log_r1 = logW1 + (-0.5 * (LOG_2PI + z1 * z1) - log(sigma1));

            double z2 = (data[i] - mu2) / sigma2;
            double log_r2 = logW2 + (-0.5 * (LOG_2PI + z2 * z2) - log(sigma2));

            double log_total = logSumExpDevice(log_r1, log_r2);
            logL += log_total;

            r1[i] = exp(log_r1 - log_total);
            r1[i] = fmax(0.0, fmin(1.0, r1[i]));
        }

        // M-Step
        double N1 = 0.0, N2 = 0.0;
        double sum_r1_x = 0.0, sum_r2_x = 0.0;

        for (int i = 0; i < n; ++i) {
            double r2_i = 1.0 - r1[i];
            N1 += r1[i];
            N2 += r2_i;
            sum_r1_x += r1[i] * data[i];
            sum_r2_x += r2_i * data[i];
        }

        if (N1 < SIGMA_FLOOR) N1 = SIGMA_FLOOR;
        if (N2 < SIGMA_FLOOR) N2 = SIGMA_FLOOR;

        mu1 = sum_r1_x / N1;
        mu2 = sum_r2_x / N2;

        double var1 = 0.0, var2 = 0.0;
        for (int i = 0; i < n; ++i) {
            double diff1 = data[i] - mu1;
            double diff2 = data[i] - mu2;
            double r2_i = 1.0 - r1[i];
            var1 += r1[i] * diff1 * diff1;
            var2 += r2_i * diff2 * diff2;
        }

        sigma1 = floorSigmaDevice(sqrt(var1 / N1));
        sigma2 = floorSigmaDevice(sqrt(var2 / N2));
        weight = clampWeightDevice(N1 / static_cast<double>(n));

        // Check convergence
        double absDiff = fabs(logL - oldLogL);
        double relDiff = (fabs(logL) > 1.0) ? absDiff / fabs(logL) : absDiff;
        if (relDiff < CONV_THRESH) {
            result.mu1 = mu1; result.sigma1 = sigma1;
            result.mu2 = mu2; result.sigma2 = sigma2;
            result.weight = weight;
            if (weight < 0.05 || weight > 0.95)
                result.logLikelihood = -1e300;
            else
                result.logLikelihood = logL;
            return result;
        }

        oldLogL = logL;
    }

    result.mu1 = mu1; result.sigma1 = sigma1;
    result.mu2 = mu2; result.sigma2 = sigma2;
    result.weight = weight;
    // Reject if one component dominates — not genuinely bimodal
    if (weight < 0.05 || weight > 0.95)
        result.logLikelihood = -1e300;
    else
        result.logLikelihood = oldLogL;
    return result;
}

// ============================================================================
// AIC
// ============================================================================

__device__ double aicDevice(double logL, int k) {
    return 2.0 * k - 2.0 * logL;
}

__device__ double aiccDevice(double logL, int k, int n) {
    double a = 2.0 * k - 2.0 * logL;
    if (n <= k + 1) return a;
    return a + (2.0 * k * (k + 1.0)) / (n - k - 1.0);
}

// ============================================================================
// Pixel selection kernel
// ============================================================================

__global__ void pixelSelectionKernel(
    const float* __restrict__ cubeData,
    float* __restrict__ outputPixels,
    uint8_t* __restrict__ distTypes,
    int nSubs,
    int height,
    int width,
    int maxOutliers,
    double outlierAlpha,
    bool adaptiveModels)
{
    int pixelIdx = blockIdx.x * blockDim.x + threadIdx.x;
    int totalPixels = height * width;
    if (pixelIdx >= totalPixels) return;

    int y = pixelIdx / width;
    int x = pixelIdx % width;

    // Z-column start in column-major (nSubs, height, width) layout:
    // element (z, y, x) is at index z + y*nSubs + x*nSubs*height
    const float* zColStart = cubeData + y * nSubs + x * nSubs * height;

    // 1. Promote to double
    double zValues[MAX_SUBS];
    for (int i = 0; i < nSubs; ++i)
        zValues[i] = static_cast<double>(zColStart[i]);

    // 2a. MAD sigma-clip pre-filter
    bool madOutlier[MAX_SUBS];
    sigmaClipMAD_device(zValues, nSubs, 3.0, madOutlier);

    // Build pre-filtered data
    double preFiltered[MAX_SUBS];
    int preFilteredIdx[MAX_SUBS];
    int nPreFiltered = 0;
    for (int i = 0; i < nSubs; ++i) {
        if (!madOutlier[i]) {
            preFiltered[nPreFiltered] = zValues[i];
            preFilteredIdx[nPreFiltered] = i;
            ++nPreFiltered;
        }
    }

    // 2b. ESD on pre-filtered data
    bool esdOutlier[MAX_SUBS]; // relative to preFiltered array
    bool allOutlier[MAX_SUBS]; // relative to original indices
    for (int i = 0; i < nSubs; ++i)
        allOutlier[i] = madOutlier[i];

    if (nPreFiltered >= 3) {
        detectOutliersESD_device(preFiltered, nPreFiltered, maxOutliers, outlierAlpha, esdOutlier);
        for (int i = 0; i < nPreFiltered; ++i) {
            if (esdOutlier[i])
                allOutlier[preFilteredIdx[i]] = true;
        }
    }

    // 3. Build clean data
    double cleanData[MAX_SUBS];
    int nClean = 0;
    for (int i = 0; i < nSubs; ++i) {
        if (!allOutlier[i]) {
            cleanData[nClean] = zValues[i];
            ++nClean;
        }
    }

    // 4. Relaxation: if too few clean points, use pre-filtered or all data
    if (nClean < 3) {
        if (nPreFiltered >= 3) {
            nClean = nPreFiltered;
            for (int i = 0; i < nPreFiltered; ++i)
                cleanData[i] = preFiltered[i];
        } else {
            nClean = nSubs;
            for (int i = 0; i < nSubs; ++i)
                cleanData[i] = zValues[i];
        }
    }

    // Compute median of clean data
    double sortedClean[MAX_SUBS];
    for (int i = 0; i < nClean; ++i) sortedClean[i] = cleanData[i];
    insertionSort(sortedClean, nClean);
    double cleanMedian = medianDevice(sortedClean, nClean);

    // Default outputs
    uint8_t bestType = DIST_GAUSSIAN;

    if (nClean >= 3) {
        // 5. Fit models — use AICc (corrected AIC) for small sample sizes
        FitResultDevice gaussFit = fitGaussian_device(cleanData, nClean);
        double aicGauss = aiccDevice(gaussFit.logLikelihood, gaussFit.k, nClean);

        FitResultDevice poisFit = fitPoisson_device(cleanData, nClean);
        double aicPois = aiccDevice(poisFit.logLikelihood, poisFit.k, nClean);

        double aicSkew = 1e300;
        double aicMix  = 1e300;
        FitResultDevice mixFit;
        mixFit.logLikelihood = -1e300;

        bool skipExpensive = false;
        if (adaptiveModels) {
            double bestSimpleAIC = fmin(aicGauss, aicPois);
            double nd = static_cast<double>(nClean);
            skipExpensive = (nClean < 6) || (bestSimpleAIC / nd < 2.0);
        }

        if (!skipExpensive) {
            FitResultDevice skewFit = fitSkewNormal_device(cleanData, nClean);
            aicSkew = aiccDevice(skewFit.logLikelihood, skewFit.k, nClean);

            mixFit = fitBimodalEM_device(cleanData, nClean);
            aicMix = aiccDevice(mixFit.logLikelihood, mixFit.k, nClean);
        }

        // 6. Select model with lowest AIC
        double bestAIC = aicGauss;
        bestType = DIST_GAUSSIAN;

        if (aicPois < bestAIC) {
            bestAIC = aicPois;
            bestType = DIST_POISSON;
        }
        if (aicSkew < bestAIC) {
            bestAIC = aicSkew;
            bestType = DIST_SKEW_NORMAL;
        }
        if (aicMix < bestAIC) {
            bestType = DIST_BIMODAL;
        }

        // 7. Distribution-aware central tendency selection
        double selectedValue = cleanMedian;
        if (bestType == DIST_GAUSSIAN || bestType == DIST_POISSON) {
            // Weighted mean — optimal for symmetric distributions
            // GPU path uses equal weights (quality weights not passed to kernel)
            double sum = 0.0;
            for (int i = 0; i < nClean; ++i)
                sum += cleanData[i];
            selectedValue = sum / nClean;
        } else if (bestType == DIST_SKEW_NORMAL) {
            // Median — robust to tail pull
            selectedValue = cleanMedian;
        } else if (bestType == DIST_BIMODAL) {
            // Weighted mean of dominant component
            double domMu = (mixFit.weight >= 0.5) ? mixFit.mu1 : mixFit.mu2;
            double domSig = (mixFit.weight >= 0.5) ? mixFit.sigma1 : mixFit.sigma2;
            double cutoff = 2.0 * fmax(domSig, 1e-10);
            double sumV = 0.0;
            int countV = 0;
            for (int i = 0; i < nClean; ++i) {
                if (fabs(cleanData[i] - domMu) <= cutoff) {
                    sumV += cleanData[i];
                    ++countV;
                }
            }
            if (countV > 0)
                selectedValue = sumV / countV;
        }
        cleanMedian = selectedValue;  // reuse variable for output
    }

    // 8. Output
    outputPixels[pixelIdx] = static_cast<float>(cleanMedian);
    distTypes[pixelIdx] = bestType;
}

// ============================================================================
// Host function: processImageGPU
// ============================================================================

// Helper macro for CUDA error checking
#define CUDA_CHECK(call, errResult)                                           \
    do {                                                                      \
        cudaError_t err = (call);                                             \
        if (err != cudaSuccess) {                                             \
            char buf[512];                                                    \
            snprintf(buf, sizeof(buf), "%s:%d: %s: %s",                       \
                     __FILE__, __LINE__, #call, cudaGetErrorString(err));      \
            (errResult).success = false;                                      \
            (errResult).errorMessage = buf;                                   \
            return errResult;                                                 \
        }                                                                     \
    } while (0)

GpuStackResult processImageGPU(
    const float* cubeData,
    float* outputPixels,
    uint8_t* distTypes,
    const GpuStackConfig& config)
{
    GpuStackResult result;
    result.success = false;

    size_t nSubs  = config.nSubs;
    size_t H      = config.height;
    size_t W      = config.width;
    size_t cubeSize   = nSubs * H * W;
    size_t pixelCount = H * W;

    if (nSubs > MAX_SUBS) {
        result.errorMessage = "nSubs exceeds MAX_SUBS (64)";
        return result;
    }

    // Allocate device memory
    float*    d_cube     = nullptr;
    float*    d_output   = nullptr;
    uint8_t*  d_distType = nullptr;

    CUDA_CHECK(cudaMalloc(&d_cube,     cubeSize   * sizeof(float)),   result);
    CUDA_CHECK(cudaMalloc(&d_output,   pixelCount * sizeof(float)),   result);
    CUDA_CHECK(cudaMalloc(&d_distType, pixelCount * sizeof(uint8_t)), result);

    // Copy cube data to device
    CUDA_CHECK(cudaMemcpy(d_cube, cubeData, cubeSize * sizeof(float),
                          cudaMemcpyHostToDevice), result);

    // Launch kernel
    constexpr int BLOCK_SIZE = 256;
    int gridSize = static_cast<int>((pixelCount + BLOCK_SIZE - 1) / BLOCK_SIZE);

    pixelSelectionKernel<<<gridSize, BLOCK_SIZE>>>(
        d_cube, d_output, d_distType,
        static_cast<int>(nSubs),
        static_cast<int>(H),
        static_cast<int>(W),
        config.maxOutliers,
        config.outlierAlpha,
        config.adaptiveModels);

    // Check for launch errors
    CUDA_CHECK(cudaGetLastError(), result);

    // Wait for kernel completion
    CUDA_CHECK(cudaDeviceSynchronize(), result);

    // Copy results back
    CUDA_CHECK(cudaMemcpy(outputPixels, d_output,
                          pixelCount * sizeof(float),
                          cudaMemcpyDeviceToHost), result);
    CUDA_CHECK(cudaMemcpy(distTypes, d_distType,
                          pixelCount * sizeof(uint8_t),
                          cudaMemcpyDeviceToHost), result);

    // Clean up
    cudaFree(d_cube);
    cudaFree(d_output);
    cudaFree(d_distType);

    result.success = true;
    return result;
}

#undef CUDA_CHECK

} // namespace cuda
} // namespace nukex
