//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "ToneMapper.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace pcl
{

// ----------------------------------------------------------------------------
// ToneMapper Implementation
// ----------------------------------------------------------------------------

ToneMapper::ToneMapper( const ToneMapConfig& config )
   : m_config( config )
{
}

// ----------------------------------------------------------------------------

void ToneMapper::SetConfig( const ToneMapConfig& config )
{
   m_config = config;
   m_lutValid = false;
}

// ----------------------------------------------------------------------------

void ToneMapper::Apply( Image& image ) const
{
   int width = image.Width();
   int height = image.Height();
   int numChannels = image.NumberOfNominalChannels();

   // Apply local contrast first (needs full image)
   if ( m_config.localContrast > 0.001 )
   {
      ApplyLocalContrast( image );
   }

   // Apply equalization (needs full image)
   if ( m_config.equalization > 0.001 )
   {
      ApplyEqualization( image );
   }

   // Apply per-pixel adjustments
   if ( m_lutValid && !m_lut.empty() )
   {
      ApplyLUT( image );
   }
   else
   {
      for ( int c = 0; c < numChannels; ++c )
      {
         for ( int y = 0; y < height; ++y )
         {
            for ( int x = 0; x < width; ++x )
            {
               image( x, y, c ) = ApplyToValue( image( x, y, c ) );
            }
         }
      }
   }
}

// ----------------------------------------------------------------------------

Image ToneMapper::Apply( const Image& image ) const
{
   Image result = image;
   Apply( result );
   return result;
}

// ----------------------------------------------------------------------------

double ToneMapper::ApplyToValue( double value ) const
{
   // Apply in order: black/white point, gamma, contrast, brightness, curve, soft clip

   value = ApplyBlackWhitePoint( value );
   value = ApplyGamma( value );
   value = ApplyContrast( value );
   value = ApplyBrightness( value );
   value = ApplyCurve( value );
   value = ApplySoftClip( value );

   return std::max( 0.0, std::min( 1.0, value ) );
}

// ----------------------------------------------------------------------------

void ToneMapper::AutoDetectPoints( const Image& image )
{
   HistogramMapper mapper;
   mapper.ComputeHistogram( image );

   if ( m_config.autoBlackPoint )
   {
      m_config.blackPoint = mapper.GetPercentile( 0.001 );  // 0.1%
   }

   if ( m_config.autoWhitePoint )
   {
      m_config.whitePoint = mapper.GetPercentile( 0.999 );  // 99.9%
   }

   m_lutValid = false;
}

// ----------------------------------------------------------------------------

void ToneMapper::BuildLUT( int lutSize )
{
   // Validate LUT size to prevent vector allocation crash
   if ( lutSize <= 0 || lutSize > 16777216 )  // Max 16M entries (reasonable for 24-bit)
      throw std::runtime_error( "Invalid LUT size for tone mapping: " + std::to_string( lutSize ) );

   m_lut.resize( lutSize );

   for ( int i = 0; i < lutSize; ++i )
   {
      double value = static_cast<double>( i ) / (lutSize - 1);
      m_lut[i] = ApplyToValue( value );
   }

   m_lutValid = true;
}

// ----------------------------------------------------------------------------

void ToneMapper::ApplyLUT( Image& image ) const
{
   if ( !m_lutValid || m_lut.empty() )
      return;

   int lutSize = static_cast<int>( m_lut.size() );
   int width = image.Width();
   int height = image.Height();
   int numChannels = image.NumberOfNominalChannels();

   for ( int c = 0; c < numChannels; ++c )
   {
      for ( int y = 0; y < height; ++y )
      {
         for ( int x = 0; x < width; ++x )
         {
            double value = image( x, y, c );
            int idx = static_cast<int>( value * (lutSize - 1) + 0.5 );
            idx = std::max( 0, std::min( lutSize - 1, idx ) );
            image( x, y, c ) = m_lut[idx];
         }
      }
   }
}

// ----------------------------------------------------------------------------

double ToneMapper::ApplyBlackWhitePoint( double value ) const
{
   double bp = m_config.blackPoint;
   double wp = m_config.whitePoint;

   if ( wp <= bp )
      return value;

   value = (value - bp) / (wp - bp);
   return std::max( 0.0, std::min( 1.0, value ) );
}

// ----------------------------------------------------------------------------

double ToneMapper::ApplyGamma( double value ) const
{
   if ( std::abs( m_config.gamma - 1.0 ) < 0.001 )
      return value;

   if ( value <= 0 )
      return 0;

   return std::pow( value, 1.0 / m_config.gamma );
}

// ----------------------------------------------------------------------------

double ToneMapper::ApplyContrast( double value ) const
{
   if ( std::abs( m_config.contrast ) < 0.001 )
      return value;

   // S-curve based contrast
   double c = m_config.contrast;

   // Center around 0.5
   value = value - 0.5;

   // Apply contrast curve
   if ( c > 0 )
   {
      // Increase contrast
      double factor = 1.0 + c * 2.0;
      value = value * factor;
   }
   else
   {
      // Decrease contrast
      double factor = 1.0 / (1.0 - c * 2.0);
      value = value * factor;
   }

   return value + 0.5;
}

// ----------------------------------------------------------------------------

double ToneMapper::ApplyBrightness( double value ) const
{
   if ( std::abs( m_config.brightness ) < 0.001 )
      return value;

   return value + m_config.brightness;
}

// ----------------------------------------------------------------------------

double ToneMapper::ApplyCurve( double value ) const
{
   if ( m_config.curve.empty() )
      return value;

   const auto& curve = m_config.curve;

   // Find surrounding control points
   size_t i = 0;
   while ( i < curve.size() - 1 && curve[i + 1].first < value )
   {
      ++i;
   }

   if ( i >= curve.size() - 1 )
   {
      return curve.back().second;
   }

   // Linear interpolation between points
   double x0 = curve[i].first;
   double y0 = curve[i].second;
   double x1 = curve[i + 1].first;
   double y1 = curve[i + 1].second;

   if ( x1 <= x0 )
      return y0;

   double t = (value - x0) / (x1 - x0);
   return y0 + t * (y1 - y0);
}

// ----------------------------------------------------------------------------

double ToneMapper::ApplySoftClip( double value ) const
{
   // Apply soft clipping to highlights (shoulder)
   if ( m_config.softClipShoulder > 0.001 && value > 1.0 - m_config.softClipShoulder )
   {
      double threshold = 1.0 - m_config.softClipShoulder;
      double excess = value - threshold;
      double compressed = threshold + m_config.softClipShoulder *
                          (1.0 - std::exp( -excess / m_config.softClipShoulder ));
      value = compressed;
   }

   // Apply soft clipping to shadows (toe)
   if ( m_config.softClipToe > 0.001 && value < m_config.softClipToe )
   {
      double threshold = m_config.softClipToe;
      double deficit = threshold - value;
      double compressed = m_config.softClipToe *
                          (1.0 - std::exp( -deficit / m_config.softClipToe ));
      value = threshold - compressed;
   }

   return value;
}

// ----------------------------------------------------------------------------

void ToneMapper::ApplyLocalContrast( Image& image ) const
{
   int width = image.Width();
   int height = image.Height();
   int numChannels = image.NumberOfNominalChannels();
   int radius = m_config.localContrastRadius;

   // Validate image dimensions to prevent allocation crashes
   if ( width <= 0 || height <= 0 || width > 100000 || height > 100000 )
      throw std::runtime_error( "Invalid image dimensions for local contrast" );

   size_t numPixels = static_cast<size_t>( width ) * height;
   if ( numPixels > 500000000 )  // 500M pixels max
      throw std::runtime_error( "Image too large for local contrast processing" );

   double strength = m_config.localContrast;

   // Create local mean image
   Image localMean( width, height, image.ColorSpace() );

   // Box blur for local mean (separable)
   Image temp( width, height, image.ColorSpace() );

   for ( int c = 0; c < numChannels; ++c )
   {
      // Horizontal pass
      for ( int y = 0; y < height; ++y )
      {
         double sum = 0;
         int count = 0;

         // Initialize window
         for ( int x = 0; x <= radius && x < width; ++x )
         {
            sum += image( x, y, c );
            ++count;
         }

         for ( int x = 0; x < width; ++x )
         {
            temp( x, y, c ) = sum / count;

            // Slide window
            if ( x + radius + 1 < width )
            {
               sum += image( x + radius + 1, y, c );
               ++count;
            }
            if ( x - radius >= 0 )
            {
               sum -= image( x - radius, y, c );
               --count;
            }
         }
      }

      // Vertical pass
      for ( int x = 0; x < width; ++x )
      {
         double sum = 0;
         int count = 0;

         for ( int y = 0; y <= radius && y < height; ++y )
         {
            sum += temp( x, y, c );
            ++count;
         }

         for ( int y = 0; y < height; ++y )
         {
            localMean( x, y, c ) = sum / count;

            if ( y + radius + 1 < height )
            {
               sum += temp( x, y + radius + 1, c );
               ++count;
            }
            if ( y - radius >= 0 )
            {
               sum -= temp( x, y - radius, c );
               --count;
            }
         }
      }
   }

   // Apply local contrast enhancement
   for ( int c = 0; c < numChannels; ++c )
   {
      for ( int y = 0; y < height; ++y )
      {
         for ( int x = 0; x < width; ++x )
         {
            double value = image( x, y, c );
            double local = localMean( x, y, c );
            double detail = value - local;

            // Enhance detail
            image( x, y, c ) = local + detail * (1.0 + strength);
         }
      }
   }
}

// ----------------------------------------------------------------------------

void ToneMapper::ApplyEqualization( Image& image ) const
{
   int numChannels = image.NumberOfNominalChannels();

   for ( int c = 0; c < numChannels; ++c )
   {
      HistogramMapper mapper;
      mapper.ComputeHistogram( image, c );

      std::vector<double> lut;
      if ( m_config.adaptiveEqualization )
      {
         lut = mapper.CreateCLAHELUT( 2.0 );
      }
      else
      {
         lut = mapper.CreateEqualizationLUT();
      }

      // Blend with original based on strength
      double strength = m_config.equalization;
      int lutSize = static_cast<int>( lut.size() );

      for ( int y = 0; y < image.Height(); ++y )
      {
         for ( int x = 0; x < image.Width(); ++x )
         {
            double value = image( x, y, c );
            int idx = static_cast<int>( value * (lutSize - 1) + 0.5 );
            idx = std::max( 0, std::min( lutSize - 1, idx ) );

            double equalized = lut[idx];
            image( x, y, c ) = value * (1.0 - strength) + equalized * strength;
         }
      }
   }
}

// ----------------------------------------------------------------------------
// FilmCurves Implementation
// ----------------------------------------------------------------------------

namespace FilmCurves
{

double SCurve( double value, double strength )
{
   if ( strength <= 0 )
      return value;

   // Sigmoid S-curve
   double x = value * 2.0 - 1.0;  // Map to -1 to 1
   double s = strength * 5.0;     // Scale strength

   double curved = x / (1.0 + std::abs( x ) * s * (1.0 - std::abs( x ) ) );

   return (curved + 1.0) / 2.0;  // Map back to 0-1
}

// ----------------------------------------------------------------------------

double Reinhard( double value, double whitePoint )
{
   return value / (1.0 + value) * (1.0 + value / (whitePoint * whitePoint));
}

// ----------------------------------------------------------------------------

double Filmic( double value )
{
   // Uncharted 2 tone mapping
   double A = 0.15;  // Shoulder Strength
   double B = 0.50;  // Linear Strength
   double C = 0.10;  // Linear Angle
   double D = 0.20;  // Toe Strength
   double E = 0.02;  // Toe Numerator
   double F = 0.30;  // Toe Denominator
   double W = 11.2;  // White Point

   auto tonemap = [A, B, C, D, E, F]( double x ) {
      return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
   };

   return tonemap( value ) / tonemap( W );
}

// ----------------------------------------------------------------------------

double ACES( double value )
{
   // ACES approximation
   double a = 2.51;
   double b = 0.03;
   double c = 2.43;
   double d = 0.59;
   double e = 0.14;

   return std::max( 0.0, (value * (a * value + b)) / (value * (c * value + d) + e) );
}

// ----------------------------------------------------------------------------

double Logarithmic( double value, double base )
{
   if ( value <= 0 )
      return 0;

   return std::log( 1.0 + value * (base - 1) ) / std::log( base );
}

// ----------------------------------------------------------------------------

std::vector<std::pair<double, double>> CreateNamedCurve( const String& name )
{
   std::vector<std::pair<double, double>> curve;

   if ( name == "Linear" || name == "linear" )
   {
      curve = { {0.0, 0.0}, {1.0, 1.0} };
   }
   else if ( name == "S-Curve" || name == "scurve" )
   {
      curve = { {0.0, 0.0}, {0.25, 0.15}, {0.5, 0.5}, {0.75, 0.85}, {1.0, 1.0} };
   }
   else if ( name == "FilmNegative" || name == "filmnegative" )
   {
      curve = { {0.0, 0.02}, {0.1, 0.12}, {0.3, 0.35}, {0.5, 0.52},
                {0.7, 0.72}, {0.9, 0.92}, {1.0, 0.98} };
   }
   else if ( name == "HighContrast" || name == "highcontrast" )
   {
      curve = { {0.0, 0.0}, {0.3, 0.1}, {0.5, 0.5}, {0.7, 0.9}, {1.0, 1.0} };
   }
   else if ( name == "LowContrast" || name == "lowcontrast" )
   {
      curve = { {0.0, 0.1}, {0.3, 0.35}, {0.5, 0.5}, {0.7, 0.65}, {1.0, 0.9} };
   }
   else
   {
      // Default linear
      curve = { {0.0, 0.0}, {1.0, 1.0} };
   }

   return curve;
}

} // namespace FilmCurves

// ----------------------------------------------------------------------------
// HistogramMapper Implementation
// ----------------------------------------------------------------------------

HistogramMapper::HistogramMapper()
   : m_histogram( m_numBins, 0.0 )
   , m_cdf( m_numBins, 0.0 )
{
}

// ----------------------------------------------------------------------------

void HistogramMapper::ComputeHistogram( const Image& image, int channel )
{
   std::fill( m_histogram.begin(), m_histogram.end(), 0.0 );

   int width = image.Width();
   int height = image.Height();
   m_totalCount = 0;

   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         double value = image( x, y, channel );
         int bin = static_cast<int>( value * (m_numBins - 1) + 0.5 );
         bin = std::max( 0, std::min( m_numBins - 1, bin ) );
         m_histogram[bin] += 1.0;
         m_totalCount += 1.0;
      }
   }

   // Compute CDF
   m_cdf[0] = m_histogram[0];
   for ( int i = 1; i < m_numBins; ++i )
   {
      m_cdf[i] = m_cdf[i - 1] + m_histogram[i];
   }

   // Normalize CDF
   if ( m_totalCount > 0 )
   {
      for ( int i = 0; i < m_numBins; ++i )
      {
         m_cdf[i] /= m_totalCount;
      }
   }
}

// ----------------------------------------------------------------------------

double HistogramMapper::GetPercentile( double p ) const
{
   p = std::max( 0.0, std::min( 1.0, p ) );

   for ( int i = 0; i < m_numBins; ++i )
   {
      if ( m_cdf[i] >= p )
      {
         return static_cast<double>( i ) / (m_numBins - 1);
      }
   }

   return 1.0;
}

// ----------------------------------------------------------------------------

std::vector<double> HistogramMapper::CreateEqualizationLUT() const
{
   std::vector<double> lut( m_numBins );

   for ( int i = 0; i < m_numBins; ++i )
   {
      lut[i] = m_cdf[i];
   }

   return lut;
}

// ----------------------------------------------------------------------------

std::vector<double> HistogramMapper::CreateCLAHELUT( double clipLimit ) const
{
   // Simplified CLAHE - just clip and redistribute
   std::vector<double> clippedHist = m_histogram;

   double clipThreshold = clipLimit * (m_totalCount / m_numBins);
   double excess = 0;

   // Clip histogram
   for ( int i = 0; i < m_numBins; ++i )
   {
      if ( clippedHist[i] > clipThreshold )
      {
         excess += clippedHist[i] - clipThreshold;
         clippedHist[i] = clipThreshold;
      }
   }

   // Redistribute excess
   double redistribute = excess / m_numBins;
   for ( int i = 0; i < m_numBins; ++i )
   {
      clippedHist[i] += redistribute;
   }

   // Compute CDF of clipped histogram
   std::vector<double> lut( m_numBins );
   double cumSum = 0;
   double total = std::accumulate( clippedHist.begin(), clippedHist.end(), 0.0 );

   for ( int i = 0; i < m_numBins; ++i )
   {
      cumSum += clippedHist[i];
      lut[i] = cumSum / total;
   }

   return lut;
}

// ----------------------------------------------------------------------------

void HistogramMapper::ApplyHistogramMatch( Image& image,
                                            const std::vector<double>& targetCDF ) const
{
   if ( targetCDF.size() != size_t( m_numBins ) )
      return;

   // Create inverse CDF mapping
   std::vector<double> mapping( m_numBins );

   for ( int i = 0; i < m_numBins; ++i )
   {
      double sourceCDF = m_cdf[i];

      // Find matching position in target CDF
      int j = 0;
      while ( j < m_numBins - 1 && targetCDF[j] < sourceCDF )
      {
         ++j;
      }

      mapping[i] = static_cast<double>( j ) / (m_numBins - 1);
   }

   // Apply mapping
   int width = image.Width();
   int height = image.Height();
   int numChannels = image.NumberOfNominalChannels();

   for ( int c = 0; c < numChannels; ++c )
   {
      for ( int y = 0; y < height; ++y )
      {
         for ( int x = 0; x < width; ++x )
         {
            double value = image( x, y, c );
            int bin = static_cast<int>( value * (m_numBins - 1) + 0.5 );
            bin = std::max( 0, std::min( m_numBins - 1, bin ) );
            image( x, y, c ) = mapping[bin];
         }
      }
   }
}

// ----------------------------------------------------------------------------

} // namespace pcl
