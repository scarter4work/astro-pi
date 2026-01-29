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
#include "engine/RegionStatistics.h"
#include "engine/Segmentation.h"

#include <algorithm>
#include <numeric>

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
         i + 1, enabledIndices.size(), IsoString( File::ExtractName( path ) ).c_str() ) );
      console.Flush();

      Image frame;
      FITSKeywordArray keywords;

      if ( !LoadFrame( path, frame, keywords ) )
      {
         console.CriticalLn( String().Format( "\r<clrbol>Failed to load: %s", IsoString( path ).c_str() ) );
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
            IsoString( path ).c_str(), frame.Width(), frame.Height(), width, height ) );
         return false;
      }

      frames.push_back( std::move( frame ) );
   }

   console.WriteLn( String().Format( "\r<clrbol>Loaded %d frames (%dx%dx%d)",
      frames.size(), width, height, channels ) );

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
      console.WriteLn( String().Format( "  Target object: %s", IsoString( summary.targetObject ).c_str() ) );

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

// ----------------------------------------------------------------------------

Image NukeXStackInstance::CreateMedianReference( const std::vector<Image>& frames ) const
{
   if ( frames.empty() )
      return Image();

   int width = frames[0].Width();
   int height = frames[0].Height();
   int channels = frames[0].NumberOfChannels();
   size_t numFrames = frames.size();

   Image reference;
   reference.AllocateData( width, height, channels );

   // For each pixel position, compute the median across all frames
   std::vector<float> values( numFrames );

   for ( int c = 0; c < channels; ++c )
   {
      for ( int y = 0; y < height; ++y )
      {
         for ( int x = 0; x < width; ++x )
         {
            // Gather values from all frames
            for ( size_t f = 0; f < numFrames; ++f )
               values[f] = frames[f].Pixel( x, y, c );

            // Sort and take median
            std::nth_element( values.begin(), values.begin() + numFrames / 2, values.end() );
            float median = values[numFrames / 2];

            // For even number of frames, average the two middle values
            if ( numFrames % 2 == 0 && numFrames > 1 )
            {
               auto maxIt = std::max_element( values.begin(), values.begin() + numFrames / 2 );
               median = ( *maxIt + median ) / 2.0f;
            }

            reference.Pixel( x, y, c ) = median;
         }
      }
   }

   return reference;
}

// ----------------------------------------------------------------------------

bool NukeXStackInstance::RunSegmentation(
   const Image& referenceImage,
   std::vector<std::vector<int>>& classMap,
   std::vector<std::vector<float>>& confidenceMap,
   double& segmentationTimeMs ) const
{
   Console console;

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
      console.WarningLn( String().Format( "ONNX model not found at: %s", IsoString( modelPath ).c_str() ) );
      console.WarningLn( "ML segmentation disabled - using statistical selection only." );
      return false;
   }

   console.WriteLn( String().Format( "<br>Loading segmentation model: %s", IsoString( modelPath ).c_str() ) );

   // Configure the segmentation engine
   SegmentationEngineConfig engineConfig;
   engineConfig.modelConfig.modelPath = modelPath;
   engineConfig.modelConfig.inputWidth = 512;
   engineConfig.modelConfig.inputHeight = 512;
   engineConfig.modelConfig.useGPU = false;  // CPU for now
   engineConfig.autoFallback = true;         // Fall back to mock if ONNX fails
   engineConfig.cacheResults = false;        // No need for caching in stacker
   engineConfig.runAnalysis = false;         // We don't need region analysis
   engineConfig.downsampleLargeImages = true;
   engineConfig.maxProcessingDimension = 1024;  // Reasonable for segmentation

   // Create and initialize engine
   SegmentationEngine engine;
   if ( !engine.Initialize( engineConfig ) )
   {
      console.WarningLn( String().Format( "Failed to initialize segmentation engine: %s",
         IsoString( engine.GetLastError() ).c_str() ) );
      return false;
   }

   if ( !engine.IsReady() )
   {
      console.WarningLn( "Segmentation engine not ready" );
      return false;
   }

   console.WriteLn( String().Format( "Using model: %s", IsoString( engine.GetModelName() ).c_str() ) );

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

   SegmentationEngineResult result = engine.Process( referenceImage, progressCallback );

   if ( !result.isValid )
   {
      console.WarningLn( String().Format( "Segmentation failed: %s", IsoString( result.errorMessage ).c_str() ) );
      return false;
   }

   segmentationTimeMs = result.totalTimeMs;

   // Convert the class map image to the vector format expected by PixelSelector
   int width = result.segmentation.width;
   int height = result.segmentation.height;

   classMap.resize( height );
   confidenceMap.resize( height );

   for ( int y = 0; y < height; ++y )
   {
      classMap[y].resize( width );
      confidenceMap[y].resize( width );

      for ( int x = 0; x < width; ++x )
      {
         // The class map stores classes normalized to [0,1]
         // We need to convert back to integer class indices
         float normalizedClass = result.segmentation.classMap.Pixel( x, y, 0 );
         int numClasses = static_cast<int>( RegionClass::Count );
         int classIdx = static_cast<int>( normalizedClass * ( numClasses - 1 ) + 0.5f );
         classIdx = std::max( 0, std::min( numClasses - 1, classIdx ) );

         classMap[y][x] = classIdx;

         // Get confidence from the mask of the predicted class
         RegionClass rc = static_cast<RegionClass>( classIdx );
         const Image* mask = result.segmentation.GetMask( rc );
         if ( mask && x < mask->Width() && y < mask->Height() )
         {
            confidenceMap[y][x] = mask->Pixel( x, y, 0 );
         }
         else
         {
            confidenceMap[y][x] = 0.5f;  // Default confidence
         }
      }
   }

   // Report class distribution
   std::map<int, int> classCounts;
   for ( int y = 0; y < height; ++y )
      for ( int x = 0; x < width; ++x )
         classCounts[classMap[y][x]]++;

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

   return true;
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

   // ML segmentation setup
   std::vector<std::vector<int>> segmentationMap;
   std::vector<std::vector<float>> confidenceMap;

   if ( p_enableMLSegmentation )
   {
      console.WriteLn( "<br>Creating median reference image for segmentation..." );

      // Create a median reference image from the stack
      Image referenceImage = CreateMedianReference( frames );

      if ( referenceImage.Width() > 0 )
      {
         console.WriteLn( String().Format( "Reference image: %dx%d",
            referenceImage.Width(), referenceImage.Height() ) );

         // Run ML segmentation on the reference image
         double segTime = 0.0;
         if ( RunSegmentation( referenceImage, segmentationMap, confidenceMap, segTime ) )
         {
            summary.segmentationTimeMs = segTime;

            // Pass segmentation to the pixel selector
            selector.SetSegmentation( segmentationMap, confidenceMap );
            console.WriteLn( String().Format( "<br>ML segmentation enabled (%.1f ms)", segTime ) );
            console.WriteLn( "Pixel selection will use region-aware strategies." );
         }
         else
         {
            console.WarningLn( "<br>ML segmentation failed - using statistical selection only." );
         }
      }
      else
      {
         console.WarningLn( "<br>Failed to create reference image for segmentation." );
      }
   }
   else
   {
      console.WriteLn( "<br>ML segmentation disabled by user." );
      console.WriteLn( "Using statistical pixel selection without ML classification." );
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

} // namespace pcl
