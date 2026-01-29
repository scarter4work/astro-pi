//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// TransitionChecker - Post-integration smoothing of hard transitions

#include "TransitionChecker.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace pcl
{

// ----------------------------------------------------------------------------
// TransitionChecker Implementation
// ----------------------------------------------------------------------------

TransitionChecker::TransitionChecker()
   : m_config()
   , m_fitter( m_config.tileSize )
{
}

TransitionChecker::TransitionChecker( const TransitionCheckerConfig& config )
   : m_config( config )
   , m_fitter( config.tileSize )
{
}

// ----------------------------------------------------------------------------

std::vector<std::vector<TransitionInfo>> TransitionChecker::AnalyzeTransitions(
   const Image& image,
   int channel ) const
{
   // Create empty segmentation for no feature alignment
   Image emptySeg;
   return AnalyzeTransitions( image, emptySeg, channel );
}

// ----------------------------------------------------------------------------

std::vector<std::vector<TransitionInfo>> TransitionChecker::AnalyzeTransitions(
   const Image& image,
   const Image& segmentation,
   int channel ) const
{
   if ( image.IsEmpty() )
      return {};

   int width = image.Width();
   int height = image.Height();
   int tileSize = m_config.tileSize;

   // Validate dimensions to prevent allocation crashes
   if ( width <= 0 || height <= 0 || width > 100000 || height > 100000 )
      throw std::runtime_error( "Invalid image dimensions for transition checking" );

   if ( tileSize <= 0 || tileSize > 10000 )
      throw std::runtime_error( "Invalid tile size for transition checking" );

   int tilesX = (width + tileSize - 1) / tileSize;
   int tilesY = (height + tileSize - 1) / tileSize;

   // Validate tile grid size
   size_t numTiles = static_cast<size_t>( tilesX ) * tilesY;
   if ( numTiles > 10000000 )  // 10M tiles max
      throw std::runtime_error( "Transition grid too large" );

   // First, compute tile statistics using DistributionFitter
   auto tileGrid = m_fitter.FitImage( image, channel );

   // Allocate transition info grid
   std::vector<std::vector<TransitionInfo>> transitions(
      tilesY, std::vector<TransitionInfo>( tilesX ) );

   bool hasSegmentation = !segmentation.IsEmpty() &&
                          segmentation.Width() == width &&
                          segmentation.Height() == height;

   // Analyze each tile
   for ( int ty = 0; ty < tilesY; ++ty )
   {
      for ( int tx = 0; tx < tilesX; ++tx )
      {
         TransitionInfo& info = transitions[ty][tx];
         info.tileX = tx;
         info.tileY = ty;

         // Copy tile statistics
         const TileMetadata& meta = tileGrid[ty][tx];
         info.tileMean = static_cast<float>( meta.distribution.mu );
         info.tileStdDev = static_cast<float>( meta.distribution.sigma );

         // Compute gradients with adjacent tiles
         // Top boundary
         if ( ty > 0 )
         {
            info.gradientTop = ComputeBoundaryGradient( image, channel,
               tx, ty, tx, ty - 1, true );
         }

         // Bottom boundary
         if ( ty < tilesY - 1 )
         {
            info.gradientBottom = ComputeBoundaryGradient( image, channel,
               tx, ty, tx, ty + 1, true );
         }

         // Left boundary
         if ( tx > 0 )
         {
            info.gradientLeft = ComputeBoundaryGradient( image, channel,
               tx, ty, tx - 1, ty, false );
         }

         // Right boundary
         if ( tx < tilesX - 1 )
         {
            info.gradientRight = ComputeBoundaryGradient( image, channel,
               tx, ty, tx + 1, ty, false );
         }

         // Find maximum gradient
         info.maxGradient = std::max( {
            info.gradientTop,
            info.gradientBottom,
            info.gradientLeft,
            info.gradientRight
         } );

         // Detect hard transition
         info.hasHardTransition = (info.maxGradient > m_config.hardTransitionThreshold);

         // Check feature alignment if requested
         if ( info.hasHardTransition && m_config.checkFeatureAlignment && hasSegmentation )
         {
            // Check which boundary has the hard transition
            if ( info.gradientTop >= info.maxGradient && ty > 0 )
               info.alignsWithFeature = CheckFeatureAlignment( segmentation, tx, ty, tx, ty - 1 );
            else if ( info.gradientBottom >= info.maxGradient && ty < tilesY - 1 )
               info.alignsWithFeature = CheckFeatureAlignment( segmentation, tx, ty, tx, ty + 1 );
            else if ( info.gradientLeft >= info.maxGradient && tx > 0 )
               info.alignsWithFeature = CheckFeatureAlignment( segmentation, tx, ty, tx - 1, ty );
            else if ( info.gradientRight >= info.maxGradient && tx < tilesX - 1 )
               info.alignsWithFeature = CheckFeatureAlignment( segmentation, tx, ty, tx + 1, ty );
         }

         // Determine if smoothing is needed
         info.needsSmoothing = info.hasHardTransition && !info.alignsWithFeature;

         // Calculate smoothing strength based on gradient magnitude
         if ( info.needsSmoothing )
         {
            // Linear interpolation between soft and hard thresholds
            float range = m_config.hardTransitionThreshold - m_config.softTransitionThreshold;
            float normalized = (info.maxGradient - m_config.softTransitionThreshold) / range;
            info.smoothingStrength = std::min( normalized, 1.0f ) * m_config.maxSmoothingStrength;
         }
      }
   }

   return transitions;
}

// ----------------------------------------------------------------------------

void TransitionChecker::ApplySmoothing(
   Image& image,
   const std::vector<std::vector<TransitionInfo>>& transitions,
   int channel ) const
{
   if ( transitions.empty() || transitions[0].empty() )
      return;

   int tilesY = static_cast<int>( transitions.size() );
   int tilesX = static_cast<int>( transitions[0].size() );
   int tileSize = m_config.tileSize;

   // Apply smoothing at boundaries that need it
   for ( int ty = 0; ty < tilesY; ++ty )
   {
      for ( int tx = 0; tx < tilesX; ++tx )
      {
         const TransitionInfo& info = transitions[ty][tx];

         if ( !info.needsSmoothing )
            continue;

         // Smooth at each boundary that exceeds threshold
         if ( info.gradientTop > m_config.softTransitionThreshold && ty > 0 )
         {
            int boundaryY = ty * tileSize;
            int centerX = tx * tileSize + tileSize / 2;
            SmoothBoundary( image, channel, centerX, boundaryY, true, info.smoothingStrength );
         }

         if ( info.gradientBottom > m_config.softTransitionThreshold && ty < tilesY - 1 )
         {
            int boundaryY = (ty + 1) * tileSize;
            int centerX = tx * tileSize + tileSize / 2;
            SmoothBoundary( image, channel, centerX, boundaryY, true, info.smoothingStrength );
         }

         if ( info.gradientLeft > m_config.softTransitionThreshold && tx > 0 )
         {
            int boundaryX = tx * tileSize;
            int centerY = ty * tileSize + tileSize / 2;
            SmoothBoundary( image, channel, boundaryX, centerY, false, info.smoothingStrength );
         }

         if ( info.gradientRight > m_config.softTransitionThreshold && tx < tilesX - 1 )
         {
            int boundaryX = (tx + 1) * tileSize;
            int centerY = ty * tileSize + tileSize / 2;
            SmoothBoundary( image, channel, boundaryX, centerY, false, info.smoothingStrength );
         }
      }
   }
}

// ----------------------------------------------------------------------------

int TransitionChecker::CheckAndSmooth( Image& image, int channel ) const
{
   auto transitions = AnalyzeTransitions( image, channel );
   ApplySmoothing( image, transitions, channel );

   int count = 0;
   for ( const auto& row : transitions )
      for ( const auto& info : row )
         if ( info.needsSmoothing )
            count++;

   return count;
}

// ----------------------------------------------------------------------------

int TransitionChecker::CheckAndSmooth( Image& image, const Image& segmentation, int channel ) const
{
   auto transitions = AnalyzeTransitions( image, segmentation, channel );
   ApplySmoothing( image, transitions, channel );

   int count = 0;
   for ( const auto& row : transitions )
      for ( const auto& info : row )
         if ( info.needsSmoothing )
            count++;

   return count;
}

// ----------------------------------------------------------------------------

TransitionChecker::TransitionSummary TransitionChecker::GetSummary(
   const std::vector<std::vector<TransitionInfo>>& transitions )
{
   TransitionSummary summary;

   if ( transitions.empty() )
      return summary;

   float sumGradient = 0.0f;
   int count = 0;

   for ( const auto& row : transitions )
   {
      for ( const auto& info : row )
      {
         summary.totalTiles++;

         if ( info.hasHardTransition )
            summary.tilesWithHardTransition++;

         if ( info.needsSmoothing )
            summary.tilesNeedingSmoothing++;

         if ( info.alignsWithFeature )
            summary.tilesAlignedWithFeatures++;

         if ( info.maxGradient > 0 )
         {
            sumGradient += info.maxGradient;
            count++;
            summary.maxGradient = std::max( summary.maxGradient, info.maxGradient );
         }
      }
   }

   if ( count > 0 )
      summary.avgGradient = sumGradient / count;

   return summary;
}

// ----------------------------------------------------------------------------

float TransitionChecker::ComputeBoundaryGradient(
   const Image& image,
   int channel,
   int tileX1, int tileY1,
   int tileX2, int tileY2,
   bool horizontal ) const
{
   int width = image.Width();
   int height = image.Height();
   int tileSize = m_config.tileSize;

   const Image::sample* data = image.PixelData( channel );

   // Get tile boundaries
   int x1_start = tileX1 * tileSize;
   int y1_start = tileY1 * tileSize;
   int x2_start = tileX2 * tileSize;
   int y2_start = tileY2 * tileSize;

   float sum1 = 0.0f, sum2 = 0.0f;
   int count = 0;

   if ( horizontal )
   {
      // Horizontal boundary - compare bottom row of tile1 with top row of tile2
      int y1 = std::min( y1_start + tileSize - 1, height - 1 );
      int y2 = std::min( y2_start, height - 1 );

      for ( int x = x1_start; x < std::min( x1_start + tileSize, width ); ++x )
      {
         sum1 += static_cast<float>( data[y1 * width + x] );
         sum2 += static_cast<float>( data[y2 * width + x] );
         count++;
      }
   }
   else
   {
      // Vertical boundary - compare right column of tile1 with left column of tile2
      int x1 = std::min( x1_start + tileSize - 1, width - 1 );
      int x2 = std::min( x2_start, width - 1 );

      for ( int y = y1_start; y < std::min( y1_start + tileSize, height ); ++y )
      {
         sum1 += static_cast<float>( data[y * width + x1] );
         sum2 += static_cast<float>( data[y * width + x2] );
         count++;
      }
   }

   if ( count == 0 )
      return 0.0f;

   float mean1 = sum1 / count;
   float mean2 = sum2 / count;

   return std::abs( mean1 - mean2 );
}

// ----------------------------------------------------------------------------

bool TransitionChecker::CheckFeatureAlignment(
   const Image& segmentation,
   int tileX, int tileY,
   int neighborTileX, int neighborTileY ) const
{
   if ( segmentation.IsEmpty() )
      return false;

   int width = segmentation.Width();
   int height = segmentation.Height();
   int tileSize = m_config.tileSize;

   const Image::sample* data = segmentation.PixelData( 0 );

   // Sample center of each tile
   int x1 = std::min( tileX * tileSize + tileSize / 2, width - 1 );
   int y1 = std::min( tileY * tileSize + tileSize / 2, height - 1 );
   int x2 = std::min( neighborTileX * tileSize + tileSize / 2, width - 1 );
   int y2 = std::min( neighborTileY * tileSize + tileSize / 2, height - 1 );

   // Get class values (assuming segmentation is stored as normalized class index)
   int class1 = static_cast<int>( data[y1 * width + x1] * (static_cast<int>( RegionClass::Count ) - 1) + 0.5f );
   int class2 = static_cast<int>( data[y2 * width + x2] * (static_cast<int>( RegionClass::Count ) - 1) + 0.5f );

   // If classes are different, the transition is at a real feature boundary
   return (class1 != class2);
}

// ----------------------------------------------------------------------------

void TransitionChecker::SmoothBoundary(
   Image& image,
   int channel,
   int boundaryX, int boundaryY,
   bool horizontal,
   float strength ) const
{
   int width = image.Width();
   int height = image.Height();
   int radius = m_config.smoothingRadius;

   // Validate smoothing radius to prevent allocation crash
   if ( radius <= 0 || radius > 1000 )
      throw std::runtime_error( "Invalid smoothing radius: " + std::to_string( radius ) );

   Image::sample* data = image.PixelData( channel );

   // Create Gaussian weights
   std::vector<float> weights( radius * 2 + 1 );
   float sigma = radius / 2.0f;
   float sum = 0.0f;

   for ( int i = 0; i <= radius * 2; ++i )
   {
      float d = static_cast<float>( i - radius );
      weights[i] = std::exp( -0.5f * d * d / (sigma * sigma) );
      sum += weights[i];
   }

   // Normalize weights
   for ( float& w : weights )
      w /= sum;

   // Apply smoothing along the boundary
   if ( horizontal )
   {
      // Smooth horizontal boundary (along X, at Y = boundaryY)
      int tileSize = m_config.tileSize;
      int xStart = std::max( 0, boundaryX - tileSize / 2 );
      int xEnd = std::min( width, boundaryX + tileSize / 2 );

      for ( int x = xStart; x < xEnd; ++x )
      {
         float smoothed = 0.0f;
         for ( int dy = -radius; dy <= radius; ++dy )
         {
            int y = std::max( 0, std::min( height - 1, boundaryY + dy ) );
            smoothed += static_cast<float>( data[y * width + x] ) * weights[dy + radius];
         }

         // Blend original with smoothed based on strength
         int y = boundaryY;
         if ( y >= 0 && y < height )
         {
            float original = static_cast<float>( data[y * width + x] );
            data[y * width + x] = static_cast<Image::sample>( original * (1.0f - strength) + smoothed * strength );
         }
      }
   }
   else
   {
      // Smooth vertical boundary (along Y, at X = boundaryX)
      int tileSize = m_config.tileSize;
      int yStart = std::max( 0, boundaryY - tileSize / 2 );
      int yEnd = std::min( height, boundaryY + tileSize / 2 );

      for ( int y = yStart; y < yEnd; ++y )
      {
         float smoothed = 0.0f;
         for ( int dx = -radius; dx <= radius; ++dx )
         {
            int x = std::max( 0, std::min( width - 1, boundaryX + dx ) );
            smoothed += static_cast<float>( data[y * width + x] ) * weights[dx + radius];
         }

         // Blend original with smoothed based on strength
         int x = boundaryX;
         if ( x >= 0 && x < width )
         {
            float original = static_cast<float>( data[y * width + x] );
            data[y * width + x] = static_cast<Image::sample>( original * (1.0f - strength) + smoothed * strength );
         }
      }
   }
}

// ----------------------------------------------------------------------------

} // namespace pcl
