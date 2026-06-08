//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "RNCStretch.h"

#include <cmath>
#include <algorithm>

namespace pcl
{

// ----------------------------------------------------------------------------

RNCStretch::RNCStretch()
{
   // Stretch factor - controls power curve
   AddParameter( AlgorithmParameter(
      "stretchFactor",
      "Stretch Factor",
      2.5,      // default
      1.0,      // min (linear)
      10.0,     // max
      2,        // precision
      "Stretch intensity. Higher values = more aggressive non-linear stretch. "
      "1.0 = linear (no stretch)."
   ) );

   // Color boost - enhance saturation after stretch
   AddParameter( AlgorithmParameter(
      "colorBoost",
      "Color Boost",
      1.0,      // default (no boost)
      0.5,      // min
      2.0,      // max
      2,        // precision
      "Color saturation multiplier. Values > 1 boost colors, < 1 reduce."
   ) );

   // Black point
   AddParameter( AlgorithmParameter(
      "blackPoint",
      "Black Point",
      0.0,      // default
      0.0,      // min
      0.5,      // max
      6,        // precision
      "Subtract this value before stretching."
   ) );

   // Saturation protection threshold
   AddParameter( AlgorithmParameter(
      "saturationProtect",
      "Saturation Protection",
      0.95,     // default
      0.5,      // min
      1.0,      // max
      2,        // precision
      "Threshold above which colors are protected from over-saturation. "
      "Lower values provide more protection."
   ) );
}

// ----------------------------------------------------------------------------

String RNCStretch::Description() const
{
   return "RNC Color Stretch preserves color ratios during stretching, "
          "preventing the desaturation and color shifts that commonly occur "
          "with standard stretching methods. The algorithm stretches luminance "
          "while maintaining the proportional relationships between RGB "
          "channels, resulting in accurate star colors and vibrant nebula hues.";
}

// ----------------------------------------------------------------------------

double RNCStretch::PowerStretch( double x ) const
{
   double stretchFactor = StretchFactor();

   if ( x <= 0.0 )
      return 0.0;
   if ( x >= 1.0 )
      return 1.0;
   if ( stretchFactor <= 1.0 )
      return x;

   // Power function stretch: x^(1/stretchFactor)
   // This lifts shadows more than highlights
   return std::pow( x, 1.0 / stretchFactor );
}

// ----------------------------------------------------------------------------

double RNCStretch::Apply( double value ) const
{
   // For single-channel application, just apply the power stretch
   double blackPoint = BlackPoint();

   if ( value <= blackPoint )
      return 0.0;

   double x = (value - blackPoint) / (1.0 - blackPoint);

   return PowerStretch( x );
}

// ----------------------------------------------------------------------------

void RNCStretch::ApplyToImageRGB( Image& image, const Image* mask ) const
{
   if ( image.NumberOfNominalChannels() < 3 )
   {
      // Fall back to standard processing for grayscale
      ApplyToImage( image, mask );
      return;
   }

   double blackPoint = BlackPoint();
   double colorBoost = ColorBoost();
   double satProtect = SaturationProtect();

   int width = image.Width();
   int height = image.Height();

   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         // Get original RGB values
         double r = image( x, y, 0 );
         double g = image( x, y, 1 );
         double b = image( x, y, 2 );

         // Apply black point
         r = std::max( 0.0, (r - blackPoint) / (1.0 - blackPoint) );
         g = std::max( 0.0, (g - blackPoint) / (1.0 - blackPoint) );
         b = std::max( 0.0, (b - blackPoint) / (1.0 - blackPoint) );

         // Calculate original luminance
         double lumOrig = LUM_R * r + LUM_G * g + LUM_B * b;

         if ( lumOrig <= 1e-10 )
         {
            // Pure black - nothing to do
            image( x, y, 0 ) = 0.0;
            image( x, y, 1 ) = 0.0;
            image( x, y, 2 ) = 0.0;
            continue;
         }

         // Stretch the luminance
         double lumStretched = PowerStretch( lumOrig );

         // Calculate scale factor to preserve color ratios
         double scale = lumStretched / lumOrig;

         // Apply scale to each channel
         double rNew = r * scale;
         double gNew = g * scale;
         double bNew = b * scale;

         // Apply color boost
         if ( std::abs( colorBoost - 1.0 ) > 0.001 )
         {
            double lumNew = LUM_R * rNew + LUM_G * gNew + LUM_B * bNew;

            // Boost saturation around the luminance
            rNew = lumNew + (rNew - lumNew) * colorBoost;
            gNew = lumNew + (gNew - lumNew) * colorBoost;
            bNew = lumNew + (bNew - lumNew) * colorBoost;
         }

         // Saturation protection - prevent out-of-gamut colors
         double maxChannel = std::max( { rNew, gNew, bNew } );
         if ( maxChannel > satProtect )
         {
            // Blend toward desaturated to bring back in gamut
            double lumNew = LUM_R * rNew + LUM_G * gNew + LUM_B * bNew;
            double overFactor = (maxChannel - satProtect) / (1.0 - satProtect);
            overFactor = std::min( 1.0, overFactor );

            rNew = rNew * (1.0 - overFactor) + lumNew * overFactor;
            gNew = gNew * (1.0 - overFactor) + lumNew * overFactor;
            bNew = bNew * (1.0 - overFactor) + lumNew * overFactor;
         }

         // Apply mask if present
         if ( mask != nullptr )
         {
            double maskVal = (*mask)( x, y, 0 );
            double origR = image( x, y, 0 );
            double origG = image( x, y, 1 );
            double origB = image( x, y, 2 );

            rNew = origR * (1.0 - maskVal) + rNew * maskVal;
            gNew = origG * (1.0 - maskVal) + gNew * maskVal;
            bNew = origB * (1.0 - maskVal) + bNew * maskVal;
         }

         // Store results
         image( x, y, 0 ) = Clamp( rNew );
         image( x, y, 1 ) = Clamp( gNew );
         image( x, y, 2 ) = Clamp( bNew );
      }
   }
}

// ----------------------------------------------------------------------------

void RNCStretch::AutoConfigure( const RegionStatistics& stats )
{
   // Calculate stretch factor based on image brightness
   double stretchFactor;

   if ( stats.median < 0.001 )
   {
      // Very dark
      stretchFactor = 6.0;
   }
   else if ( stats.median < 0.01 )
   {
      stretchFactor = 4.0 + 2.0 * (0.01 - stats.median) / 0.01;
   }
   else if ( stats.median < 0.1 )
   {
      stretchFactor = 2.0 + 2.0 * (0.1 - stats.median) / 0.1;
   }
   else
   {
      stretchFactor = 1.0 + 1.0 * (0.5 - std::min( stats.median, 0.5 )) / 0.5;
   }

   SetStretchFactor( stretchFactor );

   // Set black point
   double bp = std::max( 0.0, stats.median - 2.5 * stats.mad );
   SetBlackPoint( bp );

   // Default color boost (slight enhancement)
   SetColorBoost( 1.1 );

   // Keep default saturation protection
   SetSaturationProtect( 0.95 );
}

// ----------------------------------------------------------------------------

std::unique_ptr<IStretchAlgorithm> RNCStretch::Clone() const
{
   auto clone = std::make_unique<RNCStretch>();

   for ( const AlgorithmParameter& param : m_parameters )
   {
      clone->SetParameter( param.id, param.value );
   }

   return clone;
}

// ----------------------------------------------------------------------------

} // namespace pcl
