#pragma once

#include <vector>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>

#include "engine/SubCube.h"

namespace nukex {

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

struct ArtifactDetectorConfig
{
   // Trail detection
   double trailOutlierSigma  = 5.0;    // Sobel gradient threshold in MAD units
   double trailDilateRadius   = 5.0;    // Dilation radius (pixels) around detected lines
   int    trailBlockSize      = 64;     // Block size for background estimation grid
   int    houghThetaBins      = 180;    // Number of angle bins (1-degree steps)

   // Dust mote detection
   int    dustMinDiameter     = 10;     // Minimum blob diameter (pixels)
   int    dustMaxDiameter     = 150;    // Maximum blob diameter (pixels)
   double dustCircularityMin  = 0.5;
   double dustAttenuationMin  = 0.02;
   double dustDetectionSigma  = 3.0;    // Background deficit threshold in MAD units

   // Vignetting detection
   int    vignettingPolyOrder = 4;      // Radial polynomial order
   double vignettingMinCorr   = 0.01;   // Minimum correction to report
   double vignettingMaxCorrection = 1.5; // Upper clamp on correction factor
};

// ---------------------------------------------------------------------------
// Result structures
// ---------------------------------------------------------------------------

struct TrailDetectionResult
{
   std::vector<uint8_t> mask;    // 1 = trail pixel, 0 = clean (row-major, W*H)
   int trailPixelCount = 0;
   int trailLineCount  = 0;
};

struct DustBlobInfo
{
   double centerX        = 0;
   double centerY        = 0;
   double radius         = 0;
   double circularity    = 0;
   double meanAttenuation = 0;
};

struct DustDetectionResult
{
   std::vector<uint8_t> mask;    // 1 = dust pixel
   std::vector<DustBlobInfo> blobs;
   int dustPixelCount = 0;
};

struct VignettingDetectionResult
{
   std::vector<float> correctionMap;  // multiplicative correction (row-major, W*H)
   double maxCorrection = 1.0;        // peak correction factor
};

struct DetectionResult
{
   TrailDetectionResult   trails;
   DustDetectionResult    dust;
   VignettingDetectionResult vignetting;
};

// ---------------------------------------------------------------------------
// Hough line representation (internal, but exposed for testing convenience)
// ---------------------------------------------------------------------------

struct HoughLine
{
   double rho;     // distance from origin
   double theta;   // angle in radians
   int    votes;   // accumulator count
};

// ---------------------------------------------------------------------------
// Logging callback — engine code stays PCL-agnostic
// ---------------------------------------------------------------------------

using LogCallback = std::function<void( const std::string& )>;

// ---------------------------------------------------------------------------
// ArtifactDetector
// ---------------------------------------------------------------------------

class ArtifactDetector
{
public:
   explicit ArtifactDetector( const ArtifactDetectorConfig& config = ArtifactDetectorConfig{} );

   // --- Primary detection methods ---
   TrailDetectionResult      detectTrails( const float* image, int width, int height ) const;
   DustDetectionResult       detectDust( const float* image, int width, int height ) const;
   DustDetectionResult       detectDustSubcube( const float* stackedImage,
                                                 const std::vector<SubCube*>& channelCubes,
                                                 int width, int height,
                                                 LogCallback log = nullptr ) const;
   VignettingDetectionResult detectVignetting( const float* image, int width, int height,
                                                const uint8_t* excludeMask = nullptr ) const;
   DetectionResult           detectAll( const float* image, int width, int height ) const;

private:
   ArtifactDetectorConfig m_config;

   // --- Trail detection helpers ---
   std::vector<float> estimateBackground( const float* image, int width, int height ) const;
   std::vector<float> sobelMagnitude( const float* image, int width, int height ) const;
   std::vector<HoughLine> houghLines( const uint8_t* edgeMask, int width, int height ) const;
   std::vector<uint8_t> generateTrailMask( const std::vector<HoughLine>& lines,
                                            int width, int height,
                                            double dilateRadius ) const;

   // --- Dust/vignetting helpers ---
   std::vector<float> localBackgroundMap( const float* image, int width, int height, int kernelSize ) const;
   std::vector<double> fitRadialPolynomial( const float* image, int width, int height,
                                             const uint8_t* excludeMask, int order ) const;
};

} // namespace nukex
