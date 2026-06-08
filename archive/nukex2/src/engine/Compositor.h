//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// Compositor - Full processing pipeline

#ifndef __Compositor_h
#define __Compositor_h

#include "Segmentation.h"
#include "StretchSelector.h"
#include "BlendEngine.h"
#include "ToneMapper.h"
#include "LRGBProcessor.h"

#include <pcl/Image.h>
#include <pcl/String.h>

#include <memory>
#include <functional>

namespace pcl
{

// ----------------------------------------------------------------------------
// Compositor Configuration
// ----------------------------------------------------------------------------

struct CompositorConfig
{
   // Processing modes
   bool useSegmentation = true;         // Use AI segmentation
   bool useAutoSelection = true;        // Auto-select algorithms
   bool useLRGBMode = true;             // Process L separately
   bool applyToneMapping = true;        // Final tone adjustments

   // Sub-system configurations
   SegmentationEngineConfig segmentationConfig;
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
   std::unique_ptr<Image> segmentationPreview;

   // Segmentation result
   SegmentationEngineResult segmentation;

   // Selection summary
   SelectionSummary selectionSummary;

   // Timing
   double segmentationTimeMs = 0;
   double analysisTimeMs = 0;
   double stretchTimeMs = 0;
   double blendTimeMs = 0;
   double toneMappingTimeMs = 0;
   double totalTimeMs = 0;

   // Status
   bool isValid = false;
   String errorMessage = String();  // Must be initialized for PCL ABI
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
   String message = String();   // Must be initialized for PCL ABI
};

using CompositorProgressCallback = std::function<void( const CompositorProgress& )>;

// ----------------------------------------------------------------------------
// Compositor
//
// Main processing pipeline that combines:
// - Segmentation (AI-driven or mock)
// - Region analysis and statistics
// - Algorithm selection
// - Per-region stretching
// - Blending
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

   // Process with external segmentation (skip internal segmentation)
   CompositorResult ProcessWithSegmentation( const Image& input,
                                              const SegmentationResult& segmentation );

   // Process single region (for preview)
   Image ProcessRegion( const Image& input, RegionClass region,
                        const RegionStatistics& stats );

   // Get algorithm selection for preview
   std::map<RegionClass, SelectedStretch> PreviewSelection( const Image& input );

   // Access sub-systems
   SegmentationEngine& Segmentation() { return *m_segmentation; }
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
   std::unique_ptr<SegmentationEngine> m_segmentation;
   std::unique_ptr<RegionAnalyzer> m_analyzer;
   std::unique_ptr<StretchSelector> m_selector;
   std::unique_ptr<BlendEngine> m_blender;
   std::unique_ptr<ParallelBlendProcessor> m_parallelBlender;
   std::unique_ptr<ToneMapper> m_toneMapper;
   std::unique_ptr<LRGBProcessor> m_lrgbProcessor;

   // Progress reporting
   CompositorProgressCallback m_progressCallback;
   void ReportProgress( ProcessingStage stage, double progress, const String& message );
   double GetStageWeight( ProcessingStage stage ) const;

   // Processing stages
   SegmentationEngineResult RunSegmentation( const Image& input );
   RegionAnalysisResult RunAnalysis( const Image& input, const SegmentationResult& seg );
   std::map<RegionClass, SelectedStretch> RunSelection( const RegionAnalysisResult& analysis );
   Image RunStretching( const Image& input, const std::map<RegionClass, Image>& masks,
                        const std::map<RegionClass, SelectedStretch>& selections );
   Image RunBlending( const Image& input, const std::map<RegionClass, Image>& masks,
                       std::map<RegionClass, IStretchAlgorithm*>& algorithms );
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
