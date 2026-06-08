// src/engine/PixelSelector.h
#pragma once
#include "engine/SubCube.h"
#include "engine/DistributionFitter.h"
#include "engine/SkewNormalFitter.h"
#include "engine/GaussianMixEM.h"
#include "engine/OutlierDetector.h"
#include "engine/NumericalUtils.h"
#include <vector>
#include <functional>
#include <string>

namespace nukex {

// Progress callback: (rowsDone, totalRows)
using ProgressCallback = std::function<void(size_t, size_t)>;

class PixelSelector {
public:
    struct Config {
        int maxOutliers = 3;
        double outlierAlpha = 0.05;
        bool adaptiveModels = false;
        bool useGPU = false;
        bool enableMetadataTiebreaker = true;
    };

    PixelSelector();
    explicit PixelSelector(const Config& config);

    // Process a single pixel: fit distributions, select best Z
    // Returns the selected pixel value (the stacked result for this pixel)
    float processPixel(SubCube& cube, size_t y, size_t x,
                       const double* qualityScores = nullptr);

    // Process entire image -- returns a flat vector of selected pixel values
    // in row-major order (height * width)
    std::vector<float> processImage(SubCube& cube,
                                    const double* qualityScores = nullptr,
                                    ProgressCallback progress = nullptr);

    // GPU-accelerated image processing (falls back to CPU on failure or no GPU)
    std::vector<float> processImageGPU(SubCube& cube,
                                        const double* qualityScores,
                                        std::vector<uint8_t>& distTypesOut,
                                        ProgressCallback progress = nullptr);

    // Number of pixels that fell back to simple mean in the last processImage() call
    size_t lastErrorCount() const { return m_lastErrorCount; }

    bool lastGpuFallback() const { return m_lastGpuFallback; }
    const std::string& lastGpuError() const { return m_lastGpuError; }

    struct PixelResult {
        uint32_t selectedZ;
        DistributionType bestModel;
        float selectedValue;
    };

    // Select the best Z-value for a single pixel column.
    // Public so that Phase 7 CPU fallback can re-select individual trail pixels.
    PixelResult selectBestZ(const float* zColumnPtr, size_t nSubs,
                            const double* qualityScores = nullptr,
                            const uint8_t* maskColumn = nullptr);

private:
    Config m_config;
    size_t m_lastErrorCount = 0;
    bool m_lastGpuFallback = false;
    std::string m_lastGpuError;
};

} // namespace nukex
