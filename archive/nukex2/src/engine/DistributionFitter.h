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

#ifndef __DistributionFitter_h
#define __DistributionFitter_h

#include <pcl/Image.h>
#include <pcl/String.h>

#include <vector>
#include <cmath>
#include <memory>

namespace pcl
{

// ----------------------------------------------------------------------------
// Distribution Types
// ----------------------------------------------------------------------------

enum class DistributionType
{
   Gaussian = 0,    // Normal distribution - symmetric around mean
   Lognormal,       // Log-normal - right-skewed, common in astronomical backgrounds
   Skewed,          // Skew-normal distribution - asymmetric
   Count
};

// ----------------------------------------------------------------------------
// Distribution Parameters
// ----------------------------------------------------------------------------

struct DistributionParams
{
   DistributionType type = DistributionType::Gaussian;
   double mu = 0.0;          // Location parameter (mean for Gaussian)
   double sigma = 0.0;       // Scale parameter (std dev for Gaussian)
   double skewness = 0.0;    // Shape parameter (0 for symmetric)
   double kurtosis = 0.0;    // Excess kurtosis (0 for Gaussian)
   double quality = 0.0;     // Goodness of fit (0-1, higher is better)
   int    sampleCount = 0;   // Number of samples used for fitting

   DistributionParams() = default;

   DistributionParams( DistributionType t, double m, double s, double sk = 0.0, double k = 0.0 )
      : type( t ), mu( m ), sigma( s ), skewness( sk ), kurtosis( k )
   {
   }

   // Serialize to compact format (8 doubles)
   void Serialize( double* buffer ) const
   {
      buffer[0] = static_cast<double>( type );
      buffer[1] = mu;
      buffer[2] = sigma;
      buffer[3] = skewness;
      buffer[4] = kurtosis;
      buffer[5] = quality;
      buffer[6] = static_cast<double>( sampleCount );
      buffer[7] = 0.0;  // Reserved
   }

   // Deserialize from compact format
   static DistributionParams Deserialize( const double* buffer )
   {
      DistributionParams p;
      p.type = static_cast<DistributionType>( static_cast<int>( buffer[0] ) );
      p.mu = buffer[1];
      p.sigma = buffer[2];
      p.skewness = buffer[3];
      p.kurtosis = buffer[4];
      p.quality = buffer[5];
      p.sampleCount = static_cast<int>( buffer[6] );
      return p;
   }

   // Number of doubles needed for serialization
   static constexpr int SerializedSize() { return 8; }
};

// ----------------------------------------------------------------------------
// Tile Metadata
// ----------------------------------------------------------------------------

struct TileMetadata
{
   int x = 0;                      // Tile X index
   int y = 0;                      // Tile Y index
   int width = 0;                  // Tile width in pixels
   int height = 0;                 // Tile height in pixels
   DistributionParams distribution;  // Fitted distribution for this tile

   // Pixel bounds
   int PixelX0() const { return x * width; }
   int PixelY0() const { return y * height; }
   int PixelX1() const { return PixelX0() + width; }
   int PixelY1() const { return PixelY0() + height; }
};

// ----------------------------------------------------------------------------
// Distribution Fitter
// ----------------------------------------------------------------------------

class DistributionFitter
{
public:

   /// Constructor with tile size
   DistributionFitter( int tileSize = 16 );

   /// Set tile size (default 16x16)
   void SetTileSize( int size ) { m_tileSize = size; }
   int TileSize() const { return m_tileSize; }

   /// Fit distributions to all tiles in an image
   /// Returns grid of tile metadata [rows][cols]
   std::vector<std::vector<TileMetadata>> FitImage( const Image& image, int channel = 0 ) const;

   /// Fit a single distribution to pixel values
   DistributionParams FitDistribution( const std::vector<double>& values ) const;

   /// Compute probability of a pixel value given distribution parameters
   /// Returns 0-1, higher means pixel matches expected distribution
   static double ComputeProbability( double value, const DistributionParams& dist );

   /// Compute quality score for a pixel given reference distribution
   /// This is what the stacker uses to SELECT pixels
   static double ComputeQuality( double value, const DistributionParams& refDist );

   /// Get tile indices for a pixel coordinate
   void GetTileIndex( int pixelX, int pixelY, int& tileX, int& tileY ) const;

   /// Get grid dimensions for an image
   void GetGridDimensions( int imageWidth, int imageHeight, int& tilesX, int& tilesY ) const;

   // Distribution-specific probability functions
   static double GaussianPDF( double x, double mu, double sigma );
   static double LognormalPDF( double x, double mu, double sigma );
   static double SkewNormalPDF( double x, double mu, double sigma, double alpha );

   // Statistical helper functions
   static double ComputeMean( const std::vector<double>& values );
   static double ComputeStdDev( const std::vector<double>& values, double mean );
   static double ComputeMedian( std::vector<double>& values );  // Modifies input!
   static double ComputeMAD( std::vector<double>& values, double median );  // Modifies input!
   static double ComputeSkewness( const std::vector<double>& values, double mean, double sigma );
   static double ComputeKurtosis( const std::vector<double>& values, double mean, double sigma );

   // Convert MAD to sigma (robust standard deviation)
   static double MADToSigma( double mad ) { return mad * 1.4826; }

private:

   int m_tileSize = 16;

   // Select best distribution type based on data characteristics
   DistributionType SelectDistributionType( double skewness, double kurtosis ) const;

   // Compute goodness of fit
   double ComputeGoodnessOfFit( const std::vector<double>& values, const DistributionParams& params ) const;
};

// ----------------------------------------------------------------------------
// Tile Metadata Grid - Storage container for all tiles
// ----------------------------------------------------------------------------

class TileMetadataGrid
{
public:

   TileMetadataGrid() = default;

   /// Create grid for image dimensions
   TileMetadataGrid( int imageWidth, int imageHeight, int tileSize = 16 );

   /// Initialize from fitted data
   void Initialize( const std::vector<std::vector<TileMetadata>>& grid );

   /// Get tile metadata for a pixel coordinate
   const TileMetadata& GetTileForPixel( int x, int y ) const;

   /// Get tile metadata by tile index
   const TileMetadata& GetTile( int tileX, int tileY ) const;

   /// Set tile metadata
   void SetTile( int tileX, int tileY, const TileMetadata& meta );

   /// Get grid dimensions
   int TilesX() const { return m_tilesX; }
   int TilesY() const { return m_tilesY; }
   int TileSize() const { return m_tileSize; }

   /// Total number of tiles
   int TotalTiles() const { return m_tilesX * m_tilesY; }

   /// Serialize to flat buffer (for FITS/XISF storage)
   std::vector<double> Serialize() const;

   /// Deserialize from flat buffer
   static TileMetadataGrid Deserialize( const std::vector<double>& buffer, int tilesX, int tilesY );

   /// Get serialized size in doubles
   size_t SerializedSize() const
   {
      return static_cast<size_t>( m_tilesX ) * m_tilesY * DistributionParams::SerializedSize();
   }

   /// Check if grid is valid
   bool IsValid() const { return m_tilesX > 0 && m_tilesY > 0; }

private:

   int m_imageWidth = 0;
   int m_imageHeight = 0;
   int m_tileSize = 16;
   int m_tilesX = 0;
   int m_tilesY = 0;

   std::vector<std::vector<TileMetadata>> m_grid;
};

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __DistributionFitter_h
