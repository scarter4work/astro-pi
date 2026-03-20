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
      {
         result.trailLineCount = static_cast<int>( lines.size() );
         return result;   // caller can see trailLineCount > 0 but empty mask
      }

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
         int savedLineCount = result.trailLineCount;
         result.mask.assign( width * height, 0 );
         result.trailPixelCount = 0;
         result.trailLineCount = savedLineCount;  // preserve for diagnostics
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
                                                          const std::vector<AlignOffset>& alignments,
                                                          int width, int height,
                                                          LogCallback log ) const
{
   auto emit = [&log]( const std::string& msg ) {
      if ( log ) log( msg );
   };
   DustDetectionResult result;
   const int N = width * height;
   result.mask.assign( N, 0 );
   result.correctionMap.assign( N, 1.0f );
   result.dustPixelCount = 0;

   if ( width < 64 || height < 64 )
      return result;
   if ( channelCubes.empty() || channelCubes[0] == nullptr )
      return result;

   const SubCube* cube = channelCubes[0];
   const size_t nSubs = cube->numSubs();
   if ( nSubs < 3 || alignments.size() != nSubs )
      return result;

   // =========================================================================
   // SENSOR-SPACE DUST DETECTION
   //
   // A dust mote is fixed on the sensor. In the aligned subcube, it shifts by
   // the alignment offset per frame. By reversing the alignment, we build a
   // "sensor-space stacked image" where the mote is sharp and circular, while
   // stars/galaxies are smeared by the alignment drift. The mote becomes the
   // ONLY persistent circular feature. DoS finds it trivially.
   // =========================================================================

   // Step 1: Build sensor-space stacked image.
   // For each sensor position (sx, sy), average subcube[z, sy+dy_z, sx+dx_z]
   // across all frames. This "unaligns" the data back to sensor coordinates.
   // The mote (fixed on sensor) becomes sharp; stars/galaxy become smeared.

   emit( "[DustDetect] Building sensor-space stacked image..." );

   std::vector<float> sensorStack( N, 0.0f );
   std::vector<uint8_t> sensorValid( N, 0 );

   for ( int sy = 0; sy < height; ++sy )
      for ( int sx = 0; sx < width; ++sx )
      {
         double sum = 0;
         int count = 0;
         for ( size_t z = 0; z < nSubs; ++z )
         {
            int ax = sx + alignments[z].dx;
            int ay = sy + alignments[z].dy;
            if ( ax >= 0 && ax < width && ay >= 0 && ay < height )
            {
               sum += cube->pixel( z, ay, ax );
               ++count;
            }
         }
         if ( count >= static_cast<int>( nSubs ) / 2 )
         {
            sensorStack[sy * width + sx] = static_cast<float>( sum / count );
            sensorValid[sy * width + sx] = 1;
         }
      }

   // Step 2: Self-flat correction — divide by heavily smoothed version.
   // The smooth captures vignetting (large-scale gradient) but NOT the mote
   // (small circle). After division, only mote-scale defects remain.
   // This is what flat field correction does — a synthetic flat from the data.
   int flatKernel = std::max( 201, m_config.dustMaxDiameter * 2 + 1 );
   if ( flatKernel % 2 == 0 ) ++flatKernel;

   std::vector<float> flatSmooth = localBackgroundMap( sensorStack.data(), width, height, flatKernel );

   // Normalized image: values near 1.0 for normal pixels, < 1.0 for dust motes
   std::vector<float> normalized( N, 1.0f );
   for ( int i = 0; i < N; ++i )
      if ( sensorValid[i] && flatSmooth[i] > 1e-10f )
      {
         float norm = sensorStack[i] / flatSmooth[i];
         normalized[i] = std::isfinite( norm ) ? norm : 1.0f;
      }

   // Deficit = 1.0 - normalized (positive for attenuated pixels)
   std::vector<float> pctDeficit( N, 0.0f );
   for ( int i = 0; i < N; ++i )
      if ( sensorValid[i] )
         pctDeficit[i] = 1.0f - normalized[i];

   // Threshold using MAD
   std::vector<float> validPcts;
   validPcts.reserve( N );
   for ( int i = 0; i < N; ++i )
      if ( sensorValid[i] )
         validPcts.push_back( pctDeficit[i] );

   if ( validPcts.empty() )
   {
      emit( "[DustDetect] No valid sensor pixels -- skipping dust detection" );
      return result;
   }

   size_t mid = validPcts.size() / 2;
   std::nth_element( validPcts.begin(), validPcts.begin() + mid, validPcts.end() );
   float medianPct = validPcts[mid];

   std::vector<float> absDevs( validPcts.size() );
   for ( size_t i = 0; i < validPcts.size(); ++i )
      absDevs[i] = std::abs( validPcts[i] - medianPct );
   std::nth_element( absDevs.begin(), absDevs.begin() + mid, absDevs.end() );
   float madPct = absDevs[mid] * 1.4826f;

   // Floor guard: when MAD is zero (perfectly flat image), use small epsilon
   // to avoid flagging half the image. Matches the guard in detectDust().
   float pctThreshold;
   if ( madPct < 1e-10f )
      pctThreshold = medianPct + 1e-6f;
   else
      pctThreshold = medianPct + static_cast<float>( m_config.dustDetectionSigma ) * madPct;

   {
      std::ostringstream oss;
      oss << "[DustDetect] Self-flat correction: flatKernel=" << flatKernel
          << ", deficit median=" << medianPct << ", MAD=" << madPct
          << ", threshold=" << pctThreshold << " (" << m_config.dustDetectionSigma << "σ)";
      emit( oss.str() );
   }

   // Step 3: Flag pixels where deficit exceeds threshold (attenuated by mote)
   std::vector<uint8_t> flagged( N, 0 );
   int flagCount = 0;
   for ( int i = 0; i < N; ++i )
      if ( sensorValid[i] && pctDeficit[i] > pctThreshold )
      {
         flagged[i] = 1;
         ++flagCount;
      }
   emit( "[DustDetect] Flagged pixels: " + std::to_string( flagCount ) );

   // Morphological closing (r=5)
   {
      const int cr = 5;
      std::vector<uint8_t> dilated( N, 0 );
      for ( int y = 0; y < height; ++y )
         for ( int x = 0; x < width; ++x )
         {
            if ( !flagged[y * width + x] ) continue;
            for ( int dy = std::max( 0, y - cr ); dy <= std::min( height - 1, y + cr ); ++dy )
               for ( int dx = std::max( 0, x - cr ); dx <= std::min( width - 1, x + cr ); ++dx )
                  dilated[dy * width + dx] = 1;
         }
      std::vector<uint8_t> closed( N, 0 );
      for ( int y = 0; y < height; ++y )
         for ( int x = 0; x < width; ++x )
         {
            if ( !dilated[y * width + x] ) continue;
            bool all = true;
            for ( int dy = std::max( 0, y - cr ); dy <= std::min( height - 1, y + cr ) && all; ++dy )
               for ( int dx = std::max( 0, x - cr ); dx <= std::min( width - 1, x + cr ) && all; ++dx )
                  if ( !dilated[dy * width + dx] ) all = false;
            if ( all ) closed[y * width + x] = 1;
         }
      flagged = std::move( closed );
   }

   // Connected components (4-connected)
   std::vector<int> labels( N, 0 );
   int nextLabel = 1;
   for ( int y = 0; y < height; ++y )
      for ( int x = 0; x < width; ++x )
      {
         int idx = y * width + x;
         if ( !flagged[idx] || labels[idx] ) continue;
         int label = nextLabel++;
         std::vector<int> stk;
         stk.push_back( idx );
         labels[idx] = label;
         while ( !stk.empty() )
         {
            int ci = stk.back(); stk.pop_back();
            int cy2 = ci / width, cx2 = ci % width;
            const int ddx[] = { -1, 1, 0, 0 };
            const int ddy[] = { 0, 0, -1, 1 };
            for ( int d = 0; d < 4; ++d )
            {
               int nx = cx2 + ddx[d], ny = cy2 + ddy[d];
               if ( nx < 0 || nx >= width || ny < 0 || ny >= height ) continue;
               int ni = ny * width + nx;
               if ( flagged[ni] && !labels[ni] )
               { labels[ni] = label; stk.push_back( ni ); }
            }
         }
      }

   int numComponents = nextLabel - 1;
   emit( "[DustDetect] Components: " + std::to_string( numComponents ) );

   // Step 4: Filter by size + circularity. No separate subcube verification
   // needed — the sensor-space stacking IS the verification (every frame
   // contributes to the same sensor pixel, so the deficit is only real if
   // it's consistent across all frames).

   struct CompInfo { int area = 0; int xMin, xMax, yMin, yMax;
      double sumX = 0, sumY = 0; };
   std::vector<CompInfo> comps( numComponents );
   for ( auto& c : comps ) { c.xMin = width; c.yMin = height; c.xMax = 0; c.yMax = 0; }

   for ( int y = 0; y < height; ++y )
      for ( int x = 0; x < width; ++x )
      {
         int lbl = labels[y * width + x];
         if ( !lbl ) continue;
         auto& c = comps[lbl - 1];
         c.area++;
         c.sumX += x; c.sumY += y;
         c.xMin = std::min( c.xMin, x ); c.xMax = std::max( c.xMax, x );
         c.yMin = std::min( c.yMin, y ); c.yMax = std::max( c.yMax, y );
      }

   for ( int i = 0; i < numComponents; ++i )
   {
      const auto& c = comps[i];
      if ( c.area == 0 ) continue;
      double bbW = c.xMax - c.xMin + 1;
      double bbH = c.yMax - c.yMin + 1;
      double diameter = std::max( bbW, bbH );
      if ( diameter < m_config.dustMinDiameter || diameter > m_config.dustMaxDiameter )
         continue;
      double idealArea = M_PI * ( diameter / 2.0 ) * ( diameter / 2.0 );
      double circularity = c.area / idealArea;
      if ( circularity < m_config.dustCircularityMin )
         continue;

      // Sensor coordinates = aligned coordinates for reference frame (dx=0, dy=0)
      int cx = static_cast<int>( c.sumX / c.area );
      int cy = static_cast<int>( c.sumY / c.area );

      // Measure true radial extent from raw deficit data.
      // The detection threshold finds the dense core; trace outward in the
      // self-flat deficit map to find where the mote fades into background.
      // Use half the detection sigma as the extent threshold.
      float extentThreshold = medianPct + static_cast<float>( m_config.dustDetectionSigma * 0.5 ) * madPct;
      int maxExtentRadius = m_config.dustMaxDiameter / 2;
      int detectedRadius = std::max( 5, static_cast<int>( diameter / 2 ) );
      int maskRadius = detectedRadius;

      int belowCount = 0;
      for ( int r = detectedRadius + 1; r <= maxExtentRadius; ++r )
      {
         double ringSum = 0;
         int ringCount = 0;
         int rInner = std::max( 0, r - 2 );
         int rInnerSq = rInner * rInner;
         int rOuterSq = r * r;
         for ( int dy = -r; dy <= r; ++dy )
            for ( int dx = -r; dx <= r; ++dx )
            {
               int distSq = dx * dx + dy * dy;
               if ( distSq < rInnerSq || distSq > rOuterSq ) continue;
               int px = cx + dx, py = cy + dy;
               if ( px >= 0 && px < width && py >= 0 && py < height
                    && sensorValid[py * width + px] )
               {
                  ringSum += pctDeficit[py * width + px];
                  ++ringCount;
               }
            }
         if ( ringCount == 0 ) break;
         if ( ringSum / ringCount > extentThreshold )
         {
            maskRadius = r;
            belowCount = 0;
         }
         else
         {
            if ( ++belowCount >= 3 )
               break;
         }
      }

      {
         std::ostringstream oss;
         oss << "[DustDetect] Dust blob (sensor-space): center=(" << cx << "," << cy
             << "), detected_diam=" << diameter << ", area=" << c.area
             << ", circ=" << circularity
             << ", maskR=" << maskRadius << " (core=" << detectedRadius << ")";
         emit( oss.str() );
      }

      DustBlobInfo blob;
      blob.centerX = cx;
      blob.centerY = cy;
      blob.radius = maskRadius;
      blob.circularity = circularity;
      blob.meanAttenuation = 0;
      result.blobs.push_back( blob );

      // Paint filled circle mask and self-flat correction map
      int maskRadiusSq = maskRadius * maskRadius;
      for ( int my = std::max( 0, cy - maskRadius ); my <= std::min( height - 1, cy + maskRadius ); ++my )
         for ( int mx = std::max( 0, cx - maskRadius ); mx <= std::min( width - 1, cx + maskRadius ); ++mx )
         {
            int ddx = mx - cx, ddy = my - cy;
            if ( ddx * ddx + ddy * ddy <= maskRadiusSq )
            {
               int idx = my * width + mx;
               result.mask[idx] = 1;
               // Correction = inverse of self-flat normalized value.
               // normalized ≈ 0.95 for 5% deficit → correction ≈ 1.053
               // Clamp to 2.0 at source to prevent catastrophic overcorrection.
               if ( normalized[idx] > 0.01f )
                  result.correctionMap[idx] = std::min( 1.0f / normalized[idx], 2.0f );
            }
         }
   }

   for ( int i = 0; i < N; ++i )
      if ( result.mask[i] )
         ++result.dustPixelCount;

   emit( "[DustDetect] Result: " + std::to_string( result.blobs.size() )
         + " blobs, " + std::to_string( result.dustPixelCount ) + " pixels" );

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
