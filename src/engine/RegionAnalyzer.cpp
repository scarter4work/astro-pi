//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "RegionAnalyzer.h"

#include <algorithm>
#include <cmath>

namespace pcl
{

// ----------------------------------------------------------------------------
// RegionAnalysisResult Implementation
// ----------------------------------------------------------------------------

String RegionAnalysisResult::ToString() const
{
   String result;
   // Use IsoString for region name to ensure proper UTF-8 encoding with %s
   IsoString dominantRegionName = IsoString( RegionClassDisplayName( dominantRegion ) );
   result += String().Format(
      "Image Analysis Result\n"
      "=====================\n"
      "Size: %dx%d (%zu pixels)\n"
      "Channels: %d (%s)\n"
      "Dominant Region: %s\n"
      "\nQuality Metrics:\n"
      "  Overall SNR: %.1f\n"
      "  Dynamic Range: %.2f (%.1f dB)\n"
      "  Clipping: %.2f%%\n",
      imageWidth, imageHeight, totalPixels,
      numChannels, isColor ? "Color" : "Grayscale",
      dominantRegionName.c_str(),
      overallSNR,
      overallDynamicRange, overallDynamicRange * 20.0,
      overallClipping
   );

   result += "\nRegion Coverage:\n";
   for ( const auto& pair : regionCoverage )
   {
      if ( pair.second > 0.001 )  // Only show regions with >0.1% coverage
      {
         IsoString regionName = IsoString( RegionClassDisplayName( pair.first ) );
         result += String().Format( "  %s: %.1f%%\n",
                                     regionName.c_str(),
                                     pair.second * 100.0 );
      }
   }

   result += "\nRegion Statistics:\n";
   for ( const auto& pair : regionStats )
   {
      IsoString regionName = IsoString( RegionClassDisplayName( pair.first ) );
      result += String().Format( "\n%s:\n", regionName.c_str() );
      result += String().Format( "  Median: %.6f, Mean: %.6f\n",
                                  pair.second.median, pair.second.mean );
      result += String().Format( "  Range: [%.6f, %.6f]\n",
                                  pair.second.min, pair.second.max );
      result += String().Format( "  StdDev: %.6f, MAD: %.6f\n",
                                  pair.second.stdDev, pair.second.mad );
      result += String().Format( "  SNR: %.1f\n", pair.second.snrEstimate );
   }

   return result;
}

// ----------------------------------------------------------------------------
// RegionAnalyzer Implementation
// ----------------------------------------------------------------------------

RegionAnalyzer::RegionAnalyzer( const RegionAnalyzerConfig& config )
   : m_config( config )
   , m_histogramEngine( config.histogramConfig )
{
}

// ----------------------------------------------------------------------------

RegionAnalysisResult RegionAnalyzer::Analyze( const Image& image ) const
{
   RegionAnalysisResult result;

   // Image metadata
   result.imageWidth = image.Width();
   result.imageHeight = image.Height();
   result.numChannels = image.NumberOfNominalChannels();
   result.isColor = result.numChannels >= 3;
   result.totalPixels = size_t( result.imageWidth ) * result.imageHeight;

   // Compute global statistics
   result.globalStats = m_histogramEngine.ComputeChannelStatistics( image, nullptr );

   // Store luminance as background region (default when no masks)
   RegionStatistics bgStats = result.globalStats.luminance;
   bgStats.regionClass = RegionClass::Background;
   result.regionStats[RegionClass::Background] = bgStats;
   result.regionCoverage[RegionClass::Background] = 1.0;

   // Determine dominant region from classification
   result.dominantRegion = ClassifyRegion( bgStats );

   // Compute quality metrics
   ComputeQualityMetrics( result );

   return result;
}

// ----------------------------------------------------------------------------

RegionAnalysisResult RegionAnalyzer::Analyze( const Image& image, const Image& mask,
                                               RegionClass regionClass ) const
{
   RegionAnalysisResult result;

   // Image metadata
   result.imageWidth = image.Width();
   result.imageHeight = image.Height();
   result.numChannels = image.NumberOfNominalChannels();
   result.isColor = result.numChannels >= 3;
   result.totalPixels = size_t( result.imageWidth ) * result.imageHeight;

   // Compute global statistics (without mask)
   result.globalStats = m_histogramEngine.ComputeChannelStatistics( image, nullptr );

   // Compute region statistics with mask
   ChannelStatistics regionChannelStats = m_histogramEngine.ComputeChannelStatistics( image, &mask );

   RegionStatistics stats = regionChannelStats.luminance;
   stats.regionClass = regionClass;
   result.regionStats[regionClass] = stats;

   // Compute coverage
   double coverage = RegionMaskUtils::ComputeCoverage( mask );
   result.regionCoverage[regionClass] = coverage;

   // Also compute complement (everything not in the mask)
   if ( coverage < 0.99 )
   {
      Image inverseMask = RegionMaskUtils::InvertMask( mask );
      ChannelStatistics complementStats = m_histogramEngine.ComputeChannelStatistics( image, &inverseMask );

      RegionClass complementClass = RegionClass::Background;
      if ( regionClass == RegionClass::Background )
         complementClass = RegionClass::NebulaDark;  // Default for non-background

      RegionStatistics compStats = complementStats.luminance;
      compStats.regionClass = complementClass;
      result.regionStats[complementClass] = compStats;
      result.regionCoverage[complementClass] = 1.0 - coverage;
   }

   // Determine dominant region
   result.dominantRegion = FindDominantRegion( result.regionCoverage, result.regionStats );

   // Compute quality metrics
   ComputeQualityMetrics( result );

   return result;
}

// ----------------------------------------------------------------------------

RegionAnalysisResult RegionAnalyzer::Analyze( const Image& image,
                                               const std::map<RegionClass, Image>& masks ) const
{
   RegionAnalysisResult result;

   // Image metadata
   result.imageWidth = image.Width();
   result.imageHeight = image.Height();
   result.numChannels = image.NumberOfNominalChannels();
   result.isColor = result.numChannels >= 3;
   result.totalPixels = size_t( result.imageWidth ) * result.imageHeight;

   // Compute global statistics
   result.globalStats = m_histogramEngine.ComputeChannelStatistics( image, nullptr );

   // Analyze each region
   for ( const auto& pair : masks )
   {
      RegionClass rc = pair.first;
      const Image& mask = pair.second;

      // Compute statistics for this region
      ChannelStatistics channelStats = m_histogramEngine.ComputeChannelStatistics( image, &mask );

      RegionStatistics stats = channelStats.luminance;
      stats.regionClass = rc;

      // If color, also store per-channel info in the stats
      if ( result.isColor )
      {
         // Could extend RegionStatistics to hold per-channel data if needed
      }

      result.regionStats[rc] = stats;
      result.regionCoverage[rc] = RegionMaskUtils::ComputeCoverage( mask );
   }

   // Determine dominant region
   result.dominantRegion = FindDominantRegion( result.regionCoverage, result.regionStats );

   // Compute quality metrics
   ComputeQualityMetrics( result );

   return result;
}

// ----------------------------------------------------------------------------

RegionAnalysisResult RegionAnalyzer::AnalyzeWithSegmentation( const Image& image,
                                                               const Image& segmentationMask ) const
{
   // Convert multi-channel segmentation mask to individual region masks
   std::map<RegionClass, Image> masks;

   int numSegChannels = segmentationMask.NumberOfNominalChannels();

   for ( int c = 0; c < numSegChannels && c < static_cast<int>( RegionClass::Count ); ++c )
   {
      RegionClass rc = ChannelToRegionClass( c );

      // Extract single channel as mask
      Image channelMask( segmentationMask.Width(), segmentationMask.Height(), pcl::ColorSpace::Gray );

      for ( int y = 0; y < segmentationMask.Height(); ++y )
      {
         for ( int x = 0; x < segmentationMask.Width(); ++x )
         {
            channelMask( x, y, 0 ) = segmentationMask( x, y, c );
         }
      }

      // Only add if there's meaningful coverage
      if ( RegionMaskUtils::ComputeCoverage( channelMask ) > 0.001 )
      {
         masks[rc] = std::move( channelMask );
      }
   }

   return Analyze( image, masks );
}

// ----------------------------------------------------------------------------

RegionAnalysisResult RegionAnalyzer::QuickAnalyze( const Image& image,
                                                    const Image* mask ) const
{
   RegionAnalysisResult result;

   // Image metadata
   result.imageWidth = image.Width();
   result.imageHeight = image.Height();
   result.numChannels = image.NumberOfNominalChannels();
   result.isColor = result.numChannels >= 3;
   result.totalPixels = size_t( result.imageWidth ) * result.imageHeight;

   // Quick stats (no histogram)
   double min, max, mean, stdDev;
   m_histogramEngine.ComputeQuickStats( image, mask, 0, 0, min, max, mean, stdDev );

   RegionStatistics stats;
   stats.min = min;
   stats.max = max;
   stats.mean = mean;
   stats.stdDev = stdDev;
   stats.median = mean;  // Approximation
   stats.mad = stdDev * 0.6745;  // Approximation
   stats.regionClass = RegionClass::Background;

   // Dynamic range
   if ( min > 1e-10 )
   {
      stats.dynamicRange = std::log10( max / min );
   }
   else
   {
      stats.dynamicRange = std::log10( max / 1e-10 );
   }

   // Rough SNR estimate
   stats.noiseEstimate = stats.mad > 0 ? stats.mad * 1.4826 : stdDev;
   stats.signalEstimate = mean;
   stats.snrEstimate = stats.signalEstimate / std::max( 0.001, stats.noiseEstimate );

   result.regionStats[RegionClass::Background] = stats;
   result.regionCoverage[RegionClass::Background] = mask ? RegionMaskUtils::ComputeCoverage( *mask ) : 1.0;

   // Set dominant region
   result.dominantRegion = ClassifyRegion( stats );

   // Quality metrics
   result.overallSNR = stats.snrEstimate;
   result.overallDynamicRange = stats.dynamicRange;
   result.overallClipping = 0;  // Not computed in quick mode

   return result;
}

// ----------------------------------------------------------------------------

RegionClass RegionAnalyzer::ClassifyRegion( const RegionStatistics& stats ) const
{
   double median = stats.median;
   double snr = stats.snrEstimate;

   // Very bright regions
   if ( median > m_config.starCoreThreshold )
   {
      return RegionClass::StarBright;
   }

   // Bright regions
   if ( median > m_config.brightThreshold )
   {
      // Distinguish between different bright region types
      if ( stats.peakWidth < 0.05 )
      {
         // Narrow peak suggests stellar
         return RegionClass::StarMedium;
      }
      else if ( stats.skewness > 0.5 )
      {
         // Positive skew suggests bright nebula or galaxy core
         return snr > m_config.highSNRThreshold ? RegionClass::GalaxyCore : RegionClass::NebulaEmission;
      }
      else
      {
         return RegionClass::NebulaEmission;
      }
   }

   // Background-level regions
   if ( median < m_config.backgroundThreshold )
   {
      // Check if it's actually a dark feature
      if ( stats.min < 0.01 && stats.Range() > 0.1 )
      {
         return RegionClass::DustLane;
      }
      return RegionClass::Background;
   }

   // Mid-tones: could be faint nebula, galaxy halo, or galaxy arm
   if ( snr < m_config.lowSNRThreshold )
   {
      // Low SNR suggests faint nebulosity or IFN
      return RegionClass::NebulaDark;
   }
   else if ( stats.skewness < -0.3 )
   {
      // Negative skew suggests galaxy halo
      return RegionClass::GalaxyElliptical;
   }
   else
   {
      // Default to galaxy arm for structured mid-tone regions
      return RegionClass::GalaxySpiral;
   }
}

// ----------------------------------------------------------------------------

IsoString RegionAnalyzer::GetRecommendedAlgorithm( RegionClass regionClass,
                                                    const RegionStatistics& stats ) const
{
   // These recommendations match the algorithm strengths from the spec
   switch ( regionClass )
   {
   case RegionClass::Background:
      // Background needs gentle stretch that preserves noise characteristics
      return stats.snrEstimate > 10 ? "SAS" : "MTF";

   case RegionClass::StarBright:
   case RegionClass::StarSaturated:
      // Bright stars need HDR-friendly stretch to prevent bloating
      return "ArcSinh";

   case RegionClass::StarMedium:
   case RegionClass::StarFaint:
      // Star halos need controlled expansion
      return "GHS";

   case RegionClass::NebulaEmission:
   case RegionClass::NebulaReflection:
   case RegionClass::NebulaPlanetary:
      // Bright nebula benefits from color-preserving stretch
      return stats.snrEstimate > 15 ? "RNC" : "GHS";

   case RegionClass::NebulaDark:
      // Dark nebula needs aggressive but noise-aware stretch
      return "Log";

   case RegionClass::DustLane:
      // Dust lanes need to maintain dark detail
      return "Histogram";

   case RegionClass::GalaxyCore:
      // Galaxy cores are similar to star cores
      return "ArcSinh";

   case RegionClass::GalaxyElliptical:
   case RegionClass::GalaxyIrregular:
      // Galaxy halos are like faint nebula
      return "SAS";

   case RegionClass::GalaxySpiral:
      // Galaxy spiral arms need balanced stretch
      return "GHS";

   case RegionClass::StarClusterOpen:
   case RegionClass::StarClusterGlobular:
      // Star clusters need balanced stretch
      return "GHS";

   case RegionClass::ArtifactHotPixel:
   case RegionClass::ArtifactSatellite:
   case RegionClass::ArtifactDiffraction:
   case RegionClass::ArtifactGradient:
   case RegionClass::ArtifactNoise:
      // Artifacts should be handled carefully
      return "MTF";

   default:
      return "MTF";
   }
}

// ----------------------------------------------------------------------------

RegionClass RegionAnalyzer::FindDominantRegion( const std::map<RegionClass, double>& coverage,
                                                  const std::map<RegionClass, RegionStatistics>& stats ) const
{
   if ( !m_config.detectDominantRegion )
   {
      return RegionClass::Background;
   }

   // Find region with highest priority-weighted coverage
   double maxScore = 0;
   RegionClass dominant = RegionClass::Background;

   for ( const auto& pair : coverage )
   {
      RegionClass rc = pair.first;
      double cov = pair.second;

      // Get statistics if available
      auto statsIt = stats.find( rc );
      const RegionStatistics& regionStats = (statsIt != stats.end()) ? statsIt->second : RegionStatistics();

      // Compute priority score
      double priority = GetRegionPriority( rc, regionStats );
      double score = cov * priority;

      if ( score > maxScore )
      {
         maxScore = score;
         dominant = rc;
      }
   }

   return dominant;
}

// ----------------------------------------------------------------------------

void RegionAnalyzer::ComputeQualityMetrics( RegionAnalysisResult& result ) const
{
   // Aggregate SNR from all regions (weighted by coverage)
   double snrSum = 0;
   double drSum = 0;
   double clipSum = 0;
   double totalWeight = 0;

   for ( const auto& pair : result.regionStats )
   {
      double weight = 1.0;
      auto covIt = result.regionCoverage.find( pair.first );
      if ( covIt != result.regionCoverage.end() )
      {
         weight = covIt->second;
      }

      snrSum += pair.second.snrEstimate * weight;
      drSum += pair.second.dynamicRange * weight;
      clipSum += pair.second.clippingPct * weight;
      totalWeight += weight;
   }

   if ( totalWeight > 0 )
   {
      result.overallSNR = snrSum / totalWeight;
      result.overallDynamicRange = drSum / totalWeight;
      result.overallClipping = clipSum / totalWeight;
   }
   else
   {
      result.overallSNR = result.globalStats.luminance.snrEstimate;
      result.overallDynamicRange = result.globalStats.luminance.dynamicRange;
      result.overallClipping = result.globalStats.luminance.clippingPct;
   }
}

// ----------------------------------------------------------------------------

RegionClass RegionAnalyzer::ChannelToRegionClass( int channel )
{
   // Standard mapping from segmentation network output channels
   // This should match the model's output format (21 classes)
   static const RegionClass channelMap[] = {
      RegionClass::Background,          // 0
      RegionClass::StarBright,          // 1
      RegionClass::StarMedium,          // 2
      RegionClass::StarFaint,           // 3
      RegionClass::StarSaturated,       // 4
      RegionClass::NebulaEmission,      // 5
      RegionClass::NebulaReflection,    // 6
      RegionClass::NebulaDark,          // 7
      RegionClass::NebulaPlanetary,     // 8
      RegionClass::GalaxySpiral,        // 9
      RegionClass::GalaxyElliptical,    // 10
      RegionClass::GalaxyIrregular,     // 11
      RegionClass::GalaxyCore,          // 12
      RegionClass::DustLane,            // 13
      RegionClass::StarClusterOpen,     // 14
      RegionClass::StarClusterGlobular, // 15
      RegionClass::ArtifactHotPixel,    // 16
      RegionClass::ArtifactSatellite,   // 17
      RegionClass::ArtifactDiffraction, // 18
      RegionClass::ArtifactGradient,    // 19
      RegionClass::ArtifactNoise        // 20
   };

   if ( channel >= 0 && channel < static_cast<int>( sizeof( channelMap ) / sizeof( channelMap[0] ) ) )
   {
      return channelMap[channel];
   }

   return RegionClass::Background;
}

// ----------------------------------------------------------------------------

double RegionAnalyzer::GetRegionPriority( RegionClass rc, const RegionStatistics& stats )
{
   // Priority weights for determining dominance
   // Higher priority regions are considered more "important" even at lower coverage
   switch ( rc )
   {
   case RegionClass::StarBright:          return 3.0;
   case RegionClass::StarSaturated:       return 3.0;
   case RegionClass::GalaxyCore:          return 2.5;
   case RegionClass::NebulaPlanetary:     return 2.2;
   case RegionClass::NebulaEmission:      return 2.0;
   case RegionClass::NebulaReflection:    return 2.0;
   case RegionClass::StarMedium:          return 1.5;
   case RegionClass::GalaxySpiral:        return 1.5;
   case RegionClass::StarClusterGlobular: return 1.5;
   case RegionClass::StarClusterOpen:     return 1.4;
   case RegionClass::StarFaint:           return 1.3;
   case RegionClass::NebulaDark:          return 1.2;
   case RegionClass::GalaxyElliptical:    return 1.2;
   case RegionClass::GalaxyIrregular:     return 1.2;
   case RegionClass::DustLane:            return 1.0;
   case RegionClass::Background:          return 0.5;
   // Artifacts have low priority
   case RegionClass::ArtifactHotPixel:    return 0.2;
   case RegionClass::ArtifactSatellite:   return 0.3;
   case RegionClass::ArtifactDiffraction: return 0.3;
   case RegionClass::ArtifactGradient:    return 0.3;
   case RegionClass::ArtifactNoise:       return 0.2;
   default:                               return 1.0;
   }
}

// ----------------------------------------------------------------------------
// RegionMaskUtils Implementation
// ----------------------------------------------------------------------------

Image RegionMaskUtils::CreateThresholdMask( const Image& image, int channel,
                                             double lowThreshold, double highThreshold )
{
   int width = image.Width();
   int height = image.Height();

   Image mask( width, height, pcl::ColorSpace::Gray );

   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         double value = image( x, y, channel );
         mask( x, y, 0 ) = (value >= lowThreshold && value <= highThreshold) ? 1.0 : 0.0;
      }
   }

   return mask;
}

// ----------------------------------------------------------------------------

Image RegionMaskUtils::InvertMask( const Image& mask )
{
   Image result( mask.Width(), mask.Height(), pcl::ColorSpace::Gray );

   for ( int y = 0; y < mask.Height(); ++y )
   {
      for ( int x = 0; x < mask.Width(); ++x )
      {
         result( x, y, 0 ) = 1.0 - mask( x, y, 0 );
      }
   }

   return result;
}

// ----------------------------------------------------------------------------

Image RegionMaskUtils::MultiplyMasks( const Image& mask1, const Image& mask2 )
{
   int width = std::min( mask1.Width(), mask2.Width() );
   int height = std::min( mask1.Height(), mask2.Height() );

   Image result( width, height, pcl::ColorSpace::Gray );

   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         result( x, y, 0 ) = mask1( x, y, 0 ) * mask2( x, y, 0 );
      }
   }

   return result;
}

// ----------------------------------------------------------------------------

Image RegionMaskUtils::AddMasks( const Image& mask1, const Image& mask2 )
{
   int width = std::min( mask1.Width(), mask2.Width() );
   int height = std::min( mask1.Height(), mask2.Height() );

   Image result( width, height, pcl::ColorSpace::Gray );

   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         result( x, y, 0 ) = std::min( 1.0, double( mask1( x, y, 0 ) + mask2( x, y, 0 ) ) );
      }
   }

   return result;
}

// ----------------------------------------------------------------------------

Image RegionMaskUtils::SubtractMasks( const Image& mask1, const Image& mask2 )
{
   int width = std::min( mask1.Width(), mask2.Width() );
   int height = std::min( mask1.Height(), mask2.Height() );

   Image result( width, height, pcl::ColorSpace::Gray );

   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         result( x, y, 0 ) = std::max( 0.0, double( mask1( x, y, 0 ) - mask2( x, y, 0 ) ) );
      }
   }

   return result;
}

// ----------------------------------------------------------------------------

Image RegionMaskUtils::SmoothMask( const Image& mask, double sigma )
{
   // Simple Gaussian blur approximation using box blur iterations
   // For production, should use proper Gaussian convolution

   if ( sigma < 0.5 )
      return mask;

   int radius = static_cast<int>( sigma * 2 + 0.5 );
   if ( radius < 1 ) radius = 1;

   int width = mask.Width();
   int height = mask.Height();

   Image temp( width, height, pcl::ColorSpace::Gray );
   Image result( width, height, pcl::ColorSpace::Gray );

   // Copy to temp
   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         temp( x, y, 0 ) = mask( x, y, 0 );
      }
   }

   // Horizontal blur
   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         double sum = 0;
         int count = 0;

         for ( int dx = -radius; dx <= radius; ++dx )
         {
            int sx = x + dx;
            if ( sx >= 0 && sx < width )
            {
               sum += temp( sx, y, 0 );
               ++count;
            }
         }

         result( x, y, 0 ) = sum / count;
      }
   }

   // Vertical blur
   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         double sum = 0;
         int count = 0;

         for ( int dy = -radius; dy <= radius; ++dy )
         {
            int sy = y + dy;
            if ( sy >= 0 && sy < height )
            {
               sum += result( x, sy, 0 );
               ++count;
            }
         }

         temp( x, y, 0 ) = sum / count;
      }
   }

   return temp;
}

// ----------------------------------------------------------------------------

Image RegionMaskUtils::DilateMask( const Image& mask, int radius )
{
   int width = mask.Width();
   int height = mask.Height();

   Image result( width, height, pcl::ColorSpace::Gray );

   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         double maxVal = 0;

         for ( int dy = -radius; dy <= radius; ++dy )
         {
            for ( int dx = -radius; dx <= radius; ++dx )
            {
               int sx = x + dx;
               int sy = y + dy;

               if ( sx >= 0 && sx < width && sy >= 0 && sy < height )
               {
                  // Circular structuring element
                  if ( dx*dx + dy*dy <= radius*radius )
                  {
                     maxVal = std::max( maxVal, double( mask( sx, sy, 0 ) ) );
                  }
               }
            }
         }

         result( x, y, 0 ) = maxVal;
      }
   }

   return result;
}

// ----------------------------------------------------------------------------

Image RegionMaskUtils::ErodeMask( const Image& mask, int radius )
{
   int width = mask.Width();
   int height = mask.Height();

   Image result( width, height, pcl::ColorSpace::Gray );

   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         double minVal = 1.0;

         for ( int dy = -radius; dy <= radius; ++dy )
         {
            for ( int dx = -radius; dx <= radius; ++dx )
            {
               int sx = x + dx;
               int sy = y + dy;

               if ( sx >= 0 && sx < width && sy >= 0 && sy < height )
               {
                  // Circular structuring element
                  if ( dx*dx + dy*dy <= radius*radius )
                  {
                     minVal = std::min( minVal, double( mask( sx, sy, 0 ) ) );
                  }
               }
            }
         }

         result( x, y, 0 ) = minVal;
      }
   }

   return result;
}

// ----------------------------------------------------------------------------

Image RegionMaskUtils::CreateGradientMask( const Image& binaryMask, double featherRadius )
{
   // Create distance-based gradient from edge of binary mask
   int width = binaryMask.Width();
   int height = binaryMask.Height();

   Image result( width, height, pcl::ColorSpace::Gray );
   int radius = static_cast<int>( featherRadius + 0.5 );

   // Simple distance approximation
   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         double centerVal = binaryMask( x, y, 0 );

         if ( centerVal > 0.5 )
         {
            // Find distance to nearest zero
            double minDist = featherRadius + 1;

            for ( int dy = -radius; dy <= radius && minDist > 0; ++dy )
            {
               for ( int dx = -radius; dx <= radius; ++dx )
               {
                  int sx = x + dx;
                  int sy = y + dy;

                  if ( sx >= 0 && sx < width && sy >= 0 && sy < height )
                  {
                     if ( binaryMask( sx, sy, 0 ) < 0.5 )
                     {
                        double dist = std::sqrt( double( dx*dx + dy*dy ) );
                        minDist = std::min( minDist, dist );
                     }
                  }
               }
            }

            result( x, y, 0 ) = std::min( 1.0, minDist / featherRadius );
         }
         else
         {
            result( x, y, 0 ) = 0.0;
         }
      }
   }

   return result;
}

// ----------------------------------------------------------------------------

void RegionMaskUtils::NormalizeMask( Image& mask )
{
   double minVal = 1.0, maxVal = 0.0;

   for ( int y = 0; y < mask.Height(); ++y )
   {
      for ( int x = 0; x < mask.Width(); ++x )
      {
         double v = mask( x, y, 0 );
         minVal = std::min( minVal, v );
         maxVal = std::max( maxVal, v );
      }
   }

   if ( maxVal > minVal )
   {
      double scale = 1.0 / (maxVal - minVal);
      for ( int y = 0; y < mask.Height(); ++y )
      {
         for ( int x = 0; x < mask.Width(); ++x )
         {
            mask( x, y, 0 ) = (mask( x, y, 0 ) - minVal) * scale;
         }
      }
   }
}

// ----------------------------------------------------------------------------

double RegionMaskUtils::ComputeCoverage( const Image& mask )
{
   double sum = 0;
   size_t totalPixels = size_t( mask.Width() ) * mask.Height();

   for ( int y = 0; y < mask.Height(); ++y )
   {
      for ( int x = 0; x < mask.Width(); ++x )
      {
         sum += mask( x, y, 0 );
      }
   }

   return totalPixels > 0 ? sum / totalPixels : 0;
}

// ----------------------------------------------------------------------------

Image RegionMaskUtils::CreateStarMask( const Image& image, double threshold,
                                        int dilateRadius )
{
   int width = image.Width();
   int height = image.Height();
   int numChannels = image.NumberOfNominalChannels();

   // Compute luminance and threshold for stars
   Image mask( width, height, pcl::ColorSpace::Gray );

   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         double lum;
         if ( numChannels >= 3 )
         {
            double r = image( x, y, 0 );
            double g = image( x, y, 1 );
            double b = image( x, y, 2 );
            lum = 0.2126 * r + 0.7152 * g + 0.0722 * b;
         }
         else
         {
            lum = image( x, y, 0 );
         }

         mask( x, y, 0 ) = lum > threshold ? 1.0 : 0.0;
      }
   }

   // Dilate to catch halos
   if ( dilateRadius > 0 )
   {
      mask = DilateMask( mask, dilateRadius );
   }

   return mask;
}

// ----------------------------------------------------------------------------

Image RegionMaskUtils::CreateBackgroundMask( const Image& image, double threshold )
{
   int width = image.Width();
   int height = image.Height();
   int numChannels = image.NumberOfNominalChannels();

   Image mask( width, height, pcl::ColorSpace::Gray );

   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         double lum;
         if ( numChannels >= 3 )
         {
            double r = image( x, y, 0 );
            double g = image( x, y, 1 );
            double b = image( x, y, 2 );
            lum = 0.2126 * r + 0.7152 * g + 0.0722 * b;
         }
         else
         {
            lum = image( x, y, 0 );
         }

         // Soft threshold with smooth transition
         if ( lum < threshold * 0.5 )
         {
            mask( x, y, 0 ) = 1.0;
         }
         else if ( lum < threshold )
         {
            mask( x, y, 0 ) = 1.0 - (lum - threshold * 0.5) / (threshold * 0.5);
         }
         else
         {
            mask( x, y, 0 ) = 0.0;
         }
      }
   }

   return mask;
}

// ----------------------------------------------------------------------------

void RegionMaskUtils::SeparateCoreHalo( const Image& objectMask,
                                         Image& coreMask, Image& haloMask,
                                         double coreThreshold )
{
   int width = objectMask.Width();
   int height = objectMask.Height();

   coreMask.AllocateData( width, height, 1, ColorSpace::Gray );
   haloMask.AllocateData( width, height, 1, ColorSpace::Gray );

   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         double v = objectMask( x, y, 0 );

         if ( v > coreThreshold )
         {
            // Core: values above threshold
            coreMask( x, y, 0 ) = (v - coreThreshold) / (1.0 - coreThreshold);
            haloMask( x, y, 0 ) = 0.0;
         }
         else if ( v > 0 )
         {
            // Halo: values below threshold
            coreMask( x, y, 0 ) = 0.0;
            haloMask( x, y, 0 ) = v / coreThreshold;
         }
         else
         {
            coreMask( x, y, 0 ) = 0.0;
            haloMask( x, y, 0 ) = 0.0;
         }
      }
   }
}

// ----------------------------------------------------------------------------

} // namespace pcl
