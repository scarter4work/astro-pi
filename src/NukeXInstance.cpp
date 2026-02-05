//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "NukeXInstance.h"
#include "NukeXParameters.h"

#include <pcl/AutoViewLock.h>
#include <pcl/Console.h>
#include <pcl/FITSHeaderKeyword.h>
#include <pcl/StandardStatus.h>
#include <pcl/View.h>

#include "engine/Compositor.h"
#include "engine/RegionStatistics.h"
#include "engine/StretchLibrary.h"
#include "engine/StretchSelector.h"

namespace pcl
{

// ----------------------------------------------------------------------------

NukeXInstance::NukeXInstance( const MetaProcess* m )
   : ProcessImplementation( m )
   , p_processingMode( NXProcessingMode::Default )
   , p_previewMode( NXPreviewMode::Default )
   , p_stretchAlgorithm( NXStretchAlgorithm::Default )
   , p_autoSegment( TheNXAutoSegmentParameter->DefaultValue() )
   , p_autoSelect( TheNXAutoSelectParameter->DefaultValue() )
   , p_enableLRGB( TheNXEnableLRGBParameter->DefaultValue() )
   , p_enableToneMapping( TheNXEnableToneMappingParameter->DefaultValue() )
   , p_autoBlackPoint( TheNXAutoBlackPointParameter->DefaultValue() )
   , p_globalContrast( TheNXGlobalContrastParameter->DefaultValue() )
   , p_saturationBoost( TheNXSaturationBoostParameter->DefaultValue() )
   , p_blendRadius( TheNXBlendRadiusParameter->DefaultValue() )
   , p_stretchStrength( TheNXStretchStrengthParameter->DefaultValue() )
   , p_blackPoint( TheNXBlackPointParameter->DefaultValue() )
   , p_whitePoint( TheNXWhitePointParameter->DefaultValue() )
   , p_gamma( TheNXGammaParameter->DefaultValue() )
   , p_enableStarCores( TheNXEnableStarCoresParameter->DefaultValue() )
   , p_enableStarHalos( TheNXEnableStarHalosParameter->DefaultValue() )
   , p_enableNebulaBright( TheNXEnableNebulaBrightParameter->DefaultValue() )
   , p_enableNebulaFaint( TheNXEnableNebulaFaintParameter->DefaultValue() )
   , p_enableDustLanes( TheNXEnableDustLanesParameter->DefaultValue() )
   , p_enableGalaxyCore( TheNXEnableGalaxyCoreParameter->DefaultValue() )
   , p_enableGalaxyHalo( TheNXEnableGalaxyHaloParameter->DefaultValue() )
   , p_enableGalaxyArms( TheNXEnableGalaxyArmsParameter->DefaultValue() )
   , p_enableBackground( TheNXEnableBackgroundParameter->DefaultValue() )
{
}

// ----------------------------------------------------------------------------

NukeXInstance::NukeXInstance( const NukeXInstance& x )
   : ProcessImplementation( x )
{
   Assign( x );
}

// ----------------------------------------------------------------------------

void NukeXInstance::Assign( const ProcessImplementation& p )
{
   const NukeXInstance* x = dynamic_cast<const NukeXInstance*>( &p );
   if ( x != nullptr )
   {
      p_processingMode     = x->p_processingMode;
      p_previewMode        = x->p_previewMode;
      p_stretchAlgorithm   = x->p_stretchAlgorithm;
      p_autoSegment        = x->p_autoSegment;
      p_autoSelect         = x->p_autoSelect;
      p_enableLRGB         = x->p_enableLRGB;
      p_enableToneMapping  = x->p_enableToneMapping;
      p_autoBlackPoint     = x->p_autoBlackPoint;
      p_globalContrast     = x->p_globalContrast;
      p_saturationBoost    = x->p_saturationBoost;
      p_blendRadius        = x->p_blendRadius;
      p_stretchStrength    = x->p_stretchStrength;
      p_blackPoint         = x->p_blackPoint;
      p_whitePoint         = x->p_whitePoint;
      p_gamma              = x->p_gamma;
      p_enableStarCores    = x->p_enableStarCores;
      p_enableStarHalos    = x->p_enableStarHalos;
      p_enableNebulaBright = x->p_enableNebulaBright;
      p_enableNebulaFaint  = x->p_enableNebulaFaint;
      p_enableDustLanes    = x->p_enableDustLanes;
      p_enableGalaxyCore   = x->p_enableGalaxyCore;
      p_enableGalaxyHalo   = x->p_enableGalaxyHalo;
      p_enableGalaxyArms   = x->p_enableGalaxyArms;
      p_enableBackground   = x->p_enableBackground;
   }
}

// ----------------------------------------------------------------------------

bool NukeXInstance::Validate( String& info )
{
   // Validate parameter ranges
   if ( p_globalContrast < 0 || p_globalContrast > 2 )
   {
      info = "Global contrast must be between 0 and 2.";
      return false;
   }
   if ( p_saturationBoost < 0 || p_saturationBoost > 2 )
   {
      info = "Saturation boost must be between 0 and 2.";
      return false;
   }
   if ( p_blendRadius < 0 || p_blendRadius > 100 )
   {
      info = "Blend radius must be between 0 and 100.";
      return false;
   }
   if ( p_stretchStrength < 0 || p_stretchStrength > 1 )
   {
      info = "Stretch strength must be between 0 and 1.";
      return false;
   }
   if ( p_blackPoint < 0 || p_blackPoint > 1 )
   {
      info = "Black point must be between 0 and 1.";
      return false;
   }
   if ( p_whitePoint < 0 || p_whitePoint > 1 )
   {
      info = "White point must be between 0 and 1.";
      return false;
   }
   if ( p_blackPoint >= p_whitePoint )
   {
      info = "Black point must be less than white point.";
      return false;
   }
   if ( p_gamma < 0.1 || p_gamma > 10 )
   {
      info = "Gamma must be between 0.1 and 10.";
      return false;
   }

   info.Clear();
   return true;
}

// ----------------------------------------------------------------------------

UndoFlags NukeXInstance::UndoMode( const View& ) const
{
   return UndoFlag::PixelData;
}

// ----------------------------------------------------------------------------

bool NukeXInstance::CanExecuteOn( const View& view, String& whyNot ) const
{
   if ( view.Image().IsComplexSample() )
   {
      whyNot = "NukeX cannot be executed on complex images.";
      return false;
   }
   return true;
}

// ----------------------------------------------------------------------------

bool NukeXInstance::CanExecuteGlobal( String& whyNot ) const
{
   whyNot = "NukeX requires a view to execute on.";
   return false;
}

// ----------------------------------------------------------------------------

bool NukeXInstance::IsRealTimePreviewEnabled( const View& ) const
{
   return true;
}

// ----------------------------------------------------------------------------

CompositorConfig NukeXInstance::BuildCompositorConfig() const
{
   CompositorConfig config;

   // FULLY AUTOMATIC MODE: everything computed from image stats
   if ( p_processingMode == NXProcessingMode::FullyAutomatic )
   {
      config.useSegmentation = true;
      config.useAutoSelection = true;
      config.useLRGBMode = true;
      config.applyToneMapping = true;
      config.globalStrength = 1.0;    // Will be modulated by SNR in ExecuteOn()
      config.globalContrast = 1.0;
      config.globalSaturation = 1.0;
      config.blendConfig.featherRadius = 5.0;
      config.blendConfig.normalizeWeights = true;
      config.toneConfig.autoBlackPoint = true;
      config.toneConfig.autoWhitePoint = true;
      config.toneConfig.gamma = 1.0;
      config.useParallelBlend = true;
      config.numThreads = 0;
      return config;
   }

   // MANUAL MODE: existing code below unchanged...

   // Processing modes - use user's segmentation setting
   config.useSegmentation = p_autoSegment;
   config.useAutoSelection = p_autoSelect;
   config.useLRGBMode = p_enableLRGB;
   config.applyToneMapping = p_enableToneMapping;

   // Global parameters
   config.globalStrength = p_stretchStrength;
   config.globalContrast = p_globalContrast;
   config.globalSaturation = p_saturationBoost;

   // Blend configuration
   config.blendConfig.featherRadius = p_blendRadius;
   config.blendConfig.normalizeWeights = true;

   // Tone mapping configuration
   config.toneConfig.blackPoint = p_blackPoint;
   config.toneConfig.whitePoint = p_whitePoint;
   config.toneConfig.gamma = p_gamma;
   config.toneConfig.autoBlackPoint = p_autoBlackPoint;
   config.toneConfig.autoWhitePoint = false;

   // Performance settings
   config.useParallelBlend = true;
   config.numThreads = 0; // Auto-detect

   return config;
}

// ----------------------------------------------------------------------------

std::set<RegionClass> NukeXInstance::GetEnabledRegions() const
{
   std::set<RegionClass> enabled;

   if ( p_enableBackground )   enabled.insert( RegionClass::Background );

   // Star classes (mapped from old StarCore/StarHalo settings)
   if ( p_enableStarCores )
   {
      enabled.insert( RegionClass::StarBright );
      enabled.insert( RegionClass::StarSaturated );
   }
   if ( p_enableStarHalos )
   {
      enabled.insert( RegionClass::StarMedium );
      enabled.insert( RegionClass::StarFaint );
   }

   // Nebula classes (mapped from old NebulaBright/NebulaFaint settings)
   if ( p_enableNebulaBright )
   {
      enabled.insert( RegionClass::NebulaEmission );
      enabled.insert( RegionClass::NebulaReflection );
      enabled.insert( RegionClass::NebulaPlanetary );
   }
   if ( p_enableNebulaFaint )
   {
      enabled.insert( RegionClass::NebulaDark );
   }

   // Structural classes
   if ( p_enableDustLanes )    enabled.insert( RegionClass::DustLane );

   // Galaxy classes (mapped from old GalaxyCore/GalaxyHalo/GalaxyArm settings)
   if ( p_enableGalaxyCore )   enabled.insert( RegionClass::GalaxyCore );
   if ( p_enableGalaxyHalo )
   {
      enabled.insert( RegionClass::GalaxyElliptical );
      enabled.insert( RegionClass::GalaxyIrregular );
   }
   if ( p_enableGalaxyArms )   enabled.insert( RegionClass::GalaxySpiral );

   // Star clusters (enable if any galaxy/star settings are enabled)
   if ( p_enableStarCores || p_enableStarHalos )
   {
      enabled.insert( RegionClass::StarClusterOpen );
      enabled.insert( RegionClass::StarClusterGlobular );
   }

   return enabled;
}

// ----------------------------------------------------------------------------

AlgorithmType NukeXInstance::GetSelectedAlgorithm() const
{
   switch ( p_stretchAlgorithm )
   {
   case NXStretchAlgorithm::MTF:         return AlgorithmType::MTF;
   case NXStretchAlgorithm::Histogram:   return AlgorithmType::Histogram;
   case NXStretchAlgorithm::GHS:         return AlgorithmType::GHS;
   case NXStretchAlgorithm::ArcSinh:     return AlgorithmType::ArcSinh;
   case NXStretchAlgorithm::Log:         return AlgorithmType::Log;
   case NXStretchAlgorithm::Lumpton:     return AlgorithmType::Lumpton;
   case NXStretchAlgorithm::RNC:         return AlgorithmType::RNC;
   case NXStretchAlgorithm::Photometric: return AlgorithmType::Photometric;
   case NXStretchAlgorithm::OTS:         return AlgorithmType::OTS;
   case NXStretchAlgorithm::SAS:         return AlgorithmType::SAS;
   case NXStretchAlgorithm::Veralux:     return AlgorithmType::Veralux;
   case NXStretchAlgorithm::Auto:
   default:                               return AlgorithmType::GHS;
   }
}

// ----------------------------------------------------------------------------

bool NukeXInstance::ExecuteOn( View& view )
{
   AutoViewLock lock( view );

   Console console;
   console.EnableAbort();

   ImageVariant image = view.Image();

   console.WriteLn( "<end><cbr><br>NukeX - Intelligent Region-Aware Stretch" );
   console.WriteLn( String().Format( "<br>Image dimensions: %d x %d x %d",
                                      image.Width(), image.Height(), image.NumberOfChannels() ) );

   // Build configuration from parameters
   CompositorConfig config = BuildCompositorConfig();

   // FULLY AUTOMATIC MODE: adapt parameters from image statistics and FITS metadata
   if ( p_processingMode == NXProcessingMode::FullyAutomatic )
   {
      console.WriteLn( "<br>** Fully Automatic Mode **" );

      // Compute SNR-adaptive stretch strength from image statistics
      double median = image.Median();
      double stdDev = image.StdDev();

      double autoStrength;
      if ( median < 0.01 )       autoStrength = 1.5;   // Very faint linear
      else if ( median < 0.05 )  autoStrength = 1.2;   // Faint linear
      else if ( median < 0.15 )  autoStrength = 1.0;   // Moderate
      else if ( median < 0.35 )  autoStrength = 0.7;   // Partially stretched
      else                        autoStrength = 0.4;   // Already stretched

      // Modulate by noise level
      double snr = ( stdDev > 0 ) ? median / stdDev : 10.0;
      if ( snr < 5.0 )  autoStrength *= 0.8;
      if ( snr > 30.0 ) autoStrength *= 1.1;

      config.globalStrength = autoStrength;

      // Adapt blend radius to image scale
      int imageDim = Max( image.Width(), image.Height() );
      config.blendConfig.featherRadius = Max( 3.0, static_cast<double>( imageDim ) / 500.0 );

      // Read FITS keywords for context
      FITSKeywordArray keywords;
      view.Window().GetKeywords( keywords );

      String objectName, filter;
      for ( const FITSHeaderKeyword& kw : keywords )
      {
         if ( kw.name == "OBJECT" )
            objectName = kw.value.Trimmed();
         else if ( kw.name == "FILTER" )
            filter = kw.value.Trimmed();
      }

      if ( !objectName.IsEmpty() )
         console.WriteLn( "NukeX Auto: Target = " + objectName );
      if ( !filter.IsEmpty() )
         console.WriteLn( "NukeX Auto: Filter = " + filter );

      // Narrowband filter detection
      String upperFilter = filter.Uppercase();
      if ( upperFilter.Contains( "HA" ) || upperFilter.Contains( "H-ALPHA" ) ||
           upperFilter.Contains( "OIII" ) || upperFilter.Contains( "SII" ) )
      {
         console.WriteLn( "NukeX Auto: Narrowband detected - adjusting parameters" );
         config.globalSaturation = 0.8;
      }

      console.WriteLn( String().Format( "NukeX Auto: median=%.4f, stdDev=%.4f, SNR=%.1f",
         median, stdDev, snr ) );
      console.WriteLn( String().Format( "NukeX Auto: strength=%.2f, blend=%.1f",
         autoStrength, config.blendConfig.featherRadius ) );
   }

   // If using manual algorithm selection (not Auto), configure overrides
   if ( p_stretchAlgorithm != NXStretchAlgorithm::Auto )
   {
      config.useAutoSelection = false;
      console.WriteLn( String().Format( "Using manual algorithm: %s",
         StretchLibrary::Instance().GetInfo( GetSelectedAlgorithm() ).name.c_str() ) );
   }

   console.WriteLn( String().Format( "Segmentation: %s", p_autoSegment ? "enabled" : "disabled" ) );
   console.WriteLn( String().Format( "Auto-selection: %s", p_autoSelect ? "enabled" : "disabled" ) );
   console.WriteLn( String().Format( "LRGB mode: %s", p_enableLRGB ? "enabled" : "disabled" ) );
   console.WriteLn( String().Format( "Tone mapping: %s", p_enableToneMapping ? "enabled" : "disabled" ) );
   console.WriteLn( String().Format( "Stretch strength: %.3f", p_stretchStrength ) );

   // Create compositor
   Compositor compositor( config );

   // Set up region overrides if any regions are disabled
   auto enabledRegions = GetEnabledRegions();
   if ( enabledRegions.size() < 9 )
   {
      console.WriteLn( String().Format( "<br>Enabled regions: %d of 9", enabledRegions.size() ) );
   }

   // If using manual algorithm, set it as override for all regions
   if ( p_stretchAlgorithm != NXStretchAlgorithm::Auto )
   {
      AlgorithmType algo = GetSelectedAlgorithm();
      for ( RegionClass region : enabledRegions )
      {
         UserOverride override;
         override.region = region;
         override.algorithm = algo;
         compositor.Selector().SetOverride( override );
      }
   }

   // Convert ImageVariant to Image for processing
   Image floatImage;
   if ( image.IsFloatSample() )
   {
      switch ( image.BitsPerSample() )
      {
      case 32:
         floatImage = static_cast<Image&>( *image );
         break;
      case 64:
         floatImage = Image( static_cast<DImage&>( *image ) );
         break;
      }
   }
   else
   {
      // Convert integer samples to float
      switch ( image.BitsPerSample() )
      {
      case 8:
         floatImage = Image( static_cast<UInt8Image&>( *image ) );
         break;
      case 16:
         floatImage = Image( static_cast<UInt16Image&>( *image ) );
         break;
      case 32:
         floatImage = Image( static_cast<UInt32Image&>( *image ) );
         break;
      }
   }

   // Process with progress callback
   console.WriteLn( "<br>Processing..." );
   console.Flush();

   // Track last reported stage to avoid duplicate messages
   ProcessingStage lastReportedStage = ProcessingStage::Idle;

   CompositorResult result = compositor.Process( floatImage,
      [&console, &lastReportedStage]( const CompositorProgress& progress ) {
         // Only report when we enter a new stage (avoids garbled rapid updates)
         if ( progress.stage != lastReportedStage )
         {
            lastReportedStage = progress.stage;
            console.WriteLn( String().Format( "  %-25s [%3.0f%%]",
               progress.message.c_str(), progress.overall * 100 ) );
            console.Flush();
         }
      }
   );

   if ( !result.isValid )
   {
      console.CriticalLn( "NukeX processing failed: " + result.errorMessage );
      return false;
   }

   // Report timing
   console.WriteLn( String().Format( "<br>Timing:" ) );
   if ( result.segmentationTimeMs > 0 )
      console.WriteLn( String().Format( "  Segmentation: %.1f ms", result.segmentationTimeMs ) );
   console.WriteLn( String().Format( "  Analysis: %.1f ms", result.analysisTimeMs ) );
   console.WriteLn( String().Format( "  Stretching: %.1f ms", result.stretchTimeMs ) );
   if ( result.toneMappingTimeMs > 0 )
      console.WriteLn( String().Format( "  Tone mapping: %.1f ms", result.toneMappingTimeMs ) );
   console.WriteLn( String().Format( "  Total: %.1f ms", result.totalTimeMs ) );

   // Report algorithm selections with detailed information
   console.WriteLn( "<br>Algorithm selections:" );
   for ( const auto& item : result.selectionSummary.entries )
   {
      // Compute names on-demand to avoid PCL String ABI issues
      // Use IsoString (UTF-8) for console output - String (UTF-16) c_str() doesn't work with %s
      IsoString regionName = IsoString( RegionClassDisplayName( item.region ) );
      IsoString algorithmName = IsoString( StretchLibrary::TypeToName( item.algorithm ) );
      IsoString algorithmId = StretchLibrary::TypeToId( item.algorithm );

      // Format: Region: Algorithm Full Name (ID) - confidence%
      console.WriteLn( String().Format( "  %s: %s (%s) - %.0f%% confidence",
         regionName.c_str(),
         algorithmName.c_str(),
         algorithmId.c_str(),
         item.confidence * 100 ) );

      // Show rationale if available
      if ( !item.rationale.IsEmpty() )
      {
         console.WriteLn( String().Format( "    Reason: %s", IsoString( item.rationale ).c_str() ) );
      }
   }

   // For RGB images, note that the same algorithm is applied to all channels
   if ( image.NumberOfNominalChannels() >= 3 )
   {
      console.WriteLn( "<br>Note: For RGB images, the selected algorithm is applied uniformly to all channels." );
      console.WriteLn( "LRGB mode can be enabled for luminance-only stretching with color preservation." );
   }

   // Copy result back to view
   image.CopyImage( result.outputImage );

   console.WriteLn( "<br>NukeX stretch complete." );

   return true;
}

// ----------------------------------------------------------------------------

Image NukeXInstance::GeneratePreview( const Image& input, int previewMode ) const
{
   CompositorConfig config = BuildCompositorConfig();
   config.useParallelBlend = true;

   // Disable segmentation for real-time preview (too slow for interactive use)
   // Full segmentation runs during final ExecuteOn()
   config.useSegmentation = false;

   Compositor compositor( config );

   // Apply algorithm overrides if not using auto
   if ( p_stretchAlgorithm != NXStretchAlgorithm::Auto )
   {
      AlgorithmType algo = GetSelectedAlgorithm();
      auto enabledRegions = GetEnabledRegions();
      for ( RegionClass region : enabledRegions )
      {
         UserOverride override;
         override.region = region;
         override.algorithm = algo;
         compositor.Selector().SetOverride( override );
      }
   }

   // Process
   CompositorResult result = compositor.Process( input );

   if ( !result.isValid )
   {
      return input;
   }

   // Return result (no region map preview without segmentation)
   return result.outputImage;
}

// ----------------------------------------------------------------------------

void* NukeXInstance::LockParameter( const MetaParameter* p, size_type /*tableRow*/ )
{
   if ( p == TheNXProcessingModeParameter )
      return &p_processingMode;
   if ( p == TheNXPreviewModeParameter )
      return &p_previewMode;
   if ( p == TheNXStretchAlgorithmParameter )
      return &p_stretchAlgorithm;
   if ( p == TheNXAutoSegmentParameter )
      return &p_autoSegment;
   if ( p == TheNXAutoSelectParameter )
      return &p_autoSelect;
   if ( p == TheNXEnableLRGBParameter )
      return &p_enableLRGB;
   if ( p == TheNXEnableToneMappingParameter )
      return &p_enableToneMapping;
   if ( p == TheNXAutoBlackPointParameter )
      return &p_autoBlackPoint;
   if ( p == TheNXGlobalContrastParameter )
      return &p_globalContrast;
   if ( p == TheNXSaturationBoostParameter )
      return &p_saturationBoost;
   if ( p == TheNXBlendRadiusParameter )
      return &p_blendRadius;
   if ( p == TheNXStretchStrengthParameter )
      return &p_stretchStrength;
   if ( p == TheNXBlackPointParameter )
      return &p_blackPoint;
   if ( p == TheNXWhitePointParameter )
      return &p_whitePoint;
   if ( p == TheNXGammaParameter )
      return &p_gamma;
   if ( p == TheNXEnableStarCoresParameter )
      return &p_enableStarCores;
   if ( p == TheNXEnableStarHalosParameter )
      return &p_enableStarHalos;
   if ( p == TheNXEnableNebulaBrightParameter )
      return &p_enableNebulaBright;
   if ( p == TheNXEnableNebulaFaintParameter )
      return &p_enableNebulaFaint;
   if ( p == TheNXEnableDustLanesParameter )
      return &p_enableDustLanes;
   if ( p == TheNXEnableGalaxyCoreParameter )
      return &p_enableGalaxyCore;
   if ( p == TheNXEnableGalaxyHaloParameter )
      return &p_enableGalaxyHalo;
   if ( p == TheNXEnableGalaxyArmsParameter )
      return &p_enableGalaxyArms;
   if ( p == TheNXEnableBackgroundParameter )
      return &p_enableBackground;

   return nullptr;
}

// ----------------------------------------------------------------------------

bool NukeXInstance::AllocateParameter( size_type /*sizeOrLength*/, const MetaParameter* /*p*/, size_type /*tableRow*/ )
{
   // No variable-length parameters
   return false;
}

// ----------------------------------------------------------------------------

size_type NukeXInstance::ParameterLength( const MetaParameter* /*p*/, size_type /*tableRow*/ ) const
{
   // No variable-length parameters
   return 0;
}

// ----------------------------------------------------------------------------

} // namespace pcl
