#pragma once
#include <cmath>
#include <vector>
#include <algorithm>
#include <numeric>
#include <limits>

namespace nukex {

// Log-sum-exp: compute log(sum(exp(vals))) without overflow/underflow
// Uses the max-subtraction trick for numerical stability
inline double logSumExp(const double* vals, size_t n) {
    if (n == 0)
        return -std::numeric_limits<double>::infinity();
    if (n == 1)
        return vals[0];

    double maxVal = *std::max_element(vals, vals + n);

    double sumExp = 0.0;
    for (size_t i = 0; i < n; ++i) {
        sumExp += std::exp(vals[i] - maxVal);
    }
    return maxVal + std::log(sumExp);
}

inline double logSumExp(const std::vector<double>& vals) {
    return logSumExp(vals.data(), vals.size());
}

// Information criteria for model selection
inline double aic(double logLikelihood, int k) {
    return 2.0 * k - 2.0 * logLikelihood;
}

inline double bic(double logLikelihood, int k, int n) {
    return k * std::log(static_cast<double>(n)) - 2.0 * logLikelihood;
}

inline double aicc(double logLikelihood, int k, int n) {
    double a = aic(logLikelihood, k);
    if (n <= k + 1)
        return a;
    return a + (2.0 * k * (k + 1.0)) / (n - k - 1.0);
}

// Kahan compensated summation (prevents floating-point accumulation error)
inline double kahanSum(const double* data, size_t n) {
    double sum = 0.0;
    double c = 0.0; // compensation
    for (size_t i = 0; i < n; ++i) {
        double y = data[i] - c;
        double t = sum + y;
        c = (t - sum) - y;
        sum = t;
    }
    return sum;
}

inline double kahanMean(const double* data, size_t n) {
    if (n == 0)
        return 0.0;
    return kahanSum(data, n) / static_cast<double>(n);
}

// Compensated variance (two-pass for stability)
inline double variance(const double* data, size_t n) {
    if (n < 2)
        return 0.0;
    double mean = kahanMean(data, n);
    double sum = 0.0;
    double c = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double diff = data[i] - mean;
        double val = diff * diff;
        double y = val - c;
        double t = sum + y;
        c = (t - sum) - y;
        sum = t;
    }
    return sum / static_cast<double>(n);
}

inline double stddev(const double* data, size_t n) {
    return std::sqrt(variance(data, n));
}

// Sample skewness (Fisher's definition)
inline double skewness(const double* data, size_t n) {
    if (n < 3)
        return 0.0;
    double mean = kahanMean(data, n);
    double sd = stddev(data, n);
    if (sd == 0.0)
        return 0.0;

    double m3 = 0.0;
    double c = 0.0;
    for (size_t i = 0; i < n; ++i) {
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

// Promote float array to double vector (for the Z-column -> stats pipeline)
inline std::vector<double> promoteToDouble(const float* data, size_t n) {
    std::vector<double> result(n);
    for (size_t i = 0; i < n; ++i) {
        result[i] = static_cast<double>(data[i]);
    }
    return result;
}

} // namespace nukex
