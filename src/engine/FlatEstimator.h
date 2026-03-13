#pragma once

#include <vector>

namespace nukex {

struct FlatEstimatorConfig {
    double smoothSigma = 200.0; // Gaussian blur sigma — must be large enough to suppress galaxy-scale structure
    bool enabled = true;
};

class FlatEstimator {
public:
    explicit FlatEstimator(const FlatEstimatorConfig& config = {});

    // Compute a synthetic flat from unregistered (dithered) frames.
    // Median-stacks all frames without registration, smooths the result,
    // normalizes to mean=1.0, then divides each frame by the flat.
    //
    // frameData[frame][channel][pixel] — modified in-place.
    // Returns the per-channel self-flat for inspection/logging.
    std::vector<std::vector<float>> applyCorrection(
        std::vector<std::vector<std::vector<float>>>& frameData,
        int width, int height);

private:
    FlatEstimatorConfig m_config;

    // Compute median of values across frames for each pixel position
    std::vector<float> medianStack(
        const std::vector<std::vector<float>>& channelData,
        int numPixels) const;

    // Separable Gaussian blur (in-place)
    void gaussianBlur(std::vector<float>& image,
                      int width, int height, double sigma) const;
};

} // namespace nukex
