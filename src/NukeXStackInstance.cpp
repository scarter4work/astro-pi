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
#include "engine/FlatCalibrator.h"
#include "engine/FrameAligner.h"
#include "engine/AutoStretchSelector.h"
#include "engine/StretchLibrary.h"
#include "engine/ArtifactDetector.h"
#include "engine/DustCorrector.h"

#include <pcl/MetaModule.h>
#include <pcl/Console.h>
#include <pcl/StatusMonitor.h>
#include <pcl/StandardStatus.h>
#include <pcl/ImageWindow.h>
#include <pcl/View.h>
#include <pcl/Image.h>
#include <pcl/ErrorHandler.h>

#ifdef NUKEX_HAS_CUDA
#include "engine/cuda/CudaRuntime.h"
#include "engine/cuda/CudaRemediation.h"
#endif

#include <chrono>
#include <algorithm>
#include <cmath>
#include <omp.h>

namespace pcl
{

// ----------------------------------------------------------------------------

NukeXStackInstance::NukeXStackInstance( const MetaProcess* m )
   : ProcessImplementation( m )
   , p_generateProvenance( TheNXSGenerateProvenanceParameter->DefaultValue() )
   , p_generateDistMetadata( TheNXSGenerateDistMetadataParameter->DefaultValue() )
   , p_enableMetadataTiebreaker( TheNXSEnableMetadataTiebreakerParameter->DefaultValue() )
   , p_enableAutoStretch( TheNXSEnableAutoStretchParameter->DefaultValue() )
   , p_useGPU( TheNXSUseGPUParameter->DefaultValue() )
   , p_adaptiveModels( TheNXSAdaptiveModelsParameter->DefaultValue() )
   , p_enableRemediation( TheNXSEnableRemediationParameter->DefaultValue() )
   , p_enableTrailRemediation( TheNXSEnableTrailRemediationParameter->DefaultValue() )
   , p_enableDustRemediation( TheNXSEnableDustRemediationParameter->DefaultValue() )
   , p_enableVignettingRemediation( TheNXSEnableVignettingRemediationParameter->DefaultValue() )
   , p_outlierSigmaThreshold( static_cast<float>( TheNXSOutlierSigmaThresholdParameter->DefaultValue() ) )
   , p_trailDilateRadius( static_cast<float>( TheNXSTrailDilateRadiusParameter->DefaultValue() ) )
   , p_trailOutlierSigma( static_cast<float>( TheNXSTrailOutlierSigmaParameter->DefaultValue() ) )
   , p_dustCircularityMin( static_cast<float>( TheNXSDustCircularityMinParameter->DefaultValue() ) )
   , p_dustDetectionSigma( static_cast<float>( TheNXSDustDetectionSigmaParameter->DefaultValue() ) )
   , p_dustMaxCorrectionRatio( static_cast<float>( TheNXSDustMaxCorrectionRatioParameter->DefaultValue() ) )
   , p_vignettingMaxCorrection( static_cast<float>( TheNXSVignettingMaxCorrectionParameter->DefaultValue() ) )
   , p_dustMinDiameter( static_cast<int32>( TheNXSDustMinDiameterParameter->DefaultValue() ) )
   , p_dustMaxDiameter( static_cast<int32>( TheNXSDustMaxDiameterParameter->DefaultValue() ) )
   , p_dustNeighborRadius( static_cast<int32>( TheNXSDustNeighborRadiusParameter->DefaultValue() ) )
   , p_vignettingPolyOrder( static_cast<int32>( TheNXSVignettingPolyOrderParameter->DefaultValue() ) )
   , p_bortleNumber( static_cast<int32>( TheNXSBortleNumberParameter->DefaultValue() ) )
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
      p_flatFrames              = x->p_flatFrames;
      p_generateProvenance      = x->p_generateProvenance;
      p_generateDistMetadata    = x->p_generateDistMetadata;
      p_enableMetadataTiebreaker = x->p_enableMetadataTiebreaker;
      p_enableAutoStretch       = x->p_enableAutoStretch;
      p_useGPU                  = x->p_useGPU;
      p_adaptiveModels          = x->p_adaptiveModels;
      p_enableRemediation             = x->p_enableRemediation;
      p_enableTrailRemediation        = x->p_enableTrailRemediation;
      p_enableDustRemediation         = x->p_enableDustRemediation;
      p_enableVignettingRemediation   = x->p_enableVignettingRemediation;
      p_outlierSigmaThreshold   = x->p_outlierSigmaThreshold;
      p_trailDilateRadius       = x->p_trailDilateRadius;
      p_trailOutlierSigma       = x->p_trailOutlierSigma;
      p_dustCircularityMin      = x->p_dustCircularityMin;
      p_dustDetectionSigma      = x->p_dustDetectionSigma;
      p_dustMaxCorrectionRatio  = x->p_dustMaxCorrectionRatio;
      p_dustMinDiameter         = x->p_dustMinDiameter;
      p_dustMaxDiameter         = x->p_dustMaxDiameter;
      p_dustNeighborRadius      = x->p_dustNeighborRadius;
      p_vignettingPolyOrder     = x->p_vignettingPolyOrder;
      p_vignettingMaxCorrection = x->p_vignettingMaxCorrection;
      p_bortleNumber            = x->p_bortleNumber;
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
      auto t0 = std::chrono::steady_clock::now();

      Console console;
      console.WriteLn( "<end><cbr>"
         "\n\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
         "\n  NukeX v3 \xe2\x80\x94 Per-Pixel Statistical Inference Stacking"
         "\n\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90" );

      // Collect enabled frame paths
      std::vector<nukex::FramePath> framePaths;
      for ( const auto& frame : p_inputFrames )
         if ( frame.enabled )
            framePaths.push_back( { frame.path, true } );

      if ( framePaths.size() < 2 )
         throw Error( "At least 2 enabled frames are required." );

      // Phase 1: Load frames
      auto tPhase1 = std::chrono::steady_clock::now();
      console.WriteLn( String().Format( "\nPhase 1: Loading %d frames...", int( framePaths.size() ) ) );
      console.Flush();
      Module->ProcessEvents();

      nukex::LoadedFrames raw = nukex::FrameLoader::LoadRaw( framePaths );
      int numChannels = raw.numChannels;

      auto elapsed1 = std::chrono::duration<double>( std::chrono::steady_clock::now() - tPhase1 ).count();
      console.WriteLn( String().Format( "  Loaded %d frames (%d x %d, %d ch) in %.1fs",
         int( framePaths.size() ), raw.width, raw.height, numChannels, elapsed1 ) );

      Module->ProcessEvents();

      // Phase 1a: Flat calibration (optional)
      if ( !p_flatFrames.empty() )
      {
         console.WriteLn( String().Format( "\nPhase 1a: Flat calibration (%d flat frames)...",
            int( p_flatFrames.size() ) ) );
         console.Flush();
         Module->ProcessEvents();

         // Load flat frames using the same loader (they get debayered like lights)
         std::vector<nukex::FramePath> flatPaths;
         for ( const auto& fp : p_flatFrames )
            flatPaths.push_back( { fp, true } );

         nukex::LoadedFrames flatRaw = nukex::FrameLoader::LoadRaw( flatPaths );

         // Build master flat
         nukex::FlatCalibrator flatCal;
         for ( size_t i = 0; i < flatRaw.pixelData.size(); ++i )
         {
            flatCal.addFrame(
               flatRaw.pixelData[i][0].data(),   // R
               flatRaw.pixelData[i][1].data(),   // G
               flatRaw.pixelData[i][2].data(),   // B
               flatRaw.width, flatRaw.height );
         }

         flatCal.buildMasterFlat(
            [&console]( const std::string& msg ) {
               console.WriteLn( String( msg.c_str() ) );
            } );

         // Apply flat to each light frame
         if ( flatCal.isReady() )
         {
            for ( size_t f = 0; f < raw.pixelData.size(); ++f )
            {
               flatCal.calibrate(
                  raw.pixelData[f][0].data(),   // R
                  raw.pixelData[f][1].data(),   // G
                  raw.pixelData[f][2].data(),   // B
                  raw.width, raw.height );
            }
            console.WriteLn( String().Format( "  Flat calibration applied to %d light frames",
               int( raw.pixelData.size() ) ) );
         }
         else
         {
            console.WarningLn( "  Flat calibration failed \xe2\x80\x94 proceeding without flat correction" );
         }

         Module->ProcessEvents();
      }

      // Phase 1b: Align (channel 0 only)
      auto tPhase1b = std::chrono::steady_clock::now();
      console.WriteLn( "\nPhase 1b: Aligning frames..." );
      console.Flush();
      Module->ProcessEvents();

      std::vector<const float*> framePtrs;
      for ( const auto& f : raw.pixelData )
         framePtrs.push_back( f[0].data() );

      nukex::AlignmentOutput aligned = nukex::alignFrames(
         framePtrs, raw.width, raw.height );

      int rejectedCount = 0;
      for ( size_t i = 0; i < aligned.offsets.size(); ++i )
      {
         const auto& o = aligned.offsets[i];
         if ( o.valid )
         {
            if ( std::abs( o.rotation ) > 0.1 || o.flipped )
            {
               console.WriteLn( String().Format(
                  "  [%d/%d] dx=%+d, dy=%+d, rot=%.1f\xc2\xb0%s (%d stars, RMS=%.2f)\n",
                  int( i + 1 ), int( aligned.offsets.size() ),
                  o.dx, o.dy, o.rotation, o.flipped ? " FLIP" : "",
                  o.numMatchedStars, o.convergenceRMS ) );
            }
            else
            {
               console.WriteLn( String().Format( "  [%d/%d] dx=%+d, dy=%+d (%d stars, RMS=%.2f)\n",
                  int( i + 1 ), int( aligned.offsets.size() ),
                  o.dx, o.dy, o.numMatchedStars, o.convergenceRMS ) );
            }
         }
         else
         {
            ++rejectedCount;
            console.WarningLn( String().Format( "  [%d/%d] rejected (RMS=%.2f, scale=%.3f)\n",
               int( i + 1 ), int( aligned.offsets.size() ),
               o.convergenceRMS, o.scale ) );
         }
         console.Flush();
         Module->ProcessEvents();
      }
      if ( rejectedCount > 0 )
         console.WarningLn( String().Format( "  %d frame(s) rejected (bad alignment or scale)\n",
            rejectedCount ) );
      console.WriteLn( String().Format( "  Crop: %d x %d (from %d x %d)",
         aligned.crop.width(), aligned.crop.height(), raw.width, raw.height ) );

      auto elapsed1b = std::chrono::duration<double>( std::chrono::steady_clock::now() - tPhase1b ).count();
      console.WriteLn( String().Format( "  Alignment complete in %.1fs", elapsed1b ) );
      Module->ProcessEvents();

      // Copy metadata into aligned cube
      for ( size_t i = 0; i < raw.metadata.size(); ++i )
         aligned.alignedCube.setMetadata( i, raw.metadata[i] );

      size_t nSubs = aligned.offsets.size();
      int cropW = aligned.crop.width();
      int cropH = aligned.crop.height();

      // Phase 2: Quality scores for metadata tiebreaker
      console.WriteLn( "\nPhase 2: Extracting quality scores..." );
      console.Flush();
      Module->ProcessEvents();

      std::vector<double> qualityScores;
      const double* qualityScoresPtr = nullptr;
      if ( p_enableMetadataTiebreaker )
      {
         qualityScores.resize( nSubs );
         for ( size_t z = 0; z < nSubs; ++z )
         {
            nukex::SubMetadata meta = aligned.alignedCube.metadata( z );
            if ( meta.fwhm > 0 || meta.eccentricity > 0 )
            {
               meta.qualityScore = 0.6 * (1.0 / (1.0 + meta.fwhm))
                                 + 0.4 * (1.0 / (1.0 + meta.eccentricity));
               aligned.alignedCube.setMetadata( z, meta );
            }
            qualityScores[z] = meta.qualityScore;
            console.WriteLn( String().Format(
               "  Sub %d: FWHM=%.2f, Ecc=%.3f, Score=%.4f",
               int( z ), meta.fwhm, meta.eccentricity, meta.qualityScore ) );
         }
         qualityScoresPtr = qualityScores.data();

         double minQ = *std::min_element( qualityScores.begin(), qualityScores.end() );
         double maxQ = *std::max_element( qualityScores.begin(), qualityScores.end() );
         console.WriteLn( String().Format( "  Metadata tiebreaker: ON | Score range: %.4f \xe2\x80\x94 %.4f",
            minQ, maxQ ) );
      }
      else
      {
         console.WriteLn( "  Metadata tiebreaker: OFF (equal preference)" );
      }
      Module->ProcessEvents();

      // Phase 3: Per-channel stacking
      auto tPhase3 = std::chrono::steady_clock::now();
      console.WriteLn( String().Format( "\nPhase 3: Per-channel stacking..." ) );
      console.WriteLn( String().Format( "  Image: %d x %d, %d subs, %d channel(s)",
         cropW, cropH, int( nSubs ), numChannels ) );
      console.Flush();
      Module->ProcessEvents();

      const char* chNames[] = { "R", "G", "B" };
      if ( numChannels == 1 )
         chNames[0] = "L";

      std::vector<std::vector<float>> channelResults( numChannels );
      std::vector<std::vector<uint8_t>> distTypeMaps( numChannels );
      std::vector<nukex::SubCube> channelCubes;
      channelCubes.reserve( numChannels );

      nukex::PixelSelector::Config selConfig;
      // Derive maxOutliers from stack depth (use all data — outlier detection
      // is only for pixel selection, never frame rejection)
      selConfig.maxOutliers = std::max( 1, static_cast<int>( nSubs ) / 3 );
      // Convert sigma threshold to ESD alpha: alpha = 2*(1 - Phi(sigma))
      // where Phi is the standard normal CDF: Phi(x) = erfc(-x/sqrt(2))/2
      {
         double sigma = static_cast<double>( p_outlierSigmaThreshold );
         double phi = std::erfc( -sigma / 1.4142135623730951 ) * 0.5;
         selConfig.outlierAlpha = std::max( 0.001, std::min( 0.5, 2.0 * (1.0 - phi) ) );
      }
      selConfig.adaptiveModels = p_adaptiveModels;
      selConfig.useGPU = p_useGPU;

      // GPU detection
      bool useGPU = false;
#ifdef NUKEX_HAS_CUDA
      if ( p_useGPU && nukex::cuda::isGpuAvailable() )
      {
         useGPU = true;
         console.WriteLn( String().Format( "  GPU: %s (%zu MB VRAM)",
            nukex::cuda::gpuName(), nukex::cuda::gpuMemoryMB() ) );
      }
#endif
      if ( !useGPU )
         console.WriteLn( String().Format( "  CPU: %d OpenMP threads",
            omp_get_max_threads() ) );
      console.WriteLn( String().Format( "  Compute: %s | Adaptive: %s",
         useGPU ? "GPU (CUDA)" : "CPU (OpenMP)",
         p_adaptiveModels ? "On" : "Off" ) );

      nukex::PixelSelector selector( selConfig );
      console.WriteLn( String().Format( "  Outlier config: maxOutliers=%d, alpha=%.4f (sigma=%.1f)\n",
         selConfig.maxOutliers, selConfig.outlierAlpha, double( p_outlierSigmaThreshold ) ) );

      // Progress bar: total rows across all channels
      size_t totalRows = size_t( numChannels ) * size_t( cropH );
      StandardStatus status;
      StatusMonitor monitor;
      monitor.SetCallback( &status );
      monitor.Initialize( "NukeX: Stacking", totalRows );

      for ( int ch = 0; ch < numChannels; ++ch )
      {
         console.WriteLn( String().Format( "  Channel %s (%d/%d):",
            chNames[ch], ch + 1, numChannels ) );
         console.Flush();
         Module->ProcessEvents();

         size_t baseRows = size_t( ch ) * size_t( cropH );

         auto progressCB = [&monitor, &console, baseRows]( size_t rowsDone, size_t /*totalRows*/ )
         {
            size_t target = baseRows + rowsDone;
            size_t current = monitor.Count();
            if ( target > current )
               monitor += ( target - current );
            console.Flush();
            Module->ProcessEvents();
         };

         if ( ch == 0 )
         {
            // Reuse the aligned cube for channel 0
            channelCubes.push_back( std::move( aligned.alignedCube ) );
         }
         else
         {
            // Build per-channel frame data and apply alignment
            std::vector<std::vector<float>> chFrameData( nSubs );
            for ( size_t f = 0; f < nSubs; ++f )
               chFrameData[f] = raw.pixelData[f][ch];

            channelCubes.push_back( nukex::applyAlignment( chFrameData, aligned.offsets,
                                                             aligned.crop, raw.width, raw.height ) );

            for ( size_t i = 0; i < raw.metadata.size(); ++i )
               channelCubes[ch].setMetadata( i, raw.metadata[i] );
         }

         // Pre-stacking trail rejection: for each pixel position, compute
         // the median across all frames. Flag any frame's pixel that exceeds
         // the median by more than 2.5σ (MAD-based). This catches satellite
         // trails, airplane lights, and cosmic rays at the frame level before
         // the statistical stacking sees them.
         {
            auto& cube = channelCubes[ch];
            if ( !cube.hasMasks() )
               cube.allocateMasks();

            int trailMasked = 0;
            for ( int y = 0; y < cropH; ++y )
               for ( int x = 0; x < cropW; ++x )
               {
                  // Collect Z-column values
                  std::vector<float> zCol( nSubs );
                  for ( size_t z = 0; z < nSubs; ++z )
                     zCol[z] = cube.pixel( z, y, x );

                  // Median
                  std::vector<float> sorted = zCol;
                  size_t mid = sorted.size() / 2;
                  std::nth_element( sorted.begin(), sorted.begin() + mid, sorted.end() );
                  float median = sorted[mid];

                  // MAD
                  std::vector<float> devs( nSubs );
                  for ( size_t z = 0; z < nSubs; ++z )
                     devs[z] = std::abs( zCol[z] - median );
                  std::nth_element( devs.begin(), devs.begin() + mid, devs.end() );
                  float mad = devs[mid] * 1.4826f;

                  // Flag bright outliers (trails are bright)
                  if ( mad > 1e-10f )
                  {
                     float threshold = median + 2.5f * mad;
                     for ( size_t z = 0; z < nSubs; ++z )
                        if ( zCol[z] > threshold )
                        {
                           cube.setMask( z, y, x, 1 );
                           ++trailMasked;
                        }
                  }
               }

            if ( ch == 0 && trailMasked > 0 )
               console.WriteLn( String().Format( "  Pre-stack rejection: %d pixel-frames masked", trailMasked ) );
         }

         if ( useGPU )
         {
            channelResults[ch] = selector.processImageGPU( channelCubes[ch], qualityScoresPtr, distTypeMaps[ch], progressCB );
            if ( selector.lastGpuFallback() )
            {
               if ( ch == 0 ) // Log once, not per-channel
                  console.WarningLn( String().Format(
                     "  GPU stacking failed: %s \xe2\x80\x94 fell back to CPU",
                     selector.lastGpuError().c_str() ) );
            }
         }
         else
         {
            channelResults[ch] = selector.processImage( channelCubes[ch], qualityScoresPtr, progressCB );

            size_t mapSize = size_t( cropH ) * size_t( cropW );
            distTypeMaps[ch].resize( mapSize );
            for ( size_t y = 0; y < size_t( cropH ); ++y )
               for ( size_t x = 0; x < size_t( cropW ); ++x )
                  distTypeMaps[ch][y * cropW + x] = channelCubes[ch].distType( y, x );
         }

         {
            size_t mapSize = size_t( cropH ) * size_t( cropW );
            size_t counts[4] = {};
            for ( uint8_t t : distTypeMaps[ch] )
               if ( t < 4 ) counts[t]++;
            console.WriteLn( String().Format(
               "    Distribution: %.0f%% Gaussian, %.0f%% Poisson, %.0f%% Skew-Normal, %.0f%% Bimodal\n",
               100.0 * counts[0] / mapSize, 100.0 * counts[1] / mapSize,
               100.0 * counts[2] / mapSize, 100.0 * counts[3] / mapSize ) );
         }

         // Log fitting fallback count for this channel
         size_t errCount = selector.lastErrorCount();
         if ( errCount > 0 )
            console.WarningLn( String().Format(
               "    %zu pixels fell back to simple mean (fitting failed)\n",
               errCount ) );
         else
            console.WriteLn( "    All pixels fitted successfully\n" );

         console.Flush();
         Module->ProcessEvents();
      }

      monitor.Complete();

      // Free raw frame data
      raw.pixelData.clear();
      raw.pixelData.shrink_to_fit();

      auto elapsed3 = std::chrono::duration<double>( std::chrono::steady_clock::now() - tPhase3 ).count();
      console.WriteLn( String().Format( "  Stacking complete in %.1fs", elapsed3 ) );

      Module->ProcessEvents();

      nukex::DustDetectionResult dustDetection;

      // Phase 4: Create linear output
      console.WriteLn( "\nPhase 4: Creating linear output..." );
      console.Flush();
      Module->ProcessEvents();

      bool isColor = ( numChannels >= 3 );
      int outChannels = isColor ? 3 : 1;

      ImageWindow window( cropW, cropH, outChannels, 32, true, isColor, "NukeX_stack" );
      if ( window.IsNull() )
         throw Error( "Failed to create output image window." );

      View mainView = window.MainView();
      ImageVariant v = mainView.Image();
      Image& outputImage = static_cast<Image&>( *v );

      for ( int ch = 0; ch < outChannels; ++ch )
      {
         int srcCh = ( ch < numChannels ) ? ch : 0;
         for ( int y = 0; y < cropH; ++y )
            for ( int x = 0; x < cropW; ++x )
               outputImage.Pixel( x, y, ch ) = channelResults[srcCh][y * cropW + x];
      }

      window.Show();
      window.ZoomToFit();
      console.WriteLn( String().Format( "  Window: NukeX_stack (%d x %d, %s)",
         cropW, cropH, isColor ? "RGB" : "Mono" ) );
      Module->ProcessEvents();

      // Phase 5 & 6: Auto-stretch (if enabled)
      if ( p_enableAutoStretch )
      {
         console.WriteLn( "\nPhase 5: Auto-stretch selection..." );
         console.Flush();
         Module->ProcessEvents();

         // Compute per-channel stats from stacked result
         std::vector<nukex::ChannelStats> chStats( outChannels );
         for ( int ch = 0; ch < outChannels; ++ch )
         {
            int srcCh = ( ch < numChannels ) ? ch : 0;
            const auto& px = channelResults[srcCh];
            size_t n = px.size();

            // Mean
            double sum = 0;
            for ( float val : px ) sum += val;
            chStats[ch].mean = sum / n;

            // Median
            std::vector<float> sorted = px;
            std::sort( sorted.begin(), sorted.end() );
            chStats[ch].median = sorted[n / 2];

            // MAD
            std::vector<float> deviations( n );
            for ( size_t i = 0; i < n; ++i )
               deviations[i] = std::abs( sorted[i] - static_cast<float>( chStats[ch].median ) );
            std::sort( deviations.begin(), deviations.end() );
            chStats[ch].mad = deviations[n / 2];
         }

         // Prepare distType maps for the output channels
         std::vector<std::vector<uint8_t>> outDistMaps( outChannels );
         for ( int ch = 0; ch < outChannels; ++ch )
         {
            int srcCh = ( ch < numChannels ) ? ch : 0;
            outDistMaps[ch] = distTypeMaps[srcCh];
         }

         auto selection = nukex::AutoStretchSelector::Select( outDistMaps, chStats );

         // Log selection
         const char* chLabels[] = { "R", "G", "B" };
         if ( !isColor ) chLabels[0] = "L";

         for ( size_t c = 0; c < selection.fractions.size(); ++c )
         {
            const auto& f = selection.fractions[c];
            console.WriteLn( String().Format(
               "  %s: %.0f%% Gaussian, %.0f%% Poisson, %.0f%% Skew-Normal, %.0f%% Bimodal",
               chLabels[c], f.gaussian * 100, f.poisson * 100,
               f.skewNormal * 100, f.bimodal * 100 ) );
         }
         console.WriteLn( String().Format( "  Channel divergence: %.2f (%s)",
            selection.channelDivergence,
            selection.channelDivergence < 0.05 ? "similar" :
            selection.channelDivergence < 0.15 ? "moderate" : "divergent" ) );
         console.WriteLn( String( "  Selected: " ) + String( selection.reason.c_str() ) );
         Module->ProcessEvents();

         // Phase 6: Apply stretch
         console.WriteLn( "\nPhase 6: Applying stretch..." );
         console.Flush();
         Module->ProcessEvents();

         AlgorithmType algoType = static_cast<AlgorithmType>(
            static_cast<int>( selection.algorithm ) );

         auto algo = StretchLibrary::Instance().Create( algoType );
         if ( algo == nullptr )
         {
            console.WarningLn( "  Warning: algorithm not available, falling back to GHS" );
            algo = StretchLibrary::Instance().Create( AlgorithmType::GHS );
         }

         console.WriteLn( String( "  Algorithm: " ) + algo->Name() );

         // Create stretched output window
         ImageWindow stretchWindow( cropW, cropH, outChannels, 32, true, isColor, "NukeX_stretched" );
         if ( stretchWindow.IsNull() )
            throw Error( "Failed to create stretched output window." );

         View stretchView = stretchWindow.MainView();
         ImageVariant sv = stretchView.Image();
         Image& stretchImage = static_cast<Image&>( *sv );

         // Copy linear data
         for ( int ch = 0; ch < outChannels; ++ch )
            for ( int y = 0; y < cropH; ++y )
               for ( int x = 0; x < cropW; ++x )
                  stretchImage.Pixel( x, y, ch ) = outputImage.Pixel( x, y, ch );

         // Ensure pixel values are in [0,1] — defense against any upstream issues
         stretchImage.Truncate();

         // Channel recombination: normalize per-channel backgrounds
         // Compute MAD BEFORE scaling so the unified shadow clip reflects
         // true per-channel noise, not noise inflated by the scaling factor.
         double preScaleMad[3] = { 0, 0, 0 };
         double preScaleMaxMad = 0;

         if ( isColor )
         {
            console.WriteLn( "\n  Channel recombination (background neutralization):" );

            double medians[3];
            for ( int ch = 0; ch < 3; ++ch )
            {
               stretchImage.SelectChannel( ch );
               medians[ch] = stretchImage.Median();
               preScaleMad[ch] = stretchImage.MAD( medians[ch] );
               preScaleMaxMad = std::max( preScaleMaxMad, preScaleMad[ch] );
            }
            stretchImage.ResetChannelRange();

            double targetMedian = ( medians[0] + medians[1] + medians[2] ) / 3.0;

            for ( int ch = 0; ch < 3; ++ch )
            {
               const char* label = ch == 0 ? "R" : ch == 1 ? "G" : "B";
               if ( medians[ch] > 1.0e-10 )
               {
                  double scale = targetMedian / medians[ch];
                  stretchImage.SelectChannel( ch );
                  stretchImage *= scale;
                  console.WriteLn( String().Format(
                     "    %s: median=%.6f, scale=%.4f", label, medians[ch], scale ) );
               }
               else
               {
                  console.WriteLn( String().Format(
                     "    %s: median=%.6f (too low, skipping)", label, medians[ch] ) );
               }
            }
            stretchImage.ResetChannelRange();
            Module->ProcessEvents();
         }

         // Save per-channel configured stretch algorithms for Phase 7d re-stretch
         std::vector<std::unique_ptr<IStretchAlgorithm>> stretchAlgos( outChannels );

         // ------------------------------------------------------------------
         // Stretch optimization: try the algorithm at different strength
         // settings on sampled pixels, score each by entropy (information
         // content), and pick the setting that reveals the most detail
         // without clipping highlights or amplifying noise.
         // ------------------------------------------------------------------

         // Sample ~10k pixels from channel 0 for optimization
         stretchImage.SelectChannel( 0 );
         double refMed = stretchImage.Median();
         double refMad = stretchImage.MAD( refMed );
         stretchImage.ResetChannelRange();

         std::vector<float> optSample;
         {
            size_t totalPx = size_t( cropW ) * size_t( cropH );
            size_t step = std::max( size_t( 1 ), totalPx / 10000 );
            for ( size_t i = 0; i < totalPx; i += step )
               optSample.push_back( stretchImage.Pixel( int( i % cropW ), int( i / cropW ), 0 ) );
         }

         // Quality scoring: apply stretch to sample, compute entropy and penalties
         auto scoreStretch = []( const std::vector<float>& sample,
                                 IStretchAlgorithm* a ) -> double
         {
            // Apply
            std::vector<double> stretched( sample.size() );
            for ( size_t i = 0; i < sample.size(); ++i )
               stretched[i] = a->Apply( static_cast<double>( sample[i] ) );

            // 256-bin histogram entropy
            int bins[256] = {};
            for ( double v : stretched )
               bins[std::min( 255, std::max( 0, int( v * 256.0 ) ) )]++;

            double entropy = 0;
            double n = static_cast<double>( stretched.size() );
            for ( int b = 0; b < 256; ++b )
            {
               if ( bins[b] > 0 )
               {
                  double p = bins[b] / n;
                  entropy -= p * std::log2( p );
               }
            }

            // Clipping penalty: fraction of pixels crushed to black or white
            int clipped = 0;
            for ( double v : stretched )
               if ( v >= 0.995 || v <= 0.005 ) ++clipped;
            double clipFrac = static_cast<double>( clipped ) / n;

            // Noise penalty: if stretched MAD is too high, we're amplifying noise
            std::sort( stretched.begin(), stretched.end() );
            double sMedian = stretched[stretched.size() / 2];
            std::vector<double> devs( stretched.size() );
            for ( size_t i = 0; i < stretched.size(); ++i )
               devs[i] = std::abs( stretched[i] - sMedian );
            std::sort( devs.begin(), devs.end() );
            double sMad = devs[devs.size() / 2];
            // Penalize if noise becomes >5% of dynamic range
            double noisePenalty = ( sMad > 0.05 ) ? ( sMad - 0.05 ) * 10.0 : 0.0;

            return entropy - 10.0 * clipFrac - noisePenalty;
         };

         // Try candidate algorithms at different strengths
         struct StretchTrial
         {
            AlgorithmType type;
            double paramValue;
            double score;
            String paramName;
         };
         std::vector<StretchTrial> trials;

         // MTF trials: vary targetMedian
         // Upper bound driven by Bortle number — darker skies allow brighter
         // backgrounds without washing out contrast.
         double mtfMaxMedian = ( p_bortleNumber <= 3 ) ? 0.25
                             : ( p_bortleNumber <= 5 ) ? 0.20
                             : ( p_bortleNumber <= 7 ) ? 0.16
                             :                           0.12;
         console.WriteLn( String().Format( "  Bortle %d: MTF target median range [0.08, %.2f]",
            int( p_bortleNumber ), mtfMaxMedian ) );
         for ( double t = 0.08; t <= mtfMaxMedian; t += 0.01 )
         {
            auto trial = StretchLibrary::Instance().Create( AlgorithmType::MTF );
            trial->SetParameter( "targetMedian", t );
            trial->AutoConfigure( refMed, refMad );
            double sc = scoreStretch( optSample, trial.get() );
            trials.push_back( { AlgorithmType::MTF, t, sc, "targetMedian" } );
         }

         // GHS trials: vary D
         {
            auto ghsBase = StretchLibrary::Instance().Create( AlgorithmType::GHS );
            ghsBase->AutoConfigure( refMed, refMad );
            double baseD = ghsBase->GetParameter( "D" );
            for ( double mult = 0.3; mult <= 1.5; mult += 0.15 )
            {
               auto trial = ghsBase->Clone();
               trial->SetParameter( "D", baseD * mult );
               double sc = scoreStretch( optSample, trial.get() );
               trials.push_back( { AlgorithmType::GHS, baseD * mult, sc, "D" } );
            }
         }

         // ArcSinh trials: vary stretchFactor
         {
            auto ashBase = StretchLibrary::Instance().Create( AlgorithmType::ArcSinh );
            ashBase->AutoConfigure( refMed, refMad );
            double baseSF = ashBase->GetParameter( "stretchFactor" );
            for ( double mult = 0.3; mult <= 1.5; mult += 0.15 )
            {
               auto trial = ashBase->Clone();
               trial->SetParameter( "stretchFactor", baseSF * mult );
               double sc = scoreStretch( optSample, trial.get() );
               trials.push_back( { AlgorithmType::ArcSinh, baseSF * mult, sc, "stretchFactor" } );
            }
         }

         // Find best trial
         auto bestIt = std::max_element( trials.begin(), trials.end(),
            []( const StretchTrial& a, const StretchTrial& b ) { return a.score < b.score; } );

         AlgorithmType bestType = bestIt->type;
         double bestParam = bestIt->paramValue;
         String bestParamName = bestIt->paramName;

         console.WriteLn( String().Format( "\n  Stretch optimization: %d trials evaluated",
            int( trials.size() ) ) );
         console.WriteLn( String().Format( "    Best: %s (%s=%.4f, score=%.2f)",
            IsoString( StretchLibrary::Instance().GetInfo( bestType ).name ).c_str(),
            IsoString( bestIt->paramName ).c_str(), bestParam, bestIt->score ) );

         // Create optimized algorithm
         algo = StretchLibrary::Instance().Create( bestType );

         // Per-channel stretch with optimized algorithm
         // Use UNIFIED shadow clip (max of per-channel MADs) to prevent
         // color bias — different MADs produce different shadow clips,
         // which makes noisier channels (R) appear brighter after stretch.
         if ( isColor )
         {
            console.WriteLn( "\n  Per-channel stretch:" );

            // Compute post-scaling medians, but use PRE-SCALING MAD for
            // unified shadow clip. This prevents the channel scaling from
            // inflating R's MAD and deepening the shadow clip for all channels.
            double chMed[3];
            for ( int ch = 0; ch < 3; ++ch )
            {
               stretchImage.SelectChannel( ch );
               chMed[ch] = stretchImage.Median();
            }
            stretchImage.ResetChannelRange();

            std::unique_ptr<IStretchAlgorithm> lastChAlgo;
            for ( int ch = 0; ch < 3; ++ch )
            {
               const char* label = ch == 0 ? "R" : ch == 1 ? "G" : "B";
               console.WriteLn( String().Format( "    %s: median=%.6f, MAD=%.6f", label, chMed[ch], preScaleMad[ch] ) );

               lastChAlgo = algo->Clone();
               lastChAlgo->SetParameter( IsoString( bestIt->paramName ), bestParam );
               // Use pre-scaling unified MAD for consistent shadow clip
               lastChAlgo->AutoConfigure( chMed[ch], preScaleMaxMad );

               Image::sample_iterator it( stretchImage, ch );
               for ( ; it; ++it )
                  *it = lastChAlgo->Apply( *it );

               stretchAlgos[ch] = lastChAlgo->Clone();
            }
            stretchImage.ResetChannelRange();

            // Log parameters
            auto params = lastChAlgo->GetParameters();
            for ( const auto& param : params )
               console.WriteLn( String().Format( "  %s = %.4f",
                  IsoString( param.name ).c_str(), param.value ) );
         }
         else
         {
            double med = stretchImage.Median();
            double mad = stretchImage.MAD( med );
            console.WriteLn( String().Format( "  Image median: %.6f, MAD: %.6f", med, mad ) );

            algo->SetParameter( IsoString( bestIt->paramName ), bestParam );
            algo->AutoConfigure( med, mad );
            stretchAlgos[0] = algo->Clone();

            auto params = algo->GetParameters();
            for ( const auto& param : params )
               console.WriteLn( String().Format( "  %s = %.4f",
                  IsoString( param.name ).c_str(), param.value ) );

            algo->ApplyToImage( stretchImage );
         }

         stretchWindow.Show();
         stretchWindow.ZoomToFit();
         console.WriteLn( String().Format( "  Window: NukeX_stretched (%d x %d, %s)",
            cropW, cropH, isColor ? "RGB" : "Mono" ) );
         Module->ProcessEvents();

         // ================================================================
         // Phase 7: Post-stretch subcube remediation
         // ================================================================

         if ( p_enableRemediation )
         {
            auto tPhase7 = std::chrono::steady_clock::now();
            console.WriteLn( "\nPhase 7: Post-stretch subcube remediation" );
            console.Flush();
            Module->ProcessEvents();

            // 7a: Detection on stretched luminance
            console.WriteLn( "  Phase 7a: Detecting artifacts in stretched image..." );

            std::vector<float> luminance( size_t( cropW ) * size_t( cropH ), 0.0f );

            if ( isColor )
            {
               for ( int y = 0; y < cropH; ++y )
                  for ( int x = 0; x < cropW; ++x )
                  {
                     float r = stretchImage.Pixel( x, y, 0 );
                     float g = stretchImage.Pixel( x, y, 1 );
                     float b = stretchImage.Pixel( x, y, 2 );
                     luminance[y * cropW + x] = ( r + g + b ) / 3.0f;
                  }
            }
            else
            {
               for ( int y = 0; y < cropH; ++y )
                  for ( int x = 0; x < cropW; ++x )
                     luminance[y * cropW + x] = stretchImage.Pixel( x, y, 0 );
            }

            nukex::ArtifactDetectorConfig detConfig;
            detConfig.trailDilateRadius   = p_trailDilateRadius;
            detConfig.trailOutlierSigma   = p_trailOutlierSigma;
            detConfig.dustMinDiameter     = p_dustMinDiameter;
            detConfig.dustMaxDiameter     = p_dustMaxDiameter;
            detConfig.dustCircularityMin  = p_dustCircularityMin;
            detConfig.dustDetectionSigma  = p_dustDetectionSigma;
            detConfig.vignettingPolyOrder = p_vignettingPolyOrder;
            detConfig.vignettingMaxCorrection = p_vignettingMaxCorrection;

            nukex::ArtifactDetector detector( detConfig );
            auto detection = detector.detectAll( luminance.data(), cropW, cropH );

            // Dust detection on stretched image, verified against subcube
            if ( p_enableDustRemediation )
            {
               std::vector<nukex::SubCube*> cubePtrs;
               for ( int ch = 0; ch < numChannels; ++ch )
                  cubePtrs.push_back( &channelCubes[ch] );

               // Pass alignment offsets so detector can build sensor-space image
               std::vector<nukex::ArtifactDetector::AlignOffset> alignOffsets;
               for ( const auto& o : aligned.offsets )
                  alignOffsets.push_back( { o.dx, o.dy } );

               dustDetection = detector.detectDustSubcube(
                  luminance.data(), cubePtrs, alignOffsets, cropW, cropH,
                  [&console]( const std::string& msg ) {
                     console.WriteLn( String( msg.c_str() ) );
                  } );
            }

            console.WriteLn( String().Format( "    Trails: %d pixels (%d lines)",
               detection.trails.trailPixelCount, detection.trails.trailLineCount ) );
            console.WriteLn( String().Format( "    Dust: %d pixels (%d verified blobs)",
               dustDetection.dustPixelCount, int( dustDetection.blobs.size() ) ) );
            console.WriteLn( String().Format( "    Vignetting: max correction %.2f",
               detection.vignetting.maxCorrection ) );
            Module->ProcessEvents();

            bool anyRemediation = ( p_enableTrailRemediation && detection.trails.trailPixelCount > 0 )
                                || ( p_enableDustRemediation && dustDetection.dustPixelCount > 0 )
                                || ( p_enableVignettingRemediation && detection.vignetting.maxCorrection > 1.01 );

            if ( anyRemediation )
            {
               // 7b: Trail remediation (per channel)
               if ( p_enableTrailRemediation && detection.trails.trailPixelCount > 0 )
               {
                  console.WriteLn( String().Format( "  Phase 7b: Trail remediation (%s, %d pixels)...",
                     useGPU ? "GPU" : "CPU", int( detection.trails.trailPixelCount ) ) );
                  console.Flush();
                  Module->ProcessEvents();

                  // Build compact trail pixel list (local struct for portability)
                  struct TrailPixelCoord { int x, y; };
                  std::vector<TrailPixelCoord> trailPixels;
                  for ( int y = 0; y < cropH; ++y )
                     for ( int x = 0; x < cropW; ++x )
                        if ( detection.trails.mask[y * cropW + x] )
                           trailPixels.push_back( { x, y } );

                  for ( int ch = 0; ch < outChannels; ++ch )
                  {
                     std::vector<float> corrected( trailPixels.size() );
                     bool gpuOk = false;

#ifdef NUKEX_HAS_CUDA
                     if ( useGPU )
                     {
                        // Convert to cuda::TrailPixel for GPU API
                        std::vector<nukex::cuda::TrailPixel> cudaTrailPixels( trailPixels.size() );
                        for ( size_t i = 0; i < trailPixels.size(); ++i )
                        {
                           cudaTrailPixels[i].x = trailPixels[i].x;
                           cudaTrailPixels[i].y = trailPixels[i].y;
                        }
                        gpuOk = nukex::cuda::remediateTrailsGPU(
                           channelCubes[ch].cube().data(),
                           channelCubes[ch].numSubs(), cropH, cropW,
                           cudaTrailPixels,
                           p_trailOutlierSigma, corrected.data() );
                     }
#endif
                     if ( !gpuOk )
                     {
                        if ( useGPU )
                           console.WarningLn( "    GPU trail remediation failed -- falling back to CPU" );
                        // CPU fallback: mask bright outlier frames and re-select
                        if ( !channelCubes[ch].hasMasks() )
                           channelCubes[ch].allocateMasks();

                        nukex::PixelSelector fallbackSelector( selConfig );
                        for ( size_t i = 0; i < trailPixels.size(); ++i )
                        {
                           int px = trailPixels[i].x, py = trailPixels[i].y;
                           const float* zCol = channelCubes[ch].zColumnPtr( py, px );

                           // Compute median and MAD of Z-column
                           std::vector<float> zVals( zCol, zCol + channelCubes[ch].numSubs() );
                           std::sort( zVals.begin(), zVals.end() );
                           float med = zVals[zVals.size() / 2];

                           std::vector<float> absdev( zVals.size() );
                           for ( size_t j = 0; j < zVals.size(); ++j )
                              absdev[j] = std::abs( zVals[j] - med );
                           std::sort( absdev.begin(), absdev.end() );
                           float madVal = absdev[absdev.size() / 2] * 1.4826f;
                           if ( madVal < 1e-10f ) madVal = 1e-10f;
                           float threshold = med + float( p_trailOutlierSigma ) * madVal;

                           // Mask bright outlier frames
                           for ( size_t z = 0; z < channelCubes[ch].numSubs(); ++z )
                              if ( zCol[z] > threshold )
                                 channelCubes[ch].setMask( z, py, px, 1 );

                           auto result = fallbackSelector.selectBestZ(
                              zCol, channelCubes[ch].numSubs(), qualityScoresPtr,
                              channelCubes[ch].maskColumnPtr( py, px ) );
                           corrected[i] = result.selectedValue;
                        }
                     }

                     // Patch channelResults
                     for ( size_t i = 0; i < trailPixels.size(); ++i )
                     {
                        int px = trailPixels[i].x, py = trailPixels[i].y;
                        channelResults[ch][py * cropW + px] = corrected[i];
                     }
                  }

                  console.WriteLn( String().Format( "    Remediated %d trail pixels per channel",
                     int( trailPixels.size() ) ) );
                  Module->ProcessEvents();
               }

               // 7c: Dust remediation (per channel) — edge-referenced correction
               if ( p_enableDustRemediation && !dustDetection.blobs.empty() )
               {
                  console.WriteLn( "  Phase 7c: Dust remediation (edge-referenced correction)..." );
                  console.Flush();
                  Module->ProcessEvents();

                  // Expand blob radii to cover the full mote extent.
                  // The detector finds the high-confidence core; the visual mote
                  // extends ~2x further due to the gradual Gaussian falloff.
                  std::vector<nukex::DustBlobInfo> expandedBlobs = dustDetection.blobs;
                  for ( auto& b : expandedBlobs )
                     b.radius *= 2.0;

                  nukex::DustCorrector corrector;
                  for ( int ch = 0; ch < outChannels; ++ch )
                  {
                     corrector.correct(
                        channelResults[ch].data(), cropW, cropH,
                        expandedBlobs,
                        [&console]( const std::string& msg ) {
                           console.WriteLn( String( msg.c_str() ) );
                        } );
                  }

                  int totalPixels = 0;
                  for ( const auto& b : dustDetection.blobs )
                     totalPixels += static_cast<int>( M_PI * b.radius * b.radius );
                  console.WriteLn( String().Format( "    Corrected ~%d dust pixels per channel", totalPixels ) );
                  Module->ProcessEvents();
               }

               // 7c (cont): Vignetting correction (per channel)
               if ( p_enableVignettingRemediation && detection.vignetting.maxCorrection > 1.01 )
               {
                  console.WriteLn( String().Format( "  Phase 7c: Vignetting correction (%s)...",
                     useGPU ? "GPU" : "CPU" ) );
                  console.Flush();
                  Module->ProcessEvents();

                  for ( int ch = 0; ch < outChannels; ++ch )
                  {
                     std::vector<float> corrected( size_t( cropW ) * size_t( cropH ) );
                     bool gpuOk = false;

#ifdef NUKEX_HAS_CUDA
                     if ( useGPU )
                     {
                        gpuOk = nukex::cuda::remediateVignettingGPU(
                           channelResults[ch].data(),
                           detection.vignetting.correctionMap.data(),
                           cropW, cropH, corrected.data() );
                     }
#endif
                     if ( !gpuOk )
                     {
                        if ( useGPU )
                           console.WarningLn( "    GPU vignetting correction failed -- falling back to CPU" );
                        for ( size_t i = 0; i < size_t( cropW ) * size_t( cropH ); ++i )
                           corrected[i] = channelResults[ch][i] * detection.vignetting.correctionMap[i];
                     }

                     channelResults[ch] = std::move( corrected );
                  }

                  console.WriteLn( String().Format( "    Max vignetting correction: %.2f",
                     detection.vignetting.maxCorrection ) );
                  Module->ProcessEvents();
               }

               // 7d: Rebuild stretched image from clean data
               // The subcube re-selection (7b) produced correct linear pixel values.
               // Rebuild the ENTIRE stretched image from scratch so that stretch
               // parameters are computed from trail-free data.  Stars and nebula are
               // preserved because 7b keeps their data from non-trail frames.
               console.WriteLn( "  Phase 7d: Rebuilding stretched image from clean data..." );
               console.Flush();
               Module->ProcessEvents();

               // Patch linear output image with corrected channelResults
               for ( int ch = 0; ch < outChannels; ++ch )
                  for ( int y = 0; y < cropH; ++y )
                     for ( int x = 0; x < cropW; ++x )
                        outputImage.Pixel( x, y, ch ) = channelResults[ch][y * cropW + x];

               // Copy clean linear data to stretch window
               for ( int ch = 0; ch < outChannels; ++ch )
                  for ( int y = 0; y < cropH; ++y )
                     for ( int x = 0; x < cropW; ++x )
                        stretchImage.Pixel( x, y, ch ) = outputImage.Pixel( x, y, ch );

               stretchImage.Truncate();

               // Re-do background neutralization on clean data
               if ( isColor )
               {
                  double medians[3];
                  for ( int ch = 0; ch < 3; ++ch )
                  {
                     stretchImage.SelectChannel( ch );
                     medians[ch] = stretchImage.Median();
                  }
                  stretchImage.ResetChannelRange();

                  double tgtMed = ( medians[0] + medians[1] + medians[2] ) / 3.0;
                  for ( int ch = 0; ch < 3; ++ch )
                  {
                     if ( medians[ch] > 1.0e-10 )
                     {
                        double scale = tgtMed / medians[ch];
                        stretchImage.SelectChannel( ch );
                        stretchImage *= scale;
                     }
                  }
                  stretchImage.ResetChannelRange();
               }

               // Re-do entropy-optimized stretch on clean data
               {
                  stretchImage.SelectChannel( 0 );
                  double refMed2 = stretchImage.Median();
                  double refMad2 = stretchImage.MAD( refMed2 );
                  stretchImage.ResetChannelRange();

                  // Sample clean data for optimization
                  std::vector<float> optSample2;
                  {
                     size_t totalPx2 = size_t( cropW ) * size_t( cropH );
                     size_t step2 = std::max( size_t( 1 ), totalPx2 / 10000 );
                     for ( size_t i = 0; i < totalPx2; i += step2 )
                        optSample2.push_back( stretchImage.Pixel( int( i % cropW ), int( i / cropW ), 0 ) );
                  }

                  // Re-run stretch optimization on clean data using the same
                  // algorithm type that Phase 6 selected (not hardcoded MTF).
                  double bestScore2 = -1e30;
                  double bestParam2 = bestParam;
                  auto baseAlgo = StretchLibrary::Instance().Create( bestType );
                  baseAlgo->AutoConfigure( refMed2, refMad2 );
                  double baseParamVal = baseAlgo->GetParameter( IsoString( bestParamName ) );
                  for ( double mult = 0.5; mult <= 1.5; mult += 0.05 )
                  {
                     auto trial = baseAlgo->Clone();
                     double pv = baseParamVal * mult;
                     trial->SetParameter( IsoString( bestParamName ), pv );
                     double sc = scoreStretch( optSample2, trial.get() );
                     if ( sc > bestScore2 ) { bestScore2 = sc; bestParam2 = pv; }
                  }

                  // Apply optimized stretch to all channels
                  if ( isColor )
                  {
                     for ( int ch = 0; ch < outChannels; ++ch )
                     {
                        stretchImage.SelectChannel( ch );
                        double med = stretchImage.Median();
                        double mad = stretchImage.MAD( med );

                        auto chAlgo = StretchLibrary::Instance().Create( bestType );
                        chAlgo->SetParameter( IsoString( bestParamName ), bestParam2 );
                        chAlgo->AutoConfigure( med, mad );

                        Image::sample_iterator it( stretchImage, ch );
                        for ( ; it; ++it )
                           *it = chAlgo->Apply( *it );

                        stretchAlgos[ch] = chAlgo->Clone();
                     }
                     stretchImage.ResetChannelRange();
                  }
                  else
                  {
                     auto monoAlgo = StretchLibrary::Instance().Create( bestType );
                     monoAlgo->SetParameter( IsoString( bestParamName ), bestParam2 );
                     monoAlgo->AutoConfigure( refMed2, refMad2 );
                     monoAlgo->ApplyToImage( stretchImage );
                     stretchAlgos[0] = monoAlgo->Clone();
                  }
               }

               auto elapsed7 = std::chrono::duration<double>( std::chrono::steady_clock::now() - tPhase7 ).count();
               console.WriteLn( String().Format( "  Remediation complete in %.1fs", elapsed7 ) );
            }
            else
            {
               console.WriteLn( "  No artifacts detected — skipping remediation." );
            }

            Module->ProcessEvents();
         }
      }

      // Free subcubes (no longer needed after stretch/remediation)
      channelCubes.clear();

      // Final banner
      auto totalElapsed = std::chrono::duration<double>( std::chrono::steady_clock::now() - t0 ).count();
      int minutes = int( totalElapsed ) / 60;
      double seconds = totalElapsed - minutes * 60;

      console.WriteLn( String().Format(
         "\n\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
         "\n  NukeX stacking complete \xe2\x80\x94 %dm %.0fs total"
         "\n\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90",
         minutes, seconds ) );

      return true;
   }
   catch ( const std::bad_alloc& e )
   {
      Console().CriticalLn( "NukeX: Out of memory \xe2\x80\x94 " + String( e.what() ) +
         "\nTry reducing the number of input frames or closing other image windows.\n" );
      return false;
   }
   catch ( const ProcessAborted& )
   {
      throw;
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
   if ( p == TheNXSFlatFramePathParameter )
   {
      if ( tableRow >= p_flatFrames.size() )
         return nullptr;
      return p_flatFrames[tableRow].Begin();
   }
   if ( p == TheNXSGenerateProvenanceParameter )
      return &p_generateProvenance;
   if ( p == TheNXSGenerateDistMetadataParameter )
      return &p_generateDistMetadata;
   if ( p == TheNXSEnableMetadataTiebreakerParameter )
      return &p_enableMetadataTiebreaker;
   if ( p == TheNXSEnableAutoStretchParameter )
      return &p_enableAutoStretch;
   if ( p == TheNXSUseGPUParameter )
      return &p_useGPU;
   if ( p == TheNXSAdaptiveModelsParameter )
      return &p_adaptiveModels;
   if ( p == TheNXSEnableRemediationParameter )
      return &p_enableRemediation;
   if ( p == TheNXSEnableTrailRemediationParameter )
      return &p_enableTrailRemediation;
   if ( p == TheNXSEnableDustRemediationParameter )
      return &p_enableDustRemediation;
   if ( p == TheNXSEnableVignettingRemediationParameter )
      return &p_enableVignettingRemediation;
   if ( p == TheNXSOutlierSigmaThresholdParameter )
      return &p_outlierSigmaThreshold;
   if ( p == TheNXSTrailDilateRadiusParameter )
      return &p_trailDilateRadius;
   if ( p == TheNXSTrailOutlierSigmaParameter )
      return &p_trailOutlierSigma;
   if ( p == TheNXSDustCircularityMinParameter )
      return &p_dustCircularityMin;
   if ( p == TheNXSDustDetectionSigmaParameter )
      return &p_dustDetectionSigma;
   if ( p == TheNXSDustMaxCorrectionRatioParameter )
      return &p_dustMaxCorrectionRatio;
   if ( p == TheNXSDustMinDiameterParameter )
      return &p_dustMinDiameter;
   if ( p == TheNXSDustMaxDiameterParameter )
      return &p_dustMaxDiameter;
   if ( p == TheNXSDustNeighborRadiusParameter )
      return &p_dustNeighborRadius;
   if ( p == TheNXSVignettingPolyOrderParameter )
      return &p_vignettingPolyOrder;
   if ( p == TheNXSVignettingMaxCorrectionParameter )
      return &p_vignettingMaxCorrection;
   if ( p == TheNXSBortleNumberParameter )
      return &p_bortleNumber;

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

   if ( p == TheNXSFlatFramesParameter )
   {
      p_flatFrames.clear();
      if ( sizeOrLength > 0 )
         p_flatFrames.resize( sizeOrLength );
      return true;
   }

   if ( p == TheNXSFlatFramePathParameter )
   {
      p_flatFrames[tableRow].Clear();
      if ( sizeOrLength > 0 )
         p_flatFrames[tableRow].SetLength( sizeOrLength );
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

   if ( p == TheNXSFlatFramesParameter )
      return p_flatFrames.size();

   if ( p == TheNXSFlatFramePathParameter )
   {
      if ( tableRow >= p_flatFrames.size() )
         return 0;
      return p_flatFrames[tableRow].Length();
   }

   return 0;
}

// ----------------------------------------------------------------------------

} // namespace pcl
