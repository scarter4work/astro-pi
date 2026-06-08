//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// PixelStackAnalyzer - Per-pixel distribution fitting across frame stacks
// for intelligent pixel selection during integration

#ifndef __PixelStackAnalyzer_h
#define __PixelStackAnalyzer_h

#include "RegionStatistics.h"

#include <pcl/Image.h>
#include <pcl/String.h>

#include <vector>
#include <cmath>
#include <memory>
#include <cstdint>

namespace pcl
{

// ----------------------------------------------------------------------------
// Distribution Types for pixel stack analysis
// ----------------------------------------------------------------------------

enum class StackDistributionType : uint8_t
{
   Gaussian = 0,    // Normal - most common for stable pixels
   Lognormal,       // Right-skewed - variable signal or contamination
   Skewed,          // Asymmetric - transitional or mixed regions
   Bimodal,         // Two peaks - indicates inconsistent data (clouds, etc.)
   Uniform,         // Flat - highly variable, low confidence
   Count
};

// ----------------------------------------------------------------------------
// Distribution parameters fitted across frame stack
// ----------------------------------------------------------------------------

struct StackDistributionParams
{
   StackDistributionType type = StackDistributionType::Gaussian;
   float mu = 0.0f;           // Location (mean/median)
   float sigma = 0.0f;        // Scale (std dev)
   float skewness = 0.0f;     // Asymmetry
   float kurtosis = 0.0f;     // Tail weight
   float quality = 0.0f;      // Goodness of fit (0-1)

   StackDistributionParams() = default;

   StackDistributionParams( StackDistributionType t, float m, float s )
      : type( t ), mu( m ), sigma( s )
   {
   }
};

// ----------------------------------------------------------------------------
// Per-pixel metadata from stack analysis
// ----------------------------------------------------------------------------

struct PixelStackMetadata
{
   StackDistributionParams distribution;  // Fitted distribution across frames
   float selectedValue = 0.0f;            // The chosen "best" value
   uint16_t sourceFrame = 0;              // Which frame contributed this pixel
   float confidence = 0.0f;               // Confidence in selection (0-1)
   uint32_t outlierMask = 0;              // Bitmask of rejected frames (up to 32)

   // Check if a specific frame was marked as outlier
   bool IsOutlier( int frameIndex ) const
   {
      return (frameIndex < 32) && ((outlierMask & (1u << frameIndex)) != 0);
   }

   // Mark a frame as outlier
   void SetOutlier( int frameIndex )
   {
      if ( frameIndex < 32 )
         outlierMask |= (1u << frameIndex);
   }

   // Count number of outlier frames
   int OutlierCount() const
   {
      int count = 0;
      uint32_t mask = outlierMask;
      while ( mask )
      {
         count += (mask & 1);
         mask >>= 1;
      }
      return count;
   }
};

// ----------------------------------------------------------------------------
// Stack analysis configuration
// ----------------------------------------------------------------------------

struct StackAnalysisConfig
{
   // Outlier detection
   float outlierSigmaThreshold = 3.0f;    // Reject values beyond N sigma
   int   minFramesForStats = 3;           // Minimum frames needed for statistics
   bool  useRobustEstimators = true;      // Use median/MAD instead of mean/stddev

   // Distribution fitting
   float skewnessThreshold = 0.5f;        // Threshold for skewed distribution
   float bimodalityThreshold = 0.55f;     // Sarle's bimodality coefficient threshold

   // Selection behavior (can be overridden per-class)
   bool  favorHighSignal = false;         // For nebula: prefer brighter values
   bool  favorLowSignal = false;          // For dark nebula: prefer darker values
   bool  useMedianSelection = true;       // Default: select value closest to median
};

// ----------------------------------------------------------------------------
// PixelStackAnalyzer - Main analysis class
// ----------------------------------------------------------------------------

class PixelStackAnalyzer
{
public:

   /// Default constructor
   PixelStackAnalyzer();

   /// Constructor with configuration
   explicit PixelStackAnalyzer( const StackAnalysisConfig& config );

   /// Set analysis configuration
   void SetConfig( const StackAnalysisConfig& config ) { m_config = config; }
   const StackAnalysisConfig& Config() const { return m_config; }

   /// Analyze a stack of frames and produce per-pixel metadata
   /// @param frames Vector of prestretched frames (all same dimensions)
   /// @param channel Which channel to analyze (0 for mono/red)
   /// @return Grid of per-pixel metadata [height][width]
   std::vector<std::vector<PixelStackMetadata>> AnalyzeStack(
      const std::vector<const Image*>& frames,
      int channel = 0 ) const;

   /// Analyze a single pixel position across all frames
   /// @param values Pixel values from each frame at this position
   /// @return Metadata for this pixel
   PixelStackMetadata AnalyzePixel( const std::vector<float>& values ) const;

   /// Analyze with ML segmentation class hint
   /// @param values Pixel values from each frame
   /// @param regionClass ML segmentation class (RegionClass enum)
   /// @param classConfidence Confidence in the ML classification
   PixelStackMetadata AnalyzePixelWithClass(
      const std::vector<float>& values,
      RegionClass regionClass,
      float classConfidence = 1.0f ) const;

   /// Fit distribution to pixel values
   StackDistributionParams FitDistribution( const std::vector<float>& values ) const;

   /// Compute probability of a value given distribution
   static float ComputeProbability( float value, const StackDistributionParams& dist );

   /// Select best value from candidates given distribution and class
   float SelectBestValue(
      const std::vector<float>& values,
      const StackDistributionParams& dist,
      RegionClass regionClass,
      int& selectedFrame,
      float& confidence ) const;

   // ----------------------------------------------------------------------------
   // Statistical helper functions (static for reuse)
   // ----------------------------------------------------------------------------

   static float ComputeMean( const std::vector<float>& values );
   static float ComputeStdDev( const std::vector<float>& values, float mean );
   static float ComputeMedian( std::vector<float>& values );  // Modifies input!
   static float ComputeMAD( std::vector<float>& values, float median );
   static float ComputeSkewness( const std::vector<float>& values, float mean, float sigma );
   static float ComputeKurtosis( const std::vector<float>& values, float mean, float sigma );
   static float ComputeBimodalityCoefficient( const std::vector<float>& values, float mean, float sigma );

   /// Convert MAD to sigma (robust standard deviation)
   static float MADToSigma( float mad ) { return mad * 1.4826f; }

   // ----------------------------------------------------------------------------
   // Class-specific selection strategies
   // ----------------------------------------------------------------------------

   /// Get selection config adjusted for ML class
   StackAnalysisConfig GetClassAdjustedConfig( RegionClass regionClass ) const;

private:

   StackAnalysisConfig m_config;

   // Internal helpers
   StackDistributionType DetermineDistributionType(
      float skewness, float kurtosis, float bimodality ) const;

   void IdentifyOutliers(
      const std::vector<float>& values,
      const StackDistributionParams& dist,
      uint32_t& outlierMask ) const;
};

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __PixelStackAnalyzer_h
