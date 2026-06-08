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

   // Create luminance image
   Image luminance( width, height, pcl::ColorSpace::Gray );

   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         if ( numChannels >= 3 )
         {
            luminance( x, y, 0 ) = ComputeLuminance(
               image( x, y, 0 ), image( x, y, 1 ), image( x, y, 2 ) );
         }
         else
         {
            luminance( x, y, 0 ) = image( x, y, 0 );
         }
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

   // Get output info to determine number of classes
   auto outputs = m_session->GetOutputInfo();
   if ( !outputs.empty() && outputs[0].shape.Rank() >= 2 )
   {
      // Assuming NCHW format, channel dimension is the number of classes
      m_numClasses = static_cast<int>( outputs[0].shape[1] );
   }

   m_isReady = true;
   Console().WriteLn( String().Format( "ONNXSegmentationModel: Loaded with %d classes",
                                        m_numClasses ) );

   return true;
}

// ----------------------------------------------------------------------------

FloatTensor ONNXSegmentationModel::PreprocessImage( const Image& image ) const
{
   // Create 11-channel multi-spectral tensor for the model
   // Channels: R, G, B, L, Ha, OIII, SII, Continuum, Saturation, Hue, EmissionStrength

   int srcWidth = image.Width();
   int srcHeight = image.Height();
   int outWidth = m_config.inputWidth;
   int outHeight = m_config.inputHeight;

   constexpr int NUM_CHANNELS = 11;
   TensorShape shape = { 1, NUM_CHANNELS, outHeight, outWidth };
   FloatTensor tensor( shape );

   // Scale factors for resampling
   double scaleX = static_cast<double>( srcWidth ) / outWidth;
   double scaleY = static_cast<double>( srcHeight ) / outHeight;

   for ( int y = 0; y < outHeight; ++y )
   {
      for ( int x = 0; x < outWidth; ++x )
      {
         // Bilinear sampling coordinates
         double srcX = x * scaleX;
         double srcY = y * scaleY;

         int x0 = static_cast<int>( srcX );
         int y0 = static_cast<int>( srcY );
         int x1 = std::min( x0 + 1, srcWidth - 1 );
         int y1 = std::min( y0 + 1, srcHeight - 1 );

         double fx = srcX - x0;
         double fy = srcY - y0;

         // Bilinear interpolation for R, G, B
         auto bilinear = [&]( int c ) {
            double v00 = image( x0, y0, c );
            double v10 = image( x1, y0, c );
            double v01 = image( x0, y1, c );
            double v11 = image( x1, y1, c );
            return (1 - fx) * (1 - fy) * v00 +
                   fx * (1 - fy) * v10 +
                   (1 - fx) * fy * v01 +
                   fx * fy * v11;
         };

         // Get RGB values (already 0-1 in PixInsight)
         double r = bilinear( 0 );
         double g = bilinear( 1 );
         double b = bilinear( 2 );

         // Channel 0: R
         tensor[(0 * outHeight + y) * outWidth + x] = static_cast<float>( r );

         // Channel 1: G
         tensor[(1 * outHeight + y) * outWidth + x] = static_cast<float>( g );

         // Channel 2: B
         tensor[(2 * outHeight + y) * outWidth + x] = static_cast<float>( b );

         // Channel 3: Luminance (ITU-R BT.709)
         double lum = 0.2126 * r + 0.7152 * g + 0.0722 * b;
         tensor[(3 * outHeight + y) * outWidth + x] = static_cast<float>( lum );

         // Channel 4: Ha proxy (r - 0.5*(g+b) + 0.5, clipped)
         double ha = r - 0.5 * (g + b) + 0.5;
         tensor[(4 * outHeight + y) * outWidth + x] = static_cast<float>( std::clamp( ha, 0.0, 1.0 ) );

         // Channel 5: OIII proxy (0.5*(g+b) - 0.5*r + 0.5, clipped)
         double oiii = 0.5 * (g + b) - 0.5 * r + 0.5;
         tensor[(5 * outHeight + y) * outWidth + x] = static_cast<float>( std::clamp( oiii, 0.0, 1.0 ) );

         // Channel 6: SII proxy (r - 0.7*g - 0.3*b + 0.5, clipped)
         double sii = r - 0.7 * g - 0.3 * b + 0.5;
         tensor[(6 * outHeight + y) * outWidth + x] = static_cast<float>( std::clamp( sii, 0.0, 1.0 ) );

         // Channel 7: Continuum (1 - color_variance * 10, clipped)
         double mean = (r + g + b) / 3.0;
         double variance = ((r - mean) * (r - mean) + (g - mean) * (g - mean) + (b - mean) * (b - mean)) / 3.0;
         double continuum = 1.0 - std::min( variance * 10.0, 1.0 );
         tensor[(7 * outHeight + y) * outWidth + x] = static_cast<float>( continuum );

         // Channel 8: Saturation
         double maxRGB = std::max( { r, g, b } );
         double minRGB = std::min( { r, g, b } );
         double saturation = (maxRGB > 0) ? (maxRGB - minRGB) / maxRGB : 0;
         tensor[(8 * outHeight + y) * outWidth + x] = static_cast<float>( saturation );

         // Channel 9: Hue (simplified HSV hue normalized to 0-1)
         double hue = 0;
         double delta = maxRGB - minRGB;
         if ( delta > 0.001 )
         {
            if ( maxRGB == r )
               hue = std::fmod( (g - b) / delta, 6.0 );
            else if ( maxRGB == g )
               hue = (b - r) / delta + 2.0;
            else
               hue = (r - g) / delta + 4.0;
            hue /= 6.0;
            if ( hue < 0 ) hue += 1.0;
         }
         tensor[(9 * outHeight + y) * outWidth + x] = static_cast<float>( hue );

         // Channel 10: EmissionStrength (saturation * luminance)
         tensor[(10 * outHeight + y) * outWidth + x] = static_cast<float>( saturation * lum );
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
      config.modelPath.c_str(),
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
      console.WarningLn( String().Format( "ONNX model failed to load: %s", model->GetLastError().c_str() ) );
   }

   // Fall back to mock
   auto model = std::make_unique<MockSegmentationModel>();
   model->Initialize( config );
   String pathInfo = config.modelPath.IsEmpty() ? String( "<empty>" ) : config.modelPath;
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
