//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "BlendEngine.h"

#include <algorithm>
#include <cmath>
#include <thread>
#include <vector>

namespace pcl
{

// ----------------------------------------------------------------------------
// BlendEngine Implementation
// ----------------------------------------------------------------------------

BlendEngine::BlendEngine( const BlendConfig& config )
   : m_config( config )
{
}

// ----------------------------------------------------------------------------

Image BlendEngine::Blend( const Image& original,
                           const std::vector<RegionStretchResult>& stretchedRegions ) const
{
   if ( stretchedRegions.empty() )
      return original;

   int width = original.Width();
   int height = original.Height();
   int numChannels = original.NumberOfNominalChannels();

   Image output( width, height, original.ColorSpace() );

   for ( int c = 0; c < numChannels; ++c )
   {
      for ( int y = 0; y < height; ++y )
      {
         for ( int x = 0; x < width; ++x )
         {
            double origValue = original( x, y, c );

            // Collect stretched values and weights
            std::vector<std::pair<double, double>> stretchedWeights;

            for ( const auto& region : stretchedRegions )
            {
               double weight = 0;
               if ( x < region.mask.Width() && y < region.mask.Height() )
               {
                  weight = region.mask( x, y, 0 );
               }

               if ( weight > 0.001 )
               {
                  // Apply region strength multiplier
                  weight *= m_config.GetRegionStrength( region.region );

                  double stretchedValue = origValue;
                  if ( x < region.stretchedImage.Width() &&
                       y < region.stretchedImage.Height() &&
                       c < region.stretchedImage.NumberOfChannels() )
                  {
                     stretchedValue = region.stretchedImage( x, y, c );
                  }

                  stretchedWeights.push_back( { stretchedValue, weight } );
               }
            }

            double blended = BlendPixel( origValue, stretchedWeights );

            // Clamp if configured
            if ( m_config.clampOutput )
            {
               blended = std::max( 0.0, std::min( 1.0, blended ) );
            }

            output( x, y, c ) = blended;
         }
      }
   }

   return output;
}

// ----------------------------------------------------------------------------

Image BlendEngine::BlendWithAlgorithms(
   const Image& original,
   const std::map<RegionClass, Image>& masks,
   const std::map<RegionClass, IStretchAlgorithm*>& algorithms ) const
{
   if ( masks.empty() || algorithms.empty() )
      return original;

   int width = original.Width();
   int height = original.Height();
   int numChannels = original.NumberOfNominalChannels();

   Image output( width, height, original.ColorSpace() );

   // Prepare masks with feathering
   auto preparedMasks = PrepareMasks( masks );

   for ( int c = 0; c < numChannels; ++c )
   {
      for ( int y = 0; y < height; ++y )
      {
         for ( int x = 0; x < width; ++x )
         {
            double origValue = original( x, y, c );

            // Skip black point preservation if configured
            if ( m_config.preserveBlackPoint && origValue <= 0.0 )
            {
               output( x, y, c ) = 0.0;
               continue;
            }

            double blendedValue = 0;
            double totalWeight = 0;

            for ( const auto& maskPair : preparedMasks )
            {
               RegionClass region = maskPair.first;
               const Image& mask = maskPair.second;

               double weight = 0;
               if ( x < mask.Width() && y < mask.Height() )
               {
                  weight = mask( x, y, 0 );
               }

               if ( weight > 0.001 )
               {
                  // Apply region strength
                  weight *= m_config.GetRegionStrength( region );

                  // Get stretched value from algorithm
                  double stretchedValue = origValue;
                  auto algoIt = algorithms.find( region );
                  if ( algoIt != algorithms.end() && algoIt->second )
                  {
                     stretchedValue = algoIt->second->Apply( origValue );
                  }

                  blendedValue += stretchedValue * weight;
                  totalWeight += weight;
               }
            }

            // Normalize if we have weights
            if ( totalWeight > 0.001 )
            {
               blendedValue /= totalWeight;
            }
            else
            {
               blendedValue = origValue;  // No stretch if no mask coverage
            }

            // Clamp
            if ( m_config.clampOutput )
            {
               blendedValue = std::max( 0.0, std::min( 1.0, blendedValue ) );
            }

            output( x, y, c ) = blendedValue;
         }
      }
   }

   return output;
}

// ----------------------------------------------------------------------------

std::map<RegionClass, Image> BlendEngine::PrepareMasks(
   const std::map<RegionClass, Image>& rawMasks ) const
{
   std::map<RegionClass, Image> prepared;

   for ( const auto& pair : rawMasks )
   {
      prepared[pair.first] = FeatherMask( pair.second );
   }

   // Normalize if configured
   if ( m_config.normalizeWeights )
   {
      MaskPrep::NormalizeMasks( prepared );
   }

   return prepared;
}

// ----------------------------------------------------------------------------

Image BlendEngine::FeatherMask( const Image& mask ) const
{
   if ( m_config.featherRadius <= 0 )
      return mask;

   // Apply Gaussian blur
   Image feathered = MaskPrep::GaussianFeather( mask, m_config.featherRadius );

   // Apply falloff exponent
   if ( std::abs( m_config.blendFalloff - 1.0 ) > 0.001 )
   {
      feathered = MaskPrep::PowerFalloff( feathered, m_config.blendFalloff );
   }

   return feathered;
}

// ----------------------------------------------------------------------------

Image BlendEngine::ComputeWeightMap( const std::map<RegionClass, Image>& masks,
                                      RegionClass forRegion ) const
{
   auto it = masks.find( forRegion );
   if ( it == masks.end() )
      return Image();

   const Image& targetMask = it->second;
   int width = targetMask.Width();
   int height = targetMask.Height();

   Image weights( width, height, pcl::ColorSpace::Gray );

   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         double targetWeight = targetMask( x, y, 0 ) *
                               m_config.GetRegionStrength( forRegion );
         double totalWeight = 0;

         for ( const auto& pair : masks )
         {
            if ( x < pair.second.Width() && y < pair.second.Height() )
            {
               totalWeight += pair.second( x, y, 0 ) *
                              m_config.GetRegionStrength( pair.first );
            }
         }

         weights( x, y, 0 ) = totalWeight > 0 ? targetWeight / totalWeight : 0;
      }
   }

   return weights;
}

// ----------------------------------------------------------------------------

double BlendEngine::BlendPixel( double original,
   const std::vector<std::pair<double, double>>& stretchedWeights ) const
{
   if ( stretchedWeights.empty() )
      return original;

   // Preserve black point
   if ( m_config.preserveBlackPoint && original <= 0.0 )
      return 0.0;

   double sum = 0;
   double totalWeight = 0;

   for ( const auto& sw : stretchedWeights )
   {
      sum += sw.first * sw.second;
      totalWeight += sw.second;
   }

   if ( m_config.normalizeWeights && totalWeight > 0 )
   {
      return sum / totalWeight;
   }
   else if ( totalWeight > 0 )
   {
      // Blend with original for uncovered areas
      double covered = std::min( 1.0, totalWeight );
      return (sum / totalWeight) * covered + original * (1.0 - covered);
   }

   return original;
}

// ----------------------------------------------------------------------------
// ParallelBlendProcessor Implementation
// ----------------------------------------------------------------------------

ParallelBlendProcessor::ParallelBlendProcessor( const BlendConfig& config )
   : m_config( config )
{
}

// ----------------------------------------------------------------------------

Image ParallelBlendProcessor::Process(
   const Image& original,
   const std::map<RegionClass, Image>& masks,
   const std::map<RegionClass, IStretchAlgorithm*>& algorithms,
   int numThreads ) const
{
   return Process( original, masks, algorithms, nullptr, numThreads );
}

// ----------------------------------------------------------------------------

Image ParallelBlendProcessor::Process(
   const Image& original,
   const std::map<RegionClass, Image>& masks,
   const std::map<RegionClass, IStretchAlgorithm*>& algorithms,
   ProgressCallback progress,
   int numThreads ) const
{
   if ( masks.empty() )
      return original;

   int width = original.Width();
   int height = original.Height();
   int numChannels = original.NumberOfNominalChannels();

   Image output( width, height, original.ColorSpace() );

   // Determine thread count
   if ( numThreads <= 0 )
   {
      numThreads = std::max( 1, static_cast<int>( std::thread::hardware_concurrency() ) );
   }

   // Prepare masks
   BlendEngine blendEngine( m_config );
   auto preparedMasks = blendEngine.PrepareMasks( masks );

   int rowsPerThread = height / numThreads;
   int totalWork = height * numChannels;
   int workDone = 0;

   for ( int c = 0; c < numChannels; ++c )
   {
      std::vector<std::thread> threads;

      for ( int t = 0; t < numThreads; ++t )
      {
         int startRow = t * rowsPerThread;
         int endRow = (t == numThreads - 1) ? height : (t + 1) * rowsPerThread;

         threads.emplace_back( [this, &original, &output, &preparedMasks, &algorithms,
                                 startRow, endRow, c]() {
            ProcessRows( original, output, preparedMasks, algorithms,
                         startRow, endRow, c );
         } );
      }

      for ( auto& thread : threads )
      {
         thread.join();
      }

      workDone += height;
      if ( progress )
      {
         progress( static_cast<double>( workDone ) / totalWork,
                   String().Format( "Processing channel %d/%d", c + 1, numChannels ) );
      }
   }

   return output;
}

// ----------------------------------------------------------------------------

void ParallelBlendProcessor::ProcessRows(
   const Image& original,
   Image& output,
   const std::map<RegionClass, Image>& masks,
   const std::map<RegionClass, IStretchAlgorithm*>& algorithms,
   int startRow, int endRow,
   int channel ) const
{
   int width = original.Width();

   for ( int y = startRow; y < endRow; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         double origValue = original( x, y, channel );

         // Preserve black point
         if ( m_config.preserveBlackPoint && origValue <= 0.0 )
         {
            output( x, y, channel ) = 0.0;
            continue;
         }

         double blendedValue = 0;
         double totalWeight = 0;

         for ( const auto& maskPair : masks )
         {
            double weight = 0;
            if ( x < maskPair.second.Width() && y < maskPair.second.Height() )
            {
               weight = maskPair.second( x, y, 0 );
            }

            if ( weight > 0.001 )
            {
               weight *= m_config.GetRegionStrength( maskPair.first );

               double stretched = origValue;
               auto algoIt = algorithms.find( maskPair.first );
               if ( algoIt != algorithms.end() && algoIt->second )
               {
                  stretched = algoIt->second->Apply( origValue );
               }

               blendedValue += stretched * weight;
               totalWeight += weight;
            }
         }

         if ( totalWeight > 0.001 )
         {
            blendedValue /= totalWeight;
         }
         else
         {
            blendedValue = origValue;
         }

         if ( m_config.clampOutput )
         {
            blendedValue = std::max( 0.0, std::min( 1.0, blendedValue ) );
         }

         output( x, y, channel ) = blendedValue;
      }
   }
}

// ----------------------------------------------------------------------------
// MaskPrep Implementation
// ----------------------------------------------------------------------------

namespace MaskPrep
{

Image GaussianFeather( const Image& mask, double sigma )
{
   if ( sigma <= 0 )
      return mask;

   int width = mask.Width();
   int height = mask.Height();
   int radius = static_cast<int>( sigma * 3 + 0.5 );

   if ( radius < 1 )
      return mask;

   Image temp( width, height, pcl::ColorSpace::Gray );
   Image result( width, height, pcl::ColorSpace::Gray );

   // Horizontal pass
   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         double sum = 0;
         double weightSum = 0;

         for ( int dx = -radius; dx <= radius; ++dx )
         {
            int sx = x + dx;
            if ( sx >= 0 && sx < width )
            {
               double weight = std::exp( -(dx * dx) / (2 * sigma * sigma) );
               sum += mask( sx, y, 0 ) * weight;
               weightSum += weight;
            }
         }

         temp( x, y, 0 ) = weightSum > 0 ? sum / weightSum : 0;
      }
   }

   // Vertical pass
   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         double sum = 0;
         double weightSum = 0;

         for ( int dy = -radius; dy <= radius; ++dy )
         {
            int sy = y + dy;
            if ( sy >= 0 && sy < height )
            {
               double weight = std::exp( -(dy * dy) / (2 * sigma * sigma) );
               sum += temp( x, sy, 0 ) * weight;
               weightSum += weight;
            }
         }

         result( x, y, 0 ) = weightSum > 0 ? sum / weightSum : 0;
      }
   }

   return result;
}

// ----------------------------------------------------------------------------

Image PowerFalloff( const Image& mask, double exponent )
{
   if ( std::abs( exponent - 1.0 ) < 0.001 )
      return mask;

   int width = mask.Width();
   int height = mask.Height();

   Image result( width, height, pcl::ColorSpace::Gray );

   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         double v = mask( x, y, 0 );
         result( x, y, 0 ) = std::pow( v, exponent );
      }
   }

   return result;
}

// ----------------------------------------------------------------------------

void NormalizeMasks( std::map<RegionClass, Image>& masks )
{
   if ( masks.empty() )
      return;

   // Determine dimensions from first mask
   int width = masks.begin()->second.Width();
   int height = masks.begin()->second.Height();

   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         double sum = 0;

         for ( auto& pair : masks )
         {
            if ( x < pair.second.Width() && y < pair.second.Height() )
            {
               sum += pair.second( x, y, 0 );
            }
         }

         if ( sum > 0.001 )
         {
            for ( auto& pair : masks )
            {
               if ( x < pair.second.Width() && y < pair.second.Height() )
               {
                  pair.second( x, y, 0 ) /= sum;
               }
            }
         }
      }
   }
}

// ----------------------------------------------------------------------------

void FillMaskGaps( std::map<RegionClass, Image>& masks, RegionClass defaultRegion )
{
   if ( masks.empty() )
      return;

   int width = masks.begin()->second.Width();
   int height = masks.begin()->second.Height();

   // Ensure default region mask exists
   if ( masks.find( defaultRegion ) == masks.end() )
   {
      masks[defaultRegion].AllocateData( width, height, 1, ColorSpace::Gray );
      masks[defaultRegion].Zero();
   }

   Image& defaultMask = masks[defaultRegion];

   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         double sum = 0;

         for ( const auto& pair : masks )
         {
            if ( x < pair.second.Width() && y < pair.second.Height() )
            {
               sum += pair.second( x, y, 0 );
            }
         }

         // If no mask coverage, assign to default region
         if ( sum < 0.001 )
         {
            defaultMask( x, y, 0 ) = 1.0;
         }
         else if ( sum < 1.0 )
         {
            // Partial coverage - fill gap with default
            defaultMask( x, y, 0 ) = std::max( double( defaultMask( x, y, 0 ) ), 1.0 - sum );
         }
      }
   }
}

// ----------------------------------------------------------------------------

Image DetectTransitionZone( const std::map<RegionClass, Image>& masks )
{
   if ( masks.empty() )
      return Image();

   int width = masks.begin()->second.Width();
   int height = masks.begin()->second.Height();

   Image transition( width, height, pcl::ColorSpace::Gray );
   transition.Zero();

   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         // Count significant masks at this pixel
         int significantCount = 0;
         double maxWeight = 0;

         for ( const auto& pair : masks )
         {
            double w = 0;
            if ( x < pair.second.Width() && y < pair.second.Height() )
            {
               w = pair.second( x, y, 0 );
            }

            if ( w > 0.1 )
            {
               ++significantCount;
               maxWeight = std::max( maxWeight, w );
            }
         }

         // Transition zone: multiple significant masks or no dominant mask
         if ( significantCount > 1 || maxWeight < 0.8 )
         {
            transition( x, y, 0 ) = 1.0 - maxWeight;
         }
      }
   }

   return transition;
}

} // namespace MaskPrep

// ----------------------------------------------------------------------------

} // namespace pcl
