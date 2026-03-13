// ----------------------------------------------------------------------------
// ArtifactDetector.cpp — Trail detection on stretched (nonlinear) images
//
// Detects satellite/airplane trails using Hough transform on Sobel edge maps.
// Dust mote and vignetting detectors are stubbed for future phases.
//
// Copyright (c) 2026 Scott Carter
// ----------------------------------------------------------------------------

#include "engine/ArtifactDetector.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <vector>

namespace nukex {

// ============================================================================
// Construction
// ============================================================================

ArtifactDetector::ArtifactDetector( const ArtifactDetectorConfig& config )
   : m_config( config )
{
}

// ============================================================================
// Background estimation — block median on a coarse grid, bilinear interpolation
// ============================================================================

std::vector<float> ArtifactDetector::estimateBackground( const float* image, int width, int height ) const
{
   const int bs = m_config.trailBlockSize;
   const int gx = ( width + bs - 1 ) / bs;    // number of grid cells in x
   const int gy = ( height + bs - 1 ) / bs;   // number of grid cells in y

   // Compute median of each block
   std::vector<float> gridMedian( gx * gy, 0.0f );
   std::vector<float> blockPixels;
   blockPixels.reserve( bs * bs );

   for ( int by = 0; by < gy; ++by )
   {
      for ( int bx = 0; bx < gx; ++bx )
      {
         blockPixels.clear();
         int y0 = by * bs;
         int y1 = std::min( y0 + bs, height );
         int x0 = bx * bs;
         int x1 = std::min( x0 + bs, width );
         for ( int y = y0; y < y1; ++y )
            for ( int x = x0; x < x1; ++x )
               blockPixels.push_back( image[y * width + x] );

         if ( !blockPixels.empty() )
         {
            size_t mid = blockPixels.size() / 2;
            std::nth_element( blockPixels.begin(), blockPixels.begin() + mid, blockPixels.end() );
            gridMedian[by * gx + bx] = blockPixels[mid];
         }
      }
   }

   // Bilinear interpolation to full resolution
   // Grid cell centers are at (bx * bs + bs/2, by * bs + bs/2)
   std::vector<float> bg( width * height );
   const double halfBlock = bs * 0.5;

   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         // Continuous grid coordinates
         double gxf = ( x - halfBlock ) / bs;
         double gyf = ( y - halfBlock ) / bs;

         // Clamp to grid bounds
         gxf = std::max( 0.0, std::min( gxf, static_cast<double>( gx - 1 ) ) );
         gyf = std::max( 0.0, std::min( gyf, static_cast<double>( gy - 1 ) ) );

         int gx0 = static_cast<int>( gxf );
         int gy0 = static_cast<int>( gyf );
         int gx1 = std::min( gx0 + 1, gx - 1 );
         int gy1 = std::min( gy0 + 1, gy - 1 );

         double fx = gxf - gx0;
         double fy = gyf - gy0;

         double v00 = gridMedian[gy0 * gx + gx0];
         double v10 = gridMedian[gy0 * gx + gx1];
         double v01 = gridMedian[gy1 * gx + gx0];
         double v11 = gridMedian[gy1 * gx + gx1];

         bg[y * width + x] = static_cast<float>(
            v00 * ( 1 - fx ) * ( 1 - fy ) +
            v10 * fx * ( 1 - fy ) +
            v01 * ( 1 - fx ) * fy +
            v11 * fx * fy );
      }
   }

   return bg;
}

// ============================================================================
// Sobel edge detection — gradient magnitude
// ============================================================================

std::vector<float> ArtifactDetector::sobelMagnitude( const float* image, int width, int height ) const
{
   std::vector<float> mag( width * height, 0.0f );

   for ( int y = 1; y < height - 1; ++y )
   {
      for ( int x = 1; x < width - 1; ++x )
      {
         // Sobel 3x3 kernels
         float gx = -image[( y - 1 ) * width + ( x - 1 )] + image[( y - 1 ) * width + ( x + 1 )]
                   - 2.0f * image[y * width + ( x - 1 )]   + 2.0f * image[y * width + ( x + 1 )]
                   - image[( y + 1 ) * width + ( x - 1 )]  + image[( y + 1 ) * width + ( x + 1 )];

         float gy = -image[( y - 1 ) * width + ( x - 1 )] - 2.0f * image[( y - 1 ) * width + x]
                   - image[( y - 1 ) * width + ( x + 1 )]
                   + image[( y + 1 ) * width + ( x - 1 )] + 2.0f * image[( y + 1 ) * width + x]
                   + image[( y + 1 ) * width + ( x + 1 )];

         mag[y * width + x] = std::sqrt( gx * gx + gy * gy );
      }
   }

   return mag;
}

// ============================================================================
// Hough transform — detect lines in a binary edge mask
// ============================================================================

std::vector<HoughLine> ArtifactDetector::houghLines( const uint8_t* edgeMask, int width, int height ) const
{
   const int nTheta = m_config.houghThetaBins;
   const double maxRho = std::sqrt( static_cast<double>( width * width + height * height ) );
   const int nRho = static_cast<int>( 2.0 * maxRho ) + 1;
   const double rhoOffset = maxRho;   // shift so rho index is always >= 0

   // Pre-compute cos/sin tables
   std::vector<double> cosTable( nTheta );
   std::vector<double> sinTable( nTheta );
   for ( int t = 0; t < nTheta; ++t )
   {
      double theta = M_PI * t / nTheta;
      cosTable[t] = std::cos( theta );
      sinTable[t] = std::sin( theta );
   }

   // Accumulator
   std::vector<int> accumulator( nRho * nTheta, 0 );

   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         if ( edgeMask[y * width + x] == 0 )
            continue;
         for ( int t = 0; t < nTheta; ++t )
         {
            double rho = x * cosTable[t] + y * sinTable[t];
            int rhoIdx = static_cast<int>( std::round( rho + rhoOffset ) );
            if ( rhoIdx >= 0 && rhoIdx < nRho )
               accumulator[rhoIdx * nTheta + t]++;
         }
      }
   }

   // Peak detection: threshold = max(100, min(W,H)/4)
   int peakThreshold = std::max( 100, std::min( width, height ) / 4 );

   // Collect peaks above threshold
   struct RawPeak { double rho; double theta; int votes; };
   std::vector<RawPeak> peaks;

   for ( int ri = 0; ri < nRho; ++ri )
   {
      for ( int ti = 0; ti < nTheta; ++ti )
      {
         int v = accumulator[ri * nTheta + ti];
         if ( v >= peakThreshold )
            peaks.push_back( { ri - rhoOffset, M_PI * ti / nTheta, v } );
      }
   }

   // Sort by votes descending for clustering
   std::sort( peaks.begin(), peaks.end(),
              []( const RawPeak& a, const RawPeak& b ) { return a.votes > b.votes; } );

   // Cluster peaks: merge within 5 rho, 3 degrees
   const double rhoMerge = 5.0;
   const double thetaMerge = 3.0 * M_PI / 180.0;
   std::vector<HoughLine> lines;
   std::vector<bool> merged( peaks.size(), false );

   for ( size_t i = 0; i < peaks.size(); ++i )
   {
      if ( merged[i] )
         continue;

      double sumRho = peaks[i].rho * peaks[i].votes;
      double sumTheta = peaks[i].theta * peaks[i].votes;
      int sumVotes = peaks[i].votes;

      for ( size_t j = i + 1; j < peaks.size(); ++j )
      {
         if ( merged[j] )
            continue;
         if ( std::abs( peaks[i].rho - peaks[j].rho ) <= rhoMerge &&
              std::abs( peaks[i].theta - peaks[j].theta ) <= thetaMerge )
         {
            sumRho += peaks[j].rho * peaks[j].votes;
            sumTheta += peaks[j].theta * peaks[j].votes;
            sumVotes += peaks[j].votes;
            merged[j] = true;
         }
      }

      lines.push_back( { sumRho / sumVotes, sumTheta / sumVotes, sumVotes } );
   }

   return lines;
}

// ============================================================================
// Trail mask generation — distance from detected lines, dilated
// ============================================================================

std::vector<uint8_t> ArtifactDetector::generateTrailMask( const std::vector<HoughLine>& lines,
                                                           int width, int height,
                                                           double dilateRadius ) const
{
   std::vector<uint8_t> mask( width * height, 0 );
   if ( lines.empty() )
      return mask;

   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         for ( const auto& line : lines )
         {
            // Signed distance from point to line: rho = x*cos(theta) + y*sin(theta)
            double d = std::abs( x * std::cos( line.theta ) + y * std::sin( line.theta ) - line.rho );
            if ( d <= dilateRadius )
            {
               mask[y * width + x] = 1;
               break;   // one line is enough to mask the pixel
            }
         }
      }
   }

   return mask;
}

// ============================================================================
// detectTrails — full pipeline
// ============================================================================

TrailDetectionResult ArtifactDetector::detectTrails( const float* image, int width, int height ) const
{
   TrailDetectionResult result;
   result.mask.resize( width * height, 0 );

   if ( width < 16 || height < 16 )
      return result;

   // 1. Background estimation
   std::vector<float> bg = estimateBackground( image, width, height );

   // 2. Residual: image - background, clamped to 0
   std::vector<float> residual( width * height );
   for ( int i = 0; i < width * height; ++i )
      residual[i] = std::max( 0.0f, image[i] - bg[i] );

   // 3. Sobel edge detection
   std::vector<float> grad = sobelMagnitude( residual.data(), width, height );

   // 4. Threshold: median + sigma * MAD of gradient
   //    Use all interior pixels (y in [1,H-2], x in [1,W-2]) including zeros,
   //    so flat background establishes the noise floor and trail edges are outliers.
   {
      std::vector<float> gradValues;
      gradValues.reserve( ( width - 2 ) * ( height - 2 ) );
      for ( int y = 1; y < height - 1; ++y )
         for ( int x = 1; x < width - 1; ++x )
            gradValues.push_back( grad[y * width + x] );

      if ( gradValues.empty() )
         return result;

      // Median
      size_t mid = gradValues.size() / 2;
      std::nth_element( gradValues.begin(), gradValues.begin() + mid, gradValues.end() );
      double median = gradValues[mid];

      // MAD (median absolute deviation)
      std::vector<float> absDevs( gradValues.size() );
      for ( size_t i = 0; i < gradValues.size(); ++i )
         absDevs[i] = std::abs( gradValues[i] - static_cast<float>( median ) );
      size_t madMid = absDevs.size() / 2;
      std::nth_element( absDevs.begin(), absDevs.begin() + madMid, absDevs.end() );
      double mad = absDevs[madMid] * 1.4826;   // scale to Gaussian sigma

      // When MAD is zero (perfectly flat background), any gradient is a real edge.
      // Use a small floor so the sigma multiplier still produces a sensible threshold.
      double threshold;
      if ( mad < 1e-10 )
         threshold = median + 1e-6;  // just above the (zero) noise floor
      else
         threshold = median + m_config.trailOutlierSigma * mad;

      // Create binary edge mask
      std::vector<uint8_t> edgeMask( width * height, 0 );
      for ( int i = 0; i < width * height; ++i )
         if ( grad[i] > threshold )
            edgeMask[i] = 1;

      // 5-8. Hough transform + peak detection + clustering + mask
      std::vector<HoughLine> lines = houghLines( edgeMask.data(), width, height );

      if ( lines.empty() )
         return result;

      result.mask = generateTrailMask( lines, width, height, m_config.trailDilateRadius );
      result.trailLineCount = static_cast<int>( lines.size() );

      // Count masked pixels
      result.trailPixelCount = 0;
      for ( int i = 0; i < width * height; ++i )
         if ( result.mask[i] )
            ++result.trailPixelCount;
   }

   return result;
}

// ============================================================================
// Stub: detectDust
// ============================================================================

DustDetectionResult ArtifactDetector::detectDust( const float* /*image*/, int width, int height ) const
{
   DustDetectionResult result;
   result.mask.assign( width * height, 0 );
   result.dustPixelCount = 0;
   return result;
}

// ============================================================================
// Stub: detectVignetting
// ============================================================================

VignettingDetectionResult ArtifactDetector::detectVignetting( const float* /*image*/, int width, int height,
                                                               const uint8_t* /*excludeMask*/ ) const
{
   VignettingDetectionResult result;
   result.correctionMap.assign( width * height, 1.0f );   // identity correction
   result.maxCorrection = 1.0;
   return result;
}

// ============================================================================
// Stub: localBackgroundMap
// ============================================================================

std::vector<float> ArtifactDetector::localBackgroundMap( const float* image, int width, int height ) const
{
   // For now, just return the block-median background
   return estimateBackground( image, width, height );
}

// ============================================================================
// Stub: fitRadialPolynomial
// ============================================================================

std::vector<double> ArtifactDetector::fitRadialPolynomial( const float* /*image*/, int /*width*/, int /*height*/,
                                                            const uint8_t* /*excludeMask*/, int order ) const
{
   // Return identity polynomial: c[0]=1, rest=0
   std::vector<double> coeffs( order + 1, 0.0 );
   if ( !coeffs.empty() )
      coeffs[0] = 1.0;
   return coeffs;
}

// ============================================================================
// detectAll — combines trail, dust, and vignetting detection
// ============================================================================

DetectionResult ArtifactDetector::detectAll( const float* image, int width, int height ) const
{
   DetectionResult result;

   // Trail detection
   result.trails = detectTrails( image, width, height );

   // Dust detection
   result.dust = detectDust( image, width, height );

   // Build combined exclude mask for vignetting (trail + dust pixels)
   std::vector<uint8_t> excludeMask( width * height, 0 );
   for ( int i = 0; i < width * height; ++i )
   {
      if ( result.trails.mask.size() > 0 && result.trails.mask[i] )
         excludeMask[i] = 1;
      if ( result.dust.mask.size() > 0 && result.dust.mask[i] )
         excludeMask[i] = 1;
   }

   // Vignetting detection (excluding trail + dust regions)
   result.vignetting = detectVignetting( image, width, height, excludeMask.data() );

   return result;
}

} // namespace nukex
