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

namespace pcl
{

// ----------------------------------------------------------------------------
// Constants
// ----------------------------------------------------------------------------

static const float PI = 3.14159265358979323846f;
static const float SQRT_2PI = 2.50662827463100050242f;
static const float SQRT_2 = 1.41421356237309504880f;

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

std::vector<std::vector<PixelStackMetadata>> PixelStackAnalyzer::AnalyzeStack(
   const std::vector<const Image*>& frames,
   int channel ) const
{
   if ( frames.empty() || frames[0] == nullptr )
      return {};

   int width = frames[0]->Width();
   int height = frames[0]->Height();
   int numFrames = static_cast<int>( frames.size() );

   // Validate all frames have same dimensions
   for ( const Image* frame : frames )
   {
      if ( frame == nullptr || frame->Width() != width || frame->Height() != height )
         return {};  // Dimension mismatch
   }

   // Allocate output grid
   std::vector<std::vector<PixelStackMetadata>> result(
      height, std::vector<PixelStackMetadata>( width ) );

   // Buffer for pixel values across stack
   std::vector<float> pixelValues( numFrames );

   // Process each pixel position
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
         result[y][x] = AnalyzePixel( pixelValues );
      }
   }

   return result;
}

// ----------------------------------------------------------------------------

PixelStackMetadata PixelStackAnalyzer::AnalyzePixel( const std::vector<float>& values ) const
{
   PixelStackMetadata meta;

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

   // Identify outliers
   IdentifyOutliers( values, meta.distribution, meta.outlierMask );

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

   // Identify outliers with class-aware thresholds
   float sigma = meta.distribution.sigma;
   if ( sigma < 1e-10f )
      sigma = 1e-10f;

   meta.outlierMask = 0;
   for ( size_t i = 0; i < values.size() && i < 32; ++i )
   {
      float z = std::abs( values[i] - meta.distribution.mu ) / sigma;

      // Class-specific outlier logic
      bool isOutlier = false;

      switch ( regionClass )
      {
      case RegionClass::StarBright:
      case RegionClass::StarSaturated:
      case RegionClass::NebulaEmission:
         // Don't reject high values as outliers for bright features
         if ( values[i] < meta.distribution.mu && z > adjustedConfig.outlierSigmaThreshold )
            isOutlier = true;
         break;

      case RegionClass::NebulaDark:
      case RegionClass::DustLane:
         // Don't reject low values as outliers for dark features
         if ( values[i] > meta.distribution.mu && z > adjustedConfig.outlierSigmaThreshold )
            isOutlier = true;
         break;

      case RegionClass::Background:
      case RegionClass::ArtifactGradient:
         // For background, reject high outliers more aggressively (gradients, satellites)
         if ( values[i] > meta.distribution.mu && z > adjustedConfig.outlierSigmaThreshold * 0.7f )
            isOutlier = true;
         else if ( z > adjustedConfig.outlierSigmaThreshold )
            isOutlier = true;
         break;

      default:
         // Standard symmetric outlier rejection
         if ( z > adjustedConfig.outlierSigmaThreshold )
            isOutlier = true;
         break;
      }

      if ( isOutlier )
         meta.SetOutlier( static_cast<int>( i ) );
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

   // Compute higher moments
   float skewness = ComputeSkewness( values, mean, sigma );
   float kurtosis = ComputeKurtosis( values, mean, sigma );
   float bimodality = ComputeBimodalityCoefficient( values, mean, sigma );

   // Determine distribution type
   params.type = DetermineDistributionType( skewness, kurtosis, bimodality );
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
   float skewness, float kurtosis, float bimodality ) const
{
   // Check for bimodal first
   if ( bimodality > m_config.bimodalityThreshold )
      return StackDistributionType::Bimodal;

   // Check for high variance / uniform-like
   // (This is tricky - maybe check if kurtosis is very negative?)
   if ( kurtosis < -1.0f )
      return StackDistributionType::Uniform;

   float absSkew = std::abs( skewness );

   if ( absSkew > m_config.skewnessThreshold )
   {
      // Positive skew with heavy tails suggests lognormal
      if ( skewness > 0 && kurtosis > 1.0f )
         return StackDistributionType::Lognormal;

      return StackDistributionType::Skewed;
   }

   return StackDistributionType::Gaussian;
}

// ----------------------------------------------------------------------------

void PixelStackAnalyzer::IdentifyOutliers(
   const std::vector<float>& values,
   const StackDistributionParams& dist,
   uint32_t& outlierMask ) const
{
   outlierMask = 0;

   float sigma = dist.sigma;
   if ( sigma < 1e-10f )
      sigma = 1e-10f;

   for ( size_t i = 0; i < values.size() && i < 32; ++i )
   {
      float z = std::abs( values[i] - dist.mu ) / sigma;
      if ( z > m_config.outlierSigmaThreshold )
         outlierMask |= (1u << i);
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

   // Compute quality score for each frame's value
   std::vector<float> scores( values.size() );
   float maxScore = -std::numeric_limits<float>::max();
   int bestFrame = 0;

   for ( size_t i = 0; i < values.size(); ++i )
   {
      float v = values[i];
      float z = std::abs( v - dist.mu ) / std::max( dist.sigma, 1e-10f );

      // Base score: probability (Gaussian assumption for simplicity)
      float prob = std::exp( -0.5f * z * z );

      // Apply class-specific adjustments
      float adjustment = 1.0f;

      if ( config.favorHighSignal && v > dist.mu )
      {
         // Bonus for above-median values (nebula signal)
         adjustment = 1.0f + 0.2f * (v - dist.mu) / std::max( dist.sigma, 1e-10f );
      }
      else if ( config.favorLowSignal && v < dist.mu )
      {
         // Bonus for below-median values (dark nebula)
         adjustment = 1.0f + 0.2f * (dist.mu - v) / std::max( dist.sigma, 1e-10f );
      }

      scores[i] = prob * adjustment;

      if ( scores[i] > maxScore )
      {
         maxScore = scores[i];
         bestFrame = static_cast<int>( i );
      }
   }

   selectedFrame = bestFrame;

   // Confidence based on how much better the best is vs second best
   std::vector<float> sortedScores = scores;
   std::sort( sortedScores.begin(), sortedScores.end(), std::greater<float>() );

   if ( sortedScores.size() >= 2 && sortedScores[1] > 0 )
   {
      float ratio = sortedScores[0] / sortedScores[1];
      confidence = std::min( 1.0f, (ratio - 1.0f) * 2.0f );  // 1.5x better -> 100% confidence
   }
   else
   {
      confidence = (sortedScores[0] > 0.5f) ? 1.0f : sortedScores[0] * 2.0f;
   }

   return values[bestFrame];
}

// ----------------------------------------------------------------------------

float PixelStackAnalyzer::ComputeProbability( float value, const StackDistributionParams& dist )
{
   if ( dist.sigma <= 0 )
      return 0.0f;

   float z = (value - dist.mu) / dist.sigma;
   float prob = std::exp( -0.5f * z * z );

   // Normalize to 0-1
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
      config.outlierSigmaThreshold = 2.5f;
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

   std::sort( values.begin(), values.end() );
   size_t n = values.size();

   if ( n % 2 == 0 )
      return (values[n/2 - 1] + values[n/2]) / 2.0f;
   else
      return values[n/2];
}

// ----------------------------------------------------------------------------

float PixelStackAnalyzer::ComputeMAD( std::vector<float>& values, float median )
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
