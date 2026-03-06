//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX v3 - Statistical Stacking + Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "SASStretch.h"

#include <cmath>

namespace pcl
{

// ----------------------------------------------------------------------------

SASStretch::SASStretch()
{
   AddParameter( AlgorithmParameter(
      "snrThreshold", "SNR Threshold", 5.0, 1.0, 20.0, 1,
      "SNR above which full stretch is applied."
   ) );

   AddParameter( AlgorithmParameter(
      "noiseFloor", "Noise Floor", 0.01, 0.0001, 0.1, 4,
      "Estimated noise level (standard deviation)."
   ) );

   AddParameter( AlgorithmParameter(
      "stretchStrength", "Stretch Strength", 3.0, 1.0, 10.0, 1,
      "Base stretch strength applied in high-SNR regions."
   ) );

   AddParameter( AlgorithmParameter(
      "iterations", "Iterations", 1.0, 1.0, 5.0, 0,
      "Number of stretch iterations."
   ) );

   AddParameter( AlgorithmParameter(
      "blackPoint", "Black Point", 0.0, 0.0, 0.5, 6,
      "Background level to clip to black."
   ) );

   AddParameter( AlgorithmParameter(
      "backgroundTarget", "Background Target", 0.1, 0.0, 0.3, 2,
      "Target brightness level for background after stretch."
   ) );
}

// ----------------------------------------------------------------------------

String SASStretch::Description() const
{
   return "Statistical Adaptive Stretch is a noise-aware algorithm that adapts "
          "its stretch intensity based on local signal-to-noise ratio. Ideal "
          "for images with varying SNR across different regions.";
}

// ----------------------------------------------------------------------------

double SASStretch::EstimateSNRWeight( double value ) const
{
   double signal = std::max( 0.0, value - m_noiseEstimate * 2.0 );
   double snr = signal / std::max( m_noiseEstimate, 1e-10 );
   double threshold = SNRThreshold();

   if ( snr >= threshold )
      return 1.0;

   double t = snr / threshold;
   return t * t * (3.0 - 2.0 * t); // Smoothstep
}

// ----------------------------------------------------------------------------

double SASStretch::StretchIteration( double x, double snrWeight ) const
{
   double strength = StretchStrength();

   double fullStretch = std::pow( x, 1.0 / strength );
   double gentleStretch = std::pow( x, 1.0 / (1.0 + (strength - 1.0) * 0.3) );

   return gentleStretch * (1.0 - snrWeight) + fullStretch * snrWeight;
}

// ----------------------------------------------------------------------------

double SASStretch::Apply( double value ) const
{
   double blackPoint = BlackPoint();
   double backgroundTarget = BackgroundTarget();
   int iterations = static_cast<int>( Iterations() );

   if ( value <= blackPoint )
      return 0.0;

   double x = (value - blackPoint) / (1.0 - blackPoint);

   if ( x <= 0.0 )
      return 0.0;
   if ( x >= 1.0 )
      return 1.0;

   double result = x;

   for ( int i = 0; i < iterations; ++i )
   {
      double snrWeight = EstimateSNRWeight( result );
      result = StretchIteration( result, snrWeight );
   }

   if ( result < 0.1 && x > m_noiseEstimate * 3.0 )
   {
      double minVisible = backgroundTarget + 0.05;
      if ( result < minVisible )
      {
         result = result * 0.5 + minVisible * 0.5;
      }
   }

   return Clamp( result );
}

// ----------------------------------------------------------------------------

void SASStretch::AutoConfigure( double median, double mad )
{
   m_noiseEstimate = mad;
   SetNoiseFloor( mad );

   m_signalEstimate = std::max( median, mad * 2.0 );

   double regionSNR = (median - mad * 2.0) / std::max( mad, 1e-10 );
   regionSNR = std::max( 0.0, regionSNR );

   double snrThreshold;
   if ( regionSNR > 10.0 )
      snrThreshold = 3.0;
   else if ( regionSNR > 5.0 )
      snrThreshold = 5.0;
   else
      snrThreshold = 8.0;
   SetSNRThreshold( snrThreshold );

   double bp = std::max( 0.0, median - 3.0 * mad );
   SetBlackPoint( bp );

   double strength;
   if ( median < 0.01 )
      strength = 5.0;
   else if ( median < 0.1 )
      strength = 3.0 + 2.0 * (0.1 - median) / 0.1;
   else
      strength = 2.0;
   SetStretchStrength( strength );

   double iterations = 1.0;
   if ( median < 0.01 && regionSNR < 5.0 )
      iterations = 3.0;
   else if ( median < 0.05 )
      iterations = 2.0;
   SetIterations( iterations );

   SetBackgroundTarget( 0.1 );
}

// ----------------------------------------------------------------------------

std::unique_ptr<IStretchAlgorithm> SASStretch::Clone() const
{
   auto clone = std::make_unique<SASStretch>();
   for ( const AlgorithmParameter& param : m_parameters )
      clone->SetParameter( param.id, param.value );
   clone->m_noiseEstimate = m_noiseEstimate;
   clone->m_signalEstimate = m_signalEstimate;
   return clone;
}

// ----------------------------------------------------------------------------

} // namespace pcl
