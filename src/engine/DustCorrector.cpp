// ----------------------------------------------------------------------------
// DustCorrector.cpp — Edge-referenced dust mote correction with gradient support
//
// Algorithm:
//   1. For each blob, sample an annular ring of pixels at [R+2, R+5] from
//      the blob center, binned by angle (72 bins of 5 degrees each).
//   2. For each interior pixel at (d, theta), estimate the expected background
//      by diametral interpolation: linearly interpolate between the edge
//      brightness at theta and at theta+PI, weighted by position along
//      the diameter.  This correctly handles linear gradients.
//   3. Compute per-pixel correction = expectedBg / actualPixel, clamped to
//      [1.0, MAX_CORRECTION].  Smooth the correction field radially.
//   4. Apply the correction.
//
// Copyright (c) 2026 Scott Carter
// ----------------------------------------------------------------------------

#include "engine/DustCorrector.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <vector>

namespace nukex {

// ============================================================================
// correct — apply edge-referenced correction with gradient awareness
// ============================================================================

void DustCorrector::correct( float* image, int width, int height,
                              const std::vector<DustBlobInfo>& blobs,
                              LogCallback log ) const
{
   if ( blobs.empty() )
      return;

   const double PI = 3.14159265358979323846;
   const double TWO_PI = 2.0 * PI;
   const double BIN_WIDTH = TWO_PI / ANGULAR_BINS;

   for ( size_t bi = 0; bi < blobs.size(); ++bi )
   {
      const DustBlobInfo& blob = blobs[bi];
      const double cx = blob.centerX;
      const double cy = blob.centerY;
      const int R = std::max( 1, static_cast<int>( std::round( blob.radius ) ) );

      // ------------------------------------------------------------------
      // Step A: Sample edge ring — annulus at [R + EDGE_INNER, R + EDGE_OUTER]
      // ------------------------------------------------------------------

      std::vector<double> edgeSum( ANGULAR_BINS, 0.0 );
      std::vector<int>    edgeCount( ANGULAR_BINS, 0 );

      const int outerR = R + EDGE_OUTER;
      const int innerR = R + EDGE_INNER;

      for ( int dy = -outerR; dy <= outerR; ++dy )
      {
         for ( int dx = -outerR; dx <= outerR; ++dx )
         {
            double dist = std::sqrt( double(dx * dx + dy * dy) );
            if ( dist < innerR || dist > outerR )
               continue;

            int px = static_cast<int>( std::round( cx ) ) + dx;
            int py = static_cast<int>( std::round( cy ) ) + dy;
            if ( px < 0 || px >= width || py < 0 || py >= height )
               continue;

            double angle = std::atan2( double(dy), double(dx) ) + PI;
            int bin = static_cast<int>( angle / BIN_WIDTH );
            if ( bin >= ANGULAR_BINS )
               bin = ANGULAR_BINS - 1;

            edgeSum[bin] += double( image[py * width + px] );
            edgeCount[bin]++;
         }
      }

      // Compute mean edge brightness per bin
      std::vector<double> edgeBrightness( ANGULAR_BINS, 0.0 );
      for ( int b = 0; b < ANGULAR_BINS; ++b )
      {
         if ( edgeCount[b] > 0 )
            edgeBrightness[b] = edgeSum[b] / edgeCount[b];
      }

      // Fill empty bins from nearest populated neighbors (handles border blobs)
      for ( int b = 0; b < ANGULAR_BINS; ++b )
      {
         if ( edgeCount[b] > 0 )
            continue;

         for ( int offset = 1; offset <= ANGULAR_BINS / 2; ++offset )
         {
            int prev = ( b - offset + ANGULAR_BINS ) % ANGULAR_BINS;
            int next = ( b + offset ) % ANGULAR_BINS;
            if ( edgeCount[prev] > 0 && edgeCount[next] > 0 )
            {
               edgeBrightness[b] = 0.5 * ( edgeBrightness[prev] + edgeBrightness[next] );
               break;
            }
            else if ( edgeCount[prev] > 0 )
            {
               edgeBrightness[b] = edgeBrightness[prev];
               break;
            }
            else if ( edgeCount[next] > 0 )
            {
               edgeBrightness[b] = edgeBrightness[next];
               break;
            }
         }
      }

      // ------------------------------------------------------------------
      // Step B + C: For each interior pixel, estimate expected background
      //             via diametral interpolation, compute and apply correction.
      //
      // For a pixel at distance d from center at angle theta:
      //   edge_same = edgeBrightness[theta]
      //   edge_opp  = edgeBrightness[theta + PI]
      //   expectedBg = edge_opp + (edge_same - edge_opp) * (d + R) / (2R)
      //
      // This yields edge_same at d=R, edge_opp at d=-R, and the average
      // at d=0 (center), correctly modeling a linear gradient.
      //
      // correction = expectedBg / actualPixel (clamped to [1, MAX_CORRECTION])
      // ------------------------------------------------------------------

      int correctedPixels = 0;
      double maxApplied = 1.0;
      const int halfBins = ANGULAR_BINS / 2;

      for ( int dy = -R; dy <= R; ++dy )
      {
         for ( int dx = -R; dx <= R; ++dx )
         {
            double dist = std::sqrt( double(dx * dx + dy * dy) );
            if ( dist > double(R) )
               continue;

            int px = static_cast<int>( std::round( cx ) ) + dx;
            int py = static_cast<int>( std::round( cy ) ) + dy;
            if ( px < 0 || px >= width || py < 0 || py >= height )
               continue;

            // Angular bin for this pixel
            double angle = std::atan2( double(dy), double(dx) ) + PI;
            int bin = static_cast<int>( angle / BIN_WIDTH );
            if ( bin >= ANGULAR_BINS )
               bin = ANGULAR_BINS - 1;

            // Opposite bin (180 degrees away)
            int oppBin = ( bin + halfBins ) % ANGULAR_BINS;

            double edgeSame = edgeBrightness[bin];
            double edgeOpp  = edgeBrightness[oppBin];

            // Diametral interpolation: position along the diameter
            // At d=R (at edge, same side): t = 1.0 -> expectedBg = edgeSame
            // At d=0 (center):             t = 0.5 -> expectedBg = (edgeSame + edgeOpp) / 2
            // At d=-R (opposite edge):     t = 0.0 -> expectedBg = edgeOpp
            double t = ( dist + double(R) ) / ( 2.0 * double(R) );
            double expectedBg = edgeOpp + ( edgeSame - edgeOpp ) * t;

            // Actual pixel value
            double actual = double( image[py * width + px] );

            // Compute correction
            double corr = 1.0;
            if ( actual > 1e-12 && expectedBg > 1e-12 )
            {
               corr = expectedBg / actual;
            }

            // Clamp: never darken, never boost beyond MAX_CORRECTION
            if ( corr < 1.0 )
               corr = 1.0;
            if ( corr > double(MAX_CORRECTION) )
               corr = double(MAX_CORRECTION);

            // Apply
            float& pixel = image[py * width + px];
            pixel = float( double(pixel) * corr );
            if ( pixel > 1.0f )
               pixel = 1.0f;

            correctedPixels++;
            if ( corr > maxApplied )
               maxApplied = corr;
         }
      }

      // ------------------------------------------------------------------
      // Diagnostics
      // ------------------------------------------------------------------

      if ( log )
      {
         std::ostringstream msg;
         msg << "DustCorrector: blob " << bi
             << " center=(" << cx << "," << cy << ")"
             << " R=" << R
             << " maxCorr=" << maxApplied
             << " pixels=" << correctedPixels;
         log( msg.str() );
      }
   }
}

} // namespace nukex
