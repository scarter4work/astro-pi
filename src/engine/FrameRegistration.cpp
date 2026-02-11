//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// FrameRegistration - Triangle-matching star alignment for subframe stacking

#include "FrameRegistration.h"

#include <pcl/Console.h>
#include <pcl/FFTRegistration.h>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <chrono>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace pcl
{

// ----------------------------------------------------------------------------
// Star detection using simple peak-finding on luminance
// ----------------------------------------------------------------------------

std::vector<DetectedStar> FrameRegistration::DetectStarsInFrame( const Image& image, int maxStars )
{
   std::vector<DetectedStar> stars;

   int width = image.Width();
   int height = image.Height();
   int channels = image.NumberOfChannels();

   if ( width < 32 || height < 32 )
      return stars;

   // Create luminance image
   std::vector<float> lum( size_t( width ) * height, 0.0f );

   for ( int y = 0; y < height; ++y )
      for ( int x = 0; x < width; ++x )
      {
         float val = 0;
         for ( int c = 0; c < channels; ++c )
            val += image.Pixel( x, y, c );
         lum[size_t( y ) * width + x] = val / channels;
      }

   // Compute background statistics for thresholding
   std::vector<float> sorted( lum );
   std::sort( sorted.begin(), sorted.end() );

   double median = sorted[sorted.size() / 2];
   double mad = 0;
   {
      std::vector<float> devs( sorted.size() );
      for ( size_t i = 0; i < sorted.size(); ++i )
         devs[i] = std::abs( sorted[i] - static_cast<float>( median ) );
      std::sort( devs.begin(), devs.end() );
      mad = devs[devs.size() / 2] * 1.4826; // MAD to sigma
   }

   // Detection threshold: median + sensitivity-scaled sigma
   // Higher sensitivity = lower threshold = more stars
   double sigmaMultiple = 3.0 + ( 1.0 - m_config.sensitivity ) * 12.0; // 3-15 sigma
   double threshold = median + sigmaMultiple * mad;

   if ( threshold <= median || mad < 1e-10 )
      return stars; // Flat image, no stars

   // Find local maxima above threshold (simple 5x5 neighborhood)
   int border = 8; // Avoid edge artifacts
   for ( int y = border; y < height - border; ++y )
   {
      for ( int x = border; x < width - border; ++x )
      {
         float val = lum[size_t( y ) * width + x];
         if ( val < threshold )
            continue;

         // Check if local maximum in 5x5
         bool isMax = true;
         for ( int dy = -2; dy <= 2 && isMax; ++dy )
            for ( int dx = -2; dx <= 2 && isMax; ++dx )
            {
               if ( dx == 0 && dy == 0 )
                  continue;
               if ( lum[size_t( y + dy ) * width + ( x + dx )] > val )
                  isMax = false;
            }

         if ( !isMax )
            continue;

         // Compute centroid in 5x5 window (intensity-weighted)
         double sumW = 0, sumWx = 0, sumWy = 0;
         for ( int dy = -2; dy <= 2; ++dy )
            for ( int dx = -2; dx <= 2; ++dx )
            {
               float w = lum[size_t( y + dy ) * width + ( x + dx )];
               if ( w > static_cast<float>( median ) )
               {
                  double ww = w - median;
                  sumW += ww;
                  sumWx += ww * ( x + dx );
                  sumWy += ww * ( y + dy );
               }
            }

         if ( sumW > 0 )
         {
            DetectedStar star;
            star.x = sumWx / sumW;
            star.y = sumWy / sumW;

            // Compute flux as sum above background in 5x5
            double flux = 0;
            for ( int dy = -2; dy <= 2; ++dy )
               for ( int dx = -2; dx <= 2; ++dx )
               {
                  float v = lum[size_t( y + dy ) * width + ( x + dx )];
                  if ( v > static_cast<float>( median ) )
                     flux += v - median;
               }
            star.flux = flux;
            stars.push_back( star );
         }
      }
   }

   // Sort by flux descending and keep top N
   std::sort( stars.begin(), stars.end(),
              []( const DetectedStar& a, const DetectedStar& b ) { return a.flux > b.flux; } );

   if ( static_cast<int>( stars.size() ) > maxStars )
      stars.resize( maxStars );

   return stars;
}

// ----------------------------------------------------------------------------
// Build scale-invariant triangles from brightest stars
// ----------------------------------------------------------------------------

std::vector<StarTriangle> FrameRegistration::BuildTriangles(
   const std::vector<DetectedStar>& stars,
   int maxStars,
   int maxTriangles )
{
   std::vector<StarTriangle> triangles;

   int n = std::min( static_cast<int>( stars.size() ), maxStars );
   if ( n < 3 )
      return triangles;

   triangles.reserve( std::min( maxTriangles, n * ( n - 1 ) * ( n - 2 ) / 6 ) );

   for ( int i = 0; i < n && static_cast<int>( triangles.size() ) < maxTriangles; ++i )
   {
      for ( int j = i + 1; j < n && static_cast<int>( triangles.size() ) < maxTriangles; ++j )
      {
         for ( int k = j + 1; k < n && static_cast<int>( triangles.size() ) < maxTriangles; ++k )
         {
            // Compute side lengths
            double d01 = std::hypot( stars[i].x - stars[j].x, stars[i].y - stars[j].y );
            double d02 = std::hypot( stars[i].x - stars[k].x, stars[i].y - stars[k].y );
            double d12 = std::hypot( stars[j].x - stars[k].x, stars[j].y - stars[k].y );

            // Sort sides: s0 >= s1 >= s2
            double sides[3] = { d01, d02, d12 };
            std::sort( sides, sides + 3, std::greater<double>() );

            if ( sides[0] < 1.0 ) // Degenerate triangle
               continue;

            StarTriangle tri;
            tri.i = i;
            tri.j = j;
            tri.k = k;
            tri.r1 = sides[1] / sides[0];
            tri.r2 = sides[2] / sides[0];

            triangles.push_back( tri );
         }
      }
   }

   return triangles;
}

// ----------------------------------------------------------------------------
// Match triangles between reference and target by comparing shape ratios
// ----------------------------------------------------------------------------

std::vector<StarMatch> FrameRegistration::MatchTriangles(
   const std::vector<StarTriangle>& refTri,
   const std::vector<StarTriangle>& targetTri,
   const std::vector<DetectedStar>& refStars,
   const std::vector<DetectedStar>& targetStars )
{
   int nRef = static_cast<int>( refStars.size() );
   int nTarget = static_cast<int>( targetStars.size() );

   if ( nRef == 0 || nTarget == 0 )
      return {};

   // Vote matrix: votes[refStarIdx][targetStarIdx]
   // Use only the triangle star subset (indices 0..triangleStars-1)
   int voteN = std::min( nRef, m_config.triangleStars );
   int voteM = std::min( nTarget, m_config.triangleStars );
   std::vector<std::vector<int>> votes( voteN, std::vector<int>( voteM, 0 ) );

   double tol = m_config.ratioTolerance;

   // Sort target triangles by r1 for binary search
   std::vector<StarTriangle> sortedTarget( targetTri );
   std::sort( sortedTarget.begin(), sortedTarget.end(),
              []( const StarTriangle& a, const StarTriangle& b ) { return a.r1 < b.r1; } );

   for ( const auto& rt : refTri )
   {
      // Binary search for target triangles with r1 in [rt.r1 - tol, rt.r1 + tol]
      double r1Low = rt.r1 - tol;
      double r1High = rt.r1 + tol;

      auto itLow = std::lower_bound( sortedTarget.begin(), sortedTarget.end(), r1Low,
         []( const StarTriangle& t, double val ) { return t.r1 < val; } );

      for ( auto it = itLow; it != sortedTarget.end() && it->r1 <= r1High; ++it )
      {
         const auto& tt = *it;

         // Check r2 tolerance
         if ( std::abs( rt.r2 - tt.r2 ) >= tol )
            continue;

         // Triangle shapes match. Resolve vertex mapping.
         int refVerts[3] = { rt.i, rt.j, rt.k };
         int tarVerts[3] = { tt.i, tt.j, tt.k };

         // Skip if any vertex index is outside the vote matrix bounds
         bool skip = false;
         for ( int m = 0; m < 3; ++m )
         {
            if ( refVerts[m] >= voteN || tarVerts[m] >= voteM )
            {
               skip = true;
               break;
            }
         }
         if ( skip )
            continue;

         // Try all 6 vertex permutations, pick the one most consistent with existing votes
         static const int perms[6][3] = {
            {0,1,2}, {0,2,1}, {1,0,2}, {1,2,0}, {2,0,1}, {2,1,0}
         };

         int bestPerm = 0;
         int bestVoteSum = -1;

         for ( int p = 0; p < 6; ++p )
         {
            int v = 0;
            for ( int m = 0; m < 3; ++m )
               v += votes[refVerts[m]][tarVerts[perms[p][m]]];
            if ( v > bestVoteSum )
            {
               bestVoteSum = v;
               bestPerm = p;
            }
         }

         // Add votes for best permutation
         for ( int m = 0; m < 3; ++m )
            votes[refVerts[m]][tarVerts[perms[bestPerm][m]]]++;
      }
   }

   // Extract matches: for each ref star, find best target star
   // Require at least 3 votes for a high-confidence seed match
   std::vector<StarMatch> matches;
   int minVotesThreshold = 3;

   for ( int ri = 0; ri < voteN; ++ri )
   {
      int bestTarget = -1;
      int bestVotes = minVotesThreshold - 1;

      for ( int ti = 0; ti < voteM; ++ti )
      {
         if ( votes[ri][ti] > bestVotes )
         {
            bestVotes = votes[ri][ti];
            bestTarget = ti;
         }
      }

      if ( bestTarget >= 0 )
      {
         // Verify mutual best: this target's best ref match should be this ref star
         bool mutual = true;
         for ( int ri2 = 0; ri2 < voteN; ++ri2 )
         {
            if ( ri2 != ri && votes[ri2][bestTarget] > bestVotes )
            {
               mutual = false;
               break;
            }
         }

         if ( mutual )
         {
            StarMatch match;
            match.refIndex = ri;
            match.targetIndex = bestTarget;
            match.residual = 0;
            matches.push_back( match );
         }
      }
   }

   return matches;
}

// ----------------------------------------------------------------------------
// Compute similarity transform via least-squares
// 4 DOF: x' = a*x - b*y + tx,  y' = b*x + a*y + ty
// ----------------------------------------------------------------------------

SimilarityTransform FrameRegistration::ComputeSimilarityTransform(
   const std::vector<DetectedStar>& refStars,
   const std::vector<DetectedStar>& targetStars,
   const std::vector<StarMatch>& matches )
{
   SimilarityTransform T;

   int n = static_cast<int>( matches.size() );
   if ( n < 2 )
      return T;

   // Least-squares: minimize sum of |T(target) - ref|^2
   // We want: ref = T(target), i.e., map target coordinates to reference
   //   rx = a * tx - b * ty + tx_offset
   //   ry = b * tx + a * ty + ty_offset
   //
   // Normal equations (4x4 system):
   // [sum(tx^2+ty^2)  0                sum(tx)  sum(ty)] [a ]   [sum(rx*tx + ry*ty)]
   // [0               sum(tx^2+ty^2)  -sum(ty)  sum(tx)] [b ]   [sum(ry*tx - rx*ty)]
   // [sum(tx)         -sum(ty)         n        0      ] [tx] = [sum(rx)            ]
   // [sum(ty)         sum(tx)          0        n      ] [ty]   [sum(ry)            ]

   double Sxx = 0, Sxy = 0, Sx = 0, Sy = 0;
   double Srx_tx = 0, Sry_ty = 0, Sry_tx = 0, Srx_ty = 0;
   double Srx = 0, Sry = 0;

   for ( const auto& m : matches )
   {
      double tx = targetStars[m.targetIndex].x;
      double ty = targetStars[m.targetIndex].y;
      double rx = refStars[m.refIndex].x;
      double ry = refStars[m.refIndex].y;

      Sxx += tx * tx + ty * ty;
      Sx += tx;
      Sy += ty;
      Srx_tx += rx * tx;
      Sry_ty += ry * ty;
      Sry_tx += ry * tx;
      Srx_ty += rx * ty;
      Srx += rx;
      Sry += ry;
   }

   // Solve using direct formulas for similarity transform
   // a, b from:
   //   a * Sxx + tx_off * Sx + ty_off * Sy = Srx_tx + Sry_ty
   //   b * Sxx - tx_off * Sy + ty_off * Sx = Sry_tx - Srx_ty
   //   a * Sx - b * Sy + n * tx_off = Srx
   //   a * Sy + b * Sx + n * ty_off = Sry

   // From the last two equations:
   //   tx_off = (Srx - a * Sx + b * Sy) / n
   //   ty_off = (Sry - a * Sy - b * Sx) / n

   // Substituting back:
   double nn = static_cast<double>( n );
   double denom = Sxx - ( Sx * Sx + Sy * Sy ) / nn;

   if ( std::abs( denom ) < 1e-12 )
      return T; // Degenerate

   double num_a = ( Srx_tx + Sry_ty ) - ( Sx * Srx + Sy * Sry ) / nn;
   double num_b = ( Sry_tx - Srx_ty ) - ( Sx * Sry - Sy * Srx ) / nn;

   T.a = num_a / denom;
   T.b = num_b / denom;
   T.tx = ( Srx - T.a * Sx + T.b * Sy ) / nn;
   T.ty = ( Sry - T.b * Sx - T.a * Sy ) / nn;

   return T;
}

// ----------------------------------------------------------------------------
// Refine matches: reject outliers via MAD-based sigma clipping, recompute
// ----------------------------------------------------------------------------

SimilarityTransform FrameRegistration::RefineMatches(
   const std::vector<DetectedStar>& refStars,
   const std::vector<DetectedStar>& targetStars,
   std::vector<StarMatch>& matches )
{
   if ( matches.size() < 3 )
      return ComputeSimilarityTransform( refStars, targetStars, matches );

   SimilarityTransform T = ComputeSimilarityTransform( refStars, targetStars, matches );

   // Iterative sigma clipping (2 passes)
   for ( int pass = 0; pass < 2; ++pass )
   {
      if ( matches.size() < 3 )
         break;

      // Compute residuals
      for ( auto& m : matches )
      {
         double xp, yp;
         T.Apply( targetStars[m.targetIndex].x, targetStars[m.targetIndex].y, xp, yp );
         double dx = xp - refStars[m.refIndex].x;
         double dy = yp - refStars[m.refIndex].y;
         m.residual = std::sqrt( dx * dx + dy * dy );
      }

      // Compute MAD of residuals
      std::vector<double> residuals;
      residuals.reserve( matches.size() );
      for ( const auto& m : matches )
         residuals.push_back( m.residual );

      std::sort( residuals.begin(), residuals.end() );
      double medianRes = residuals[residuals.size() / 2];

      std::vector<double> devs;
      devs.reserve( residuals.size() );
      for ( double r : residuals )
         devs.push_back( std::abs( r - medianRes ) );
      std::sort( devs.begin(), devs.end() );
      double madSigma = devs[devs.size() / 2] * 1.4826;

      if ( madSigma < 1e-10 )
         break; // All residuals essentially identical

      double clipThreshold = medianRes + m_config.outlierSigma * madSigma;

      // Remove outliers
      std::vector<StarMatch> refined;
      refined.reserve( matches.size() );
      for ( const auto& m : matches )
      {
         if ( m.residual <= clipThreshold )
            refined.push_back( m );
      }

      if ( refined.size() < 3 || refined.size() == matches.size() )
      {
         matches = std::move( refined );
         break;
      }

      matches = std::move( refined );
      T = ComputeSimilarityTransform( refStars, targetStars, matches );
   }

   return T;
}

// ----------------------------------------------------------------------------
// Expand matches using transform: project ref stars, find nearest target neighbors
// This dramatically increases match count after an initial seed alignment
// ----------------------------------------------------------------------------

std::vector<StarMatch> FrameRegistration::ExpandMatchesWithTransform(
   const std::vector<DetectedStar>& refStars,
   const std::vector<DetectedStar>& targetStars,
   const SimilarityTransform& T,
   double searchRadius )
{
   std::vector<StarMatch> expanded;
   double radiusSq = searchRadius * searchRadius;

   // For each target star, transform to reference space and find nearest ref star
   // (T maps target->ref, so T.Apply(target) gives predicted ref position)
   std::vector<bool> refUsed( refStars.size(), false );
   std::vector<bool> targetUsed( targetStars.size(), false );

   // Build candidate list with distances
   struct Candidate {
      int refIdx;
      int targetIdx;
      double distSq;
   };
   std::vector<Candidate> candidates;

   for ( int ti = 0; ti < static_cast<int>( targetStars.size() ); ++ti )
   {
      // Transform target star to reference space
      double mappedX, mappedY;
      T.Apply( targetStars[ti].x, targetStars[ti].y, mappedX, mappedY );

      // Find nearest ref star
      int bestRef = -1;
      double bestDistSq = radiusSq;

      for ( int ri = 0; ri < static_cast<int>( refStars.size() ); ++ri )
      {
         double dx = mappedX - refStars[ri].x;
         double dy = mappedY - refStars[ri].y;
         double dSq = dx * dx + dy * dy;
         if ( dSq < bestDistSq )
         {
            bestDistSq = dSq;
            bestRef = ri;
         }
      }

      if ( bestRef >= 0 )
      {
         Candidate c;
         c.refIdx = bestRef;
         c.targetIdx = ti;
         c.distSq = bestDistSq;
         candidates.push_back( c );
      }
   }

   // Sort by distance and greedily assign (closest first, no duplicates)
   std::sort( candidates.begin(), candidates.end(),
              []( const Candidate& a, const Candidate& b ) { return a.distSq < b.distSq; } );

   for ( const auto& c : candidates )
   {
      if ( !refUsed[c.refIdx] && !targetUsed[c.targetIdx] )
      {
         StarMatch match;
         match.refIndex = c.refIdx;
         match.targetIndex = c.targetIdx;
         match.residual = std::sqrt( c.distSq );
         expanded.push_back( match );
         refUsed[c.refIdx] = true;
         targetUsed[c.targetIdx] = true;
      }
   }

   return expanded;
}

// ----------------------------------------------------------------------------
// Catmull-Rom spline weight for bicubic interpolation
// ----------------------------------------------------------------------------

double FrameRegistration::CatmullRom( double t )
{
   double at = std::abs( t );
   if ( at < 1.0 )
      return 0.5 * ( 2.0 - 5.0 * at * at + 3.0 * at * at * at );  // Catmull-Rom a=-0.5 variant simplified
   else if ( at < 2.0 )
      return 0.5 * ( 4.0 - 8.0 * at + 5.0 * at * at - at * at * at );
   return 0.0;
}

// ----------------------------------------------------------------------------
// Apply similarity transform via bicubic resampling
// ----------------------------------------------------------------------------

void FrameRegistration::ApplyTransform( Image& frame, const SimilarityTransform& T )
{
   int width = frame.Width();
   int height = frame.Height();
   int channels = frame.NumberOfChannels();

   // We need the INVERSE transform: for each output pixel, find source location
   // Forward: ref = T(target)
   // We're transforming target frame TO reference space.
   // So for each pixel (x,y) in output (ref space), find source in target:
   //   target = T^{-1}(ref)
   //
   // T: x' = a*x - b*y + tx,  y' = b*x + a*y + ty
   // T^{-1}: x = (a*(x'-tx) + b*(y'-ty)) / (a^2+b^2)
   //         y = (-b*(x'-tx) + a*(y'-ty)) / (a^2+b^2)

   double det = T.a * T.a + T.b * T.b;
   if ( det < 1e-15 )
      return; // Degenerate

   double invDet = 1.0 / det;
   double ia =  T.a * invDet;
   double ib = -T.b * invDet;  // Note sign
   double itx = -( ia * T.tx + ib * T.ty );  // Actually: -(ia*tx - ib_neg*ty) ... let me redo
   // Correct inverse:
   // x_src = ia*(x_out - tx) + ib_pos*(y_out - ty)  where ia = a/det, ib_pos = b/det
   // y_src = -ib_pos*(x_out - tx) + ia*(y_out - ty)
   // Let's just compute it directly:

   // Create output buffer
   std::vector<float> output( size_t( width ) * height * channels, 0.0f );

   for ( int c = 0; c < channels; ++c )
   {
      #ifdef _OPENMP
      #pragma omp parallel for schedule(dynamic, 16)
      #endif
      for ( int y = 0; y < height; ++y )
      {
         for ( int x = 0; x < width; ++x )
         {
            // Inverse transform: find source coordinates
            double dx = x - T.tx;
            double dy = y - T.ty;
            double srcX = ( T.a * dx + T.b * dy ) * invDet;
            double srcY = ( -T.b * dx + T.a * dy ) * invDet;

            // Bicubic interpolation
            int ix = static_cast<int>( std::floor( srcX ) );
            int iy = static_cast<int>( std::floor( srcY ) );
            double fx = srcX - ix;
            double fy = srcY - iy;

            // Check if completely out of bounds
            if ( ix < -1 || ix >= width + 1 || iy < -1 || iy >= height + 1 )
            {
               output[( size_t( c ) * height + y ) * width + x] = 0.0f;
               continue;
            }

            double val = 0;
            double wsum = 0;

            for ( int jj = -1; jj <= 2; ++jj )
            {
               double wy = CatmullRom( fy - jj );
               int sy = iy + jj;
               if ( sy < 0 || sy >= height )
                  continue;

               for ( int ii = -1; ii <= 2; ++ii )
               {
                  double wx = CatmullRom( fx - ii );
                  int sx = ix + ii;
                  if ( sx < 0 || sx >= width )
                     continue;

                  double w = wx * wy;
                  val += w * frame.Pixel( sx, sy, c );
                  wsum += w;
               }
            }

            if ( wsum > 0 )
               val /= wsum;

            // Clamp to [0,1]
            if ( val < 0 ) val = 0;
            if ( val > 1 ) val = 1;

            output[( size_t( c ) * height + y ) * width + x] = static_cast<float>( val );
         }
      }
   }

   // Copy result back to frame
   for ( int c = 0; c < channels; ++c )
      for ( int y = 0; y < height; ++y )
         for ( int x = 0; x < width; ++x )
            frame.Pixel( x, y, c ) = output[( size_t( c ) * height + y ) * width + x];
}

// ----------------------------------------------------------------------------
// Create luminance image from RGB
// ----------------------------------------------------------------------------

Image FrameRegistration::CreateLuminance( const Image& image )
{
   int width = image.Width();
   int height = image.Height();
   int channels = image.NumberOfChannels();

   Image lum;
   lum.AllocateData( width, height, 1, ColorSpace::Gray );

   if ( channels >= 3 )
   {
      for ( int y = 0; y < height; ++y )
         for ( int x = 0; x < width; ++x )
            lum.Pixel( x, y, 0 ) = 0.2126f * image.Pixel( x, y, 0 )
                                  + 0.7152f * image.Pixel( x, y, 1 )
                                  + 0.0722f * image.Pixel( x, y, 2 );
   }
   else
   {
      for ( int y = 0; y < height; ++y )
         for ( int x = 0; x < width; ++x )
            lum.Pixel( x, y, 0 ) = image.Pixel( x, y, 0 );
   }

   return lum;
}

// ----------------------------------------------------------------------------
// Phase correlation registration (FFT-based translation detection)
// ----------------------------------------------------------------------------

FrameRegistrationResult FrameRegistration::RegisterFramePhaseCorrelation(
   Image& frame,
   int frameIndex )
{
   FrameRegistrationResult result;
   result.frameIndex = frameIndex;
   result.method = RegistrationMethod::PhaseCorrelation;

   if ( !m_referenceInitialized )
   {
      result.success = false;
      result.message = String().Format( "Frame %d: Phase correlation skipped (no reference luminance)",
         frameIndex + 1 );
      return result;
   }

   // Create luminance of target frame
   Image targetLum = CreateLuminance( frame );

   // Run FFT phase correlation for translation
   FFTTranslation fftTranslation;
   fftTranslation.Initialize( m_referenceLuminance );
   fftTranslation.Evaluate( targetLum );

   float peakValue = fftTranslation.Peak();
   float dx = fftTranslation.Delta().x;
   float dy = fftTranslation.Delta().y;

   // Quality check: peak value indicates correlation strength
   // Typical good alignment: peak > 0.1; poor/no correlation: peak < 0.02
   if ( peakValue < 0.02f )
   {
      result.success = false;
      result.message = String().Format(
         "Frame %d: Phase correlation too weak (peak=%.4f, dx=%.1f, dy=%.1f)",
         frameIndex + 1, peakValue, dx, dy );
      return result;
   }

   // Build pure translation transform
   SimilarityTransform T;
   T.a = 1.0;
   T.b = 0.0;
   T.tx = dx;
   T.ty = dy;

   // Validate translation magnitude
   if ( T.TranslationPx() > m_config.maxTranslation )
   {
      result.success = false;
      result.message = String().Format(
         "Frame %d: Phase corr translation %.1f px exceeds limit %.1f px (peak=%.4f)",
         frameIndex + 1, T.TranslationPx(), m_config.maxTranslation, peakValue );
      return result;
   }

   result.dx = T.tx;
   result.dy = T.ty;
   result.rotationDeg = 0.0;
   result.scale = 1.0;
   result.rmsResidual = 0.0;  // Not applicable for phase corr
   result.starsDetected = 0;
   result.starsMatched = 0;
   result.success = true;

   // Check for near-identity
   if ( T.IsNearIdentity( m_config.nearIdentityPx ) )
   {
      result.skippedNearIdentity = true;
      result.message = String().Format(
         "Frame %d: Phase corr near-identity (dx=%.2f dy=%.2f peak=%.4f), skip resampling",
         frameIndex + 1, T.tx, T.ty, peakValue );
      return result;
   }

   // Apply transform
   ApplyTransform( frame, T );

   result.message = String().Format(
      "Frame %d: Phase corr dx=%.1f dy=%.1f peak=%.4f",
      frameIndex + 1, result.dx, result.dy, peakValue );

   return result;
}

// ----------------------------------------------------------------------------
// Register a single frame against reference stars
// ----------------------------------------------------------------------------

FrameRegistrationResult FrameRegistration::RegisterFrame(
   Image& frame,
   const std::vector<DetectedStar>& refStars,
   const std::vector<StarTriangle>& refTriangles,
   int frameIndex )
{
   // === Try triangle matching first (on a copy if phase corr is also enabled) ===
   FrameRegistrationResult triangleResult;
   triangleResult.frameIndex = frameIndex;
   triangleResult.method = RegistrationMethod::Triangle;

   // Detect stars in target frame
   auto targetStars = DetectStarsInFrame( frame, m_config.maxStars );
   triangleResult.starsDetected = static_cast<int>( targetStars.size() );

   bool triangleAttempted = ( triangleResult.starsDetected >= 3 );

   if ( triangleAttempted )
   {
      auto targetTriangles = BuildTriangles( targetStars, m_config.triangleStars, m_config.maxTriangles );

      if ( !targetTriangles.empty() )
      {
         auto matches = MatchTriangles( refTriangles, targetTriangles, refStars, targetStars );
         triangleResult.starsMatched = static_cast<int>( matches.size() );

         if ( triangleResult.starsMatched >= m_config.minMatches )
         {
            SimilarityTransform T = RefineMatches( refStars, targetStars, matches );
            triangleResult.starsMatched = static_cast<int>( matches.size() );

            if ( triangleResult.starsMatched >= m_config.minMatches )
            {
               // Expand matches
               auto expandedMatches = ExpandMatchesWithTransform(
                  refStars, targetStars, T, m_config.expandRadius );
               if ( static_cast<int>( expandedMatches.size() ) > triangleResult.starsMatched )
               {
                  matches = std::move( expandedMatches );
                  T = RefineMatches( refStars, targetStars, matches );
                  triangleResult.starsMatched = static_cast<int>( matches.size() );
               }

               // Compute residuals
               double sumResidSq = 0;
               for ( const auto& m : matches )
               {
                  double xp, yp;
                  T.Apply( targetStars[m.targetIndex].x, targetStars[m.targetIndex].y, xp, yp );
                  double ddx = xp - refStars[m.refIndex].x;
                  double ddy = yp - refStars[m.refIndex].y;
                  sumResidSq += ddx * ddx + ddy * ddy;
               }
               triangleResult.rmsResidual = std::sqrt( sumResidSq / matches.size() );
               triangleResult.dx = T.tx;
               triangleResult.dy = T.ty;
               triangleResult.rotationDeg = T.RotationDeg();
               triangleResult.scale = T.Scale();

               // Validate transform
               bool valid = true;
               if ( T.TranslationPx() > m_config.maxTranslation ) valid = false;
               if ( std::abs( triangleResult.rotationDeg ) > m_config.maxRotationDeg ) valid = false;
               if ( std::abs( triangleResult.scale - 1.0 ) > m_config.maxScaleDev ) valid = false;

               if ( valid )
               {
                  triangleResult.success = true;

                  // If triangle succeeded with good RMS, prefer it over phase corr
                  if ( triangleResult.rmsResidual < 1.0 || !m_config.enablePhaseCorrelation )
                  {
                     // Apply triangle result directly
                     if ( T.IsNearIdentity( m_config.nearIdentityPx ) )
                     {
                        triangleResult.skippedNearIdentity = true;
                        triangleResult.message = String().Format(
                           "Frame %d: Near-identity (dx=%.2f dy=%.2f rot=%.4f), skip resampling",
                           frameIndex + 1, T.tx, T.ty, triangleResult.rotationDeg );
                     }
                     else
                     {
                        ApplyTransform( frame, T );
                        triangleResult.message = String().Format(
                           "Frame %d: %d/%d stars matched, dx=%.1f dy=%.1f rot=%.3f%c scale=%.5f RMS=%.2f px",
                           frameIndex + 1, triangleResult.starsMatched, triangleResult.starsDetected,
                           triangleResult.dx, triangleResult.dy, triangleResult.rotationDeg,
                           0xB0, triangleResult.scale, triangleResult.rmsResidual );
                     }
                     return triangleResult;
                  }
                  // else: triangle succeeded but RMS >= 1.0, try phase corr too
               }
            }
         }
      }
   }

   // === Triangle failed or had high RMS — try phase correlation ===
   if ( m_config.enablePhaseCorrelation && m_referenceInitialized )
   {
      FrameRegistrationResult phaseResult = RegisterFramePhaseCorrelation( frame, frameIndex );

      if ( phaseResult.success )
      {
         // If triangle also succeeded, compare
         if ( triangleResult.success )
         {
            // Phase corr is translation-only. If triangle had rotation/scale, prefer triangle
            // despite higher RMS (phase corr can't handle rotation)
            bool triangleHasRotScale = ( std::abs( triangleResult.rotationDeg ) > 0.01
                                       || std::abs( triangleResult.scale - 1.0 ) > 0.001 );
            if ( triangleHasRotScale )
            {
               // Triangle is better for this frame (rotation/scale present)
               SimilarityTransform T;
               T.a = std::cos( triangleResult.rotationDeg * 3.14159265358979323846 / 180.0 ) * triangleResult.scale;
               T.b = std::sin( triangleResult.rotationDeg * 3.14159265358979323846 / 180.0 ) * triangleResult.scale;
               T.tx = triangleResult.dx;
               T.ty = triangleResult.dy;
               ApplyTransform( frame, T );
               triangleResult.message = String().Format(
                  "Frame %d: %d/%d stars matched, dx=%.1f dy=%.1f rot=%.3f%c scale=%.5f RMS=%.2f px (preferred over phase corr)",
                  frameIndex + 1, triangleResult.starsMatched, triangleResult.starsDetected,
                  triangleResult.dx, triangleResult.dy, triangleResult.rotationDeg,
                  0xB0, triangleResult.scale, triangleResult.rmsResidual );
               return triangleResult;
            }
         }

         // Phase correlation wins (triangle failed or translation-only case)
         phaseResult.message += " [recovered by phase correlation]";
         return phaseResult;
      }
   }

   // === Both methods failed ===
   if ( triangleResult.success )
   {
      // Triangle succeeded but we somehow got here (shouldn't happen, but safety)
      SimilarityTransform T;
      T.a = std::cos( triangleResult.rotationDeg * 3.14159265358979323846 / 180.0 ) * triangleResult.scale;
      T.b = std::sin( triangleResult.rotationDeg * 3.14159265358979323846 / 180.0 ) * triangleResult.scale;
      T.tx = triangleResult.dx;
      T.ty = triangleResult.dy;
      ApplyTransform( frame, T );
      return triangleResult;
   }

   // Both failed
   triangleResult.success = false;
   triangleResult.method = RegistrationMethod::Failed;
   if ( !triangleAttempted )
      triangleResult.message = String().Format( "Frame %d: Only %d stars detected (need >= 3), both methods failed",
         frameIndex + 1, triangleResult.starsDetected );
   else
      triangleResult.message = String().Format( "Frame %d: Triangle matching (%d matches) and phase correlation both failed",
         frameIndex + 1, triangleResult.starsMatched );
   return triangleResult;
}

// ----------------------------------------------------------------------------
// Register all frames (frame[0] is reference)
// ----------------------------------------------------------------------------

bool FrameRegistration::RegisterFrames( std::vector<Image>& frames, RegistrationSummary& summary )
{
   Console console;

   auto startTime = std::chrono::high_resolution_clock::now();

   summary = RegistrationSummary();
   summary.totalFrames = static_cast<int>( frames.size() );

   if ( frames.size() < 2 )
   {
      console.WarningLn( "Registration requires at least 2 frames." );
      return false;
   }

   console.WriteLn( "<br><b>Frame Registration</b>" );
   console.WriteLn( String().Format( "Reference: frame 1 of %d", summary.totalFrames ) );

   // Detect stars in reference frame (frame[0])
   auto refStars = DetectStarsInFrame( frames[0], m_config.maxStars );
   console.WriteLn( String().Format( "Reference stars detected: %d", static_cast<int>( refStars.size() ) ) );

   if ( static_cast<int>( refStars.size() ) < m_config.minMatches )
   {
      console.WarningLn( String().Format(
         "Only %d reference stars detected (need >= %d). Registration skipped.",
         static_cast<int>( refStars.size() ), m_config.minMatches ) );
      return false;
   }

   // Initialize reference luminance for phase correlation
   if ( m_config.enablePhaseCorrelation )
   {
      m_referenceLuminance = CreateLuminance( frames[0] );
      m_referenceInitialized = true;
      console.WriteLn( "Phase correlation enabled (FFT-based fallback)" );
   }

   // Build reference triangles (use triangleStars for diverse vertex coverage)
   auto refTriangles = BuildTriangles( refStars, m_config.triangleStars, m_config.maxTriangles );
   console.WriteLn( String().Format( "Reference triangles: %d", static_cast<int>( refTriangles.size() ) ) );

   // Reference frame result (always success, identity)
   FrameRegistrationResult refResult;
   refResult.frameIndex = 0;
   refResult.starsDetected = static_cast<int>( refStars.size() );
   refResult.starsMatched = refResult.starsDetected;
   refResult.success = true;
   refResult.skippedNearIdentity = true;
   refResult.message = String().Format( "Frame 1: Reference frame (%d stars)", refResult.starsDetected );
   summary.perFrame.push_back( refResult );
   summary.skippedFrames++;

   console.WriteLn( refResult.message );

   // Register remaining frames
   for ( size_t i = 1; i < frames.size(); ++i )
   {
      FrameRegistrationResult result = RegisterFrame( frames[i], refStars, refTriangles, static_cast<int>( i ) );
      summary.perFrame.push_back( result );

      if ( result.success )
      {
         if ( result.skippedNearIdentity )
            summary.skippedFrames++;
         else
            summary.registeredFrames++;

         if ( result.method == RegistrationMethod::Triangle )
            summary.triangleSucceeded++;
         else if ( result.method == RegistrationMethod::PhaseCorrelation )
            summary.phaseCorrelationRecovered++;
      }
      else
      {
         summary.failedFrames++;
      }

      console.WriteLn( result.message );
   }

   auto endTime = std::chrono::high_resolution_clock::now();
   summary.totalTimeMs = std::chrono::duration<double, std::milli>( endTime - startTime ).count();

   console.WriteLn( String().Format(
      "<br>Registration complete: %d registered (%d triangle, %d phase-corr), %d skipped, %d failed (%.1f s)",
      summary.registeredFrames, summary.triangleSucceeded, summary.phaseCorrelationRecovered,
      summary.skippedFrames, summary.failedFrames,
      summary.totalTimeMs / 1000.0 ) );

   return true;
}

// ----------------------------------------------------------------------------

} // namespace pcl
