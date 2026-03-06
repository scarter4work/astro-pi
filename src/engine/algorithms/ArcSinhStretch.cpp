//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX v3 - Statistical Stacking + Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "ArcSinhStretch.h"

#include <cmath>

namespace pcl
{

// ----------------------------------------------------------------------------

ArcSinhStretch::ArcSinhStretch()
{
   AddParameter( AlgorithmParameter(
      "beta", "Stretch Factor (Beta)", 10.0, 0.1, 1000.0, 1,
      "Controls the strength of the stretch. Higher values stretch faint "
      "details more aggressively while compressing bright regions."
   ) );

   AddParameter( AlgorithmParameter(
      "blackPoint", "Black Point", 0.0, 0.0, 0.5, 6,
      "Subtract this value before stretching (sets the black level)."
   ) );

   AddParameter( AlgorithmParameter(
      "stretch", "Stretch", 1.0, 0.1, 5.0, 2,
      "Overall stretch intensity multiplier applied after the asinh transform."
   ) );
}

// ----------------------------------------------------------------------------

String ArcSinhStretch::Description() const
{
   return "The Inverse Hyperbolic Sine (arcsinh) stretch provides a "
          "logarithmic-like transformation that handles high dynamic range "
          "data exceptionally well. Unlike logarithm, arcsinh is defined at "
          "zero and provides smooth, continuous stretching.";
}

// ----------------------------------------------------------------------------

void ArcSinhStretch::UpdateNormFactor() const
{
   double beta = Beta();
   if ( std::abs( beta - m_lastBeta ) > 1e-10 )
   {
      m_lastBeta = beta;
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

   if ( value <= blackPoint )
      return 0.0;

   double x = (value - blackPoint) / (1.0 - blackPoint);

   if ( x <= 0.0 )
      return 0.0;
   if ( x >= 1.0 && beta < 1.0 )
      return 1.0;

   UpdateNormFactor();

   double result = std::asinh( x * beta ) / m_normFactor;
   result *= stretch;

   return Clamp( result );
}

// ----------------------------------------------------------------------------

void ArcSinhStretch::AutoConfigure( double median, double mad )
{
   double brightness = median;
   double beta;

   if ( brightness > 0.5 )
      beta = 2.0 + 8.0 * (1.0 - brightness);
   else if ( brightness < 0.01 )
   {
      beta = 100.0 + 400.0 * (0.01 - brightness) / 0.01;
      beta = std::min( beta, 500.0 );
   }
   else
   {
      beta = 5.0 / brightness;
      beta = Clamp( beta, 5.0, 200.0 );
   }

   SetBeta( beta );

   double blackPoint = std::max( 0.0, median - 2.5 * mad );
   SetBlackPoint( blackPoint );

   SetStretch( 1.0 );
}

// ----------------------------------------------------------------------------

std::unique_ptr<IStretchAlgorithm> ArcSinhStretch::Clone() const
{
   auto clone = std::make_unique<ArcSinhStretch>();
   for ( const AlgorithmParameter& param : m_parameters )
      clone->SetParameter( param.id, param.value );
   return clone;
}

// ----------------------------------------------------------------------------

} // namespace pcl
