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
#include "engine/AutoStretchSelector.h"
#include "engine/StretchLibrary.h"

#include <pcl/MetaModule.h>
#include <pcl/Console.h>
#include <pcl/StatusMonitor.h>
#include <pcl/StandardStatus.h>
#include <pcl/ImageWindow.h>
#include <pcl/View.h>
#include <pcl/Image.h>
#include <pcl/ErrorHandler.h>

#include <chrono>
#include <algorithm>
#include <cmath>

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

      int unalignedCount = 0;
      for ( size_t i = 0; i < aligned.offsets.size(); ++i )
      {
         const auto& o = aligned.offsets[i];
         if ( o.valid )
         {
            console.WriteLn( String().Format( "  [%d/%d] dx=%+d, dy=%+d (%d stars, RMS=%.2f)\n",
               int( i + 1 ), int( aligned.offsets.size() ),
               o.dx, o.dy, o.numMatchedStars, o.convergenceRMS ) );
         }
         else
         {
            ++unalignedCount;
            console.WarningLn( String().Format( "  [%d/%d] alignment failed — using zero offset (unaligned)\n",
               int( i + 1 ), int( aligned.offsets.size() ) ) );
         }
         console.Flush();
         Module->ProcessEvents();
      }
      if ( unalignedCount > 0 )
         console.WarningLn( String().Format( "  %d frame(s) could not be aligned — included with zero offset\n",
            unalignedCount ) );
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

      // Phase 2: Quality weights
      console.WriteLn( "\nPhase 2: Computing quality weights..." );
      console.Flush();
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
         for ( size_t z = 0; z < nSubs; ++z )
            metaVec.push_back( aligned.alignedCube.metadata( z ) );

         weights = nukex::ComputeQualityWeights( metaVec, wcfg );
         console.WriteLn( String().Format( "  Mode: Full | Weight range: %.2f \xe2\x80\x94 %.2f",
            *std::min_element( weights.begin(), weights.end() ),
            *std::max_element( weights.begin(), weights.end() ) ) );
      }
      else
      {
         weights.assign( nSubs, 1.0 / nSubs );
         console.WriteLn( "  Mode: Equal weights (quality weighting disabled)" );
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
            nukex::SubCube cube = std::move( aligned.alignedCube );
            channelResults[ch] = selector.processImage( cube, weights, progressCB );

            size_t mapSize = size_t( cropH ) * size_t( cropW );
            distTypeMaps[ch].resize( mapSize );
            for ( size_t y = 0; y < size_t( cropH ); ++y )
               for ( size_t x = 0; x < size_t( cropW ); ++x )
                  distTypeMaps[ch][y * cropW + x] = cube.distType( y, x );

            size_t counts[4] = {};
            for ( uint8_t t : distTypeMaps[ch] )
               if ( t < 4 ) counts[t]++;
            console.WriteLn( String().Format(
               "    Distribution: %.0f%% Gaussian, %.0f%% Poisson, %.0f%% Skew-Normal, %.0f%% Bimodal\n",
               100.0 * counts[0] / mapSize, 100.0 * counts[1] / mapSize,
               100.0 * counts[2] / mapSize, 100.0 * counts[3] / mapSize ) );
         }
         else
         {
            // Build per-channel frame data and apply alignment
            std::vector<std::vector<float>> chFrameData( nSubs );
            for ( size_t f = 0; f < nSubs; ++f )
               chFrameData[f] = raw.pixelData[f][ch];

            nukex::SubCube cube = nukex::applyAlignment( chFrameData, aligned.offsets,
                                                          aligned.crop, raw.width, raw.height );

            for ( size_t i = 0; i < raw.metadata.size(); ++i )
               cube.setMetadata( i, raw.metadata[i] );

            channelResults[ch] = selector.processImage( cube, weights, progressCB );

            size_t mapSize = size_t( cropH ) * size_t( cropW );
            distTypeMaps[ch].resize( mapSize );
            for ( size_t y = 0; y < size_t( cropH ); ++y )
               for ( size_t x = 0; x < size_t( cropW ); ++x )
                  distTypeMaps[ch][y * cropW + x] = cube.distType( y, x );

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

         // Channel recombination: normalize per-channel backgrounds
         if ( isColor )
         {
            console.WriteLn( "\n  Channel recombination (background neutralization):" );

            double medians[3];
            for ( int ch = 0; ch < 3; ++ch )
            {
               stretchImage.SelectChannel( ch );
               medians[ch] = stretchImage.Median();
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

         // Auto-configure and apply per-channel to preserve color
         if ( isColor )
         {
            console.WriteLn( "\n  Per-channel stretch:" );
            for ( int ch = 0; ch < 3; ++ch )
            {
               const char* label = ch == 0 ? "R" : ch == 1 ? "G" : "B";

               stretchImage.SelectChannel( ch );
               double med = stretchImage.Median();
               double mad = stretchImage.MAD( med );
               console.WriteLn( String().Format( "    %s: median=%.6f, MAD=%.6f", label, med, mad ) );

               auto chAlgo = algo->Clone();
               chAlgo->AutoConfigure( med, mad );

               // Apply to this channel only
               Image::sample_iterator it( stretchImage, ch );
               for ( ; it; ++it )
                  *it = chAlgo->Apply( *it );
            }
            stretchImage.ResetChannelRange();

            // Log parameters from last channel as representative
            auto params = algo->GetParameters();
            for ( const auto& param : params )
               console.WriteLn( String().Format( "  %s = %.4f",
                  IsoString( param.name ).c_str(), param.value ) );
         }
         else
         {
            double med = stretchImage.Median();
            double mad = stretchImage.MAD( med );
            console.WriteLn( String().Format( "  Image median: %.6f, MAD: %.6f", med, mad ) );

            algo->AutoConfigure( med, mad );

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
      }

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
