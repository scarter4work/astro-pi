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
#ifdef NUKEX_USE_ONNX
#include "engine/Segmentation.h"
#endif
#include "engine/FrameStreamer.h"
#include "engine/BayerDemosaic.h"
#include "engine/algorithms/ArcSinhStretch.h"
#include "engine/IStretchAlgorithm.h"
#include "engine/Compositor.h"

#include <algorithm>
#include <cstring>
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
   , p_enableAutoStretch( TheNXSEnableAutoStretchParameter->DefaultValue() )
   , p_enableRegistration( TheNXSEnableRegistrationParameter->DefaultValue() )
   , p_enableNormalization( TheNXSEnableNormalizationParameter->DefaultValue() )
   , p_enableQualityWeighting( TheNXSEnableQualityWeightingParameter->DefaultValue() )
   , p_excludeFailedRegistration( TheNXSExcludeFailedRegistrationParameter->DefaultValue() )
   , p_outlierSigmaThreshold( static_cast<float>( TheNXSOutlierSigmaThresholdParameter->DefaultValue() ) )
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
      p_enableAutoStretch       = x->p_enableAutoStretch;
      p_enableRegistration      = x->p_enableRegistration;
      p_enableNormalization         = x->p_enableNormalization;
      p_enableQualityWeighting      = x->p_enableQualityWeighting;
      p_excludeFailedRegistration   = x->p_excludeFailedRegistration;
      p_outlierSigmaThreshold   = x->p_outlierSigmaThreshold;
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

// Helper function to extract double value from FITS keywords
static double GetKeywordDouble( const FITSKeywordArray& keywords, const char* name1, const char* name2 = nullptr, double defaultVal = 0.0 )
{
   for ( const FITSHeaderKeyword& kw : keywords )
   {
      IsoString n = kw.name.Uppercase();
      if ( n == name1 || (name2 && n == name2) )
         try { return kw.value.ToDouble(); }
         catch ( ... ) { return defaultVal; }
   }
   return defaultVal;
}

// Helper function to extract string value from FITS keywords
static IsoString GetKeywordString( const FITSKeywordArray& keywords, const char* name, const IsoString& defaultVal = IsoString() )
{
   for ( const FITSHeaderKeyword& kw : keywords )
   {
      IsoString n = kw.name.Uppercase();
      if ( n == name )
      {
         // Remove quotes if present
         IsoString val = kw.value;
         val.Trim();
         if ( val.StartsWith( '\'' ) && val.EndsWith( '\'' ) )
            val = val.Substring( 1, val.Length() - 2 );
         val.Trim();
         return val;
      }
   }
   return defaultVal;
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

   // Collect enabled frame paths
   std::vector<String> framePaths;
   framePaths.reserve( enabledIndices.size() );
   for ( size_t i = 0; i < enabledIndices.size(); ++i )
      framePaths.push_back( p_inputFrames[enabledIndices[i]].path );

   // Initialize FrameStreamer to validate dimensions and extract keywords
   FrameStreamer streamer;
   if ( !streamer.Initialize( framePaths ) )
   {
      console.CriticalLn( "Failed to initialize frame streamer." );
      return false;
   }

   int width = streamer.Width();
   int height = streamer.Height();
   int channels = streamer.Channels();
   FITSKeywordArray referenceKeywords = streamer.GetKeywords( 0 );

   // Detect and log frame format details
   {
      String colorDesc;
      if ( channels >= 3 )
         colorDesc = "RGB";
      else
         colorDesc = "mono";

      // Check for CFA/Bayer pattern in reference keywords (informational only -
      // actual demosaicing happens in LoadFrame() or FrameStreamer)
      for ( const FITSHeaderKeyword& kw : referenceKeywords )
      {
         if ( kw.name.Uppercase().Trimmed() == "BAYERPAT" )
         {
            console.WriteLn( String().Format(
               "<br>CFA/Bayer mosaic detected (pattern: %s) - auto-demosaicing to RGB",
               IsoString( kw.value.Trimmed() ).c_str() ) );
            break;
         }
      }

      console.WriteLn( String().Format(
         "<br>Frame format: %dx%d, %d channel(s) (%s)",
         width, height, channels, IsoString( colorDesc ).c_str() ) );
   }

   // Compute total frame data size to decide streaming vs in-memory
   size_t totalFrameBytes = static_cast<size_t>( streamer.NumFrames() )
      * width * height * channels * sizeof( float );
   const size_t streamingThreshold = 8ULL * 1024 * 1024 * 1024;  // 8 GB

   bool useStreaming = ( totalFrameBytes > streamingThreshold );

   Image output;
   IntegrationSummary summary;

   if ( useStreaming )
   {
      console.WriteLn( String().Format(
         "<br>Streaming mode: %.1f GB frame data exceeds %.1f GB threshold",
         totalFrameBytes / ( 1024.0 * 1024.0 * 1024.0 ),
         streamingThreshold / ( 1024.0 * 1024.0 * 1024.0 ) ) );

      if ( p_enableRegistration )
         console.WarningLn( "** Registration is not available in streaming mode. "
                            "Frames will be stacked without star alignment. "
                            "Consider using in-memory mode for better results." );

      if ( !RunIntegrationStreaming( streamer, referenceKeywords, output, summary ) )
      {
         console.CriticalLn( "Streaming integration failed." );
         return false;
      }
   }
   else
   {
      console.WriteLn( String().Format(
         "<br>In-memory mode: %.1f GB frame data within %.1f GB threshold",
         totalFrameBytes / ( 1024.0 * 1024.0 * 1024.0 ),
         streamingThreshold / ( 1024.0 * 1024.0 * 1024.0 ) ) );

      // Close streamer and load frames into memory
      streamer.Close();

      console.WriteLn( "<br>Loading frames..." );
      std::vector<Image> frames;
      frames.reserve( framePaths.size() );

      int totalFrames = static_cast<int>( framePaths.size() );
      int lastPctReported = -1;

      for ( size_t i = 0; i < framePaths.size(); ++i )
      {
         // Progress reporting every 10% or on last frame
         int pct = static_cast<int>( 100.0 * ( i + 1 ) / totalFrames );
         int pctBucket = pct / 10;
         if ( pctBucket > lastPctReported || static_cast<int>( i ) == totalFrames - 1 )
         {
            console.Write( String().Format( "\rLoading frames... %d/%d (%d%%)",
               static_cast<int>( i + 1 ), totalFrames, pct ) );
            console.Flush();
            lastPctReported = pctBucket;
         }

         Image frame;
         FITSKeywordArray keywords;
         if ( !LoadFrame( framePaths[i], frame, keywords ) )
         {
            console.CriticalLn( String().Format( "\r<clrbol>Failed to load frame %d: %s",
               static_cast<int>( i + 1 ), IsoString( framePaths[i] ).c_str() ) );
            return false;
         }

         frames.push_back( std::move( frame ) );
      }

      // Final summary
      size_t totalBytes = static_cast<size_t>( frames.size() ) * width * height * channels * sizeof( float );
      console.WriteLn( String().Format( "\r<clrbol>Loaded %d frames (%dx%dx%d, %.1f GB in memory)",
         static_cast<int>( frames.size() ), width, height, channels,
         totalBytes / ( 1024.0 * 1024.0 * 1024.0 ) ) );

      // === Preprocessing: Background normalization ===
      if ( p_enableNormalization && frames.size() > 1 )
      {
         console.WriteLn( "<br>Computing background normalization..." );
         auto normParams = ComputeNormalizationParams( frames );
         NormalizeFrames( frames, normParams );
         summary.normalizedFrames = static_cast<int>( frames.size() );
         console.WriteLn( String().Format( "Normalized %d frames to reference background.",
            static_cast<int>( frames.size() ) ) );
      }

      // === Frame registration (star alignment) ===
      RegistrationSummary regSummary;
      if ( p_enableRegistration && frames.size() > 1 )
         regSummary = RegisterAllFrames( frames );

      // === Exclude failed registrations ===
      if ( p_excludeFailedRegistration && regSummary.failedFrames > 0 )
      {
         ExcludeFailedFrames( frames, framePaths, regSummary );
         summary.excludedFrames = regSummary.failedFrames;

         if ( frames.size() < 2 )
         {
            console.CriticalLn( "All frames failed registration. Cannot integrate." );
            return false;
         }
      }

      // === Quality weighting ===
      std::vector<float> frameWeights;
      if ( p_enableQualityWeighting && frames.size() > 1 )
      {
         console.WriteLn( "<br>Computing frame quality weights..." );
         frameWeights = ComputeQualityWeights( frames );
      }

      if ( !RunIntegration( frames, referenceKeywords, output, summary, frameWeights ) )
      {
         console.CriticalLn( "Integration failed." );
         return false;
      }
   }

   auto endTime = std::chrono::high_resolution_clock::now();
   summary.totalTimeMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();


   // Apply auto-stretch to output if enabled
   if ( p_enableAutoStretch )
   {
      console.WriteLn( "<br>Applying automatic stretch to integrated output..." );
      auto stretchStart = std::chrono::high_resolution_clock::now();
      output = QuickProcess::AutoStretch( output );
      auto stretchEnd = std::chrono::high_resolution_clock::now();
      double stretchMs = std::chrono::duration<double, std::milli>( stretchEnd - stretchStart ).count();
      console.WriteLn( String().Format( "  Auto-stretch completed in %.1f ms", stretchMs ) );
   }

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
   if ( p == TheNXSEnableAutoStretchParameter )
      return &p_enableAutoStretch;
   if ( p == TheNXSEnableRegistrationParameter )
      return &p_enableRegistration;
   if ( p == TheNXSEnableNormalizationParameter )
      return &p_enableNormalization;
   if ( p == TheNXSEnableQualityWeightingParameter )
      return &p_enableQualityWeighting;
   if ( p == TheNXSExcludeFailedRegistrationParameter )
      return &p_excludeFailedRegistration;
   if ( p == TheNXSOutlierSigmaThresholdParameter )
      return &p_outlierSigmaThreshold;
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

      // Find valid HDUs
      std::vector<int> validHDUs;
      for ( int d = 0; d < images.Length(); ++d )
      {
         if ( images[d].info.width > 0 && images[d].info.height > 0 &&
              images[d].info.numberOfChannels >= 1 )
            validHDUs.push_back( d );
      }

      if ( validHDUs.empty() )
      {
         file.Close();
         return false;
      }

      // Check for multi-extension RGB
      bool multiExtRGB = false;
      if ( validHDUs.size() > 1 )
      {
         bool allSingleCh = true;
         bool allSameDims = true;
         int refW = images[validHDUs[0]].info.width;
         int refH = images[validHDUs[0]].info.height;
         for ( int vi : validHDUs )
         {
            if ( images[vi].info.numberOfChannels != 1 ) allSingleCh = false;
            if ( images[vi].info.width != refW || images[vi].info.height != refH ) allSameDims = false;
         }
         multiExtRGB = ( allSingleCh && allSameDims );
      }

      if ( multiExtRGB )
      {
         // Multi-extension RGB: read each HDU as a channel
         int w = images[validHDUs[0]].info.width;
         int h = images[validHDUs[0]].info.height;
         int totalChannels = static_cast<int>( validHDUs.size() );

         image.AllocateData( w, h, totalChannels,
            totalChannels >= 3 ? ColorSpace::RGB : ColorSpace::Gray );

         for ( int ci = 0; ci < totalChannels; ++ci )
         {
            file.SelectImage( validHDUs[ci] );
            Image temp;
            temp.AllocateData( w, h, 1, ColorSpace::Gray );
            if ( !file.ReadImage( temp ) )
            {
               file.Close();
               return false;
            }
            // Copy temp channel 0 to output channel ci
            const Image::sample* src = temp.PixelData( 0 );
            Image::sample* dst = image.PixelData( ci );
            size_t numPx = static_cast<size_t>( w ) * h;
            std::memcpy( dst, src, numPx * sizeof( Image::sample ) );
         }
      }
      else
      {
         // Single-extension or standard multi-channel: use first valid HDU
         int vi = validHDUs[0];
         if ( vi > 0 )
            file.SelectImage( vi );

         image.AllocateData( images[vi].info.width, images[vi].info.height,
            images[vi].info.numberOfChannels,
            images[vi].info.numberOfChannels >= 3 ? ColorSpace::RGB : ColorSpace::Gray );

         if ( !file.ReadImage( image ) )
         {
            file.Close();
            return false;
         }
      }

      // Auto-demosaic CFA/Bayer data to RGB
      if ( image.NumberOfChannels() == 1 )
      {
         for ( const FITSHeaderKeyword& kw : keywords )
         {
            if ( kw.name.Uppercase().Trimmed() == "BAYERPAT" )
            {
               BayerPattern bp = ParseBayerPattern( IsoString( kw.value.Trimmed() ) );
               if ( bp != BayerPattern::Unknown )
                  image = BayerDemosaic::Demosaic( image, bp );
               break;
            }
         }
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
   reference.AllocateData( width, height, channels,
      channels >= 3 ? ColorSpace::RGB : ColorSpace::Gray );

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

            std::sort( values.begin(), values.end() );
            float median;
            if ( numFrames % 2 == 0 && numFrames > 1 )
               median = (values[numFrames / 2 - 1] + values[numFrames / 2]) / 2.0f;
            else
               median = values[numFrames / 2];

            reference.Pixel( x, y, c ) = median;
         }
      }
   }

   return reference;
}

// ----------------------------------------------------------------------------

Image NukeXStackInstance::CreateMedianReferenceStreaming( FrameStreamer& streamer ) const
{
   int width = streamer.Width();
   int height = streamer.Height();
   int channels = streamer.Channels();
   int numFrames = streamer.NumFrames();

   Image reference;
   reference.AllocateData( width, height, channels,
      channels >= 3 ? ColorSpace::RGB : ColorSpace::Gray );

   std::vector<float> values( numFrames );

   for ( int c = 0; c < channels; ++c )
   {
      // Reset file handles before each channel (except first)
      // to ensure clean read state after sequential row reads
      if ( c > 0 )
      {
         if ( !streamer.ResetAllFiles() )
         {
            Console().CriticalLn( "Failed to reset streamer for median reference channel change." );
            return Image();
         }
      }

      for ( int y = 0; y < height; ++y )
      {
         // Read this row from all frames
         std::vector<std::vector<float>> rowData;
         if ( !streamer.ReadRow( y, c, rowData ) )
         {
            Console().CriticalLn( String().Format(
               "Failed to read row %d for median reference", y ) );
            return Image();
         }

         for ( int x = 0; x < width; ++x )
         {
            // Gather values from all frames for this pixel
            for ( int f = 0; f < numFrames; ++f )
               values[f] = rowData[f][x];

            std::sort( values.begin(), values.end() );
            float median;
            if ( numFrames % 2 == 0 && numFrames > 1 )
               median = (values[numFrames / 2 - 1] + values[numFrames / 2]) / 2.0f;
            else
               median = values[numFrames / 2];

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
#ifdef NUKEX_USE_ONNX
   Console console;

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
   engineConfig.modelConfig.deterministic = true;
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
#else
   (void)referenceImage;
   (void)classMap;
   (void)confidenceMap;
   (void)segmentationTimeMs;
   return false;
#endif
}

// ----------------------------------------------------------------------------

bool NukeXStackInstance::RunIntegration(
   std::vector<Image>& frames,
   const FITSKeywordArray& keywords,
   Image& output,
   IntegrationSummary& summary,
   const std::vector<float>& frameWeights ) const
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

   // Pass quality weights to selector
   if ( !frameWeights.empty() )
      selector.SetFrameWeights( frameWeights );

   // ML segmentation setup - median composite approach
   // Instead of per-frame segmentation (N inferences), we run segmentation ONCE
   // on a median composite. This is faster and produces a single consistent class map.
   std::vector<std::vector<int>> segmentationMap;
   std::vector<std::vector<float>> confidenceMap;

   if ( p_enableMLSegmentation )
   {
      console.WriteLn( "<br>Creating median composite for segmentation..." );
      auto segStart = std::chrono::high_resolution_clock::now();

      // Step 1: Create median reference from all frames
      Image medianRef = CreateMedianReference( frames );

      if ( medianRef.Width() > 0 && medianRef.Height() > 0 )
      {
         console.WriteLn( String().Format( "Median composite: %dx%d",
            medianRef.Width(), medianRef.Height() ) );

         // Step 2: Apply arcsinh stretch to the median composite
         // (segmentation model expects stretched/visible-range input)
         RegionStatistics stats;
         {
            double median, mad;
            median = medianRef.Median();

            // Compute MAD manually: median of |pixel - median|
            Image devImage;
            devImage.Assign( medianRef );
            for ( int c = 0; c < devImage.NumberOfChannels(); ++c )
            {
               Image::sample* data = devImage.PixelData( c );
               size_t numPx = static_cast<size_t>( devImage.Width() ) * devImage.Height();
               for ( size_t p = 0; p < numPx; ++p )
                  data[p] = std::abs( data[p] - static_cast<float>( median ) );
            }
            mad = devImage.Median();

            stats.median = median;
            stats.mad = mad;
            stats.min = 0.0;
            stats.max = 1.0;
            stats.snrEstimate = ( mad > 1e-10 ) ? ( median / mad ) : 10.0;
         }

         ArcSinhStretch stretcher;
         stretcher.AutoConfigure( stats );
         stretcher.ApplyToImage( medianRef );

         console.WriteLn( "Applied arcsinh stretch to median composite." );

         // Step 3: Run segmentation ONCE on the stretched median composite
         double segTime = 0.0;
         if ( RunSegmentation( medianRef, segmentationMap, confidenceMap, segTime ) )
         {
            summary.segmentationTimeMs = segTime;

            // Step 4: Pass single class map to selector
            selector.SetSegmentation( segmentationMap, confidenceMap );

            auto segEnd = std::chrono::high_resolution_clock::now();
            double totalSegTime = std::chrono::duration<double, std::milli>(
               segEnd - segStart ).count();

            console.WriteLn( String().Format(
               "<br>Median composite segmentation complete (%.1f ms total, %.1f ms inference)",
               totalSegTime, segTime ) );
            console.WriteLn( "Pixel selection will use region-aware strategies." );
         }
         else
         {
            console.WarningLn( "<br>ML segmentation failed on median composite - "
               "using statistical selection only." );
         }
      }
      else
      {
         console.WarningLn( "<br>Failed to create median composite for segmentation." );
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
   output.AllocateData( width, height, channels,
      channels >= 3 ? ColorSpace::RGB : ColorSpace::Gray );

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
         std::vector<PixelSelectionResult> metadata;
         Image channelResult = selector.ProcessStackWithMetadata( framePtrs, c, metadata );

         // Copy to output
         for ( int y = 0; y < height; ++y )
            for ( int x = 0; x < width; ++x )
               output.Pixel( x, y, c ) = channelResult.Pixel( x, y, 0 );

         // Count outliers from flat metadata
         for ( const auto& result : metadata )
            if ( result.outlierCount > 0 )
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
            // Segmentation map may have different dimensions than the output image
            // (due to downsampling in the segmentation engine)
            int segHeight = static_cast<int>( segmentationMap.size() );
            int segWidth = segHeight > 0 ? static_cast<int>( segmentationMap[0].size() ) : 0;

            // Build segmentation image at output image dimensions with nearest-neighbor upscaling
            segImage.AllocateData( width, height, 1 );
            float scaleX = ( segWidth > 1 ) ? static_cast<float>( segWidth - 1 ) / std::max( 1, width - 1 ) : 0.0f;
            float scaleY = ( segHeight > 1 ) ? static_cast<float>( segHeight - 1 ) / std::max( 1, height - 1 ) : 0.0f;

            for ( int y = 0; y < height; ++y )
            {
               int sy = std::min( static_cast<int>( y * scaleY + 0.5f ), segHeight - 1 );
               for ( int x = 0; x < width; ++x )
               {
                  int sx = std::min( static_cast<int>( x * scaleX + 0.5f ), segWidth - 1 );
                  segImage.Pixel( x, y, 0 ) = static_cast<float>( segmentationMap[sy][sx] ) /
                     static_cast<float>( static_cast<int>( RegionClass::Count ) - 1 );
               }
            }

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

bool NukeXStackInstance::RunIntegrationStreaming(
   FrameStreamer& streamer,
   const FITSKeywordArray& keywords,
   Image& output,
   IntegrationSummary& summary ) const
{
   Console console;

   summary.totalFrames = static_cast<int>( p_inputFrames.size() );
   summary.enabledFrames = streamer.NumFrames();

   if ( streamer.NumFrames() == 0 )
      return false;

   int width = streamer.Width();
   int height = streamer.Height();
   int channels = streamer.Channels();

   // Build pixel selector configuration
   PixelSelectorConfig selectorConfig = BuildSelectorConfig();
   PixelSelector selector( selectorConfig );

   // === Preprocessing: Background normalization (streaming) ===
   if ( p_enableNormalization && streamer.NumFrames() > 1 )
   {
      console.WriteLn( "<br>Computing streaming background normalization..." );
      auto normParams = ComputeNormalizationParamsStreaming( streamer );

      // Log reference frame stats and scale range
      console.WriteLn( String().Format(
         "  Reference (frame 1): median=%.6f, MAD=%.6f",
         normParams[0].median, normParams[0].mad ) );

      double minScale = normParams[0].scale;
      double maxScale = normParams[0].scale;
      for ( size_t f = 1; f < normParams.size(); ++f )
      {
         minScale = std::min( minScale, normParams[f].scale );
         maxScale = std::max( maxScale, normParams[f].scale );
      }
      console.WriteLn( String().Format(
         "  Normalization scales range: %.2f - %.2f", minScale, maxScale ) );

      // Convert FrameNormalizationParams (double) to FrameNormalization (float) for PixelSelector
      std::vector<FrameNormalization> norms( normParams.size() );
      for ( size_t f = 0; f < normParams.size(); ++f )
      {
         norms[f].scale  = static_cast<float>( normParams[f].scale );
         norms[f].offset = static_cast<float>( normParams[f].offset );
      }
      selector.SetFrameNormalization( norms );

      summary.normalizedFrames = streamer.NumFrames();
   }

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
      console.WriteLn( "<br>Creating streaming median reference for segmentation..." );

      Image referenceImage = CreateMedianReferenceStreaming( streamer );

      // Reset file handles after median reference pass so pixel selection
      // can read from the beginning of each file
      if ( !streamer.ResetAllFiles() )
      {
         console.CriticalLn( "Failed to reset frame streamer after median reference." );
         return false;
      }

      if ( referenceImage.Width() > 0 )
      {
         console.WriteLn( String().Format( "Reference image: %dx%d",
            referenceImage.Width(), referenceImage.Height() ) );

         double segTime = 0.0;
         if ( RunSegmentation( referenceImage, segmentationMap, confidenceMap, segTime ) )
         {
            summary.segmentationTimeMs = segTime;
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

   // Allocate output image
   output.AllocateData( width, height, channels,
      channels >= 3 ? ColorSpace::RGB : ColorSpace::Gray );

   // Process each channel using streaming
   console.WriteLn( "<br>Selecting pixels (streaming)..." );
   auto selectionStart = std::chrono::high_resolution_clock::now();

   for ( int c = 0; c < channels; ++c )
   {
      // Reset file handles before each channel (except first)
      // to ensure clean read state for sequential row reads
      if ( c > 0 )
      {
         if ( !streamer.ResetAllFiles() )
         {
            console.CriticalLn( String().Format(
               "Failed to reset frame streamer for channel %d.", c ) );
            return false;
         }
      }

      console.Write( String().Format( "\rProcessing channel %d of %d...", c + 1, channels ) );
      console.Flush();

      if ( p_generateMetadata )
      {
         std::vector<PixelSelectionResult> metadata;
         Image channelResult = selector.ProcessStackWithMetadata( streamer, c, metadata );

         for ( int y = 0; y < height; ++y )
            for ( int x = 0; x < width; ++x )
               output.Pixel( x, y, c ) = channelResult.Pixel( x, y, 0 );

         for ( const auto& result : metadata )
            if ( result.outlierCount > 0 )
               summary.outlierPixels++;
      }
      else
      {
         Image channelResult = selector.ProcessStack( streamer, c );

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

   // Transition smoothing (identical to in-memory path - works on output image)
   if ( p_enableTransitionSmoothing )
   {
      console.WriteLn( "<br>Checking for hard transitions..." );
      auto smoothStart = std::chrono::high_resolution_clock::now();

      TransitionCheckerConfig transConfig = BuildTransitionConfig();
      TransitionChecker checker( transConfig );

      for ( int c = 0; c < channels; ++c )
      {
         Image segImage;
         if ( !segmentationMap.empty() )
         {
            int segHeight = static_cast<int>( segmentationMap.size() );
            int segWidth = segHeight > 0 ? static_cast<int>( segmentationMap[0].size() ) : 0;

            segImage.AllocateData( width, height, 1 );
            float scaleX = ( segWidth > 1 ) ? static_cast<float>( segWidth - 1 ) / std::max( 1, width - 1 ) : 0.0f;
            float scaleY = ( segHeight > 1 ) ? static_cast<float>( segHeight - 1 ) / std::max( 1, height - 1 ) : 0.0f;

            for ( int y = 0; y < height; ++y )
            {
               int sy = std::min( static_cast<int>( y * scaleY + 0.5f ), segHeight - 1 );
               for ( int x = 0; x < width; ++x )
               {
                  int sx = std::min( static_cast<int>( x * scaleX + 0.5f ), segWidth - 1 );
                  segImage.Pixel( x, y, 0 ) = static_cast<float>( segmentationMap[sy][sx] ) /
                     static_cast<float>( static_cast<int>( RegionClass::Count ) - 1 );
               }
            }

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

RegistrationSummary NukeXStackInstance::RegisterAllFrames( std::vector<Image>& frames ) const
{
   Console console;

   FrameRegistrationConfig config;
   config.maxStars = 200;
   config.sensitivity = 0.5;
   config.minMatches = 6;
   config.enablePhaseCorrelation = true;

   FrameRegistration registration( config );
   RegistrationSummary regSummary;

   registration.RegisterFrames( frames, regSummary );

   if ( regSummary.phaseCorrelationRecovered > 0 )
   {
      console.NoteLn( String().Format(
         "<br>Phase correlation recovered %d frame(s) that triangle matching could not align.",
         regSummary.phaseCorrelationRecovered ) );
   }

   return regSummary;
}

// ----------------------------------------------------------------------------

std::vector<FrameNormalizationParams> NukeXStackInstance::ComputeNormalizationParams(
   const std::vector<Image>& frames ) const
{
   Console console;
   auto startTime = std::chrono::high_resolution_clock::now();

   std::vector<FrameNormalizationParams> params( frames.size() );

   // Compute per-frame luminance median and MAD
   for ( size_t i = 0; i < frames.size(); ++i )
   {
      const Image& frame = frames[i];
      int width = frame.Width();
      int height = frame.Height();
      int channels = frame.NumberOfChannels();

      // Compute luminance values
      std::vector<float> luminance( static_cast<size_t>( width ) * height );
      for ( int y = 0; y < height; ++y )
      {
         for ( int x = 0; x < width; ++x )
         {
            size_t idx = static_cast<size_t>( y ) * width + x;
            if ( channels >= 3 )
            {
               luminance[idx] = 0.2126f * frame.Pixel( x, y, 0 )
                              + 0.7152f * frame.Pixel( x, y, 1 )
                              + 0.0722f * frame.Pixel( x, y, 2 );
            }
            else
            {
               luminance[idx] = frame.Pixel( x, y, 0 );
            }
         }
      }

      // Compute median
      std::vector<float> sorted = luminance;
      std::sort( sorted.begin(), sorted.end() );
      double median = sorted[sorted.size() / 2];

      // Compute MAD
      std::vector<float> deviations( sorted.size() );
      for ( size_t j = 0; j < sorted.size(); ++j )
         deviations[j] = std::abs( sorted[j] - static_cast<float>( median ) );
      std::sort( deviations.begin(), deviations.end() );
      double mad = deviations[deviations.size() / 2];

      params[i].median = median;
      params[i].mad = mad;
   }

   // Reference frame is frame[0]
   double refMedian = params[0].median;
   double refMAD = params[0].mad;

   for ( size_t i = 0; i < params.size(); ++i )
   {
      if ( params[i].mad > 1e-10 )
         params[i].scale = refMAD / params[i].mad;
      else
         params[i].scale = 1.0;

      params[i].offset = refMedian - params[i].median * params[i].scale;

      if ( i > 0 )
      {
         console.WriteLn( String().Format(
            "  Frame %d: median=%.6f, MAD=%.6f, scale=%.4f, offset=%+.6f",
            static_cast<int>( i + 1 ), params[i].median, params[i].mad,
            params[i].scale, params[i].offset ) );
      }
   }

   auto endTime = std::chrono::high_resolution_clock::now();
   double ms = std::chrono::duration<double, std::milli>( endTime - startTime ).count();
   console.WriteLn( String().Format( "Normalization params computed in %.1f ms", ms ) );

   return params;
}

// ----------------------------------------------------------------------------

void NukeXStackInstance::NormalizeFrames(
   std::vector<Image>& frames,
   const std::vector<FrameNormalizationParams>& params ) const
{
   for ( size_t i = 0; i < frames.size(); ++i )
   {
      if ( i >= params.size() )
         break;

      double scale = params[i].scale;
      double offset = params[i].offset;

      // Skip reference frame (scale=1, offset=0)
      if ( std::abs( scale - 1.0 ) < 1e-10 && std::abs( offset ) < 1e-10 )
         continue;

      Image& frame = frames[i];
      int channels = frame.NumberOfChannels();

      for ( int c = 0; c < channels; ++c )
      {
         Image::sample* data = frame.PixelData( c );
         size_t numPx = static_cast<size_t>( frame.Width() ) * frame.Height();
         for ( size_t p = 0; p < numPx; ++p )
         {
            double val = data[p] * scale + offset;
            data[p] = static_cast<Image::sample>( std::max( 0.0, std::min( 1.0, val ) ) );
         }
      }
   }
}

// ----------------------------------------------------------------------------

std::vector<FrameNormalizationParams> NukeXStackInstance::ComputeNormalizationParamsStreaming(
   FrameStreamer& streamer ) const
{
   Console console;
   int numFrames = streamer.NumFrames();
   int width = streamer.Width();
   int height = streamer.Height();
   int channels = streamer.Channels();

   std::vector<FrameNormalizationParams> params( numFrames );

   // Sample ~100 rows evenly distributed across the image
   int sampleRows = std::min( 100, height );
   int rowStep = std::max( 1, height / sampleRows );

   // Collect samples per frame
   std::vector<std::vector<float>> frameSamples( numFrames );
   for ( auto& v : frameSamples )
      v.reserve( static_cast<size_t>( sampleRows ) * width );

   for ( int row = 0; row < height; row += rowStep )
   {
      std::vector<std::vector<float>> rowData;
      // Read channel 0 (or compute luminance from RGB rows)
      if ( !streamer.ReadRow( row, 0, rowData ) )
         break;

      for ( int f = 0; f < numFrames; ++f )
      {
         for ( int x = 0; x < width; ++x )
            frameSamples[f].push_back( rowData[f][x] );
      }
   }

   // Reset streamer for subsequent use
   streamer.ResetAllFiles();

   // Compute median and MAD from samples
   for ( int f = 0; f < numFrames; ++f )
   {
      std::vector<float>& samples = frameSamples[f];
      if ( samples.empty() )
         continue;

      std::sort( samples.begin(), samples.end() );
      double median = samples[samples.size() / 2];

      std::vector<float> deviations( samples.size() );
      for ( size_t j = 0; j < samples.size(); ++j )
         deviations[j] = std::abs( samples[j] - static_cast<float>( median ) );
      std::sort( deviations.begin(), deviations.end() );
      double mad = deviations[deviations.size() / 2];

      params[f].median = median;
      params[f].mad = mad;
   }

   // Compute scale/offset relative to frame[0]
   double refMedian = params[0].median;
   double refMAD = params[0].mad;

   for ( int f = 0; f < numFrames; ++f )
   {
      if ( params[f].mad > 1e-10 )
         params[f].scale = refMAD / params[f].mad;
      else
         params[f].scale = 1.0;

      params[f].offset = refMedian - params[f].median * params[f].scale;
   }

   console.WriteLn( String().Format(
      "Streaming normalization: sampled %d rows, %d frames", sampleRows, numFrames ) );

   return params;
}

// ----------------------------------------------------------------------------

std::vector<float> NukeXStackInstance::ComputeQualityWeights(
   const std::vector<Image>& frames ) const
{
   Console console;
   auto startTime = std::chrono::high_resolution_clock::now();

   std::vector<FrameQualityMetrics> metrics( frames.size() );

   FrameRegistrationConfig regConfig;
   regConfig.maxStars = 200;
   FrameRegistration registration( regConfig );

   for ( size_t i = 0; i < frames.size(); ++i )
   {
      const Image& frame = frames[i];
      int width = frame.Width();
      int height = frame.Height();

      // 1. Detect stars and get count
      std::vector<DetectedStar> stars = registration.DetectStarsInFrame( frame, 200 );
      metrics[i].starCount = static_cast<int>( stars.size() );

      // 2. Compute median FWHM from top 20 stars
      int fwhmCount = std::min( 20, static_cast<int>( stars.size() ) );
      if ( fwhmCount > 0 )
      {
         std::vector<float> fwhmValues;
         fwhmValues.reserve( fwhmCount );

         for ( int s = 0; s < fwhmCount; ++s )
         {
            PSFParameters psf = PSFModel::FitToStar( frame,
               static_cast<float>( stars[s].x ),
               static_cast<float>( stars[s].y ),
               20.0f );

            if ( psf.fwhm > 0.5f && psf.fwhm < 50.0f )
               fwhmValues.push_back( psf.fwhm );
         }

         if ( !fwhmValues.empty() )
         {
            std::sort( fwhmValues.begin(), fwhmValues.end() );
            metrics[i].medianFWHM = fwhmValues[fwhmValues.size() / 2];
         }
      }

      // 3. Noise estimate from luminance MAD
      {
         std::vector<float> luminance( static_cast<size_t>( width ) * height );
         int channels = frame.NumberOfChannels();
         for ( int y = 0; y < height; ++y )
         {
            for ( int x = 0; x < width; ++x )
            {
               size_t idx = static_cast<size_t>( y ) * width + x;
               if ( channels >= 3 )
                  luminance[idx] = 0.2126f * frame.Pixel( x, y, 0 )
                                 + 0.7152f * frame.Pixel( x, y, 1 )
                                 + 0.0722f * frame.Pixel( x, y, 2 );
               else
                  luminance[idx] = frame.Pixel( x, y, 0 );
            }
         }

         std::sort( luminance.begin(), luminance.end() );
         float median = luminance[luminance.size() / 2];

         std::vector<float> deviations( luminance.size() );
         for ( size_t j = 0; j < luminance.size(); ++j )
            deviations[j] = std::abs( luminance[j] - median );
         std::sort( deviations.begin(), deviations.end() );
         metrics[i].noiseEstimate = deviations[deviations.size() / 2] * 1.4826;
      }

      console.WriteLn( String().Format(
         "  Frame %d: stars=%d, FWHM=%.2f px, noise=%.6f",
         static_cast<int>( i + 1 ), metrics[i].starCount,
         metrics[i].medianFWHM, metrics[i].noiseEstimate ) );
   }

   // Find best values
   double bestFWHM = 1e10;
   double bestNoise = 1e10;
   for ( const auto& m : metrics )
   {
      if ( m.medianFWHM > 0 && m.medianFWHM < bestFWHM )
         bestFWHM = m.medianFWHM;
      if ( m.noiseEstimate > 0 && m.noiseEstimate < bestNoise )
         bestNoise = m.noiseEstimate;
   }

   // Compute combined weights
   std::vector<float> weights( frames.size(), 1.0f );
   double maxWeight = 0.0;

   for ( size_t i = 0; i < frames.size(); ++i )
   {
      double fwhmWeight = ( metrics[i].medianFWHM > 0 )
         ? bestFWHM / metrics[i].medianFWHM : 1.0;
      double noiseWeight = ( metrics[i].noiseEstimate > 0 )
         ? bestNoise / metrics[i].noiseEstimate : 1.0;

      double combined = std::sqrt( fwhmWeight * noiseWeight );
      combined = std::max( 0.1, std::min( 1.0, combined ) );

      weights[i] = static_cast<float>( combined );
      if ( combined > maxWeight )
         maxWeight = combined;
   }

   // Normalize so max = 1.0
   if ( maxWeight > 0 )
   {
      for ( auto& w : weights )
         w = static_cast<float>( w / maxWeight );
   }

   auto endTime = std::chrono::high_resolution_clock::now();
   double ms = std::chrono::duration<double, std::milli>( endTime - startTime ).count();

   console.WriteLn( String().Format( "Quality weights computed in %.1f ms:", ms ) );
   for ( size_t i = 0; i < weights.size(); ++i )
   {
      console.WriteLn( String().Format( "  Frame %d: weight=%.3f",
         static_cast<int>( i + 1 ), weights[i] ) );
   }

   return weights;
}

// ----------------------------------------------------------------------------

void NukeXStackInstance::ExcludeFailedFrames(
   std::vector<Image>& frames,
   std::vector<String>& framePaths,
   const RegistrationSummary& regSummary ) const
{
   Console console;

   // Identify which frames to keep.
   // Avoid vector::erase on Image objects — shifting PCL Images in-place
   // via move/copy assignment can segfault. Instead, build new vectors.
   std::vector<bool> keep( frames.size(), true );

   for ( size_t i = 1; i < regSummary.perFrame.size() && i < frames.size(); ++i )
   {
      if ( !regSummary.perFrame[i].success )
      {
         keep[i] = false;
         console.WarningLn( String().Format(
            "  Excluding frame %d (failed registration: %s)",
            static_cast<int>( i + 1 ),
            IsoString( regSummary.perFrame[i].message ).c_str() ) );
      }
   }

   // Build new vectors with only kept frames
   std::vector<Image> keptFrames;
   std::vector<String> keptPaths;
   size_t keptCount = 0;
   for ( size_t i = 0; i < frames.size(); ++i )
      if ( keep[i] ) ++keptCount;
   keptFrames.reserve( keptCount );
   keptPaths.reserve( keptCount );

   for ( size_t i = 0; i < frames.size(); ++i )
   {
      if ( keep[i] )
      {
         keptFrames.push_back( std::move( frames[i] ) );
         if ( i < framePaths.size() )
            keptPaths.push_back( std::move( framePaths[i] ) );
      }
   }

   frames = std::move( keptFrames );
   framePaths = std::move( keptPaths );

   console.WriteLn( String().Format(
      "  %d frames remaining after excluding failed registrations.",
      static_cast<int>( frames.size() ) ) );
}

// ----------------------------------------------------------------------------

} // namespace pcl
