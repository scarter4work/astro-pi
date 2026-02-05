//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// Compositor - Simplified processing pipeline (no ML segmentation)

#ifndef __Compositor_h
#define __Compositor_h

#include "RegionStatistics.h"
#include "StretchSelector.h"
#include "BlendEngine.h"
#include "ToneMapper.h"
#include "LRGBProcessor.h"
#include "Segmentation.h"

#include <pcl/Image.h>
#include <pcl/String.h>

#include <memory>
#include <functional>

namespace pcl
{

// ----------------------------------------------------------------------------
// Compositor Configuration (simplified - no segmentation)
// ----------------------------------------------------------------------------

struct CompositorConfig
{
   // Processing modes
   bool useSegmentation = false;        // Disabled - no ML segmentation
   bool useAutoSelection = true;        // Auto-select algorithms
   bool useLRGBMode = true;             // Process L separately
   bool applyToneMapping = true;        // Final tone adjustments

   // Sub-system configurations
   BlendConfig blendConfig;
   ToneMapConfig toneConfig;
   LRGBConfig lrgbConfig;

   // Global parameters
   double globalStrength = 1.0;         // Overall stretch intensity
   double globalContrast = 0.0;         // Global contrast adjustment
   double globalSaturation = 0.0;       // Global saturation adjustment

   // Performance
   int numThreads = 0;                  // 0 = auto
   bool useParallelBlend = true;        // Multi-threaded blending
};

// ----------------------------------------------------------------------------
// Processing Stage
// ----------------------------------------------------------------------------

enum class ProcessingStage
{
   Idle,
   Segmenting,
   Analyzing,
   SelectingAlgorithms,
   Stretching,
   Blending,
   ToneMapping,
   Finalizing,
   Complete,
   Failed
};

// ----------------------------------------------------------------------------
// Compositor Result
// ----------------------------------------------------------------------------

struct CompositorResult
{
   // Output image
   Image outputImage;

   // Intermediate results (optional, for debugging/preview)
   std::unique_ptr<Image> luminanceImage;

   // Selection summary
   SelectionSummary selectionSummary;

   // Timing
   double analysisTimeMs = 0;
   double stretchTimeMs = 0;
   double blendTimeMs = 0;
   double toneMappingTimeMs = 0;
   double totalTimeMs = 0;
   double segmentationTimeMs = 0;  // Always 0 - kept for API compatibility

   // Status
   bool isValid = false;
   String errorMessage = String();
   ProcessingStage stage = ProcessingStage::Idle;
};

// ----------------------------------------------------------------------------
// Progress Callback
// ----------------------------------------------------------------------------

struct CompositorProgress
{
   ProcessingStage stage = ProcessingStage::Idle;
   double progress = 0.0;       // 0-1 within current stage
   double overall = 0.0;        // 0-1 overall progress
   IsoString message;           // Use IsoString for cross-module ABI compatibility
};

using CompositorProgressCallback = std::function<void( const CompositorProgress& )>;

// ----------------------------------------------------------------------------
// Compositor
//
// Simplified processing pipeline:
// - Global image analysis and statistics
// - Algorithm selection based on image characteristics
// - Single-pass stretching (no region-based blending)
// - Tone mapping
// - LRGB processing
// ----------------------------------------------------------------------------

class Compositor
{
public:

   Compositor();
   Compositor( const CompositorConfig& config );
   ~Compositor();

   // Initialize with configuration
   bool Initialize( const CompositorConfig& config );

   // Main processing entry point
   CompositorResult Process( const Image& input );

   // Process with progress callback
   CompositorResult Process( const Image& input, CompositorProgressCallback progress );

   // Access sub-systems
   StretchSelector& Selector() { return *m_selector; }
   BlendEngine& Blender() { return *m_blender; }
   ToneMapper& ToneMap() { return *m_toneMapper; }
   LRGBProcessor& LRGB() { return *m_lrgbProcessor; }

   // Configuration
   const CompositorConfig& Config() const { return m_config; }
   void SetConfig( const CompositorConfig& config );

   // Get last error
   String GetLastError() const { return m_lastError; }

private:

   CompositorConfig m_config;
   String m_lastError;

   // Sub-systems
   std::unique_ptr<StretchSelector> m_selector;
   std::unique_ptr<BlendEngine> m_blender;
   std::unique_ptr<ParallelBlendProcessor> m_parallelBlender;
   std::unique_ptr<ToneMapper> m_toneMapper;
   std::unique_ptr<LRGBProcessor> m_lrgbProcessor;

   // Segmentation (optional)
   std::unique_ptr<SegmentationEngine> m_segmentation;
   SegmentationEngineResult m_segmentationResult;

   // Progress reporting
   CompositorProgressCallback m_progressCallback;
   void ReportProgress( ProcessingStage stage, double progress, const IsoString& message );
   double GetStageWeight( ProcessingStage stage ) const;

   // Processing stages (called by Process)
   void RunSegmentationStage( const Image& input, CompositorResult& result );
   RegionStatistics RunAnalysisStage( const Image& input, CompositorResult& result );
   void RunAlgorithmSelectionStage(
      const Image& input,
      const RegionStatistics& stats,
      CompositorResult& result,
      bool& useRegionAware,
      std::map<RegionClass, SelectedStretch>& regionAlgorithms,
      std::map<RegionClass, Image>& regionMasks );
   Image RunStretchStage(
      const Image& input,
      bool useRegionAware,
      std::map<RegionClass, SelectedStretch>& regionAlgorithms,
      const std::map<RegionClass, Image>& regionMasks,
      CompositorResult& result );
   void RunTransitionStage( Image& stretched, bool useRegionAware );
   Image RunToneMappingStage( const Image& stretched, CompositorResult& result );
   void RunFinalizationStage( Image& toned, CompositorResult& result );

   // Internal helpers
   bool RunSegmentation( const Image& input, double& segmentationTimeMs );
   RegionStatistics AnalyzeImage( const Image& input );
   SelectedStretch SelectAlgorithm( const RegionStatistics& stats );
   Image ApplyStretch( const Image& input, IStretchAlgorithm* algorithm );
   Image ApplyRegionAwareStretch(
      const Image& input,
      const std::map<RegionClass, SelectedStretch>& regionAlgorithms,
      const std::map<RegionClass, Image>& regionMasks );
   Image RunToneMapping( const Image& input );
   Image RunLRGBProcessing( const Image& original, const Image& stretched );
};

// ----------------------------------------------------------------------------
// Quick Processing Functions
// ----------------------------------------------------------------------------

namespace QuickProcess
{
   // One-shot processing with default settings
   Image AutoStretch( const Image& input );

   // Quick stretch with custom global parameters
   Image AutoStretch( const Image& input, double strength, double contrast = 0 );

   // Single algorithm stretch (no segmentation)
   Image SingleStretch( const Image& input, AlgorithmType algorithm );

   // LRGB stretch (stretch luminance only)
   Image LRGBStretch( const Image& input, double strength = 1.0 );
}

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __Compositor_h
