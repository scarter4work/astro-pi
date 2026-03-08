//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX v3 - Statistical Stacking + Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "NukeXStackInstance.h"
#include "NukeXStackParameters.h"
#include "engine/FrameAligner.h"

#include <pcl/MetaModule.h>
#include <pcl/Console.h>
#include <pcl/StatusMonitor.h>
#include <pcl/StandardStatus.h>
#include <pcl/ImageWindow.h>
#include <pcl/View.h>
#include <pcl/Image.h>
#include <pcl/ErrorHandler.h>

namespace pcl
{

// ----------------------------------------------------------------------------

NukeXStackInstance::NukeXStackInstance( const MetaProcess* m )
   : ProcessImplementation( m )
   , p_qualityWeightMode( NXSQualityWeightMode::Default )
   , p_generateProvenance( TheNXSGenerateProvenanceParameter->DefaultValue() )
   , p_generateDistMetadata( TheNXSGenerateDistMetadataParameter->DefaultValue() )
   , p_enableQualityWeighting( TheNXSEnableQualityWeightingParameter->DefaultValue() )
   , p_enableAutoStretch( TheNXSEnableAutoStretchParameter->DefaultValue() )
   , p_outlierSigmaThreshold( static_cast<float>( TheNXSOutlierSigmaThresholdParameter->DefaultValue() ) )
   , p_fwhmWeight( static_cast<float>( TheNXSFWHMWeightParameter->DefaultValue() ) )
   , p_eccentricityWeight( static_cast<float>( TheNXSEccentricityWeightParameter->DefaultValue() ) )
   , p_skyBackgroundWeight( static_cast<float>( TheNXSSkyBackgroundWeightParameter->DefaultValue() ) )
   , p_hfrWeight( static_cast<float>( TheNXSHFRWeightParameter->DefaultValue() ) )
   , p_altitudeWeight( static_cast<float>( TheNXSAltitudeWeightParameter->DefaultValue() ) )
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
      p_qualityWeightMode       = x->p_qualityWeightMode;
      p_generateProvenance      = x->p_generateProvenance;
      p_generateDistMetadata    = x->p_generateDistMetadata;
      p_enableQualityWeighting  = x->p_enableQualityWeighting;
      p_enableAutoStretch       = x->p_enableAutoStretch;
      p_outlierSigmaThreshold   = x->p_outlierSigmaThreshold;
      p_fwhmWeight              = x->p_fwhmWeight;
      p_eccentricityWeight      = x->p_eccentricityWeight;
      p_skyBackgroundWeight     = x->p_skyBackgroundWeight;
      p_hfrWeight               = x->p_hfrWeight;
      p_altitudeWeight          = x->p_altitudeWeight;
   }
}

// ----------------------------------------------------------------------------

bool NukeXStackInstance::Validate( String& info )
{
   int enabledCount = 0;
   for ( const auto& frame : p_inputFrames )
      if ( frame.enabled )
         ++enabledCount;

   if ( enabledCount < 2 )
   {
      info = "At least 2 enabled input frames are required for stacking.";
      return false;
   }
   return true;
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
   try
   {
      Console().WriteLn( "<end><cbr><br>NukeX v3 — Per-Pixel Statistical Inference Stacking" );
      Console().WriteLn( String().Format( "Processing %d input frames", p_inputFrames.size() ) );

      // Collect enabled frame paths
      std::vector<nukex::FramePath> framePaths;
      for ( const auto& frame : p_inputFrames )
         if ( frame.enabled )
            framePaths.push_back( { frame.path, true } );

      if ( framePaths.size() < 2 )
         throw Error( "At least 2 enabled frames are required." );

      // Phase 1: Load frames
      Console().WriteLn( String().Format( "<br>Phase 1: Loading %d frames...", framePaths.size() ) );
      Console().Flush();
      Module->ProcessEvents();

      nukex::LoadedFrames raw = nukex::FrameLoader::LoadRaw( framePaths );

      Module->ProcessEvents();

      // Phase 1b: Align frames
      Console().WriteLn( "<br>Phase 1b: Aligning frames..." );
      Console().Flush();
      Module->ProcessEvents();

      std::vector<const float*> framePtrs;
      for ( const auto& f : raw.pixelData )
         framePtrs.push_back( f[0].data() );  // align on channel 0

      nukex::AlignmentOutput aligned = nukex::alignFrames(
         framePtrs, raw.width, raw.height );

      Module->ProcessEvents();

      Console().WriteLn( String().Format( "  Aligned %d frames, crop: %dx%d (from %dx%d)",
         int( aligned.offsets.size() ),
         aligned.crop.width(), aligned.crop.height(),
         raw.width, raw.height ) );

      // Copy metadata into aligned cube
      for ( size_t i = 0; i < raw.metadata.size(); ++i )
         aligned.alignedCube.setMetadata( i, raw.metadata[i] );

      nukex::SubCube cube = std::move( aligned.alignedCube );

      // Free raw frame data — no longer needed, reclaims ~N*W*H*4 bytes
      raw.pixelData.clear();
      raw.pixelData.shrink_to_fit();

      // Phase 2: Compute quality weights
      Console().WriteLn( "<br>Phase 2: Computing quality weights..." );
      Console().Flush();
      Module->ProcessEvents();
      std::vector<double> weights;
      if ( p_enableQualityWeighting )
      {
         nukex::WeightConfig wcfg;
         wcfg.fwhmWeight           = p_fwhmWeight;
         wcfg.eccentricityWeight   = p_eccentricityWeight;
         wcfg.skyBackgroundWeight  = p_skyBackgroundWeight;
         wcfg.hfrWeight            = p_hfrWeight;
         wcfg.altitudeWeight       = p_altitudeWeight;

         std::vector<nukex::SubMetadata> metaVec;
         for ( size_t z = 0; z < cube.numSubs(); ++z )
            metaVec.push_back( cube.metadata( z ) );

         weights = nukex::ComputeQualityWeights( metaVec, wcfg );
      }
      else
      {
         weights.assign( cube.numSubs(), 1.0 / cube.numSubs() );
      }

      // Phase 3: Per-pixel distribution fitting and selection
      Console().WriteLn( "<br>Phase 3: Per-pixel statistical inference..." );
      Console().WriteLn( String().Format( "  Image: %d x %d, %d subs", int( cube.width() ), int( cube.height() ), int( cube.numSubs() ) ) );
      Console().Flush();
      Module->ProcessEvents();

      nukex::PixelSelector::Config selConfig;
      selConfig.maxOutliers = static_cast<int>( p_outlierSigmaThreshold );

      nukex::PixelSelector selector( selConfig );
      std::vector<float> resultPixels = selector.processImage( cube, weights );

      Module->ProcessEvents();

      // Phase 4: Create output image
      Console().WriteLn( "<br>Phase 4: Creating output image..." );
      Console().Flush();
      Module->ProcessEvents();
      int w = static_cast<int>( cube.width() );
      int h = static_cast<int>( cube.height() );

      ImageWindow window( w, h, 1, 32, true, false, "NukeX_stack" );
      if ( window.IsNull() )
         throw Error( "Failed to create output image window." );

      View mainView = window.MainView();
      ImageVariant v = mainView.Image();

      // Copy result pixels into the output image
      Image& outputImage = static_cast<Image&>( *v );
      for ( int y = 0; y < h; ++y )
         for ( int x = 0; x < w; ++x )
            outputImage.Pixel( x, y ) = resultPixels[y * w + x];

      window.Show();
      window.ZoomToFit();

      Console().WriteLn( "<br>NukeX stacking complete." );
      return true;
   }
   catch ( const std::bad_alloc& e )
   {
      Console().CriticalLn( "NukeX: Out of memory — " + String( e.what() ) );
      return false;
   }
   catch ( const ProcessAborted& )
   {
      throw; // re-throw abort
   }
   catch ( const Error& e )
   {
      Console().CriticalLn( "NukeX: " + e.Message() );
      return false;
   }
   catch ( const std::exception& e )
   {
      Console().CriticalLn( "NukeX: " + String( e.what() ) );
      return false;
   }
   catch ( ... )
   {
      Console().CriticalLn( "NukeX: Unknown error during stacking." );
      return false;
   }
}

// ----------------------------------------------------------------------------

void* NukeXStackInstance::LockParameter( const MetaParameter* p, size_type tableRow )
{
   if ( p == TheNXSInputFramePathParameter )
   {
      if ( tableRow >= p_inputFrames.size() )
         return nullptr;
      return p_inputFrames[tableRow].path.Begin();
   }
   if ( p == TheNXSInputFrameEnabledParameter )
   {
      if ( tableRow >= p_inputFrames.size() )
         return nullptr;
      return &p_inputFrames[tableRow].enabled;
   }
   if ( p == TheNXSQualityWeightModeParameter )
      return &p_qualityWeightMode;
   if ( p == TheNXSGenerateProvenanceParameter )
      return &p_generateProvenance;
   if ( p == TheNXSGenerateDistMetadataParameter )
      return &p_generateDistMetadata;
   if ( p == TheNXSEnableQualityWeightingParameter )
      return &p_enableQualityWeighting;
   if ( p == TheNXSEnableAutoStretchParameter )
      return &p_enableAutoStretch;
   if ( p == TheNXSOutlierSigmaThresholdParameter )
      return &p_outlierSigmaThreshold;
   if ( p == TheNXSFWHMWeightParameter )
      return &p_fwhmWeight;
   if ( p == TheNXSEccentricityWeightParameter )
      return &p_eccentricityWeight;
   if ( p == TheNXSSkyBackgroundWeightParameter )
      return &p_skyBackgroundWeight;
   if ( p == TheNXSHFRWeightParameter )
      return &p_hfrWeight;
   if ( p == TheNXSAltitudeWeightParameter )
      return &p_altitudeWeight;

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
   {
      if ( tableRow >= p_inputFrames.size() )
         return 0;
      return p_inputFrames[tableRow].path.Length();
   }

   return 0;
}

// ----------------------------------------------------------------------------

} // namespace pcl
