//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "SegmentationModel.h"

#include <pcl/Console.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <stdexcept>

namespace pcl
{

// ----------------------------------------------------------------------------
// SegmentationResult Implementation
// ----------------------------------------------------------------------------

std::map<RegionClass, double> SegmentationResult::ComputeCoverage() const
{
   std::map<RegionClass, double> coverage;

   for ( const auto& pair : masks )
   {
      const Image& mask = pair.second;
      double sum = 0;
      size_t totalPixels = size_t( mask.Width() ) * mask.Height();

      for ( int y = 0; y < mask.Height(); ++y )
      {
         for ( int x = 0; x < mask.Width(); ++x )
         {
            sum += mask( x, y, 0 );
         }
      }

      coverage[pair.first] = totalPixels > 0 ? sum / totalPixels : 0;
   }

   return coverage;
}

// ----------------------------------------------------------------------------
// MockSegmentationModel Implementation
// ----------------------------------------------------------------------------

MockSegmentationModel::MockSegmentationModel()
{
}

// ----------------------------------------------------------------------------

bool MockSegmentationModel::Initialize( const SegmentationConfig& config )
{
   m_config = config;
   return true;
}

// ----------------------------------------------------------------------------

double MockSegmentationModel::ComputeLuminance( double r, double g, double b ) const
{
   return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

// ----------------------------------------------------------------------------

Image MockSegmentationModel::DetectStars( const Image& luminance ) const
{
   int width = luminance.Width();
   int height = luminance.Height();

   Image starMask( width, height, pcl::ColorSpace::Gray );
   starMask.Zero();

   // Simple local maximum detection
   int radius = 3;

   for ( int y = radius; y < height - radius; ++y )
   {
      for ( int x = radius; x < width - radius; ++x )
      {
         double centerValue = luminance( x, y, 0 );

         // Skip if below star threshold
         if ( centerValue < m_starThreshold )
            continue;

         // Check if local maximum
         bool isMax = true;
         double neighborSum = 0;
         int neighborCount = 0;

         for ( int dy = -radius; dy <= radius && isMax; ++dy )
         {
            for ( int dx = -radius; dx <= radius && isMax; ++dx )
            {
               if ( dx == 0 && dy == 0 )
                  continue;

               double neighborValue = luminance( x + dx, y + dy, 0 );
               neighborSum += neighborValue;
               ++neighborCount;

               if ( neighborValue > centerValue )
                  isMax = false;
            }
         }

         if ( isMax )
         {
            // Star detected - create soft profile
            double neighborMean = neighborSum / neighborCount;
            double contrast = centerValue - neighborMean;

            // Mark star core and create falloff
            for ( int dy = -radius * 2; dy <= radius * 2; ++dy )
            {
               for ( int dx = -radius * 2; dx <= radius * 2; ++dx )
               {
                  int px = x + dx;
                  int py = y + dy;

                  if ( px >= 0 && px < width && py >= 0 && py < height )
                  {
                     double dist = std::sqrt( double( dx * dx + dy * dy ) );
                     double falloff = std::exp( -dist * dist / (radius * radius) );
                     double value = contrast * falloff;

                     starMask( px, py, 0 ) = std::max( double( starMask( px, py, 0 ) ), value );
                  }
               }
            }
         }
      }
   }

   return starMask;
}

// ----------------------------------------------------------------------------

Image MockSegmentationModel::CreateGradientMask( const Image& binary, double featherRadius ) const
{
   int width = binary.Width();
   int height = binary.Height();
   int radius = static_cast<int>( featherRadius + 0.5 );

   if ( radius < 1 )
      return binary;

   Image result( width, height, pcl::ColorSpace::Gray );

   // Horizontal pass
   Image temp( width, height, pcl::ColorSpace::Gray );
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
               sum += binary( sx, y, 0 );
               ++count;
            }
         }

         temp( x, y, 0 ) = sum / count;
      }
   }

   // Vertical pass
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
               sum += temp( x, sy, 0 );
               ++count;
            }
         }

         result( x, y, 0 ) = sum / count;
      }
   }

   return result;
}

// ----------------------------------------------------------------------------

SegmentationResult MockSegmentationModel::Segment( const Image& image )
{
   auto startTime = std::chrono::high_resolution_clock::now();

   SegmentationResult result;
   result.width = image.Width();
   result.height = image.Height();

   int width = image.Width();
   int height = image.Height();
   int numChannels = image.NumberOfNominalChannels();

   // Collect pixel values for percentile computation
   std::vector<float> allValues;
   allValues.reserve( static_cast<size_t>( width ) * height );

   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         float lum;
         if ( numChannels >= 3 )
         {
            lum = static_cast<float>( ComputeLuminance(
               image( x, y, 0 ), image( x, y, 1 ), image( x, y, 2 ) ) );
         }
         else
         {
            lum = static_cast<float>( image( x, y, 0 ) );
         }
         allValues.push_back( lum );
      }
   }

   // Compute percentiles for linear detection
   std::vector<float> sorted = allValues;
   std::sort( sorted.begin(), sorted.end() );
   const float p1 = sorted[static_cast<size_t>( 0.01 * (sorted.size() - 1) )];
   const float p99 = sorted[static_cast<size_t>( 0.99 * (sorted.size() - 1) )];
   const float median = sorted[sorted.size() / 2];

   // Detect if input is linear using improved heuristic:
   // - Either: p99 < 0.15 AND median < 0.1 (very compressed data)
   // - Or: median < 0.1 AND median > 0 AND (p99/median) > 5 (high dynamic range)
   const float ratio = (median > 0.0f) ? (p99 / median) : 0.0f;
   const bool isLinear = (p99 < 0.15f && median < 0.1f) ||
                         (median < 0.1f && median > 0.0f && ratio > 5.0f);

   // Debug logging to diagnose linear detection
   Console().WriteLn( String().Format(
      "MockSegmentation: Data stats - median=%.6f, p99=%.6f, ratio=%.2f, isLinear=%s",
      median, p99, ratio, isLinear ? "true" : "false" ) );
   Console().WriteLn( String().Format(
      "MockSegmentation: Thresholds - p99<0.15=%s, median<0.1=%s, median>0=%s, ratio>5=%s",
      (p99 < 0.15f) ? "true" : "false",
      (median < 0.1f) ? "true" : "false",
      (median > 0.0f) ? "true" : "false",
      (ratio > 5.0f) ? "true" : "false" ) );

   if ( isLinear )
   {
      Console().WriteLn( "MockSegmentation: Input appears to be linear data, applying arcsinh stretch" );
   }
   else
   {
      Console().WriteLn( "MockSegmentation: Input appears pre-stretched, using as-is" );
   }

   // Arcsinh stretch parameters (matching training)
   constexpr double ARCSINH_SCALE = 0.1;
   const double ARCSINH_NORM = std::asinh( 1.0 / ARCSINH_SCALE );  // asinh(10) ~ 2.998

   const float range = (p99 > p1) ? (p99 - p1) : 1.0f;

   // Create luminance image with proper preprocessing
   Image luminance( width, height, pcl::ColorSpace::Gray );

   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         float lum;
         if ( numChannels >= 3 )
         {
            lum = static_cast<float>( ComputeLuminance(
               image( x, y, 0 ), image( x, y, 1 ), image( x, y, 2 ) ) );
         }
         else
         {
            lum = static_cast<float>( image( x, y, 0 ) );
         }

         // Percentile normalization
         lum = std::clamp( (lum - p1) / range, 0.0f, 1.0f );

         // Apply arcsinh stretch if linear input
         if ( isLinear )
         {
            lum = static_cast<float>( std::clamp( std::asinh( lum / ARCSINH_SCALE ) / ARCSINH_NORM, 0.0, 1.0 ) );
         }

         luminance( x, y, 0 ) = lum;
      }
   }

   // Initialize masks for all region classes
   for ( int i = 0; i < static_cast<int>( RegionClass::Count ); ++i )
   {
      RegionClass rc = static_cast<RegionClass>( i );
      result.masks[rc].AllocateData( width, height, 1, ColorSpace::Gray );
      result.masks[rc].Zero();
   }

   // Initialize class map
   result.classMap.AllocateData( width, height, 1, ColorSpace::Gray );

   // Detect stars
   Image starMask = DetectStars( luminance );

   // Create region masks based on luminance thresholds
   // Map to new 21-class structure for mock model
   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         double lum = luminance( x, y, 0 );
         double starWeight = std::min( 1.0, starMask( x, y, 0 ) * 2.0 );

         // Determine probabilities for each class
         double probBackground = 0;
         double probStarBright = 0;    // Replaces StarCore
         double probStarMedium = 0;    // Replaces StarHalo
         double probStarFaint = 0;
         double probNebulaEmission = 0;  // Replaces NebulaBright
         double probNebulaDark = 0;      // Replaces NebulaFaint dark regions
         double probDustLane = 0;

         // Star regions - classify by brightness
         if ( starWeight > 0.5 )
         {
            if ( lum > 0.9 )
            {
               probStarBright = starWeight;
            }
            else if ( lum > 0.5 )
            {
               probStarMedium = starWeight * 0.8;
               probStarBright = starWeight * 0.2;
            }
            else
            {
               probStarFaint = starWeight * 0.7;
               probStarMedium = starWeight * 0.3;
            }
         }

         // Non-star regions based on luminance
         double nonStarWeight = 1.0 - starWeight;

         if ( lum < m_backgroundThreshold )
         {
            probBackground = nonStarWeight;

            // Check for dust lanes / dark nebula (very dark but not uniform)
            if ( lum < 0.01 )
            {
               probDustLane = nonStarWeight * 0.2;
               probNebulaDark = nonStarWeight * 0.1;
               probBackground = nonStarWeight * 0.7;
            }
         }
         else if ( lum < m_faintNebulaThreshold )
         {
            // Transition between background and dark nebula
            double t = (lum - m_backgroundThreshold) /
                       (m_faintNebulaThreshold - m_backgroundThreshold);
            probNebulaDark = nonStarWeight * t;
            probBackground = nonStarWeight * (1.0 - t);
         }
         else if ( lum < m_brightNebulaThreshold )
         {
            // Dark nebula / faint emission region
            probNebulaDark = nonStarWeight * 0.6;
            probNebulaEmission = nonStarWeight * 0.4;
         }
         else if ( lum < m_starThreshold )
         {
            // Transition to bright emission nebula
            double t = (lum - m_brightNebulaThreshold) /
                       (m_starThreshold - m_brightNebulaThreshold);
            probNebulaEmission = nonStarWeight * t;
            probNebulaDark = nonStarWeight * (1.0 - t) * 0.5;
         }
         else
         {
            // Bright emission nebula (non-stellar bright regions)
            probNebulaEmission = nonStarWeight;
         }

         // Store probabilities using new class names
         result.masks[RegionClass::Background]( x, y, 0 ) = probBackground;
         result.masks[RegionClass::StarBright]( x, y, 0 ) = probStarBright;
         result.masks[RegionClass::StarMedium]( x, y, 0 ) = probStarMedium;
         result.masks[RegionClass::StarFaint]( x, y, 0 ) = probStarFaint;
         result.masks[RegionClass::NebulaEmission]( x, y, 0 ) = probNebulaEmission;
         result.masks[RegionClass::NebulaDark]( x, y, 0 ) = probNebulaDark;
         result.masks[RegionClass::DustLane]( x, y, 0 ) = probDustLane;

         // Determine dominant class for class map
         double maxProb = probBackground;
         int maxClass = 0;

         if ( probStarBright > maxProb ) { maxProb = probStarBright; maxClass = 1; }
         if ( probStarMedium > maxProb ) { maxProb = probStarMedium; maxClass = 2; }
         if ( probStarFaint > maxProb ) { maxProb = probStarFaint; maxClass = 3; }
         if ( probNebulaEmission > maxProb ) { maxProb = probNebulaEmission; maxClass = 5; }
         if ( probNebulaDark > maxProb ) { maxProb = probNebulaDark; maxClass = 7; }
         if ( probDustLane > maxProb ) { maxProb = probDustLane; maxClass = 13; }

         result.classMap( x, y, 0 ) = static_cast<double>( maxClass ) / 20.0;  // 21 classes (0-20)
      }
   }

   // Apply smoothing if configured
   if ( m_config.smoothingRadius > 0 )
   {
      for ( auto& pair : result.masks )
      {
         pair.second = CreateGradientMask( pair.second, m_config.smoothingRadius );
      }
   }

   auto endTime = std::chrono::high_resolution_clock::now();
   result.processingTimeMs = std::chrono::duration<double, std::milli>(
      endTime - startTime ).count();

   result.isValid = true;

   return result;
}

// ----------------------------------------------------------------------------
// ONNXSegmentationModel Implementation
// ----------------------------------------------------------------------------

ONNXSegmentationModel::ONNXSegmentationModel()
   : m_session( std::make_unique<ONNXSession>() )
{
}

// ----------------------------------------------------------------------------

ONNXSegmentationModel::~ONNXSegmentationModel()
{
}

// ----------------------------------------------------------------------------

bool ONNXSegmentationModel::Initialize( const SegmentationConfig& config )
{
   m_config = config;
   m_isReady = false;

   if ( config.modelPath.IsEmpty() )
   {
      m_lastError = "No model path specified";
      return false;
   }

   // Configure ONNX session
   ONNXConfig onnxConfig;
   onnxConfig.useGPU = config.useGPU;
   onnxConfig.numThreads = 0;  // Auto

   if ( !m_session->LoadModel( config.modelPath, onnxConfig ) )
   {
      m_lastError = "Failed to load model: " + m_session->GetLastError();
      return false;
   }

   // Get input info to determine number of expected channels
   auto inputs = m_session->GetInputInfo();
   if ( !inputs.empty() && inputs[0].shape.Rank() >= 2 )
   {
      // Assuming NCHW format, dim[1] is the channel dimension
      m_numInputChannels = static_cast<int>( inputs[0].shape[1] );
      // Validate - should be 3 (RGB) or 4 (RGB + ColorContrast)
      if ( m_numInputChannels != 3 && m_numInputChannels != 4 )
      {
         Console().WarningLn( String().Format(
            "ONNXSegmentationModel: Unexpected input channels %d, defaulting to 3",
            m_numInputChannels ) );
         m_numInputChannels = 3;
      }
   }

   // Get output info to determine number of classes
   auto outputs = m_session->GetOutputInfo();
   if ( !outputs.empty() && outputs[0].shape.Rank() >= 2 )
   {
      // Assuming NCHW format, channel dimension is the number of classes
      m_numClasses = static_cast<int>( outputs[0].shape[1] );
   }

   m_isReady = true;
   Console().WriteLn( String().Format( "ONNXSegmentationModel: Loaded with %d input channels, %d classes",
                                        m_numInputChannels, m_numClasses ) );

   return true;
}

// ----------------------------------------------------------------------------

FloatTensor ONNXSegmentationModel::PreprocessImage( const Image& image ) const
{
   // Create tensor matching the model's expected input channels:
   // - 3 channels: R, G, B
   // - 4 channels: R, G, B, ColorContrast (B - R, range -1 to 1)
   //
   // Preprocessing pipeline (matching training from process_qnap_data.py):
   // 1. Resample to model input size (bilinear interpolation)
   // 2. Detect if input is linear or pre-stretched
   // 3. If linear: apply percentile normalization then arcsinh stretch
   // 4. If pre-stretched: use as-is with percentile normalization
   // 5. Create color contrast channel: B - R (only if 4 channels expected)
   //
   // Training data was created with:
   //   p1, p99 = np.percentile(data, [1, 99])
   //   normalized = (data - p1) / (p99 - p1)
   //   stretched = arcsinh(normalized / 0.1) / arcsinh(10)

   const int numChannels = m_numInputChannels;  // Use detected value (3 or 4)
   constexpr double PERCENTILE_LOW = 0.01;   // 1st percentile
   constexpr double PERCENTILE_HIGH = 0.99;  // 99th percentile

   const int srcWidth = image.Width();
   const int srcHeight = image.Height();
   const int outWidth = m_config.inputWidth;
   const int outHeight = m_config.inputHeight;

   // Validate dimensions to prevent allocation crashes
   if ( outWidth <= 0 || outHeight <= 0 || outWidth > 16384 || outHeight > 16384 )
      throw std::runtime_error( "Invalid segmentation model input dimensions" );

   const size_t numPixels = static_cast<size_t>( outWidth ) * outHeight;
   if ( numPixels > 268435456 )  // 256M pixels max for segmentation
      throw std::runtime_error( "Segmentation input too large" );

   // Scale factors for resampling
   const double scaleX = static_cast<double>( srcWidth ) / outWidth;
   const double scaleY = static_cast<double>( srcHeight ) / outHeight;

   // Temporary storage for resampled RGB channels
   std::vector<float> channelR( numPixels );
   std::vector<float> channelG( numPixels );
   std::vector<float> channelB( numPixels );

   // Step 1: Resample using bilinear interpolation
   for ( int y = 0; y < outHeight; ++y )
   {
      for ( int x = 0; x < outWidth; ++x )
      {
         // Bilinear sampling coordinates
         const double srcX = x * scaleX;
         const double srcY = y * scaleY;

         const int x0 = static_cast<int>( srcX );
         const int y0 = static_cast<int>( srcY );
         const int x1 = std::min( x0 + 1, srcWidth - 1 );
         const int y1 = std::min( y0 + 1, srcHeight - 1 );

         const double fx = srcX - x0;
         const double fy = srcY - y0;

         // Bilinear interpolation lambda
         auto bilinear = [&]( int c ) {
            const double v00 = image( x0, y0, c );
            const double v10 = image( x1, y0, c );
            const double v01 = image( x0, y1, c );
            const double v11 = image( x1, y1, c );
            return (1.0 - fx) * (1.0 - fy) * v00 +
                   fx * (1.0 - fy) * v10 +
                   (1.0 - fx) * fy * v01 +
                   fx * fy * v11;
         };

         // Resample
         const size_t idx = static_cast<size_t>( y ) * outWidth + x;
         channelR[idx] = static_cast<float>( bilinear( 0 ) );
         channelG[idx] = static_cast<float>( bilinear( 1 ) );
         channelB[idx] = static_cast<float>( bilinear( 2 ) );
      }
   }

   // Step 2: Compute percentiles for normalization and linear detection
   // Lambda to compute percentile from sorted data
   auto computePercentile = []( std::vector<float>& data, double percentile ) {
      std::vector<float> sorted = data;
      std::sort( sorted.begin(), sorted.end() );
      const size_t idx = static_cast<size_t>( percentile * (sorted.size() - 1) );
      return sorted[idx];
   };

   // Compute percentiles for each RGB channel
   const float r_p1 = computePercentile( channelR, PERCENTILE_LOW );
   const float r_p99 = computePercentile( channelR, PERCENTILE_HIGH );
   const float g_p1 = computePercentile( channelG, PERCENTILE_LOW );
   const float g_p99 = computePercentile( channelG, PERCENTILE_HIGH );
   const float b_p1 = computePercentile( channelB, PERCENTILE_LOW );
   const float b_p99 = computePercentile( channelB, PERCENTILE_HIGH );

   // Compute median for linear detection (use green channel as proxy for luminance)
   const float g_median = computePercentile( channelG, 0.5 );

   // Detect if input is linear or already stretched using improved heuristic:
   // - Either: p99 < 0.15 AND median < 0.1 (very compressed data)
   // - Or: median < 0.1 AND median > 0 AND (p99/median) > 5 (high dynamic range)
   const float ratio = (g_median > 0.0f) ? (g_p99 / g_median) : 0.0f;
   const bool isLinear = (g_p99 < 0.15f && g_median < 0.1f) ||
                         (g_median < 0.1f && g_median > 0.0f && ratio > 5.0f);

   // Debug logging to diagnose linear detection
   Console().WriteLn( String().Format(
      "Segmentation: Data stats - median=%.6f, p99=%.6f, ratio=%.2f, isLinear=%s",
      g_median, g_p99, ratio, isLinear ? "true" : "false" ) );
   Console().WriteLn( String().Format(
      "Segmentation: Thresholds - p99<0.15=%s, median<0.1=%s, median>0=%s, ratio>5=%s",
      (g_p99 < 0.15f) ? "true" : "false",
      (g_median < 0.1f) ? "true" : "false",
      (g_median > 0.0f) ? "true" : "false",
      (ratio > 5.0f) ? "true" : "false" ) );

   if ( isLinear )
   {
      Console().WriteLn( "Segmentation: Input appears to be linear data, applying arcsinh stretch" );
   }
   else
   {
      Console().WriteLn( "Segmentation: Input appears pre-stretched, using as-is" );
   }

   // Avoid division by zero
   const float r_range = (r_p99 > r_p1) ? (r_p99 - r_p1) : 1.0f;
   const float g_range = (g_p99 > g_p1) ? (g_p99 - g_p1) : 1.0f;
   const float b_range = (b_p99 > b_p1) ? (b_p99 - b_p1) : 1.0f;

   // Arcsinh stretch parameters (matching training)
   constexpr double ARCSINH_SCALE = 0.1;
   const double ARCSINH_NORM = std::asinh( 1.0 / ARCSINH_SCALE );  // asinh(10) ~ 2.998

   // Create output tensor
   TensorShape shape = { 1, numChannels, outHeight, outWidth };
   FloatTensor tensor( shape );

   // Step 3: Apply percentile normalization, arcsinh stretch (if linear), and create tensor
   for ( int y = 0; y < outHeight; ++y )
   {
      for ( int x = 0; x < outWidth; ++x )
      {
         const size_t srcIdx = static_cast<size_t>( y ) * outWidth + x;

         // First: percentile normalization to [0, 1]
         float r = std::clamp( (channelR[srcIdx] - r_p1) / r_range, 0.0f, 1.0f );
         float g = std::clamp( (channelG[srcIdx] - g_p1) / g_range, 0.0f, 1.0f );
         float b = std::clamp( (channelB[srcIdx] - b_p1) / b_range, 0.0f, 1.0f );

         // If linear input, apply arcsinh stretch (matching training)
         if ( isLinear )
         {
            r = static_cast<float>( std::clamp( std::asinh( r / ARCSINH_SCALE ) / ARCSINH_NORM, 0.0, 1.0 ) );
            g = static_cast<float>( std::clamp( std::asinh( g / ARCSINH_SCALE ) / ARCSINH_NORM, 0.0, 1.0 ) );
            b = static_cast<float>( std::clamp( std::asinh( b / ARCSINH_SCALE ) / ARCSINH_NORM, 0.0, 1.0 ) );
         }

         // Store in tensor (NCHW format)
         tensor[(0 * outHeight + y) * outWidth + x] = r;               // Channel 0: R
         tensor[(1 * outHeight + y) * outWidth + x] = g;               // Channel 1: G
         tensor[(2 * outHeight + y) * outWidth + x] = b;               // Channel 2: B

         // Only add color contrast channel if model expects 4 channels
         if ( numChannels == 4 )
         {
            // Color contrast: B - R (range -1 to 1, no clamping needed for model)
            const float colorContrast = b - r;
            tensor[(3 * outHeight + y) * outWidth + x] = colorContrast;   // Channel 3: B - R
         }
      }
   }

   return tensor;
}

// ----------------------------------------------------------------------------

SegmentationResult ONNXSegmentationModel::PostprocessOutput( const FloatTensor& output,
                                                              int originalWidth,
                                                              int originalHeight ) const
{
   SegmentationResult result;
   result.width = originalWidth;
   result.height = originalHeight;

   if ( output.shape.Rank() != 4 )
   {
      result.errorMessage = "Unexpected output shape";
      return result;
   }

   int outChannels = static_cast<int>( output.shape[1] );
   int outHeight = static_cast<int>( output.shape[2] );
   int outWidth = static_cast<int>( output.shape[3] );

   // Apply softmax if needed
   FloatTensor processed = output;
   if ( m_config.applySoftmax )
   {
      ONNXUtils::SoftmaxChannels( processed );
   }

   // Resize to original dimensions and create masks
   double scaleX = static_cast<double>( outWidth ) / originalWidth;
   double scaleY = static_cast<double>( outHeight ) / originalHeight;

   // Initialize masks for mapped region classes
   for ( size_t c = 0; c < m_config.channelMapping.size() && c < size_t( outChannels ); ++c )
   {
      RegionClass rc = m_config.channelMapping[c];
      result.masks[rc].AllocateData( originalWidth, originalHeight, 1, ColorSpace::Gray );
   }

   // Class map
   result.classMap.AllocateData( originalWidth, originalHeight, 1, ColorSpace::Gray );

   // Resample to original size
   for ( int y = 0; y < originalHeight; ++y )
   {
      for ( int x = 0; x < originalWidth; ++x )
      {
         // Bilinear sampling from output
         double srcX = x * scaleX;
         double srcY = y * scaleY;

         int x0 = static_cast<int>( srcX );
         int y0 = static_cast<int>( srcY );
         int x1 = std::min( x0 + 1, outWidth - 1 );
         int y1 = std::min( y0 + 1, outHeight - 1 );

         double fx = srcX - x0;
         double fy = srcY - y0;

         double maxProb = 0;
         int maxClass = 0;

         for ( int c = 0; c < outChannels && c < static_cast<int>( m_config.channelMapping.size() ); ++c )
         {
            // Bilinear interpolation
            size_t idx00 = (c * outHeight + y0) * outWidth + x0;
            size_t idx10 = (c * outHeight + y0) * outWidth + x1;
            size_t idx01 = (c * outHeight + y1) * outWidth + x0;
            size_t idx11 = (c * outHeight + y1) * outWidth + x1;

            double prob = (1 - fx) * (1 - fy) * processed[idx00] +
                          fx * (1 - fy) * processed[idx10] +
                          (1 - fx) * fy * processed[idx01] +
                          fx * fy * processed[idx11];

            RegionClass rc = m_config.channelMapping[c];

            // Apply threshold
            if ( prob >= m_config.probabilityThreshold )
            {
               result.masks[rc]( x, y, 0 ) = prob;
            }

            if ( prob > maxProb )
            {
               maxProb = prob;
               maxClass = c;
            }
         }

         result.classMap( x, y, 0 ) = static_cast<double>( maxClass ) / (outChannels - 1);
      }
   }

   result.isValid = true;
   return result;
}

// ----------------------------------------------------------------------------

SegmentationResult ONNXSegmentationModel::Segment( const Image& image )
{
   auto startTime = std::chrono::high_resolution_clock::now();

   SegmentationResult result;
   result.width = image.Width();
   result.height = image.Height();

   if ( !m_isReady )
   {
      result.errorMessage = "Model not initialized";
      return result;
   }

   try
   {
      // Preprocess
      FloatTensor input = PreprocessImage( image );

      // Run inference
      FloatTensor output;
      if ( !m_session->Run( input, output ) )
      {
         result.errorMessage = "Inference failed: " + m_session->GetLastError();
         return result;
      }

      // Postprocess
      result = PostprocessOutput( output, image.Width(), image.Height() );
   }
   catch ( const std::exception& e )
   {
      result.errorMessage = String( "Exception: " ) + e.what();
   }

   auto endTime = std::chrono::high_resolution_clock::now();
   result.processingTimeMs = std::chrono::duration<double, std::milli>(
      endTime - startTime ).count();

   return result;
}

// ----------------------------------------------------------------------------
// SegmentationModelFactory Implementation
// ----------------------------------------------------------------------------

std::unique_ptr<ISegmentationModel> SegmentationModelFactory::Create( ModelType type )
{
   switch ( type )
   {
   case ModelType::Mock:
      return std::make_unique<MockSegmentationModel>();

   case ModelType::ONNX:
      if ( IsONNXAvailable() )
         return std::make_unique<ONNXSegmentationModel>();
      else
         return nullptr;

   case ModelType::Auto:
      if ( IsONNXAvailable() )
         return std::make_unique<ONNXSegmentationModel>();
      else
         return std::make_unique<MockSegmentationModel>();

   default:
      return std::make_unique<MockSegmentationModel>();
   }
}

// ----------------------------------------------------------------------------

std::unique_ptr<ISegmentationModel> SegmentationModelFactory::CreateWithFallback(
   const SegmentationConfig& config )
{
   Console console;
   console.WriteLn( String().Format( "CreateWithFallback: modelPath='%s', IsONNXAvailable=%s",
      IsoString( config.modelPath ).c_str(),
      IsONNXAvailable() ? "true" : "false" ) );

   // Try ONNX first if model path is provided
   if ( !config.modelPath.IsEmpty() && IsONNXAvailable() )
   {
      console.WriteLn( "Attempting to load ONNX model..." );
      auto model = std::make_unique<ONNXSegmentationModel>();
      if ( model->Initialize( config ) )
      {
         console.WriteLn( "Using ONNX segmentation model" );
         return model;
      }
      console.WarningLn( String().Format( "ONNX model failed to load: %s", IsoString( model->GetLastError() ).c_str() ) );
   }

   // Fall back to mock
   auto model = std::make_unique<MockSegmentationModel>();
   model->Initialize( config );
   IsoString pathInfo = config.modelPath.IsEmpty() ? IsoString( "<empty>" ) : IsoString( config.modelPath );
   console.WriteLn( "*** ONNX DEBUG ***" );
   console.WriteLn( String().Format( "  Model path: %s", pathInfo.c_str() ) );
   console.WriteLn( String().Format( "  ONNX available: %s", IsONNXAvailable() ? "YES" : "NO" ) );
   console.WriteLn( "Using mock (threshold-based) segmentation" );
   return model;
}

// ----------------------------------------------------------------------------

bool SegmentationModelFactory::IsONNXAvailable()
{
   return ONNXSession::IsRuntimeAvailable();
}

// ----------------------------------------------------------------------------

} // namespace pcl
