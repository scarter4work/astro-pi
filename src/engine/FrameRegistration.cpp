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
   // Vote matrix: votes[refStarIdx][targetStarIdx]
   int nRef = static_cast<int>( refStars.size() );
   int nTarget = static_cast<int>( targetStars.size() );

   if ( nRef == 0 || nTarget == 0 )
      return {};

   std::vector<std::vector<int>> votes( nRef, std::vector<int>( nTarget, 0 ) );

   double tol = m_config.ratioTolerance;

   for ( const auto& rt : refTri )
   {
      for ( const auto& tt : targetTri )
      {
         // Compare triangle shapes
         if ( std::abs( rt.r1 - tt.r1 ) < tol && std::abs( rt.r2 - tt.r2 ) < tol )
         {
            // This triangle pair matches. Each vertex in the ref triangle
            // could map to any vertex in the target triangle.
            // We try all 3 possible vertex mappings and pick the one
            // most consistent with previous votes.

            int refVerts[3] = { rt.i, rt.j, rt.k };
            int tarVerts[3] = { tt.i, tt.j, tt.k };

            // Try each of the 6 permutations (3 rotations * 2 reflections)
            // For simplicity, try all 6:
            int perms[6][3] = {
               {0,1,2}, {0,2,1}, {1,0,2}, {1,2,0}, {2,0,1}, {2,1,0}
            };

            int bestPerm = 0;
            int bestVotes = -1;

            for ( int p = 0; p < 6; ++p )
            {
               int v = 0;
               for ( int m = 0; m < 3; ++m )
                  v += votes[refVerts[m]][tarVerts[perms[p][m]]];
               if ( v > bestVotes )
               {
                  bestVotes = v;
                  bestPerm = p;
               }
            }

            // Add votes for best permutation
            for ( int m = 0; m < 3; ++m )
               votes[refVerts[m]][tarVerts[perms[bestPerm][m]]]++;
         }
      }
   }

   // Extract matches: for each ref star, find the target star with most votes
   std::vector<StarMatch> matches;
   int minVotesThreshold = 2; // Require at least 2 triangle votes

   for ( int ri = 0; ri < nRef; ++ri )
   {
      int bestTarget = -1;
      int bestVotes = minVotesThreshold - 1;

      for ( int ti = 0; ti < nTarget; ++ti )
      {
         if ( votes[ri][ti] > bestVotes )
         {
            bestVotes = votes[ri][ti];
            bestTarget = ti;
         }
      }

      if ( bestTarget >= 0 )
      {
         // Verify this target star's best match is also this ref star (mutual best)
         bool mutual = true;
         for ( int ri2 = 0; ri2 < nRef; ++ri2 )
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
// Register a single frame against reference stars
// ----------------------------------------------------------------------------

FrameRegistrationResult FrameRegistration::RegisterFrame(
   Image& frame,
   const std::vector<DetectedStar>& refStars,
   const std::vector<StarTriangle>& refTriangles,
   int frameIndex )
{
   FrameRegistrationResult result;
   result.frameIndex = frameIndex;

   // Detect stars in target frame
   auto targetStars = DetectStarsInFrame( frame, m_config.maxStars );
   result.starsDetected = static_cast<int>( targetStars.size() );

   if ( result.starsDetected < 3 )
   {
      result.success = false;
      result.message = String().Format( "Frame %d: Only %d stars detected (need >= 3), skipping registration",
         frameIndex + 1, result.starsDetected );
      return result;
   }

   // Build triangles for target
   auto targetTriangles = BuildTriangles( targetStars, m_config.maxStars, m_config.maxTriangles );

   if ( targetTriangles.empty() )
   {
      result.success = false;
      result.message = String().Format( "Frame %d: No valid triangles, skipping registration", frameIndex + 1 );
      return result;
   }

   // Match triangles
   auto matches = MatchTriangles( refTriangles, targetTriangles, refStars, targetStars );
   result.starsMatched = static_cast<int>( matches.size() );

   if ( result.starsMatched < m_config.minMatches )
   {
      result.success = false;
      result.message = String().Format( "Frame %d: Only %d star matches (need >= %d), skipping registration",
         frameIndex + 1, result.starsMatched, m_config.minMatches );
      return result;
   }

   // Compute and refine transform
   SimilarityTransform T = RefineMatches( refStars, targetStars, matches );
   result.starsMatched = static_cast<int>( matches.size() ); // May have changed after refinement

   if ( result.starsMatched < m_config.minMatches )
   {
      result.success = false;
      result.message = String().Format( "Frame %d: Only %d matches after refinement (need >= %d)",
         frameIndex + 1, result.starsMatched, m_config.minMatches );
      return result;
   }

   // Compute final residuals
   double sumResidSq = 0;
   for ( const auto& m : matches )
   {
      double xp, yp;
      T.Apply( targetStars[m.targetIndex].x, targetStars[m.targetIndex].y, xp, yp );
      double dx = xp - refStars[m.refIndex].x;
      double dy = yp - refStars[m.refIndex].y;
      sumResidSq += dx * dx + dy * dy;
   }
   result.rmsResidual = std::sqrt( sumResidSq / matches.size() );

   result.dx = T.tx;
   result.dy = T.ty;
   result.rotationDeg = T.RotationDeg();
   result.scale = T.Scale();

   // Validate transform
   if ( T.TranslationPx() > m_config.maxTranslation )
   {
      result.success = false;
      result.message = String().Format( "Frame %d: Translation %.1f px exceeds limit %.1f px",
         frameIndex + 1, T.TranslationPx(), m_config.maxTranslation );
      return result;
   }

   if ( std::abs( result.rotationDeg ) > m_config.maxRotationDeg )
   {
      result.success = false;
      result.message = String().Format( "Frame %d: Rotation %.3f deg exceeds limit %.1f deg",
         frameIndex + 1, result.rotationDeg, m_config.maxRotationDeg );
      return result;
   }

   if ( std::abs( result.scale - 1.0 ) > m_config.maxScaleDev )
   {
      result.success = false;
      result.message = String().Format( "Frame %d: Scale %.6f deviates from 1.0 by more than %.3f",
         frameIndex + 1, result.scale, m_config.maxScaleDev );
      return result;
   }

   result.success = true;

   // Check for near-identity
   if ( T.IsNearIdentity( m_config.nearIdentityPx ) )
   {
      result.skippedNearIdentity = true;
      result.message = String().Format( "Frame %d: Near-identity (dx=%.2f dy=%.2f rot=%.4f), skip resampling",
         frameIndex + 1, T.tx, T.ty, result.rotationDeg );
      return result;
   }

   // Apply transform
   ApplyTransform( frame, T );

   result.message = String().Format(
      "Frame %d: %d/%d stars matched, dx=%.1f dy=%.1f rot=%.3f° scale=%.5f RMS=%.2f px",
      frameIndex + 1, result.starsMatched, result.starsDetected,
      result.dx, result.dy, result.rotationDeg, result.scale, result.rmsResidual );

   return result;
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

   // Build reference triangles
   auto refTriangles = BuildTriangles( refStars, m_config.maxStars, m_config.maxTriangles );
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
      "<br>Registration complete: %d registered, %d skipped (near-identity), %d failed (%.1f s)",
      summary.registeredFrames, summary.skippedFrames, summary.failedFrames,
      summary.totalTimeMs / 1000.0 ) );

   return true;
}

// ----------------------------------------------------------------------------

} // namespace pcl
