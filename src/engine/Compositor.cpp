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

#include "Compositor.h"
#include "HistogramEngine.h"

#include <pcl/Console.h>

#include <chrono>

namespace pcl
{

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

void Compositor::ReportProgress( ProcessingStage stage, double progress, const String& message )
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

      SelectedStretch selection = SelectAlgorithm( stats );

      // Create selection summary with single entry
      SelectionSummary::RegionEntry entry;
      entry.region = RegionClass::Background;
      entry.algorithm = selection.algorithm;
      entry.confidence = selection.confidence;
      entry.coverage = 1.0;
      entry.rationale = selection.rationale;  // Capture selection rationale
      result.selectionSummary.entries.push_back( entry );

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

         // Stretch luminance only
         Image stretchedL = ApplyStretch( luminance, selection.algorithmInstance.get() );

         // Recombine with color
         stretched = m_lrgbProcessor->ApplyStretchedLuminance( input, stretchedL );

         // Store for debugging
         result.luminanceImage = std::make_unique<Image>( stretchedL );
      }
      else
      {
         // Direct stretching
         stretched = ApplyStretch( input, selection.algorithmInstance.get() );
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

} // namespace pcl
