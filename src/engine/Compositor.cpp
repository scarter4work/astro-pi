//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "Compositor.h"

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

   // Only initialize segmentation engine if segmentation is enabled
   if ( config.useSegmentation )
   {
      Console().WriteLn( "Compositor: Initializing segmentation engine..." );
      m_segmentation = std::make_unique<SegmentationEngine>();
      if ( !m_segmentation->Initialize( config.segmentationConfig ) )
      {
         Console().WarningLn( "Segmentation initialization failed, using mock" );
      }
   }
   else
   {
      Console().WriteLn( "Compositor: Segmentation disabled, using simple background mask" );
   }

   // Initialize analyzer
   RegionAnalyzerConfig analyzerConfig;
   m_analyzer = std::make_unique<RegionAnalyzer>( analyzerConfig );

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
   case ProcessingStage::Segmenting:         return 0.25;
   case ProcessingStage::Analyzing:          return 0.10;
   case ProcessingStage::SelectingAlgorithms: return 0.05;
   case ProcessingStage::Stretching:         return 0.05;
   case ProcessingStage::Blending:           return 0.40;
   case ProcessingStage::ToneMapping:        return 0.10;
   case ProcessingStage::Finalizing:         return 0.05;
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
      // Stage 1: Segmentation
      result.stage = ProcessingStage::Segmenting;
      ReportProgress( ProcessingStage::Segmenting, 0.0, "Starting segmentation" );

      SegmentationEngineResult segResult;
      if ( m_config.useSegmentation )
      {
         segResult = RunSegmentation( input );
         result.segmentation = segResult;
         result.segmentationTimeMs = segResult.totalTimeMs;
      }
      else
      {
         // Create simple whole-image mask as background
         segResult.segmentation.masks[RegionClass::Background] =
            Image( input.Width(), input.Height(), pcl::ColorSpace::Gray );
         segResult.segmentation.masks[RegionClass::Background].White();

         // Set all validity flags and metadata
         segResult.segmentation.width = input.Width();
         segResult.segmentation.height = input.Height();
         segResult.segmentation.isValid = true;
         segResult.isValid = true;
      }

      if ( !segResult.isValid )
      {
         result.errorMessage = "Segmentation failed: " + segResult.errorMessage;
         result.stage = ProcessingStage::Failed;
         return result;
      }

      ReportProgress( ProcessingStage::Segmenting, 1.0, "Segmentation complete" );

      // Stage 2: Analysis
      result.stage = ProcessingStage::Analyzing;
      ReportProgress( ProcessingStage::Analyzing, 0.0, "Analyzing regions" );

      auto analysisStart = std::chrono::high_resolution_clock::now();
      RegionAnalysisResult analysis = RunAnalysis( input, segResult.segmentation );
      auto analysisEnd = std::chrono::high_resolution_clock::now();
      result.analysisTimeMs = std::chrono::duration<double, std::milli>(
         analysisEnd - analysisStart ).count();

      ReportProgress( ProcessingStage::Analyzing, 1.0, "Analysis complete" );

      // Stage 3: Algorithm Selection
      result.stage = ProcessingStage::SelectingAlgorithms;
      ReportProgress( ProcessingStage::SelectingAlgorithms, 0.0, "Selecting algorithms" );

      std::map<RegionClass, SelectedStretch> selections;
      if ( m_config.useAutoSelection )
      {
         selections = RunSelection( analysis );
      }
      else
      {
         // Use default GHS for all regions
         for ( const auto& pair : analysis.regionStats )
         {
            selections[pair.first] = m_selector->Select( pair.first, pair.second );
         }
      }

      // Create selection summary
      result.selectionSummary = SelectionSummary::Create(
         selections, analysis.regionCoverage );

      ReportProgress( ProcessingStage::SelectingAlgorithms, 1.0, "Algorithms selected" );

      // Stage 4-5: Stretching and Blending
      result.stage = ProcessingStage::Blending;
      ReportProgress( ProcessingStage::Blending, 0.0, "Applying stretches" );

      auto stretchStart = std::chrono::high_resolution_clock::now();

      // Get algorithm pointers for blending
      std::map<RegionClass, IStretchAlgorithm*> algorithms;
      for ( auto& pair : selections )
      {
         if ( pair.second.algorithmInstance )
         {
            algorithms[pair.first] = pair.second.algorithmInstance.get();
         }
      }

      Image stretched;
      if ( m_config.useLRGBMode && input.NumberOfNominalChannels() >= 3 )
      {
         // Extract luminance
         Image luminance = m_lrgbProcessor->ExtractLuminance( input );

         // Stretch luminance only
         Image stretchedL = RunBlending( luminance, segResult.segmentation.masks, algorithms );

         // Recombine with color
         stretched = m_lrgbProcessor->ApplyStretchedLuminance( input, stretchedL );

         // Store for debugging
         result.luminanceImage = std::make_unique<Image>( stretchedL );
      }
      else
      {
         // Direct RGB stretching
         stretched = RunBlending( input, segResult.segmentation.masks, algorithms );
      }

      auto stretchEnd = std::chrono::high_resolution_clock::now();
      result.stretchTimeMs = std::chrono::duration<double, std::milli>(
         stretchEnd - stretchStart ).count();

      ReportProgress( ProcessingStage::Blending, 1.0, "Blending complete" );

      // Stage 6: Tone Mapping
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

      // Stage 7: Finalization
      result.stage = ProcessingStage::Finalizing;
      ReportProgress( ProcessingStage::Finalizing, 0.0, "Finalizing" );

      // Apply global saturation adjustment
      if ( m_config.useLRGBMode && std::abs( m_config.globalSaturation ) > 0.001 )
      {
         m_lrgbProcessor->ApplySaturationBoost( toned, m_config.globalSaturation );
      }

      result.outputImage = std::move( toned );

      // Create segmentation preview
      result.segmentationPreview = std::make_unique<Image>(
         SegmentationVisualizer::CreateMaskOverlay( input, segResult.segmentation, 0.3 ) );

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

CompositorResult Compositor::ProcessWithSegmentation( const Image& input,
                                                       const SegmentationResult& segmentation )
{
   // Create a segmentation result with provided masks
   SegmentationEngineResult segResult;
   segResult.segmentation = segmentation;
   segResult.isValid = true;

   // Continue with normal processing (skip segmentation stage)
   m_config.useSegmentation = false;  // Temporary disable

   auto result = Process( input );

   m_config.useSegmentation = true;   // Restore

   return result;
}

// ----------------------------------------------------------------------------

Image Compositor::ProcessRegion( const Image& input, RegionClass region,
                                  const RegionStatistics& stats )
{
   // Select algorithm for this region
   SelectedStretch selection = m_selector->Select( region, stats );

   if ( !selection.algorithmInstance )
   {
      return input;
   }

   // Apply stretch to entire image
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
            output( x, y, c ) = selection.algorithmInstance->Apply( input( x, y, c ) );
         }
      }
   }

   return output;
}

// ----------------------------------------------------------------------------

std::map<RegionClass, SelectedStretch> Compositor::PreviewSelection( const Image& input )
{
   // Quick analysis
   RegionAnalysisResult analysis = m_analyzer->Analyze( input );

   // Get selections
   return m_selector->SelectAll( analysis );
}

// ----------------------------------------------------------------------------

SegmentationEngineResult Compositor::RunSegmentation( const Image& input )
{
   return m_segmentation->Process( input,
      [this]( SegmentationEventType event, double progress, const String& msg ) {
         // Map segmentation progress to compositor progress
         double mappedProgress = 0;
         switch ( event )
         {
         case SegmentationEventType::Preprocessing: mappedProgress = 0.1; break;
         case SegmentationEventType::ModelRunning:  mappedProgress = 0.3 + progress * 0.5; break;
         case SegmentationEventType::Postprocessing: mappedProgress = 0.85; break;
         case SegmentationEventType::Completed:     mappedProgress = 1.0; break;
         default: break;
         }
         ReportProgress( ProcessingStage::Segmenting, mappedProgress, msg );
      }
   );
}

// ----------------------------------------------------------------------------

RegionAnalysisResult Compositor::RunAnalysis( const Image& input,
                                               const SegmentationResult& seg )
{
   std::map<RegionClass, Image> masks = seg.masks;
   return m_analyzer->Analyze( input, masks );
}

// ----------------------------------------------------------------------------

std::map<RegionClass, SelectedStretch> Compositor::RunSelection(
   const RegionAnalysisResult& analysis )
{
   return m_selector->SelectAll( analysis );
}

// ----------------------------------------------------------------------------

Image Compositor::RunStretching( const Image& input,
                                  const std::map<RegionClass, Image>& masks,
                                  const std::map<RegionClass, SelectedStretch>& selections )
{
   // This is now handled by RunBlending for efficiency
   return input;
}

// ----------------------------------------------------------------------------

Image Compositor::RunBlending( const Image& input,
                                const std::map<RegionClass, Image>& masks,
                                std::map<RegionClass, IStretchAlgorithm*>& algorithms )
{
   if ( m_config.useParallelBlend )
   {
      return m_parallelBlender->Process( input, masks, algorithms,
         [this]( double progress, const String& msg ) {
            ReportProgress( ProcessingStage::Blending, progress, msg );
         },
         m_config.numThreads );
   }
   else
   {
      return m_blender->BlendWithAlgorithms( input, masks, algorithms );
   }
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

   // Auto-configure from global statistics
   RegionAnalyzer analyzer;
   auto analysis = analyzer.Analyze( input );

   stretchAlgo->AutoConfigure( analysis.globalStats.luminance );

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
   config.useSegmentation = false;  // Simple LRGB without segmentation

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
