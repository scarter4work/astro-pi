//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX v3 - Statistical Stacking + Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "LumptonStretch.h"

#include <cmath>

namespace pcl
{

// ----------------------------------------------------------------------------

LumptonStretch::LumptonStretch()
{
   AddParameter( AlgorithmParameter(
      "Q", "Q (Softening)", 8.0, 0.1, 100.0, 1,
      "Softening parameter. Higher values = more aggressive non-linear stretch."
   ) );

   AddParameter( AlgorithmParameter(
      "minimum", "Minimum (Alpha)", 0.05, 0.001, 1.0, 4,
      "Noise floor / scaling parameter."
   ) );

   AddParameter( AlgorithmParameter(
      "blackPoint", "Black Point", 0.0, 0.0, 0.5, 6,
      "Subtract this value before stretching."
   ) );

   AddParameter( AlgorithmParameter(
      "stretch", "Stretch", 1.0, 0.1, 5.0, 2,
      "Final stretch multiplier."
   ) );
}

// ----------------------------------------------------------------------------

String LumptonStretch::Description() const
{
   return "The Lumpton stretch is based on the SDSS (Sloan Digital Sky Survey) "
          "method. It uses an inverse hyperbolic sine transformation that "
          "transitions smoothly from linear to logarithmic compression. "
          "Excellent for HDR astronomical images.";
}

// ----------------------------------------------------------------------------

double LumptonStretch::Apply( double value ) const
{
   double blackPoint = BlackPoint();
   double q = Q();
   double minimum = Minimum();
   double stretch = Stretch();

   if ( value <= blackPoint )
      return 0.0;

   double x = (value - blackPoint) / (1.0 - blackPoint);

   if ( x <= 0.0 )
      return 0.0;

   double scaledX = x / minimum;
   double result = std::asinh( q * scaledX );

   double maxVal = std::asinh( q / minimum );
   if ( maxVal > 1e-10 )
      result /= maxVal;

   result *= stretch;

   return Clamp( result );
}

// ----------------------------------------------------------------------------

void LumptonStretch::AutoConfigure( double median, double mad )
{
   double minimum = mad * 2.0;
   minimum = Clamp( minimum, 0.001, 0.5 );
   SetMinimum( minimum );

   double q;
   if ( median < 0.001 )
      q = 50.0;
   else if ( median < 0.01 )
      q = 20.0 + 30.0 * (0.01 - median) / 0.01;
   else if ( median < 0.1 )
      q = 5.0 + 15.0 * (0.1 - median) / 0.1;
   else
      q = 2.0 + 3.0 * (1.0 - median);

   SetQ( q );

   double bp = std::max( 0.0, median - 3.0 * mad );
   SetBlackPoint( bp );

   SetStretch( 1.0 );
}

// ----------------------------------------------------------------------------

std::unique_ptr<IStretchAlgorithm> LumptonStretch::Clone() const
{
   auto clone = std::make_unique<LumptonStretch>();
   for ( const AlgorithmParameter& param : m_parameters )
      clone->SetParameter( param.id, param.value );
   return clone;
}

// ----------------------------------------------------------------------------

} // namespace pcl
