//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "SegmentationModel.h"
#include "Constants.h"

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

   // Validate model class count matches channel mapping
   if ( m_numClasses != static_cast<int>( m_config.channelMapping.size() ) )
   {
      Console().WarningLn( String().Format(
         "** Warning: ONNX model has %d output classes but channel mapping has %d entries. "
         "Results may be incorrect.", m_numClasses, static_cast<int>( m_config.channelMapping.size() ) ) );

      // If model has fewer classes, truncate mapping
      if ( m_numClasses < static_cast<int>( m_config.channelMapping.size() ) )
      {
         m_config.channelMapping.resize( m_numClasses );
         Console().WarningLn( "   Channel mapping truncated to match model." );
      }
      // If model has more classes, the extra channels will be ignored
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
   const int imageChannels = image.NumberOfChannels();
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
            return Interpolation::BilinearInterpolate( v00, v10, v01, v11, fx, fy );
         };

         // Resample
         const size_t idx = static_cast<size_t>( y ) * outWidth + x;
         channelR[idx] = static_cast<float>( bilinear( 0 ) );
         channelG[idx] = static_cast<float>( bilinear( imageChannels >= 3 ? 1 : 0 ) );
         channelB[idx] = static_cast<float>( bilinear( imageChannels >= 3 ? 2 : 0 ) );
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

   // Adaptive arcsinh stretch parameters based on data compression
   double ARCSINH_SCALE = 0.1;  // default for normal data
   if ( isLinear && ratio < 2.0 )
   {
      // Very compressed data needs stronger stretch
      ARCSINH_SCALE = 0.02;  // 5x more aggressive
      Console().WarningLn( String().Format(
         "Segmentation: Extremely compressed linear data (ratio=%.2f), using aggressive stretch (scale=%.3f)",
         ratio, ARCSINH_SCALE ) );
   }
   else if ( isLinear && ratio < 5.0 )
   {
      // Moderately compressed data
      ARCSINH_SCALE = 0.05;  // 2x more aggressive
      Console().WriteLn( String().Format(
         "Segmentation: Moderately compressed linear data, applying arcsinh stretch (scale=%.3f)",
         ARCSINH_SCALE ) );
   }
   else if ( isLinear )
   {
      Console().WriteLn( String().Format(
         "Segmentation: Input appears to be linear data, applying arcsinh stretch (scale=%.3f)",
         ARCSINH_SCALE ) );
   }
   else
   {
      Console().WriteLn( "Segmentation: Input appears pre-stretched, using as-is" );
   }

   // Avoid division by zero
   const float r_range = (r_p99 > r_p1) ? (r_p99 - r_p1) : 1.0f;
   const float g_range = (g_p99 > g_p1) ? (g_p99 - g_p1) : 1.0f;
   const float b_range = (b_p99 > b_p1) ? (b_p99 - b_p1) : 1.0f;

   const double ARCSINH_NORM = std::asinh( 1.0 / ARCSINH_SCALE );

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

            double prob = Interpolation::BilinearInterpolate(
                             processed[idx00], processed[idx10],
                             processed[idx01], processed[idx11], fx, fy );

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

std::unique_ptr<ISegmentationModel> SegmentationModelFactory::Create()
{
   if ( IsONNXAvailable() )
      return std::make_unique<ONNXSegmentationModel>();
   return nullptr;
}

// ----------------------------------------------------------------------------

bool SegmentationModelFactory::IsONNXAvailable()
{
   return ONNXSession::IsRuntimeAvailable();
}

// ----------------------------------------------------------------------------

} // namespace pcl
