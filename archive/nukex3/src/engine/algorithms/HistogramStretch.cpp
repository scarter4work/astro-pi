//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX v3 - Statistical Stacking + Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "HistogramStretch.h"

#include <cmath>

namespace pcl
{

// ----------------------------------------------------------------------------

HistogramStretch::HistogramStretch()
{
   AddParameter( AlgorithmParameter(
      "shadowsClip", "Shadows", 0.0, 0.0, 1.0, 6,
      "Input values below this point are clipped to black."
   ) );

   AddParameter( AlgorithmParameter(
      "midtones", "Midtones", 0.5, 0.0001, 0.9999, 4,
      "Midtones balance. Values < 0.5 brighten, values > 0.5 darken."
   ) );

   AddParameter( AlgorithmParameter(
      "highlightsClip", "Highlights", 1.0, 0.0, 1.0, 6,
      "Input values above this point are clipped to white."
   ) );

   AddParameter( AlgorithmParameter(
      "lowOutput", "Low Output", 0.0, 0.0, 1.0, 4,
      "Output range lower bound."
   ) );

   AddParameter( AlgorithmParameter(
      "highOutput", "High Output", 1.0, 0.0, 1.0, 4,
      "Output range upper bound."
   ) );
}

// ----------------------------------------------------------------------------

String HistogramStretch::Description() const
{
   return "Classic histogram transformation with full control over shadows "
          "clipping, highlights clipping, midtones adjustment, and output "
          "range expansion. Equivalent to PixInsight's HistogramTransformation.";
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

   if ( value <= shadows )
      return lowOut;
   if ( value >= highlights )
      return highOut;

   double range = highlights - shadows;
   if ( range <= 0 )
      return lowOut;

   double x = (value - shadows) / range;
   double stretched = MTF( x, midtones );
   double outRange = highOut - lowOut;
   double result = lowOut + stretched * outRange;

   return Clamp( result );
}

// ----------------------------------------------------------------------------

void HistogramStretch::AutoConfigure( double median, double mad )
{
   double shadowsClip = median - 2.8 * mad;
   shadowsClip = std::max( 0.0, shadowsClip );
   shadowsClip = std::min( shadowsClip, median * 0.9 );
   SetShadowsClip( shadowsClip );

   double highlightsClip = 1.0;
   SetHighlightsClip( highlightsClip );

   double effectiveMedian = (median - shadowsClip) / (highlightsClip - shadowsClip);
   effectiveMedian = Clamp( effectiveMedian, 0.0001, 0.9999 );

   double targetMedian = 0.25;

   double numerator = effectiveMedian * (targetMedian - 1.0);
   double denominator = effectiveMedian * (2.0 * targetMedian - 1.0) - targetMedian;

   double midtones = 0.5;
   if ( std::abs( denominator ) > 1e-10 )
   {
      midtones = numerator / denominator;
      midtones = Clamp( midtones, 0.0001, 0.9999 );
   }
   SetMidtones( midtones );

   SetLowOutput( 0.0 );
   SetHighOutput( 1.0 );
}

// ----------------------------------------------------------------------------

std::unique_ptr<IStretchAlgorithm> HistogramStretch::Clone() const
{
   auto clone = std::make_unique<HistogramStretch>();
   for ( const AlgorithmParameter& param : m_parameters )
      clone->SetParameter( param.id, param.value );
   return clone;
}

// ----------------------------------------------------------------------------

} // namespace pcl
