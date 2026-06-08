//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// TransitionChecker - Post-integration smoothing of hard transitions

#ifndef __TransitionChecker_h
#define __TransitionChecker_h

#include "DistributionFitter.h"
#include "RegionStatistics.h"

#include <pcl/Image.h>

#include <vector>

namespace pcl
{

// ----------------------------------------------------------------------------
// Transition Detection Result
// ----------------------------------------------------------------------------

struct TransitionInfo
{
   int tileX = 0;               // Tile X index
   int tileY = 0;               // Tile Y index

   // Boundary gradients (difference with adjacent tiles)
   float gradientTop = 0.0f;    // Gradient at top boundary
   float gradientBottom = 0.0f; // Gradient at bottom boundary
   float gradientLeft = 0.0f;   // Gradient at left boundary
   float gradientRight = 0.0f;  // Gradient at right boundary

   // Maximum gradient at any boundary
   float maxGradient = 0.0f;

   // Statistics of this tile
   float tileMean = 0.0f;
   float tileStdDev = 0.0f;

   // Detection flags
   bool hasHardTransition = false;   // Detected hard transition
   bool alignsWithFeature = false;   // Transition aligns with real feature (star, nebula edge)
   bool needsSmoothing = false;      // Should apply smoothing

   // Smoothing parameters (if needed)
   float smoothingStrength = 0.0f;   // 0-1, how much to smooth
};

// ----------------------------------------------------------------------------
// TransitionChecker Configuration
// ----------------------------------------------------------------------------

struct TransitionCheckerConfig
{
   int tileSize = 16;                    // Tile size for analysis

   // Detection thresholds
   float hardTransitionThreshold = 0.05f;  // Gradient threshold for hard transition
   float softTransitionThreshold = 0.02f;  // Below this, no smoothing needed

   // Smoothing parameters
   float maxSmoothingStrength = 0.5f;    // Maximum blending weight
   int smoothingRadius = 3;              // Pixels to blend at boundaries

   // Feature alignment
   bool checkFeatureAlignment = true;    // Check if transition aligns with real feature
   float featureAlignmentTolerance = 2.0f;  // Pixel tolerance for alignment
};

// ----------------------------------------------------------------------------
// TransitionChecker - Detects and smooths hard transitions
// ----------------------------------------------------------------------------

class TransitionChecker
{
public:

   /// Default constructor
   TransitionChecker();

   /// Constructor with configuration
   explicit TransitionChecker( const TransitionCheckerConfig& config );

   /// Set configuration
   void SetConfig( const TransitionCheckerConfig& config ) { m_config = config; }
   const TransitionCheckerConfig& Config() const { return m_config; }

   /// Analyze an image for hard transitions
   /// @param image The integrated image to analyze
   /// @param channel Channel to analyze (0 for mono/red)
   /// @return Grid of transition info [tilesY][tilesX]
   std::vector<std::vector<TransitionInfo>> AnalyzeTransitions(
      const Image& image,
      int channel = 0 ) const;

   /// Analyze with segmentation mask for feature alignment
   /// @param image The integrated image
   /// @param segmentation Segmentation result (class map)
   /// @param channel Channel to analyze
   std::vector<std::vector<TransitionInfo>> AnalyzeTransitions(
      const Image& image,
      const Image& segmentation,
      int channel = 0 ) const;

   /// Apply smoothing to detected transitions
   /// @param image Image to smooth (modified in place)
   /// @param transitions Transition analysis results
   /// @param channel Channel to process
   void ApplySmoothing(
      Image& image,
      const std::vector<std::vector<TransitionInfo>>& transitions,
      int channel = 0 ) const;

   /// Convenience: Analyze and smooth in one call
   /// @param image Image to process (modified in place)
   /// @param channel Channel to process
   /// @return Number of transitions smoothed
   int CheckAndSmooth( Image& image, int channel = 0 ) const;

   /// Convenience with segmentation
   int CheckAndSmooth( Image& image, const Image& segmentation, int channel = 0 ) const;

   /// Get summary of transitions found
   struct TransitionSummary
   {
      int totalTiles = 0;
      int tilesWithHardTransition = 0;
      int tilesNeedingSmoothing = 0;
      int tilesAlignedWithFeatures = 0;
      float avgGradient = 0.0f;
      float maxGradient = 0.0f;
   };

   static TransitionSummary GetSummary( const std::vector<std::vector<TransitionInfo>>& transitions );

private:

   TransitionCheckerConfig m_config;
   DistributionFitter m_fitter;

   // Compute gradient at tile boundary
   float ComputeBoundaryGradient(
      const Image& image,
      int channel,
      int tileX1, int tileY1,
      int tileX2, int tileY2,
      bool horizontal ) const;

   // Check if transition aligns with segmentation boundary
   bool CheckFeatureAlignment(
      const Image& segmentation,
      int tileX, int tileY,
      int neighborTileX, int neighborTileY ) const;

   // Apply Gaussian smoothing at a boundary
   void SmoothBoundary(
      Image& image,
      int channel,
      int boundaryX, int boundaryY,
      bool horizontal,
      float strength ) const;
};

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __TransitionChecker_h
