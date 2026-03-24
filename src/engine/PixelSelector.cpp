// src/engine/PixelSelector.cpp
#include "engine/PixelSelector.h"

#ifdef NUKEX_HAS_CUDA
#include "engine/cuda/CudaPixelSelector.h"
#include "engine/cuda/CudaRuntime.h"
#endif

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
                           const double* qualityScores,
                           const uint8_t* maskColumn)
{
    PixelResult result{};

    // 0. If per-frame masks are present, pre-filter masked frames
    //    (trail-contaminated pixels are excluded before any statistics)
    std::vector<double> zValues;
    std::vector<size_t> originalIndices;  // maps filtered index → original frame index
    if (maskColumn) {
        zValues.reserve(nSubs);
        originalIndices.reserve(nSubs);
        for (size_t i = 0; i < nSubs; ++i) {
            if (maskColumn[i] == 0) {
                zValues.push_back(static_cast<double>(zColumnPtr[i]));
                originalIndices.push_back(i);
            }
        }
        // If too many masked, fall back to all frames
        if (zValues.size() < 3) {
            zValues = promoteToDouble(zColumnPtr, nSubs);
            originalIndices.resize(nSubs);
            std::iota(originalIndices.begin(), originalIndices.end(), 0);
        }
    } else {
        // 1. Promote float Z-column to double (no masking)
        zValues = promoteToDouble(zColumnPtr, nSubs);
        originalIndices.resize(nSubs);
        std::iota(originalIndices.begin(), originalIndices.end(), 0);
    }

    size_t nValid = zValues.size();

    // 2a. MAD-based sigma clipping pre-filter (robust to outliers).
    // MAD uses the median, so outliers can't inflate the scale estimate
    // and mask their own detection (unlike mean+stddev in ESD).
    std::vector<size_t> madOutliers = sigmaClipMAD(zValues, 3.0);
    std::unordered_set<size_t> madOutlierSet(madOutliers.begin(),
                                              madOutliers.end());

    // Build pre-filtered data for ESD
    std::vector<double> preFiltered;
    std::vector<size_t> preFilteredIndices;
    preFiltered.reserve(nValid);
    preFilteredIndices.reserve(nValid);
    for (size_t i = 0; i < nValid; ++i) {
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
    cleanData.reserve(nValid);
    cleanIndices.reserve(nValid);
    for (size_t i = 0; i < nValid; ++i) {
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
            cleanData.resize(nValid);
            cleanIndices.resize(nValid);
            for (size_t i = 0; i < nValid; ++i) {
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
                bestZ = static_cast<uint32_t>(originalIndices[idx]);
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

    // 5. Fit models — adaptive mode may skip expensive fits
    //    Use AICc (corrected AIC) because N is small (~30 subs).
    //    Plain AIC under-penalizes complex models at small N, causing
    //    Bimodal (k=5) to dominate even on unimodal data.
    int nClean = static_cast<int>(cleanData.size());

    FitResult gaussianFit = fitGaussian(cleanData);
    double aicGaussian = aicc(gaussianFit.logLikelihood, gaussianFit.k, nClean);

    FitResult poissonFit = fitPoisson(cleanData);
    double aicPoisson = aicc(poissonFit.logLikelihood, poissonFit.k, nClean);

    double aicSkew = std::numeric_limits<double>::infinity();
    double aicMix  = std::numeric_limits<double>::infinity();

    bool skipExpensive = false;
    if (m_config.adaptiveModels) {
        // With small N (< 6 clean points), complex models with 3-5 parameters
        // are poorly constrained and AIC correction dominates — skip them.
        // With larger N, skip if the best simple model's per-sample log-likelihood
        // exceeds -1.0 (very good fit), meaning AIC/N < 2*k/N + 2.0 which for
        // Gaussian (k=2) means AIC < 4 + 2N — essentially always good.
        // A more targeted criterion: skip if Gaussian normalized log-likelihood
        // is above -1.0 per sample (tight fit to data).
        double bestSimpleAIC = std::min(aicGaussian, aicPoisson);
        double n = static_cast<double>(cleanData.size());
        skipExpensive = (cleanData.size() < 6)
                     || (bestSimpleAIC / n < 2.0);  // normalized AIC indicates good fit
    }

    if (!skipExpensive) {
        SkewNormalFitResult skewFit = fitSkewNormal(cleanData);
        aicSkew = aicc(skewFit.logLikelihood, skewFit.k, nClean);

        GaussianMixResult mixFit = fitGaussianMixture2(cleanData);
        // Reject bimodal if one component dominates (weight near 0 or 1)
        // — EM always converges to some split, even on unimodal data
        if (mixFit.weight > 0.05 && mixFit.weight < 0.95)
            aicMix = aicc(mixFit.logLikelihood, mixFit.k, nClean);
    }

    // 6. Select model with lowest AIC
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

    // 7. Distribution-aware central tendency selection.
    //    The best-fit distribution tells us the shape of the pixel stack,
    //    and the optimal estimator depends on that shape:
    //      Gaussian/Poisson  → quality-weighted mean (optimal for symmetric)
    //      Skew-Normal       → median (robust to tail-pull)
    //      Bimodal           → weighted mean of dominant component
    //                          (median falls in the valley between peaks)
    // Densest-cluster value selection (shortest-half mode estimator)
    //
    // Find the narrowest interval containing half the clean values.
    // The center of that interval is where the data is most concentrated —
    // the MODE of the distribution.  This naturally:
    //   - Finds the dark cluster for background with bright contamination (trails)
    //   - Finds the bright cluster for signal with dark contamination (dust motes)
    //   - Matches the median for clean symmetric data
    double selectedValue = cleanMedian;
    {
        // Build sorted clean values
        std::vector<double> sortedClean;
        sortedClean.reserve(cleanIndices.size());
        for (size_t idx : cleanIndices)
            sortedClean.push_back(zValues[idx]);
        std::sort(sortedClean.begin(), sortedClean.end());

        int N = static_cast<int>(sortedClean.size());
        int halfN = N / 2;
        if (halfN < 1) halfN = 1;

        double minRange = sortedClean[halfN - 1] - sortedClean[0];
        int bestStart = 0;
        for (int i = 1; i + halfN - 1 < N; ++i) {
            double range = sortedClean[i + halfN - 1] - sortedClean[i];
            if (range < minRange) {
                minRange = range;
                bestStart = i;
            }
        }

        // Mode estimate: mean of the densest half
        double sum = 0.0;
        for (int i = bestStart; i < bestStart + halfN; ++i)
            sum += sortedClean[i];
        selectedValue = sum / halfN;
    }

    // 8. Find the frame whose value is closest to the computed central tendency
    uint32_t bestZ = 0;
    double bestDist = std::numeric_limits<double>::infinity();
    for (size_t idx : cleanIndices) {
        double dist = std::abs(zValues[idx] - selectedValue);
        if (dist < bestDist) {
            bestDist = dist;
            bestZ = static_cast<uint32_t>(originalIndices[idx]);
        }
    }

    // 8b. Metadata tiebreaker: if multiple frames are within MAD tolerance
    //     of the selected value, prefer the one with the best quality score.
    if (qualityScores != nullptr && m_config.enableMetadataTiebreaker) {
        std::vector<double> shSorted;
        shSorted.reserve(cleanIndices.size());
        for (size_t idx : cleanIndices)
            shSorted.push_back(zValues[idx]);
        std::sort(shSorted.begin(), shSorted.end());

        int shN = static_cast<int>(shSorted.size());
        int shHalf = shN / 2;
        if (shHalf < 1) shHalf = 1;

        if (shHalf > 1) {
            double shMinRange = shSorted[shHalf - 1] - shSorted[0];
            int shBestStart = 0;
            for (int i = 1; i + shHalf - 1 < shN; ++i) {
                double range = shSorted[i + shHalf - 1] - shSorted[i];
                if (range < shMinRange) {
                    shMinRange = range;
                    shBestStart = i;
                }
            }

            double shLo = shSorted[shBestStart];
            double shHi = shSorted[shBestStart + shHalf - 1];
            std::vector<double> shValues(shSorted.begin() + shBestStart,
                                          shSorted.begin() + shBestStart + shHalf);
            double shMedian = medianOfSorted(shValues.data(), shValues.size());
            std::vector<double> shDeviations(shValues.size());
            for (size_t i = 0; i < shValues.size(); ++i)
                shDeviations[i] = std::abs(shValues[i] - shMedian);
            std::sort(shDeviations.begin(), shDeviations.end());
            double shMAD = 1.4826 * medianOfSorted(shDeviations.data(), shDeviations.size());

            if (shMAD > 0.0) {
                double bestScore = qualityScores[bestZ];
                for (size_t idx : cleanIndices) {
                    double val = zValues[idx];
                    if (val >= shLo && val <= shHi &&
                        std::abs(val - selectedValue) <= shMAD) {
                        double score = qualityScores[originalIndices[idx]];
                        if (score > bestScore) {
                            bestScore = score;
                            bestZ = static_cast<uint32_t>(originalIndices[idx]);
                        }
                    }
                }
            }
        }
    }

    // 9. Return result
    result.selectedZ = bestZ;
    result.bestModel = bestType;
    result.selectedValue = static_cast<float>(selectedValue);
    return result;
}

float PixelSelector::processPixel(SubCube& cube, size_t y, size_t x,
                                   const double* qualityScores)
{
    auto result = selectBestZ(cube.zColumnPtr(y, x), cube.numSubs(), qualityScores,
                              cube.maskColumnPtr(y, x));
    cube.setProvenance(y, x, result.selectedZ);
    cube.setDistType(y, x, static_cast<uint8_t>(result.bestModel));
    return result.selectedValue;
}

std::vector<float> PixelSelector::processImage(SubCube& cube,
                                                const double* qualityScores,
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
                    auto result = selectBestZ(cube.zColumnPtr(y, x), N, qualityScores,
                                              cube.maskColumnPtr(y, x));
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

std::vector<float> PixelSelector::processImageGPU(SubCube& cube,
                                                    const double* qualityScores,
                                                    std::vector<uint8_t>& distTypesOut,
                                                    ProgressCallback progress)
{
#ifdef NUKEX_HAS_CUDA
    m_lastGpuFallback = false;
    m_lastGpuError.clear();

    if (!cuda::isGpuAvailable()) {
        // No GPU — fall back to CPU
        m_lastGpuFallback = true;
        m_lastGpuError = "No CUDA-capable GPU detected at runtime";
        return processImage(cube, qualityScores, progress);
    }

    size_t H = cube.height();
    size_t W = cube.width();
    size_t totalPixels = H * W;

    std::vector<float> output(totalPixels);
    distTypesOut.resize(totalPixels);

    cuda::GpuStackConfig gpuConfig;
    gpuConfig.maxOutliers = m_config.maxOutliers;
    gpuConfig.outlierAlpha = m_config.outlierAlpha;
    gpuConfig.adaptiveModels = m_config.adaptiveModels;
    gpuConfig.enableMetadataTiebreaker = m_config.enableMetadataTiebreaker;
    gpuConfig.nSubs = cube.numSubs();
    gpuConfig.height = H;
    gpuConfig.width = W;
    gpuConfig.qualityScores = qualityScores;
    gpuConfig.maskData = cube.maskTensorData();
    gpuConfig.provenanceOut = nullptr;

    auto result = cuda::processImageGPU(
        cube.cube().data(), output.data(), distTypesOut.data(), gpuConfig);

    if (!result.success) {
        // GPU failed — fall back to CPU. Log error for diagnostics.
        m_lastGpuFallback = true;
        m_lastGpuError = result.errorMessage;
        std::fprintf(stderr, "NukeX: GPU stacking failed: %s -- falling back to CPU\n",
            result.errorMessage.c_str());
        return processImage(cube, qualityScores, progress);
    }

    // Copy distTypes into SubCube metadata
    for (size_t y = 0; y < H; ++y)
        for (size_t x = 0; x < W; ++x)
            cube.setDistType(y, x, distTypesOut[y * W + x]);

    return output;
#else
    // No CUDA compiled in — CPU only
    (void)distTypesOut;
    return processImage(cube, qualityScores, progress);
#endif
}

} // namespace nukex
