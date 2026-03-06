#include "engine/OutlierDetector.h"
#include "engine/NumericalUtils.h"

#include <boost/math/distributions/students_t.hpp>
#include <boost/math/distributions/normal.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace nukex {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

// Sample mean (Kahan-compensated, reusing NumericalUtils)
inline double sampleMean(const double* data, size_t n) {
    return kahanMean(data, n);
}

// Sample standard deviation (divide by n-1, required for Grubbs' test)
// NumericalUtils::stddev uses population stddev (divide by n), so we
// compute sample stddev here for correct statistical test behaviour.
inline double sampleStddev(const double* data, size_t n) {
    if (n < 2) return 0.0;
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
    return std::sqrt(sum / static_cast<double>(n - 1));
}

// Grubbs' critical value for a sample of size n at significance level alpha
// G_crit = ((n-1) / sqrt(n)) * sqrt(t^2 / (n - 2 + t^2))
// where t = quantile of t-distribution at alpha/(2*n) with n-2 degrees of freedom
inline double grubbsCritical(size_t n, double alpha) {
    double nd = static_cast<double>(n);
    double df = nd - 2.0;
    if (df < 1.0) return 0.0;

    double p = alpha / (2.0 * nd);
    boost::math::students_t_distribution<double> tdist(df);
    double t = boost::math::quantile(boost::math::complement(tdist, p));

    double t2 = t * t;
    double g_crit = ((nd - 1.0) / std::sqrt(nd)) * std::sqrt(t2 / (nd - 2.0 + t2));
    return g_crit;
}

// Find the index of the most extreme value (max |x_i - mean|)
// Returns the index and the Grubbs statistic G
struct GrubbsResult {
    size_t index;
    double G;
};

inline GrubbsResult findMostExtreme(const double* data, size_t n) {
    double mean = sampleMean(data, n);
    double sd = sampleStddev(data, n);

    GrubbsResult result{SIZE_MAX, 0.0};
    if (sd <= 0.0) return result;

    double maxDev = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double dev = std::abs(data[i] - mean);
        if (dev > maxDev) {
            maxDev = dev;
            result.index = i;
        }
    }
    result.G = maxDev / sd;
    return result;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// grubbsTest
// ---------------------------------------------------------------------------

size_t grubbsTest(const std::vector<double>& data, double alpha) {
    size_t n = data.size();
    if (n < 3) return SIZE_MAX;

    auto result = findMostExtreme(data.data(), n);
    if (result.index == SIZE_MAX) return SIZE_MAX;

    double g_crit = grubbsCritical(n, alpha);
    if (result.G > g_crit) {
        return result.index;
    }
    return SIZE_MAX;
}

// ---------------------------------------------------------------------------
// detectOutliersESD (Generalized Extreme Studentized Deviate)
// ---------------------------------------------------------------------------

std::vector<size_t> detectOutliersESD(
    const std::vector<double>& data,
    int maxOutliers,
    double alpha)
{
    size_t n = data.size();
    if (n < 3 || maxOutliers <= 0) return {};

    // Cap maxOutliers at n-2 (need at least 3 points for each iteration,
    // but we relax to n-2 to allow testing down to 2 remaining points
    // since the last iteration may still detect with the prior critical value)
    size_t maxK = std::min(static_cast<size_t>(maxOutliers), n - 2);

    // Build working copy and index map to track original indices
    std::vector<double> working(data.begin(), data.end());
    std::vector<size_t> indexMap(n);
    std::iota(indexMap.begin(), indexMap.end(), 0);

    // Store test statistics and critical values for each iteration
    std::vector<double> testStats(maxK);
    std::vector<double> critVals(maxK);
    std::vector<size_t> removedOriginalIndices(maxK);

    for (size_t i = 0; i < maxK; ++i) {
        size_t curN = working.size();
        if (curN < 3) break;

        auto result = findMostExtreme(working.data(), curN);
        if (result.index == SIZE_MAX) {
            // Zero stddev, no more outliers
            testStats[i] = 0.0;
            critVals[i] = 1.0; // ensure test fails
            break;
        }

        testStats[i] = result.G;
        critVals[i] = grubbsCritical(curN, alpha);
        removedOriginalIndices[i] = indexMap[result.index];

        // Remove the most extreme value from working set and index map
        working.erase(working.begin() + static_cast<std::ptrdiff_t>(result.index));
        indexMap.erase(indexMap.begin() + static_cast<std::ptrdiff_t>(result.index));
    }

    // Find the largest i such that testStats[i] > critVals[i]
    // All points 0..i are outliers
    int lastSignificant = -1;
    for (size_t i = 0; i < maxK; ++i) {
        if (testStats[i] > critVals[i]) {
            lastSignificant = static_cast<int>(i);
        } else {
            break; // ESD stops at first non-significant
        }
    }

    std::vector<size_t> outliers;
    if (lastSignificant >= 0) {
        outliers.reserve(static_cast<size_t>(lastSignificant + 1));
        for (int i = 0; i <= lastSignificant; ++i) {
            outliers.push_back(removedOriginalIndices[static_cast<size_t>(i)]);
        }
    }
    return outliers;
}

// ---------------------------------------------------------------------------
// detectOutliersChauvenet
// ---------------------------------------------------------------------------

std::vector<size_t> detectOutliersChauvenet(const std::vector<double>& data) {
    size_t n = data.size();
    if (n < 3) return {};

    double mean = kahanMean(data.data(), n);
    double sd = stddev(data.data(), n);
    if (sd <= 0.0) return {};

    boost::math::normal_distribution<double> norm(0.0, 1.0);
    double nd = static_cast<double>(n);

    std::vector<size_t> outliers;
    for (size_t i = 0; i < n; ++i) {
        double z = std::abs(data[i] - mean) / sd;
        // P(|X - mean| >= |x_i - mean|) = 2 * (1 - cdf(normal, |z|))
        double p = 2.0 * (1.0 - boost::math::cdf(norm, z));
        if (p * nd < 0.5) {
            outliers.push_back(i);
        }
    }
    return outliers;
}

} // namespace nukex
