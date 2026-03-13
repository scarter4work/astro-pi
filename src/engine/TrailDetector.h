#pragma once

#include <vector>
#include <cstdint>

namespace nukex {

struct DetectedTrail {
    double rho;      // distance from origin (pixels)
    double theta;    // angle in radians [0, pi)
    int votes;       // Hough accumulator votes
    double width;    // estimated trail width (pixels)
};

struct TrailDetectorConfig {
    double threshold = 1.5;       // sigma threshold for binary edge detection
    int minLineLength = 100;      // min trail length in pixels
    int maxLineGap = 20;          // max gap between trail segments
    double dilateRadius = 5.0;    // mask dilation radius (pixels)
    int backgroundBoxSize = 64;   // local background estimation box size
};

class TrailDetector {
public:
    explicit TrailDetector(const TrailDetectorConfig& config = {});

    // Detect trails in a single 2D frame and return a binary mask.
    // mask[i] = 1 means pixel i is part of a trail and should be rejected.
    // frameData is a flat row-major array of width*height floats.
    std::vector<uint8_t> detectAndMask(const float* frameData,
                                        int width, int height) const;

    // Detect trails in all frames, returning per-frame masks.
    // frameMasks[f][pixel] = 1 if pixel in frame f is trail-contaminated.
    std::vector<std::vector<uint8_t>> detectAllFrames(
        const std::vector<std::vector<float>>& frameData,
        int width, int height) const;

    // Get the trails detected in the last call (for logging)
    const std::vector<DetectedTrail>& lastDetectedTrails() const { return m_lastTrails; }

private:
    TrailDetectorConfig m_config;
    mutable std::vector<DetectedTrail> m_lastTrails;

    // Compute local background using box median
    std::vector<float> estimateBackground(const float* data,
                                           int width, int height) const;

    // Sobel gradient magnitude
    std::vector<float> sobelMagnitude(const float* residual,
                                       int width, int height) const;

    // Hough transform line detection
    std::vector<DetectedTrail> houghLines(const std::vector<uint8_t>& edges,
                                           int width, int height) const;

    // Generate dilated mask from detected lines
    std::vector<uint8_t> generateMask(const std::vector<DetectedTrail>& trails,
                                       int width, int height) const;
};

} // namespace nukex
