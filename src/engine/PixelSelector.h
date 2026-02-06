//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// PixelSelector - Intelligent pixel selection for integration
// Combines statistical analysis with ML segmentation for optimal pixel selection

#ifndef __PixelSelector_h
#define __PixelSelector_h

#include "PixelStackAnalyzer.h"
#include "RegionStatistics.h"

#include <pcl/Image.h>
#include <pcl/String.h>
#include <pcl/FITSHeaderKeyword.h>

#include <vector>
#include <memory>
#include <string>

namespace pcl
{

// ----------------------------------------------------------------------------
// Selection result for a single pixel
// ----------------------------------------------------------------------------

struct PixelSelectionResult
{
   float value = 0.0f;                        // Selected pixel value
   uint16_t sourceFrame = 0;                  // Frame index that contributed this value
   float confidence = 0.0f;                   // Confidence in selection (0-1)
   RegionClass regionClass = RegionClass::Background;  // ML segmentation class
   float classConfidence = 0.0f;              // Confidence in ML class
   uint64_t outlierMask = 0;                  // Per-frame detail for first 64 frames
   uint16_t outlierCount = 0;                 // Total outlier count
   uint16_t totalFrames = 0;                  // Total frames analyzed

   // Distribution info (for debugging/analysis)
   float distMu = 0.0f;
   float distSigma = 0.0f;
};

// ----------------------------------------------------------------------------
// Context information from FITS headers
// ----------------------------------------------------------------------------

struct TargetContext
{
   String objectName;               // OBJECT keyword (e.g., "M42", "NGC 7000")
   String filter;                   // FILTER keyword (e.g., "Ha", "OIII", "L")
   double exposureTime = 0.0;       // EXPTIME in seconds
   double gain = 0.0;               // GAIN setting
   double temperature = 0.0;        // CCD-TEMP
   String telescope;                // TELESCOP
   String instrument;               // INSTRUME

   // Expected feature hints based on object
   bool expectsEmissionNebula = false;
   bool expectsDarkNebula = false;
   bool expectsGalaxy = false;
   bool expectsStarCluster = false;
   bool expectsPlanetaryNebula = false;

   // Parse FITS headers to populate context
   void ParseFromHeaders( const FITSKeywordArray& keywords );

   // Infer expected features from object name
   void InferExpectedFeatures();
};

// ----------------------------------------------------------------------------
// Spatial context for a pixel (neighboring classes)
// ----------------------------------------------------------------------------

struct SpatialContext
{
   int centerClass = 0;             // ML class at this pixel
   int neighborClasses[8] = {0};    // Classes of 8 neighbors (clockwise from top)
   float neighborConfidences[8] = {0};

   // Statistics about neighborhood
   int dominantNeighborClass = 0;
   int numMatchingNeighbors = 0;    // How many neighbors match center
   bool isEdgePixel = false;        // At image boundary
   bool isTransitionZone = false;   // Multiple classes in neighborhood
};

// ----------------------------------------------------------------------------
// PixelSelector Configuration
// ----------------------------------------------------------------------------

struct PixelSelectorConfig
{
   // Stack analysis config
   StackAnalysisConfig stackConfig;

   // Spatial context
   bool useSpatialContext = true;        // Consider neighboring pixels
   float transitionSmoothingWeight = 0.3f;  // Blending weight in transition zones

   // ML integration
   float minClassConfidence = 0.5f;      // Min confidence to use ML class
   bool fallbackToMedian = true;         // If ML fails, use median selection

   // Target context
   bool useTargetContext = true;         // Use FITS metadata hints
   float contextWeight = 0.2f;           // How much target hints affect selection
};

// ----------------------------------------------------------------------------
// PixelSelector - Main selection orchestrator
// ----------------------------------------------------------------------------

class PixelSelector
{
public:

   /// Default constructor
   PixelSelector();

   /// Constructor with configuration
   explicit PixelSelector( const PixelSelectorConfig& config );

   /// Set configuration
   void SetConfig( const PixelSelectorConfig& config ) { m_config = config; }
   const PixelSelectorConfig& Config() const { return m_config; }

   /// Set target context from FITS headers
   void SetTargetContext( const TargetContext& context ) { m_targetContext = context; }
   void SetTargetContext( const FITSKeywordArray& keywords );

   /// Set ML segmentation results (flat vectors, indexed as [y * width + x])
   /// @param segmentationMap Per-pixel class labels, flat [height * width]
   /// @param confidenceMap Per-pixel confidence, flat [height * width]
   /// @param width Image width
   /// @param height Image height
   void SetSegmentation(
      const std::vector<int>& segmentationMap,
      const std::vector<float>& confidenceMap,
      int width, int height );

   /// Set ML segmentation results (2D vector overload for backward compatibility)
   /// Internally flattens to 1D storage
   void SetSegmentation(
      const std::vector<std::vector<int>>& segmentationMap,
      const std::vector<std::vector<float>>& confidenceMap );

   /// Set ML segmentation from Image (class in channel 0, confidence in channel 1)
   void SetSegmentation( const Image& segmentationImage );

   /// Process a stack of frames and produce integrated image
   /// @param frames Input prestretched frames
   /// @param channel Channel to process
   /// @return Integrated image with best pixels selected
   Image ProcessStack(
      const std::vector<const Image*>& frames,
      int channel = 0 ) const;

   /// Process stack and return detailed metadata
   /// @param frames Input prestretched frames
   /// @param channel Channel to process
   /// @param metadata Output per-pixel metadata (flat, indexed as [y * width + x])
   /// @return Integrated image
   Image ProcessStackWithMetadata(
      const std::vector<const Image*>& frames,
      int channel,
      std::vector<PixelSelectionResult>& metadata ) const;

   /// Select pixel value at a specific position
   /// @param values Pixel values from all frames
   /// @param x X coordinate (for spatial context)
   /// @param y Y coordinate (for spatial context)
   PixelSelectionResult SelectPixel(
      const std::vector<float>& values,
      int x, int y ) const;

   /// Get spatial context for a pixel position
   SpatialContext GetSpatialContext( int x, int y ) const;

   /// Set per-frame quality weights (e.g., from EXPTIME normalization)
   /// Weights should be normalized to [0,1]. Empty means equal weight.
   void SetFrameWeights( const std::vector<float>& weights );

   /// Access the internal analyzer
   const PixelStackAnalyzer& Analyzer() const { return m_analyzer; }

private:

   PixelSelectorConfig m_config;
   PixelStackAnalyzer m_analyzer;
   TargetContext m_targetContext;

   // Segmentation data (flat vectors, indexed as [y * m_segWidth + x])
   std::vector<int> m_segmentationMap;
   std::vector<float> m_confidenceMap;
   int m_segWidth = 0;
   int m_segHeight = 0;
   bool m_hasSegmentation = false;

   // Per-frame quality weights
   std::vector<float> m_frameWeights;

   // Get ML class at position (with bounds checking)
   RegionClass GetRegionClass( int x, int y ) const;
   float GetClassConfidence( int x, int y ) const;

   // Apply target context adjustments to selection
   void ApplyTargetContextAdjustments(
      PixelSelectionResult& result,
      RegionClass regionClass,
      const std::vector<float>& values ) const;

   // Apply spatial smoothing in transition zones
   float ApplySpatialSmoothing(
      const std::vector<float>& values,
      const SpatialContext& context,
      float selectedValue,
      int selectedFrame ) const;
};

// ----------------------------------------------------------------------------
// Helper: Known deep sky objects and their characteristics
// ----------------------------------------------------------------------------

namespace KnownObjects
{
   // Returns expected features for common objects
   void GetExpectedFeatures( const String& objectName,
                             bool& emission, bool& dark, bool& galaxy,
                             bool& cluster, bool& planetary );

   // Common object categories
   bool IsMessierEmissionNebula( const String& name );
   bool IsMessierGalaxy( const String& name );
   bool IsNGCEmissionNebula( const String& name );
   bool IsNGCGalaxy( const String& name );
   bool IsPlanetaryNebula( const String& name );
}

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __PixelSelector_h
