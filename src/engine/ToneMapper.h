//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// Tone Mapper - Final tone adjustments

#ifndef __ToneMapper_h
#define __ToneMapper_h

#include <pcl/Image.h>
#include <pcl/String.h>

#include <vector>

namespace pcl
{

// ----------------------------------------------------------------------------
// Tone Mapping Configuration
// ----------------------------------------------------------------------------

struct ToneMapConfig
{
   // Black/white point adjustment
   double blackPoint = 0.0;          // Input black point (0-1)
   double whitePoint = 1.0;          // Input white point (0-1)
   bool autoBlackPoint = false;      // Auto-detect from histogram
   bool autoWhitePoint = false;      // Auto-detect from histogram

   // Gamma correction
   double gamma = 1.0;               // Gamma value (1.0 = no change)

   // Contrast
   double contrast = 0.0;            // Contrast adjustment (-1 to 1)
   double localContrast = 0.0;       // Local contrast enhancement (0-1)
   int localContrastRadius = 50;     // Radius for local contrast

   // Brightness
   double brightness = 0.0;          // Brightness offset (-1 to 1)

   // Histogram equalization
   double equalization = 0.0;        // Equalization strength (0-1)
   bool adaptiveEqualization = false;// Use CLAHE

   // Curve
   std::vector<std::pair<double, double>> curve;  // Custom curve points

   // Soft clipping
   double softClipShoulder = 0.0;    // Soft clip at highlights (0-1)
   double softClipToe = 0.0;         // Soft clip at shadows (0-1)
};

// ----------------------------------------------------------------------------
// Tone Mapper
//
// Applies final tone adjustments to the blended image.
// ----------------------------------------------------------------------------

class ToneMapper
{
public:

   ToneMapper( const ToneMapConfig& config = ToneMapConfig() );

   // Apply tone mapping to image (in-place)
   void Apply( Image& image ) const;

   // Apply tone mapping (returns new image)
   Image Apply( const Image& image ) const;

   // Apply to a single value
   double ApplyToValue( double value ) const;

   // Auto-detect black/white points from image
   void AutoDetectPoints( const Image& image );

   // Build lookup table for fast processing
   void BuildLUT( int lutSize = 65536 );

   // Apply using LUT (faster for repeated application)
   void ApplyLUT( Image& image ) const;

   // Configuration
   const ToneMapConfig& Config() const { return m_config; }
   void SetConfig( const ToneMapConfig& config );

private:

   ToneMapConfig m_config;
   std::vector<double> m_lut;
   bool m_lutValid = false;

   // Individual adjustments
   double ApplyBlackWhitePoint( double value ) const;
   double ApplyGamma( double value ) const;
   double ApplyContrast( double value ) const;
   double ApplyBrightness( double value ) const;
   double ApplyCurve( double value ) const;
   double ApplySoftClip( double value ) const;

   // Local contrast enhancement (requires full image)
   void ApplyLocalContrast( Image& image ) const;

   // Histogram equalization (requires full image)
   void ApplyEqualization( Image& image ) const;
};

// ----------------------------------------------------------------------------
// Film-Like Tone Curves
// ----------------------------------------------------------------------------

namespace FilmCurves
{
   // S-curve (standard film response)
   double SCurve( double value, double strength );

   // Reinhard tone mapping
   double Reinhard( double value, double whitePoint = 1.0 );

   // Filmic tone mapping (Uncharted 2 style)
   double Filmic( double value );

   // ACES tone mapping
   double ACES( double value );

   // Logarithmic
   double Logarithmic( double value, double base = 10.0 );

   // Create curve points for a named curve
   std::vector<std::pair<double, double>> CreateNamedCurve( const String& name );
}

// ----------------------------------------------------------------------------
// Histogram Operations
// ----------------------------------------------------------------------------

class HistogramMapper
{
public:

   HistogramMapper();

   // Compute histogram from image
   void ComputeHistogram( const Image& image, int channel = 0 );

   // Get percentile value
   double GetPercentile( double p ) const;

   // Create equalization LUT
   std::vector<double> CreateEqualizationLUT() const;

   // Create contrast-limited equalization LUT (CLAHE-like)
   std::vector<double> CreateCLAHELUT( double clipLimit = 2.0 ) const;

   // Apply histogram matching
   void ApplyHistogramMatch( Image& image,
                              const std::vector<double>& targetCDF ) const;

private:

   std::vector<double> m_histogram;
   std::vector<double> m_cdf;
   int m_numBins = 65536;
   double m_totalCount = 0;
};

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __ToneMapper_h
