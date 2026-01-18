//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// Region Analyzer - Main analysis engine

#ifndef __RegionAnalyzer_h
#define __RegionAnalyzer_h

#include "RegionStatistics.h"
#include "HistogramEngine.h"

#include <pcl/Image.h>
#include <vector>
#include <map>
#include <memory>

namespace pcl
{

// ----------------------------------------------------------------------------
// Region Analysis Configuration
// ----------------------------------------------------------------------------

struct RegionAnalyzerConfig
{
   // Histogram engine settings
   HistogramConfig histogramConfig;

   // Analysis settings
   bool analyzeAllChannels = true;     // Analyze R, G, B, and luminance
   bool computeColorMetrics = true;    // Compute saturation, hue, etc.
   bool detectDominantRegion = true;   // Identify the dominant region class

   // Sampling settings (for large images)
   bool useSampling = false;           // Sample instead of full analysis
   int maxSamplePixels = 1000000;      // Max pixels to sample
   double sampleFraction = 0.1;        // Fraction to sample if enabled

   // Classification thresholds
   double backgroundThreshold = 0.05;   // Median below this = background
   double brightThreshold = 0.5;        // Median above this = bright region
   double starCoreThreshold = 0.8;      // Median above this = star core
   double lowSNRThreshold = 5.0;        // SNR below this = low quality
   double highSNRThreshold = 20.0;      // SNR above this = high quality
};

// ----------------------------------------------------------------------------
// Region Analysis Result
// ----------------------------------------------------------------------------

struct RegionAnalysisResult
{
   // Per-region statistics
   std::map<RegionClass, RegionStatistics> regionStats;

   // Overall image statistics
   ChannelStatistics globalStats;

   // Region coverage (fraction of image)
   std::map<RegionClass, double> regionCoverage;

   // Dominant region (largest or most significant)
   RegionClass dominantRegion = RegionClass::Background;

   // Analysis metadata
   int imageWidth = 0;
   int imageHeight = 0;
   int numChannels = 0;
   bool isColor = false;
   size_t totalPixels = 0;

   // Quality metrics
   double overallSNR = 10.0;
   double overallDynamicRange = 1.0;
   double overallClipping = 0.0;

   // String summary
   String ToString() const;
};

// ----------------------------------------------------------------------------
// Region Analyzer
//
// Main analysis engine for region-aware image processing.
// Computes statistics for each region class using provided masks.
// ----------------------------------------------------------------------------

class RegionAnalyzer
{
public:

   RegionAnalyzer( const RegionAnalyzerConfig& config = RegionAnalyzerConfig() );

   // Analyze image without masks (global statistics only)
   RegionAnalysisResult Analyze( const Image& image ) const;

   // Analyze image with a single mask
   RegionAnalysisResult Analyze( const Image& image, const Image& mask,
                                 RegionClass regionClass = RegionClass::Background ) const;

   // Analyze image with multiple region masks
   // Each mask should be a single-channel image with soft weights (0-1)
   RegionAnalysisResult Analyze( const Image& image,
                                 const std::map<RegionClass, Image>& masks ) const;

   // Analyze image with segmentation result (multi-channel mask image)
   // Channel 0 = background, Channel 1 = stars, etc.
   RegionAnalysisResult AnalyzeWithSegmentation( const Image& image,
                                                  const Image& segmentationMask ) const;

   // Quick analysis (faster, less detailed)
   RegionAnalysisResult QuickAnalyze( const Image& image,
                                       const Image* mask = nullptr ) const;

   // Classify a region based on its statistics
   RegionClass ClassifyRegion( const RegionStatistics& stats ) const;

   // Get recommended algorithm for a region
   IsoString GetRecommendedAlgorithm( RegionClass regionClass,
                                       const RegionStatistics& stats ) const;

   // Configuration
   const RegionAnalyzerConfig& Config() const { return m_config; }
   void SetConfig( const RegionAnalyzerConfig& config ) { m_config = config; }

private:

   RegionAnalyzerConfig m_config;
   mutable HistogramEngine m_histogramEngine;

   // Determine dominant region from coverage map
   RegionClass FindDominantRegion( const std::map<RegionClass, double>& coverage,
                                   const std::map<RegionClass, RegionStatistics>& stats ) const;

   // Compute overall quality metrics
   void ComputeQualityMetrics( RegionAnalysisResult& result ) const;

   // Map segmentation channel index to region class
   static RegionClass ChannelToRegionClass( int channel );

   // Get priority for region (for determining dominance)
   static double GetRegionPriority( RegionClass rc, const RegionStatistics& stats );
};

// ----------------------------------------------------------------------------
// Region Mask Utilities
// ----------------------------------------------------------------------------

class RegionMaskUtils
{
public:

   // Create a mask from a threshold
   static Image CreateThresholdMask( const Image& image, int channel,
                                      double lowThreshold, double highThreshold );

   // Create an inverted mask
   static Image InvertMask( const Image& mask );

   // Combine masks (multiply)
   static Image MultiplyMasks( const Image& mask1, const Image& mask2 );

   // Combine masks (add with clamp)
   static Image AddMasks( const Image& mask1, const Image& mask2 );

   // Subtract masks (with clamp)
   static Image SubtractMasks( const Image& mask1, const Image& mask2 );

   // Smooth a mask (Gaussian blur)
   static Image SmoothMask( const Image& mask, double sigma );

   // Dilate a mask
   static Image DilateMask( const Image& mask, int radius );

   // Erode a mask
   static Image ErodeMask( const Image& mask, int radius );

   // Create gradient mask (distance from edge)
   static Image CreateGradientMask( const Image& binaryMask, double featherRadius );

   // Normalize mask to 0-1 range
   static void NormalizeMask( Image& mask );

   // Compute mask coverage (fraction of non-zero pixels)
   static double ComputeCoverage( const Image& mask );

   // Create a star mask from the image
   static Image CreateStarMask( const Image& image, double threshold = 0.5,
                                 int dilateRadius = 2 );

   // Create a background mask (inverse of star mask)
   static Image CreateBackgroundMask( const Image& image, double threshold = 0.1 );

   // Separate mask into core and halo
   static void SeparateCoreHalo( const Image& objectMask,
                                  Image& coreMask, Image& haloMask,
                                  double coreThreshold = 0.7 );
};

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __RegionAnalyzer_h
