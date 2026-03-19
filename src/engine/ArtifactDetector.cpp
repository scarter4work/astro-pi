// ----------------------------------------------------------------------------
// ArtifactDetector.cpp — Artifact detection on stretched (nonlinear) images
//
// Detects satellite/airplane trails using Hough transform on Sobel edge maps.
// Detects dust motes via local background deficit + connected component analysis.
// Detects vignetting via radial polynomial fit + multiplicative correction map.
//
// Copyright (c) 2026 Scott Carter
// ----------------------------------------------------------------------------

#include "engine/ArtifactDetector.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <sstream>
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

   // Dynamic peak detection: satellite trails produce sharp outlier spikes
   // in the accumulator.  Compute the median and MAD of all non-zero bins,
   // then flag bins that are statistical outliers (>5 sigma above median).
   // This adapts to the actual edge structure — no hardcoded threshold.
   struct RawPeak { double rho; double theta; int votes; };
   std::vector<RawPeak> peaks;

   {
      // Collect all non-zero accumulator values
      std::vector<int> nonZero;
      nonZero.reserve( nRho * nTheta / 4 );
      for ( int i = 0; i < nRho * nTheta; ++i )
         if ( accumulator[i] > 0 )
            nonZero.push_back( accumulator[i] );

      if ( !nonZero.empty() )
      {
         // Median of non-zero bins
         size_t mid = nonZero.size() / 2;
         std::nth_element( nonZero.begin(), nonZero.begin() + mid, nonZero.end() );
         double accMedian = nonZero[mid];

         // MAD
         std::vector<double> devs( nonZero.size() );
         for ( size_t i = 0; i < nonZero.size(); ++i )
            devs[i] = std::abs( nonZero[i] - accMedian );
         size_t madMid = devs.size() / 2;
         std::nth_element( devs.begin(), devs.begin() + madMid, devs.end() );
         double accMad = devs[madMid] * 1.4826;   // scale to sigma

         // Threshold: bins that are strong outliers above the noise floor.
         // When MAD is near zero (flat accumulator), use 3× median as floor.
         // Always require at least 100 votes to reject tiny edge clusters.
         int peakThreshold;
         if ( accMad < 1.0 )
            peakThreshold = static_cast<int>( accMedian * 3.0 + 100 );
         else
            peakThreshold = static_cast<int>( accMedian + 5.0 * accMad );
         peakThreshold = std::max( peakThreshold, 100 );

         for ( int ri = 0; ri < nRho; ++ri )
         {
            for ( int ti = 0; ti < nTheta; ++ti )
            {
               int v = accumulator[ri * nTheta + ti];
               if ( v >= peakThreshold )
                  peaks.push_back( { ri - rhoOffset, M_PI * ti / nTheta, v } );
            }
         }
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

      // Safety cap: more than 30 lines almost certainly means false positives
      // from galaxy structure, stretched noise, or other non-trail features.
      constexpr int MAX_TRAIL_LINES = 30;
      if ( static_cast<int>( lines.size() ) > MAX_TRAIL_LINES )
         return result;

      result.mask = generateTrailMask( lines, width, height, m_config.trailDilateRadius );
      result.trailLineCount = static_cast<int>( lines.size() );

      // Count masked pixels
      result.trailPixelCount = 0;
      for ( int i = 0; i < width * height; ++i )
         if ( result.mask[i] )
            ++result.trailPixelCount;

      // Safety cap: if more than 10% of the image is masked, something went
      // wrong — galaxy structure or noise triggered false positives.
      double maskedFraction = static_cast<double>( result.trailPixelCount ) / ( width * height );
      if ( maskedFraction > 0.10 )
      {
         result.mask.assign( width * height, 0 );
         result.trailPixelCount = 0;
         result.trailLineCount = 0;
         return result;
      }
   }

   return result;
}

// ============================================================================
// detectDust — detect dark circular blobs (dust motes on sensor)
// ============================================================================

DustDetectionResult ArtifactDetector::detectDust( const float* image, int width, int height ) const
{
   DustDetectionResult result;
   result.mask.assign( width * height, 0 );
   result.dustPixelCount = 0;

   if ( width < 16 || height < 16 )
      return result;

   const int N = width * height;

   // 1. Difference-of-smoothing: small kernel captures mote structure,
   // large kernel captures true background. Deficit = large - small.
   int smallKernel = std::max( 3, m_config.dustMinDiameter );
   // Ensure odd kernel for symmetric filtering
   if ( smallKernel % 2 == 0 ) ++smallKernel;
   int largeKernel = static_cast<int>( m_config.dustMaxDiameter * 1.5 );
   if ( largeKernel % 2 == 0 ) ++largeKernel;

   std::vector<float> smallSmooth = localBackgroundMap( image, width, height, smallKernel );
   std::vector<float> largeSmooth = localBackgroundMap( image, width, height, largeKernel );

   // 2. Compute deficit: positive = darker than surroundings at mote scale
   std::vector<float> deficit( N );
   for ( int i = 0; i < N; ++i )
      deficit[i] = largeSmooth[i] - smallSmooth[i];   // positive = darker than surroundings at mote scale

   // 3. Compute MAD of deficit for threshold calibration
   //    Use only positive deficits to estimate the noise scale
   std::vector<float> deficitCopy( deficit.begin(), deficit.end() );
   size_t mid = deficitCopy.size() / 2;
   std::nth_element( deficitCopy.begin(), deficitCopy.begin() + mid, deficitCopy.end() );
   double medianDeficit = deficitCopy[mid];

   std::vector<float> absDevs( N );
   for ( int i = 0; i < N; ++i )
      absDevs[i] = std::abs( deficit[i] - static_cast<float>( medianDeficit ) );
   size_t madMid = absDevs.size() / 2;
   std::nth_element( absDevs.begin(), absDevs.begin() + madMid, absDevs.end() );
   double mad = absDevs[madMid] * 1.4826;   // scale to Gaussian sigma

   // When MAD is zero (nearly uniform background), any positive deficit is real.
   // Use a small floor so the sigma multiplier still yields a meaningful threshold.
   double threshold;
   if ( mad < 1e-10 )
      threshold = medianDeficit + 1e-6;   // just above the noise floor
   else
      threshold = medianDeficit + m_config.dustDetectionSigma * mad;

   // 4. Flag pixels significantly darker than local background
   std::vector<uint8_t> flagged( N, 0 );
   for ( int i = 0; i < N; ++i )
      if ( deficit[i] > threshold )
         flagged[i] = 1;

   // 5. Connected component labeling via flood fill
   std::vector<int> labels( N, 0 );
   int nextLabel = 1;

   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         int idx = y * width + x;
         if ( flagged[idx] == 0 || labels[idx] != 0 )
            continue;

         // BFS flood fill
         int label = nextLabel++;
         std::vector<int> stack;
         stack.push_back( idx );
         labels[idx] = label;

         while ( !stack.empty() )
         {
            int ci = stack.back();
            stack.pop_back();
            int cy = ci / width;
            int cx = ci % width;

            // 4-connected neighbors
            const int dx[] = { -1, 1, 0, 0 };
            const int dy[] = { 0, 0, -1, 1 };
            for ( int d = 0; d < 4; ++d )
            {
               int nx = cx + dx[d];
               int ny = cy + dy[d];
               if ( nx < 0 || nx >= width || ny < 0 || ny >= height )
                  continue;
               int ni = ny * width + nx;
               if ( flagged[ni] && labels[ni] == 0 )
               {
                  labels[ni] = label;
                  stack.push_back( ni );
               }
            }
         }
      }
   }

   int numComponents = nextLabel - 1;

   // 6. For each component: compute area, bounding box, centroid, circularity
   struct ComponentInfo
   {
      int area = 0;
      int xMin = 0, xMax = 0, yMin = 0, yMax = 0;
      double sumX = 0, sumY = 0;
      double sumDeficit = 0;
   };

   std::vector<ComponentInfo> components( numComponents );
   for ( int i = 0; i < numComponents; ++i )
   {
      components[i].xMin = width;
      components[i].yMin = height;
   }

   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         int lbl = labels[y * width + x];
         if ( lbl == 0 )
            continue;
         auto& c = components[lbl - 1];
         c.area++;
         c.sumX += x;
         c.sumY += y;
         c.sumDeficit += deficit[y * width + x];
         c.xMin = std::min( c.xMin, x );
         c.xMax = std::max( c.xMax, x );
         c.yMin = std::min( c.yMin, y );
         c.yMax = std::max( c.yMax, y );
      }
   }

   // 7. Filter components and build result
   for ( int i = 0; i < numComponents; ++i )
   {
      const auto& c = components[i];
      if ( c.area == 0 )
         continue;

      // Bounding box diameter (use the larger dimension)
      double bbWidth  = c.xMax - c.xMin + 1;
      double bbHeight = c.yMax - c.yMin + 1;
      double diameter = std::max( bbWidth, bbHeight );

      // Diameter filter
      if ( diameter < m_config.dustMinDiameter || diameter > m_config.dustMaxDiameter )
         continue;

      // Circularity: ratio of actual area to area of circle with this diameter
      double idealArea = M_PI * ( diameter / 2.0 ) * ( diameter / 2.0 );
      double circularity = c.area / idealArea;

      if ( circularity < m_config.dustCircularityMin )
         continue;

      // This blob passes all filters
      DustBlobInfo blob;
      blob.centerX = c.sumX / c.area;
      blob.centerY = c.sumY / c.area;
      blob.radius = diameter / 2.0;
      blob.circularity = circularity;
      blob.meanAttenuation = c.sumDeficit / c.area;   // mean deficit

      result.blobs.push_back( blob );

      // Mark this component in the mask
      int label = i + 1;
      for ( int j = 0; j < N; ++j )
         if ( labels[j] == label )
            result.mask[j] = 1;
   }

   // Count dust pixels
   for ( int i = 0; i < N; ++i )
      if ( result.mask[i] )
         ++result.dustPixelCount;

   return result;
}

// ============================================================================
// detectDustSubcube — detect dust on linear stacked image, verify via subcube
//
// Step 1: Spatial candidate detection using difference-of-smoothing deficit
// Step 2: Subcube consistency verification — dust is present in ALL frames
// ============================================================================

DustDetectionResult ArtifactDetector::detectDustSubcube( const float* stackedImage,
                                                          const std::vector<SubCube*>& channelCubes,
                                                          int width, int height,
                                                          LogCallback log ) const
{
   // Helper: emit diagnostic only if a callback was provided
   auto emit = [&log]( const std::string& msg ) {
      if ( log ) log( msg );
   };
   DustDetectionResult result;
   result.mask.assign( width * height, 0 );
   result.dustPixelCount = 0;

   if ( width < 16 || height < 16 )
      return result;

   const int N = width * height;

   // ---- Step 1: Spatial candidate detection on stacked image ----

   // Diagnostic: stacked image value range
   {
      float imgMin = *std::min_element( stackedImage, stackedImage + N );
      float imgMax = *std::max_element( stackedImage, stackedImage + N );
      std::ostringstream oss;
      oss << "[DustDetect] Stacked image: " << width << "x" << height
          << ", value range [" << imgMin << ", " << imgMax << "]";
      emit( oss.str() );
   }

   // Difference-of-smoothing: small captures mote, large captures background
   int smallKernel = std::max( 3, m_config.dustMinDiameter );
   if ( smallKernel % 2 == 0 ) ++smallKernel;
   int largeKernel = static_cast<int>( m_config.dustMaxDiameter * 1.5 );
   if ( largeKernel % 2 == 0 ) ++largeKernel;

   {
      std::ostringstream oss;
      oss << "[DustDetect] Kernels: small=" << smallKernel << ", large=" << largeKernel
          << ", sigma=" << m_config.dustDetectionSigma;
      emit( oss.str() );
   }

   std::vector<float> smallSmooth = localBackgroundMap( stackedImage, width, height, smallKernel );
   std::vector<float> largeSmooth = localBackgroundMap( stackedImage, width, height, largeKernel );

   // Deficit: positive = darker than surroundings at mote scale
   std::vector<float> deficit( N );
   for ( int i = 0; i < N; ++i )
      deficit[i] = largeSmooth[i] - smallSmooth[i];

   // Threshold: median + sigma * 1.4826 * MAD
   std::vector<float> deficitCopy( deficit.begin(), deficit.end() );
   size_t mid = deficitCopy.size() / 2;
   std::nth_element( deficitCopy.begin(), deficitCopy.begin() + mid, deficitCopy.end() );
   double medianDef = deficitCopy[mid];

   std::vector<float> absDevs( N );
   for ( int i = 0; i < N; ++i )
      absDevs[i] = std::abs( deficit[i] - static_cast<float>( medianDef ) );
   size_t madMid = absDevs.size() / 2;
   std::nth_element( absDevs.begin(), absDevs.begin() + madMid, absDevs.end() );
   double mad = absDevs[madMid] * 1.4826;

   double threshold;
   if ( mad < 1e-10 )
      threshold = medianDef + 1e-6;
   else
      threshold = medianDef + m_config.dustDetectionSigma * mad;

   // Diagnostic: deficit statistics
   float maxDeficit = *std::max_element( deficit.begin(), deficit.end() );
   float minDeficit = *std::min_element( deficit.begin(), deficit.end() );
   {
      std::ostringstream oss;
      oss << "[DustDetect] Deficit range: " << minDeficit << " to " << maxDeficit
          << ", median=" << medianDef << ", MAD=" << mad
          << ", threshold=" << threshold;
      emit( oss.str() );
   }

   // Diagnostic probe at known dust mote location (2094, 953)
   {
      int px = 2094, py = 953;
      if ( px < width && py < height )
      {
         int idx = py * width + px;
         std::ostringstream oss;
         oss << "[DustDetect] PROBE (2094,953): pixel=" << stackedImage[idx]
             << ", smallSmooth=" << smallSmooth[idx]
             << ", largeSmooth=" << largeSmooth[idx]
             << ", deficit=" << deficit[idx]
             << ", threshold=" << threshold
             << ", aboveThresh=" << (deficit[idx] > threshold ? "YES" : "NO")
             << ", brightnessOK=" << (stackedImage[idx] <= largeSmooth[idx] ? "YES" : "NO");
         emit( oss.str() );
         // Also probe nearby ring for comparison
         int probeR = 40;
         float ringSum = 0; int ringN = 0;
         for ( int dy = -probeR; dy <= probeR; dy += probeR )
            for ( int dx = -probeR; dx <= probeR; dx += probeR )
            {
               if ( dx == 0 && dy == 0 ) continue;
               int nx = px + dx, ny = py + dy;
               if ( nx >= 0 && nx < width && ny >= 0 && ny < height )
               { ringSum += stackedImage[ny * width + nx]; ++ringN; }
            }
         if ( ringN > 0 )
         {
            std::ostringstream oss2;
            oss2 << "[DustDetect] PROBE ring: avgNeighbor=" << (ringSum/ringN)
                 << ", centerDeficit=" << ((ringSum/ringN) - stackedImage[py*width+px])
                 << ", relDeficit=" << ((ringSum/ringN - stackedImage[py*width+px]) / (ringSum/ringN));
            emit( oss2.str() );
         }
      }
   }

   // Flag pixels: deficit above threshold AND brightness exclusion
   std::vector<uint8_t> flagged( N, 0 );
   int flagCount = 0;
   for ( int i = 0; i < N; ++i )
      if ( deficit[i] > threshold && stackedImage[i] <= largeSmooth[i] )
      {
         flagged[i] = 1;
         ++flagCount;
      }
   emit( "[DustDetect] Flagged pixels: " + std::to_string( flagCount ) );

   // Morphological closing (dilate then erode) to bridge small gaps within
   // dust mote regions. Without this, noise causes the deficit to dip below
   // threshold at some pixels, fragmenting the mote into many tiny components.
   {
      const int closeRadius = 5;
      // Dilate: flag pixel if ANY neighbor within radius is flagged
      std::vector<uint8_t> dilated( N, 0 );
      for ( int y = 0; y < height; ++y )
         for ( int x = 0; x < width; ++x )
         {
            if ( flagged[y * width + x] )
            {
               int y0 = std::max( 0, y - closeRadius );
               int y1 = std::min( height - 1, y + closeRadius );
               int x0 = std::max( 0, x - closeRadius );
               int x1 = std::min( width - 1, x + closeRadius );
               for ( int dy = y0; dy <= y1; ++dy )
                  for ( int dx = x0; dx <= x1; ++dx )
                     dilated[dy * width + dx] = 1;
            }
         }
      // Erode: keep pixel only if ALL neighbors within radius are set
      std::vector<uint8_t> closed( N, 0 );
      for ( int y = 0; y < height; ++y )
         for ( int x = 0; x < width; ++x )
         {
            if ( !dilated[y * width + x] ) continue;
            int y0 = std::max( 0, y - closeRadius );
            int y1 = std::min( height - 1, y + closeRadius );
            int x0 = std::max( 0, x - closeRadius );
            int x1 = std::min( width - 1, x + closeRadius );
            bool allSet = true;
            for ( int dy = y0; dy <= y1 && allSet; ++dy )
               for ( int dx = x0; dx <= x1 && allSet; ++dx )
                  if ( !dilated[dy * width + dx] )
                     allSet = false;
            if ( allSet )
               closed[y * width + x] = 1;
         }
      flagged = std::move( closed );
      int closedCount = 0;
      for ( int i = 0; i < N; ++i )
         if ( flagged[i] ) ++closedCount;
      emit( "[DustDetect] After morphological closing (r=5): " + std::to_string( closedCount ) + " pixels" );
   }

   // Connected component labeling via flood fill (4-connected)
   std::vector<int> labels( N, 0 );
   int nextLabel = 1;

   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         int idx = y * width + x;
         if ( flagged[idx] == 0 || labels[idx] != 0 )
            continue;

         int label = nextLabel++;
         std::vector<int> stack;
         stack.push_back( idx );
         labels[idx] = label;

         while ( !stack.empty() )
         {
            int ci = stack.back();
            stack.pop_back();
            int cy = ci / width;
            int cx = ci % width;

            const int dx[] = { -1, 1, 0, 0 };
            const int dy[] = { 0, 0, -1, 1 };
            for ( int d = 0; d < 4; ++d )
            {
               int nx = cx + dx[d];
               int ny = cy + dy[d];
               if ( nx < 0 || nx >= width || ny < 0 || ny >= height )
                  continue;
               int ni = ny * width + nx;
               if ( flagged[ni] && labels[ni] == 0 )
               {
                  labels[ni] = label;
                  stack.push_back( ni );
               }
            }
         }
      }
   }

   int numComponents = nextLabel - 1;
   emit( "[DustDetect] Connected components: " + std::to_string( numComponents ) );

   // Compute component properties
   struct ComponentInfo
   {
      int area = 0;
      int xMin = 0, xMax = 0, yMin = 0, yMax = 0;
      double sumX = 0, sumY = 0;
      double sumDeficit = 0;
      std::vector<int> memberPixels;   // linear indices of pixels in this blob
   };

   std::vector<ComponentInfo> components( numComponents );
   for ( int i = 0; i < numComponents; ++i )
   {
      components[i].xMin = width;
      components[i].yMin = height;
   }

   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         int lbl = labels[y * width + x];
         if ( lbl == 0 )
            continue;
         auto& c = components[lbl - 1];
         c.area++;
         c.sumX += x;
         c.sumY += y;
         c.sumDeficit += deficit[y * width + x];
         c.xMin = std::min( c.xMin, x );
         c.xMax = std::max( c.xMax, x );
         c.yMin = std::min( c.yMin, y );
         c.yMax = std::max( c.yMax, y );
         c.memberPixels.push_back( y * width + x );
      }
   }

   // Filter blobs by size and circularity, then verify against subcube
   for ( int i = 0; i < numComponents; ++i )
   {
      const auto& c = components[i];
      if ( c.area == 0 )
         continue;

      double bbWidth  = c.xMax - c.xMin + 1;
      double bbHeight = c.yMax - c.yMin + 1;
      double diameter = std::max( bbWidth, bbHeight );

      if ( diameter < m_config.dustMinDiameter || diameter > m_config.dustMaxDiameter )
         continue;

      double idealArea = M_PI * ( diameter / 2.0 ) * ( diameter / 2.0 );
      double circularity = c.area / idealArea;

      if ( circularity < m_config.dustCircularityMin )
         continue;

      {
         std::ostringstream oss;
         oss << "[DustDetect] Candidate blob: center=(" << (c.sumX/c.area) << "," << (c.sumY/c.area)
             << "), diameter=" << diameter << ", area=" << c.area
             << ", circularity=" << circularity;
         emit( oss.str() );
      }

      // ---- Step 2: Subcube consistency verification ----

      DustBlobInfo blob;
      blob.centerX = c.sumX / c.area;
      blob.centerY = c.sumY / c.area;
      blob.radius = diameter / 2.0;
      blob.circularity = circularity;
      blob.meanAttenuation = c.sumDeficit / c.area;

      // If no subcube data, accept spatial-only (graceful degradation)
      if ( channelCubes.empty() || channelCubes[0] == nullptr )
      {
         result.blobs.push_back( blob );
         int label = i + 1;
         for ( int j = 0; j < N; ++j )
            if ( labels[j] == label )
               result.mask[j] = 1;
         continue;
      }

      // Aggregate spatial verification: average deficit across sample pixels
      // per frame, then check consistency of per-frame means.
      // Per-pixel SNR is <2 in individual frames — spatial averaging over K
      // pixels reduces noise by sqrt(K), making the dust deficit detectable.
      // See: specs/2026-03-18-aggregate-subcube-verification-amendment.md
      const int maxSamples = 40;
      std::vector<int> samplePixels;
      if ( c.area <= maxSamples )
      {
         samplePixels = c.memberPixels;
      }
      else
      {
         int step = c.area / maxSamples;
         for ( int s = 0; s < maxSamples; ++s )
            samplePixels.push_back( c.memberPixels[s * step] );
      }

      const SubCube* cube = channelCubes[0];
      size_t nSubs = cube->numSubs();
      // Neighbor radius must reach OUTSIDE the largest possible mote,
      // not just outside the detected blob (which may be a fragment).
      int neighborRadius = std::max( 5, m_config.dustMaxDiameter / 2 );

      // Accumulate deficit per frame across all sample pixels
      std::vector<double> frameAvgDeficit( nSubs, 0.0 );
      for ( int pixIdx : samplePixels )
      {
         int px = pixIdx % width;
         int py = pixIdx / width;

         int nx0 = std::max( 0, px - neighborRadius );
         int nx1 = std::min( width - 1, px + neighborRadius );
         int ny0 = std::max( 0, py - neighborRadius );
         int ny1 = std::min( height - 1, py + neighborRadius );

         for ( size_t z = 0; z < nSubs; ++z )
         {
            double neighborMean = ( cube->pixel( z, ny0, nx0 )
                                  + cube->pixel( z, ny0, nx1 )
                                  + cube->pixel( z, ny1, nx0 )
                                  + cube->pixel( z, ny1, nx1 ) ) / 4.0;
            double pixelVal = cube->pixel( z, py, px );
            frameAvgDeficit[z] += ( neighborMean - pixelVal );
         }
      }

      int nSamples = static_cast<int>( samplePixels.size() );
      for ( size_t z = 0; z < nSubs; ++z )
         frameAvgDeficit[z] /= nSamples;

      // Median and MAD of per-frame aggregate deficits
      std::vector<double> sortedDeficits = frameAvgDeficit;
      size_t defMid = sortedDeficits.size() / 2;
      std::nth_element( sortedDeficits.begin(), sortedDeficits.begin() + defMid, sortedDeficits.end() );
      double medianAggDef = sortedDeficits[defMid];

      std::vector<double> defAbsDevs( nSubs );
      for ( size_t z = 0; z < nSubs; ++z )
         defAbsDevs[z] = std::abs( frameAvgDeficit[z] - medianAggDef );
      size_t defMadMid = defAbsDevs.size() / 2;
      std::nth_element( defAbsDevs.begin(), defAbsDevs.begin() + defMadMid, defAbsDevs.end() );
      double madAggDef = 1.4826 * defAbsDevs[defMadMid];

      double ratio = ( medianAggDef > 0 ) ? madAggDef / medianAggDef : 999.0;
      bool blobPassed = ( medianAggDef > 0 && ratio < 1.0 );

      {
         std::ostringstream oss;
         oss << "[DustDetect] Blob verification (aggregate): medDeficit="
             << medianAggDef << ", madDeficit=" << madAggDef
             << ", ratio=" << ratio << ", samples=" << nSamples
             << ( blobPassed ? " PASS" : " FAIL" );
         emit( oss.str() );
      }

      if ( blobPassed )
      {
         result.blobs.push_back( blob );
         int label = i + 1;
         for ( int j = 0; j < N; ++j )
            if ( labels[j] == label )
               result.mask[j] = 1;
      }
   }

   // Count dust pixels
   for ( int i = 0; i < N; ++i )
      if ( result.mask[i] )
         ++result.dustPixelCount;

   return result;
}

// ============================================================================
// detectVignetting — radial polynomial fit + correction map
// ============================================================================

VignettingDetectionResult ArtifactDetector::detectVignetting( const float* image, int width, int height,
                                                               const uint8_t* excludeMask ) const
{
   VignettingDetectionResult result;
   const int N = width * height;
   result.correctionMap.assign( N, 1.0f );
   result.maxCorrection = 1.0;

   if ( width < 4 || height < 4 )
      return result;

   // 1. Fit radial polynomial to (radius, brightness) samples
   int order = m_config.vignettingPolyOrder;
   std::vector<double> coeffs = fitRadialPolynomial( image, width, height, excludeMask, order );

   // 2. Evaluate polynomial at center (r=0) → reference brightness = c0
   double centerBrightness = coeffs[0];
   if ( centerBrightness <= 1e-10 )
   {
      // Degenerate fit — return identity
      return result;
   }

   // 3. Build correction map
   double cx = width / 2.0;
   double cy = height / 2.0;
   double maxR = std::sqrt( cx * cx + cy * cy );

   double maxCorr = 1.0;

   for ( int y = 0; y < height; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         double dx = x - cx;
         double dy = y - cy;
         double r = std::sqrt( dx * dx + dy * dy ) / maxR;   // normalized [0, 1]

         // Evaluate polynomial at this radius
         double fitted = 0.0;
         double rPow = 1.0;
         for ( int k = 0; k <= order; ++k )
         {
            fitted += coeffs[k] * rPow;
            rPow *= r;
         }

         // Correction factor = centerBrightness / fittedBrightness
         // Clamp to [1.0, maxCorrection] — never darken, cap overcorrection
         double correction = 1.0;
         if ( fitted > 1e-10 )
            correction = centerBrightness / fitted;
         if ( correction < 1.0 )
            correction = 1.0;
         if ( correction > m_config.vignettingMaxCorrection )
            correction = m_config.vignettingMaxCorrection;

         result.correctionMap[y * width + x] = static_cast<float>( correction );
         if ( correction > maxCorr )
            maxCorr = correction;
      }
   }

   result.maxCorrection = maxCorr;
   return result;
}

// ============================================================================
// localBackgroundMap — box filter (uniform average) via integral image
//
// Computes a box-averaged version of the image at the specified kernel size.
// Uses an integral image (prefix sum) for O(1) per-pixel lookup regardless
// of kernel size. Total cost is O(N) to build the integral image.
// ============================================================================

std::vector<float> ArtifactDetector::localBackgroundMap( const float* image, int width, int height, int kernelSize ) const
{
   const int N = width * height;
   int halfK = kernelSize / 2;

   // Build integral image (double precision to avoid float accumulation error)
   // integral[y][x] = sum of image[0..y-1][0..x-1]
   // Using (height+1) x (width+1) with zero padding on top and left
   std::vector<double> integral( (size_t)( height + 1 ) * ( width + 1 ), 0.0 );
   const int iw = width + 1;

   for ( int y = 0; y < height; ++y )
      for ( int x = 0; x < width; ++x )
         integral[( y + 1 ) * iw + ( x + 1 )] = image[y * width + x]
            + integral[y * iw + ( x + 1 )]
            + integral[( y + 1 ) * iw + x]
            - integral[y * iw + x];

   // Box average using integral image
   std::vector<float> result( N );
   for ( int y = 0; y < height; ++y )
   {
      int y0 = std::max( 0, y - halfK );
      int y1 = std::min( height - 1, y + halfK );
      for ( int x = 0; x < width; ++x )
      {
         int x0 = std::max( 0, x - halfK );
         int x1 = std::min( width - 1, x + halfK );

         // Sum of rectangle [y0..y1, x0..x1] using integral image
         double sum = integral[( y1 + 1 ) * iw + ( x1 + 1 )]
                    - integral[y0 * iw + ( x1 + 1 )]
                    - integral[( y1 + 1 ) * iw + x0]
                    + integral[y0 * iw + x0];
         int count = ( y1 - y0 + 1 ) * ( x1 - x0 + 1 );
         result[y * width + x] = static_cast<float>( sum / count );
      }
   }

   return result;
}

// ============================================================================
// fitRadialPolynomial — least-squares polynomial fit of brightness vs radius
//
// Collects (radius, brightness) samples from unmasked pixels (subsampled
// every 4th pixel for efficiency). Normalizes radius to [0, 1]. Solves
// normal equations A^T A x = A^T b via Gaussian elimination with partial
// pivoting.
// ============================================================================

std::vector<double> ArtifactDetector::fitRadialPolynomial( const float* image, int width, int height,
                                                            const uint8_t* excludeMask, int order ) const
{
   const int nCoeffs = order + 1;

   // Fallback: identity polynomial
   std::vector<double> coeffs( nCoeffs, 0.0 );
   coeffs[0] = 1.0;

   if ( width < 4 || height < 4 || order < 1 )
      return coeffs;

   double cx = width / 2.0;
   double cy = height / 2.0;
   double maxR = std::sqrt( cx * cx + cy * cy );

   if ( maxR < 1e-10 )
      return coeffs;

   // 1. Collect (r, brightness) samples — subsample every 4th pixel
   //    Build normal equation matrices incrementally to avoid storing samples
   //
   //    Normal equations: (A^T A) x = A^T b
   //    where A[i][k] = r_i^k, b[i] = brightness_i
   //
   //    (A^T A)[j][k] = sum_i r_i^(j+k)
   //    (A^T b)[j]    = sum_i r_i^j * brightness_i

   // We need sums of r^(j+k) for j,k in [0, order], so r^0 through r^(2*order)
   const int maxPow = 2 * order;
   std::vector<double> rPowSums( maxPow + 1, 0.0 );   // sum of r^p over all samples
   std::vector<double> rBrightSums( nCoeffs, 0.0 );    // sum of r^j * brightness
   int sampleCount = 0;

   const int step = 4;
   for ( int y = 0; y < height; y += step )
   {
      for ( int x = 0; x < width; x += step )
      {
         int idx = y * width + x;

         // Skip masked pixels
         if ( excludeMask != nullptr && excludeMask[idx] != 0 )
            continue;

         double dx = x - cx;
         double dy = y - cy;
         double r = std::sqrt( dx * dx + dy * dy ) / maxR;   // normalized [0, 1]
         double brightness = static_cast<double>( image[idx] );

         // Accumulate r^p sums
         double rPow = 1.0;
         for ( int p = 0; p <= maxPow; ++p )
         {
            rPowSums[p] += rPow;
            rPow *= r;
         }

         // Accumulate r^j * brightness sums
         rPow = 1.0;
         for ( int j = 0; j < nCoeffs; ++j )
         {
            rBrightSums[j] += rPow * brightness;
            rPow *= r;
         }

         ++sampleCount;
      }
   }

   if ( sampleCount < nCoeffs )
      return coeffs;   // not enough samples to fit

   // 2. Build normal equation matrix (A^T A) and RHS (A^T b)
   //    Using augmented matrix for in-place Gaussian elimination
   //    [A^T A | A^T b]  size: nCoeffs x (nCoeffs + 1)
   std::vector<double> aug( nCoeffs * ( nCoeffs + 1 ), 0.0 );
   auto augIdx = [&]( int row, int col ) -> double& {
      return aug[row * ( nCoeffs + 1 ) + col];
   };

   for ( int j = 0; j < nCoeffs; ++j )
   {
      for ( int k = 0; k < nCoeffs; ++k )
         augIdx( j, k ) = rPowSums[j + k];
      augIdx( j, nCoeffs ) = rBrightSums[j];
   }

   // 3. Gaussian elimination with partial pivoting
   for ( int col = 0; col < nCoeffs; ++col )
   {
      // Find pivot
      int pivotRow = col;
      double pivotVal = std::abs( augIdx( col, col ) );
      for ( int row = col + 1; row < nCoeffs; ++row )
      {
         double v = std::abs( augIdx( row, col ) );
         if ( v > pivotVal )
         {
            pivotVal = v;
            pivotRow = row;
         }
      }

      if ( pivotVal < 1e-15 )
         return coeffs;   // singular — return identity

      // Swap rows
      if ( pivotRow != col )
      {
         for ( int k = 0; k <= nCoeffs; ++k )
            std::swap( augIdx( col, k ), augIdx( pivotRow, k ) );
      }

      // Eliminate below
      double diag = augIdx( col, col );
      for ( int row = col + 1; row < nCoeffs; ++row )
      {
         double factor = augIdx( row, col ) / diag;
         for ( int k = col; k <= nCoeffs; ++k )
            augIdx( row, k ) -= factor * augIdx( col, k );
      }
   }

   // 4. Back substitution
   for ( int row = nCoeffs - 1; row >= 0; --row )
   {
      double sum = augIdx( row, nCoeffs );
      for ( int k = row + 1; k < nCoeffs; ++k )
         sum -= augIdx( row, k ) * coeffs[k];
      coeffs[row] = sum / augIdx( row, row );
   }

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
