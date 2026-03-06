// src/engine/PixelSelector.h
#pragma once
#include "engine/SubCube.h"
#include "engine/DistributionFitter.h"
#include "engine/SkewNormalFitter.h"
#include "engine/GaussianMixEM.h"
#include "engine/OutlierDetector.h"
#include "engine/NumericalUtils.h"
#include <vector>

namespace nukex {

class PixelSelector {
public:
    struct Config {
        int maxOutliers = 3;          // max outliers to detect per pixel
        double outlierAlpha = 0.05;   // significance level
        bool useQualityWeights = true;
    };

    PixelSelector();
    explicit PixelSelector(const Config& config);

    // Process a single pixel: fit distributions, select best Z
    // Returns the selected pixel value (the stacked result for this pixel)
    float processPixel(SubCube& cube, size_t y, size_t x,
                       const std::vector<double>& qualityWeights);

    // Process entire image -- returns a flat vector of selected pixel values
    // in row-major order (height * width)
    std::vector<float> processImage(SubCube& cube,
                                    const std::vector<double>& qualityWeights);

private:
    Config m_config;

    struct PixelResult {
        uint32_t selectedZ;
        DistributionType bestModel;
        float selectedValue;
    };

    PixelResult selectBestZ(const float* zColumnPtr, size_t nSubs,
                            const std::vector<double>& qualityWeights);
};

} // namespace nukex
