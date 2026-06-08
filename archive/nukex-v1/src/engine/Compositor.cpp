//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// Compositor - Processing pipeline with optional ML segmentation

#include "Compositor.h"
#include "Constants.h"
#include "HistogramEngine.h"
#include "StretchLibrary.h"
#include "TransitionChecker.h"

#include <pcl/Console.h>
#include <pcl/File.h>

#include <chrono>
#include <map>
#include <algorithm>

namespace pcl
{

// Forward declarations for helper functions
static std::map<RegionClass, double> ComputeRegionCoverage(
   const SegmentationEngineResult& segResult,
   double minCoverage );

static std::map<RegionClass, Image> ExtractRegionMasks(
   const SegmentationEngineResult& segResult,
   const std::map<RegionClass, double>& coverage );

// ----------------------------------------------------------------------------
// Compositor Implementation
// ----------------------------------------------------------------------------

Compositor::Compositor()
{
   Initialize( CompositorConfig() );
}

// ----------------------------------------------------------------------------

Compositor::Compositor( const CompositorConfig& config )
{
   Initialize( config );
}

// ----------------------------------------------------------------------------

Compositor::~Compositor()
{
}

// ----------------------------------------------------------------------------

bool Compositor::Initialize( const CompositorConfig& config )
{
   m_config = config;

   // Initialize selector
   m_selector = std::make_unique<StretchSelector>();
   m_selector->Initialize();
   m_selector->SetGlobalStrength( config.globalStrength );

   // Initialize blend engine
   m_blender = std::make_unique<BlendEngine>( config.blendConfig );
   m_parallelBlender = std::make_unique<ParallelBlendProcessor>( config.blendConfig );

   // Initialize tone mapper
   m_toneMapper = std::make_unique<ToneMapper>( config.toneConfig );

   // Initialize LRGB processor
   m_lrgbProcessor = std::make_unique<LRGBProcessor>( config.lrgbConfig );

   return true;
}

// ----------------------------------------------------------------------------

void Compositor::SetConfig( const CompositorConfig& config )
{
   m_config = config;

   m_blender->SetConfig( config.blendConfig );
   m_toneMapper->SetConfig( config.toneConfig );
   m_lrgbProcessor->SetConfig( config.lrgbConfig );
   m_selector->SetGlobalStrength( config.globalStrength );
}

// ----------------------------------------------------------------------------

void Compositor::ReportProgress( ProcessingStage stage, double progress, const IsoString& message )
{
   if ( !m_progressCallback )
      return;

   CompositorProgress p;
   p.stage = stage;
   p.progress = progress;
   p.message = message;

   // Calculate overall progress based on stage weights
   double baseProgress = 0;
   for ( int i = 0; i < static_cast<int>( stage ); ++i )
   {
      baseProgress += GetStageWeight( static_cast<ProcessingStage>( i ) );
   }

   p.overall = baseProgress + progress * GetStageWeight( stage );

   m_progressCallback( p );
}

// ----------------------------------------------------------------------------

double Compositor::GetStageWeight( ProcessingStage stage ) const
{
   // Relative weights for each stage (should sum to 1.0)
   // Weights adjusted when segmentation is enabled
   if ( m_config.useSegmentation )
   {
      switch ( stage )
      {
      case ProcessingStage::Segmenting:         return 0.20;
      case ProcessingStage::Analyzing:          return 0.10;
      case ProcessingStage::SelectingAlgorithms: return 0.05;
      case ProcessingStage::Stretching:         return 0.40;
      case ProcessingStage::ToneMapping:        return 0.15;
      case ProcessingStage::Finalizing:         return 0.10;
      default:                                   return 0;
      }
   }
   else
   {
      switch ( stage )
      {
      case ProcessingStage::Analyzing:          return 0.15;
      case ProcessingStage::SelectingAlgorithms: return 0.05;
      case ProcessingStage::Stretching:         return 0.50;
      case ProcessingStage::ToneMapping:        return 0.20;
      case ProcessingStage::Finalizing:         return 0.10;
      default:                                   return 0;
      }
   }
}

// ----------------------------------------------------------------------------

CompositorResult Compositor::Process( const Image& input )
{
   return Process( input, nullptr );
}

// ----------------------------------------------------------------------------

CompositorResult Compositor::Process( const Image& input, CompositorProgressCallback progress )
{
   m_progressCallback = progress;

   auto totalStart = std::chrono::high_resolution_clock::now();

   CompositorResult result;
   result.stage = ProcessingStage::Idle;

   try
   {
      // Stage 0: Segmentation (if enabled)
      RunSegmentationStage( input, result );

      // Stage 1: Analysis
      RegionStatistics stats = RunAnalysisStage( input, result );

      // Stage 2: Algorithm Selection
      bool useRegionAware = false;
      std::map<RegionClass, SelectedStretch> regionAlgorithms;
      std::map<RegionClass, Image> regionMasks;
      RunAlgorithmSelectionStage( input, stats, result,
                                  useRegionAware, regionAlgorithms, regionMasks );

      // Stage 3: Stretching
      Image stretched = RunStretchStage( input, useRegionAware,
                                          regionAlgorithms, regionMasks, result );

      // Post-stretch transition smoothing
      RunTransitionStage( stretched, useRegionAware );

      // Stage 4: Tone Mapping
      Image toned = RunToneMappingStage( stretched, result );

      // Stage 5: Finalization
      RunFinalizationStage( toned, result );
   }
   catch ( const std::exception& e )
   {
      result.errorMessage = String( "Exception: " ) + e.what();
      result.stage = ProcessingStage::Failed;
   }

   auto totalEnd = std::chrono::high_resolution_clock::now();
   result.totalTimeMs = std::chrono::duration<double, std::milli>(
      totalEnd - totalStart ).count();

   return result;
}

// ----------------------------------------------------------------------------

void Compositor::RunSegmentationStage( const Image& input, CompositorResult& result )
{
   if ( m_config.useSegmentation )
   {
      result.stage = ProcessingStage::Segmenting;
      ReportProgress( ProcessingStage::Segmenting, 0.0, "Running segmentation" );

      double segTimeMs = 0;
      bool segSuccess = RunSegmentation( input, segTimeMs );
      result.segmentationTimeMs = segTimeMs;

      if ( segSuccess )
      {
         ReportProgress( ProcessingStage::Segmenting, 1.0, "Segmentation complete" );
      }
      else
      {
         ReportProgress( ProcessingStage::Segmenting, 1.0, "Segmentation skipped" );
      }
   }
}

// ----------------------------------------------------------------------------

RegionStatistics Compositor::RunAnalysisStage( const Image& input, CompositorResult& result )
{
   result.stage = ProcessingStage::Analyzing;
   ReportProgress( ProcessingStage::Analyzing, 0.0, "Analyzing image" );

   auto analysisStart = std::chrono::high_resolution_clock::now();
   RegionStatistics stats = AnalyzeImage( input );
   auto analysisEnd = std::chrono::high_resolution_clock::now();
   result.analysisTimeMs = std::chrono::duration<double, std::milli>(
      analysisEnd - analysisStart ).count();

   ReportProgress( ProcessingStage::Analyzing, 1.0, "Analysis complete" );

   return stats;
}

// ----------------------------------------------------------------------------

void Compositor::RunAlgorithmSelectionStage(
   const Image& input,
   const RegionStatistics& stats,
   CompositorResult& result,
   bool& useRegionAware,
   std::map<RegionClass, SelectedStretch>& regionAlgorithms,
   std::map<RegionClass, Image>& regionMasks )
{
   result.stage = ProcessingStage::SelectingAlgorithms;
   ReportProgress( ProcessingStage::SelectingAlgorithms, 0.0, "Selecting algorithm" );

   // Check if we have valid segmentation results for region-aware processing
   useRegionAware = m_config.useSegmentation && m_segmentationResult.isValid;

   // Maps for region-aware processing
   std::map<RegionClass, double> regionCoverage;

   if ( useRegionAware )
   {
      Console console;
      console.WriteLn( "<br>Region-aware algorithm selection:" );

      // Get regions with significant coverage (>1%)
      regionCoverage = ComputeRegionCoverage( m_segmentationResult, 0.01 );

      // Extract masks for significant regions
      regionMasks = ExtractRegionMasks( m_segmentationResult, regionCoverage );

      // For each significant region, compute statistics and select algorithm
      HistogramEngine histEngine;
      for ( const auto& coveragePair : regionCoverage )
      {
         RegionClass rc = coveragePair.first;
         double coverage = coveragePair.second;

         // Get mask for this region
         auto maskIt = regionMasks.find( rc );
         if ( maskIt == regionMasks.end() )
            continue;

         // Scale mask to match input image dimensions if necessary
         const Image& segMask = maskIt->second;
         int maskWidth = segMask.Width();
         int maskHeight = segMask.Height();
         int imgWidth = input.Width();
         int imgHeight = input.Height();

         // Compute LOCAL statistics for this region using the mask
         // This is CRITICAL - we must use the actual pixel values within the masked region
         RegionStatistics regionStats;

         if ( maskWidth == imgWidth && maskHeight == imgHeight )
         {
            // Mask is already at image resolution - use directly
            regionStats = histEngine.ComputeStatistics( input, segMask, 0, 0 );
         }
         else
         {
            // Scale mask to image resolution using bilinear interpolation
            Image scaledMask( imgWidth, imgHeight, ColorSpace::Gray );
            double scaleX = static_cast<double>( maskWidth ) / imgWidth;
            double scaleY = static_cast<double>( maskHeight ) / imgHeight;

            for ( int y = 0; y < imgHeight; ++y )
            {
               for ( int x = 0; x < imgWidth; ++x )
               {
                  double sx = x * scaleX;
                  double sy = y * scaleY;
                  int x0 = static_cast<int>( sx );
                  int y0 = static_cast<int>( sy );
                  int x1 = std::min( x0 + 1, maskWidth - 1 );
                  int y1 = std::min( y0 + 1, maskHeight - 1 );
                  double fx = sx - x0;
                  double fy = sy - y0;

                  double v00 = segMask( x0, y0, 0 );
                  double v10 = segMask( x1, y0, 0 );
                  double v01 = segMask( x0, y1, 0 );
                  double v11 = segMask( x1, y1, 0 );

                  double value = Interpolation::BilinearInterpolate( v00, v10, v01, v11, fx, fy );

                  // Apply threshold to exclude blended edge pixels from stats
                  // Only use pixels where mask > 0.5 for cleaner region statistics
                  scaledMask( x, y, 0 ) = (value > 0.5) ? value : 0.0;
               }
            }

            regionStats = histEngine.ComputeStatistics( input, scaledMask, 0, 0 );
         }

         // Set the region metadata
         regionStats.regionClass = rc;
         regionStats.maskCoverage = coverage;

         // Log the local statistics for debugging
         console.WriteLn( String().Format( "  %s local stats: median=%.4f, stdDev=%.4f, SNR=%.1f",
            IsoString( RegionClassDisplayName( rc ) ).c_str(),
            regionStats.median,
            regionStats.stdDev,
            regionStats.snrEstimate ) );

         // Select algorithm for this region based on LOCAL statistics
         SelectedStretch selection = m_selector->Select( rc, regionStats );

         // Log the selection
         IsoString regionName = IsoString( RegionClassDisplayName( rc ) );
         IsoString algoName = IsoString( StretchLibrary::TypeToName( selection.algorithm ) );
         console.WriteLn( String().Format( "  %s (%.1f%%): %s (confidence: %.0f%%)",
            regionName.c_str(),
            coverage * 100.0,
            algoName.c_str(),
            selection.confidence * 100.0 ) );

         // Add to selection summary
         SelectionSummary::RegionEntry entry;
         entry.region = rc;
         entry.algorithm = selection.algorithm;
         entry.confidence = selection.confidence;
         entry.coverage = coverage;
         entry.rationale = selection.rationale;
         result.selectionSummary.entries.push_back( entry );

         // Store the selection (need to move the unique_ptr)
         regionAlgorithms.emplace( rc, std::move( selection ) );
      }

      console.WriteLn( String().Format( "<br>Selected algorithms for %d regions",
         static_cast<int>( regionAlgorithms.size() ) ) );
   }
   else
   {
      // Fall back to single algorithm for entire image
      SelectedStretch selection = SelectAlgorithm( stats );

      // Create selection summary with single entry
      SelectionSummary::RegionEntry entry;
      entry.region = RegionClass::Background;
      entry.algorithm = selection.algorithm;
      entry.confidence = selection.confidence;
      entry.coverage = 1.0;
      entry.rationale = selection.rationale;
      result.selectionSummary.entries.push_back( entry );

      regionAlgorithms.emplace( RegionClass::Background, std::move( selection ) );
   }

   ReportProgress( ProcessingStage::SelectingAlgorithms, 1.0, "Algorithm selected" );
}

// ----------------------------------------------------------------------------

Image Compositor::RunStretchStage(
   const Image& input,
   bool useRegionAware,
   std::map<RegionClass, SelectedStretch>& regionAlgorithms,
   const std::map<RegionClass, Image>& regionMasks,
   CompositorResult& result )
{
   result.stage = ProcessingStage::Stretching;
   ReportProgress( ProcessingStage::Stretching, 0.0, "Applying stretch" );

   auto stretchStart = std::chrono::high_resolution_clock::now();

   Image stretched;
   if ( m_config.useLRGBMode && input.NumberOfNominalChannels() >= 3 )
   {
      // Extract luminance
      Image luminance = m_lrgbProcessor->ExtractLuminance( input );

      // Apply region-aware or single stretch to luminance
      Image stretchedL;
      if ( useRegionAware && regionAlgorithms.size() > 1 )
      {
         stretchedL = ApplyRegionAwareStretch( luminance, regionAlgorithms, regionMasks );
      }
      else
      {
         // Use first (and only) algorithm
         auto it = regionAlgorithms.begin();
         stretchedL = ApplyStretch( luminance, it->second.algorithmInstance.get() );
      }

      // Recombine with color
      stretched = m_lrgbProcessor->ApplyStretchedLuminance( input, stretchedL );

      // Store for debugging
      result.luminanceImage = std::make_unique<Image>( stretchedL );
   }
   else
   {
      // Apply region-aware or single stretch directly
      if ( useRegionAware && regionAlgorithms.size() > 1 )
      {
         stretched = ApplyRegionAwareStretch( input, regionAlgorithms, regionMasks );
      }
      else
      {
         // Use first (and only) algorithm
         auto it = regionAlgorithms.begin();
         stretched = ApplyStretch( input, it->second.algorithmInstance.get() );
      }
   }

   auto stretchEnd = std::chrono::high_resolution_clock::now();
   result.stretchTimeMs = std::chrono::duration<double, std::milli>(
      stretchEnd - stretchStart ).count();

   ReportProgress( ProcessingStage::Stretching, 1.0, "Stretch complete" );

   return stretched;
}

// ----------------------------------------------------------------------------

void Compositor::RunTransitionStage( Image& stretched, bool useRegionAware )
{
   // Post-stretch transition smoothing
   // Check for hard transitions at region boundaries and smooth if needed
   TransitionChecker transitionChecker;
   int totalSmoothed = 0;
   int numChannels = stretched.NumberOfNominalChannels();

   // Process each channel
   for ( int c = 0; c < numChannels; ++c )
   {
      int numSmoothed;
      if ( useRegionAware && !m_segmentationResult.segmentation.classMap.IsEmpty() )
      {
         // Use segmentation-aware smoothing to preserve real feature boundaries
         numSmoothed = transitionChecker.CheckAndSmooth(
            stretched, m_segmentationResult.segmentation.classMap, c );
      }
      else
      {
         numSmoothed = transitionChecker.CheckAndSmooth( stretched, c );
      }
      totalSmoothed += numSmoothed;
   }

   if ( totalSmoothed > 0 )
   {
      Console().WriteLn( String().Format( "  Smoothed %d hard transitions across %d channel(s)",
         totalSmoothed, numChannels ) );
   }
}

// ----------------------------------------------------------------------------

Image Compositor::RunToneMappingStage( const Image& stretched, CompositorResult& result )
{
   result.stage = ProcessingStage::ToneMapping;
   Image toned = stretched;

   if ( m_config.applyToneMapping )
   {
      ReportProgress( ProcessingStage::ToneMapping, 0.0, "Applying tone mapping" );

      auto toneStart = std::chrono::high_resolution_clock::now();
      toned = RunToneMapping( stretched );
      auto toneEnd = std::chrono::high_resolution_clock::now();
      result.toneMappingTimeMs = std::chrono::duration<double, std::milli>(
         toneEnd - toneStart ).count();

      ReportProgress( ProcessingStage::ToneMapping, 1.0, "Tone mapping complete" );
   }

   return toned;
}

// ----------------------------------------------------------------------------

void Compositor::RunFinalizationStage( Image& toned, CompositorResult& result )
{
   result.stage = ProcessingStage::Finalizing;
   ReportProgress( ProcessingStage::Finalizing, 0.0, "Finalizing" );

   // Apply global saturation adjustment
   if ( m_config.useLRGBMode && std::abs( m_config.globalSaturation ) > 0.001 )
   {
      m_lrgbProcessor->ApplySaturationBoost( toned, m_config.globalSaturation );
   }

   result.outputImage = std::move( toned );

   ReportProgress( ProcessingStage::Finalizing, 1.0, "Complete" );

   // Done
   result.stage = ProcessingStage::Complete;
   result.isValid = true;
}

// ----------------------------------------------------------------------------

RegionStatistics Compositor::AnalyzeImage( const Image& input )
{
   RegionStatistics stats;
   stats.regionClass = RegionClass::Background;

   // Use histogram engine for fast statistics
   HistogramEngine histEngine;
   auto hist = histEngine.ComputeHistogram( input );

   // Compute basic statistics
   stats.mean = hist.Mean();
   stats.median = hist.Median();
   stats.stdDev = hist.StdDev();
   stats.min = 0.0;
   stats.max = 1.0;

   // Find actual min/max from histogram
   const auto& data = hist.Data();
   for ( int i = 0; i < static_cast<int>( data.size() ); ++i )
   {
      if ( data[i] > 0 )
      {
         stats.min = static_cast<double>( i ) / ( data.size() - 1 );
         break;
      }
   }
   for ( int i = static_cast<int>( data.size() ) - 1; i >= 0; --i )
   {
      if ( data[i] > 0 )
      {
         stats.max = static_cast<double>( i ) / ( data.size() - 1 );
         break;
      }
   }

   // Compute percentiles
   stats.p01 = hist.Percentile( 0.01 );
   stats.p05 = hist.Percentile( 0.05 );
   stats.p10 = hist.Percentile( 0.10 );
   stats.p25 = hist.Percentile( 0.25 );
   stats.p75 = hist.Percentile( 0.75 );
   stats.p90 = hist.Percentile( 0.90 );
   stats.p95 = hist.Percentile( 0.95 );
   stats.p99 = hist.Percentile( 0.99 );

   // Histogram shape
   stats.skewness = hist.Skewness();
   stats.kurtosis = hist.Kurtosis();
   stats.mode = hist.Mode();

   // Estimate dynamic range
   if ( stats.min > 0 )
   {
      stats.dynamicRange = std::log10( stats.max / stats.min );
      stats.dynamicRangeDB = 20.0 * stats.dynamicRange;
   }

   // Clipping analysis
   stats.clippedLowPct = hist.ClippedLowPercent();
   stats.clippedHighPct = hist.ClippedHighPercent();
   stats.clippingPct = stats.clippedLowPct + stats.clippedHighPct;

   // Simple SNR estimate
   double signal = stats.median - stats.p05;
   double noise = stats.stdDev;
   stats.snrEstimate = (noise > 0) ? signal / noise : 100.0;
   stats.noiseEstimate = noise;
   stats.signalEstimate = signal;

   // Pixel count
   stats.pixelCount = static_cast<size_t>( input.Width() ) * input.Height();
   stats.maskCoverage = 1.0;

   return stats;
}

// ----------------------------------------------------------------------------

SelectedStretch Compositor::SelectAlgorithm( const RegionStatistics& stats )
{
   // Use selector with Background region (whole image)
   return m_selector->Select( RegionClass::Background, stats );
}

// ----------------------------------------------------------------------------

Image Compositor::ApplyStretch( const Image& input, IStretchAlgorithm* algorithm )
{
   if ( !algorithm )
   {
      return input;
   }

   int width = input.Width();
   int height = input.Height();
   int numChannels = input.NumberOfNominalChannels();

   // Build a lookup table for the transfer function
   // 65536 entries gives <0.002% interpolation error
   static constexpr int LUT_SIZE = 65536;
   std::vector<float> lut( LUT_SIZE );
   for ( int i = 0; i < LUT_SIZE; ++i )
      lut[i] = algorithm->Apply( static_cast<float>( i ) / ( LUT_SIZE - 1 ) );

   Image output( width, height, input.ColorSpace() );

   for ( int c = 0; c < numChannels; ++c )
   {
      for ( int y = 0; y < height; ++y )
      {
         for ( int x = 0; x < width; ++x )
         {
            float v = input( x, y, c );
            // Clamp and lookup with linear interpolation
            v = std::max( 0.0f, std::min( 1.0f, v ) );
            float fidx = v * ( LUT_SIZE - 1 );
            int idx0 = static_cast<int>( fidx );
            int idx1 = std::min( idx0 + 1, LUT_SIZE - 1 );
            float frac = fidx - idx0;
            output( x, y, c ) = lut[idx0] * ( 1.0f - frac ) + lut[idx1] * frac;
         }
      }
   }

   return output;
}

// ----------------------------------------------------------------------------

bool Compositor::RunSegmentation( const Image& input, double& segmentationTimeMs )
{
   Console console;
   segmentationTimeMs = 0;

   // Get the model path - look in standard locations
   String modelPath = SegmentationEngine::GetDefaultModelPath();

   if ( modelPath.IsEmpty() )
   {
      // Try standard installed location
      modelPath = "/opt/PixInsight/bin/nukex_segmentation.onnx";
   }

   if ( !File::Exists( modelPath ) )
   {
      console.WarningLn( String().Format( "ONNX model not found at: %s", IsoString( modelPath ).c_str() ) );
      console.WarningLn( "Segmentation requires an ONNX model. Install nukex_segmentation.onnx to enable." );
      return false;
   }

   console.WriteLn( String().Format( "<br>Loading segmentation model: %s", IsoString( modelPath ).c_str() ) );

   // Configure the segmentation engine
   SegmentationEngineConfig engineConfig;
   engineConfig.modelConfig.modelPath = modelPath;
   engineConfig.modelConfig.inputWidth = 512;
   engineConfig.modelConfig.inputHeight = 512;
   engineConfig.modelConfig.useGPU = false;  // CPU for now
   engineConfig.cacheResults = false;        // No caching needed
   engineConfig.runAnalysis = true;          // Run region analysis for statistics
   engineConfig.downsampleLargeImages = true;
   engineConfig.maxProcessingDimension = 1024;  // Reasonable for segmentation

   // Create and initialize engine
   m_segmentation = std::make_unique<SegmentationEngine>();
   if ( !m_segmentation->Initialize( engineConfig ) )
   {
      console.WarningLn( String().Format( "Failed to initialize segmentation engine: %s",
         IsoString( m_segmentation->GetLastError() ).c_str() ) );
      m_segmentation.reset();
      return false;
   }

   if ( !m_segmentation->IsReady() )
   {
      console.WarningLn( "Segmentation engine not ready" );
      m_segmentation.reset();
      return false;
   }

   console.WriteLn( String().Format( "Using model: %s", IsoString( m_segmentation->GetModelName() ).c_str() ) );

   // Run segmentation with progress reporting
   auto progressCallback = []( SegmentationEventType event, double progress, const String& message )
   {
      Console console;
      switch ( event )
      {
      case SegmentationEventType::Started:
         console.WriteLn( "  Starting segmentation..." );
         break;
      case SegmentationEventType::Preprocessing:
         console.WriteLn( "  Preprocessing image..." );
         break;
      case SegmentationEventType::ModelRunning:
         console.WriteLn( "  Running neural network..." );
         break;
      case SegmentationEventType::Postprocessing:
         console.WriteLn( "  Postprocessing masks..." );
         break;
      case SegmentationEventType::AnalyzingRegions:
         console.WriteLn( "  Analyzing regions..." );
         break;
      case SegmentationEventType::Completed:
         console.WriteLn( String().Format( "  %s", IsoString( message ).c_str() ) );
         break;
      case SegmentationEventType::Failed:
         console.WarningLn( String().Format( "  Segmentation failed: %s", IsoString( message ).c_str() ) );
         break;
      default:
         break;
      }
   };

   m_segmentationResult = m_segmentation->Process( input, progressCallback );

   if ( !m_segmentationResult.isValid )
   {
      console.WarningLn( String().Format( "Segmentation failed: %s", IsoString( m_segmentationResult.errorMessage ).c_str() ) );
      return false;
   }

   segmentationTimeMs = m_segmentationResult.totalTimeMs;

   // Report class distribution
   int width = m_segmentationResult.segmentation.width;
   int height = m_segmentationResult.segmentation.height;

   // Count pixels per class from the class map
   std::map<int, int> classCounts;
   const float* classData = m_segmentationResult.segmentation.classMap.PixelData( 0 );

   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         size_t idx = static_cast<size_t>( y ) * width + x;
         int numClasses = static_cast<int>( RegionClass::Count );
         int classIdx = static_cast<int>( classData[idx] * ( numClasses - 1 ) + 0.5f );
         classIdx = std::max( 0, std::min( numClasses - 1, classIdx ) );
         classCounts[classIdx]++;
      }
   }

   console.WriteLn( "<br>Segmentation class distribution:" );
   for ( const auto& pair : classCounts )
   {
      double percentage = 100.0 * pair.second / ( width * height );
      if ( percentage > 0.5 )  // Only report classes with > 0.5% coverage
      {
         RegionClass rc = static_cast<RegionClass>( pair.first );
         // Use IsoString for region name to ensure proper UTF-8 encoding with %s
         IsoString regionName = IsoString( RegionClassDisplayName( rc ) );
         console.WriteLn( String().Format( "  %s: %.1f%%",
            regionName.c_str(), percentage ) );
      }
   }

   // Report dominant region
   RegionClass dominant = m_segmentationResult.GetDominantRegion();
   IsoString dominantName = IsoString( RegionClassDisplayName( dominant ) );
   console.WriteLn( String().Format( "<br>Dominant region: %s", dominantName.c_str() ) );

   return true;
}

// ----------------------------------------------------------------------------

Image Compositor::RunToneMapping( const Image& input )
{
   // Auto-detect black/white points if configured
   if ( m_config.toneConfig.autoBlackPoint || m_config.toneConfig.autoWhitePoint )
   {
      m_toneMapper->AutoDetectPoints( input );
   }

   // Apply global contrast
   if ( std::abs( m_config.globalContrast ) > 0.001 )
   {
      ToneMapConfig config = m_toneMapper->Config();
      config.contrast = m_config.globalContrast;
      m_toneMapper->SetConfig( config );
   }

   // Build LUT for fast processing
   m_toneMapper->BuildLUT();

   return m_toneMapper->Apply( input );
}

// ----------------------------------------------------------------------------

Image Compositor::RunLRGBProcessing( const Image& original, const Image& stretched )
{
   return m_lrgbProcessor->ApplyStretchedLuminance( original, stretched );
}

// ----------------------------------------------------------------------------
// QuickProcess Implementation
// ----------------------------------------------------------------------------

namespace QuickProcess
{

Image AutoStretch( const Image& input )
{
   Compositor compositor;
   auto result = compositor.Process( input );

   if ( result.isValid )
   {
      return result.outputImage;
   }

   Console().CriticalLn( "AutoStretch failed: " + result.errorMessage );
   return input;
}

// ----------------------------------------------------------------------------

Image AutoStretch( const Image& input, double strength, double contrast )
{
   CompositorConfig config;
   config.globalStrength = strength;
   config.globalContrast = contrast;

   Compositor compositor( config );
   auto result = compositor.Process( input );

   if ( result.isValid )
   {
      return result.outputImage;
   }

   return input;
}

// ----------------------------------------------------------------------------

Image SingleStretch( const Image& input, AlgorithmType algorithm )
{
   auto stretchAlgo = StretchLibrary::Instance().Create( algorithm );
   if ( !stretchAlgo )
   {
      return input;
   }

   // Compute global stats for auto-configuration
   HistogramEngine histEngine;
   auto hist = histEngine.ComputeHistogram( input );

   RegionStatistics stats;
   stats.mean = hist.Mean();
   stats.median = hist.Median();
   stats.stdDev = hist.StdDev();
   stats.p05 = hist.Percentile( 0.05 );
   stats.p95 = hist.Percentile( 0.95 );

   stretchAlgo->AutoConfigure( stats );

   // Apply to entire image
   int width = input.Width();
   int height = input.Height();
   int numChannels = input.NumberOfNominalChannels();

   Image output( width, height, input.ColorSpace() );

   for ( int c = 0; c < numChannels; ++c )
   {
      for ( int y = 0; y < height; ++y )
      {
         for ( int x = 0; x < width; ++x )
         {
            output( x, y, c ) = stretchAlgo->Apply( input( x, y, c ) );
         }
      }
   }

   return output;
}

// ----------------------------------------------------------------------------

Image LRGBStretch( const Image& input, double strength )
{
   if ( input.NumberOfNominalChannels() < 3 )
   {
      // Grayscale - just do regular stretch
      return AutoStretch( input, strength );
   }

   CompositorConfig config;
   config.useLRGBMode = true;
   config.globalStrength = strength;
   config.useSegmentation = false;

   Compositor compositor( config );
   auto result = compositor.Process( input );

   if ( result.isValid )
   {
      return result.outputImage;
   }

   return input;
}

} // namespace QuickProcess

// ----------------------------------------------------------------------------
// Region-Aware Stretch Implementation
// ----------------------------------------------------------------------------

Image Compositor::ApplyRegionAwareStretch(
   const Image& input,
   const std::map<RegionClass, SelectedStretch>& regionAlgorithms,
   const std::map<RegionClass, Image>& regionMasks )
{
   Console console;
   console.WriteLn( "  Applying region-aware stretch with soft mask blending..." );

   int width = input.Width();
   int height = input.Height();

   // Get dimensions from segmentation result for mask scaling
   int maskWidth = m_segmentationResult.segmentation.width;
   int maskHeight = m_segmentationResult.segmentation.height;

   // Build a map of raw pointers to algorithms for BlendEngine
   std::map<RegionClass, IStretchAlgorithm*> algoPointers;
   for ( const auto& pair : regionAlgorithms )
   {
      algoPointers[pair.first] = pair.second.algorithmInstance.get();
   }

   // Check if masks need to be scaled to image resolution
   std::map<RegionClass, Image> scaledMasks;
   bool needsScaling = ( maskWidth != width || maskHeight != height );

   if ( needsScaling )
   {
      console.WriteLn( String().Format( "  Scaling masks from %dx%d to %dx%d...",
         maskWidth, maskHeight, width, height ) );

      // Scale each mask to match input image dimensions using edge-preserving interpolation
      for ( const auto& maskPair : regionMasks )
      {
         const Image& srcMask = maskPair.second;
         Image dstMask( width, height, ColorSpace::Gray );

         double scaleX = static_cast<double>( srcMask.Width() ) / width;
         double scaleY = static_cast<double>( srcMask.Height() ) / height;

         for ( int y = 0; y < height; ++y )
         {
            for ( int x = 0; x < width; ++x )
            {
               // Map to source coordinates
               double sx = x * scaleX;
               double sy = y * scaleY;

               // Edge-preserving interpolation for segmentation masks
               // Use nearest-neighbor at edges (high gradient), bilinear in smooth regions
               int x0 = static_cast<int>( sx );
               int y0 = static_cast<int>( sy );
               int x1 = std::min( x0 + 1, srcMask.Width() - 1 );
               int y1 = std::min( y0 + 1, srcMask.Height() - 1 );

               double v00 = srcMask( x0, y0, 0 );
               double v10 = srcMask( x1, y0, 0 );
               double v01 = srcMask( x0, y1, 0 );
               double v11 = srcMask( x1, y1, 0 );

               // Compute local gradient (edge detection)
               double gradX = std::abs( v10 - v00 ) + std::abs( v11 - v01 );
               double gradY = std::abs( v01 - v00 ) + std::abs( v11 - v10 );
               double gradient = (gradX + gradY) * 0.25;  // Average gradient magnitude

               // Edge threshold - if gradient is high, we're at a mask boundary
               const double edgeThreshold = 0.3;  // Tunable: lower = more nearest-neighbor

               double value;
               if ( gradient > edgeThreshold )
               {
                  // At edge: use nearest-neighbor to preserve hard boundary
                  // Pick the value closest to the sample point
                  double fx = sx - x0;
                  double fy = sy - y0;
                  if ( fx < 0.5 )
                     value = (fy < 0.5) ? v00 : v01;
                  else
                     value = (fy < 0.5) ? v10 : v11;
               }
               else
               {
                  // Smooth region: use bilinear for anti-aliasing
                  double fx = sx - x0;
                  double fy = sy - y0;
                  value = Interpolation::BilinearInterpolate( v00, v10, v01, v11, fx, fy );
               }

               dstMask( x, y, 0 ) = value;
            }
         }

         scaledMasks[maskPair.first] = std::move( dstMask );
      }
   }
   else
   {
      // Masks are already at the right resolution, just copy references
      scaledMasks = regionMasks;
   }

   // Use BlendEngine for soft mask blending with feathering
   // This applies Gaussian feathering to mask edges and normalizes weights
   console.WriteLn( String().Format( "  Blending %d regions with feathered masks (radius=%.1f)...",
      static_cast<int>( scaledMasks.size() ), m_config.blendConfig.featherRadius ) );

   Image output = m_blender->BlendWithAlgorithms( input, scaledMasks, algoPointers );

   console.WriteLn( "  Region-aware stretch with soft blending complete." );
   return output;
}

// ----------------------------------------------------------------------------
// Helper Functions for Region Coverage and Mask Extraction
// ----------------------------------------------------------------------------

static std::map<RegionClass, double> ComputeRegionCoverage(
   const SegmentationEngineResult& segResult,
   double minCoverage )
{
   std::map<RegionClass, double> coverage;

   int width = segResult.segmentation.width;
   int height = segResult.segmentation.height;
   size_t totalPixels = static_cast<size_t>( width ) * height;

   if ( totalPixels == 0 )
      return coverage;

   // Count pixels per class from the class map
   std::map<int, size_t> classCounts;
   const float* classData = segResult.segmentation.classMap.PixelData( 0 );
   int numClasses = static_cast<int>( RegionClass::Count );

   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         size_t idx = static_cast<size_t>( y ) * width + x;
         int classIdx = static_cast<int>( classData[idx] * ( numClasses - 1 ) + 0.5f );
         classIdx = std::max( 0, std::min( numClasses - 1, classIdx ) );
         classCounts[classIdx]++;
      }
   }

   // Convert counts to coverage percentages and filter by minimum
   for ( const auto& pair : classCounts )
   {
      double pct = static_cast<double>( pair.second ) / totalPixels;
      if ( pct >= minCoverage )
      {
         RegionClass rc = static_cast<RegionClass>( pair.first );
         coverage[rc] = pct;
      }
   }

   return coverage;
}

// ----------------------------------------------------------------------------

static std::map<RegionClass, Image> ExtractRegionMasks(
   const SegmentationEngineResult& segResult,
   const std::map<RegionClass, double>& coverage )
{
   std::map<RegionClass, Image> masks;

   int width = segResult.segmentation.width;
   int height = segResult.segmentation.height;

   if ( width == 0 || height == 0 )
      return masks;

   const float* classData = segResult.segmentation.classMap.PixelData( 0 );
   int numClasses = static_cast<int>( RegionClass::Count );

   // Create a binary mask for each region with significant coverage
   for ( const auto& pair : coverage )
   {
      RegionClass rc = pair.first;
      int targetClass = static_cast<int>( rc );

      Image mask( width, height, ColorSpace::Gray );

      for ( int y = 0; y < height; ++y )
      {
         for ( int x = 0; x < width; ++x )
         {
            size_t idx = static_cast<size_t>( y ) * width + x;
            int classIdx = static_cast<int>( classData[idx] * ( numClasses - 1 ) + 0.5f );
            classIdx = std::max( 0, std::min( numClasses - 1, classIdx ) );

            mask( x, y, 0 ) = ( classIdx == targetClass ) ? 1.0f : 0.0f;
         }
      }

      masks[rc] = std::move( mask );
   }

   return masks;
}

// ----------------------------------------------------------------------------

} // namespace pcl
