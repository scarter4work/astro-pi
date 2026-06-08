//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "VeraluxStretch.h"

#include <cmath>

namespace pcl
{

// ----------------------------------------------------------------------------

VeraluxStretch::VeraluxStretch()
{
   // Exposure - shifts the curve left/right (like EV adjustment)
   AddParameter( AlgorithmParameter(
      "exposure",
      "Exposure",
      0.0,      // default (no shift)
      -3.0,     // min (darker)
      3.0,      // max (brighter)
      2,        // precision
      "Exposure adjustment in EV-like units. Positive = brighter, negative = darker."
   ) );

   // Contrast - affects the slope of the linear region
   AddParameter( AlgorithmParameter(
      "contrast",
      "Contrast",
      1.0,      // default
      0.5,      // min (low contrast)
      2.0,      // max (high contrast)
      2,        // precision
      "Contrast adjustment. Affects the steepness of the mid-tone curve."
   ) );

   // Toe strength - controls shadow roll-off
   AddParameter( AlgorithmParameter(
      "toeStrength",
      "Toe Strength",
      0.5,      // default
      0.0,      // min (linear shadows)
      1.0,      // max (strong toe)
      2,        // precision
      "Shadow roll-off strength. Higher values create smoother shadow transitions."
   ) );

   // Shoulder strength - controls highlight compression
   AddParameter( AlgorithmParameter(
      "shoulderStrength",
      "Shoulder Strength",
      0.5,      // default
      0.0,      // min (linear highlights)
      1.0,      // max (strong shoulder)
      2,        // precision
      "Highlight compression strength. Higher values protect highlights better."
   ) );

   // Black point
   AddParameter( AlgorithmParameter(
      "blackPoint",
      "Black Point",
      0.0,      // default
      0.0,      // min
      0.2,      // max
      4,        // precision
      "Minimum output level (D-max in film terms)."
   ) );

   // White point
   AddParameter( AlgorithmParameter(
      "whitePoint",
      "White Point",
      1.0,      // default
      0.8,      // min
      1.0,      // max
      4,        // precision
      "Maximum output level (D-min in film terms)."
   ) );
}

// ----------------------------------------------------------------------------

String VeraluxStretch::Description() const
{
   return "Veralux emulates the characteristic S-curve response of photographic "
          "film. It provides natural-looking contrast with smooth shadow roll-off "
          "(toe) and gentle highlight compression (shoulder). This creates images "
          "with a pleasing, filmic aesthetic that avoids the harsh look of purely "
          "mathematical stretches.";
}

// ----------------------------------------------------------------------------

double VeraluxStretch::ToeCurve( double x, double strength ) const
{
   // Toe curve: smooth transition from black
   // Uses a modified power function
   if ( strength <= 0.0 )
      return x;

   double toePoint = 0.3 * strength;
   if ( x >= toePoint )
      return x;

   // Smooth toe using quadratic blend
   double t = x / toePoint;
   double toeValue = toePoint * t * t * (3.0 - 2.0 * t);

   return toeValue;
}

// ----------------------------------------------------------------------------

double VeraluxStretch::ShoulderCurve( double x, double strength ) const
{
   // Shoulder curve: smooth compression of highlights
   if ( strength <= 0.0 )
      return x;

   double shoulderStart = 1.0 - 0.3 * strength;
   if ( x <= shoulderStart )
      return x;

   // Smooth shoulder using inverse of toe
   double range = 1.0 - shoulderStart;
   double t = (x - shoulderStart) / range;

   // Compress using smoothstep
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

   // Apply exposure adjustment (like a log scale shift)
   double exposureFactor = std::pow( 2.0, exposure );
   double adjusted = x * exposureFactor;

   // Apply toe (shadows)
   double toed = ToeCurve( adjusted, toeStrength );

   // Apply contrast (mid-tones)
   // Center contrast adjustment around 0.5
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

   // Apply shoulder (highlights)
   double shouldered = ShoulderCurve( contrasted, shoulderStrength );

   // Apply output range (black point / white point)
   double outputRange = whitePoint - blackPoint;
   double result = blackPoint + shouldered * outputRange;

   return Clamp( result );
}

// ----------------------------------------------------------------------------

double VeraluxStretch::Apply( double value ) const
{
   if ( value <= 0.0 )
      return BlackPoint();
   if ( value >= 1.0 )
      return WhitePoint();

   return FilmCurve( value );
}

// ----------------------------------------------------------------------------

void VeraluxStretch::AutoConfigure( const RegionStatistics& stats )
{
   // Calculate exposure to bring median to target
   double targetMedian = 0.3; // Slightly brighter for film look
   double currentMedian = stats.median;

   double exposure = 0.0;
   if ( currentMedian > 0.001 && currentMedian < 0.9 )
   {
      // Calculate EV shift needed
      exposure = std::log2( targetMedian / currentMedian );
      exposure = Clamp( exposure, -3.0, 3.0 );
   }
   SetExposure( exposure );

   // Set contrast based on image dynamic range
   double contrast = 1.0;
   if ( stats.dynamicRange > 3.0 )
   {
      // High DR - slightly reduce contrast
      contrast = 0.9;
   }
   else if ( stats.dynamicRange < 1.5 )
   {
      // Low DR - boost contrast
      contrast = 1.2;
   }
   SetContrast( contrast );

   // Set toe strength based on shadow detail
   double toeStrength = 0.5;
   if ( stats.min < 0.01 && stats.median < 0.1 )
   {
      // Faint image - stronger toe for shadow protection
      toeStrength = 0.7;
   }
   SetToeStrength( toeStrength );

   // Set shoulder based on highlights
   double shoulderStrength = 0.5;
   if ( stats.max > 0.9 || stats.clippingPct > 0.5 )
   {
      // Bright highlights - stronger shoulder
      shoulderStrength = 0.7;
   }
   SetShoulderStrength( shoulderStrength );

   // Default output range
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
   {
      clone->SetParameter( param.id, param.value );
   }

   return clone;
}

// ----------------------------------------------------------------------------

} // namespace pcl
