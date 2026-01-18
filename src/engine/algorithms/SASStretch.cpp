//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "SASStretch.h"

#include <cmath>

namespace pcl
{

// ----------------------------------------------------------------------------

SASStretch::SASStretch()
{
   // SNR threshold for full stretch
   AddParameter( AlgorithmParameter(
      "snrThreshold",
      "SNR Threshold",
      5.0,      // default
      1.0,      // min
      20.0,     // max
      1,        // precision
      "Signal-to-noise ratio above which full stretch is applied. "
      "Below this threshold, stretch is progressively reduced."
   ) );

   // Noise floor estimate
   AddParameter( AlgorithmParameter(
      "noiseFloor",
      "Noise Floor",
      0.01,     // default
      0.0001,   // min
      0.1,      // max
      4,        // precision
      "Estimated noise level (standard deviation). Used to calculate "
      "local SNR for adaptive stretching."
   ) );

   // Stretch strength
   AddParameter( AlgorithmParameter(
      "stretchStrength",
      "Stretch Strength",
      3.0,      // default
      1.0,      // min
      10.0,     // max
      1,        // precision
      "Base stretch strength applied in high-SNR regions."
   ) );

   // Number of iterations
   AddParameter( AlgorithmParameter(
      "iterations",
      "Iterations",
      1.0,      // default
      1.0,      // min
      5.0,      // max
      0,        // precision (integer)
      "Number of stretch iterations. Multiple gentle passes can be "
      "smoother than one aggressive pass."
   ) );

   // Black point
   AddParameter( AlgorithmParameter(
      "blackPoint",
      "Black Point",
      0.0,      // default
      0.0,      // min
      0.5,      // max
      6,        // precision
      "Background level to clip to black."
   ) );

   // Background target
   AddParameter( AlgorithmParameter(
      "backgroundTarget",
      "Background Target",
      0.1,      // default
      0.0,      // min
      0.3,      // max
      2,        // precision
      "Target brightness level for background after stretch."
   ) );
}

// ----------------------------------------------------------------------------

String SASStretch::Description() const
{
   return "Statistical Adaptive Stretch is a noise-aware algorithm that "
          "adapts its stretch intensity based on local signal-to-noise ratio. "
          "This prevents amplification of noise in faint regions while allowing "
          "aggressive stretching where the signal is strong. Ideal for images "
          "with varying SNR across different regions.";
}

// ----------------------------------------------------------------------------

double SASStretch::EstimateSNRWeight( double value ) const
{
   // Estimate SNR for this pixel value
   // SNR = (value - background) / noise

   double signal = std::max( 0.0, value - m_noiseEstimate * 2.0 );
   double snr = signal / std::max( m_noiseEstimate, 1e-10 );

   double threshold = SNRThreshold();

   // Convert SNR to weight (0-1)
   // Below threshold: weight increases linearly
   // Above threshold: weight is 1
   if ( snr >= threshold )
      return 1.0;

   // Smooth transition using sigmoid-like function
   double t = snr / threshold;
   return t * t * (3.0 - 2.0 * t); // Smoothstep
}

// ----------------------------------------------------------------------------

double SASStretch::StretchIteration( double x, double snrWeight ) const
{
   double strength = StretchStrength();

   // Base stretch using power function
   // Full stretch: x^(1/strength)
   // No stretch: x

   // Blend based on SNR weight
   double fullStretch = std::pow( x, 1.0 / strength );
   double gentleStretch = std::pow( x, 1.0 / (1.0 + (strength - 1.0) * 0.3) );

   // Interpolate based on SNR weight
   return gentleStretch * (1.0 - snrWeight) + fullStretch * snrWeight;
}

// ----------------------------------------------------------------------------

double SASStretch::Apply( double value ) const
{
   double blackPoint = BlackPoint();
   double backgroundTarget = BackgroundTarget();
   int iterations = static_cast<int>( Iterations() );

   // Apply black point
   if ( value <= blackPoint )
      return 0.0;

   double x = (value - blackPoint) / (1.0 - blackPoint);

   if ( x <= 0.0 )
      return 0.0;
   if ( x >= 1.0 )
      return 1.0;

   // Apply iterative stretch
   double result = x;

   for ( int i = 0; i < iterations; ++i )
   {
      // Calculate SNR weight for current value
      double snrWeight = EstimateSNRWeight( result );

      // Apply one iteration
      result = StretchIteration( result, snrWeight );
   }

   // Adjust to meet background target
   // If the low end is too bright/dark, apply subtle correction
   if ( result < 0.1 && x > m_noiseEstimate * 3.0 )
   {
      // This is signal, not noise - ensure it's visible
      double minVisible = backgroundTarget + 0.05;
      if ( result < minVisible )
      {
         result = result * 0.5 + minVisible * 0.5;
      }
   }

   return Clamp( result );
}

// ----------------------------------------------------------------------------

void SASStretch::AutoConfigure( const RegionStatistics& stats )
{
   // Estimate noise from MAD
   m_noiseEstimate = stats.mad;
   SetNoiseFloor( stats.mad );

   // Estimate signal level
   m_signalEstimate = std::max( stats.median, stats.mad * 2.0 );

   // Calculate SNR of the region
   double regionSNR = (stats.median - stats.mad * 2.0) / std::max( stats.mad, 1e-10 );
   regionSNR = std::max( 0.0, regionSNR );

   // Set SNR threshold based on region quality
   double snrThreshold;
   if ( regionSNR > 10.0 )
   {
      // Good SNR region - can use lower threshold
      snrThreshold = 3.0;
   }
   else if ( regionSNR > 5.0 )
   {
      snrThreshold = 5.0;
   }
   else
   {
      // Noisy region - use higher threshold
      snrThreshold = 8.0;
   }
   SetSNRThreshold( snrThreshold );

   // Set black point
   double bp = std::max( 0.0, stats.median - 3.0 * stats.mad );
   SetBlackPoint( bp );

   // Calculate stretch strength based on brightness
   double strength;
   if ( stats.median < 0.01 )
   {
      strength = 5.0;
   }
   else if ( stats.median < 0.1 )
   {
      strength = 3.0 + 2.0 * (0.1 - stats.median) / 0.1;
   }
   else
   {
      strength = 2.0;
   }
   SetStretchStrength( strength );

   // Set iterations based on how aggressive we need to be
   double iterations = 1.0;
   if ( stats.median < 0.01 && regionSNR < 5.0 )
   {
      // Very faint and noisy - use multiple gentle passes
      iterations = 3.0;
   }
   else if ( stats.median < 0.05 )
   {
      iterations = 2.0;
   }
   SetIterations( iterations );

   // Background target
   SetBackgroundTarget( 0.1 );
}

// ----------------------------------------------------------------------------

std::unique_ptr<IStretchAlgorithm> SASStretch::Clone() const
{
   auto clone = std::make_unique<SASStretch>();

   for ( const AlgorithmParameter& param : m_parameters )
   {
      clone->SetParameter( param.id, param.value );
   }

   clone->m_noiseEstimate = m_noiseEstimate;
   clone->m_signalEstimate = m_signalEstimate;

   return clone;
}

// ----------------------------------------------------------------------------

} // namespace pcl
