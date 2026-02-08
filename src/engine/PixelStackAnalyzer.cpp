//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// PixelStackAnalyzer - Per-pixel distribution fitting across frame stacks

#include "PixelStackAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <limits>
#include <stdexcept>
#include <omp.h>

namespace pcl
{

// Helper: class-specific outlier detection (eliminates 3x code duplication)
static bool IsClassSpecificOutlier(
   float value, float mu, float sigma, float threshold, RegionClass regionClass )
{
   float z = std::abs( value - mu ) / std::max( sigma, 1e-10f );

   switch ( regionClass )
   {
   case RegionClass::StarBright:
   case RegionClass::StarSaturated:
   case RegionClass::ArtifactDiffraction:
      // Bright stars: only reject LOW outliers (clouds, tracking errors)
      return ( value < mu && z > threshold );

   case RegionClass::StarMedium:
   case RegionClass::StarFaint:
   case RegionClass::StarClusterOpen:
   case RegionClass::StarClusterGlobular:
   case RegionClass::StarHalo:
      // Medium/faint stars and halos: SYMMETRIC rejection (both high noise spikes AND low outliers)
      return ( z > threshold );

   case RegionClass::NebulaDark:
   case RegionClass::DustLane:
      // Dark features: only reject HIGH outliers
      return ( value > mu && z > threshold );

   case RegionClass::NebulaEmission:
   case RegionClass::NebulaReflection:
   case RegionClass::NebulaPlanetary:
   case RegionClass::GalaxyCore:
   case RegionClass::GalaxySpiral:
   case RegionClass::GalaxyElliptical:
   case RegionClass::GalaxyIrregular:
   case RegionClass::GalacticCirrus:
      // Emission/galaxy: reject LOW outliers, keep high signal
      return ( value < mu && z > threshold );

   case RegionClass::Background:
   case RegionClass::ArtifactGradient:
      // Background: reject HIGH outliers more aggressively (0.85x multiplier)
      if ( value > mu && z > threshold * 0.85f )
         return true;
      return ( z > threshold );

   default:
      return ( z > threshold );
   }
}

// ----------------------------------------------------------------------------
// PixelStackAnalyzer Implementation
// ----------------------------------------------------------------------------

PixelStackAnalyzer::PixelStackAnalyzer()
   : m_config()
{
}

PixelStackAnalyzer::PixelStackAnalyzer( const StackAnalysisConfig& config )
   : m_config( config )
{
}

// ----------------------------------------------------------------------------

std::vector<PixelStackMetadata> PixelStackAnalyzer::AnalyzeStack(
   const std::vector<const Image*>& frames,
   int channel ) const
{
   if ( frames.empty() || frames[0] == nullptr )
      return {};

   int width = frames[0]->Width();
   int height = frames[0]->Height();
   int numFrames = static_cast<int>( frames.size() );

   // Validate dimensions to prevent allocation crashes
   if ( width <= 0 || height <= 0 || width > 100000 || height > 100000 )
      throw std::runtime_error( "Invalid image dimensions for stack analysis" );

   size_t numPixels = static_cast<size_t>( width ) * height;
   if ( numPixels > 500000000 )  // 500M pixels max
      throw std::runtime_error( "Image too large for stack analysis" );

   if ( numFrames <= 0 || numFrames > 10000 )
      throw std::runtime_error( "Invalid number of frames: " + std::to_string( numFrames ) );

   // Validate all frames have same dimensions
   for ( const Image* frame : frames )
   {
      if ( frame == nullptr || frame->Width() != width || frame->Height() != height )
         return {};  // Dimension mismatch
   }

   // Allocate flat output vector indexed as [y * width + x]
   std::vector<PixelStackMetadata> result( height * width );

   // Process each pixel position with OpenMP parallelization
   // pixelValues buffer is thread-local to avoid data races
   #pragma omp parallel
   {
      std::vector<float> pixelValues( numFrames );

      #pragma omp for schedule( dynamic, 16 )
      for ( int y = 0; y < height; ++y )
      {
         for ( int x = 0; x < width; ++x )
         {
            // Collect values from all frames at this position
            for ( int f = 0; f < numFrames; ++f )
            {
               const Image::sample* data = frames[f]->PixelData( channel );
               size_t idx = static_cast<size_t>( y ) * width + x;
               pixelValues[f] = static_cast<float>( data[idx] );
            }

            // Analyze this pixel stack
            result[y * width + x] = AnalyzePixel( pixelValues );
         }
      }
   }

   return result;
}

// ----------------------------------------------------------------------------

PixelStackMetadata PixelStackAnalyzer::AnalyzePixel( const std::vector<float>& values ) const
{
   PixelStackMetadata meta;
   meta.totalFrames = static_cast<uint16_t>( values.size() );

   if ( values.size() < static_cast<size_t>( m_config.minFramesForStats ) )
   {
      // Not enough frames - just use median
      if ( !values.empty() )
      {
         std::vector<float> sorted = values;
         meta.selectedValue = ComputeMedian( sorted );
         meta.sourceFrame = 0;
         meta.confidence = 0.0f;
      }
      return meta;
   }

   // Fit distribution to values
   meta.distribution = FitDistribution( values );

   // Local rejected vector tracks ALL frames regardless of index (fixes >64 frame bug)
   std::vector<bool> rejected( values.size(), false );

   // Identify outliers (initial pass)
   float sigma = meta.distribution.sigma;
   if ( sigma < 1e-10f )
      sigma = 1e-10f;

   float initialSigma = sigma;  // Save initial sigma as floor for iterative passes

   for ( size_t i = 0; i < values.size(); ++i )
   {
      float z = std::abs( values[i] - meta.distribution.mu ) / sigma;
      if ( z > m_config.outlierSigmaThreshold )
         rejected[i] = true;
   }

   // Iterative sigma clipping - refine outlier detection
   // Use initialSigma * 0.5 as floor to prevent death spiral where sigma
   // shrinks each pass, causing cascading rejection of valid data
   for ( int pass = 1; pass < 3; ++pass )
   {
      // Count current valid (non-rejected) frames
      int validCount = 0;
      for ( size_t i = 0; i < values.size(); ++i )
         if ( !rejected[i] )
            ++validCount;

      int minRequired = std::max( m_config.minFramesForStats, static_cast<int>( values.size() ) / 2 );
      if ( validCount < minRequired )
         break;  // Too few frames left, stop clipping

      // Recompute statistics from valid frames only
      std::vector<float> validValues;
      validValues.reserve( validCount );
      for ( size_t i = 0; i < values.size(); ++i )
         if ( !rejected[i] )
            validValues.push_back( values[i] );

      float newMu = ComputeMedian( validValues );
      float newSigma = ComputeMAD( validValues, newMu ) * 1.4826f;

      if ( newSigma < 1e-10f )
         break;

      // Floor sigma at 50% of initial estimate to prevent death spiral
      newSigma = std::max( newSigma, initialSigma * 0.5f );

      // Check for new outliers with refined statistics
      std::vector<bool> prevRejected = rejected;
      meta.distribution.mu = newMu;
      meta.distribution.sigma = newSigma;

      for ( size_t i = 0; i < values.size(); ++i )
      {
         float z = std::abs( values[i] - newMu ) / newSigma;
         if ( z > m_config.outlierSigmaThreshold )
            rejected[i] = true;
      }

      // Converged if no new outliers found
      if ( rejected == prevRejected )
         break;
   }

   // Aggregate rejected vector into metadata
   meta.outlierMask = 0;
   meta.outlierCount = 0;
   for ( size_t i = 0; i < rejected.size(); ++i )
   {
      if ( rejected[i] )
      {
         if ( i < 64 )
            meta.outlierMask |= (uint64_t( 1 ) << i);
         meta.outlierCount++;
      }
   }

   // Select best value (no class hint, use background as default)
   int selectedFrame = 0;
   float confidence = 0.0f;
   meta.selectedValue = SelectBestValue( values, meta.distribution, RegionClass::Background, selectedFrame, confidence );
   meta.sourceFrame = static_cast<uint16_t>( selectedFrame );
   meta.confidence = confidence;

   return meta;
}

// ----------------------------------------------------------------------------

PixelStackMetadata PixelStackAnalyzer::AnalyzePixelWithClass(
   const std::vector<float>& values,
   RegionClass regionClass,
   float classConfidence ) const
{
   PixelStackMetadata meta;
   meta.totalFrames = static_cast<uint16_t>( values.size() );

   if ( values.size() < static_cast<size_t>( m_config.minFramesForStats ) )
   {
      if ( !values.empty() )
      {
         std::vector<float> sorted = values;
         meta.selectedValue = ComputeMedian( sorted );
         meta.sourceFrame = 0;
         meta.confidence = 0.0f;
      }
      return meta;
   }

   // Fit distribution
   meta.distribution = FitDistribution( values );

   // Get class-adjusted config for outlier detection
   StackAnalysisConfig adjustedConfig = GetClassAdjustedConfig( regionClass );

   // Local rejected vector tracks ALL frames regardless of index (fixes >64 frame bug)
   std::vector<bool> rejected( values.size(), false );

   // Identify outliers with class-aware thresholds
   float sigma = meta.distribution.sigma;
   if ( sigma < 1e-10f )
      sigma = 1e-10f;

   float initialSigma = sigma;  // Save initial sigma as floor for iterative passes

   for ( size_t i = 0; i < values.size(); ++i )
   {
      if ( IsClassSpecificOutlier( values[i], meta.distribution.mu, sigma, adjustedConfig.outlierSigmaThreshold, regionClass ) )
         rejected[i] = true;
   }

   // Iterative sigma clipping - refine outlier detection with class awareness
   // Use initialSigma * 0.5 as floor to prevent death spiral
   for ( int pass = 1; pass < 3; ++pass )
   {
      // Count current valid (non-rejected) frames
      int validCount = 0;
      for ( size_t i = 0; i < values.size(); ++i )
         if ( !rejected[i] )
            ++validCount;

      int minRequired = std::max( m_config.minFramesForStats, static_cast<int>( values.size() ) / 2 );
      if ( validCount < minRequired )
         break;  // Too few frames left, stop clipping

      // Recompute statistics from valid frames only
      std::vector<float> validValues;
      validValues.reserve( validCount );
      for ( size_t i = 0; i < values.size(); ++i )
         if ( !rejected[i] )
            validValues.push_back( values[i] );

      float newMu = ComputeMedian( validValues );
      float newSigma = ComputeMAD( validValues, newMu ) * 1.4826f;

      if ( newSigma < 1e-10f )
         break;

      // Floor sigma at 50% of initial estimate to prevent death spiral
      newSigma = std::max( newSigma, initialSigma * 0.5f );

      // Check for new outliers with refined statistics
      std::vector<bool> prevRejected = rejected;
      meta.distribution.mu = newMu;
      meta.distribution.sigma = newSigma;

      for ( size_t i = 0; i < values.size(); ++i )
      {
         if ( IsClassSpecificOutlier( values[i], newMu, newSigma, adjustedConfig.outlierSigmaThreshold, regionClass ) )
            rejected[i] = true;
      }

      // Converged if no new outliers found
      if ( rejected == prevRejected )
         break;
   }

   // Aggregate rejected vector into metadata
   meta.outlierMask = 0;
   meta.outlierCount = 0;
   for ( size_t i = 0; i < rejected.size(); ++i )
   {
      if ( rejected[i] )
      {
         if ( i < 64 )
            meta.outlierMask |= (uint64_t( 1 ) << i);
         meta.outlierCount++;
      }
   }

   // Select best value with class awareness
   int selectedFrame = 0;
   float confidence = 0.0f;
   meta.selectedValue = SelectBestValue( values, meta.distribution, regionClass, selectedFrame, confidence );
   meta.sourceFrame = static_cast<uint16_t>( selectedFrame );
   meta.confidence = confidence * classConfidence;  // Weight by class confidence

   return meta;
}

// ----------------------------------------------------------------------------

PixelStackMetadata PixelStackAnalyzer::AnalyzePixelWithClassAndAnomalies(
   const std::vector<float>& values,
   RegionClass regionClass,
   float classConfidence,
   const std::vector<bool>& anomalyFlags ) const
{
   PixelStackMetadata meta;
   meta.totalFrames = static_cast<uint16_t>( values.size() );

   if ( values.size() < static_cast<size_t>( m_config.minFramesForStats ) )
   {
      if ( !values.empty() )
      {
         std::vector<float> sorted = values;
         meta.selectedValue = ComputeMedian( sorted );
         meta.sourceFrame = 0;
         meta.confidence = 0.0f;
      }
      return meta;
   }

   // Fit distribution
   meta.distribution = FitDistribution( values );

   // Get class-adjusted config for outlier detection
   StackAnalysisConfig adjustedConfig = GetClassAdjustedConfig( regionClass );

   std::vector<bool> rejected( values.size(), false );

   // Identify outliers with class-aware thresholds AND anomaly penalty
   float sigma = meta.distribution.sigma;
   if ( sigma < 1e-10f )
      sigma = 1e-10f;

   float initialSigma = sigma;  // Save initial sigma as floor for iterative passes

   for ( size_t i = 0; i < values.size(); ++i )
   {
      // Anomalous frames (class disagrees with consensus) get a tighter threshold
      float effectiveThreshold = adjustedConfig.outlierSigmaThreshold;
      if ( i < anomalyFlags.size() && anomalyFlags[i] )
         effectiveThreshold *= 0.7f;  // 30% more aggressive for anomalous frames

      if ( IsClassSpecificOutlier( values[i], meta.distribution.mu, sigma,
                                    effectiveThreshold, regionClass ) )
         rejected[i] = true;
   }

   // Iterative sigma clipping (same as AnalyzePixelWithClass but with anomaly awareness)
   // Use initialSigma * 0.5 as floor to prevent death spiral
   for ( int pass = 1; pass < 3; ++pass )
   {
      int validCount = 0;
      for ( size_t i = 0; i < values.size(); ++i )
         if ( !rejected[i] )
            ++validCount;

      int minRequired = std::max( m_config.minFramesForStats,
                                   static_cast<int>( values.size() ) / 2 );
      if ( validCount < minRequired )
         break;

      std::vector<float> validValues;
      validValues.reserve( validCount );
      for ( size_t i = 0; i < values.size(); ++i )
         if ( !rejected[i] )
            validValues.push_back( values[i] );

      float newMu = ComputeMedian( validValues );
      float newSigma = ComputeMAD( validValues, newMu ) * 1.4826f;

      if ( newSigma < 1e-10f )
         break;

      // Floor sigma at 50% of initial estimate to prevent death spiral
      newSigma = std::max( newSigma, initialSigma * 0.5f );

      std::vector<bool> prevRejected = rejected;
      meta.distribution.mu = newMu;
      meta.distribution.sigma = newSigma;

      for ( size_t i = 0; i < values.size(); ++i )
      {
         float effectiveThreshold = adjustedConfig.outlierSigmaThreshold;
         if ( i < anomalyFlags.size() && anomalyFlags[i] )
            effectiveThreshold *= 0.7f;

         if ( IsClassSpecificOutlier( values[i], newMu, newSigma,
                                       effectiveThreshold, regionClass ) )
            rejected[i] = true;
      }

      if ( rejected == prevRejected )
         break;
   }

   // Aggregate rejected vector into metadata
   meta.outlierMask = 0;
   meta.outlierCount = 0;
   for ( size_t i = 0; i < rejected.size(); ++i )
   {
      if ( rejected[i] )
      {
         if ( i < 64 )
            meta.outlierMask |= (uint64_t( 1 ) << i);
         meta.outlierCount++;
      }
   }

   // Select best value with class awareness
   int selectedFrame = 0;
   float confidence = 0.0f;
   meta.selectedValue = SelectBestValue( values, meta.distribution, regionClass,
                                          selectedFrame, confidence );
   meta.sourceFrame = static_cast<uint16_t>( selectedFrame );
   meta.confidence = confidence * classConfidence;

   return meta;
}

// ----------------------------------------------------------------------------

StackDistributionParams PixelStackAnalyzer::FitDistribution( const std::vector<float>& values ) const
{
   StackDistributionParams params;

   if ( values.empty() )
      return params;

   // Make copies for statistical computations
   std::vector<float> sortedValues = values;

   float median = ComputeMedian( sortedValues );
   float mad = ComputeMAD( sortedValues, median );
   float robustSigma = MADToSigma( mad );

   float mean = ComputeMean( values );
   float sigma = ComputeStdDev( values, mean );

   // Use robust estimators if configured and they suggest contamination
   if ( m_config.useRobustEstimators && robustSigma > 0 && sigma > 2.0f * robustSigma )
   {
      sigma = robustSigma;
      mean = median;
   }

   // Ensure non-zero sigma
   if ( sigma < 1e-10f )
      sigma = 1e-10f;

   // Compute higher moments with sample-size awareness
   int N = static_cast<int>( values.size() );
   float skewness = ComputeSkewness( values, mean, sigma );

   // Only use kurtosis for classification when N >= 15
   // For small samples, kurtosis estimates are extremely unreliable
   float kurtosis = ( N >= 15 ) ? ComputeKurtosis( values, mean, sigma ) : 0.0f;

   float bimodality = ComputeBimodalityCoefficient( values, mean, sigma );

   // Determine distribution type with sample-size-aware thresholds
   params.type = DetermineDistributionType( skewness, kurtosis, bimodality, N );
   params.mu = mean;
   params.sigma = sigma;
   params.skewness = skewness;
   params.kurtosis = kurtosis;

   // Quality is inverse of coefficient of variation, clamped
   float cv = sigma / std::max( std::abs( mean ), 1e-10f );
   params.quality = 1.0f / (1.0f + cv);

   return params;
}

// ----------------------------------------------------------------------------

StackDistributionType PixelStackAnalyzer::DetermineDistributionType(
   float skewness, float kurtosis, float bimodality, int sampleSize ) const
{
   // Check for bimodal first
   if ( bimodality > m_config.bimodalityThreshold )
      return StackDistributionType::Bimodal;

   // Check for high variance / uniform-like
   // Only use kurtosis classification for sufficiently large samples
   if ( sampleSize >= 15 && kurtosis < -1.0f )
      return StackDistributionType::Uniform;

   // Adaptive skewness threshold: increases for small samples
   // Standard error of skewness ~ sqrt(6/N), so the 0.5 threshold
   // is unreliable for N < ~25. Use at least 1.5x the standard error.
   float skewnessThreshold = m_config.skewnessThreshold;
   if ( sampleSize > 0 && sampleSize < 25 )
   {
      float se = std::sqrt( 6.0f / static_cast<float>( sampleSize ) );
      skewnessThreshold = std::max( m_config.skewnessThreshold, 1.5f * se );
   }

   float absSkew = std::abs( skewness );

   if ( absSkew > skewnessThreshold )
   {
      // Positive skew with heavy tails suggests lognormal
      // Only use kurtosis criterion for sufficiently large samples
      if ( skewness > 0 && sampleSize >= 15 && kurtosis > 1.0f )
         return StackDistributionType::Lognormal;

      return StackDistributionType::Skewed;
   }

   return StackDistributionType::Gaussian;
}

// ----------------------------------------------------------------------------

void PixelStackAnalyzer::IdentifyOutliers(
   const std::vector<float>& values,
   const StackDistributionParams& dist,
   uint64_t& outlierMask ) const
{
   outlierMask = 0;

   float sigma = dist.sigma;
   if ( sigma < 1e-10f )
      sigma = 1e-10f;

   for ( size_t i = 0; i < values.size(); ++i )
   {
      float z = std::abs( values[i] - dist.mu ) / sigma;
      if ( z > m_config.outlierSigmaThreshold )
      {
         if ( i < 64 )
            outlierMask |= (uint64_t( 1 ) << i);
      }
   }
}

// ----------------------------------------------------------------------------

float PixelStackAnalyzer::SelectBestValue(
   const std::vector<float>& values,
   const StackDistributionParams& dist,
   RegionClass regionClass,
   int& selectedFrame,
   float& confidence ) const
{
   if ( values.empty() )
   {
      selectedFrame = 0;
      confidence = 0.0f;
      return 0.0f;
   }

   // Get class-adjusted config
   StackAnalysisConfig config = GetClassAdjustedConfig( regionClass );

   // First, identify outliers based on class-specific logic
   // Build a valid (non-outlier) value list with frame indices
   std::vector<std::pair<float, int>> validValues;
   float sigma = std::max( dist.sigma, 1e-10f );

   for ( size_t i = 0; i < values.size(); ++i )
   {
      if ( !IsClassSpecificOutlier( values[i], dist.mu, sigma, config.outlierSigmaThreshold, regionClass ) )
         validValues.push_back( { values[i], static_cast<int>( i ) } );
   }

   // If all values were rejected, fall back to median of all values
   if ( validValues.empty() )
   {
      std::vector<float> allVals( values.begin(), values.end() );
      std::nth_element( allVals.begin(), allVals.begin() + allVals.size() / 2, allVals.end() );
      float median = allVals[allVals.size() / 2];
      // Find frame closest to median
      float bestDist = std::numeric_limits<float>::max();
      for ( size_t i = 0; i < values.size(); ++i )
      {
         float d = std::abs( values[i] - median );
         if ( d < bestDist )
         {
            bestDist = d;
            selectedFrame = static_cast<int>( i );
         }
      }
      confidence = 0.3f;  // Low confidence for fallback
      return values[selectedFrame];
   }

   // Compute data-dependent confidence from stack statistics
   auto ComputeConfidence = [&]( float baseConfidence ) -> float
   {
      // Factor 1: What fraction of frames are valid (not outliers)?
      float validRatio = static_cast<float>( validValues.size() ) / static_cast<float>( values.size() );

      // Factor 2: How tight is the distribution? (lower CV = higher confidence)
      float cv = ( dist.mu > 1e-6f ) ? ( dist.sigma / dist.mu ) : 1.0f;
      float cvFactor = 1.0f / ( 1.0f + cv * 2.0f );

      // Factor 3: Distribution fit quality
      float qualityFactor = dist.quality;

      // Combine: base * validity * cv * quality
      float conf = baseConfidence * ( 0.4f + 0.3f * validRatio + 0.2f * cvFactor + 0.1f * qualityFactor );
      return std::max( 0.3f, std::min( 0.98f, conf ) );
   };

   // CLASS-SPECIFIC SELECTION STRATEGY:
   // For dark features: select MINIMUM value (darkest = correct)
   // For bright features: select value that preserves signal
   // For background: select near median

   int bestFrame = 0;
   float bestValue = 0.0f;

   // Helper lambda: get frame weight (1.0 if no weights configured)
   auto GetFrameWeight = [this]( int frameIdx ) -> float
   {
      if ( !m_config.frameWeights.empty() &&
           frameIdx >= 0 &&
           static_cast<size_t>( frameIdx ) < m_config.frameWeights.size() )
         return m_config.frameWeights[frameIdx];
      return 1.0f;
   };

   switch ( regionClass )
   {
   case RegionClass::NebulaDark:
   case RegionClass::DustLane:
      {
         // SELECT MINIMUM: The darkest valid value is the correct dark nebula value
         // Frame weight acts as a tiebreaker - prefer darker values from better frames
         float bestScore = std::numeric_limits<float>::max();
         for ( const auto& vp : validValues )
         {
            // Lower values are better; frame weight reduces the score (favors weighted frames)
            float score = vp.first / GetFrameWeight( vp.second );
            if ( score < bestScore )
            {
               bestScore = score;
               bestFrame = vp.second;
               bestValue = vp.first;
            }
         }
         // High confidence because we're selecting the correct extreme
         confidence = ComputeConfidence( 0.9f );
      }
      break;

   case RegionClass::StarBright:
   case RegionClass::StarSaturated:
      {
         // SELECT MAXIMUM: The brightest valid value preserves the star signal
         // Frame weight boosts score - prefer brighter values from better frames
         float maxScore = -std::numeric_limits<float>::max();
         for ( const auto& vp : validValues )
         {
            float score = vp.first * GetFrameWeight( vp.second );
            if ( score > maxScore )
            {
               maxScore = score;
               bestFrame = vp.second;
               bestValue = vp.first;
            }
         }
         confidence = ComputeConfidence( 0.9f );
      }
      break;

   case RegionClass::StarMedium:
   case RegionClass::StarFaint:
   case RegionClass::StarClusterOpen:
   case RegionClass::StarClusterGlobular:
      {
         // Probability-weighted selection biased toward higher signal
         // Uses same approach as nebula but with moderate upward bias
         float maxScore = -std::numeric_limits<float>::max();
         for ( const auto& vp : validValues )
         {
            float v = vp.first;
            float z = std::abs( v - dist.mu ) / sigma;
            float prob = std::exp( -0.5f * z * z );
            // Moderate upward bias (0.2) - less aggressive than nebula (0.3)
            float score = prob * (1.0f + 0.2f * (v - dist.mu) / sigma);
            score *= GetFrameWeight( vp.second );
            if ( score > maxScore )
            {
               maxScore = score;
               bestFrame = vp.second;
               bestValue = v;
            }
         }
         confidence = ComputeConfidence( 0.8f );
      }
      break;

   case RegionClass::StarHalo:
      {
         // Probability-weighted with reduced upward bias for spatial consistency
         float maxScore = -std::numeric_limits<float>::max();
         for ( const auto& vp : validValues )
         {
            float v = vp.first;
            float z = std::abs( v - dist.mu ) / sigma;
            float prob = std::exp( -0.5f * z * z );
            // Gentle upward bias (0.15) - halos need smoothness
            float score = prob * (1.0f + 0.15f * (v - dist.mu) / sigma);
            score *= GetFrameWeight( vp.second );
            if ( score > maxScore )
            {
               maxScore = score;
               bestFrame = vp.second;
               bestValue = v;
            }
         }
         confidence = ComputeConfidence( 0.75f );
      }
      break;

   case RegionClass::NebulaEmission:
   case RegionClass::NebulaReflection:
   case RegionClass::NebulaPlanetary:
   case RegionClass::GalaxyCore:
   case RegionClass::GalaxySpiral:
   case RegionClass::GalaxyElliptical:
   case RegionClass::GalaxyIrregular:
   case RegionClass::GalacticCirrus:
      {
         // For emission nebula, galaxies, and IFN: favor signal (higher values)
         // but be more conservative than stars. GalacticCirrus is extremely faint -
         // very careful signal preservation, DO NOT reject low values as they may
         // represent real dark structure in the cirrus.
         float maxScore = -std::numeric_limits<float>::max();
         for ( const auto& vp : validValues )
         {
            float v = vp.first;
            float z = std::abs( v - dist.mu ) / sigma;
            float prob = std::exp( -0.5f * z * z );
            // Score: probability weighted toward higher signal
            float score = prob * (1.0f + 0.3f * (v - dist.mu) / sigma);
            score *= GetFrameWeight( vp.second );
            if ( score > maxScore )
            {
               maxScore = score;
               bestFrame = vp.second;
               bestValue = v;
            }
         }
         confidence = ComputeConfidence( 0.8f );
      }
      break;

   default:
      {
         // Default: select value closest to median (standard behavior)
         // Frame weight acts as a tiebreaker for similarly-close values
         std::vector<float> sortedValid;
         for ( const auto& vp : validValues )
            sortedValid.push_back( vp.first );
         std::sort( sortedValid.begin(), sortedValid.end() );
         float median = sortedValid[sortedValid.size() / 2];

         float bestInvScore = std::numeric_limits<float>::max();
         for ( const auto& vp : validValues )
         {
            float d = std::abs( vp.first - median );
            // Lower distance is better; divide by frame weight to favor weighted frames
            float invScore = d / GetFrameWeight( vp.second );
            if ( invScore < bestInvScore )
            {
               bestInvScore = invScore;
               bestFrame = vp.second;
               bestValue = vp.first;
            }
         }
         confidence = ComputeConfidence( 0.7f );
      }
      break;
   }

   selectedFrame = bestFrame;

   return bestValue;
}

// ----------------------------------------------------------------------------

float PixelStackAnalyzer::ComputeProbability( float value, const StackDistributionParams& dist )
{
   if ( dist.sigma <= 0 )
      return 0.0f;

   float z = (value - dist.mu) / dist.sigma;
   float prob = std::exp( -0.5f * z * z );

   // Returns the Gaussian kernel value (0-1) as a relative probability measure.
   // This is NOT a normalized probability density; it gives the likelihood
   // relative to the distribution peak (value at mean returns 1.0).
   return prob;
}

// ----------------------------------------------------------------------------

StackAnalysisConfig PixelStackAnalyzer::GetClassAdjustedConfig( RegionClass regionClass ) const
{
   StackAnalysisConfig config = m_config;

   switch ( regionClass )
   {
   // Background - aggressive high outlier rejection
   case RegionClass::Background:
   case RegionClass::ArtifactGradient:
   case RegionClass::ArtifactNoise:
      config.outlierSigmaThreshold = 3.0f;
      config.favorHighSignal = false;
      config.favorLowSignal = false;
      config.useMedianSelection = true;
      break;

   // Stars - don't reject high values
   case RegionClass::StarBright:
   case RegionClass::StarMedium:
   case RegionClass::StarFaint:
   case RegionClass::StarSaturated:
   case RegionClass::ArtifactDiffraction:  // Diffraction spikes are star features
      config.outlierSigmaThreshold = 4.0f;  // More permissive
      config.favorHighSignal = true;        // Stars should be bright
      config.favorLowSignal = false;
      break;

   // Star clusters
   case RegionClass::StarClusterOpen:
   case RegionClass::StarClusterGlobular:
      config.outlierSigmaThreshold = 3.5f;
      config.favorHighSignal = true;
      config.favorLowSignal = false;
      break;

   // Emission nebula - favor signal
   case RegionClass::NebulaEmission:
      config.outlierSigmaThreshold = 3.5f;
      config.favorHighSignal = true;
      config.favorLowSignal = false;
      break;

   // Reflection nebula
   case RegionClass::NebulaReflection:
      config.outlierSigmaThreshold = 3.0f;
      config.favorHighSignal = true;
      config.favorLowSignal = false;
      break;

   // Dark nebula - preserve dark values!
   case RegionClass::NebulaDark:
   case RegionClass::DustLane:
      config.outlierSigmaThreshold = 3.0f;
      config.favorHighSignal = false;
      config.favorLowSignal = true;    // Dark features should stay dark
      break;

   // Planetary nebula - preserve structure
   case RegionClass::NebulaPlanetary:
      config.outlierSigmaThreshold = 3.0f;
      config.favorHighSignal = true;
      config.favorLowSignal = false;
      break;

   // Galaxy features
   case RegionClass::GalaxyCore:
      config.outlierSigmaThreshold = 3.5f;
      config.favorHighSignal = true;
      config.favorLowSignal = false;
      break;

   case RegionClass::GalaxySpiral:
   case RegionClass::GalaxyElliptical:
   case RegionClass::GalaxyIrregular:
      config.outlierSigmaThreshold = 3.0f;
      config.favorHighSignal = true;
      config.favorLowSignal = false;
      break;

   // Star halos - similar to medium stars but with more spatial consistency
   case RegionClass::StarHalo:
      config.outlierSigmaThreshold = 3.5f;  // Between stars (4.0) and nebulae (3.0)
      config.favorHighSignal = true;         // Preserve halo signal
      config.favorLowSignal = false;
      break;

   // Galactic cirrus / IFN - extremely faint, at noise floor
   case RegionClass::GalacticCirrus:
      config.outlierSigmaThreshold = 4.0f;  // Very permissive - don't reject faint features
      config.favorHighSignal = true;         // Preserve faint signal
      config.favorLowSignal = false;
      break;

   // Artifacts to reject
   case RegionClass::ArtifactHotPixel:
   case RegionClass::ArtifactSatellite:
      config.outlierSigmaThreshold = 2.0f;  // Very aggressive rejection
      config.favorHighSignal = false;
      config.favorLowSignal = false;
      config.useMedianSelection = true;
      break;

   default:
      // Keep defaults
      break;
   }

   return config;
}

// ----------------------------------------------------------------------------
// Statistical Helper Functions
// ----------------------------------------------------------------------------

float PixelStackAnalyzer::ComputeMean( const std::vector<float>& values )
{
   if ( values.empty() )
      return 0.0f;

   float sum = std::accumulate( values.begin(), values.end(), 0.0f );
   return sum / static_cast<float>( values.size() );
}

// ----------------------------------------------------------------------------

float PixelStackAnalyzer::ComputeStdDev( const std::vector<float>& values, float mean )
{
   if ( values.size() < 2 )
      return 0.0f;

   float sumSq = 0.0f;
   for ( float v : values )
   {
      float d = v - mean;
      sumSq += d * d;
   }

   return std::sqrt( sumSq / static_cast<float>( values.size() - 1 ) );
}

// ----------------------------------------------------------------------------

float PixelStackAnalyzer::ComputeMedian( std::vector<float>& values )
{
   if ( values.empty() )
      return 0.0f;

   size_t n = values.size();
   size_t mid = n / 2;

   std::nth_element( values.begin(), values.begin() + mid, values.end() );

   if ( n % 2 == 0 )
   {
      float upper = values[mid];
      // Find max of lower partition for the other middle element
      float lower = *std::max_element( values.begin(), values.begin() + mid );
      return (lower + upper) / 2.0f;
   }
   else
      return values[mid];
}

// ----------------------------------------------------------------------------

float PixelStackAnalyzer::ComputeMAD( std::vector<float> values, float median )
{
   if ( values.empty() )
      return 0.0f;

   for ( float& v : values )
      v = std::abs( v - median );

   return ComputeMedian( values );
}

// ----------------------------------------------------------------------------

float PixelStackAnalyzer::ComputeSkewness( const std::vector<float>& values, float mean, float sigma )
{
   if ( values.size() < 3 || sigma <= 0 )
      return 0.0f;

   float sum3 = 0.0f;
   for ( float v : values )
   {
      float z = (v - mean) / sigma;
      sum3 += z * z * z;
   }

   float n = static_cast<float>( values.size() );
   return sum3 * n / ((n - 1.0f) * (n - 2.0f));
}

// ----------------------------------------------------------------------------

float PixelStackAnalyzer::ComputeKurtosis( const std::vector<float>& values, float mean, float sigma )
{
   if ( values.size() < 4 || sigma <= 0 )
      return 0.0f;

   float sum4 = 0.0f;
   for ( float v : values )
   {
      float z = (v - mean) / sigma;
      sum4 += z * z * z * z;
   }

   float n = static_cast<float>( values.size() );

   // Excess kurtosis (Gaussian = 0)
   return (sum4 / n) - 3.0f;
}

// ----------------------------------------------------------------------------

float PixelStackAnalyzer::ComputeBimodalityCoefficient( const std::vector<float>& values, float mean, float sigma )
{
   // Sarle's bimodality coefficient: (skewness^2 + 1) / (kurtosis + 3)
   // Values > 0.555 suggest bimodality

   if ( values.size() < 4 || sigma <= 0 )
      return 0.0f;

   float skewness = ComputeSkewness( values, mean, sigma );
   float kurtosis = ComputeKurtosis( values, mean, sigma );

   // Kurtosis here is excess kurtosis, so add 3 back for the formula
   float denominator = kurtosis + 3.0f;
   if ( denominator <= 0 )
      return 0.0f;

   return (skewness * skewness + 1.0f) / denominator;
}

// ----------------------------------------------------------------------------

} // namespace pcl
