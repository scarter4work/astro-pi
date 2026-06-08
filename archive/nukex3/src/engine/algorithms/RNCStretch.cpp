//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX v3 - Statistical Stacking + Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "RNCStretch.h"

#include <cmath>
#include <algorithm>

namespace pcl
{

// ----------------------------------------------------------------------------

RNCStretch::RNCStretch()
{
   AddParameter( AlgorithmParameter(
      "stretchFactor", "Stretch Factor", 2.5, 1.0, 10.0, 2,
      "Stretch intensity. Higher = more aggressive. 1.0 = linear."
   ) );

   AddParameter( AlgorithmParameter(
      "colorBoost", "Color Boost", 1.0, 0.5, 2.0, 2,
      "Color saturation multiplier. > 1 boosts, < 1 reduces."
   ) );

   AddParameter( AlgorithmParameter(
      "blackPoint", "Black Point", 0.0, 0.0, 0.5, 6,
      "Subtract this value before stretching."
   ) );

   AddParameter( AlgorithmParameter(
      "saturationProtect", "Saturation Protection", 0.95, 0.5, 1.0, 2,
      "Threshold above which colors are protected from over-saturation."
   ) );
}

// ----------------------------------------------------------------------------

String RNCStretch::Description() const
{
   return "RNC Color Stretch preserves color ratios during stretching, "
          "preventing desaturation and color shifts. The algorithm stretches "
          "luminance while maintaining proportional relationships between RGB channels.";
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

   return std::pow( x, 1.0 / stretchFactor );
}

// ----------------------------------------------------------------------------

double RNCStretch::Apply( double value ) const
{
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
         double r = image( x, y, 0 );
         double g = image( x, y, 1 );
         double b = image( x, y, 2 );

         r = std::max( 0.0, (r - blackPoint) / (1.0 - blackPoint) );
         g = std::max( 0.0, (g - blackPoint) / (1.0 - blackPoint) );
         b = std::max( 0.0, (b - blackPoint) / (1.0 - blackPoint) );

         double lumOrig = LUM_R * r + LUM_G * g + LUM_B * b;

         if ( lumOrig <= 1e-10 )
         {
            image( x, y, 0 ) = 0.0;
            image( x, y, 1 ) = 0.0;
            image( x, y, 2 ) = 0.0;
            continue;
         }

         double lumStretched = PowerStretch( lumOrig );
         double scale = lumStretched / lumOrig;

         double rNew = r * scale;
         double gNew = g * scale;
         double bNew = b * scale;

         if ( std::abs( colorBoost - 1.0 ) > 0.001 )
         {
            double lumNew = LUM_R * rNew + LUM_G * gNew + LUM_B * bNew;
            rNew = lumNew + (rNew - lumNew) * colorBoost;
            gNew = lumNew + (gNew - lumNew) * colorBoost;
            bNew = lumNew + (bNew - lumNew) * colorBoost;
         }

         double maxChannel = std::max( { rNew, gNew, bNew } );
         if ( maxChannel > satProtect )
         {
            double lumNew = LUM_R * rNew + LUM_G * gNew + LUM_B * bNew;
            double overFactor = (maxChannel - satProtect) / (1.0 - satProtect);
            overFactor = std::min( 1.0, overFactor );
            rNew = rNew * (1.0 - overFactor) + lumNew * overFactor;
            gNew = gNew * (1.0 - overFactor) + lumNew * overFactor;
            bNew = bNew * (1.0 - overFactor) + lumNew * overFactor;
         }

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

         image( x, y, 0 ) = Clamp( rNew );
         image( x, y, 1 ) = Clamp( gNew );
         image( x, y, 2 ) = Clamp( bNew );
      }
   }
}

// ----------------------------------------------------------------------------

void RNCStretch::AutoConfigure( double median, double mad )
{
   double stretchFactor;
   if ( median < 0.001 )
      stretchFactor = 6.0;
   else if ( median < 0.01 )
      stretchFactor = 4.0 + 2.0 * (0.01 - median) / 0.01;
   else if ( median < 0.1 )
      stretchFactor = 2.0 + 2.0 * (0.1 - median) / 0.1;
   else
      stretchFactor = 1.0 + 1.0 * (0.5 - std::min( median, 0.5 )) / 0.5;

   SetStretchFactor( stretchFactor );

   double bp = std::max( 0.0, median - 2.5 * mad );
   SetBlackPoint( bp );

   SetColorBoost( 1.1 );
   SetSaturationProtect( 0.95 );
}

// ----------------------------------------------------------------------------

std::unique_ptr<IStretchAlgorithm> RNCStretch::Clone() const
{
   auto clone = std::make_unique<RNCStretch>();
   for ( const AlgorithmParameter& param : m_parameters )
      clone->SetParameter( param.id, param.value );
   return clone;
}

// ----------------------------------------------------------------------------

} // namespace pcl
