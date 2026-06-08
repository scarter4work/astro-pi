//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "LumptonStretch.h"

#include <cmath>

namespace pcl
{

// ----------------------------------------------------------------------------

LumptonStretch::LumptonStretch()
{
   // Q - Softening parameter
   // Controls the transition from linear (low values) to asinh-like (high values)
   AddParameter( AlgorithmParameter(
      "Q",
      "Q (Softening)",
      8.0,      // default
      0.1,      // min
      100.0,    // max
      1,        // precision
      "Softening parameter. Higher values = more aggressive non-linear stretch. "
      "Lower values = more linear behavior for faint pixels."
   ) );

   // Minimum - noise floor scaling
   AddParameter( AlgorithmParameter(
      "minimum",
      "Minimum (Alpha)",
      0.05,     // default
      0.001,    // min
      1.0,      // max
      4,        // precision
      "Noise floor / scaling parameter. Sets the intensity level below which "
      "the stretch becomes approximately linear."
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

   // Overall stretch multiplier
   AddParameter( AlgorithmParameter(
      "stretch",
      "Stretch",
      1.0,      // default
      0.1,      // min
      5.0,      // max
      2,        // precision
      "Final stretch multiplier."
   ) );
}

// ----------------------------------------------------------------------------

String LumptonStretch::Description() const
{
   return "The Lumpton stretch is based on the method developed for SDSS "
          "(Sloan Digital Sky Survey) image processing. It uses an inverse "
          "hyperbolic sine transformation that transitions smoothly from "
          "linear behavior at low intensities to logarithmic compression "
          "at high intensities. This makes it excellent for high dynamic "
          "range astronomical images, particularly galaxy halos and faint "
          "extended emission.";
}

// ----------------------------------------------------------------------------

void LumptonStretch::UpdateNormFactor() const
{
   double q = Q();
   if ( std::abs( q - m_lastQ ) > 1e-10 )
   {
      m_lastQ = q;
      // Normalization to ensure output reaches 1.0 for input 1.0
      m_normFactor = std::asinh( q );
      if ( m_normFactor < 1e-10 )
         m_normFactor = 1.0;
   }
}

// ----------------------------------------------------------------------------

double LumptonStretch::Apply( double value ) const
{
   double blackPoint = BlackPoint();
   double q = Q();
   double minimum = Minimum();
   double stretch = Stretch();

   // Apply black point
   if ( value <= blackPoint )
      return 0.0;

   double x = (value - blackPoint) / (1.0 - blackPoint);

   if ( x <= 0.0 )
      return 0.0;

   // Update normalization
   UpdateNormFactor();

   // Lumpton formula: asinh(Q * x / minimum) / asinh(Q / minimum)
   // Simplified for normalized 0-1 input:
   // stretched = asinh(Q * x) / asinh(Q)

   // Scale by minimum parameter
   double scaledX = x / minimum;

   // Apply asinh stretch
   double result = std::asinh( q * scaledX );

   // Normalize by the maximum possible value (when x = 1/minimum)
   double maxVal = std::asinh( q / minimum );
   if ( maxVal > 1e-10 )
      result /= maxVal;

   // Apply stretch multiplier
   result *= stretch;

   return Clamp( result );
}

// ----------------------------------------------------------------------------

void LumptonStretch::AutoConfigure( const RegionStatistics& stats )
{
   // Set minimum based on noise level (MAD)
   double minimum = stats.mad * 2.0;
   minimum = Clamp( minimum, 0.001, 0.5 );
   SetMinimum( minimum );

   // Calculate Q based on image brightness and dynamic range
   double q;

   if ( stats.median < 0.001 )
   {
      // Very faint - high Q for aggressive stretch
      q = 50.0;
   }
   else if ( stats.median < 0.01 )
   {
      // Faint
      q = 20.0 + 30.0 * (0.01 - stats.median) / 0.01;
   }
   else if ( stats.median < 0.1 )
   {
      // Moderate
      q = 5.0 + 15.0 * (0.1 - stats.median) / 0.1;
   }
   else
   {
      // Bright
      q = 2.0 + 3.0 * (1.0 - stats.median);
   }

   SetQ( q );

   // Set black point
   double bp = std::max( 0.0, stats.median - 3.0 * stats.mad );
   SetBlackPoint( bp );

   // Default stretch
   SetStretch( 1.0 );
}

// ----------------------------------------------------------------------------

std::unique_ptr<IStretchAlgorithm> LumptonStretch::Clone() const
{
   auto clone = std::make_unique<LumptonStretch>();

   for ( const AlgorithmParameter& param : m_parameters )
   {
      clone->SetParameter( param.id, param.value );
   }

   return clone;
}

// ----------------------------------------------------------------------------

} // namespace pcl
