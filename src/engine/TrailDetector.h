// src/engine/TrailDetector.h
#pragma once

#include <vector>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>

namespace nukex {

class SubCube;  // forward declaration

struct TrailDetectorConfig
{
    double seedSigma       = 3.0;   // spatial outlier threshold (local MAD units)
    int    seedWindowSize  = 7;     // local neighborhood window (pixels, odd)
    double linearityMin    = 0.9;   // minimum linearity score for cluster
    int    minClusterLen   = 20;    // minimum extent along principal axis (pixels)
    double confirmSigma    = 2.5;   // cross-line neighbor confirmation threshold (lower than seedSigma — line geometry is already established)
    int    crossLineOffset = 4;     // perpendicular neighbor distance (pixels)
    double dilateRadius    = 2.0;   // perpendicular dilation (pixels)
    int    gapTolerance    = 3;     // morphological dilation radius before connected-component labeling
};

struct TrailLine
{
    double cx, cy;     // centroid of seed cluster
    double dx, dy;     // unit direction vector along the line
    int    confirmedCount = 0;
};

struct FrameTrailResult
{
    int frameIndex     = -1;
    int maskedPixels   = 0;
    int linesDetected  = 0;
    std::vector<TrailLine> lines;
};

using LogCallback = std::function<void( const std::string& )>;

class TrailDetector
{
public:
    explicit TrailDetector( const TrailDetectorConfig& config = TrailDetectorConfig{} );

    // Detect trails in a single frame. frameData is row-major (height * width).
    // alignMask is the existing alignment mask for this frame (nullptr = no alignment mask).
    // Returns result with detected lines and pixel count.
    FrameTrailResult detectFrame( const float* frameData,
                                  const uint8_t* alignMask,
                                  int width, int height ) const;

    // Detect trails across all frames in a SubCube and set masks.
    // Returns total number of pixel-frames masked.
    int detectAndMask( SubCube& cube, LogCallback log = nullptr ) const;

private:
    TrailDetectorConfig m_config;

    // Internal pipeline stages
    std::vector<uint8_t> findSeeds( const float* frameData,
                                    const uint8_t* alignMask,
                                    int width, int height ) const;

    struct Cluster {
        std::vector<int> pixelIndices;  // indices into W*H flat array
        double cx, cy;                  // centroid
        double dirX, dirY;              // principal axis unit vector
        double linearity;               // 1 - (lambda_min / lambda_max)
        double extent;                  // length along principal axis
    };

    std::vector<Cluster> clusterSeeds( const std::vector<uint8_t>& seeds,
                                       int width, int height ) const;

    std::vector<uint8_t> walkAndConfirm( const float* frameData,
                                          const uint8_t* alignMask,
                                          const std::vector<Cluster>& clusters,
                                          int width, int height,
                                          std::vector<TrailLine>& linesOut ) const;
};

} // namespace nukex
