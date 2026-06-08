//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "ArcSinhStretch.h"

#include <cmath>

namespace pcl
{

// ----------------------------------------------------------------------------

ArcSinhStretch::ArcSinhStretch()
{
   // Beta (stretch factor)
   // Higher values = more aggressive stretch of faint details
   // Lower values = more linear, protects highlights better
   AddParameter( AlgorithmParameter(
      "beta",
      "Stretch Factor (Beta)",
      10.0,     // default
      0.1,      // min
      1000.0,   // max
      1,        // precision
      "Controls the strength of the stretch. Higher values stretch faint "
      "details more aggressively while compressing bright regions."
   ) );

   // Black point
   AddParameter( AlgorithmParameter(
      "blackPoint",
      "Black Point",
      0.0,      // default
      0.0,      // min
      0.5,      // max
      6,        // precision
      "Subtract this value before stretching (sets the black level)."
   ) );

   // Overall stretch multiplier
   AddParameter( AlgorithmParameter(
      "stretch",
      "Stretch",
      1.0,      // default
      0.1,      // min
      5.0,      // max
      2,        // precision
      "Overall stretch intensity multiplier applied after the asinh transform."
   ) );
}

// ----------------------------------------------------------------------------

String ArcSinhStretch::Description() const
{
   return "The Inverse Hyperbolic Sine (arcsinh) stretch provides a "
          "logarithmic-like transformation that handles high dynamic range "
          "data exceptionally well. Unlike logarithm, arcsinh is defined at "
          "zero and provides smooth, continuous stretching. It's particularly "
          "effective for protecting bright star cores and galaxy nuclei from "
          "blowing out while still revealing faint detail.";
}

// ----------------------------------------------------------------------------

void ArcSinhStretch::UpdateNormFactor() const
{
   double beta = Beta();
   if ( std::abs( beta - m_lastBeta ) > 1e-10 )
   {
      m_lastBeta = beta;
      // Normalization factor ensures output is 0-1 for input 0-1
      m_normFactor = std::asinh( beta );
      if ( m_normFactor < 1e-10 )
         m_normFactor = 1.0;
   }
}

// ----------------------------------------------------------------------------

double ArcSinhStretch::Apply( double value ) const
{
   double blackPoint = BlackPoint();
   double beta = Beta();
   double stretch = Stretch();

   // Apply black point
   if ( value <= blackPoint )
      return 0.0;

   double x = (value - blackPoint) / (1.0 - blackPoint);

   // Edge cases
   if ( x <= 0.0 )
      return 0.0;
   if ( x >= 1.0 && beta < 1.0 )
      return 1.0;

   // Update normalization factor if needed
   UpdateNormFactor();

   // Apply arcsinh stretch
   // Formula: asinh(x * beta) / asinh(beta)
   double result = std::asinh( x * beta ) / m_normFactor;

   // Apply stretch multiplier
   result *= stretch;

   return Clamp( result );
}

// ----------------------------------------------------------------------------

void ArcSinhStretch::AutoConfigure( const RegionStatistics& stats )
{
   // For star cores and bright regions:
   // - Use lower beta to protect highlights
   // - For faint regions, use higher beta

   double dynamicRange = stats.max - stats.min;
   double brightness = stats.median;

   // Calculate beta based on image characteristics
   double beta;

   if ( brightness > 0.5 )
   {
      // Bright region (like star core) - use gentler stretch
      beta = 2.0 + 8.0 * (1.0 - brightness);
   }
   else if ( brightness < 0.01 )
   {
      // Very faint - use aggressive stretch
      beta = 100.0 + 400.0 * (0.01 - brightness) / 0.01;
      beta = std::min( beta, 500.0 );
   }
   else
   {
      // Normal range - scale beta with inverse of brightness
      beta = 5.0 / brightness;
      beta = Clamp( beta, 5.0, 200.0 );
   }

   SetBeta( beta );

   // Set black point based on background level
   double blackPoint = std::max( 0.0, stats.median - 2.5 * stats.mad );
   SetBlackPoint( blackPoint );

   // Adjust stretch based on SNR
   double stretch = 1.0;
   if ( stats.snrEstimate < 5.0 )
   {
      // Low SNR - reduce stretch to avoid amplifying noise
      stretch = 0.7 + 0.3 * (stats.snrEstimate / 5.0);
   }
   SetStretch( stretch );
}

// ----------------------------------------------------------------------------

std::unique_ptr<IStretchAlgorithm> ArcSinhStretch::Clone() const
{
   auto clone = std::make_unique<ArcSinhStretch>();

   for ( const AlgorithmParameter& param : m_parameters )
   {
      clone->SetParameter( param.id, param.value );
   }

   return clone;
}

// ----------------------------------------------------------------------------

} // namespace pcl
