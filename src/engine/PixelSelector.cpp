// src/engine/PixelSelector.cpp
#include "engine/PixelSelector.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <numeric>
#include <unordered_set>


namespace nukex {

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

    // 2. Detect outliers using Generalized ESD
    std::vector<size_t> outlierIndices = detectOutliersESD(
        zValues, m_config.maxOutliers, m_config.outlierAlpha);

    // Build set of outlier indices for fast lookup
    std::unordered_set<size_t> outlierSet(outlierIndices.begin(),
                                          outlierIndices.end());

    // 3. Build clean data vector (exclude outlier indices)
    std::vector<double> cleanData;
    std::vector<size_t> cleanIndices;
    cleanData.reserve(nSubs);
    cleanIndices.reserve(nSubs);
    for (size_t i = 0; i < nSubs; ++i) {
        if (outlierSet.find(i) == outlierSet.end()) {
            cleanData.push_back(zValues[i]);
            cleanIndices.push_back(i);
        }
    }

    // 4. If clean data has fewer than 3 points, fall back to quality-weighted mean
    if (cleanData.size() < 3) {
        // Compute quality-weighted mean of all non-outlier Z values
        double weightedSum = 0.0;
        double weightSum = 0.0;
        for (size_t idx : cleanIndices) {
            double w = (idx < qualityWeights.size()) ? qualityWeights[idx] : 1.0;
            weightedSum += w * zValues[idx];
            weightSum += w;
        }
        double weightedMean = (weightSum > 0.0)
            ? weightedSum / weightSum
            : kahanMean(cleanData.data(), cleanData.size());

        // Find closest Z index
        uint32_t bestZ = 0;
        double bestDist = std::numeric_limits<double>::infinity();
        for (size_t idx : cleanIndices) {
            double dist = std::abs(zValues[idx] - weightedMean);
            if (dist < bestDist) {
                bestDist = dist;
                bestZ = static_cast<uint32_t>(idx);
            }
        }
        // If no clean indices at all, just pick index 0
        if (cleanIndices.empty()) {
            bestZ = 0;
            weightedMean = zValues[0];
        }

        result.selectedZ = bestZ;
        result.bestModel = DistributionType::Gaussian;  // default fallback
        result.selectedValue = static_cast<float>(weightedMean);
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

    // 8. Choose the best Z value:
    //    Compute quality-weighted mean of non-outlier Z values,
    //    then find the Z index whose value is closest.
    double weightedSum = 0.0;
    double weightSum = 0.0;
    for (size_t idx : cleanIndices) {
        double w = (m_config.useQualityWeights && idx < qualityWeights.size())
            ? qualityWeights[idx] : 1.0;
        weightedSum += w * zValues[idx];
        weightSum += w;
    }
    double weightedMean = (weightSum > 0.0)
        ? weightedSum / weightSum
        : kahanMean(cleanData.data(), cleanData.size());

    // Find the Z index (among non-outliers) whose value is closest to the weighted mean
    uint32_t bestZ = 0;
    double bestDist = std::numeric_limits<double>::infinity();
    for (size_t idx : cleanIndices) {
        double dist = std::abs(zValues[idx] - weightedMean);
        if (dist < bestDist) {
            bestDist = dist;
            bestZ = static_cast<uint32_t>(idx);
        }
    }

    // 9. Return result
    result.selectedZ = bestZ;
    result.bestModel = bestType;
    result.selectedValue = static_cast<float>(weightedMean);
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
                    // Fitting failure — fall back to simple mean of Z-column
                    ++errorCount;
                    const float* col = cube.zColumnPtr(y, x);
                    double sum = 0.0;
                    for (size_t z = 0; z < N; ++z)
                        sum += col[z];
                    output[y * W + x] = static_cast<float>(sum / N);
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
