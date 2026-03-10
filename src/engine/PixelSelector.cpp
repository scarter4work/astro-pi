// src/engine/PixelSelector.cpp
#include "engine/PixelSelector.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <numeric>
#include <unordered_set>


namespace nukex {

namespace {

// Compute median of a sorted array
inline double medianOfSorted(const double* sorted, size_t n) {
    if (n == 0) return 0.0;
    return (n % 2 == 0)
        ? (sorted[n / 2 - 1] + sorted[n / 2]) * 0.5
        : sorted[n / 2];
}

} // anonymous namespace

PixelSelector::PixelSelector()
    : m_config(Config{}) {}

PixelSelector::PixelSelector(const Config& config)
    : m_config(config) {}

PixelSelector::PixelResult
PixelSelector::selectBestZ(const float* zColumnPtr, size_t nSubs,
                           const std::vector<double>& qualityWeights)
{
    PixelResult result{};

    // 1. Promote float Z-column to double
    std::vector<double> zValues = promoteToDouble(zColumnPtr, nSubs);

    // 2a. MAD-based sigma clipping pre-filter (robust to outliers).
    // MAD uses the median, so outliers can't inflate the scale estimate
    // and mask their own detection (unlike mean+stddev in ESD).
    std::vector<size_t> madOutliers = sigmaClipMAD(zValues, 3.0);
    std::unordered_set<size_t> madOutlierSet(madOutliers.begin(),
                                              madOutliers.end());

    // Build pre-filtered data for ESD
    std::vector<double> preFiltered;
    std::vector<size_t> preFilteredIndices;
    preFiltered.reserve(nSubs);
    preFilteredIndices.reserve(nSubs);
    for (size_t i = 0; i < nSubs; ++i) {
        if (madOutlierSet.find(i) == madOutlierSet.end()) {
            preFiltered.push_back(zValues[i]);
            preFilteredIndices.push_back(i);
        }
    }

    // 2b. ESD on pre-filtered data (catches subtler outliers in the clean set)
    std::unordered_set<size_t> allOutliers(madOutliers.begin(),
                                            madOutliers.end());
    if (preFiltered.size() >= 3) {
        std::vector<size_t> esdLocal = detectOutliersESD(
            preFiltered, m_config.maxOutliers, m_config.outlierAlpha);
        for (size_t localIdx : esdLocal)
            allOutliers.insert(preFilteredIndices[localIdx]);
    }

    // 3. Build clean data vector (exclude all outliers)
    std::vector<double> cleanData;
    std::vector<size_t> cleanIndices;
    cleanData.reserve(nSubs);
    cleanIndices.reserve(nSubs);
    for (size_t i = 0; i < nSubs; ++i) {
        if (allOutliers.find(i) == allOutliers.end()) {
            cleanData.push_back(zValues[i]);
            cleanIndices.push_back(i);
        }
    }

    // 4. If too few clean points, relax to pre-filtered or all data
    if (cleanData.size() < 3) {
        if (preFiltered.size() >= 3) {
            cleanData = preFiltered;
            cleanIndices = preFilteredIndices;
        } else {
            cleanData.resize(nSubs);
            cleanIndices.resize(nSubs);
            for (size_t i = 0; i < nSubs; ++i) {
                cleanData[i] = zValues[i];
                cleanIndices[i] = i;
            }
        }
    }

    // Compute median of clean data (robust central tendency)
    std::vector<double> sortedClean(cleanData);
    std::sort(sortedClean.begin(), sortedClean.end());
    double cleanMedian = medianOfSorted(sortedClean.data(), sortedClean.size());

    if (cleanData.size() < 3) {
        // Not enough data for distribution fitting — use median
        uint32_t bestZ = 0;
        double bestDist = std::numeric_limits<double>::infinity();
        for (size_t idx : cleanIndices) {
            double dist = std::abs(zValues[idx] - cleanMedian);
            if (dist < bestDist) {
                bestDist = dist;
                bestZ = static_cast<uint32_t>(idx);
            }
        }
        if (cleanIndices.empty()) {
            bestZ = 0;
            cleanMedian = zValues[0];
        }

        result.selectedZ = bestZ;
        result.bestModel = DistributionType::Gaussian;
        result.selectedValue = static_cast<float>(cleanMedian);
        return result;
    }

    // 5. Fit all 4 models on clean data
    FitResult gaussianFit = fitGaussian(cleanData);
    FitResult poissonFit = fitPoisson(cleanData);
    SkewNormalFitResult skewFit = fitSkewNormal(cleanData);
    GaussianMixResult mixFit = fitGaussianMixture2(cleanData);

    // 6. Compute AIC for each model
    double aicGaussian = aic(gaussianFit.logLikelihood, gaussianFit.k);
    double aicPoisson = aic(poissonFit.logLikelihood, poissonFit.k);
    double aicSkew = aic(skewFit.logLikelihood, skewFit.k);
    double aicMix = aic(mixFit.logLikelihood, mixFit.k);

    // 7. Select model with lowest AIC
    struct ModelAIC {
        DistributionType type;
        double aicValue;
    };

    ModelAIC models[] = {
        {DistributionType::Gaussian, aicGaussian},
        {DistributionType::Poisson,  aicPoisson},
        {DistributionType::SkewNormal, aicSkew},
        {DistributionType::Bimodal,  aicMix}
    };

    DistributionType bestType = DistributionType::Gaussian;
    double bestAIC = aicGaussian;
    for (const auto& m : models) {
        if (m.aicValue < bestAIC) {
            bestAIC = m.aicValue;
            bestType = m.type;
        }
    }

    // 8. Select pixel value using median of clean data.
    //    Median is inherently robust to residual outliers that survive
    //    the MAD+ESD pipeline — important for background pixels where
    //    stretch amplifies any contamination.
    uint32_t bestZ = 0;
    double bestDist = std::numeric_limits<double>::infinity();
    for (size_t idx : cleanIndices) {
        double dist = std::abs(zValues[idx] - cleanMedian);
        if (dist < bestDist) {
            bestDist = dist;
            bestZ = static_cast<uint32_t>(idx);
        }
    }

    // 9. Return result
    result.selectedZ = bestZ;
    result.bestModel = bestType;
    result.selectedValue = static_cast<float>(cleanMedian);
    return result;
}

float PixelSelector::processPixel(SubCube& cube, size_t y, size_t x,
                                   const std::vector<double>& qualityWeights)
{
    auto result = selectBestZ(cube.zColumnPtr(y, x), cube.numSubs(), qualityWeights);
    cube.setProvenance(y, x, result.selectedZ);
    cube.setDistType(y, x, static_cast<uint8_t>(result.bestModel));
    return result.selectedValue;
}

std::vector<float> PixelSelector::processImage(SubCube& cube,
                                                const std::vector<double>& qualityWeights,
                                                ProgressCallback progress)
{
    size_t H = cube.height();
    size_t W = cube.width();
    size_t N = cube.numSubs();
    std::vector<float> output(H * W);

    // 10 rows per chunk: frequent UI updates for real-time progress feedback.
    // PCL event processing is main-thread-affine, so progress callback
    // runs between parallel regions on the main thread.
    constexpr size_t CHUNK = 10;

    std::atomic<size_t> errorCount{0};

    for (size_t yStart = 0; yStart < H; yStart += CHUNK) {
        size_t yEnd = std::min(yStart + CHUNK, H);

        // Thread-safe: selectBestZ uses only stack-local allocations.
        // Writes to output[y*W+x], provenance(y,x), distType(y,x) are
        // non-overlapping across (y,x) pairs.
        #pragma omp parallel for schedule(dynamic, 4)
        for (size_t y = yStart; y < yEnd; ++y) {
            for (size_t x = 0; x < W; ++x) {
                try {
                    auto result = selectBestZ(cube.zColumnPtr(y, x), N, qualityWeights);
                    output[y * W + x] = result.selectedValue;
                    cube.setProvenance(y, x, result.selectedZ);
                    cube.setDistType(y, x, static_cast<uint8_t>(result.bestModel));
                } catch (const std::bad_alloc&) {
                    throw;  // Memory errors must propagate
                } catch (const std::exception&) {
                    // Fitting failure — fall back to median of Z-column
                    ++errorCount;
                    const float* col = cube.zColumnPtr(y, x);
                    std::vector<double> zv(N);
                    for (size_t z = 0; z < N; ++z)
                        zv[z] = static_cast<double>(col[z]);
                    std::sort(zv.begin(), zv.end());
                    output[y * W + x] = static_cast<float>(
                        medianOfSorted(zv.data(), N));
                    cube.setProvenance(y, x, 0);
                    cube.setDistType(y, x, static_cast<uint8_t>(DistributionType::Gaussian));
                }
            }
        }

        if (progress)
            progress(yEnd, H);
    }

    // Store error count for caller to retrieve
    m_lastErrorCount = errorCount.load();

    return output;
}

} // namespace nukex
