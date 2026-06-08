//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX v3 - Statistical Stacking + Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "VeraluxStretch.h"

#include <cmath>

namespace pcl
{

// ----------------------------------------------------------------------------

VeraluxStretch::VeraluxStretch()
{
   AddParameter( AlgorithmParameter(
      "exposure", "Exposure", 0.0, -3.0, 3.0, 2,
      "Exposure adjustment in EV-like units."
   ) );

   AddParameter( AlgorithmParameter(
      "contrast", "Contrast", 1.0, 0.5, 2.0, 2,
      "Contrast adjustment. Affects the steepness of the mid-tone curve."
   ) );

   AddParameter( AlgorithmParameter(
      "toeStrength", "Toe Strength", 0.5, 0.0, 1.0, 2,
      "Shadow roll-off strength."
   ) );

   AddParameter( AlgorithmParameter(
      "shoulderStrength", "Shoulder Strength", 0.5, 0.0, 1.0, 2,
      "Highlight compression strength."
   ) );

   AddParameter( AlgorithmParameter(
      "blackPoint", "Black Point", 0.0, 0.0, 0.2, 4,
      "Minimum output level."
   ) );

   AddParameter( AlgorithmParameter(
      "whitePoint", "White Point", 1.0, 0.8, 1.0, 4,
      "Maximum output level."
   ) );
}

// ----------------------------------------------------------------------------

String VeraluxStretch::Description() const
{
   return "Veralux emulates the characteristic S-curve response of photographic "
          "film. Natural-looking contrast with smooth shadow roll-off and "
          "gentle highlight compression.";
}

// ----------------------------------------------------------------------------

double VeraluxStretch::ToeCurve( double x, double strength ) const
{
   if ( strength <= 0.0 )
      return x;

   double toePoint = 0.3 * strength;
   if ( x >= toePoint )
      return x;

   double t = x / toePoint;
   double toeValue = toePoint * t * t * (3.0 - 2.0 * t);

   return toeValue;
}

// ----------------------------------------------------------------------------

double VeraluxStretch::ShoulderCurve( double x, double strength ) const
{
   if ( strength <= 0.0 )
      return x;

   double shoulderStart = 1.0 - 0.3 * strength;
   if ( x <= shoulderStart )
      return x;

   double range = 1.0 - shoulderStart;
   double t = (x - shoulderStart) / range;
   double compressed = t * (2.0 - t);

   return shoulderStart + range * compressed;
}

// ----------------------------------------------------------------------------

double VeraluxStretch::FilmCurve( double x ) const
{
   double exposure = Exposure();
   double contrast = Contrast();
   double toeStrength = ToeStrength();
   double shoulderStrength = ShoulderStrength();
   double blackPoint = BlackPoint();
   double whitePoint = WhitePoint();

   double exposureFactor = std::pow( 2.0, exposure );
   double adjusted = x * exposureFactor;

   double toed = ToeCurve( adjusted, toeStrength );

   double contrasted;
   if ( contrast != 1.0 )
   {
      double centered = toed - 0.5;
      contrasted = 0.5 + centered * contrast;
   }
   else
   {
      contrasted = toed;
   }

   double shouldered = ShoulderCurve( contrasted, shoulderStrength );

   double outputRange = whitePoint - blackPoint;
   double result = blackPoint + shouldered * outputRange;

   return Clamp( result );
}

// ----------------------------------------------------------------------------

double VeraluxStretch::Apply( double value ) const
{
   if ( value <= 0.0 )
      return 0.0;
   if ( value >= 1.0 )
      return 1.0;

   return FilmCurve( value );
}

// ----------------------------------------------------------------------------

void VeraluxStretch::AutoConfigure( double median, double mad )
{
   double targetMedian = 0.3;
   double currentMedian = median;

   double exposure = 0.0;
   if ( currentMedian > 0.001 && currentMedian < 0.9 )
   {
      exposure = std::log2( targetMedian / currentMedian );
      exposure = Clamp( exposure, -3.0, 3.0 );
   }
   SetExposure( exposure );

   SetContrast( 1.0 );

   double toeStrength = 0.5;
   if ( median < 0.1 )
      toeStrength = 0.7;
   SetToeStrength( toeStrength );

   SetShoulderStrength( 0.5 );
   SetBlackPoint( 0.0 );
   SetWhitePoint( 1.0 );
}

// ----------------------------------------------------------------------------

void VeraluxStretch::PresetNeutral()
{
   SetExposure( 0.0 );
   SetContrast( 1.0 );
   SetToeStrength( 0.3 );
   SetShoulderStrength( 0.3 );
   SetBlackPoint( 0.0 );
   SetWhitePoint( 1.0 );
}

// ----------------------------------------------------------------------------

void VeraluxStretch::PresetHighContrast()
{
   SetExposure( 0.0 );
   SetContrast( 1.5 );
   SetToeStrength( 0.6 );
   SetShoulderStrength( 0.6 );
   SetBlackPoint( 0.02 );
   SetWhitePoint( 0.98 );
}

// ----------------------------------------------------------------------------

void VeraluxStretch::PresetLowContrast()
{
   SetExposure( 0.0 );
   SetContrast( 0.7 );
   SetToeStrength( 0.2 );
   SetShoulderStrength( 0.2 );
   SetBlackPoint( 0.05 );
   SetWhitePoint( 0.95 );
}

// ----------------------------------------------------------------------------

void VeraluxStretch::PresetCinematic()
{
   SetExposure( -0.3 );
   SetContrast( 1.2 );
   SetToeStrength( 0.7 );
   SetShoulderStrength( 0.8 );
   SetBlackPoint( 0.03 );
   SetWhitePoint( 0.97 );
}

// ----------------------------------------------------------------------------

std::unique_ptr<IStretchAlgorithm> VeraluxStretch::Clone() const
{
   auto clone = std::make_unique<VeraluxStretch>();
   for ( const AlgorithmParameter& param : m_parameters )
      clone->SetParameter( param.id, param.value );
   return clone;
}

// ----------------------------------------------------------------------------

} // namespace pcl
