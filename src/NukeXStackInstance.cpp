//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "NukeXStackInstance.h"
#include "NukeXStackParameters.h"

#include <pcl/AutoViewLock.h>
#include <pcl/Console.h>
#include <pcl/FITSHeaderKeyword.h>
#include <pcl/FileFormat.h>
#include <pcl/FileFormatInstance.h>
#include <pcl/ImageWindow.h>
#include <pcl/StandardStatus.h>
#include <pcl/View.h>
#include <chrono>

#include "engine/PixelSelector.h"
#include "engine/PixelStackAnalyzer.h"
#include "engine/TransitionChecker.h"
#include "engine/Segmentation.h"
#include "engine/StretchLibrary.h"
#include "engine/algorithms/StatisticalAutoStretch.h"

#include <algorithm>

namespace pcl
{

// ----------------------------------------------------------------------------

NukeXStackInstance::NukeXStackInstance( const MetaProcess* m )
   : ProcessImplementation( m )
   , p_selectionStrategy( NXSSelectionStrategy::Default )
   , p_enableMLSegmentation( TheNXSEnableMLSegmentationParameter->DefaultValue() )
   , p_enableTransitionSmoothing( TheNXSEnableTransitionSmoothingParameter->DefaultValue() )
   , p_useSpatialContext( TheNXSUseSpatialContextParameter->DefaultValue() )
   , p_useTargetContext( TheNXSUseTargetContextParameter->DefaultValue() )
   , p_generateMetadata( TheNXSGenerateMetadataParameter->DefaultValue() )
   , p_outlierSigmaThreshold( static_cast<float>( TheNXSOutlierSigmaThresholdParameter->DefaultValue() ) )
   , p_minClassConfidence( static_cast<float>( TheNXSMinClassConfidenceParameter->DefaultValue() ) )
   , p_smoothingStrength( static_cast<float>( TheNXSSmoothingStrengthParameter->DefaultValue() ) )
   , p_transitionThreshold( static_cast<float>( TheNXSTransitionThresholdParameter->DefaultValue() ) )
   , p_tileSize( static_cast<int32>( TheNXSTileSizeParameter->DefaultValue() ) )
   , p_smoothingRadius( static_cast<int32>( TheNXSSmoothingRadiusParameter->DefaultValue() ) )
   , p_preStretchWithNukeX( TheNXSPreStretchWithNukeXParameter->DefaultValue() )
   , p_preStretchStrength( static_cast<float>( TheNXSPreStretchStrengthParameter->DefaultValue() ) )
{
}

// ----------------------------------------------------------------------------

NukeXStackInstance::NukeXStackInstance( const NukeXStackInstance& x )
   : ProcessImplementation( x )
{
   Assign( x );
}

// ----------------------------------------------------------------------------

void NukeXStackInstance::Assign( const ProcessImplementation& p )
{
   const NukeXStackInstance* x = dynamic_cast<const NukeXStackInstance*>( &p );
   if ( x != nullptr )
   {
      p_inputFrames             = x->p_inputFrames;
      p_selectionStrategy       = x->p_selectionStrategy;
      p_enableMLSegmentation    = x->p_enableMLSegmentation;
      p_enableTransitionSmoothing = x->p_enableTransitionSmoothing;
      p_useSpatialContext       = x->p_useSpatialContext;
      p_useTargetContext        = x->p_useTargetContext;
      p_generateMetadata        = x->p_generateMetadata;
      p_outlierSigmaThreshold   = x->p_outlierSigmaThreshold;
      p_minClassConfidence      = x->p_minClassConfidence;
      p_smoothingStrength       = x->p_smoothingStrength;
      p_transitionThreshold     = x->p_transitionThreshold;
      p_tileSize                = x->p_tileSize;
      p_smoothingRadius         = x->p_smoothingRadius;
      p_preStretchWithNukeX     = x->p_preStretchWithNukeX;
      p_preStretchStrength      = x->p_preStretchStrength;
   }
}

// ----------------------------------------------------------------------------

bool NukeXStackInstance::Validate( String& info )
{
   // Need at least 2 frames
   int enabledCount = 0;
   for ( const auto& frame : p_inputFrames )
      if ( frame.enabled )
         ++enabledCount;

   if ( enabledCount < 2 )
   {
      info = "At least 2 input frames must be enabled for integration.";
      return false;
   }

   // Validate parameter ranges
   if ( p_outlierSigmaThreshold < 1.0 || p_outlierSigmaThreshold > 10.0 )
   {
      info = "Outlier sigma threshold must be between 1.0 and 10.0.";
      return false;
   }

   if ( p_minClassConfidence < 0.0 || p_minClassConfidence > 1.0 )
   {
      info = "Minimum class confidence must be between 0.0 and 1.0.";
      return false;
   }

   if ( p_smoothingStrength < 0.0 || p_smoothingStrength > 1.0 )
   {
      info = "Smoothing strength must be between 0.0 and 1.0.";
      return false;
   }

   info.Clear();
   return true;
}

// ----------------------------------------------------------------------------

UndoFlags NukeXStackInstance::UndoMode( const View& ) const
{
   return UndoFlag::All;
}

// ----------------------------------------------------------------------------

bool NukeXStackInstance::CanExecuteOn( const View&, String& whyNot ) const
{
   whyNot = "NukeXStack is a global process that operates on file inputs.";
   return false;
}

// ----------------------------------------------------------------------------

bool NukeXStackInstance::CanExecuteGlobal( String& whyNot ) const
{
   int enabledCount = 0;
   for ( const auto& frame : p_inputFrames )
      if ( frame.enabled )
         ++enabledCount;

   if ( enabledCount < 2 )
   {
      whyNot = "At least 2 input frames must be enabled.";
      return false;
   }

   return true;
}

// ----------------------------------------------------------------------------

bool NukeXStackInstance::ExecuteGlobal()
{
   Console console;
   console.EnableAbort();

   auto startTime = std::chrono::high_resolution_clock::now();

   console.WriteLn( "<end><cbr><br>NukeXStack - Intelligent Pixel Selection Integration" );
   console.WriteLn( "=" );

   // Count enabled frames
   std::vector<size_t> enabledIndices;
   for ( size_t i = 0; i < p_inputFrames.size(); ++i )
      if ( p_inputFrames[i].enabled )
         enabledIndices.push_back( i );

   console.WriteLn( String().Format( "<br>Input frames: %d enabled of %d total",
      enabledIndices.size(), p_inputFrames.size() ) );

   // Load all enabled frames
   console.WriteLn( "<br>Loading frames..." );
   std::vector<Image> frames;
   frames.reserve( enabledIndices.size() );

   FITSKeywordArray referenceKeywords;
   int width = 0, height = 0, channels = 0;

   for ( size_t i = 0; i < enabledIndices.size(); ++i )
   {
      const String& path = p_inputFrames[enabledIndices[i]].path;
      console.Write( String().Format( "\rLoading frame %d of %d: %s",
         i + 1, enabledIndices.size(), File::ExtractName( path ).c_str() ) );
      console.Flush();

      Image frame;
      FITSKeywordArray keywords;

      if ( !LoadFrame( path, frame, keywords ) )
      {
         console.CriticalLn( String().Format( "\r<clrbol>Failed to load: %s", path.c_str() ) );
         return false;
      }

      // Validate dimensions match
      if ( i == 0 )
      {
         width = frame.Width();
         height = frame.Height();
         channels = frame.NumberOfChannels();
         referenceKeywords = keywords;
      }
      else if ( frame.Width() != width || frame.Height() != height )
      {
         console.CriticalLn( String().Format(
            "\r<clrbol>Frame dimension mismatch: %s (%dx%d) vs expected (%dx%d)",
            path.c_str(), frame.Width(), frame.Height(), width, height ) );
         return false;
      }

      frames.push_back( std::move( frame ) );
   }

   console.WriteLn( String().Format( "\r<clrbol>Loaded %d frames (%dx%dx%d)",
      frames.size(), width, height, channels ) );

   // Pre-stretch frames if enabled
   if ( p_preStretchWithNukeX )
   {
      console.WriteLn( String().Format( "<br>Pre-stretching frames with NukeX (strength=%.2f)...",
         p_preStretchStrength ) );

      for ( size_t i = 0; i < frames.size(); ++i )
      {
         console.Write( String().Format( "\rPre-stretching frame %d of %d...", i + 1, frames.size() ) );
         console.Flush();

         if ( !PreStretchFrame( frames[i] ) )
         {
            console.WarningLn( String().Format( "\r<clrbol>Warning: Pre-stretch failed for frame %d, using original",
               i + 1 ) );
         }
      }

      console.WriteLn( String().Format( "\r<clrbol>Pre-stretched %d frames", frames.size() ) );
   }

   // Run integration
   Image output;
   IntegrationSummary summary;

   if ( !RunIntegration( frames, referenceKeywords, output, summary ) )
   {
      console.CriticalLn( "Integration failed." );
      return false;
   }

   auto endTime = std::chrono::high_resolution_clock::now();
   summary.totalTimeMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();

   // Create output window
   ImageWindow outputWindow( width, height, channels,
      32, true, channels >= 3, true, "NukeXStack_Integration" );

   outputWindow.MainView().Image().CopyImage( output );
   outputWindow.Show();

   // Report summary
   console.WriteLn( "<br>Integration Summary:" );
   console.WriteLn( String().Format( "  Frames processed: %d", summary.enabledFrames ) );
   console.WriteLn( String().Format( "  Pixels processed: %d", summary.processedPixels ) );
   console.WriteLn( String().Format( "  Outlier pixels rejected: %d", summary.outlierPixels ) );

   if ( p_enableTransitionSmoothing )
      console.WriteLn( String().Format( "  Transitions smoothed: %d", summary.smoothedTransitions ) );

   if ( !summary.targetObject.IsEmpty() )
      console.WriteLn( String().Format( "  Target object: %s", summary.targetObject.c_str() ) );

   console.WriteLn( "<br>Timing:" );
   if ( p_enableMLSegmentation && summary.segmentationTimeMs > 0 )
      console.WriteLn( String().Format( "  Segmentation: %.1f ms", summary.segmentationTimeMs ) );
   console.WriteLn( String().Format( "  Pixel selection: %.1f ms", summary.selectionTimeMs ) );
   if ( p_enableTransitionSmoothing && summary.smoothingTimeMs > 0 )
      console.WriteLn( String().Format( "  Transition smoothing: %.1f ms", summary.smoothingTimeMs ) );
   console.WriteLn( String().Format( "  Total: %.1f ms", summary.totalTimeMs ) );

   console.WriteLn( "<br>NukeXStack integration complete." );

   return true;
}

// ----------------------------------------------------------------------------

void* NukeXStackInstance::LockParameter( const MetaParameter* p, size_type tableRow )
{
   if ( p == TheNXSInputFramePathParameter )
      return p_inputFrames[tableRow].path.Begin();
   if ( p == TheNXSInputFrameEnabledParameter )
      return &p_inputFrames[tableRow].enabled;
   if ( p == TheNXSSelectionStrategyParameter )
      return &p_selectionStrategy;
   if ( p == TheNXSEnableMLSegmentationParameter )
      return &p_enableMLSegmentation;
   if ( p == TheNXSEnableTransitionSmoothingParameter )
      return &p_enableTransitionSmoothing;
   if ( p == TheNXSUseSpatialContextParameter )
      return &p_useSpatialContext;
   if ( p == TheNXSUseTargetContextParameter )
      return &p_useTargetContext;
   if ( p == TheNXSGenerateMetadataParameter )
      return &p_generateMetadata;
   if ( p == TheNXSOutlierSigmaThresholdParameter )
      return &p_outlierSigmaThreshold;
   if ( p == TheNXSMinClassConfidenceParameter )
      return &p_minClassConfidence;
   if ( p == TheNXSSmoothingStrengthParameter )
      return &p_smoothingStrength;
   if ( p == TheNXSTransitionThresholdParameter )
      return &p_transitionThreshold;
   if ( p == TheNXSTileSizeParameter )
      return &p_tileSize;
   if ( p == TheNXSSmoothingRadiusParameter )
      return &p_smoothingRadius;
   if ( p == TheNXSPreStretchWithNukeXParameter )
      return &p_preStretchWithNukeX;
   if ( p == TheNXSPreStretchStrengthParameter )
      return &p_preStretchStrength;

   return nullptr;
}

// ----------------------------------------------------------------------------

bool NukeXStackInstance::AllocateParameter( size_type sizeOrLength, const MetaParameter* p, size_type tableRow )
{
   if ( p == TheNXSInputFramesParameter )
   {
      p_inputFrames.clear();
      if ( sizeOrLength > 0 )
         p_inputFrames.resize( sizeOrLength );
      return true;
   }

   if ( p == TheNXSInputFramePathParameter )
   {
      p_inputFrames[tableRow].path.Clear();
      if ( sizeOrLength > 0 )
         p_inputFrames[tableRow].path.SetLength( sizeOrLength );
      return true;
   }

   return false;
}

// ----------------------------------------------------------------------------

size_type NukeXStackInstance::ParameterLength( const MetaParameter* p, size_type tableRow ) const
{
   if ( p == TheNXSInputFramesParameter )
      return p_inputFrames.size();

   if ( p == TheNXSInputFramePathParameter )
      return p_inputFrames[tableRow].path.Length();

   return 0;
}

// ----------------------------------------------------------------------------

void NukeXStackInstance::AddInputFrame( const String& path, bool enabled )
{
   p_inputFrames.push_back( InputFrameData( path, enabled ) );
}

// ----------------------------------------------------------------------------

void NukeXStackInstance::ClearInputFrames()
{
   p_inputFrames.clear();
}

// ----------------------------------------------------------------------------

PixelSelectorConfig NukeXStackInstance::BuildSelectorConfig() const
{
   PixelSelectorConfig config;

   config.stackConfig = BuildStackConfig();
   config.useSpatialContext = p_useSpatialContext;
   config.minClassConfidence = p_minClassConfidence;
   config.useTargetContext = p_useTargetContext;

   return config;
}

// ----------------------------------------------------------------------------

TransitionCheckerConfig NukeXStackInstance::BuildTransitionConfig() const
{
   TransitionCheckerConfig config;

   config.tileSize = p_tileSize;
   config.hardTransitionThreshold = p_transitionThreshold;
   config.softTransitionThreshold = p_transitionThreshold * 0.4f;
   config.maxSmoothingStrength = p_smoothingStrength;
   config.smoothingRadius = p_smoothingRadius;
   config.checkFeatureAlignment = p_enableMLSegmentation;

   return config;
}

// ----------------------------------------------------------------------------

StackAnalysisConfig NukeXStackInstance::BuildStackConfig() const
{
   StackAnalysisConfig config;

   config.outlierSigmaThreshold = p_outlierSigmaThreshold;
   config.minFramesForStats = 3;

   // Set selection mode based on strategy
   switch ( p_selectionStrategy )
   {
   case NXSSelectionStrategy::Distribution:
      config.useMedianSelection = false;
      break;
   case NXSSelectionStrategy::WeightedMedian:
      config.useMedianSelection = true;
      break;
   case NXSSelectionStrategy::MLGuided:
   case NXSSelectionStrategy::Hybrid:
   default:
      config.useMedianSelection = false;
      break;
   }

   return config;
}

// ----------------------------------------------------------------------------

bool NukeXStackInstance::PreStretchFrame( Image& frame ) const
{
   // Use StatisticalAutoStretch directly - bypasses all Compositor complexity
   // This is designed specifically for linear astronomical data
   StatisticalAutoStretch stretch;

   // Configure stretch strength via target background
   // Lower target = more aggressive stretch
   // Map p_preStretchStrength (0-1) to target background (0.25 down to 0.05)
   double targetBg = 0.25 - (p_preStretchStrength * 0.20);
   stretch.SetTargetBackground( targetBg );

   // Apply stretch directly to the frame
   // ApplyToImage auto-computes statistics and applies appropriate MTF stretch
   stretch.ApplyToImage( frame );

   return true;
}

// ----------------------------------------------------------------------------

bool NukeXStackInstance::LoadFrame( const String& path, Image& image, FITSKeywordArray& keywords ) const
{
   try
   {
      // Determine file format from extension
      String extension = File::ExtractExtension( path ).Lowercase();

      FileFormat format( extension, true, false );

      FileFormatInstance file( format );
      ImageDescriptionArray images;

      if ( !file.Open( images, path ) )
         return false;

      if ( images.IsEmpty() )
      {
         file.Close();
         return false;
      }

      // Read FITS keywords if available
      if ( format.CanStoreKeywords() )
         file.ReadFITSKeywords( keywords );

      // Read as 32-bit float
      image.AllocateData( images[0].info.width, images[0].info.height,
         images[0].info.numberOfChannels, ColorSpace::Gray );

      if ( !file.ReadImage( image ) )
      {
         file.Close();
         return false;
      }

      file.Close();
      return true;
   }
   catch ( ... )
   {
      return false;
   }
}

// ----------------------------------------------------------------------------

bool NukeXStackInstance::RunIntegration(
   std::vector<Image>& frames,
   const FITSKeywordArray& keywords,
   Image& output,
   IntegrationSummary& summary ) const
{
   Console console;

   summary.totalFrames = static_cast<int>( p_inputFrames.size() );
   summary.enabledFrames = static_cast<int>( frames.size() );

   if ( frames.empty() )
      return false;

   int width = frames[0].Width();
   int height = frames[0].Height();
   int channels = frames[0].NumberOfChannels();

   // Build pixel selector configuration
   PixelSelectorConfig selectorConfig = BuildSelectorConfig();
   PixelSelector selector( selectorConfig );

   // Parse target context from FITS headers
   if ( p_useTargetContext )
   {
      TargetContext context;
      context.ParseFromHeaders( keywords );
      context.InferExpectedFeatures();
      selector.SetTargetContext( context );
      summary.targetObject = context.objectName;
   }

   // Run ML segmentation on reference frame (first or median)
   std::vector<std::vector<int>> segmentationMap;
   std::vector<std::vector<float>> confidenceMap;

   if ( p_enableMLSegmentation )
   {
      console.WriteLn( "<br>Running ML segmentation..." );
      auto segStart = std::chrono::high_resolution_clock::now();

      if ( RunSegmentation( frames[0], segmentationMap, confidenceMap ) )
      {
         selector.SetSegmentation( segmentationMap, confidenceMap );
         auto segEnd = std::chrono::high_resolution_clock::now();
         summary.segmentationTimeMs = std::chrono::duration<double, std::milli>(segEnd - segStart).count();
         console.WriteLn( String().Format( "Segmentation complete (%.1f ms)",
            summary.segmentationTimeMs ) );
      }
      else
      {
         console.WarningLn( "ML segmentation failed, continuing without class info." );
      }
   }

   // Prepare frame pointers for selector
   std::vector<const Image*> framePtrs;
   framePtrs.reserve( frames.size() );
   for ( const auto& frame : frames )
      framePtrs.push_back( &frame );

   // Allocate output image
   output.AllocateData( width, height, channels );

   // Process each channel
   console.WriteLn( "<br>Selecting pixels..." );
   auto selectionStart = std::chrono::high_resolution_clock::now();

   for ( int c = 0; c < channels; ++c )
   {
      console.Write( String().Format( "\rProcessing channel %d of %d...", c + 1, channels ) );
      console.Flush();

      // If generating metadata, use the detailed method
      if ( p_generateMetadata )
      {
         std::vector<std::vector<PixelSelectionResult>> metadata;
         Image channelResult = selector.ProcessStackWithMetadata( framePtrs, c, metadata );

         // Copy to output
         for ( int y = 0; y < height; ++y )
            for ( int x = 0; x < width; ++x )
               output.Pixel( x, y, c ) = channelResult.Pixel( x, y, 0 );

         // Count outliers from metadata
         for ( const auto& row : metadata )
            for ( const auto& result : row )
               if ( result.outlierMask != 0 )
                  summary.outlierPixels++;
      }
      else
      {
         // Use simpler method without metadata
         Image channelResult = selector.ProcessStack( framePtrs, c );

         // Copy to output
         for ( int y = 0; y < height; ++y )
            for ( int x = 0; x < width; ++x )
               output.Pixel( x, y, c ) = channelResult.Pixel( x, y, 0 );
      }
   }

   auto selectionEnd = std::chrono::high_resolution_clock::now();
   summary.selectionTimeMs = std::chrono::duration<double, std::milli>(selectionEnd - selectionStart).count();
   summary.processedPixels = width * height * channels;
   console.WriteLn( String().Format( "\r<clrbol>Pixel selection complete (%.1f ms)",
      summary.selectionTimeMs ) );

   // Apply transition smoothing
   if ( p_enableTransitionSmoothing )
   {
      console.WriteLn( "<br>Checking for hard transitions..." );
      auto smoothStart = std::chrono::high_resolution_clock::now();

      TransitionCheckerConfig transConfig = BuildTransitionConfig();
      TransitionChecker checker( transConfig );

      for ( int c = 0; c < channels; ++c )
      {
         // Create segmentation image from map if we have it
         Image segImage;
         if ( !segmentationMap.empty() )
         {
            segImage.AllocateData( width, height, 1 );
            for ( int y = 0; y < height; ++y )
               for ( int x = 0; x < width; ++x )
                  segImage.Pixel( x, y, 0 ) = static_cast<float>( segmentationMap[y][x] ) /
                     static_cast<float>( static_cast<int>( RegionClass::Count ) - 1 );

            summary.smoothedTransitions += checker.CheckAndSmooth( output, segImage, c );
         }
         else
         {
            summary.smoothedTransitions += checker.CheckAndSmooth( output, c );
         }
      }

      auto smoothEnd = std::chrono::high_resolution_clock::now();
      summary.smoothingTimeMs = std::chrono::duration<double, std::milli>(smoothEnd - smoothStart).count();
      console.WriteLn( String().Format( "Smoothed %d transitions (%.1f ms)",
         summary.smoothedTransitions, summary.smoothingTimeMs ) );
   }

   return true;
}

// ----------------------------------------------------------------------------

bool NukeXStackInstance::RunSegmentation(
   const Image& referenceFrame,
   std::vector<std::vector<int>>& segmentationMap,
   std::vector<std::vector<float>>& confidenceMap ) const
{
   // Try to find an available ONNX model
   auto modelPaths = SegmentationEngine::FindAvailableModels();
   if ( modelPaths.empty() )
      return false;

   // Configure segmentation using SegmentationEngineConfig
   SegmentationEngineConfig config;
   config.modelConfig.modelPath = modelPaths[0];
   config.autoFallback = true;

   SegmentationEngine engine( config );

   // Run segmentation using Process() which returns SegmentationEngineResult
   SegmentationEngineResult result = engine.Process( referenceFrame );
   if ( !result.isValid )
      return false;

   // Extract class and confidence from result
   int width = referenceFrame.Width();
   int height = referenceFrame.Height();

   segmentationMap.resize( height, std::vector<int>( width, 0 ) );
   confidenceMap.resize( height, std::vector<float>( width, 0.0f ) );

   // Convert from Image to vectors
   // The segmentation result contains classMap in segmentation member
   const Image& classMap = result.segmentation.classMap;
   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         // Class from first channel (normalized to class index)
         float classVal = classMap.Pixel( x, y, 0 );
         segmentationMap[y][x] = static_cast<int>(
            classVal * (static_cast<int>( RegionClass::Count ) - 1) + 0.5f );

         // Confidence from second channel if available, otherwise use 1.0
         if ( classMap.NumberOfChannels() > 1 )
            confidenceMap[y][x] = classMap.Pixel( x, y, 1 );
         else
            confidenceMap[y][x] = 1.0f;
      }
   }

   return true;
}

// ----------------------------------------------------------------------------

} // namespace pcl
