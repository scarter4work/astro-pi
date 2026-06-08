//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "MTFStretch.h"

#include <cmath>

namespace pcl
{

// ----------------------------------------------------------------------------

MTFStretch::MTFStretch()
{
   // Midtones balance - the core parameter
   // Values < 0.5 lighten, values > 0.5 darken
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

   // Midtones Transfer Function formula:
   // MTF(x, m) = (m - 1) * x / ((2*m - 1) * x - m)
   //
   // This formula maps:
   //   - x=0 -> 0
   //   - x=1 -> 1
   //   - x=m -> 0.5 (the midtones balance point maps to middle gray)

   double numerator = (m - 1.0) * x;
   double denominator = (2.0 * m - 1.0) * x - m;

   // Avoid division by zero (shouldn't happen with valid inputs)
   if ( std::abs( denominator ) < 1e-15 )
      return x;

   return numerator / denominator;
}

// ----------------------------------------------------------------------------

double MTFStretch::CalculateMidtonesBalance( double currentMedian, double targetMedian )
{
   // We want to find m such that MTF(currentMedian, m) = targetMedian
   //
   // From the MTF formula:
   // targetMedian = (m - 1) * currentMedian / ((2*m - 1) * currentMedian - m)
   //
   // Solving for m:
   // targetMedian * ((2*m - 1) * currentMedian - m) = (m - 1) * currentMedian
   // targetMedian * (2*m*currentMedian - currentMedian - m) = m*currentMedian - currentMedian
   // 2*targetMedian*m*currentMedian - targetMedian*currentMedian - targetMedian*m = m*currentMedian - currentMedian
   // m * (2*targetMedian*currentMedian - targetMedian - currentMedian) = targetMedian*currentMedian - currentMedian
   // m = (targetMedian*currentMedian - currentMedian) / (2*targetMedian*currentMedian - targetMedian - currentMedian)
   // m = currentMedian * (targetMedian - 1) / (currentMedian * (2*targetMedian - 1) - targetMedian)

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

void MTFStretch::AutoConfigure( const RegionStatistics& stats )
{
   // Calculate midtones balance to bring median to target
   double targetMedian = TargetMedian();
   double currentMedian = stats.median;

   // If image is already stretched (median > 0.1), don't stretch as hard
   if ( currentMedian > 0.1 )
   {
      targetMedian = std::min( targetMedian, currentMedian * 1.5 );
   }

   double midtones = CalculateMidtonesBalance( currentMedian, targetMedian );
   SetMidtones( midtones );

   // Auto-set shadow clipping based on noise level
   // Use a fraction of MAD below the median as black point
   double shadowClip = std::max( 0.0, stats.median - 3.0 * stats.mad );
   SetShadowsClip( shadowClip );

   // Keep highlights at 1.0 for now (no clipping)
   SetHighlightsClip( 1.0 );
}

// ----------------------------------------------------------------------------

std::unique_ptr<IStretchAlgorithm> MTFStretch::Clone() const
{
   auto clone = std::make_unique<MTFStretch>();

   // Copy all parameter values
   for ( const AlgorithmParameter& param : m_parameters )
   {
      clone->SetParameter( param.id, param.value );
   }

   return clone;
}

// ----------------------------------------------------------------------------

} // namespace pcl
