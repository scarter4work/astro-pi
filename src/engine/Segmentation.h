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
#include <chrono>
#include <cstdint>
#include <mutex>

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

   // Resolution settings (improved from fixed 2048 limit)
   bool downsampleLargeImages = true;  // Downsample for faster processing
   int maxProcessingDimension = 4096;  // Max dimension before downsampling (increased from 2048)
   bool useAdaptiveResolution = true;  // Use full resolution up to adaptiveThreshold, then scale
   int adaptiveThreshold = 4096;       // Use full resolution below this, proportional above

   // Tiled segmentation for very large images (preserves small features)
   bool useTiledSegmentation = true;   // Use overlapping tiles for large images
   int tileSize = 2048;                // Size of each tile for tiled segmentation
   int tileOverlap = 256;              // Overlap between tiles for smooth blending
   int tiledSegmentationThreshold = 6144; // Use tiled approach above this dimension
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
   [[nodiscard]] SegmentationEngineResult Process( const Image& image );

   // Run segmentation with progress callback
   [[nodiscard]] SegmentationEngineResult Process( const Image& image,
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

   // Get default model path (checks env var, then searches)
   static String GetDefaultModelPath();

private:

   SegmentationEngineConfig m_config;
   std::unique_ptr<ISegmentationModel> m_model;
   String m_lastError;

   // Cached results (protected by mutex for thread safety)
   mutable SegmentationEngineResult m_cachedResult;
   mutable uint64_t m_cachedImageHash = 0;
   mutable std::mutex m_cacheMutex;

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

   // Tiled segmentation for large images (preserves small features)
   SegmentationResult ProcessTiled( const Image& image );

   // Merge overlapping tile results with blending
   void MergeTileResult( SegmentationResult& target, const SegmentationResult& tile,
                         int tileX, int tileY, int tileWidth, int tileHeight,
                         int overlapLeft, int overlapTop, int overlapRight, int overlapBottom );

   // Upscale masks with edge-aware method (better for class boundaries)
   Image UpscaleMaskEdgeAware( const Image& mask, int targetWidth, int targetHeight,
                                double scale ) const;

   // Run postprocessing steps (mask cleanup, region analysis, caching, timing)
   void RunPostprocessing( SegmentationEngineResult& result, const Image& image,
                           const std::chrono::high_resolution_clock::time_point& totalStart );
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
