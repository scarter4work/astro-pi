//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// Segmentation Engine - High-level interface

#ifndef __Segmentation_h
#define __Segmentation_h

#include "SegmentationModel.h"
#include "RegionAnalyzer.h"

#include <pcl/Image.h>
#include <pcl/String.h>

#include <memory>
#include <map>
#include <functional>
#include <cstdint>

namespace pcl
{

// ----------------------------------------------------------------------------
// Segmentation Engine Configuration
// ----------------------------------------------------------------------------

struct SegmentationEngineConfig
{
   // Model settings
   SegmentationConfig modelConfig;

   // Processing settings
   bool autoFallback = true;           // Fall back to mock if ONNX fails
   bool cacheResults = true;           // Cache segmentation results
   bool runAnalysis = true;            // Run RegionAnalyzer after segmentation

   // Mask post-processing
   double maskSmoothingRadius = 2.0;   // Gaussian smoothing on masks
   double maskDilationRadius = 0.0;    // Expand mask boundaries
   double maskErosionRadius = 0.0;     // Shrink mask boundaries
   double minMaskCoverage = 0.001;     // Minimum coverage to keep a mask (0.1%)

   // Performance settings
   bool downsampleLargeImages = true;  // Downsample for faster processing
   int maxProcessingDimension = 2048;  // Max dimension before downsampling
};

// ----------------------------------------------------------------------------
// Segmentation Engine Event Types
// ----------------------------------------------------------------------------

enum class SegmentationEventType
{
   Started,
   Preprocessing,
   ModelRunning,
   Postprocessing,
   AnalyzingRegions,
   Completed,
   Failed
};

// Progress callback: (eventType, progress 0-1, message)
using SegmentationProgressCallback = std::function<void( SegmentationEventType, double, const String& )>;

// ----------------------------------------------------------------------------
// Complete Segmentation Result
// ----------------------------------------------------------------------------

struct SegmentationEngineResult
{
   // Basic segmentation result (masks and class map)
   SegmentationResult segmentation;

   // Per-region analysis results
   std::map<RegionClass, RegionStatistics> regionStats;

   // Global analysis
   RegionAnalysisResult analysisResult;

   // Metadata
   String modelUsed = String();  // Must be initialized for PCL ABI
   double totalTimeMs = 0;
   double segmentationTimeMs = 0;
   double analysisTimeMs = 0;
   bool usedFallback = false;

   // Status
   bool isValid = false;
   String errorMessage = String();  // Must be initialized for PCL ABI

   // Get statistics for a specific region
   const RegionStatistics* GetRegionStats( RegionClass rc ) const
   {
      auto it = regionStats.find( rc );
      return (it != regionStats.end()) ? &it->second : nullptr;
   }

   // Get sorted list of regions by coverage
   std::vector<std::pair<RegionClass, double>> GetRegionsByCoverage() const;

   // Get dominant region
   RegionClass GetDominantRegion() const;
};

// ----------------------------------------------------------------------------
// Segmentation Engine
//
// High-level interface for image segmentation.
// Manages model lifecycle, caching, and result analysis.
// ----------------------------------------------------------------------------

class SegmentationEngine
{
public:

   SegmentationEngine();
   SegmentationEngine( const SegmentationEngineConfig& config );
   ~SegmentationEngine();

   // Initialize with configuration
   bool Initialize( const SegmentationEngineConfig& config );

   // Check if engine is ready
   bool IsReady() const { return m_model != nullptr && m_model->IsReady(); }

   // Run segmentation on an image
   SegmentationEngineResult Process( const Image& image );

   // Run segmentation with progress callback
   SegmentationEngineResult Process( const Image& image,
                                      SegmentationProgressCallback progressCallback );

   // Get cached result (if available and valid for this image)
   const SegmentationEngineResult* GetCachedResult() const;

   // Clear cache
   void ClearCache();

   // Get individual masks
   const Image* GetMask( RegionClass rc ) const;
   std::map<RegionClass, Image> GetAllMasks() const;

   // Get configuration
   const SegmentationEngineConfig& Config() const { return m_config; }

   // Get model info
   String GetModelName() const;
   String GetModelDescription() const;

   // Get last error
   String GetLastError() const { return m_lastError; }

   // Check ONNX availability
   static bool IsONNXAvailable();

   // Get available model paths (searches common locations)
   static std::vector<String> FindAvailableModels();

private:

   SegmentationEngineConfig m_config;
   std::unique_ptr<ISegmentationModel> m_model;
   String m_lastError;

   // Cached results
   mutable SegmentationEngineResult m_cachedResult;
   mutable uint64_t m_cachedImageHash = 0;

   // Region analyzer for post-segmentation analysis
   std::unique_ptr<RegionAnalyzer> m_analyzer;

   // Progress reporting
   SegmentationProgressCallback m_progressCallback;

   // Report progress
   void ReportProgress( SegmentationEventType event, double progress, const String& message );

   // Preprocess image (resize if needed)
   Image PreprocessImage( const Image& image, double& scale ) const;

   // Postprocess masks (smooth, dilate, etc.)
   void PostprocessMasks( SegmentationResult& result ) const;

   // Run analysis on segmentation result
   void AnalyzeRegions( const Image& image, SegmentationEngineResult& result ) const;

   // Compute simple hash of image for cache validation
   static uint64_t ComputeImageHash( const Image& image );
};

// ----------------------------------------------------------------------------
// Segmentation Visualization
// ----------------------------------------------------------------------------

class SegmentationVisualizer
{
public:

   // Create color-coded visualization of class map
   static Image CreateClassMapVisualization( const SegmentationResult& result );

   // Create overlay of masks on original image
   static Image CreateMaskOverlay( const Image& original,
                                    const SegmentationResult& result,
                                    double opacity = 0.5 );

   // Create individual region visualization
   static Image CreateRegionVisualization( const Image& original,
                                            const Image& mask,
                                            RegionClass rc,
                                            double opacity = 0.5 );

   // Get color for region class
   static void GetRegionColor( RegionClass rc, double& r, double& g, double& b );

   // Create legend image
   static Image CreateLegend( int width, int height );
};

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __Segmentation_h
