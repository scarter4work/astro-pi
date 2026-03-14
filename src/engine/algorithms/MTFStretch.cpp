//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX v3 - Statistical Stacking + Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "MTFStretch.h"

#include <cmath>

namespace pcl
{

// ----------------------------------------------------------------------------

MTFStretch::MTFStretch()
{
   // Midtones balance - the core parameter
   AddParameter( AlgorithmParameter(
      "midtones",
      "Midtones Balance",
      0.5,      // default (no change)
      0.0001,   // min
      0.9999,   // max
      4,        // precision
      "Midtones balance point. Lower values lighten the image, "
      "higher values darken it. 0.5 = no change."
   ) );

   // Shadow clipping point
   AddParameter( AlgorithmParameter(
      "shadowsClip",
      "Shadows Clip",
      0.0,      // default
      0.0,      // min
      0.5,      // max
      6,        // precision
      "Clip shadows below this value to black."
   ) );

   // Highlights clipping point
   AddParameter( AlgorithmParameter(
      "highlightsClip",
      "Highlights Clip",
      1.0,      // default
      0.5,      // min
      1.0,      // max
      6,        // precision
      "Clip highlights above this value to white."
   ) );

   // Target median for auto-configuration
   AddParameter( AlgorithmParameter(
      "targetMedian",
      "Target Median",
      0.25,     // default - typical well-stretched astro image
      0.1,      // min
      0.5,      // max
      3,        // precision
      "Target median value for auto-stretch."
   ) );
}

// ----------------------------------------------------------------------------

String MTFStretch::Description() const
{
   return "The Midtones Transfer Function (MTF) is the classic PixInsight "
          "non-linear stretch. It remaps pixel values through a curve "
          "controlled by the midtones balance parameter. This is the same "
          "transformation used by ScreenTransferFunction (STF) and "
          "HistogramTransformation in PixInsight.";
}

// ----------------------------------------------------------------------------

double MTFStretch::Apply( double value ) const
{
   double shadows = ShadowsClip();
   double highlights = HighlightsClip();
   double midtones = Midtones();

   // Apply shadows/highlights clipping
   if ( value <= shadows )
      return 0.0;
   if ( value >= highlights )
      return 1.0;

   // Normalize to clipping range
   double x = (value - shadows) / (highlights - shadows);

   // Apply MTF
   return MTF( x, midtones );
}

// ----------------------------------------------------------------------------

double MTFStretch::MTF( double x, double m )
{
   // Edge cases
   if ( x <= 0.0 )
      return 0.0;
   if ( x >= 1.0 )
      return 1.0;
   if ( std::abs( m - 0.5 ) < 1e-10 )
      return x; // No change when m = 0.5

   double numerator = (m - 1.0) * x;
   double denominator = (2.0 * m - 1.0) * x - m;

   // Avoid division by zero
   if ( std::abs( denominator ) < 1e-15 )
      return x;

   return numerator / denominator;
}

// ----------------------------------------------------------------------------

double MTFStretch::CalculateMidtonesBalance( double currentMedian, double targetMedian )
{
   // Edge cases
   if ( currentMedian <= 0.0 || currentMedian >= 1.0 )
      return 0.5;
   if ( targetMedian <= 0.0 || targetMedian >= 1.0 )
      return 0.5;
   if ( std::abs( currentMedian - targetMedian ) < 1e-10 )
      return 0.5; // Already at target

   double numerator = currentMedian * (targetMedian - 1.0);
   double denominator = currentMedian * (2.0 * targetMedian - 1.0) - targetMedian;

   if ( std::abs( denominator ) < 1e-15 )
      return 0.5;

   double m = numerator / denominator;

   // Clamp to valid range
   return Clamp( m, 0.0001, 0.9999 );
}

// ----------------------------------------------------------------------------

void MTFStretch::AutoConfigure( double median, double mad )
{
   double targetMedian = TargetMedian();

   // Shadow clipping: clip the noise floor (matches PI's STF c0=2.8)
   double shadowClip = std::max( 0.0, median - 2.8 * mad );
   SetShadowsClip( shadowClip );
   SetHighlightsClip( 1.0 );

   // Normalize median to the shadow-clipped range — this is what
   // Apply() sees after clipping.  PI's STF computes m on this
   // normalized value, which is why it stretches so aggressively.
   double scale = 1.0 - shadowClip;
   double normalizedMedian = (scale > 1e-10) ? (median - shadowClip) / scale : median;

   // Clamp: if already bright, gentle stretch
   if ( normalizedMedian > 0.5 )
      targetMedian = std::max( targetMedian, normalizedMedian );

   double midtones = CalculateMidtonesBalance( normalizedMedian, targetMedian );
   SetMidtones( midtones );
}

// ----------------------------------------------------------------------------

std::unique_ptr<IStretchAlgorithm> MTFStretch::Clone() const
{
   auto clone = std::make_unique<MTFStretch>();

   for ( const AlgorithmParameter& param : m_parameters )
   {
      clone->SetParameter( param.id, param.value );
   }

   return clone;
}

// ----------------------------------------------------------------------------

} // namespace pcl
