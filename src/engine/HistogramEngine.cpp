//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "HistogramEngine.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <thread>

namespace pcl
{

// ----------------------------------------------------------------------------
// ParallelHistogramAccumulator Implementation
// ----------------------------------------------------------------------------

ParallelHistogramAccumulator::ParallelHistogramAccumulator( int numBins )
   : m_bins( numBins, 0.0 )
{
}

// ----------------------------------------------------------------------------

void ParallelHistogramAccumulator::Add( double value, double weight )
{
   if ( value < 0.0 ) value = 0.0;
   if ( value > 1.0 ) value = 1.0;

   int numBins = static_cast<int>( m_bins.size() );
   int bin = static_cast<int>( value * (numBins - 1) + 0.5 );
   bin = std::max( 0, std::min( numBins - 1, bin ) );

   m_bins[bin] += weight;
   m_count += 1;
   m_weight += weight;
   m_sumValues += value * weight;
   m_sumSquares += value * value * weight;
   m_min = std::min( m_min, value );
   m_max = std::max( m_max, value );
}

// ----------------------------------------------------------------------------

void ParallelHistogramAccumulator::Merge( const ParallelHistogramAccumulator& other )
{
   for ( size_t i = 0; i < m_bins.size(); ++i )
   {
      m_bins[i] += other.m_bins[i];
   }
   m_count += other.m_count;
   m_weight += other.m_weight;
   m_sumValues += other.m_sumValues;
   m_sumSquares += other.m_sumSquares;
   m_min = std::min( m_min, other.m_min );
   m_max = std::max( m_max, other.m_max );
}

// ----------------------------------------------------------------------------
// HistogramEngine Implementation
// ----------------------------------------------------------------------------

HistogramEngine::HistogramEngine( const HistogramConfig& config )
   : m_config( config )
{
}

// ----------------------------------------------------------------------------

double HistogramEngine::ComputeLuminance( double r, double g, double b ) const
{
   // Rec. 709 luminance coefficients
   return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

// ----------------------------------------------------------------------------

NXHistogram HistogramEngine::ComputeHistogram( const Image& image, int channel ) const
{
   NXHistogram hist( m_config.numBins );

   int width = image.Width();
   int height = image.Height();

   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         double value = image( x, y, channel );

         if ( value >= m_config.clipLow && value <= m_config.clipHigh )
         {
            hist.Add( value );
         }
      }
   }

   return hist;
}

// ----------------------------------------------------------------------------

NXHistogram HistogramEngine::ComputeHistogram( const Image& image, const Image& mask,
                                             int channel, int maskChannel ) const
{
   NXHistogram hist( m_config.numBins );

   int width = image.Width();
   int height = image.Height();

   // Ensure mask dimensions match (or use whole image if no match)
   bool useMask = (mask.Width() == width && mask.Height() == height);

   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         double value = image( x, y, channel );
         double weight = useMask ? mask( x, y, maskChannel ) : 1.0;

         if ( weight > 0.0 && value >= m_config.clipLow && value <= m_config.clipHigh )
         {
            hist.Add( value, weight );
         }
      }
   }

   return hist;
}

// ----------------------------------------------------------------------------

NXHistogram HistogramEngine::ComputeHistogram( const std::vector<double>& values,
                                             const std::vector<double>& weights ) const
{
   NXHistogram hist( m_config.numBins );

   bool hasWeights = !weights.empty() && weights.size() == values.size();

   for ( size_t i = 0; i < values.size(); ++i )
   {
      double value = values[i];
      double weight = hasWeights ? weights[i] : 1.0;

      if ( value >= m_config.clipLow && value <= m_config.clipHigh )
      {
         hist.Add( value, weight );
      }
   }

   return hist;
}

// ----------------------------------------------------------------------------

void HistogramEngine::ComputeQuickStats( const Image& image, const Image* mask,
                                         int channel, int maskChannel,
                                         double& min, double& max,
                                         double& mean, double& stdDev ) const
{
   int width = image.Width();
   int height = image.Height();

   min = 1.0;
   max = 0.0;
   double sum = 0;
   double sumSq = 0;
   double count = 0;

   bool useMask = mask && mask->Width() == width && mask->Height() == height;

   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         double weight = useMask ? (*mask)( x, y, maskChannel ) : 1.0;
         if ( weight <= 0 )
            continue;

         double value = image( x, y, channel );
         min = std::min( min, value );
         max = std::max( max, value );
         sum += value * weight;
         sumSq += value * value * weight;
         count += weight;
      }
   }

   if ( count > 0 )
   {
      mean = sum / count;
      double variance = (sumSq / count) - (mean * mean);
      stdDev = std::sqrt( std::max( 0.0, variance ) );
   }
   else
   {
      mean = 0;
      stdDev = 0;
   }
}

// ----------------------------------------------------------------------------

double HistogramEngine::ComputeMAD( const std::vector<double>& sortedValues,
                                    double median ) const
{
   if ( sortedValues.empty() )
      return 0;

   std::vector<double> deviations;
   deviations.reserve( sortedValues.size() );

   for ( double v : sortedValues )
   {
      deviations.push_back( std::abs( v - median ) );
   }

   std::sort( deviations.begin(), deviations.end() );

   size_t n = deviations.size();
   if ( n % 2 == 0 )
   {
      return (deviations[n/2 - 1] + deviations[n/2]) / 2.0;
   }
   else
   {
      return deviations[n/2];
   }
}

// ----------------------------------------------------------------------------

double HistogramEngine::ComputeSn( const std::vector<double>& values ) const
{
   // Sn scale estimator (Rousseeuw and Croux, 1993)
   // This is a simplified version; full implementation is more complex

   size_t n = values.size();
   if ( n < 2 )
      return 0;

   // For efficiency, use a sampling approach for large datasets
   size_t sampleSize = std::min( n, size_t( 1000 ) );
   double step = double( n ) / sampleSize;

   std::vector<double> medians;
   medians.reserve( sampleSize );

   for ( size_t i = 0; i < sampleSize; ++i )
   {
      size_t idx = size_t( i * step );
      double vi = values[idx];

      std::vector<double> diffs;
      diffs.reserve( sampleSize );

      for ( size_t j = 0; j < sampleSize; ++j )
      {
         size_t jdx = size_t( j * step );
         diffs.push_back( std::abs( vi - values[jdx] ) );
      }

      std::sort( diffs.begin(), diffs.end() );
      medians.push_back( diffs[diffs.size() / 2] );
   }

   std::sort( medians.begin(), medians.end() );

   // Scale factor for consistency with standard deviation
   return 1.1926 * medians[medians.size() / 2];
}

// ----------------------------------------------------------------------------

double HistogramEngine::ComputeBiweightMidvariance( const std::vector<double>& values,
                                                    double median, double mad ) const
{
   if ( values.empty() || mad <= 0 )
      return 0;

   double c = 9.0;  // Tuning constant
   double sumNum = 0;
   double sumDen = 0;

   for ( double v : values )
   {
      double u = (v - median) / (c * mad);

      if ( std::abs( u ) < 1.0 )
      {
         double u2 = u * u;
         double factor = 1.0 - u2;
         double factor2 = factor * factor;
         double factor4 = factor2 * factor2;

         sumNum += factor4 * (v - median) * (v - median);
         sumDen += factor2 * (1.0 - 5.0 * u2);
      }
   }

   double n = values.size();
   if ( std::abs( sumDen ) < 1e-10 )
      return mad * mad;

   return n * sumNum / (sumDen * sumDen);
}

// ----------------------------------------------------------------------------

double HistogramEngine::EstimateSNR( const RegionStatistics& stats ) const
{
   // Estimate SNR as (signal - background) / noise
   // Using MAD as robust noise estimator

   double noise = stats.mad > 0 ? stats.mad * 1.4826 : stats.stdDev;
   if ( noise <= 0 )
      noise = 0.001;

   double background = stats.p05;  // Use 5th percentile as background estimate
   double signal = stats.median - background;

   return std::max( 0.1, signal / noise );
}

// ----------------------------------------------------------------------------

void HistogramEngine::ComputeRobustStats( const std::vector<double>& sortedValues,
                                          const std::vector<double>& weights,
                                          RegionStatistics& stats ) const
{
   if ( sortedValues.empty() )
      return;

   // MAD
   stats.mad = ComputeMAD( sortedValues, stats.median );

   // Sn estimator (only if we have enough samples)
   if ( sortedValues.size() >= 10 && m_config.computeRobustStats )
   {
      stats.sn = ComputeSn( sortedValues );
      stats.biweightMidvariance = ComputeBiweightMidvariance(
         sortedValues, stats.median, stats.mad );
   }
   else
   {
      stats.sn = stats.mad * 1.4826;  // Scale MAD to estimate stddev
      stats.biweightMidvariance = stats.mad * stats.mad;
   }
}

// ----------------------------------------------------------------------------

RegionStatistics HistogramEngine::ComputeStatistics( const Image& image, int channel ) const
{
   RegionStatistics stats;

   int width = image.Width();
   int height = image.Height();

   // Validate dimensions to catch corruption early
   if ( width <= 0 || height <= 0 || width > 100000 || height > 100000 )
      throw std::runtime_error( "Invalid image dimensions in ComputeStatistics" );

   size_t numPixels = size_t( width ) * height;
   if ( numPixels > 500000000 )  // 500M pixel limit
      throw std::runtime_error( "Image too large for statistics computation" );

   // Collect all values
   std::vector<double> values;
   values.reserve( numPixels );

   double min = 1.0, max = 0.0;
   double sum = 0;

   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         double v = image( x, y, channel );
         values.push_back( v );
         min = std::min( min, v );
         max = std::max( max, v );
         sum += v;
      }
   }

   size_t n = values.size();
   stats.pixelCount = n;
   stats.maskCoverage = 1.0;

   if ( n == 0 )
      return stats;

   // Basic stats
   stats.min = min;
   stats.max = max;
   stats.mean = sum / n;

   // Sort for percentiles and median
   std::sort( values.begin(), values.end() );

   stats.median = values[n / 2];
   stats.p01 = values[size_t( n * 0.01 )];
   stats.p05 = values[size_t( n * 0.05 )];
   stats.p10 = values[size_t( n * 0.10 )];
   stats.p25 = values[size_t( n * 0.25 )];
   stats.p75 = values[size_t( n * 0.75 )];
   stats.p90 = values[size_t( n * 0.90 )];
   stats.p95 = values[size_t( n * 0.95 )];
   stats.p99 = values[size_t( n * 0.99 )];

   // Standard deviation
   double sumSq = 0;
   for ( double v : values )
   {
      double diff = v - stats.mean;
      sumSq += diff * diff;
   }
   stats.stdDev = std::sqrt( sumSq / n );

   // Robust stats
   if ( m_config.computeRobustStats )
   {
      ComputeRobustStats( values, {}, stats );
   }

   // Dynamic range
   if ( min > 1e-10 )
   {
      stats.dynamicRange = std::log10( max / min );
      stats.dynamicRangeDB = 20.0 * std::log10( max / min );
   }
   else
   {
      stats.dynamicRange = std::log10( max / 1e-10 );
      stats.dynamicRangeDB = 20.0 * stats.dynamicRange;
   }

   // Clipping
   size_t clippedLow = 0, clippedHigh = 0;
   for ( double v : values )
   {
      if ( v <= 0.0 ) ++clippedLow;
      if ( v >= 1.0 ) ++clippedHigh;
   }
   stats.clippedLowPct = 100.0 * clippedLow / n;
   stats.clippedHighPct = 100.0 * clippedHigh / n;
   stats.clippingPct = stats.clippedLowPct + stats.clippedHighPct;

   // SNR estimate
   stats.noiseEstimate = stats.mad > 0 ? stats.mad * 1.4826 : stats.stdDev;
   stats.signalEstimate = stats.median - stats.p05;
   stats.snrEstimate = EstimateSNR( stats );

   // Histogram
   stats.histogram = ComputeHistogram( values );
   stats.mode = stats.histogram.Mode();
   stats.peakLocation = stats.histogram.PeakValue();
   stats.peakWidth = stats.histogram.PeakWidth();
   stats.skewness = stats.histogram.Skewness();
   stats.kurtosis = stats.histogram.Kurtosis();

   return stats;
}

// ----------------------------------------------------------------------------

RegionStatistics HistogramEngine::ComputeStatistics( const Image& image, const Image& mask,
                                                     int channel, int maskChannel ) const
{
   RegionStatistics stats;

   int width = image.Width();
   int height = image.Height();

   // Validate dimensions to catch corruption early
   if ( width <= 0 || height <= 0 || width > 100000 || height > 100000 )
      throw std::runtime_error( "Invalid image dimensions in ComputeStatistics (masked)" );

   bool useMask = (mask.Width() == width && mask.Height() == height);

   // Collect values with weights
   std::vector<double> values;
   std::vector<double> weights;
   size_t estimatedSize = size_t( width ) * height / 4;
   values.reserve( estimatedSize );
   weights.reserve( estimatedSize );

   double min = 1.0, max = 0.0;
   double sumW = 0, sumVW = 0;
   double totalMaskWeight = 0;

   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         double w = useMask ? mask( x, y, maskChannel ) : 1.0;
         if ( w <= 0.0001 )
            continue;

         double v = image( x, y, channel );
         values.push_back( v );
         weights.push_back( w );

         min = std::min( min, v );
         max = std::max( max, v );
         sumW += w;
         sumVW += v * w;
         totalMaskWeight += w;
      }
   }

   size_t n = values.size();
   stats.pixelCount = n;
   stats.maskCoverage = totalMaskWeight / (width * height);

   if ( n == 0 || sumW <= 0 )
      return stats;

   // Basic stats
   stats.min = min;
   stats.max = max;
   stats.mean = sumVW / sumW;

   // Sort values (with weights) for percentiles
   std::vector<size_t> indices( n );
   std::iota( indices.begin(), indices.end(), 0 );
   std::sort( indices.begin(), indices.end(),
              [&values]( size_t a, size_t b ) { return values[a] < values[b]; } );

   // Create sorted values
   std::vector<double> sortedValues( n );
   std::vector<double> sortedWeights( n );
   for ( size_t i = 0; i < n; ++i )
   {
      sortedValues[i] = values[indices[i]];
      sortedWeights[i] = weights[indices[i]];
   }

   // Weighted percentiles
   auto weightedPercentile = [&]( double p ) -> double {
      double targetWeight = p * sumW;
      double cumWeight = 0;
      for ( size_t i = 0; i < n; ++i )
      {
         cumWeight += sortedWeights[i];
         if ( cumWeight >= targetWeight )
            return sortedValues[i];
      }
      return sortedValues[n - 1];
   };

   stats.median = weightedPercentile( 0.5 );
   stats.p01 = weightedPercentile( 0.01 );
   stats.p05 = weightedPercentile( 0.05 );
   stats.p10 = weightedPercentile( 0.10 );
   stats.p25 = weightedPercentile( 0.25 );
   stats.p75 = weightedPercentile( 0.75 );
   stats.p90 = weightedPercentile( 0.90 );
   stats.p95 = weightedPercentile( 0.95 );
   stats.p99 = weightedPercentile( 0.99 );

   // Weighted standard deviation
   double sumWSq = 0;
   for ( size_t i = 0; i < n; ++i )
   {
      double diff = values[i] - stats.mean;
      sumWSq += weights[i] * diff * diff;
   }
   stats.stdDev = std::sqrt( sumWSq / sumW );

   // Robust stats (use unweighted for simplicity)
   if ( m_config.computeRobustStats )
   {
      ComputeRobustStats( sortedValues, sortedWeights, stats );
   }

   // Dynamic range
   if ( min > 1e-10 )
   {
      stats.dynamicRange = std::log10( max / min );
   }
   else
   {
      stats.dynamicRange = std::log10( max / 1e-10 );
   }
   stats.dynamicRangeDB = 20.0 * stats.dynamicRange;

   // Clipping
   double clippedLowW = 0, clippedHighW = 0;
   for ( size_t i = 0; i < n; ++i )
   {
      if ( values[i] <= 0.0 ) clippedLowW += weights[i];
      if ( values[i] >= 1.0 ) clippedHighW += weights[i];
   }
   stats.clippedLowPct = 100.0 * clippedLowW / sumW;
   stats.clippedHighPct = 100.0 * clippedHighW / sumW;
   stats.clippingPct = stats.clippedLowPct + stats.clippedHighPct;

   // SNR estimate
   stats.noiseEstimate = stats.mad > 0 ? stats.mad * 1.4826 : stats.stdDev;
   stats.signalEstimate = stats.median - stats.p05;
   stats.snrEstimate = EstimateSNR( stats );

   // Histogram
   stats.histogram = ComputeHistogram( values, weights );
   stats.mode = stats.histogram.Mode();
   stats.peakLocation = stats.histogram.PeakValue();
   stats.peakWidth = stats.histogram.PeakWidth();
   stats.skewness = stats.histogram.Skewness();
   stats.kurtosis = stats.histogram.Kurtosis();

   return stats;
}

// ----------------------------------------------------------------------------

ChannelStatistics HistogramEngine::ComputeChannelStatistics( const Image& image,
                                                             const Image* mask ) const
{
   ChannelStatistics stats;

   stats.numChannels = image.NumberOfNominalChannels();
   stats.isColor = (stats.numChannels >= 3);

   // Compute luminance statistics
   if ( stats.isColor )
   {
      // Create luminance image
      int width = image.Width();
      int height = image.Height();

      std::vector<double> lumValues;
      std::vector<double> lumWeights;
      lumValues.reserve( size_t( width ) * height );

      bool useMask = mask && mask->Width() == width && mask->Height() == height;

      double satSum = 0;
      double satMax = 0;
      double hueSum = 0;
      double hueCount = 0;

      for ( int y = 0; y < height; ++y )
      {
         for ( int x = 0; x < width; ++x )
         {
            double w = useMask ? (*mask)( x, y, 0 ) : 1.0;
            if ( w <= 0.0001 )
               continue;

            double r = image( x, y, 0 );
            double g = image( x, y, 1 );
            double b = image( x, y, 2 );

            double lum = ComputeLuminance( r, g, b );
            lumValues.push_back( lum );
            if ( useMask )
               lumWeights.push_back( w );

            // Saturation (simple HSL-like)
            double maxRGB = std::max( { r, g, b } );
            double minRGB = std::min( { r, g, b } );
            double sat = (maxRGB > 0) ? (maxRGB - minRGB) / maxRGB : 0;
            satSum += sat * w;
            satMax = std::max( satMax, sat );

            // Hue (simplified)
            if ( maxRGB - minRGB > 0.001 )
            {
               double hue;
               if ( maxRGB == r )
                  hue = 60.0 * std::fmod( (g - b) / (maxRGB - minRGB), 6.0 );
               else if ( maxRGB == g )
                  hue = 60.0 * ((b - r) / (maxRGB - minRGB) + 2.0);
               else
                  hue = 60.0 * ((r - g) / (maxRGB - minRGB) + 4.0);

               if ( hue < 0 ) hue += 360.0;
               hueSum += hue * w;
               hueCount += w;
            }
         }
      }

      // Luminance histogram
      stats.luminance = ComputeStatistics( lumValues.empty() ? std::vector<double>{0.5} : lumValues,
                                           lumWeights.empty() ? std::vector<double>{} : lumWeights );

      // Color metrics
      double totalWeight = lumValues.size();
      stats.saturationMean = (totalWeight > 0) ? satSum / totalWeight : 0;
      stats.saturationMax = satMax;
      stats.hueDominant = (hueCount > 0) ? hueSum / hueCount : 0;

      // Per-channel
      if ( mask )
      {
         stats.red = ComputeStatistics( image, *mask, 0, 0 );
         stats.green = ComputeStatistics( image, *mask, 1, 0 );
         stats.blue = ComputeStatistics( image, *mask, 2, 0 );
      }
      else
      {
         stats.red = ComputeStatistics( image, 0 );
         stats.green = ComputeStatistics( image, 1 );
         stats.blue = ComputeStatistics( image, 2 );
      }

      // Color balance
      stats.colorBalance = (stats.blue.mean > 0) ? stats.red.mean / stats.blue.mean : 1.0;
   }
   else
   {
      // Grayscale
      if ( mask )
      {
         stats.luminance = ComputeStatistics( image, *mask, 0, 0 );
      }
      else
      {
         stats.luminance = ComputeStatistics( image, 0 );
      }
   }

   return stats;
}

// Helper for vector-based statistics
RegionStatistics HistogramEngine::ComputeStatistics( const std::vector<double>& values,
                                                     const std::vector<double>& weights ) const
{
   RegionStatistics stats;

   if ( values.empty() )
      return stats;

   // This is a simplified version for internal use
   std::vector<double> sorted = values;
   std::sort( sorted.begin(), sorted.end() );

   size_t n = sorted.size();
   stats.pixelCount = n;
   stats.min = sorted.front();
   stats.max = sorted.back();
   stats.median = sorted[n / 2];

   double sum = std::accumulate( values.begin(), values.end(), 0.0 );
   stats.mean = sum / n;

   double sumSq = 0;
   for ( double v : values )
   {
      double diff = v - stats.mean;
      sumSq += diff * diff;
   }
   stats.stdDev = std::sqrt( sumSq / n );

   stats.mad = ComputeMAD( sorted, stats.median );

   // Percentiles
   stats.p25 = sorted[n / 4];
   stats.p75 = sorted[3 * n / 4];

   return stats;
}

// ----------------------------------------------------------------------------

} // namespace pcl
