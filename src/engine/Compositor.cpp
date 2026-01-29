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
#include "HistogramEngine.h"
#include "StretchLibrary.h"

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

      // Stage 1: Analysis
      result.stage = ProcessingStage::Analyzing;
      ReportProgress( ProcessingStage::Analyzing, 0.0, "Analyzing image" );

      auto analysisStart = std::chrono::high_resolution_clock::now();
      RegionStatistics stats = AnalyzeImage( input );
      auto analysisEnd = std::chrono::high_resolution_clock::now();
      result.analysisTimeMs = std::chrono::duration<double, std::milli>(
         analysisEnd - analysisStart ).count();

      ReportProgress( ProcessingStage::Analyzing, 1.0, "Analysis complete" );

      // Stage 2: Algorithm Selection
      result.stage = ProcessingStage::SelectingAlgorithms;
      ReportProgress( ProcessingStage::SelectingAlgorithms, 0.0, "Selecting algorithm" );

      // Check if we have valid segmentation results for region-aware processing
      bool useRegionAware = m_config.useSegmentation && m_segmentationResult.isValid;

      // Maps for region-aware processing
      std::map<RegionClass, SelectedStretch> regionAlgorithms;
      std::map<RegionClass, Image> regionMasks;
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
         RegionAnalyzer analyzer;
         for ( const auto& coveragePair : regionCoverage )
         {
            RegionClass rc = coveragePair.first;
            double coverage = coveragePair.second;

            // Get mask for this region
            auto maskIt = regionMasks.find( rc );
            if ( maskIt == regionMasks.end() )
               continue;

            // Compute statistics for this region using the mask
            // For now, use the global stats scaled by region characteristics
            RegionStatistics regionStats = stats;
            regionStats.regionClass = rc;
            regionStats.maskCoverage = coverage;

            // Select algorithm for this region
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

      // Stage 3: Stretching
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

      // Stage 4: Tone Mapping
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

      // Stage 5: Finalization
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

   Image output( width, height, input.ColorSpace() );

   for ( int c = 0; c < numChannels; ++c )
   {
      for ( int y = 0; y < height; ++y )
      {
         for ( int x = 0; x < width; ++x )
         {
            output( x, y, c ) = algorithm->Apply( input( x, y, c ) );
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
      // Try to find the model in the src/models directory relative to module
      // This handles development builds
      modelPath = "/home/scarter4work/projects/NukeX/src/models/nukex_segmentation.onnx";
      if ( !File::Exists( modelPath ) )
      {
         // Also try installed location
         modelPath = "/opt/PixInsight/bin/nukex_segmentation.onnx";
      }
   }

   if ( !File::Exists( modelPath ) )
   {
      console.WarningLn( String().Format( "ONNX model not found at: %s", modelPath.c_str() ) );
      console.WarningLn( "ML segmentation disabled - using statistical selection only." );
      return false;
   }

   console.WriteLn( String().Format( "<br>Loading segmentation model: %s", modelPath.c_str() ) );

   // Configure the segmentation engine
   SegmentationEngineConfig engineConfig;
   engineConfig.modelConfig.modelPath = modelPath;
   engineConfig.modelConfig.inputWidth = 512;
   engineConfig.modelConfig.inputHeight = 512;
   engineConfig.modelConfig.useGPU = false;  // CPU for now
   engineConfig.autoFallback = true;         // Fall back to mock if ONNX fails
   engineConfig.cacheResults = false;        // No caching needed
   engineConfig.runAnalysis = true;          // Run region analysis for statistics
   engineConfig.downsampleLargeImages = true;
   engineConfig.maxProcessingDimension = 1024;  // Reasonable for segmentation

   // Create and initialize engine
   m_segmentation = std::make_unique<SegmentationEngine>();
   if ( !m_segmentation->Initialize( engineConfig ) )
   {
      console.WarningLn( String().Format( "Failed to initialize segmentation engine: %s",
         m_segmentation->GetLastError().c_str() ) );
      m_segmentation.reset();
      return false;
   }

   if ( !m_segmentation->IsReady() )
   {
      console.WarningLn( "Segmentation engine not ready" );
      m_segmentation.reset();
      return false;
   }

   console.WriteLn( String().Format( "Using model: %s", m_segmentation->GetModelName().c_str() ) );

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
         console.WriteLn( String().Format( "  %s", message.c_str() ) );
         break;
      case SegmentationEventType::Failed:
         console.WarningLn( String().Format( "  Segmentation failed: %s", message.c_str() ) );
         break;
      default:
         break;
      }
   };

   m_segmentationResult = m_segmentation->Process( input, progressCallback );

   if ( !m_segmentationResult.isValid )
   {
      console.WarningLn( String().Format( "Segmentation failed: %s", m_segmentationResult.errorMessage.c_str() ) );
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
   console.WriteLn( "  Applying region-aware stretch blending..." );

   int width = input.Width();
   int height = input.Height();
   int numChannels = input.NumberOfNominalChannels();

   // Get dimensions from segmentation result for mask scaling
   int maskWidth = m_segmentationResult.segmentation.width;
   int maskHeight = m_segmentationResult.segmentation.height;

   // Scale factors for mask to image coordinates
   double scaleX = static_cast<double>( maskWidth ) / width;
   double scaleY = static_cast<double>( maskHeight ) / height;

   Image output( width, height, input.ColorSpace() );

   // Build a map of raw pointers to algorithms for per-pixel access
   std::map<RegionClass, IStretchAlgorithm*> algoPointers;
   for ( const auto& pair : regionAlgorithms )
   {
      algoPointers[pair.first] = pair.second.algorithmInstance.get();
   }

   // Get the class map data for per-pixel class lookup
   const float* classMapData = m_segmentationResult.segmentation.classMap.PixelData( 0 );
   int numClasses = static_cast<int>( RegionClass::Count );

   // For each pixel, determine the region class and apply the appropriate stretch
   for ( int c = 0; c < numChannels; ++c )
   {
      for ( int y = 0; y < height; ++y )
      {
         for ( int x = 0; x < width; ++x )
         {
            double inputValue = input( x, y, c );

            // Map image coordinates to mask coordinates
            int mx = static_cast<int>( x * scaleX );
            int my = static_cast<int>( y * scaleY );
            mx = std::min( mx, maskWidth - 1 );
            my = std::min( my, maskHeight - 1 );

            // Get the class index from the class map
            size_t maskIdx = static_cast<size_t>( my ) * maskWidth + mx;
            float classValue = classMapData[maskIdx];
            int classIdx = static_cast<int>( classValue * ( numClasses - 1 ) + 0.5f );
            classIdx = std::max( 0, std::min( numClasses - 1, classIdx ) );
            RegionClass pixelClass = static_cast<RegionClass>( classIdx );

            // Find the algorithm for this region
            auto algoIt = algoPointers.find( pixelClass );
            if ( algoIt == algoPointers.end() || algoIt->second == nullptr )
            {
               // Fall back to Background algorithm
               algoIt = algoPointers.find( RegionClass::Background );
               if ( algoIt == algoPointers.end() || algoIt->second == nullptr )
               {
                  // No algorithm found, use first available
                  algoIt = algoPointers.begin();
               }
            }

            // Apply the stretch
            double stretchedValue = inputValue;
            if ( algoIt != algoPointers.end() && algoIt->second != nullptr )
            {
               stretchedValue = algoIt->second->Apply( inputValue );
            }

            // Clamp to valid range
            output( x, y, c ) = std::max( 0.0, std::min( 1.0, stretchedValue ) );
         }
      }
   }

   console.WriteLn( "  Region-aware stretch complete." );
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
