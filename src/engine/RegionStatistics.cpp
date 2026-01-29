//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "RegionStatistics.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace pcl
{

// ----------------------------------------------------------------------------
// Histogram Implementation
// ----------------------------------------------------------------------------

NXHistogram::NXHistogram( int numBins )
   : m_bins()
{
   // Validate number of bins to prevent allocation crash
   if ( numBins <= 0 || numBins > 16777216 )  // Max 16M bins
      throw std::runtime_error( "Invalid histogram bin count: " + std::to_string( numBins ) );

   m_bins.resize( numBins, 0.0 );
}

// ----------------------------------------------------------------------------

void NXHistogram::Clear()
{
   std::fill( m_bins.begin(), m_bins.end(), 0.0 );
   m_totalCount = 0;
   m_totalWeight = 0;
   m_statsValid = false;
}

// ----------------------------------------------------------------------------

int NXHistogram::ValueToBin( double value ) const
{
   int numBins = static_cast<int>( m_bins.size() );
   int bin = static_cast<int>( value * (numBins - 1) + 0.5 );
   return std::max( 0, std::min( numBins - 1, bin ) );
}

// ----------------------------------------------------------------------------

double NXHistogram::BinToValue( int bin ) const
{
   int numBins = static_cast<int>( m_bins.size() );
   return static_cast<double>( bin ) / (numBins - 1);
}

// ----------------------------------------------------------------------------

void NXHistogram::Add( double value, double weight )
{
   if ( value < 0.0 ) value = 0.0;
   if ( value > 1.0 ) value = 1.0;

   int bin = ValueToBin( value );
   m_bins[bin] += weight;
   m_totalCount += 1;
   m_totalWeight += weight;
   m_statsValid = false;
}

// ----------------------------------------------------------------------------

void NXHistogram::ComputeBasicStats() const
{
   if ( m_statsValid )
      return;

   if ( m_totalWeight <= 0 )
   {
      m_cachedMean = 0;
      m_cachedVariance = 0;
      m_statsValid = true;
      return;
   }

   // Compute mean
   double sum = 0;
   int numBins = static_cast<int>( m_bins.size() );

   for ( int i = 0; i < numBins; ++i )
   {
      sum += m_bins[i] * BinToValue( i );
   }
   m_cachedMean = sum / m_totalWeight;

   // Compute variance
   double varSum = 0;
   for ( int i = 0; i < numBins; ++i )
   {
      double diff = BinToValue( i ) - m_cachedMean;
      varSum += m_bins[i] * diff * diff;
   }
   m_cachedVariance = varSum / m_totalWeight;

   m_statsValid = true;
}

// ----------------------------------------------------------------------------

double NXHistogram::Mean() const
{
   ComputeBasicStats();
   return m_cachedMean;
}

// ----------------------------------------------------------------------------

double NXHistogram::StdDev() const
{
   ComputeBasicStats();
   return std::sqrt( m_cachedVariance );
}

// ----------------------------------------------------------------------------

double NXHistogram::Median() const
{
   return Percentile( 0.5 );
}

// ----------------------------------------------------------------------------

double NXHistogram::Mode() const
{
   int peakBin = PeakBin();
   return BinToValue( peakBin );
}

// ----------------------------------------------------------------------------

double NXHistogram::Percentile( double p ) const
{
   if ( m_totalWeight <= 0 )
      return 0.0;

   double targetWeight = p * m_totalWeight;
   double cumulative = 0;
   int numBins = static_cast<int>( m_bins.size() );

   for ( int i = 0; i < numBins; ++i )
   {
      cumulative += m_bins[i];
      if ( cumulative >= targetWeight )
      {
         // Linear interpolation within bin
         double excess = cumulative - targetWeight;
         double binFrac = 1.0 - (m_bins[i] > 0 ? excess / m_bins[i] : 0);
         return BinToValue( i ) + binFrac / (numBins - 1);
      }
   }

   return 1.0;
}

// ----------------------------------------------------------------------------

int NXHistogram::PeakBin() const
{
   int numBins = static_cast<int>( m_bins.size() );
   int peakBin = 0;
   double peakValue = m_bins[0];

   for ( int i = 1; i < numBins; ++i )
   {
      if ( m_bins[i] > peakValue )
      {
         peakValue = m_bins[i];
         peakBin = i;
      }
   }

   return peakBin;
}

// ----------------------------------------------------------------------------

double NXHistogram::PeakValue() const
{
   return BinToValue( PeakBin() );
}

// ----------------------------------------------------------------------------

double NXHistogram::PeakWidth( double threshold ) const
{
   int peak = PeakBin();
   double peakHeight = m_bins[peak];
   double halfHeight = peakHeight * threshold;
   int numBins = static_cast<int>( m_bins.size() );

   // Find left half-max
   int left = peak;
   while ( left > 0 && m_bins[left] > halfHeight )
      --left;

   // Find right half-max
   int right = peak;
   while ( right < numBins - 1 && m_bins[right] > halfHeight )
      ++right;

   return BinToValue( right ) - BinToValue( left );
}

// ----------------------------------------------------------------------------

double NXHistogram::Skewness() const
{
   ComputeBasicStats();

   if ( m_cachedVariance <= 0 || m_totalWeight <= 0 )
      return 0.0;

   double stdDev3 = std::pow( m_cachedVariance, 1.5 );
   double sum = 0;
   int numBins = static_cast<int>( m_bins.size() );

   for ( int i = 0; i < numBins; ++i )
   {
      double diff = BinToValue( i ) - m_cachedMean;
      sum += m_bins[i] * diff * diff * diff;
   }

   return sum / (m_totalWeight * stdDev3);
}

// ----------------------------------------------------------------------------

double NXHistogram::Kurtosis() const
{
   ComputeBasicStats();

   if ( m_cachedVariance <= 0 || m_totalWeight <= 0 )
      return 0.0;

   double variance2 = m_cachedVariance * m_cachedVariance;
   double sum = 0;
   int numBins = static_cast<int>( m_bins.size() );

   for ( int i = 0; i < numBins; ++i )
   {
      double diff = BinToValue( i ) - m_cachedMean;
      double diff2 = diff * diff;
      sum += m_bins[i] * diff2 * diff2;
   }

   return sum / (m_totalWeight * variance2) - 3.0;  // Excess kurtosis
}

// ----------------------------------------------------------------------------

double NXHistogram::ClippedLowPercent() const
{
   if ( m_totalWeight <= 0 )
      return 0.0;
   return 100.0 * m_bins[0] / m_totalWeight;
}

// ----------------------------------------------------------------------------

double NXHistogram::ClippedHighPercent() const
{
   if ( m_totalWeight <= 0 )
      return 0.0;
   return 100.0 * m_bins[m_bins.size() - 1] / m_totalWeight;
}

// ----------------------------------------------------------------------------

void NXHistogram::Normalize()
{
   if ( m_totalWeight > 0 )
   {
      for ( double& bin : m_bins )
      {
         bin /= m_totalWeight;
      }
      m_totalWeight = 1.0;
   }
}

// ----------------------------------------------------------------------------
// RegionStatistics Implementation
// ----------------------------------------------------------------------------

String RegionStatistics::ToString() const
{
   // Use IsoString for region name to ensure proper UTF-8 encoding with %s
   IsoString regionName = IsoString( RegionClassDisplayName( regionClass ) );
   return String().Format(
      "Region: %s\n"
      "  Pixels: %zu (%.2f%% coverage)\n"
      "  Range: [%.6f, %.6f] (DR: %.2f)\n"
      "  Mean: %.6f, Median: %.6f, StdDev: %.6f\n"
      "  MAD: %.6f, SNR: %.1f\n"
      "  Clipping: %.2f%% low, %.2f%% high\n"
      "  Percentiles: P05=%.4f, P25=%.4f, P75=%.4f, P95=%.4f",
      regionName.c_str(),
      pixelCount, maskCoverage * 100.0,
      min, max, dynamicRange,
      mean, median, stdDev,
      mad, snrEstimate,
      clippedLowPct, clippedHighPct,
      p05, p25, p75, p95
   );
}

// ----------------------------------------------------------------------------

} // namespace pcl
