// ----------------------------------------------------------------------------
// DustCorrector.cpp — Edge-referenced radial profile dust mote correction
//
// Algorithm:
//   1. For each blob, sample an annular ring of pixels at [R+2, R+5] from
//      the blob center, binned by angle (72 bins of 5 degrees each).
//   2. For each radius r from 0..R, sample interior brightness per angular
//      bin.  Compute correction[r][bin] = edgeBrightness[bin] / interior[bin].
//      Collapse to azimuthal mean per radius.
//   3. Smooth the radial profile with a 3-point running average.
//      Force profile[R] = 1.0 (boundary continuity).
//   4. For each pixel inside the blob, multiply by interpolated correction
//      from the profile at its distance from center.
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
// correct — apply edge-referenced radial correction to each blob
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
      // Step 1: Sample edge ring — annulus at [R + EDGE_INNER, R + EDGE_OUTER]
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

            // Angle in [0, 2pi)
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

         // Search outward in both directions
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
      // Step 2: Build per-radius correction profile
      //
      // For each integer radius r (0..R-1), sample interior brightness in
      // each angular bin, compute per-bin correction, then azimuthal mean.
      // ------------------------------------------------------------------

      std::vector<double> correctionProfile( R + 1, 1.0 );

      for ( int r = 0; r < R; ++r )
      {
         // Ring at radius r: sample pixels at distance [r, r+1)
         double rInner = ( r == 0 ) ? 0.0 : double(r);
         double rOuter = double(r + 1);

         std::vector<double> ringSum( ANGULAR_BINS, 0.0 );
         std::vector<int>    ringCount( ANGULAR_BINS, 0 );

         int scanR = r + 1;
         for ( int dy = -scanR; dy <= scanR; ++dy )
         {
            for ( int dx = -scanR; dx <= scanR; ++dx )
            {
               double dist = std::sqrt( double(dx * dx + dy * dy) );
               if ( dist < rInner || dist >= rOuter )
                  continue;

               int px = static_cast<int>( std::round( cx ) ) + dx;
               int py = static_cast<int>( std::round( cy ) ) + dy;
               if ( px < 0 || px >= width || py < 0 || py >= height )
                  continue;

               double angle = std::atan2( double(dy), double(dx) ) + PI;
               int bin = static_cast<int>( angle / BIN_WIDTH );
               if ( bin >= ANGULAR_BINS )
                  bin = ANGULAR_BINS - 1;

               ringSum[bin] += double( image[py * width + px] );
               ringCount[bin]++;
            }
         }

         // Azimuthal mean of per-bin correction ratios
         double totalCorr = 0.0;
         int    validBins = 0;
         for ( int b = 0; b < ANGULAR_BINS; ++b )
         {
            if ( ringCount[b] > 0 && edgeBrightness[b] > 1e-12 )
            {
               double interiorMean = ringSum[b] / ringCount[b];
               if ( interiorMean > 1e-12 )
               {
                  double ratio = edgeBrightness[b] / interiorMean;
                  totalCorr += ratio;
                  validBins++;
               }
            }
         }

         if ( validBins > 0 )
            correctionProfile[r] = totalCorr / validBins;
         else
            correctionProfile[r] = 1.0;
      }

      // Boundary: force correction at edge to 1.0
      correctionProfile[R] = 1.0;

      // ------------------------------------------------------------------
      // Step 3: Smooth the profile — 3-point running average
      // ------------------------------------------------------------------

      if ( R >= 2 )
      {
         std::vector<double> smoothed( R + 1 );
         smoothed[0] = correctionProfile[0];
         smoothed[R] = 1.0;  // boundary stays at 1.0
         for ( int r = 1; r < R; ++r )
            smoothed[r] = ( correctionProfile[r - 1]
                          + correctionProfile[r]
                          + correctionProfile[r + 1] ) / 3.0;
         correctionProfile = smoothed;
      }

      // Force boundary again after smoothing
      correctionProfile[R] = 1.0;

      // Clamp corrections
      for ( int r = 0; r <= R; ++r )
      {
         if ( correctionProfile[r] > double(MAX_CORRECTION) )
            correctionProfile[r] = double(MAX_CORRECTION);
         if ( correctionProfile[r] < 1.0 )
            correctionProfile[r] = 1.0;  // never darken pixels
      }

      // ------------------------------------------------------------------
      // Step 4: Apply per-pixel correction
      // ------------------------------------------------------------------

      int correctedPixels = 0;
      double maxApplied = 1.0;

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

            // Linearly interpolate correction from the profile
            double corr;
            if ( dist <= 0.0 )
            {
               corr = correctionProfile[0];
            }
            else
            {
               int rLow = static_cast<int>( dist );
               if ( rLow >= R )
               {
                  corr = 1.0;
               }
               else
               {
                  double frac = dist - double(rLow);
                  corr = correctionProfile[rLow] * ( 1.0 - frac )
                       + correctionProfile[rLow + 1] * frac;
               }
            }

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
             << " centerCorr=" << correctionProfile[0]
             << " maxCorr=" << maxApplied
             << " pixels=" << correctedPixels;
         log( msg.str() );
      }
   }
}

} // namespace nukex
