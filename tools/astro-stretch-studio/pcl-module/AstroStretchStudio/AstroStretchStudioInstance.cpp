// ----------------------------------------------------------------------------
// AstroStretchStudio Instance Implementation
// ----------------------------------------------------------------------------

#include "AstroStretchStudioInstance.h"
#include "AstroStretchStudioProcess.h"
#include "AstroStretchStudioParameters.h"
#include "AstroStretchStudioZoneHDR.h"

#include <pcl/AutoViewLock.h>
#include <pcl/Console.h>
#include <pcl/StandardStatus.h>
#include <pcl/View.h>
#include <pcl/MuteStatus.h>
#include <pcl/Histogram.h>

#include <cmath>

namespace pcl
{

// ----------------------------------------------------------------------------

AstroStretchStudioInstance::AstroStretchStudioInstance( const MetaProcess* m )
   : ProcessImplementation( m )
{
   SetDefaultParameters();
}

// ----------------------------------------------------------------------------

AstroStretchStudioInstance::AstroStretchStudioInstance( const AstroStretchStudioInstance& x )
   : ProcessImplementation( x )
{
   Assign( x );
}

// ----------------------------------------------------------------------------

void AstroStretchStudioInstance::SetDefaultParameters()
{
   p_algorithm = ASSAlgorithm::Default;

   // OTS defaults
   p_otsObjectType = ASSOTSObjectType::Default;
   p_otsBackgroundTarget = TheASSOTSBackgroundTargetParameter->DefaultValue();
   p_otsStretchIntensity = TheASSOTSStretchIntensityParameter->DefaultValue();
   p_otsProtectHighlights = TheASSOTSProtectHighlightsParameter->DefaultValue();
   p_otsPreserveColor = TheASSOTSPreserveColorParameter->DefaultValue();

   // OTS HDR defaults
   p_otsHDREnabled = TheASSOTSHDREnabledParameter->DefaultValue();
   p_otsHDRAmount = TheASSOTSHDRAmountParameter->DefaultValue();
   p_otsHDRThreshold = TheASSOTSHDRThresholdParameter->DefaultValue();
   p_otsStarProtection = TheASSOTSStarProtectionParameter->DefaultValue();

   // SAS defaults
   p_sasNumScales = int32( TheASSSASNumScalesParameter->DefaultValue() );
   p_sasBackgroundTarget = TheASSSASBackgroundTargetParameter->DefaultValue();
   p_sasFineScaleGain = TheASSSASFineScaleGainParameter->DefaultValue();
   p_sasMidScaleGain = TheASSSASMidScaleGainParameter->DefaultValue();
   p_sasCoarseScaleGain = TheASSSASCoarseScaleGainParameter->DefaultValue();
   p_sasCompressionAlpha = TheASSSASCompressionAlphaParameter->DefaultValue();
   p_sasHighlightProtection = TheASSSASHighlightProtectionParameter->DefaultValue();
   p_sasNoiseThreshold = TheASSSASNoiseThresholdParameter->DefaultValue();
   p_sasFlattenBackground = TheASSSASFlattenBackgroundParameter->DefaultValue();
   p_sasPreserveColor = TheASSSASPreserveColorParameter->DefaultValue();

   // Zone HDR defaults
   p_zoneHDREnabled = TheASSZoneHDREnabledParameter->DefaultValue();
   p_zonePreviewMode = ASSZonePreviewMode::Default;
   p_zoneCount = 0;
   for ( int i = 0; i < 8; ++i )
   {
      p_zoneIntensity[i] = 0.0;
      p_zoneSaturation[i] = 0.0;
   }
   p_zoneSelectedIndex = 0;
}

// ----------------------------------------------------------------------------

void AstroStretchStudioInstance::Assign( const ProcessImplementation& p )
{
   const AstroStretchStudioInstance* x = dynamic_cast<const AstroStretchStudioInstance*>( &p );
   if ( x != nullptr )
   {
      p_algorithm = x->p_algorithm;

      p_otsObjectType = x->p_otsObjectType;
      p_otsBackgroundTarget = x->p_otsBackgroundTarget;
      p_otsStretchIntensity = x->p_otsStretchIntensity;
      p_otsProtectHighlights = x->p_otsProtectHighlights;
      p_otsPreserveColor = x->p_otsPreserveColor;

      p_otsHDREnabled = x->p_otsHDREnabled;
      p_otsHDRAmount = x->p_otsHDRAmount;
      p_otsHDRThreshold = x->p_otsHDRThreshold;
      p_otsStarProtection = x->p_otsStarProtection;

      p_sasNumScales = x->p_sasNumScales;
      p_sasBackgroundTarget = x->p_sasBackgroundTarget;
      p_sasFineScaleGain = x->p_sasFineScaleGain;
      p_sasMidScaleGain = x->p_sasMidScaleGain;
      p_sasCoarseScaleGain = x->p_sasCoarseScaleGain;
      p_sasCompressionAlpha = x->p_sasCompressionAlpha;
      p_sasHighlightProtection = x->p_sasHighlightProtection;
      p_sasNoiseThreshold = x->p_sasNoiseThreshold;
      p_sasFlattenBackground = x->p_sasFlattenBackground;
      p_sasPreserveColor = x->p_sasPreserveColor;

      p_zoneHDREnabled = x->p_zoneHDREnabled;
      p_zonePreviewMode = x->p_zonePreviewMode;
      p_zoneCount = x->p_zoneCount;
      for ( int i = 0; i < 8; ++i )
      {
         p_zoneIntensity[i] = x->p_zoneIntensity[i];
         p_zoneSaturation[i] = x->p_zoneSaturation[i];
      }
      p_zoneSelectedIndex = x->p_zoneSelectedIndex;
   }
}

// ----------------------------------------------------------------------------

bool AstroStretchStudioInstance::IsHistoryUpdater( const View& ) const
{
   return true;
}

// ----------------------------------------------------------------------------

UndoFlags AstroStretchStudioInstance::UndoMode( const View& ) const
{
   return UndoFlag::PixelData;
}

// ----------------------------------------------------------------------------

bool AstroStretchStudioInstance::CanExecuteOn( const View& view, String& whyNot ) const
{
   if ( view.Image().IsComplexSample() )
   {
      whyNot = "AstroStretchStudio cannot be executed on complex images.";
      return false;
   }
   return true;
}

// ----------------------------------------------------------------------------

bool AstroStretchStudioInstance::ExecuteOn( View& view )
{
   AutoViewLock lock( view );

   ImageVariant image = view.Image();

   StandardStatus status;
   image.SetStatusCallback( &status );

   Console console;
   console.EnableAbort();

   // Get image data as float for processing
   if ( image.IsFloatSample() )
   {
      switch ( image.BitsPerSample() )
      {
      case 32:
         {
            Image& img = static_cast<Image&>( *image );
            if ( p_algorithm == ASSAlgorithm::OTS )
            {
               console.WriteLn( "<end><cbr>Applying Optimal Transport Stretch..." );
               ApplyOTS( img );
            }
            else
            {
               console.WriteLn( "<end><cbr>Applying Starlet Arctan Stretch..." );
               ApplySAS( img );
            }
            if ( p_zoneHDREnabled )
            {
               console.WriteLn( "<end><cbr>Applying Zone HDR..." );
               ApplyZoneHDR( img );
            }
         }
         break;
      case 64:
         {
            DImage& img = static_cast<DImage&>( *image );
            // Convert to float, process, convert back
            Image workImage( img );  // Copy constructor handles conversion
            if ( p_algorithm == ASSAlgorithm::OTS )
            {
               console.WriteLn( "<end><cbr>Applying Optimal Transport Stretch..." );
               ApplyOTS( workImage );
            }
            else
            {
               console.WriteLn( "<end><cbr>Applying Starlet Arctan Stretch..." );
               ApplySAS( workImage );
            }
            if ( p_zoneHDREnabled )
            {
               console.WriteLn( "<end><cbr>Applying Zone HDR..." );
               ApplyZoneHDR( workImage );
            }
            img.Assign( workImage );
         }
         break;
      }
   }
   else
   {
      // Integer images - convert to float, process, convert back
      switch ( image.BitsPerSample() )
      {
      case 8:
         {
            UInt8Image& img = static_cast<UInt8Image&>( *image );
            Image workImage( img );
            if ( p_algorithm == ASSAlgorithm::OTS )
            {
               console.WriteLn( "<end><cbr>Applying Optimal Transport Stretch..." );
               ApplyOTS( workImage );
            }
            else
            {
               console.WriteLn( "<end><cbr>Applying Starlet Arctan Stretch..." );
               ApplySAS( workImage );
            }
            if ( p_zoneHDREnabled )
            {
               console.WriteLn( "<end><cbr>Applying Zone HDR..." );
               ApplyZoneHDR( workImage );
            }
            img.Assign( workImage );
         }
         break;
      case 16:
         {
            UInt16Image& img = static_cast<UInt16Image&>( *image );
            Image workImage( img );
            if ( p_algorithm == ASSAlgorithm::OTS )
            {
               console.WriteLn( "<end><cbr>Applying Optimal Transport Stretch..." );
               ApplyOTS( workImage );
            }
            else
            {
               console.WriteLn( "<end><cbr>Applying Starlet Arctan Stretch..." );
               ApplySAS( workImage );
            }
            if ( p_zoneHDREnabled )
            {
               console.WriteLn( "<end><cbr>Applying Zone HDR..." );
               ApplyZoneHDR( workImage );
            }
            img.Assign( workImage );
         }
         break;
      case 32:
         {
            UInt32Image& img = static_cast<UInt32Image&>( *image );
            Image workImage( img );
            if ( p_algorithm == ASSAlgorithm::OTS )
            {
               console.WriteLn( "<end><cbr>Applying Optimal Transport Stretch..." );
               ApplyOTS( workImage );
            }
            else
            {
               console.WriteLn( "<end><cbr>Applying Starlet Arctan Stretch..." );
               ApplySAS( workImage );
            }
            if ( p_zoneHDREnabled )
            {
               console.WriteLn( "<end><cbr>Applying Zone HDR..." );
               ApplyZoneHDR( workImage );
            }
            img.Assign( workImage );
         }
         break;
      }
   }

   return true;
}

// ----------------------------------------------------------------------------

void* AstroStretchStudioInstance::LockParameter( const MetaParameter* p, size_type /*tableRow*/ )
{
   if ( p == TheASSAlgorithmParameter )            return &p_algorithm;
   if ( p == TheASSOTSObjectTypeParameter )        return &p_otsObjectType;
   if ( p == TheASSOTSBackgroundTargetParameter )  return &p_otsBackgroundTarget;
   if ( p == TheASSOTSStretchIntensityParameter )  return &p_otsStretchIntensity;
   if ( p == TheASSOTSProtectHighlightsParameter ) return &p_otsProtectHighlights;
   if ( p == TheASSOTSPreserveColorParameter )     return &p_otsPreserveColor;
   if ( p == TheASSOTSHDREnabledParameter )        return &p_otsHDREnabled;
   if ( p == TheASSOTSHDRAmountParameter )         return &p_otsHDRAmount;
   if ( p == TheASSOTSHDRThresholdParameter )      return &p_otsHDRThreshold;
   if ( p == TheASSOTSStarProtectionParameter )    return &p_otsStarProtection;
   if ( p == TheASSSASNumScalesParameter )         return &p_sasNumScales;
   if ( p == TheASSSASBackgroundTargetParameter )  return &p_sasBackgroundTarget;
   if ( p == TheASSSASFineScaleGainParameter )     return &p_sasFineScaleGain;
   if ( p == TheASSSASMidScaleGainParameter )      return &p_sasMidScaleGain;
   if ( p == TheASSSASCoarseScaleGainParameter )   return &p_sasCoarseScaleGain;
   if ( p == TheASSSASCompressionAlphaParameter )  return &p_sasCompressionAlpha;
   if ( p == TheASSSASHighlightProtectionParameter ) return &p_sasHighlightProtection;
   if ( p == TheASSSASNoiseThresholdParameter )    return &p_sasNoiseThreshold;
   if ( p == TheASSSASFlattenBackgroundParameter ) return &p_sasFlattenBackground;
   if ( p == TheASSSASPreserveColorParameter )     return &p_sasPreserveColor;
   if ( p == TheASSZoneHDREnabledParameter )       return &p_zoneHDREnabled;
   if ( p == TheASSZonePreviewModeParameter )      return &p_zonePreviewMode;
   if ( p == TheASSZoneCountParameter )            return &p_zoneCount;
   if ( p == TheASSZoneSelectedIndexParameter )    return &p_zoneSelectedIndex;
   return nullptr;
}

// ----------------------------------------------------------------------------

bool AstroStretchStudioInstance::AllocateParameter( size_type sizeOrLength,
                                                     const MetaParameter* p,
                                                     size_type tableRow )
{
   // No variable-length parameters
   return false;
}

// ----------------------------------------------------------------------------

size_type AstroStretchStudioInstance::ParameterLength( const MetaParameter* p,
                                                        size_type tableRow ) const
{
   return 0;
}

// ----------------------------------------------------------------------------
// OTS Implementation
// ----------------------------------------------------------------------------

void AstroStretchStudioInstance::ApplyOTS( Image& image ) const
{
   const int resolution = 65536;

   // Extract or compute luminance
   Image L;
   bool isColor = image.NumberOfChannels() >= 3;

   if ( isColor && p_otsPreserveColor )
   {
      // Extract CIE luminance
      L.AllocateData( image.Width(), image.Height() );
      for ( int y = 0; y < image.Height(); ++y )
         for ( int x = 0; x < image.Width(); ++x )
         {
            double r = image( x, y, 0 );
            double g = image( x, y, 1 );
            double b = image( x, y, 2 );
            L( x, y ) = float( 0.2126 * r + 0.7152 * g + 0.0722 * b );
         }
   }
   else
   {
      // Use first channel or grayscale
      L.AllocateData( image.Width(), image.Height() );
      for ( int y = 0; y < image.Height(); ++y )
         for ( int x = 0; x < image.Width(); ++x )
            L( x, y ) = image( x, y, 0 );
   }

   // Store original luminance for color reconstruction
   Image L_orig( L );

   // Compute source histogram and CDF
   FVector srcCDF( resolution );
   ComputeHistogramCDF( L, srcCDF );

   // Generate target CDF based on object type
   FVector tgtCDF( resolution );
   GenerateTargetCDF( tgtCDF, p_otsObjectType, p_otsBackgroundTarget );

   // Compute optimal transport map
   FVector transportMap( resolution );
   ComputeTransportMap( transportMap, srcCDF, tgtCDF );

   // Apply highlight protection
   if ( p_otsProtectHighlights > 0 )
   {
      for ( int i = 0; i < resolution; ++i )
      {
         double x = double( i ) / ( resolution - 1 );
         double t = ( x - 0.7 ) / 0.25;
         t = Max( 0.0, Min( 1.0, t ) );
         double blend = t * t * ( 3 - 2 * t ) * p_otsProtectHighlights;
         transportMap[i] = float( ( 1 - blend ) * transportMap[i] + blend * x );
      }
   }

   // Apply stretch intensity blend
   for ( int i = 0; i < resolution; ++i )
   {
      double identity = double( i ) / ( resolution - 1 );
      transportMap[i] = float( ( 1 - p_otsStretchIntensity ) * identity +
                         p_otsStretchIntensity * transportMap[i] );
   }

   // Apply transport map to luminance
   for ( int y = 0; y < L.Height(); ++y )
      for ( int x = 0; x < L.Width(); ++x )
      {
         int bin = Min( resolution - 1, Max( 0, RoundInt( L( x, y ) * ( resolution - 1 ) ) ) );
         L( x, y ) = transportMap[bin];
      }

   // Apply HDR compression if enabled
   if ( p_otsHDREnabled )
      ApplyHDR( L );

   // Reconstruct color or apply to image
   if ( isColor && p_otsPreserveColor )
   {
      for ( int y = 0; y < image.Height(); ++y )
         for ( int x = 0; x < image.Width(); ++x )
         {
            double origLum = L_orig( x, y );
            double newLum = L( x, y );
            if ( origLum > 1e-10 )
            {
               double scale = newLum / origLum;
               for ( int c = 0; c < image.NumberOfChannels(); ++c )
                  image( x, y, c ) = float( Range( double( image( x, y, c ) ) * scale, 0.0, 1.0 ) );
            }
         }
   }
   else
   {
      for ( int c = 0; c < image.NumberOfChannels(); ++c )
         for ( int y = 0; y < image.Height(); ++y )
            for ( int x = 0; x < image.Width(); ++x )
            {
               int bin = Min( resolution - 1, Max( 0, RoundInt( double( image( x, y, c ) ) * ( resolution - 1 ) ) ) );
               image( x, y, c ) = transportMap[bin];
            }
   }
}

// ----------------------------------------------------------------------------

void AstroStretchStudioInstance::GenerateTargetCDF( FVector& cdf, int objectType, double bgTarget ) const
{
   const int n = cdf.Length();
   FVector pdf( n );

   // Initialize to zero
   for ( int i = 0; i < n; ++i )
      pdf[i] = 0;

   for ( int i = 0; i < n; ++i )
   {
      double x = double( i ) / ( n - 1 );

      switch ( objectType )
      {
      case ASSOTSObjectType::Nebula:
         {
            // Background peak
            double diff1 = ( x - bgTarget ) / 0.03;
            pdf[i] = float( 0.3 * std::exp( -0.5 * diff1 * diff1 ) );
            // Nebula body
            if ( x >= bgTarget && x <= 0.7 )
               pdf[i] += float( 0.5 * std::pow( x - bgTarget, 1.0 ) * std::pow( 0.7 - x, 2.0 ) );
            // Highlights
            if ( x >= 0.6 && x <= 0.95 )
               pdf[i] += float( 0.2 * std::pow( x - 0.6, 0.5 ) * std::pow( 0.95 - x, 3.0 ) );
         }
         break;

      case ASSOTSObjectType::Galaxy:
         {
            double diff1 = ( x - bgTarget ) / 0.025;
            pdf[i] = float( 0.25 * std::exp( -0.5 * diff1 * diff1 ) );
            if ( x >= bgTarget && x <= 0.5 )
               pdf[i] += float( 0.35 * std::pow( x - bgTarget, 1.5 ) * std::pow( 0.5 - x, 1.5 ) );
            if ( x >= 0.4 && x <= 0.75 )
               pdf[i] += float( 0.25 * std::pow( x - 0.4, 2.0 ) * std::pow( 0.75 - x, 1.0 ) );
            if ( x >= 0.7 && x <= 0.9 )
               pdf[i] += 0.15f;
         }
         break;

      case ASSOTSObjectType::StarCluster:
         {
            double diff1 = ( x - bgTarget * 0.8 ) / 0.02;
            pdf[i] = float( 0.20 * std::exp( -0.5 * diff1 * diff1 ) );
            if ( x >= 0.15 && x <= 0.70 )
               pdf[i] += float( 0.50 * std::pow( x - 0.15, 0.5 ) * std::pow( 0.70 - x, 1.0 ) );
            if ( x >= 0.60 && x <= 0.95 )
               pdf[i] += float( 0.30 * std::pow( x - 0.60, 1.0 ) * std::pow( 0.95 - x, 4.0 ) );
         }
         break;

      case ASSOTSObjectType::DarkNebula:
         {
            double diff1 = ( x - bgTarget * 1.3 ) / 0.04;
            pdf[i] = float( 0.15 * std::exp( -0.5 * diff1 * diff1 ) );
            if ( x >= 0.05 && x <= bgTarget )
               pdf[i] += float( 0.40 * std::pow( x - 0.05, 2.0 ) * std::pow( bgTarget - x, 1.0 ) );
            if ( x >= bgTarget && x <= 0.55 )
               pdf[i] += float( 0.30 * std::pow( x - bgTarget, 1.0 ) * std::pow( 0.55 - x, 1.5 ) );
            if ( x >= 0.5 && x <= 0.85 )
               pdf[i] += 0.15f;
         }
         break;

      default:
         pdf[i] = 1.0f;
      }
   }

   // Normalize PDF and compute CDF
   double sum = 0;
   for ( int i = 0; i < n; ++i )
      sum += pdf[i];
   if ( sum > 0 )
      for ( int i = 0; i < n; ++i )
         pdf[i] /= float( sum );

   cdf[0] = pdf[0];
   for ( int i = 1; i < n; ++i )
      cdf[i] = cdf[i-1] + pdf[i];

   // Ensure ends are 0 and 1
   if ( cdf[n-1] > 0 )
      for ( int i = 0; i < n; ++i )
         cdf[i] /= cdf[n-1];
}

// ----------------------------------------------------------------------------

void AstroStretchStudioInstance::ComputeHistogramCDF( const Image& image, FVector& cdf ) const
{
   const int n = cdf.Length();
   FVector hist( n );

   // Initialize histogram to zero
   for ( int i = 0; i < n; ++i )
      hist[i] = 0;

   for ( Image::const_sample_iterator i( image ); i; ++i )
   {
      int bin = Min( n - 1, Max( 0, RoundInt( double( *i ) * ( n - 1 ) ) ) );
      hist[bin]++;
   }

   double sum = 0;
   for ( int i = 0; i < n; ++i )
      sum += hist[i];

   cdf[0] = float( hist[0] / sum );
   for ( int i = 1; i < n; ++i )
      cdf[i] = cdf[i-1] + float( hist[i] / sum );
}

// ----------------------------------------------------------------------------

void AstroStretchStudioInstance::ComputeTransportMap( FVector& tmap,
                                                       const FVector& srcCDF,
                                                       const FVector& tgtCDF ) const
{
   const int n = tmap.Length();

   for ( int i = 0; i < n; ++i )
   {
      double quantile = srcCDF[i];

      // Binary search for inverse CDF
      int lo = 0, hi = n - 1;
      while ( lo < hi )
      {
         int mid = ( lo + hi ) / 2;
         if ( tgtCDF[mid] < quantile )
            lo = mid + 1;
         else
            hi = mid;
      }

      tmap[i] = float( double( lo ) / ( n - 1 ) );
   }
}

// ----------------------------------------------------------------------------
// HDR Compression Implementation
// ----------------------------------------------------------------------------

void AstroStretchStudioInstance::ApplyHDR( Image& L ) const
{
   const int width = L.Width();
   const int height = L.Height();
   const double threshold = p_otsHDRThreshold;
   const double amount = p_otsHDRAmount;
   const double starProt = p_otsStarProtection;

   // Bayer 4x4 dithering matrix
   static const double bayer4x4[4][4] = {
      { 0.0/16, 8.0/16, 2.0/16, 10.0/16 },
      { 12.0/16, 4.0/16, 14.0/16, 6.0/16 },
      { 3.0/16, 11.0/16, 1.0/16, 9.0/16 },
      { 15.0/16, 7.0/16, 13.0/16, 5.0/16 }
   };

   // Create star mask (detect local maxima with high contrast)
   Image starMask;
   starMask.AllocateData( width, height );
   starMask.Zero();

   // Detect stars: local maxima with high contrast
   for ( int y = 2; y < height - 2; ++y )
   {
      for ( int x = 2; x < width - 2; ++x )
      {
         double center = L( x, y );

         // Only check bright pixels
         if ( center < 0.5 )
            continue;

         // Check if local maximum in 5x5 neighborhood
         bool isLocalMax = true;
         double neighborSum = 0;
         int neighborCount = 0;

         for ( int dy = -2; dy <= 2 && isLocalMax; ++dy )
         {
            for ( int dx = -2; dx <= 2; ++dx )
            {
               if ( dx == 0 && dy == 0 )
                  continue;

               double neighbor = L( x + dx, y + dy );
               neighborSum += neighbor;
               ++neighborCount;

               if ( neighbor > center )
               {
                  isLocalMax = false;
                  break;
               }
            }
         }

         if ( isLocalMax && neighborCount > 0 )
         {
            double neighborMean = neighborSum / neighborCount;
            double contrastRatio = ( neighborMean > 0.01 ) ? center / neighborMean : 1.0;

            // Star cores have high contrast ratio
            if ( contrastRatio > 1.5 )
            {
               // Create a soft star mask with falloff from center
               for ( int dy = -2; dy <= 2; ++dy )
               {
                  for ( int dx = -2; dx <= 2; ++dx )
                  {
                     double dist = Sqrt( double( dx * dx + dy * dy ) );
                     double falloff = Max( 0.0, 1.0 - dist / 3.0 );
                     int px = x + dx;
                     int py = y + dy;
                     if ( px >= 0 && px < width && py >= 0 && py < height )
                        starMask( px, py ) = float( Max( double( starMask( px, py ) ), falloff ) );
                  }
               }
            }
         }
      }
   }

   // Apply HDR compression with dithering and star protection
   const double twoOverPi = 2.0 / 3.14159265358979323846;

   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         double lum = L( x, y );

         if ( lum <= threshold )
            continue;

         // Compute smooth brightness mask with smoothstep
         double t = ( lum - threshold ) / ( 1.0 - threshold );
         double mask = t * t * ( 3.0 - 2.0 * t );

         // Add Bayer dithering to prevent banding
         double dither = ( bayer4x4[y % 4][x % 4] - 0.5 ) * 0.05;
         mask = Range( mask + dither, 0.0, 1.0 );

         // Reduce mask based on star protection
         double starVal = starMask( x, y );
         double effectiveMask = mask * ( 1.0 - starVal * starProt );

         if ( effectiveMask <= 0 )
            continue;

         // Apply arctan compression
         double normalizedLum = ( lum - threshold ) / ( 1.0 - threshold );
         double compressed = threshold + twoOverPi * std::atan( 3.0 * normalizedLum ) * ( 1.0 - threshold );

         // Blend original and compressed based on effective mask and HDR amount
         L( x, y ) = float( ( 1.0 - effectiveMask * amount ) * lum + effectiveMask * amount * compressed );
      }
   }
}

// ----------------------------------------------------------------------------
// SAS Implementation
// ----------------------------------------------------------------------------

void AstroStretchStudioInstance::ApplySAS( Image& image ) const
{
   bool isColor = image.NumberOfChannels() >= 3;

   // Extract luminance
   Image L;
   L.AllocateData( image.Width(), image.Height() );

   if ( isColor && p_sasPreserveColor )
   {
      for ( int y = 0; y < image.Height(); ++y )
         for ( int x = 0; x < image.Width(); ++x )
         {
            double r = image( x, y, 0 );
            double g = image( x, y, 1 );
            double b = image( x, y, 2 );
            L( x, y ) = float( 0.2126 * r + 0.7152 * g + 0.0722 * b );
         }
   }
   else
   {
      for ( int y = 0; y < image.Height(); ++y )
         for ( int x = 0; x < image.Width(); ++x )
            L( x, y ) = image( x, y, 0 );
   }

   Image L_orig( L );

   // Starlet decomposition
   Array<Image> scales;
   StarletDecompose( L, scales, p_sasNumScales );

   // Estimate noise from finest scale
   double sigma_noise = EstimateNoise( scales[0] );

   // Process each scale
   for ( int j = 0; j < p_sasNumScales; ++j )
   {
      double gain = ComputeScaleGain( j );

      // Noise thresholding for fine scales
      if ( j <= 1 )
      {
         double threshold = p_sasNoiseThreshold * sigma_noise * 5;
         for ( Image::sample_iterator i( scales[j] ); i; ++i )
         {
            double w = *i;
            if ( Abs( w ) <= threshold )
               *i = 0;
            else
               *i = float( ( w > 0 ) ? ( w - threshold ) : ( w + threshold ) );
         }
      }

      // Apply gain with highlight protection
      if ( p_sasHighlightProtection > 0 )
      {
         // Use original luminance for modulation (simpler than gaussian blur)
         for ( int y = 0; y < scales[j].Height(); ++y )
            for ( int x = 0; x < scales[j].Width(); ++x )
            {
               double intensity = L_orig( x, y );
               double sigmoid = 1.0 / ( 1.0 + std::exp( -8.0 * ( intensity - 0.5 ) ) );
               double mod = Max( 1.0 - p_sasHighlightProtection * sigmoid, 0.2 );
               scales[j]( x, y ) = float( double( scales[j]( x, y ) ) * gain * mod );
            }
      }
      else
      {
         scales[j] *= float( gain );
      }
   }

   // Process coarsest scale
   if ( p_sasFlattenBackground )
   {
      double coarseTarget = p_sasBackgroundTarget * 0.5;
      for ( Image::sample_iterator i( scales[p_sasNumScales] ); i; ++i )
         *i = float( 0.2 * double( *i ) + 0.8 * coarseTarget );
   }

   // Reconstruct
   StarletReconstruct( L, scales );

   // Arctangent compression
   const double twoOverPi = 2.0 / 3.14159265358979323846;
   for ( Image::sample_iterator i( L ); i; ++i )
   {
      double x = *i;
      if ( x > p_sasBackgroundTarget )
      {
         double normalized = ( x - p_sasBackgroundTarget ) / ( 1.0 - p_sasBackgroundTarget );
         double compressed = twoOverPi * std::atan( p_sasCompressionAlpha * normalized );
         *i = float( p_sasBackgroundTarget + compressed * ( 1.0 - p_sasBackgroundTarget ) );
      }
   }

   // Normalize background
   Array<double> samples;
   for ( Image::const_sample_iterator i( L ); i; ++i )
      samples.Add( double( *i ) );
   Sort( samples.Begin(), samples.End() );
   double currentBg = samples[samples.Length() / 20];

   if ( currentBg > 0 && currentBg != p_sasBackgroundTarget )
   {
      double scale = p_sasBackgroundTarget / currentBg;
      for ( Image::sample_iterator i( L ); i; ++i )
      {
         double v = *i;
         if ( v <= currentBg )
            *i = float( v * scale );
         else
            *i = float( p_sasBackgroundTarget + ( v - currentBg ) / ( 1.0 - currentBg ) * ( 1.0 - p_sasBackgroundTarget ) );
      }
   }

   L.Truncate( 0, 1 );

   // Reconstruct color
   if ( isColor && p_sasPreserveColor )
   {
      for ( int y = 0; y < image.Height(); ++y )
         for ( int x = 0; x < image.Width(); ++x )
         {
            double origLum = L_orig( x, y );
            double newLum = L( x, y );
            if ( origLum > 1e-10 )
            {
               double s = newLum / origLum;
               for ( int c = 0; c < image.NumberOfChannels(); ++c )
                  image( x, y, c ) = float( Range( double( image( x, y, c ) ) * s, 0.0, 1.0 ) );
            }
         }
   }
   else
   {
      for ( int c = 0; c < image.NumberOfChannels(); ++c )
         for ( int y = 0; y < image.Height(); ++y )
            for ( int x = 0; x < image.Width(); ++x )
               image( x, y, c ) = L( x, y );
   }
}

// ----------------------------------------------------------------------------

void AstroStretchStudioInstance::StarletDecompose( const Image& image,
                                                    Array<Image>& scales,
                                                    int numScales ) const
{
   scales.Clear();
   Image current( image );

   // B3-spline kernel [1,4,6,4,1]/16
   static const float b3[] = { 1.0f/16, 4.0f/16, 6.0f/16, 4.0f/16, 1.0f/16 };

   for ( int j = 0; j < numScales; ++j )
   {
      Image smooth;
      smooth.AllocateData( current.Width(), current.Height() );
      int spacing = 1 << j;

      // Separable convolution with spacing (à trous)
      Image temp;
      temp.AllocateData( current.Width(), current.Height() );

      // Horizontal
      for ( int y = 0; y < current.Height(); ++y )
         for ( int x = 0; x < current.Width(); ++x )
         {
            double sum = 0;
            for ( int k = -2; k <= 2; ++k )
            {
               int xx = x + k * spacing;
               xx = Max( 0, Min( current.Width() - 1, xx ) );
               sum += b3[k+2] * current( xx, y );
            }
            temp( x, y ) = float( sum );
         }

      // Vertical
      for ( int y = 0; y < current.Height(); ++y )
         for ( int x = 0; x < current.Width(); ++x )
         {
            double sum = 0;
            for ( int k = -2; k <= 2; ++k )
            {
               int yy = y + k * spacing;
               yy = Max( 0, Min( current.Height() - 1, yy ) );
               sum += b3[k+2] * temp( x, yy );
            }
            smooth( x, y ) = float( sum );
         }

      // Wavelet = difference
      Image wavelet;
      wavelet.AllocateData( current.Width(), current.Height() );
      for ( int y = 0; y < current.Height(); ++y )
         for ( int x = 0; x < current.Width(); ++x )
            wavelet( x, y ) = current( x, y ) - smooth( x, y );

      scales.Add( wavelet );
      current = smooth;
   }

   scales.Add( current ); // Residual
}

// ----------------------------------------------------------------------------

void AstroStretchStudioInstance::StarletReconstruct( Image& output,
                                                      const Array<Image>& scales ) const
{
   output.Zero();
   for ( size_type i = 0; i < scales.Length(); ++i )
      output += scales[i];
}

// ----------------------------------------------------------------------------

double AstroStretchStudioInstance::EstimateNoise( const Image& fineScale ) const
{
   Array<double> absValues;
   for ( Image::const_sample_iterator i( fineScale ); i; ++i )
      absValues.Add( Abs( double( *i ) ) );

   Sort( absValues.Begin(), absValues.End() );
   double median = absValues[absValues.Length() / 2];

   Array<double> absDeviations;
   for ( size_type i = 0; i < absValues.Length(); ++i )
      absDeviations.Add( Abs( absValues[i] - median ) );

   Sort( absDeviations.Begin(), absDeviations.End() );
   double mad = absDeviations[absDeviations.Length() / 2];

   return mad * 1.4826;
}

// ----------------------------------------------------------------------------

double AstroStretchStudioInstance::ComputeScaleGain( int j ) const
{
   if ( j <= 1 )
      return p_sasFineScaleGain;
   else if ( j <= 3 )
   {
      double t = ( j - 1.5 ) / 2.0;
      return ( 1 - t ) * p_sasFineScaleGain + t * p_sasMidScaleGain;
   }
   else if ( j <= 5 )
   {
      double t = ( j - 3.5 ) / 2.0;
      return ( 1 - t ) * p_sasMidScaleGain + t * p_sasCoarseScaleGain;
   }
   return p_sasCoarseScaleGain;
}

// ----------------------------------------------------------------------------
// Zone HDR Processing
// ----------------------------------------------------------------------------

void AstroStretchStudioInstance::ApplyZoneHDR( Image& image ) const
{
   if ( !p_zoneHDREnabled )
      return;

   Console console;
   console.WriteLn( "Applying Zone HDR adjustments..." );

   int width = image.Width();
   int height = image.Height();
   int numChannels = image.NumberOfChannels();

   // Compute luminance
   Image luminance;
   luminance.AllocateData( width, height, 1, ColorSpace::Gray );

   if ( numChannels >= 3 )
   {
      // Color image: compute luminance from RGB
      for ( int y = 0; y < height; ++y )
         for ( int x = 0; x < width; ++x )
         {
            double r = image( x, y, 0 );
            double g = image( x, y, 1 );
            double b = image( x, y, 2 );
            luminance( x, y ) = float( 0.2126 * r + 0.7152 * g + 0.0722 * b );
         }
   }
   else
   {
      // Grayscale: copy channel 0
      for ( int y = 0; y < height; ++y )
         for ( int x = 0; x < width; ++x )
            luminance( x, y ) = image( x, y, 0 );
   }

   // Create zone HDR engine and detect zones
   ZoneHDREngine engine;
   int zoneCount = engine.DetectZones( luminance );

   if ( zoneCount == 0 )
   {
      console.WarningLn( "Zone HDR: No zones detected" );
      return;
   }

   console.WriteLn( String().Format( "Zone HDR: Detected %d zones", zoneCount ) );

   // Copy zone adjustments from instance parameters
   Array<Zone>& zones = engine.Zones();
   int effectiveZones = Min( zoneCount, int( 8 ) );
   for ( int i = 0; i < effectiveZones; ++i )
   {
      zones[i].intensity = p_zoneIntensity[i];
      zones[i].saturation = p_zoneSaturation[i];
   }

   // Check if any adjustments are non-zero
   bool hasAdjustments = false;
   for ( int i = 0; i < effectiveZones; ++i )
   {
      if ( Abs( p_zoneIntensity[i] ) > 0.001 || Abs( p_zoneSaturation[i] ) > 0.001 )
      {
         hasAdjustments = true;
         break;
      }
   }

   if ( !hasAdjustments )
   {
      console.WriteLn( "Zone HDR: No adjustments to apply" );
      return;
   }

   // Generate masks and apply adjustments
   engine.GenerateMasks( luminance );
   engine.ApplyZoneAdjustments( luminance, image );

   console.WriteLn( "Zone HDR: Complete" );
}

// ----------------------------------------------------------------------------
// Real-Time Preview
// ----------------------------------------------------------------------------

void AstroStretchStudioInstance::Preview( UInt16Image& image ) const
{
   // Convert UInt16Image to floating point Image for processing
   Image floatImage;
   floatImage.AllocateData( image.Width(), image.Height(), image.NumberOfChannels(), image.ColorSpace() );

   // Copy and normalize UInt16 to [0,1] float
   for ( int c = 0; c < image.NumberOfChannels(); ++c )
      for ( int y = 0; y < image.Height(); ++y )
         for ( int x = 0; x < image.Width(); ++x )
            floatImage( x, y, c ) = float( image( x, y, c ) ) / 65535.0f;

   // Apply the selected algorithm
   if ( p_algorithm == ASSAlgorithm::OTS )
      ApplyOTS( floatImage );
   else
      ApplySAS( floatImage );

   // Apply Zone HDR if enabled
   ApplyZoneHDR( floatImage );

   // Convert back to UInt16
   for ( int c = 0; c < image.NumberOfChannels(); ++c )
      for ( int y = 0; y < image.Height(); ++y )
         for ( int x = 0; x < image.Width(); ++x )
         {
            double v = Range( double( floatImage( x, y, c ) ), 0.0, 1.0 );
            image( x, y, c ) = uint16( v * 65535.0 + 0.5 );
         }
}

// ----------------------------------------------------------------------------

} // namespace pcl

// ----------------------------------------------------------------------------
