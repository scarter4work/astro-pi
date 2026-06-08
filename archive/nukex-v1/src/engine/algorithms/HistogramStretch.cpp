//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "HistogramStretch.h"

#include <cmath>

namespace pcl
{

// ----------------------------------------------------------------------------

HistogramStretch::HistogramStretch()
{
   // Shadows clipping point
   AddParameter( AlgorithmParameter(
      "shadowsClip",
      "Shadows",
      0.0,      // default
      0.0,      // min
      1.0,      // max
      6,        // precision
      "Input values below this point are clipped to black."
   ) );

   // Midtones balance
   AddParameter( AlgorithmParameter(
      "midtones",
      "Midtones",
      0.5,      // default
      0.0001,   // min
      0.9999,   // max
      4,        // precision
      "Midtones balance. Values < 0.5 brighten, values > 0.5 darken."
   ) );

   // Highlights clipping point
   AddParameter( AlgorithmParameter(
      "highlightsClip",
      "Highlights",
      1.0,      // default
      0.0,      // min
      1.0,      // max
      6,        // precision
      "Input values above this point are clipped to white."
   ) );

   // Low output range
   AddParameter( AlgorithmParameter(
      "lowOutput",
      "Low Output",
      0.0,      // default
      0.0,      // min
      1.0,      // max
      4,        // precision
      "Output range lower bound."
   ) );

   // High output range
   AddParameter( AlgorithmParameter(
      "highOutput",
      "High Output",
      1.0,      // default
      0.0,      // min
      1.0,      // max
      4,        // precision
      "Output range upper bound."
   ) );
}

// ----------------------------------------------------------------------------

String HistogramStretch::Description() const
{
   return "Classic histogram transformation with full control over shadows "
          "clipping, highlights clipping, midtones adjustment, and output "
          "range expansion. This is equivalent to PixInsight's "
          "HistogramTransformation process.";
}

// ----------------------------------------------------------------------------

double HistogramStretch::MTF( double x, double m )
{
   if ( x <= 0.0 )
      return 0.0;
   if ( x >= 1.0 )
      return 1.0;
   if ( std::abs( m - 0.5 ) < 1e-10 )
      return x;

   return (m - 1.0) * x / ((2.0 * m - 1.0) * x - m);
}

// ----------------------------------------------------------------------------

double HistogramStretch::Apply( double value ) const
{
   double shadows = ShadowsClip();
   double highlights = HighlightsClip();
   double midtones = Midtones();
   double lowOut = LowOutput();
   double highOut = HighOutput();

   // Step 1: Apply input clipping
   if ( value <= shadows )
      return lowOut;
   if ( value >= highlights )
      return highOut;

   // Step 2: Normalize to clipped input range
   double range = highlights - shadows;
   if ( range <= 0 )
      return lowOut;

   double x = (value - shadows) / range;

   // Step 3: Apply MTF
   double stretched = MTF( x, midtones );

   // Step 4: Expand to output range
   double outRange = highOut - lowOut;
   double result = lowOut + stretched * outRange;

   return Clamp( result );
}

// ----------------------------------------------------------------------------

void HistogramStretch::AutoConfigure( const RegionStatistics& stats )
{
   // Calculate shadows clip based on background
   // Use median - k*MAD as black point
   double shadowsClip = stats.median - 2.8 * stats.mad;
   shadowsClip = std::max( 0.0, shadowsClip );
   shadowsClip = std::min( shadowsClip, stats.median * 0.9 );
   SetShadowsClip( shadowsClip );

   // Highlights clip - usually keep at 1.0 unless there's significant clipping
   double highlightsClip = 1.0;
   if ( stats.clippingPct > 1.0 )
   {
      // Some clipping exists, be more conservative
      highlightsClip = stats.max;
   }
   SetHighlightsClip( highlightsClip );

   // Calculate midtones to achieve target brightness
   // After clipping, what's the effective median?
   double effectiveMedian = (stats.median - shadowsClip) / (highlightsClip - shadowsClip);
   effectiveMedian = Clamp( effectiveMedian, 0.0001, 0.9999 );

   // Target: bring median to ~0.25 for typical astro stretch
   double targetMedian = 0.25;

   // Solve for midtones: MTF(effectiveMedian, m) = targetMedian
   // Using the inverse of the MTF relationship
   double numerator = effectiveMedian * (targetMedian - 1.0);
   double denominator = effectiveMedian * (2.0 * targetMedian - 1.0) - targetMedian;

   double midtones = 0.5;
   if ( std::abs( denominator ) > 1e-10 )
   {
      midtones = numerator / denominator;
      midtones = Clamp( midtones, 0.0001, 0.9999 );
   }
   SetMidtones( midtones );

   // Keep default output range
   SetLowOutput( 0.0 );
   SetHighOutput( 1.0 );
}

// ----------------------------------------------------------------------------

std::unique_ptr<IStretchAlgorithm> HistogramStretch::Clone() const
{
   auto clone = std::make_unique<HistogramStretch>();

   for ( const AlgorithmParameter& param : m_parameters )
   {
      clone->SetParameter( param.id, param.value );
   }

   return clone;
}

// ----------------------------------------------------------------------------

} // namespace pcl
