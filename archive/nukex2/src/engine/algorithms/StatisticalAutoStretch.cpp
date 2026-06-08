//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// Statistical Auto-Stretch Implementation

#include "StatisticalAutoStretch.h"

#include <pcl/Math.h>
#include <algorithm>
#include <cmath>

namespace pcl
{

// ----------------------------------------------------------------------------

StatisticalAutoStretch::StatisticalAutoStretch()
{
   // Target background level after stretch
   AddParameter( AlgorithmParameter(
      "targetBackground",
      "Target Background",
      0.15,        // default
      0.05,        // min
      0.35,        // max
      3,           // precision
      "Target background level after stretch (0.15 is typical for PixInsight STF)"
   ) );

   // Sigma clipping factor for black point
   AddParameter( AlgorithmParameter(
      "sigmaClip",
      "Sigma Clip",
      2.8,         // default (matches PI)
      1.0,         // min
      5.0,         // max
      2,           // precision
      "Number of sigma below median to set black point"
   ) );

   // Manual midtone override (0 = auto)
   AddParameter( AlgorithmParameter(
      "midtoneOverride",
      "Midtone Override",
      0.0,         // default (0 = auto)
      0.0,         // min
      0.99,        // max
      3,           // precision
      "Manual midtone value (0 = automatic calculation from statistics)"
   ) );

   // Highlight protection percentile
   AddParameter( AlgorithmParameter(
      "highlightProtection",
      "Highlight Protection",
      99.9,        // default
      95.0,        // min
      100.0,       // max
      2,           // precision
      "Percentile to use for white point (protects bright stars)"
   ) );

   // Minimum midtone (prevents over-aggressive stretch)
   AddParameter( AlgorithmParameter(
      "minMidtone",
      "Min Midtone",
      0.02,        // default
      0.001,       // min
      0.5,         // max
      4,           // precision
      "Minimum midtone value (prevents extreme stretch)"
   ) );

   // Maximum midtone (prevents too-mild stretch)
   AddParameter( AlgorithmParameter(
      "maxMidtone",
      "Max Midtone",
      0.5,         // default
      0.1,         // min
      0.99,        // max
      3,           // precision
      "Maximum midtone value (ensures some stretch is applied)"
   ) );
}

// ----------------------------------------------------------------------------

String StatisticalAutoStretch::Description() const
{
   return "Statistical Auto-Stretch automatically determines optimal stretch parameters "
          "from image statistics. It analyzes signal compression (how tightly packed "
          "the signal is above background) and selects an appropriate midtone balance. "
          "Designed for linear astronomical data where most signal is compressed into "
          "a tiny range. No ML models required - pure statistical analysis.";
}

// ----------------------------------------------------------------------------

double StatisticalAutoStretch::MTF( double x, double midtone )
{
   // Midtones Transfer Function
   // MTF(x, m) = (m - 1) * x / ((2*m - 1) * x - m)
   //
   // Clamp midtone to avoid division by zero
   double m = Clamp( midtone, 0.0001, 0.9999 );

   if ( x <= 0.0 )
      return 0.0;
   if ( x >= 1.0 )
      return 1.0;

   double denominator = (2.0 * m - 1.0) * x - m;

   // Avoid division by zero
   if ( Abs( denominator ) < 1e-10 )
      return x;

   return Clamp( (m - 1.0) * x / denominator, 0.0, 1.0 );
}

// ----------------------------------------------------------------------------

double StatisticalAutoStretch::CompressionToMidtone( double compression, double minMid, double maxMid )
{
   // Map signal compression ratio to midtone value
   // Lower compression = more aggressive stretch needed = lower midtone
   //
   // compression < 0.1  → very compressed   → midtone ~ 0.02-0.05
   // compression ~ 0.3  → moderately compressed → midtone ~ 0.10-0.15
   // compression > 0.5  → spread out → midtone ~ 0.25-0.50
   //
   // Using a logarithmic mapping for smooth transition

   compression = Clamp( compression, 0.001, 1.0 );

   // Logarithmic mapping: midtone = minMid * (maxMid/minMid)^compression
   // This gives minMid when compression→0, maxMid when compression→1

   double logMin = std::log( minMid );
   double logMax = std::log( maxMid );

   double logMidtone = logMin + compression * (logMax - logMin);

   return Clamp( std::exp( logMidtone ), minMid, maxMid );
}

// ----------------------------------------------------------------------------

void StatisticalAutoStretch::AutoConfigure( const RegionStatistics& stats )
{
   double sigmaClip = SigmaClip();
   // double targetBg = TargetBackground();  // Reserved for future use
   double highlightPct = HighlightProtection();
   double midtoneOverride = MidtoneOverride();
   double minMid = MinMidtone();
   double maxMid = MaxMidtone();

   // Calculate robust sigma from MAD
   double sigma = MADToSigma( stats.mad );

   // Black point: background - sigma clip * sigma
   // Use the mode (histogram peak) as background estimate if available,
   // otherwise use median
   double background = (stats.mode > 0) ? stats.mode : stats.median;
   m_blackPoint = Max( 0.0, background - sigmaClip * sigma );

   // White point: use specified percentile for highlight protection
   // We need to interpolate based on highlightPct
   if ( highlightPct >= 99.9 )
      m_whitePoint = stats.p99;  // Use p99 as proxy for p99.9
   else if ( highlightPct >= 99.0 )
      m_whitePoint = stats.p99;
   else if ( highlightPct >= 95.0 )
      m_whitePoint = stats.p95;
   else
      m_whitePoint = stats.p90;

   // Ensure white point is above black point
   if ( m_whitePoint <= m_blackPoint )
      m_whitePoint = stats.max;

   // Calculate signal compression ratio
   // How much of the dynamic range between p90 and p99 is used for signal?
   double signalRange = stats.p90 - background;
   double dynamicRange = stats.p99 - background;

   if ( dynamicRange > 1e-10 )
      m_signalCompression = signalRange / dynamicRange;
   else
      m_signalCompression = 0.5; // Default if can't compute

   // Calculate midtone from compression (or use override)
   if ( midtoneOverride > 0.001 )
   {
      m_midtone = Clamp( midtoneOverride, minMid, maxMid );
   }
   else
   {
      m_midtone = CompressionToMidtone( m_signalCompression, minMid, maxMid );
   }

   m_configured = true;
}

// ----------------------------------------------------------------------------

void StatisticalAutoStretch::ComputeStatistics( const Image& image ) const
{
   // Compute statistics directly from image if not configured
   // This allows Apply to work even without explicit AutoConfigure call

   if ( m_configured )
      return;

   // Sample image to compute statistics quickly
   const size_t maxSamples = 1000000;
   size_t totalPixels = image.NumberOfPixels();
   size_t stride = Max( size_t( 1 ), totalPixels / maxSamples );

   std::vector<double> samples;
   samples.reserve( Min( maxSamples, totalPixels ) );

   // Sample from first channel (luminance or red)
   // Note: PCL Image uses float samples, not double
   const Image::sample* data = image.PixelData( 0 );
   for ( size_t i = 0; i < totalPixels; i += stride )
   {
      samples.push_back( static_cast<double>( data[i] ) );
   }

   // Sort for percentile calculation
   std::sort( samples.begin(), samples.end() );
   size_t n = samples.size();

   if ( n == 0 )
      return;

   // Calculate statistics
   double median = samples[n / 2];
   double p90 = samples[size_t( n * 0.90 )];
   double p99 = samples[size_t( n * 0.99 )];
   double p999 = samples[Min( size_t( n * 0.999 ), n - 1 )];

   // Calculate MAD
   std::vector<double> deviations( n );
   for ( size_t i = 0; i < n; ++i )
      deviations[i] = Abs( samples[i] - median );
   std::sort( deviations.begin(), deviations.end() );
   double mad = deviations[n / 2];
   double sigma = MADToSigma( mad );

   // Set stretch parameters
   double sigmaClip = SigmaClip();
   m_blackPoint = Max( 0.0, median - sigmaClip * sigma );
   m_whitePoint = p999;

   // Calculate compression
   double signalRange = p90 - median;
   double dynamicRange = p99 - median;
   m_signalCompression = (dynamicRange > 1e-10) ? (signalRange / dynamicRange) : 0.5;

   // Calculate midtone
   double midtoneOverride = MidtoneOverride();
   if ( midtoneOverride > 0.001 )
      m_midtone = midtoneOverride;
   else
      m_midtone = CompressionToMidtone( m_signalCompression, MinMidtone(), MaxMidtone() );

   m_configured = true;
}

// ----------------------------------------------------------------------------

double StatisticalAutoStretch::Apply( double value ) const
{
   // Normalize to [0, 1] using computed black/white points
   double range = m_whitePoint - m_blackPoint;
   if ( range <= 0 )
      return value;

   double normalized = (value - m_blackPoint) / range;
   normalized = Clamp( normalized, 0.0, 1.0 );

   // Apply MTF stretch
   return MTF( normalized, m_midtone );
}

// ----------------------------------------------------------------------------

void StatisticalAutoStretch::ApplyToImage( Image& image, const Image* mask ) const
{
   // Compute statistics if not already configured
   ComputeStatistics( image );

   // Apply to each channel
   // Note: PCL Image uses float samples, not double
   for ( int c = 0; c < image.NumberOfChannels(); ++c )
   {
      Image::sample* data = image.PixelData( c );
      size_t n = image.NumberOfPixels();

      if ( mask != nullptr && mask->NumberOfChannels() > 0 )
      {
         const Image::sample* maskData = mask->PixelData( 0 );
         for ( size_t i = 0; i < n; ++i )
         {
            double m = static_cast<double>( maskData[i] );
            if ( m > 0 )
            {
               double original = static_cast<double>( data[i] );
               double stretched = Apply( original );
               data[i] = static_cast<Image::sample>( original * (1.0 - m) + stretched * m );
            }
         }
      }
      else
      {
         for ( size_t i = 0; i < n; ++i )
         {
            data[i] = static_cast<Image::sample>( Apply( static_cast<double>( data[i] ) ) );
         }
      }
   }
}

// ----------------------------------------------------------------------------

std::unique_ptr<IStretchAlgorithm> StatisticalAutoStretch::Clone() const
{
   auto clone = std::make_unique<StatisticalAutoStretch>();

   // Copy parameters
   for ( const auto& param : m_parameters )
   {
      clone->SetParameter( param.id, param.value );
   }

   // Copy computed values
   clone->m_blackPoint = m_blackPoint;
   clone->m_whitePoint = m_whitePoint;
   clone->m_midtone = m_midtone;
   clone->m_signalCompression = m_signalCompression;
   clone->m_configured = m_configured;

   return clone;
}

// ----------------------------------------------------------------------------

} // namespace pcl
