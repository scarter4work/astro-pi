//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// Distribution Fitter - Per-tile distribution modeling for selective stacking

#include "DistributionFitter.h"

#include <pcl/Math.h>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace pcl
{

// ----------------------------------------------------------------------------
// Mathematical constants
// ----------------------------------------------------------------------------

static const double PI = 3.14159265358979323846;
static const double SQRT_2 = 1.41421356237309504880;
static const double SQRT_2PI = 2.50662827463100050242;

// ----------------------------------------------------------------------------
// DistributionFitter Implementation
// ----------------------------------------------------------------------------

DistributionFitter::DistributionFitter( int tileSize )
   : m_tileSize( tileSize )
{
}

// ----------------------------------------------------------------------------

void DistributionFitter::GetTileIndex( int pixelX, int pixelY, int& tileX, int& tileY ) const
{
   tileX = pixelX / m_tileSize;
   tileY = pixelY / m_tileSize;
}

// ----------------------------------------------------------------------------

void DistributionFitter::GetGridDimensions( int imageWidth, int imageHeight, int& tilesX, int& tilesY ) const
{
   tilesX = (imageWidth + m_tileSize - 1) / m_tileSize;
   tilesY = (imageHeight + m_tileSize - 1) / m_tileSize;
}

// ----------------------------------------------------------------------------

std::vector<std::vector<TileMetadata>> DistributionFitter::FitImage( const Image& image, int channel ) const
{
   int width = image.Width();
   int height = image.Height();

   // Validate dimensions to prevent allocation crashes
   if ( width <= 0 || height <= 0 || width > 100000 || height > 100000 )
      throw std::runtime_error( "Invalid image dimensions for distribution fitting" );

   int tilesX, tilesY;
   GetGridDimensions( width, height, tilesX, tilesY );

   // Validate tile grid size
   size_t numTiles = static_cast<size_t>( tilesX ) * tilesY;
   if ( numTiles > 10000000 )  // 10M tiles max
      throw std::runtime_error( "Distribution fitter tile grid too large" );

   std::vector<std::vector<TileMetadata>> grid( tilesY, std::vector<TileMetadata>( tilesX ) );

   // Get pixel data - PCL Image uses float samples
   const Image::sample* data = image.PixelData( channel );

   // Process each tile
   for ( int ty = 0; ty < tilesY; ++ty )
   {
      for ( int tx = 0; tx < tilesX; ++tx )
      {
         TileMetadata& meta = grid[ty][tx];
         meta.x = tx;
         meta.y = ty;
         meta.width = m_tileSize;
         meta.height = m_tileSize;

         // Collect pixel values in this tile
         std::vector<double> values;
         values.reserve( m_tileSize * m_tileSize );

         int x0 = tx * m_tileSize;
         int y0 = ty * m_tileSize;
         int x1 = std::min( x0 + m_tileSize, width );
         int y1 = std::min( y0 + m_tileSize, height );

         for ( int y = y0; y < y1; ++y )
         {
            for ( int x = x0; x < x1; ++x )
            {
               size_t idx = static_cast<size_t>( y ) * width + x;
               values.push_back( static_cast<double>( data[idx] ) );
            }
         }

         // Fit distribution to tile values
         meta.distribution = FitDistribution( values );
         meta.distribution.sampleCount = static_cast<int>( values.size() );
      }
   }

   return grid;
}

// ----------------------------------------------------------------------------

DistributionParams DistributionFitter::FitDistribution( const std::vector<double>& values ) const
{
   DistributionParams params;

   if ( values.empty() )
      return params;

   // Make a copy for median/MAD computation (which sorts)
   std::vector<double> sortedValues = values;

   // Compute robust statistics
   double median = ComputeMedian( sortedValues );
   double mad = ComputeMAD( sortedValues, median );
   double robustSigma = MADToSigma( mad );

   // Compute standard statistics
   double mean = ComputeMean( values );
   double sigma = ComputeStdDev( values, mean );

   // Use robust sigma if standard sigma seems contaminated
   // (robust sigma much smaller indicates outliers)
   if ( robustSigma > 0 && sigma > 2.0 * robustSigma )
   {
      sigma = robustSigma;
      mean = median;  // Use median as location estimate
   }

   // Compute higher moments for distribution selection
   double skewness = (sigma > 1e-10) ? ComputeSkewness( values, mean, sigma ) : 0.0;
   double kurtosis = (sigma > 1e-10) ? ComputeKurtosis( values, mean, sigma ) : 0.0;

   // Select distribution type based on moments
   params.type = SelectDistributionType( skewness, kurtosis );
   params.mu = mean;
   params.sigma = std::max( sigma, 1e-10 );  // Prevent zero sigma
   params.skewness = skewness;
   params.kurtosis = kurtosis;

   // Compute goodness of fit
   params.quality = ComputeGoodnessOfFit( values, params );

   return params;
}

// ----------------------------------------------------------------------------

DistributionType DistributionFitter::SelectDistributionType( double skewness, double kurtosis ) const
{
   // Thresholds for distribution selection
   const double SKEW_THRESHOLD = 0.5;      // Significant skewness
   const double KURTOSIS_THRESHOLD = 1.0;  // Heavy tails

   double absSkew = std::abs( skewness );

   // Highly skewed data -> use skew-normal or lognormal
   if ( absSkew > SKEW_THRESHOLD )
   {
      // Positive skew with heavy tails suggests lognormal
      if ( skewness > 0 && kurtosis > KURTOSIS_THRESHOLD )
         return DistributionType::Lognormal;

      // Otherwise use skew-normal
      return DistributionType::Skewed;
   }

   // Relatively symmetric -> Gaussian
   return DistributionType::Gaussian;
}

// ----------------------------------------------------------------------------

double DistributionFitter::ComputeGoodnessOfFit( const std::vector<double>& values, const DistributionParams& params ) const
{
   if ( values.empty() )
      return 0.0;

   // Simple goodness of fit: mean probability of all values
   double sumProb = 0.0;
   for ( double v : values )
   {
      sumProb += ComputeProbability( v, params );
   }

   return sumProb / values.size();
}

// ----------------------------------------------------------------------------

double DistributionFitter::ComputeProbability( double value, const DistributionParams& dist )
{
   // Probability of observing this value given the distribution
   // Returns 0-1, with 1 being perfect match to expected

   if ( dist.sigma <= 0 )
      return 0.0;

   double pdf = 0.0;

   switch ( dist.type )
   {
   case DistributionType::Gaussian:
      pdf = GaussianPDF( value, dist.mu, dist.sigma );
      break;

   case DistributionType::Lognormal:
      pdf = LognormalPDF( value, dist.mu, dist.sigma );
      break;

   case DistributionType::Skewed:
      pdf = SkewNormalPDF( value, dist.mu, dist.sigma, dist.skewness );
      break;

   default:
      pdf = GaussianPDF( value, dist.mu, dist.sigma );
   }

   // Normalize to 0-1 range
   // PDF values can exceed 1, so we use a transformation
   // For Gaussian, max PDF at mean is 1/(sigma*sqrt(2*pi))
   // We compare to this maximum to get a 0-1 probability-like score
   double maxPdf = 1.0 / (dist.sigma * SQRT_2PI);
   if ( maxPdf > 0 )
   {
      double ratio = pdf / maxPdf;
      return (ratio < 0.0) ? 0.0 : ((ratio > 1.0) ? 1.0 : ratio);
   }

   return 0.0;
}

// ----------------------------------------------------------------------------

double DistributionFitter::ComputeQuality( double value, const DistributionParams& refDist )
{
   // Quality score for selective stacking
   // Higher score = pixel is more consistent with reference distribution
   // This is what the stacker uses to SELECT which frame's pixel to use

   double prob = ComputeProbability( value, refDist );

   // Boost quality for values close to the mean
   // Penalize outliers more severely
   double z = std::abs( value - refDist.mu ) / std::max( refDist.sigma, 1e-10 );

   // Exponential penalty for outliers
   // z=0 -> factor=1.0, z=2 -> factor~0.14, z=3 -> factor~0.01
   double outlierPenalty = std::exp( -0.5 * z * z );

   return prob * outlierPenalty;
}

// ----------------------------------------------------------------------------
// Probability Density Functions
// ----------------------------------------------------------------------------

double DistributionFitter::GaussianPDF( double x, double mu, double sigma )
{
   if ( sigma <= 0 )
      return 0.0;

   double z = (x - mu) / sigma;
   return std::exp( -0.5 * z * z ) / (sigma * SQRT_2PI);
}

// ----------------------------------------------------------------------------

double DistributionFitter::LognormalPDF( double x, double mu, double sigma )
{
   // Lognormal is defined for x > 0, and mu (sample-space mean) must be > 0
   if ( sigma <= 0 || x <= 0 || mu <= 0 )
      return 0.0;

   // mu and sigma are in sample space. Convert to log-space parameters:
   // sigma_log = sqrt(log(1 + (sigma/mu)^2))
   // mu_log = log(mu) - 0.5 * sigma_log^2
   double cv = sigma / mu;
   double sigma_log = std::sqrt( std::log( 1.0 + cv * cv ) );
   double mu_log = std::log( mu ) - 0.5 * sigma_log * sigma_log;

   double logx = std::log( x );
   double z = (logx - mu_log) / sigma_log;

   return std::exp( -0.5 * z * z ) / (x * sigma_log * SQRT_2PI);
}

// ----------------------------------------------------------------------------

double DistributionFitter::SkewNormalPDF( double x, double mu, double sigma, double alpha )
{
   // Skew-normal distribution
   // pdf(x) = 2 * phi(z) * Phi(alpha * z)
   // where phi is standard normal PDF, Phi is standard normal CDF

   if ( sigma <= 0 )
      return 0.0;

   double z = (x - mu) / sigma;

   // Standard normal PDF: phi(z)
   double phi = std::exp( -0.5 * z * z ) / SQRT_2PI;

   // Standard normal CDF approximation: Phi(alpha * z)
   // Using error function: Phi(x) = 0.5 * (1 + erf(x / sqrt(2)))
   double az = alpha * z;
   double Phi = 0.5 * (1.0 + std::erf( az / SQRT_2 ));

   return 2.0 * phi * Phi / sigma;
}

// ----------------------------------------------------------------------------
// Statistical Helper Functions
// ----------------------------------------------------------------------------

double DistributionFitter::ComputeMean( const std::vector<double>& values )
{
   if ( values.empty() )
      return 0.0;

   double sum = std::accumulate( values.begin(), values.end(), 0.0 );
   return sum / values.size();
}

// ----------------------------------------------------------------------------

double DistributionFitter::ComputeStdDev( const std::vector<double>& values, double mean )
{
   if ( values.size() < 2 )
      return 0.0;

   double sumSq = 0.0;
   for ( double v : values )
   {
      double d = v - mean;
      sumSq += d * d;
   }

   return std::sqrt( sumSq / (values.size() - 1) );
}

// ----------------------------------------------------------------------------

double DistributionFitter::ComputeMedian( std::vector<double>& values )
{
   if ( values.empty() )
      return 0.0;

   std::sort( values.begin(), values.end() );
   size_t n = values.size();

   if ( n % 2 == 0 )
      return (values[n/2 - 1] + values[n/2]) / 2.0;
   else
      return values[n/2];
}

// ----------------------------------------------------------------------------

double DistributionFitter::ComputeMAD( std::vector<double>& values, double median )
{
   if ( values.empty() )
      return 0.0;

   // Compute absolute deviations from median
   for ( double& v : values )
      v = std::abs( v - median );

   // MAD is median of absolute deviations
   return ComputeMedian( values );
}

// ----------------------------------------------------------------------------

double DistributionFitter::ComputeSkewness( const std::vector<double>& values, double mean, double sigma )
{
   if ( values.size() < 3 || sigma <= 0 )
      return 0.0;

   double sum3 = 0.0;
   for ( double v : values )
   {
      double z = (v - mean) / sigma;
      sum3 += z * z * z;
   }

   size_t n = values.size();
   return sum3 * n / ((n - 1.0) * (n - 2.0));
}

// ----------------------------------------------------------------------------

double DistributionFitter::ComputeKurtosis( const std::vector<double>& values, double mean, double sigma )
{
   if ( values.size() < 4 || sigma <= 0 )
      return 0.0;

   double sum4 = 0.0;
   for ( double v : values )
   {
      double z = (v - mean) / sigma;
      sum4 += z * z * z * z;
   }

   size_t n = values.size();

   // Excess kurtosis (Gaussian = 0)
   double k = (sum4 / n) - 3.0;

   // Bias correction
   double correction = (n - 1.0) / ((n - 2.0) * (n - 3.0));
   return k * ((n + 1.0) * n * correction);
}

// ----------------------------------------------------------------------------
// TileMetadataGrid Implementation
// ----------------------------------------------------------------------------

TileMetadataGrid::TileMetadataGrid( int imageWidth, int imageHeight, int tileSize )
   : m_imageWidth( imageWidth )
   , m_imageHeight( imageHeight )
   , m_tileSize( tileSize )
{
   // Validate dimensions to prevent allocation crashes
   if ( imageWidth <= 0 || imageHeight <= 0 || imageWidth > 100000 || imageHeight > 100000 )
      throw std::runtime_error( "Invalid image dimensions for tile metadata grid" );

   if ( tileSize <= 0 || tileSize > 10000 )
      throw std::runtime_error( "Invalid tile size for tile metadata grid" );

   m_tilesX = (imageWidth + tileSize - 1) / tileSize;
   m_tilesY = (imageHeight + tileSize - 1) / tileSize;

   // Validate tile grid size
   size_t numTiles = static_cast<size_t>( m_tilesX ) * m_tilesY;
   if ( numTiles > 10000000 )  // 10M tiles max
      throw std::runtime_error( "Tile metadata grid too large" );

   m_grid.resize( m_tilesY, std::vector<TileMetadata>( m_tilesX ) );

   // Initialize tile coordinates
   for ( int ty = 0; ty < m_tilesY; ++ty )
   {
      for ( int tx = 0; tx < m_tilesX; ++tx )
      {
         m_grid[ty][tx].x = tx;
         m_grid[ty][tx].y = ty;
         m_grid[ty][tx].width = tileSize;
         m_grid[ty][tx].height = tileSize;
      }
   }
}

// ----------------------------------------------------------------------------

void TileMetadataGrid::Initialize( const std::vector<std::vector<TileMetadata>>& grid )
{
   m_grid = grid;

   if ( !grid.empty() && !grid[0].empty() )
   {
      m_tilesY = static_cast<int>( grid.size() );
      m_tilesX = static_cast<int>( grid[0].size() );
      m_tileSize = grid[0][0].width;
      m_imageWidth = m_tilesX * m_tileSize;
      m_imageHeight = m_tilesY * m_tileSize;
   }
}

// ----------------------------------------------------------------------------

const TileMetadata& TileMetadataGrid::GetTileForPixel( int x, int y ) const
{
   int tx = x / m_tileSize;
   int ty = y / m_tileSize;
   return GetTile( tx, ty );
}

// ----------------------------------------------------------------------------

const TileMetadata& TileMetadataGrid::GetTile( int tileX, int tileY ) const
{
   static TileMetadata nullTile;

   if ( tileX < 0 || tileX >= m_tilesX || tileY < 0 || tileY >= m_tilesY )
      return nullTile;

   return m_grid[tileY][tileX];
}

// ----------------------------------------------------------------------------

void TileMetadataGrid::SetTile( int tileX, int tileY, const TileMetadata& meta )
{
   if ( tileX >= 0 && tileX < m_tilesX && tileY >= 0 && tileY < m_tilesY )
      m_grid[tileY][tileX] = meta;
}

// ----------------------------------------------------------------------------

std::vector<double> TileMetadataGrid::Serialize() const
{
   std::vector<double> buffer;
   buffer.reserve( SerializedSize() );

   for ( int ty = 0; ty < m_tilesY; ++ty )
   {
      for ( int tx = 0; tx < m_tilesX; ++tx )
      {
         double tileBuffer[DistributionParams::SerializedSize()];
         m_grid[ty][tx].distribution.Serialize( tileBuffer );

         for ( int i = 0; i < DistributionParams::SerializedSize(); ++i )
            buffer.push_back( tileBuffer[i] );
      }
   }

   return buffer;
}

// ----------------------------------------------------------------------------

TileMetadataGrid TileMetadataGrid::Deserialize( const std::vector<double>& buffer, int tilesX, int tilesY )
{
   TileMetadataGrid grid;
   grid.m_tilesX = tilesX;
   grid.m_tilesY = tilesY;
   grid.m_grid.resize( tilesY, std::vector<TileMetadata>( tilesX ) );

   size_t idx = 0;
   const int paramSize = DistributionParams::SerializedSize();

   for ( int ty = 0; ty < tilesY && idx + paramSize <= buffer.size(); ++ty )
   {
      for ( int tx = 0; tx < tilesX && idx + paramSize <= buffer.size(); ++tx )
      {
         grid.m_grid[ty][tx].x = tx;
         grid.m_grid[ty][tx].y = ty;
         grid.m_grid[ty][tx].distribution = DistributionParams::Deserialize( &buffer[idx] );
         idx += paramSize;
      }
   }

   return grid;
}

// ----------------------------------------------------------------------------

} // namespace pcl
